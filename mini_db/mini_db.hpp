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
	int port_;
	char *path_;
	int serverFd_;
	int epollFd_;
	std::map<int, std::string> clients_;
	std::map<std::string, std::string> database_;

	void exit_with_error(const std::string &cause);

	void set_nonblocking(int fd);
	void register_epoll(int fd);
	void unregister_epoll(int fd);

	void handle_new_connection();
	void handle_disconnection(int fd);

	void load_database();
	void save_database();

	std::string process_command(const std::string &command);
	void process_commands(int fd);
	void recv_data(int fd);

	Server();
	Server(const Server &src);
	Server &operator=(const Server &src);
};
