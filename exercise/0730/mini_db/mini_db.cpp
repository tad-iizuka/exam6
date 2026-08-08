#include "mini_db.hpp"

volatile sig_atomic_t g_run = 1;

void signal_handler(int sig) {
	(void)sig;
	g_run = 0;
}
Server::Server(char *port, char *path) : _port(atoi(port)), _path(path), _serverFd(-1), _epollFd(-1) {};
Server::~Server() {
	if (_epollFd != -1)
		close(_epollFd);
	for (std::map<int, std::string>::iterator it = _clients.begin();
		it != _clients.end(); ++it) {
		close(it->first);
	}
	if (_serverFd != -1)
		close(_serverFd);
};
void Server::setup() {
	signal(SIGINT, signal_handler);
	signal(SIGPIPE, SIG_IGN);
	load_db();
	_serverFd = socket(AF_INET, SOCK_STREAM, 0);
	if (_serverFd < 0)
		exit_with_error("socket");
	int opt = 1;
	if (setsockopt(_serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
		exit_with_error("setsockopt");
	set_nb(_serverFd);
	struct sockaddr_in addr;
	bzero(&addr, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(_port);
	if (bind(_serverFd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		exit_with_error("bind");
	if (listen(_serverFd, 128) < 0)
		exit_with_error("listen");
	_epollFd = epoll_create1(0);
	if (_epollFd < 0)
		exit_with_error("epoll_create1");
	reg_ep(_serverFd);
	std::cout << "ready" << std::endl;
};
void Server::run() {
	struct epoll_event events[64];
	while (g_run) {
		int r = epoll_wait(_epollFd, events, 64, -1);
		if (r > 0) {
			for (int i = 0; i < r; ++i) {
				if (events[i].data.fd == _serverFd)
					newconnect();
				else
					rdata(events[i].data.fd);
			}
		} else if (r < 0) {
			if (errno == EINTR)
				continue;
			else
				exit_with_error("epoll_wait");
		}
	}
	save_db();
};
void Server::exit_with_error(const std::string &cause) {
	std::cerr << "Fatal error: " << cause << std::endl;
	exit(1);
};
void Server::set_nb(int fd) {
	int flag = fcntl(fd, F_GETFL, 0);
	if (flag < 0)
		exit_with_error("fcntl");
	if (fcntl(fd, F_SETFL, flag | O_NONBLOCK) < 0)
		exit_with_error("fcntl");
};
void Server::reg_ep(int fd) {
	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.fd = fd;
	if (epoll_ctl(_epollFd, EPOLL_CTL_ADD, fd, &ev) < 0)
		exit_with_error("reg_ep");
};
void Server::unreg_ep(int fd) {
	if (epoll_ctl(_epollFd, EPOLL_CTL_DEL, fd, NULL) < 0)
		exit_with_error("unreg_ep");
};
void Server::newconnect() {
	while (1) {
		int client_fd = accept(_serverFd, NULL, NULL);
		if (client_fd >= 0) {
			set_nb(client_fd);
			reg_ep(client_fd);
			_clients[client_fd] = "";
		}
		else {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return ;
			else
				exit_with_error("accept");
		}
	}
};
void Server::disconnect(int fd) {
	_clients.erase(fd);
	unreg_ep(fd);
	close(fd);
};
void Server::load_db() {
	std::ifstream ifs(_path);
	if (!ifs.is_open())
		return;
	std::string line;
	while (std::getline(ifs, line)) {
		std::istringstream iss(line);
		std::string key;
		std::string value;
		if (iss >> key >> value)
			_db[key] = value;
	}
	ifs.close();
};
void Server::save_db() {
	std::ofstream ofs(_path);
	if (!ofs.is_open())
		exit_with_error("ofs");
	for (std::map<std::string, std::string>::iterator it = _db.begin();
		it != _db.end(); ++it) {
			ofs << it->first << " " << it->second << "\n";
	}
	ofs.close();
};
std::vector<std::string> Server::tokens(std::string cmd) {
	std::istringstream iss(cmd);
	std::vector<std::string> tks;
	std::string token;
	while (iss >> token)
		tks.push_back(token);
	return tks;
};
std::string Server::comd(std::string cmd) {
	std::vector<std::string> tks = tokens(cmd);
	if (tks.empty()) {
		return "2\n";
	} else if (tks[0] == "POST" && tks.size() == 3) {
		_db[tks[1]] = tks[2];
		return "0\n";
	} else if (tks[0] == "GET" && tks.size() == 2) {
		if (_db.count(tks[1]))
			return "0 " + _db[tks[1]] + "\n";
		else
			return "1\n";
	} else if (tks[0] == "DELETE" && tks.size() == 2) {
		if (_db.erase(tks[1]))
			return "0\n";
		else
			return "1\n";
	} else {
		return "2\n";
	}
};
void Server::comds(int fd) {
	std::string &buff = _clients[fd];
	size_t pos;
	while ((pos = buff.find("\n")) != std::string::npos) {
		std::string cmd = buff.substr(0, pos);
		buff.erase(0, pos + 1);
		std::string res = comd(cmd);
		send(fd, res.c_str(), res.length(), 0);
	}
};
void Server::rdata(int fd) {
	char buff[1024];

	while (1) {
		int r = recv(fd, buff, sizeof(buff) - 1, 0);
		if (r > 0) {
			buff[r] = '\0';
			_clients[fd] += buff;
		} else if (r == 0) {
			disconnect(fd);
			return;
		} else {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				break;
			} else {
				disconnect(fd);
				return;
			}
		}
	}
	comds(fd);
};

int main(int argc, char *argv[]) {
	if (argc != 3) {
		std::cerr << "Usage: " << argv[0] << " <port> <path>" << std::endl;
		return 1;
	}
	Server server(argv[1], argv[2]);
	server.setup();
	server.run();
	return 0;
}
