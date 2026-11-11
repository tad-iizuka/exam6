#!/usr/bin/env bash
# Ubuntu 22.04 / Python 3. No third-party packages required.
set -u
SERVER=${SERVER:-./mini_serv}
PORT=${PORT:-50000}
if [[ ! -x "$SERVER" ]]; then echo "Missing executable: $SERVER" >&2; exit 1; fi
python3 - "$SERVER" "$PORT" <<'PY'
import os, socket, subprocess, sys, time, select, signal

binary, first_port = sys.argv[1], int(sys.argv[2])
passed = failed = 0
server = None
clients = []

def connect(port):
    s = socket.socket()
    s.settimeout(3)
    s.connect(('127.0.0.1', port))
    clients.append(s)
    return s

def read_until(s, marker, timeout=4):
    data = b''
    deadline = time.monotonic() + timeout
    while marker not in data:
        left = deadline - time.monotonic()
        if left <= 0: raise AssertionError(f'Expected {marker!r}, got {data[-200:]!r}')
        s.settimeout(left)
        try: part = s.recv(65536)
        except socket.timeout: raise AssertionError(f'Expected {marker!r}, got {data[-200:]!r}')
        if not part: raise AssertionError('Unexpected disconnect')
        data += part
    return data

def start():
    global server
    for port in range(start.port, start.port + 30):
        p = subprocess.Popen([binary, str(port)], stdout=subprocess.DEVNULL,
                             stderr=subprocess.PIPE)
        time.sleep(.08)
        if p.poll() is None:
            server = p
            start.port = port + 1
            return port
        p.stderr.close()
    raise RuntimeError('Could not start server on an available port')
start.port = first_port

def stop():
    global server
    for s in clients:
        try: s.close()
        except OSError: pass
    clients.clear()
    if server:
        if server.poll() is None:
            server.terminate()
            try: server.wait(timeout=2)
            except subprocess.TimeoutExpired:
                server.kill(); server.wait()
        server.stderr.close()
        server = None

def test(name, fn):
    global passed, failed
    try:
        port = start()
        fn(port)
        if server.poll() is not None: raise AssertionError('Server terminated')
        passed += 1
        print(f'PASS: {name} (port {port})', flush=True)
    except Exception as e:
        failed += 1
        print(f'FAIL: {name}: {e}', flush=True)
    finally:
        stop()

def arrival(p):
    a = connect(p); b = connect(p)
    assert b'server: client 1 just arrived\n' in read_until(a, b'just arrived\n')

def message(p):
    a = connect(p); b = connect(p)
    read_until(a, b'just arrived\n')
    b.sendall(b'Hello from sender\n')
    assert b'client 1: Hello from sender\n' in read_until(a, b'Hello from sender\n')

def departure(p):
    a = connect(p); b = connect(p)
    read_until(a, b'just arrived\n')
    b.close(); clients.remove(b)
    assert b'server: client 1 just left\n' in read_until(a, b'just left\n')

def partial(p):
    a = connect(p); b = connect(p)
    read_until(a, b'just arrived\n')
    b.sendall(b'Hel'); time.sleep(.1); b.sendall(b'lo\n')
    assert b'client 1: Hello\n' in read_until(a, b'Hello\n')

def multiline(p):
    a = connect(p); b = connect(p)
    read_until(a, b'just arrived\n')
    b.sendall(b'Line 1\nLine 2\nLine 3\n')
    got = read_until(a, b'client 1: Line 3\n')
    for i in range(1, 4): assert got.count(f'client 1: Line {i}\n'.encode()) == 1

def slow(p):
    # The slow client does not read until after a substantial backlog is queued.
    slow = connect(p)
    slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    fast = connect(p)
    read_until(slow, b'just arrived\n')
    sender = connect(p)
    read_until(fast, b'just arrived\n')
    # Drain the fast reader concurrently while sender generates a backlog.
    import threading
    got = bytearray()
    errors = []
    def drain():
        try:
            fast.settimeout(15)
            while b'client 2: END_MARKER\n' not in got:
                chunk = fast.recv(65536)
                if not chunk: raise AssertionError('Fast client disconnected')
                got.extend(chunk)
        except Exception as e: errors.append(e)
    worker = threading.Thread(target=drain, daemon=True)
    worker.start()
    payload = b'X' * 1024 + b'\n'
    for _ in range(1024): sender.sendall(payload)
    sender.sendall(b'END_MARKER\n')
    worker.join(timeout=15)
    if worker.is_alive() or errors: raise AssertionError(f'Fast client stalled: {errors}')
    assert got.count(b'client 2: ' + payload) == 1024, 'Fast client lost/duplicated data'
    assert server.poll() is None, 'Server exited while slow client was not reading'
    # Check the slow client is still connected by consuming the start of its backlog.
    slow.settimeout(5)
    assert slow.recv(4096), 'Slow client was disconnected'

def slow_full_delivery(p):
    # Pause the receiver, then verify every byte is eventually delivered.
    receiver = connect(p)
    sender = connect(p)
    read_until(receiver, b'just arrived\n')
    payload = b'Q' * 1024 + b'\n'
    expected = b'client 1: ' + payload
    count = 256
    for _ in range(count): sender.sendall(payload)
    sender.sendall(b'END_FULL\n')
    time.sleep(.4)
    assert server.poll() is None, 'Server stopped while receiver was idle'
    data = read_until(receiver, b'client 1: END_FULL\n', timeout=30)
    assert data == expected * count + b'client 1: END_FULL\n', \
        f'Data corrupted: received {len(data)} bytes'

def resources(p):
    # Linux-only /proc checks: FD count and VmRSS after 150 connect/disconnect cycles.
    observer = connect(p)
    proc = f'/proc/{server.pid}'
    if not os.path.isdir(proc): raise AssertionError('Requires Linux /proc')
    def fdcount(): return len(os.listdir(proc + '/fd'))
    def rss():
        with open(proc + '/status') as f:
            for line in f:
                if line.startswith('VmRSS:'): return int(line.split()[1])
        raise AssertionError('VmRSS unavailable')
    base_fd, base_rss = fdcount(), rss()
    for i in range(150):
        s = connect(p)
        read_until(observer, b'just arrived\n')
        s.sendall(b'test\n')
        read_until(observer, b': test\n')
        s.close(); clients.remove(s)
        read_until(observer, b'just left\n')
    time.sleep(.2)
    end_fd, end_rss = fdcount(), rss()
    assert end_fd <= base_fd + 2, f'FD growth: {base_fd} -> {end_fd}'
    assert end_rss - base_rss < 8192, f'RSS grew {end_rss - base_rss} KB'
    print(f'  FD: {base_fd} -> {end_fd}; RSS: {base_rss} -> {end_rss} KB')

try:
    print('=== mini_serv comprehensive tests (Ubuntu / Python 3) ===', flush=True)
    for name, fn in [('Connection notification', arrival),
                     ('Message broadcast', message),
                     ('Disconnect notification', departure),
                     ('Partial message', partial),
                     ('Multiple lines', multiline),
                     ('Slow reader + 1 MiB broadcast', slow),
                      ('Paused reader full delivery (256 KiB)', slow_full_delivery),
                     ('FD/RSS stability (150 cycles)', resources)]:
        test(name, fn)
finally:
    stop()
print(f'\nPassed: {passed}  Failed: {failed}', flush=True)
sys.exit(1 if failed else 0)
PY
