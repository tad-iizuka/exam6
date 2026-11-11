#!/usr/bin/env python3
"""mini_db comprehensive black-box test. Linux, Python 3, no dependencies."""
import argparse
import concurrent.futures
import os
import pathlib
import signal
import socket
import subprocess
import sys
import tempfile
import time

HOST = '127.0.0.1'

class Client:
    def __init__(self, port, timeout=4):
        self.sock = socket.create_connection((HOST, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.pending = bytearray()
    def send(self, data):
        self.sock.sendall(data if isinstance(data, bytes) else data.encode())
    def line(self):
        while b'\n' not in self.pending:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise AssertionError('connection closed before response')
            self.pending.extend(chunk)
        pos = self.pending.index(10)
        out = bytes(self.pending[:pos + 1])
        del self.pending[:pos + 1]
        return out
    def query(self, command):
        self.send(command + '\n')
        return self.line()
    def close(self):
        self.sock.close()
    def __enter__(self):
        return self
    def __exit__(self, *_):
        self.close()

class Server:
    def __init__(self, binary, path):
        self.binary = binary
        self.path = path
        self.proc = None
        self.port = None
    def start(self):
        # Reserve a port briefly; server binds immediately after release.
        with socket.socket() as s:
            s.bind((HOST, 0))
            self.port = s.getsockname()[1]
        self.proc = subprocess.Popen([str(self.binary), str(self.port), str(self.path)],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     start_new_session=True)
        import select
        ready, _, _ = select.select([self.proc.stdout], [], [], 4)
        if not ready:
            self.stop(force=True)
            raise AssertionError('no ready output within 4 seconds')
        line = self.proc.stdout.readline()
        if line != b'ready\n':
            self.stop(force=True)
            raise AssertionError(f'expected ready\\n, got {line!r}')
        if self.proc.poll() is not None:
            raise AssertionError('server exited after ready')
        return self
    def stop(self, force=False):
        if not self.proc or self.proc.poll() is not None:
            return
        if not force:
            self.proc.send_signal(signal.SIGINT)
            try:
                self.proc.wait(timeout=4)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
                raise AssertionError('SIGINT did not terminate server within 4 seconds')
            if self.proc.returncode != 0:
                raise AssertionError(f'SIGINT exit status {self.proc.returncode}')
        else:
            self.proc.kill()
            self.proc.wait()
    def client(self, timeout=4):
        return Client(self.port, timeout)

def eq(actual, expected):
    if actual != expected:
        raise AssertionError(f'expected {expected[:100]!r} ({len(expected)} bytes), got {actual[:100]!r} ({len(actual)} bytes)')

def test_crud(s):
    with s.client() as c:
        eq(c.query('POST A B'), b'0\n')
        eq(c.query('GET A'), b'0 B\n')
        eq(c.query('POST A C'), b'0\n')
        eq(c.query('GET A'), b'0 C\n')
        eq(c.query('DELETE A'), b'0\n')
        eq(c.query('GET A'), b'1\n')
        eq(c.query('DELETE A'), b'1\n')

def test_unknown(s):
    with s.client() as c:
        for cmd in ('UNKNOWN', 'POST a', 'GET', 'DELETE', 'POST a b c', 'GET a b', 'DELETE a b', ''):
            eq(c.query(cmd), b'2\n')

def test_persistent(s):
    with s.client() as c:
        c.send('POST persist yes\nGET persist\nDELETE persist\nGET persist\n')
        for expected in (b'0\n', b'0 yes\n', b'0\n', b'1\n'):
            eq(c.line(), expected)

def test_partial(s):
    with s.client() as c:
        c.send('PO'); time.sleep(.02)
        c.send('ST split val'); time.sleep(.02)
        c.send('ue\nGET split\n')
        eq(c.line(), b'0\n')
        eq(c.line(), b'0 value\n')

def test_max_request(s):
    key = 'K' * 450
    value = 'V' * (1000 - len('POST ') - len(key) - 1)
    assert len(f'POST {key} {value}') == 1000
    with s.client() as c:
        eq(c.query(f'POST {key} {value}'), b'0\n')
        eq(c.query(f'GET {key}'), ('0 ' + value + '\n').encode())
        eq(c.query(f'DELETE {key}'), b'0\n')

def test_multi_client(s):
    def worker(i):
        with s.client() as c:
            key = f'parallel_{i}'
            eq(c.query(f'POST {key} value{i}'), b'0\n')
            eq(c.query(f'GET {key}'), f'0 value{i}\n'.encode())
    with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
        list(pool.map(worker, range(64)))
    with s.client() as c:
        for i in range(64):
            eq(c.query(f'GET parallel_{i}'), f'0 value{i}\n'.encode())

def test_large_pipeline(s):
    n = 4000
    with s.client(timeout=12) as c:
        c.send(('POST bulk v\n' + 'GET bulk\n') * n)
        for _ in range(n):
            eq(c.line(), b'0\n')
            eq(c.line(), b'0 v\n')

def test_slow_reader(s):
    # Valid requests; server must not lose responses when peer pauses reading.
    n = 2500
    with s.client(timeout=20) as c:
        c.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
        c.send('POST slow ' + 'x' * 900 + '\n')
        eq(c.line(), b'0\n')
        # Interleave writes and reads to avoid client-side sendall deadlock.
        import threading
        errors = []
        def writer():
            try:
                data = b'GET slow\n' * n
                c.send(data)
            except Exception as exc:
                errors.append(exc)
        t = threading.Thread(target=writer, daemon=True)
        t.start()
        time.sleep(.15)
        expected = b'0 ' + b'x' * 900 + b'\n'
        for _ in range(n):
            eq(c.line(), expected)
        t.join(timeout=3)
        if t.is_alive() or errors:
            raise AssertionError(f'writer incomplete: {errors}')

def test_disconnect(s):
    for i in range(100):
        with s.client() as c:
            eq(c.query(f'POST cycle{i} v'), b'0\n')
    with s.client() as c:
        eq(c.query('GET cycle99'), b'0 v\n')

def test_persistence(s):
    with s.client() as c:
        eq(c.query('POST saved value'), b'0\n')
        eq(c.query('POST removed value'), b'0\n')
        eq(c.query('DELETE removed'), b'0\n')
    s.stop()
    if not s.path.is_file():
        raise AssertionError('save file not created on SIGINT')
    s.start()
    with s.client() as c:
        eq(c.query('GET saved'), b'0 value\n')
        eq(c.query('GET removed'), b'1\n')
        eq(c.query('POST saved changed'), b'0\n')
    s.stop()
    s.start()
    with s.client() as c:
        eq(c.query('GET saved'), b'0 changed\n')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default='./mini_db', help='compiled executable path')
    parser.add_argument('--skip-stress', action='store_true', help='skip pipeline and slow reader tests')
    args = parser.parse_args()
    binary = pathlib.Path(args.binary).resolve()
    if not binary.is_file():
        parser.error(f'not found: {binary}')
    tests = [('CRUD and overwrite', test_crud), ('Invalid/unknown commands', test_unknown),
             ('Persistent connection and multiple lines', test_persistent),
             ('Fragmented request', test_partial), ('1000-character request', test_max_request),
             ('64 concurrent clients', test_multi_client)]
    if not args.skip_stress:
        tests += [('8000 pipelined responses', test_large_pipeline),
                  ('Paused/slow reader and complete delivery', test_slow_reader)]
    tests += [('100 connect/disconnect cycles', test_disconnect),
              ('SIGINT persistence and reload', test_persistence)]
    passed = failed = 0
    with tempfile.TemporaryDirectory(prefix='mini_db_test_') as d:
        server = Server(binary, pathlib.Path(d) / 'database.txt')
        try:
            server.start()
            for label, fn in tests:
                try:
                    fn(server)
                    print(f'PASS: {label}', flush=True)
                    passed += 1
                except Exception as exc:
                    print(f'FAIL: {label}: {type(exc).__name__}: {exc}', flush=True)
                    failed += 1
                    if server.proc.poll() is not None:
                        print('Server exited unexpectedly; stopping tests', flush=True)
                        break
        finally:
            server.stop(force=True)
    print(f'\nPassed: {passed}  Failed: {failed}', flush=True)
    return 1 if failed else 0

if __name__ == '__main__':
    sys.exit(main())
