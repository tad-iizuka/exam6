#pragma once

#include <sys/socket.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <strings.h>
#include <stdlib.h>

#include <csignal>

#include <string>
#include <vector>
#include <map>
#include <iostream>
#include <fstream>
#include <sstream>

class Server
{
	public:
		Server(char *po, char *pa);
		~Server();
		void setup();
		void run();

	private:
		int _po;
		char *_pa;
		int _svFd;
		int _epFd;
		std::map<int, std::string> _cs;
		std::map<std::string, std::string> _db;
		void error_exit(const std::string &r);
		void snb(int fd);
		void rep(int fd);
		void uep(int fd);
		void ncon();
		void dcon(int fd);
		void rdb();
		void wdb();
		std::vector<std::string> pars(std::string c);
		std::string com(std::string c);
		void coms(int fd);
		void rcv(int fd);
		Server();
		Server(const Server &rhs);
		Server &operator=(const Server &rhs);
};
