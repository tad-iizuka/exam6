# mini_db（select版）コード解説

対象：`mini_db_names_aligned/mini_db.cpp` と `mini_db.hpp`（C++98、変数名統一版）。

## 1. 概要と全体の流れ

`mini_db` は `127.0.0.1` の指定ポートでTCP接続を受け付け、改行区切りの `POST`、`GET`、`DELETE` コマンドを処理するサーバーです。複数の接続を維持し、SIGINTでデータベースをファイルに保存します。

```text
main()
 ├─ Server(port, path)
 ├─ setup() ─ signal登録 → DB読込 → socket → setsockopt → 非ブロッキング化 → bind → listen → ready
 └─ run() ─ select() ─┬─ handle_new_connection()
                      ├─ recv_data() → process_commands() → process_command()
                      └─ send_data()
            SIGINT → ループ終了 → save_database()
```

## 2. データ構造と変数

```cpp
struct Client {
    std::string in;
    std::string out;
    size_t sent;
    Client() : sent(0) {}
};
```

| 変数 | 意味 |
|---|---|
| `in` | 受信済みで、まだ処理していないデータ |
| `out` | クライアントに送信する応答データ |
| `sent` | `out` のうち送信済みのバイト数 |
| `server` | 待ち受けソケットのfd |
| `clients_` | `std::map<int, Client>`：fdからクライアント状態を引く |
| `database_` | `std::map<std::string, std::string>`：キーと値 |
| `port_`, `path_` | ポート番号と保存ファイルパス |
| `g_running` | SIGINTで0にされる終了フラグ（`volatile sig_atomic_t`） |

`mini_serv` と異なり、受信・送信バッファは `std::string` なので `free()` は不要です。`sent` によって送信済み部分を記録し、部分送信のたびに文字列を詰め直さずに済みます。

## 3. 起動・終了に関わる関数

### `main()` / コンストラクタ / デストラクタ

`main()` は引数を確認し、`Server` を生成して `setup()`、`run()` を順に呼びます。コンストラクタは `port_`、`path_`、`server = -1` を設定します。デストラクタは登録中のクライアントfdと待ち受けfdを閉じます。

### `setup()`

1. `signal(SIGINT, signal_handler)`：Ctrl+C等で終了フラグを下げる。
2. `signal(SIGPIPE, SIG_IGN)`：切断先への送信でプロセスが終了するのを防ぐ。
3. `load_database()`：保存ファイルがあれば復元。
4. `socket(AF_INET, SOCK_STREAM, 0)`：TCPソケットを生成。
5. `FD_SETSIZE` を確認。
6. `setsockopt(... SO_REUSEADDR ...)`：同じポートでの再起動をしやすくする。
7. `set_nonblocking(server)`：待ち受けfdを非ブロッキング化。
8. `bind()`：`127.0.0.1` とポートに割り当てる。
9. `listen(server, 128)`：接続待機を開始。
10. `ready` を標準出力に出す。

### `set_nonblocking(int fd)`

`fcntl(F_GETFL)` で現在のフラグを取得し、`fcntl(F_SETFL, flags | O_NONBLOCK)` で非ブロッキングにします。データがないときに `recv()` や `accept()` が待ち続けることを防ぎます。

### `signal_handler()` / `save_database()` / `load_database()`

シグナルハンドラは `g_running = 0` のみ実行します。`run()` がループを抜けた後に `save_database()` が `key value` の形式で各要素を書き込みます。`load_database()` は起動時に各行からキーと値を読み取ります。キー・値には空白が含まれない課題仕様です。

## 4. `run()`：selectによるイベントループ

```cpp
fd_set rd, wr;
FD_ZERO(&rd);
FD_ZERO(&wr);
FD_SET(server, &rd);
int maxfd = server;
```

- `rd` は新規接続・受信可能fdの監視集合。
- `wr` は送信待ちがあるfdの書き込み可能状態の監視集合。
- 全クライアントを `rd` に登録し、`sent < out.size()` のクライアントのみ `wr` に登録。
- `maxfd` は登録したfdの最大値。`select(maxfd + 1, &rd, &wr, NULL, NULL)` を実行。
- `FD_ISSET(server, &rd)` なら新規接続処理。
- 各クライアントについて `rd` なら `recv_data()`、まだ接続中かつ `wr` なら `send_data()`。
- `select()` はfd集合を書き換えるため、毎回 `FD_ZERO` / `FD_SET` し直します。

