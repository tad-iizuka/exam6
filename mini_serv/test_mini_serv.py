#!/usr/bin/env python3
"""
mini_serv.c の自動テストスクリプト。
使い方: python3 test_mini_serv.py [./mini_serv のパス]
"""
import socket
import subprocess
import sys
import time
import os
import signal

SERVER_BIN = sys.argv[1] if len(sys.argv) > 1 else "./mini_serv"
PORT = 8123

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


def recv_all(sock, timeout=0.5):
    sock.settimeout(timeout)
    data = b""
    try:
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            data += chunk
    except socket.timeout:
        pass
    return data


def test_wrong_args():
    proc = subprocess.run([SERVER_BIN], capture_output=True, timeout=5)
    check("引数なし -> 終了ステータス1", proc.returncode == 1,
          f"(got {proc.returncode})")
    check("引数なし -> stderr メッセージ",
          proc.stderr == b"Wrong number of arguments\n",
          f"(got {proc.stderr!r})")

    proc = subprocess.run([SERVER_BIN, "1", "2"], capture_output=True, timeout=5)
    check("引数2つ -> 終了ステータス1", proc.returncode == 1)


def test_main_flow():
    server = subprocess.Popen([SERVER_BIN, str(PORT)],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(0.3)
    try:
        a = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        a.connect(("127.0.0.1", PORT))
        time.sleep(0.1)

        b = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        b.connect(("127.0.0.1", PORT))
        time.sleep(0.1)

        msg = recv_all(a)
        check("client Bが接続 -> Aに 'just arrived' 通知",
              msg == b"server: client 1 just arrived\n", f"(got {msg!r})")

        a.sendall(b"hello world\n")
        time.sleep(0.1)
        msg = recv_all(b)
        check("client 0 のメッセージが 'client 0: ' 付きでBに届く",
              msg == b"client 0: hello world\n", f"(got {msg!r})")

        # 複数行を含む1回の送信
        b.sendall(b"line1\nline2\n")
        time.sleep(0.1)
        msg = recv_all(a)
        check("複数行メッセージが行ごとに 'client 1: ' 付きで届く",
              msg == b"client 1: line1\nclient 1: line2\n", f"(got {msg!r})")

        # 改行なしの部分送信 -> 改行が来るまで転送されない
        b.sendall(b"partial")
        time.sleep(0.1)
        msg = recv_all(a, timeout=0.3)
        check("改行が来るまでメッセージは転送されない", msg == b"", f"(got {msg!r})")
        b.sendall(b" message\n")
        time.sleep(0.1)
        msg = recv_all(a)
        check("分割送信されたメッセージが結合されて届く",
              msg == b"client 1: partial message\n", f"(got {msg!r})")

        # 3人目のクライアント -> 新しいidは2 (0,1が既に使われているため)
        c = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        c.connect(("127.0.0.1", PORT))
        time.sleep(0.1)
        msg_a = recv_all(a)
        msg_b = recv_all(b)
        check("3人目参加 -> id=2でAに通知",
              msg_a == b"server: client 2 just arrived\n", f"(got {msg_a!r})")
        check("3人目参加 -> id=2でBに通知",
              msg_b == b"server: client 2 just arrived\n", f"(got {msg_b!r})")

        # bの切断
        b.close()
        time.sleep(0.2)
        msg_a = recv_all(a)
        msg_c = recv_all(c)
        check("client 1切断 -> Aに 'just left' 通知",
              msg_a == b"server: client 1 just left\n", f"(got {msg_a!r})")
        check("client 1切断 -> Cに 'just left' 通知",
              msg_c == b"server: client 1 just left\n", f"(got {msg_c!r})")

        a.close()
        c.close()
    finally:
        server.send_signal(signal.SIGKILL)
        server.wait()


def test_fd_reuse_id_increments():
    """切断後に再接続しても id は使い回されず増え続けることを確認"""
    server = subprocess.Popen([SERVER_BIN, str(PORT + 1)],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(0.3)
    try:
        a = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        a.connect(("127.0.0.1", PORT + 1))
        time.sleep(0.1)
        a.close()
        time.sleep(0.2)

        b = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        b.connect(("127.0.0.1", PORT + 1))
        time.sleep(0.1)

        c = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        c.connect(("127.0.0.1", PORT + 1))
        time.sleep(0.1)
        msg = recv_all(b)
        check("最初のclient(id0)切断後、次の接続はid1から始まる",
              msg == b"server: client 2 just arrived\n", f"(got {msg!r})")
        b.close()
        c.close()
    finally:
        server.send_signal(signal.SIGKILL)
        server.wait()


if __name__ == "__main__":
    if not os.path.exists(SERVER_BIN):
        print(f"サーバーのバイナリが見つかりません: {SERVER_BIN}")
        sys.exit(1)

    test_wrong_args()
    test_main_flow()
    test_fd_reuse_id_increments()

    print(f"\n合計: {passed} 成功 / {failed} 失敗")
    sys.exit(1 if failed else 0)
