#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

typedef struct s_session
{
	int		fd;
	int		id;
	char	*read_buf;
	size_t	read_len;
	char	*write_buf;
	size_t	write_len;
	size_t	write_sent;
} Session;

typedef struct s_server
{
	int		fd;
	int		max_fd;
	int		next_id;
	fd_set	master;
	Session	clients[FD_SETSIZE];
} Server;

static void fatal_error(void)
{
	write(2, "Fatal error\n", 12);
	exit(1);
}

static void session_init(Session *s)
{
	s->fd = -1;
	s->id = -1;
	s->read_buf = NULL;
	s->read_len = 0;
	s->write_buf = NULL;
	s->write_len = 0;
	s->write_sent = 0;
}

static char *join_data(const char *a, size_t a_len,
	const char *b, size_t b_len)
{
	char	*p;
	size_t	i;

	p = (char *)malloc(a_len + b_len + 1);
	if (!p)
		return (NULL);
	for (i = 0; i < a_len; ++i)
		p[i] = a[i];
	for (i = 0; i < b_len; ++i)
		p[a_len + i] = b[i];
	p[a_len + b_len] = '\0';
	return (p);
}

static int append_data(char **buf, size_t *len,
	const char *data, size_t data_len)
{
	char	*p;

	p = join_data(*buf, *len, data, data_len);
	if (!p)
		return (-1);
	free(*buf);
	*buf = p;
	*len += data_len;
	return (0);
}

static int queue_message(Session *s, const char *msg, size_t len)
{
	char	*p;
	size_t	pending;

	pending = s->write_len - s->write_sent;
	p = join_data(s->write_buf ? s->write_buf + s->write_sent : NULL,
		pending, msg, len);
	if (!p)
		return (-1);
	free(s->write_buf);
	s->write_buf = p;
	s->write_len = pending + len;
	s->write_sent = 0;
	return (0);
}

static int broadcast(Server *srv, int sender_fd,
	const char *msg, size_t len)
{
	int	fd;

	for (fd = 0; fd <= srv->max_fd; ++fd)
	{
		if (fd == srv->fd || fd == sender_fd)
			continue;
		if (FD_ISSET(fd, &srv->master)
			&& queue_message(&srv->clients[fd], msg, len) < 0)
			return (-1);
	}
	return (0);
}

static int broadcast_client_line(Server *srv, int client_fd,
	const char *line, size_t line_len)
{
	char	prefix[32];
	char	*msg;
	int		prefix_len;
	int		result;

	prefix_len = sprintf(prefix, "client %d: ",
		srv->clients[client_fd].id);
	msg = join_data(prefix, (size_t)prefix_len, line, line_len);
	if (!msg)
		return (-1);
	result = broadcast(srv, client_fd, msg,
		(size_t)prefix_len + line_len);
	free(msg);
	return (result);
}

static int broadcast_server_event(Server *srv, int id, int arrived)
{
	char	msg[64];
	int		len;

	len = sprintf(msg, "server: client %d just %s\n",
		id, arrived ? "arrived" : "left");
	return (broadcast(srv, -1, msg, (size_t)len));
}