反復中にクライアントが削除されてもよいよう、イテレータを先に進め、送信前に `clients_.find(fd)` で存在確認しています。

## 5. 接続と切断

### `handle_new_connection()`

`accept()` を繰り返して保留中の接続を取り出します。`EAGAIN` / `EWOULDBLOCK` になったら終了し、`EINTR` なら再試行します。受け付けたfdが `FD_SETSIZE` 以上なら閉じ、それ以外は非ブロッキング化して `clients_[fd] = Client()` で登録します。

### `handle_disconnection(int fd)`

`clients_.erase(fd)` でバッファを含むクライアント情報を削除し、`close(fd)` でソケットを閉じます。

## 6. 受信からコマンド実行まで

### `recv_data(int fd)`

`recv(fd, buf, sizeof(buf), 0)` をループして、受信したバイト列を `client.in.append(...)` で蓄積し、`process_commands(fd)` を呼びます。

| 結果 | 動作 |
|---|---|
| `n > 0` | バッファへ追加してコマンド解析 |
| `n == 0` | 相手が接続を閉じたので切断 |
| `EINTR` | 再試行 |
| `EAGAIN` / `EWOULDBLOCK` | 現時点の受信を終了 |
| その他のエラー | 切断 |

### `process_commands(int fd)`

`in.find('\n')` で改行を探し、見つかった行を `substr()` で取り出して `erase()` で消します。`process_command()` の戻り値を `out` に追加します。改行のない末尾の断片は次の受信まで残します。

```text
受信1: "PO"             → in = "PO"（まだ実行しない）
受信2: "ST A B\nGET A\n" → POSTとGETを順番に実行
```

### `process_command(const std::string &command)`

`std::istringstream` で空白区切りのトークンを取り出し、引数の不足・余分な引数を検査します。

| コマンド | 操作 | 応答 |
|---|---|---|
| `POST A B` | `database_["A"] = "B"` | `0\n` |
| `GET A` | 検索して値を返す | `0 B\n` または `1\n` |
| `DELETE A` | キーを削除 | `0\n` または `1\n` |
| 不正なコマンド | 操作しない | `2\n` |

## 7. `send_data(int fd)`：部分送信への対応

```cpp
send(fd, client.out.data() + client.sent,
     client.out.size() - client.sent, 0);
```

例：10バイト中4バイト送れたら `sent = 4`、次は残り6バイトを送ります。`EINTR` は再試行、`EAGAIN` は後の書き込みイベントを待ちます。全部送信したら `out.clear()` と `sent = 0` を実行します。

`process_commands()` で応答を `out` に追加した直後は、通常は次の `select()` で書き込み可能と判定されてから送信します。

## 8. mini_servとの対応

| mini_db | mini_serv |
|---|---|
| `setup()` | `main()` の初期化部分 |
| `run()` | `main()` の `select()` ループ |
| `handle_new_connection()` | `accept()` の分岐 |
| `recv_data()` | `receive_client()` |
| `process_commands()` | `receive_client()` の改行解析 |
| `process_command()` | チャットではなくDB固有の処理 |
| `send_data()` | `main()` の `send()` 部分 |
| `handle_disconnection()` | `disconnect_client()` |

## 9. 注意点とテスト

- `select()` は `FD_SETSIZE` に制約があるため、fdの上限を確認しています。
- `SO_REUSEADDR` は再起動を容易にしますが、別のサーバーが同じアドレス・ポートで待ち受けている場合の重複使用を許すものではありません。
- 現実装では、受信した直後に相手が切断すると、まだ `out` に残る応答が破棄される場合があります。
- 送信待ちバッファの容量上限は設けていません。
- 添付の `comprehensive_test.py` はCRUD、不正入力、分割受信、1000文字、同時接続、パイプライン、遅い受信者、切断、永続化を検証します。全件PASSに加え、Valgrindによるリーク確認も有用です。

**要点：** `select()` → `recv_data()` → `process_commands()` → `process_command()` → `send_data()` が中心の流れです。
