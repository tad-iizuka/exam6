#pragma once

#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <strings.h>

#include <cstdlib>
#include <csignal>
#include <cstring>

#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include <map>
#include <vector>

class Server
{
	public:
		Server(char *port, char *path);
		~Server();
		void setup();
		void run();
	private:
		int	_port;
		char *_path;
		int _serverFd;
		int _epollFd;
		std::map<int, std::string> _clients;
		std::map<std::string, std::string> _db;
		void exit_with_error(const std::string &cause);
		void set_nb(int fd);
		void reg_ep(int fd);
		void unreg_ep(int fd);
		void newconnect();
		void disconnect(int fd);
		void load_db();
		void save_db();
		std::vector<std::string> tokens(std::string cmd);
		std::string comd(std::string cmd);
		void comds(int fd);
		void rdata(int fd);
		Server();
		Server(const Server &rhs);
		Server &operator=(const Server &rhs);
};