static int server_init(Server *srv, int port)
{
	struct sockaddr_in	addr;
	unsigned char		*ip;
	unsigned char		*port_bytes;
	int					fd;

	srv->fd = -1;
	srv->max_fd = -1;
	srv->next_id = 0;
	FD_ZERO(&srv->master);
	for (fd = 0; fd < FD_SETSIZE; ++fd)
		session_init(&srv->clients[fd]);
	srv->fd = socket(AF_INET, SOCK_STREAM, 0);
	if (srv->fd < 0 || srv->fd >= FD_SETSIZE)
		return (-1);
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	ip = (unsigned char *)&addr.sin_addr.s_addr;
	ip[0] = 127;
	ip[1] = 0;
	ip[2] = 0;
	ip[3] = 1;
	port_bytes = (unsigned char *)&addr.sin_port;
	port_bytes[0] = (unsigned char)(port >> 8);
	port_bytes[1] = (unsigned char)port;
	if (bind(srv->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0
		|| listen(srv->fd, 128) < 0)
		return (-1);
	FD_SET(srv->fd, &srv->master);
	srv->max_fd = srv->fd;
	return (0);
}

static void server_cleanup(Server *srv)
{
	int	fd;

	for (fd = 0; fd < FD_SETSIZE; ++fd)
	{
		if (srv->clients[fd].fd >= 0)
			close(srv->clients[fd].fd);
		free(srv->clients[fd].read_buf);
		free(srv->clients[fd].write_buf);
		session_init(&srv->clients[fd]);
	}
	if (srv->fd >= 0)
		close(srv->fd);
}

static int accept_client(Server *srv)
{
	int	fd;
	int	id;

	fd = accept(srv->fd, NULL, NULL);
	if (fd < 0)
		return (-1);
	if (fd >= FD_SETSIZE)
	{
		close(fd);
		return (0);
	}
	id = srv->next_id++;
	srv->clients[fd].fd = fd;
	srv->clients[fd].id = id;
	FD_SET(fd, &srv->master);
	if (fd > srv->max_fd)
		srv->max_fd = fd;
	return (broadcast_server_event(srv, id, 1));
}

static int disconnect_client(Server *srv, int fd)
{
	int	id;

	id = srv->clients[fd].id;
	FD_CLR(fd, &srv->master);
	close(fd);
	free(srv->clients[fd].read_buf);
	free(srv->clients[fd].write_buf);
	session_init(&srv->clients[fd]);
	while (srv->max_fd > srv->fd
		&& !FD_ISSET(srv->max_fd, &srv->master))
		--srv->max_fd;
	return (broadcast_server_event(srv, id, 0));
}

static int process_lines(Server *srv, int fd)
{
	Session	*s;
	char	*nl;
	size_t	line_len;
	size_t	i;

	s = &srv->clients[fd];
	while (s->read_buf && (nl = strstr(s->read_buf, "\n")) != NULL)
	{
		line_len = (size_t)(nl - s->read_buf) + 1;
		if (broadcast_client_line(srv, fd, s->read_buf, line_len) < 0)
			return (-1);
		s->read_len -= line_len;
		for (i = 0; i < s->read_len; ++i)
			s->read_buf[i] = s->read_buf[line_len + i];
		s->read_buf[s->read_len] = '\0';
		if (s->read_len == 0)
		{
			free(s->read_buf);
			s->read_buf = NULL;
		}
	}
	return (0);
}

static int receive_data(Server *srv, int fd)
{
	char	buf[4096];
	ssize_t	n;

	n = recv(fd, buf, sizeof(buf), 0);
	if (n == 0)
		return (disconnect_client(srv, fd));
	if (n < 0)
		return (-1);
	if (append_data(&srv->clients[fd].read_buf,
		&srv->clients[fd].read_len, buf, (size_t)n) < 0)
		return (-1);
	return (process_lines(srv, fd));
}

static int send_data(Server *srv, int fd)
{
	Session	*s;
	ssize_t	n;

	s = &srv->clients[fd];
	if (!s->write_buf)
		return (0);
	n = send(fd, s->write_buf + s->write_sent,
		s->write_len - s->write_sent, 0);
	if (n <= 0)
		return (-1);
	s->write_sent += (size_t)n;
	if (s->write_sent == s->write_len)
	{
		free(s->write_buf);
		s->write_buf = NULL;
		s->write_len = 0;
		s->write_sent = 0;
	}
	return (0);
}

static int server_run(Server *srv)
{
	fd_set	read_fds;
	fd_set	write_fds;
	int		fd;
	int		ret;

	while (1)
	{
		read_fds = srv->master;
		FD_ZERO(&write_fds);
		for (fd = 0; fd <= srv->max_fd; ++fd)
			if (srv->clients[fd].fd >= 0
				&& srv->clients[fd].write_buf)
				FD_SET(fd, &write_fds);
		ret = select(srv->max_fd + 1, &read_fds, &write_fds, NULL, NULL);
		if (ret < 0)
		{
			if (errno == EINTR)
				continue;
			return (-1);
		}
		for (fd = 0; fd <= srv->max_fd; ++fd)
		{
			if (FD_ISSET(fd, &read_fds))
			{
				if (fd == srv->fd)
				{
					if (accept_client(srv) < 0)
						return (-1);
				}
				else if (srv->clients[fd].fd == fd
					&& receive_data(srv, fd) < 0)
					return (-1);
			}
			if (FD_ISSET(fd, &write_fds)
				&& srv->clients[fd].fd == fd
				&& send_data(srv, fd) < 0)
				return (-1);
		}
	}
	return (0);
}

int main(int argc, char **argv)
{
	Server	srv;
	int		port;

	if (argc != 2)
	{
		write(2, "Wrong number of arguments\n", 26);
		return (1);
	}
	port = atoi(argv[1]);
	if (port < 1 || port > 65535)
		fatal_error();
	if (server_init(&srv, port) < 0)
	{
		server_cleanup(&srv);
		fatal_error();
	}
	if (server_run(&srv) < 0)
	{
		server_cleanup(&srv);
		fatal_error();
	}
	server_cleanup(&srv);
	return (0);
}
