/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   mini_db.hpp                                        :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: tiizuka <tiizuka@student.42tokyo.jp>       +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2026/07/22 16:28:29 by tiizuka           #+#    #+#             */
/*   Updated: 2026/07/22 17:47:47 by tiizuka          ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */

#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <strings.h>
#include <stdlib.h>
#include <signal.h>

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
	// var
	int port_;
	char *path_;
	int serverFd_;
	int epollFd_;
	std::map<int, std::string> clients_;
	std::map<std::string, std::string> database_;

	// helper
	void exit_with_error(const std::string &cause);

	// fd
	void set_nonblocking(int fd);
	void register_epoll(int fd);
	void unregister_epoll(int fd);

	// connection
	void handle_new_connection();
	void handle_disconnection(int fd);

	// database
	void load_database();
	void save_database();

	// recv data & process command
	std::vector<std::string> process_tokens(std::string command);
	std::string process_command(std::string line);
	void process_commands(int fd);
	void recv_data(int fd);

	// = delete
	Server();
	Server(const Server &src);
	Server &operator=(const Server &src);
};
