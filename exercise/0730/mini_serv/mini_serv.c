#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>


int	g_next_id;
int	g_server_fd;
int	g_max_fd;
fd_set	g_master_fds;

typedef struct s_db {
	int	id;
	char	*b;
} t_db;

t_db g_db[FD_SETSIZE];

int	extmsg(char **b, char **m) {
	char *new_b;
	int	i;

	*m = 0;
	if (*b == 0)
		return (0);
	i = 0;
	while ((*b)[i]) {
		if ((*b)[i] == '\n') {
			new_b = calloc(1, sizeof(*new_b) * (strlen(*b + i + 1) + 1));
			if (new_b == 0)
				return (-1);
			strcpy(new_b, *b + i + 1);
			*m = *b;
			(*m)[i + 1] = 0;
			*b = new_b;
			return (1);
		}
		i++;
	}
	return (0);
}

char *sjoin(char *b, char *a) {
	char *new_b;
	int	i;

	if (b == 0)
		i = 0;
	else
		i = strlen(b);
	new_b = malloc(sizeof(*new_b) * (i + strlen(a) + 1));
	if (new_b == 0)
		return (0);
	new_b[0] = 0;
	if (b != 0)
		strcat(new_b, b);
	free(b);
	strcat(new_b, a);
	return (new_b);
}

void plog(char *s) {
	if (s != NULL)
		write(2, s, strlen(s));
}

void ferr(void) {
	write(2, "Fatal error\n", strlen("Fatal error\n"));
	exit(1);
}

void clean(void) {
	for (int fd = 0; fd <= g_max_fd; ++fd) {
		if (FD_ISSET(fd, &g_master_fds) && fd != g_server_fd) {
			FD_CLR(fd, &g_master_fds);
			g_db[fd].id = -1;
			free(g_db[fd].b);
			close(fd);
		}
	}
	FD_ZERO(&g_master_fds);
	close(g_server_fd);
}

void bcast(int s_fd, const char *msg) {
	for (int fd = 0; fd <= g_max_fd; ++fd) {
		if (!FD_ISSET(fd, &g_master_fds))
			continue;
		if (fd == g_server_fd || fd == s_fd)
			continue;
		send(fd, msg, strlen(msg), 0);
	}
}

void newconnect(void) {
	int new_fd = accept(g_server_fd, NULL, NULL);

	if (new_fd < 0)
		return ;
	if (new_fd >= FD_SETSIZE) {
		close(new_fd);
		return ;
	}
	FD_SET(new_fd, &g_master_fds);
	if (new_fd > g_max_fd)
		g_max_fd = new_fd;
	g_db[new_fd].id = g_next_id++;
	g_db[new_fd].b = NULL;
	char msg[64];
	sprintf(msg, "server: client %d just arrived\n", g_db[new_fd].id);
	bcast(new_fd, msg);
}

void disconnect(int fd) {
	char msg[64];

	sprintf(msg, "server: client %d just left\n", g_db[fd].id);
	bcast(fd, msg);
	FD_CLR(fd, &g_master_fds);
	free(g_db[fd].b);
	g_db[fd].b = NULL;
	g_db[fd].id = -1;
	close(fd);
	while (g_max_fd > g_server_fd && !FD_ISSET(g_max_fd, &g_master_fds))
		g_max_fd--;
}

void rdata(int fd) {
	char d[4096];
	int r1 = recv(fd, d, sizeof(d) - 1, 0);
	if (r1 > 0) {
		d[r1] = '\0';
		char *jstr = sjoin(g_db[fd].b, d);
		if (jstr == NULL) {
			clean();
			ferr();
		}
		g_db[fd].b = jstr;
		char *msg;
		while (1) {
			int r2 = extmsg(&g_db[fd].b, &msg);
			if (r2 > 0) {
				char pfix[32];
				sprintf(pfix, "client %d: ", g_db[fd].id);
				char *msg_w_pfix = malloc(strlen(pfix) + strlen(msg) + 1);
				if (msg_w_pfix == NULL) {
					free(msg);
					clean();
					ferr();
				}
				strcpy(msg_w_pfix, pfix);
				strcat(msg_w_pfix, msg);
				free(msg);
				bcast(fd, msg_w_pfix);
				free(msg_w_pfix);
			} else if (r2 == 0) {
				break;
			} else {
				clean();
				ferr();
			}
		}
	} else if (r1 <= 0) {
		disconnect(fd);
	}
}

void setup(char *argv[]) {
	g_server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (g_server_fd < 0)
		ferr();
	struct sockaddr_in addr;
	bzero(&addr, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(atoi(argv[1]));
	if (bind(g_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(g_server_fd);
		ferr();
	}
	if (listen(g_server_fd, 128) < 0) {
		close(g_server_fd);
		ferr();
	}
	FD_ZERO(&g_master_fds);
	FD_SET(g_server_fd, &g_master_fds);
	g_max_fd = g_server_fd;
}

int main(int argc, char *argv[]) {
	if (argc != 2) {
		write(2, "Wrong number of arguments\n", strlen("Wrong number of arguments\n"));
		return (1);
	}
	setup(argv);
	while (1) {
		fd_set r_fds = g_master_fds;
		int r = select(g_max_fd + 1, &r_fds, NULL, NULL, NULL);
		if (r > 0) {
			for (int fd = 0; fd <= g_max_fd; ++fd) {
				if (FD_ISSET(fd, &r_fds)) {
					if (fd == g_server_fd)
						newconnect();
					else
						rdata(fd);
				}
			}
		} else if (r < 0) {
			if (errno == EINTR)
				continue;
			clean();
			ferr();
		}
	}
	return (0);
};
