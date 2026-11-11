#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/select.h>

typedef struct s_client {
    int id;
    char *in, *out;
    size_t ni, no, sent;
} t_client;

static t_client c[FD_SETSIZE];
static int server, maxfd, nextid;

static void fatal(void)
{
    write(2, "Fatal error\n", 12);
    exit(1);
}

static void append(char **buf, size_t *len, const char *s, size_t n)
{
    char *p = realloc(*buf, *len + n + 1);
    if (!p) fatal();
    *buf = p;
    for (size_t i = 0; i < n; i++) p[*len + i] = s[i];
    *len += n;
    p[*len] = 0;
}

static void broadcast(int from, const char *s, size_t n)
{
    for (int fd = 0; fd <= maxfd; fd++)
        if (fd != from && c[fd].id >= 0)
            append(&c[fd].out, &c[fd].no, s, n);
}

static void disconnect_client(int fd)
{
    char msg[80];
    int id = c[fd].id;
    close(fd);
    free(c[fd].in);
    free(c[fd].out);
    c[fd] = (t_client){.id = -1};
    int n = sprintf(msg, "server: client %d just left\n", id);
    broadcast(fd, msg, n);
}

static void receive_client(int fd)
{
    char buf[4096], prefix[64];
    int n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) { disconnect_client(fd); return; }
    append(&c[fd].in, &c[fd].ni, buf, n);
    size_t start = 0;
    for (size_t i = 0; i < c[fd].ni; i++)
        if (c[fd].in[i] == '\n') {
            int k = sprintf(prefix, "client %d: ", c[fd].id);
            broadcast(fd, prefix, k);
            broadcast(fd, c[fd].in + start, i - start + 1);
            start = i + 1;
        }
    if (start) {
        c[fd].ni -= start;
        for (size_t i = 0; i < c[fd].ni; i++)
            c[fd].in[i] = c[fd].in[start + i];
        c[fd].in[c[fd].ni] = 0;
    }
}

int main(int argc, char **argv)
{
    struct sockaddr_in addr;
    fd_set rd, wr;
    if (argc != 2) {
        write(2, "Wrong number of arguments\n", 26);
        return 1;
    }
    for (int i = 0; i < FD_SETSIZE; i++) c[i].id = -1;
    server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0 || server >= FD_SETSIZE) fatal();
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7f000001);
    addr.sin_port = htons(atoi(argv[1]));
    if (bind(server, (void *)&addr, sizeof(addr)) < 0 || listen(server, 128) < 0)
        fatal();
    maxfd = server;
    while (1) {
        FD_ZERO(&rd);
        FD_ZERO(&wr);
        FD_SET(server, &rd);
        for (int fd = 0; fd <= maxfd; fd++)
            if (c[fd].id >= 0) {
                FD_SET(fd, &rd);
                if (c[fd].no > c[fd].sent) FD_SET(fd, &wr);
            }
        if (select(maxfd + 1, &rd, &wr, 0, 0) < 0) continue;
        if (FD_ISSET(server, &rd)) {
            int fd = accept(server, 0, 0);
            if (fd >= 0) {
                if (fd >= FD_SETSIZE) close(fd);
                else {
                    char msg[80];
                    int id = nextid++;
                    int n = sprintf(msg, "server: client %d just arrived\n", id);
                    broadcast(fd, msg, n);
                    c[fd] = (t_client){.id = id};
                    if (fd > maxfd) maxfd = fd;
                }
            }
        }
        for (int fd = 0; fd <= maxfd; fd++) {
            if (c[fd].id < 0) continue;
            if (FD_ISSET(fd, &rd)) receive_client(fd);
            if (c[fd].id >= 0 && FD_ISSET(fd, &wr) && c[fd].no > c[fd].sent) {
                int n = send(fd, c[fd].out + c[fd].sent,
                             c[fd].no - c[fd].sent, MSG_NOSIGNAL);
                if (n > 0) {
                    c[fd].sent += n;
                    if (c[fd].sent == c[fd].no) {
                        free(c[fd].out);
                        c[fd].out = 0;
                        c[fd].no = c[fd].sent = 0;
                    }
                } else if (n < 0) disconnect_client(fd);
            }
        }
    }
}
