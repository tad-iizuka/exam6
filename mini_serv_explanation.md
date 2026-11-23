# mini_serv コード解説

対象：添付された `mini_serv(3).c`（C言語、`select()` 版）。

## 1. 概要と全体の流れ

`mini_serv` は `127.0.0.1` の指定ポートで接続を受け付け、あるクライアントの各行を、送信元以外のクライアントに `client <id>: ` を付けて配信するTCPチャットサーバーです。

```text
main() ─ socket → bind → listen
  └─ 無限ループ
       ├─ fd_setを準備 → select()
       ├─ serverが読み込み可能 → accept() → 到着通知をbroadcast()
       ├─ clientが読み込み可能 → receive_client() → append() → broadcast()
       └─ clientが書き込み可能 → send() → 送信済み部分を削除
切断 → disconnect_client() → close/free → 退出通知をbroadcast()
```

## 2. 構造体とグローバル変数

```c
typedef struct s_client {
    int id;
    char *in, *out;
    size_t ni, no;
} t_client;

static t_client c[FD_SETSIZE];
static int server, maxfd, nextid;
```

| 変数 | 役割 |
|---|---|
| `id` | 接続順に増加するクライアントID。`-1` は未使用 |
| `in` / `ni` | 受信バッファ / 保持データ長 |
| `out` / `no` | 送信待ちバッファ / 保持データ長 |
| `c[fd]` | fdを添字としてクライアント状態を管理 |
| `server` | 待ち受けソケットのfd |
| `maxfd` | 監視対象fdの最大値 |
| `nextid` | 次の接続に割り当てるID |

fdはOSが再利用しますが、クライアントIDは `nextid++` により増加します。`ni` と `no` は確保容量ではなく現在のデータ長です。

## 3. `fatal()`

```c
static void fatal(void) {
    write(2, "Fatal error\n", 12);
    exit(1);
}
```

メモリ確保失敗や起動時の致命的エラーで、標準エラー出力にメッセージを書いて終了します。

## 4. `append()`：受信・送信バッファの拡張

```c
static void append(char **buf, size_t *len, const char *s, size_t n) {
    char *p = realloc(*buf, *len + n + 1);
    if (!p) fatal();
    *buf = p;
    for (size_t i = 0; i < n; i++)
        p[*len + i] = s[i];
    *len += n;
    p[*len] = 0;
}
```

`realloc()` で必要な領域を確保し、既存データの後ろに `n` バイト追加します。最後に文字列終端 `\0` を置きます。`char **buf` にする理由は、`realloc()` によってメモリ位置が変わる場合に呼び出し元のポインタを更新するためです。

```text
in = "Hel", ni = 3
追加 "lo\n"（3バイト）
→ in = "Hello\n", ni = 6
```

## 5. `broadcast()`：他クライアントの送信バッファへ追加

```c
static void broadcast(int from, const char *s, size_t n) {
    for (int fd = 0; fd <= maxfd; fd++)
        if (fd != from && c[fd].id >= 0)
            append(&c[fd].out, &c[fd].no, s, n);
}
```

送信元 `from` を除く接続中のクライアントにデータを配る準備をします。**この関数では `send()` しません。** 実際の送信は次回以降の `select()` で書き込み可能になってからです。

## 6. `disconnect_client()`：切断と退出通知

1. 切断するクライアントのIDを保存。
2. `close(fd)` でソケットを閉じる。
3. `free(in)`、`free(out)` でバッファを解放。
4. `c[fd] = (t_client){.id = -1}` で未使用状態に戻す（他のフィールドはゼロ）。
5. `server: client <id> just left\n` を他クライアントに配信予約。

`maxfd` は切断時に減らさないので、使われなくなったfdまで走査する場合がありますが、未使用の `id = -1` を除外します。

## 7. `receive_client()`：受信と改行単位の配信

### 受信

```c
char buf[4096], prefix[64];
int n = recv(fd, buf, sizeof(buf), 0);
if (n <= 0) { disconnect_client(fd); return; }
append(&c[fd].in, &c[fd].ni, buf, n);
```

TCPではデータが分割されるため、受信バッファに蓄積します。`recv()` が0なら切断、負の値でもこの実装は切断します。

### 改行の解析

`start` は現在の行の開始位置、`i` は探索位置です。`in[i] == '\n'` を見つけるたびに次の2回の `broadcast()` を呼びます。

