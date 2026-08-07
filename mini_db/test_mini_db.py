#!/usr/bin/env python3
"""mini_db の自動テストスクリプト"""
import socket
import subprocess
import sys
import time
import os
import signal

BIN = sys.argv[1] if len(sys.argv) > 1 else "./mini_db"
PORT = 9123
SAVE_FILE = "/tmp/mini_db_test.save"

passed = 0
failed = 0


def check(name, cond, extra=""):
    global passed, failed
    if cond:
        print(f"[ OK ] {name}")
        passed += 1
    else:
        print(f"[FAIL] {name} {extra}")
        failed += 1


def send_cmd(sock, cmd):
    sock.sendall((cmd + "\n").encode())
    sock.settimeout(1.0)
    data = b""
    while not data.endswith(b"\n"):
        chunk = sock.recv(4096)
        if not chunk:
            break
        data += chunk
    return data.decode().rstrip("\n")


def wait_ready(proc, timeout=3.0):
    start = time.time()
    while time.time() - start < timeout:
        line = proc.stdout.readline()
        if line.strip() == "ready":
            return True
    return False


if os.path.exists(SAVE_FILE):
    os.remove(SAVE_FILE)

server = subprocess.Popen([BIN, str(PORT), SAVE_FILE],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
try:
    check("起動時に 'ready' を標準出力に表示", wait_ready(server))

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect(("127.0.0.1", PORT))

    check("POST A B -> '0'", send_cmd(s, "POST A B") == "0")
    check("POST B C -> '0'", send_cmd(s, "POST B C") == "0")
    check("GET A -> '0 B'", send_cmd(s, "GET A") == "0 B")
    check("GET C (存在しない) -> '1'", send_cmd(s, "GET C") == "1")
    check("DELETE A -> '0'", send_cmd(s, "DELETE A") == "0")
    check("DELETE C (存在しない) -> '1'", send_cmd(s, "DELETE C") == "1")
    check("未知のコマンド -> '2'", send_cmd(s, "UNKNOWN_COMMAND") == "2")
    check("POST の引数不足 -> '2'", send_cmd(s, "POST OnlyKey") == "2")

    # 同一セッションで複数コマンドを送れること(persistent connection)
    check("同一セッションでの複数コマンド1",
          send_cmd(s, "POST X 1") == "0")
    check("同一セッションでの複数コマンド2",
          send_cmd(s, "GET X") == "0 1")

    # 上書き(POSTで既存キーを更新)
    check("既存キーへのPOSTで値が上書きされる",
          send_cmd(s, "POST B Z") == "0" and send_cmd(s, "GET B") == "0 Z")

    s.close()

    # SIGINT で保存して終了することを確認
    server.send_signal(signal.SIGINT)
    try:
        server.wait(timeout=3)
        check("SIGINT を受けてプロセスが終了する", True)
    except subprocess.TimeoutExpired:
        check("SIGINT を受けてプロセスが終了する", False, "(timeout)")
        server.kill()

    check("SIGINT 後にセーブファイルが作成される", os.path.exists(SAVE_FILE))

finally:
    if server.poll() is None:
        server.kill()
        server.wait()

# 再起動して永続化されたデータを読み込めるか確認
server2 = subprocess.Popen([BIN, str(PORT + 1), SAVE_FILE],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
try:
    check("再起動時にも 'ready' を表示", wait_ready(server2))
    s2 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s2.connect(("127.0.0.1", PORT + 1))
    check("再起動後、保存されていた B の値(更新後のZ)を取得できる",
          send_cmd(s2, "GET B") == "0 Z")
    check("再起動後、削除済みの A は存在しない",
          send_cmd(s2, "GET A") == "1")
    check("再起動後、X も永続化されている",
          send_cmd(s2, "GET X") == "0 1")
    s2.close()
finally:
    if server2.poll() is None:
        server2.send_signal(signal.SIGINT)
        try:
            server2.wait(timeout=3)
        except subprocess.TimeoutExpired:
            server2.kill()
            server2.wait()

if os.path.exists(SAVE_FILE):
    os.remove(SAVE_FILE)

print(f"\n合計: {passed} 成功 / {failed} 失敗")
sys.exit(1 if failed else 0)