```c
int k = sprintf(prefix, "client %d: ", c[fd].id);
broadcast(fd, prefix, k);
broadcast(fd, c[fd].in + start, i - start + 1);
start = i + 1;
```

例：クライアント2が `Hello\nWorld\n` を送信すると、他のクライアントへ次の内容が配信予約されます。

```text
client 2: Hello
client 2: World
```

### 処理済み部分の削除

最後の改行より前を除き、改行がまだない残りのデータをバッファの先頭へ移動します。

```text
受信: "Hello\nWor"
配信: "client 2: Hello\n"
残り: in = "Wor", ni = 3
```

これは `mini_db` の `client.in.erase(0, pos + 1)` に相当します。

## 8. `main()`：初期化とイベントループ

### 初期化

1. 引数がポート1個でなければ `Wrong number of arguments\n` を出力して終了。
2. `c[i].id = -1` で全要素を未使用にする。
3. `socket(AF_INET, SOCK_STREAM, 0)` でTCPソケットを作成。
4. `addr.sin_addr.s_addr = htonl(0x7f000001)` で `127.0.0.1` に限定。
5. `addr.sin_port = htons(atoi(argv[1]))` でポート設定。
6. `bind()` → `listen(server, 128)`。
7. `maxfd = server` としてイベントループへ。

### `select()`

```c
FD_ZERO(&rd);
FD_ZERO(&wr);
FD_SET(server, &rd);
for (int fd = 0; fd <= maxfd; fd++)
    if (c[fd].id >= 0) {
        FD_SET(fd, &rd);
        if (c[fd].no) FD_SET(fd, &wr);
    }
select(maxfd + 1, &rd, &wr, 0, 0);
```

- `rd`：待ち受けソケットの新規接続、および全クライアントの受信を監視。
- `wr`：`no > 0`（送信待ちデータあり）のクライアントだけを監視。
- `FD_ZERO()` / `FD_SET()` は毎ループ必要。`select()` は集合を変更するため。
- `maxfd + 1` は監視する最大fdより1大きい値。

### 新規接続

`FD_ISSET(server, &rd)` なら `accept()`。fdが `FD_SETSIZE` 以上なら閉じます。正常な接続には `nextid++` でIDを割り当て、すでに接続していたクライアントへ `server: client <id> just arrived\n` を配信予約します。新規クライアント本人には到着通知を送りません。

### 既存クライアントのイベント

`FD_ISSET(fd, &rd)` なら `receive_client(fd)`。その処理で切断されていないことを再確認してから、`FD_ISSET(fd, &wr)` かつ `no > 0` なら `send()` します。

## 9. `send()` と部分送信

```c
int n = send(fd, c[fd].out, c[fd].no, MSG_NOSIGNAL);
```

例えば10バイト中4バイト送れたら、残り6バイトをバッファ先頭に移し、`no = 6` とします。全送信できたら `free(out)` して `out = 0` にします。`MSG_NOSIGNAL` は切断先への送信によるSIGPIPEを防ぐためのLinux向けフラグです。

`mini_db` の `sent` 方式と異なり、`mini_serv` は送信済みデータを実際に前詰めします。

## 10. mini_dbとの対応

| mini_serv | mini_db |
|---|---|
| `main()` の初期化 | `setup()` |
| `main()` の `select()` ループ | `run()` |
| `accept()` の分岐 | `handle_new_connection()` |
| `receive_client()` | `recv_data()` + `process_commands()` |
| `broadcast()` | DBでは `process_command()` が応答を生成 |
| `main()` の `send()` | `send_data()` |
| `disconnect_client()` | `handle_disconnection()` |
| `char *` + `realloc/free` | `std::string` |

## 11. 注意点

- 課題で許可される関数に `epoll()` や `setsockopt()` は含まれていません。
- 課題は `select()` 前提の特別な送受信条件を記載しています。一般的な非ブロッキングソケットにそのまま移植すると、`EAGAIN` 等のエラー処理を追加する必要があります。
- 遅いクライアント向けの `out` は保持しますが、バッファの容量上限はありません。
- `append()` は毎回 `realloc()` するため、頻繁な追加では効率が落ちる可能性があります。
- `maxfd` は切断時に縮小しません。

**要点：** `select()` → `recv()` → `append()` → 改行解析 → `broadcast()` → 次回以降の `select()` → `send()` の順序を理解することが重要です。
