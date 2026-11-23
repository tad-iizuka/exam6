#include "mini_db.hpp"

volatile sig_atomic_t g_running = 1;

Server::Server(char *port, char *path)
    : port_(atoi(port)), path_(path), server(-1) {}

Server::~Server() {
    for (std::map<int, Client>::iterator it = clients_.begin(); it != clients_.end(); ++it)
        close(it->first);
    if (server != -1)
        close(server);
}

void Server::exit_with_error(const std::string &cause) {
    std::cerr << "Fatal error: " << cause << std::endl;
    exit(1);
}

void signal_handler(int signum) {
    (void)signum;
    g_running = 0;
}

void Server::set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        exit_with_error("fcntl");
}

void Server::handle_new_connection() {
    while (g_running) {
        int fd = accept(server, NULL, NULL);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            if (errno == EINTR)
                continue;
            exit_with_error("accept");
        }
        if (fd >= FD_SETSIZE) {
            close(fd);
            continue;
        }
        set_nonblocking(fd);
        clients_[fd] = Client();
    }
}

void Server::handle_disconnection(int fd) {
    clients_.erase(fd);
    close(fd);
}

void Server::load_database() {
    std::ifstream ifs(path_);
    if (!ifs.is_open())
        return;
    std::string line;
    while (std::getline(ifs, line)) {
        std::istringstream iss(line);
        std::string key, value;
        if (iss >> key >> value)
            database_[key] = value;
    }
}

void Server::save_database() {
    std::ofstream ofs(path_);
    if (!ofs.is_open())
        exit_with_error("ofs");
    for (std::map<std::string, std::string>::iterator it = database_.begin();
         it != database_.end(); ++it)
        ofs << it->first << " " << it->second << "\n";
    ofs.close();
    if (!ofs)
        exit_with_error("save_database");
}

std::string Server::process_command(const std::string &command) {
    std::istringstream iss(command);
    std::string cmd, key, value, extra;
    if (!(iss >> cmd))
        return "2\n";
    if (cmd == "POST") {
        if (!(iss >> key >> value) || (iss >> extra))
            return "2\n";
        database_[key] = value;
        return "0\n";
    }
    if (cmd == "GET") {
        if (!(iss >> key) || (iss >> extra))
            return "2\n";
        std::map<std::string, std::string>::const_iterator it = database_.find(key);
        if (it == database_.end())
            return "1\n";
        return "0 " + it->second + "\n";
    }
    if (cmd == "DELETE") {
        if (!(iss >> key) || (iss >> extra))
            return "2\n";
        return database_.erase(key) ? "0\n" : "1\n";
    }
    return "2\n";
}

void Server::process_commands(int fd) {
    Client &client = clients_.find(fd)->second;
    size_t pos;
    while ((pos = client.in.find('\n')) != std::string::npos) {
        std::string command = client.in.substr(0, pos);
        client.in.erase(0, pos + 1);
        client.out += process_command(command);
    }
}

void Server::recv_data(int fd) {
    char buf[4096];
    while (g_running) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            clients_.find(fd)->second.in.append(buf, static_cast<size_t>(n));
            process_commands(fd);
        } else if (n == 0) {
            handle_disconnection(fd);
            return;
        } else {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            handle_disconnection(fd);
            return;
        }
    }
}

void Server::send_data(int fd) {
    Client &client = clients_.find(fd)->second;
    while (client.sent < client.out.size()) {
        ssize_t n = send(fd, client.out.data() + client.sent,
                         client.out.size() - client.sent, 0);
        if (n > 0)
            client.sent += static_cast<size_t>(n);
        else if (n < 0 && errno == EINTR)
            continue;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        else {
            handle_disconnection(fd);
            return;
        }
    }
    client.out.clear();
    client.sent = 0;
}

void Server::run() {
    while (g_running) {
        fd_set rd, wr;
        FD_ZERO(&rd);
        FD_ZERO(&wr);
        FD_SET(server, &rd);
        int maxfd = server;
        for (std::map<int, Client>::iterator it = clients_.begin();
             it != clients_.end(); ++it) {
            FD_SET(it->first, &rd);
            if (it->second.sent < it->second.out.size())
                FD_SET(it->first, &wr);
            if (it->first > maxfd)
                maxfd = it->first;
        }
        int ret = select(maxfd + 1, &rd, &wr, NULL, NULL);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            exit_with_error("select");
        }
        if (!g_running)
            break;
        if (FD_ISSET(server, &rd))
            handle_new_connection();
        for (std::map<int, Client>::iterator it = clients_.begin();
             it != clients_.end() && g_running;) {
            int fd = it->first;
            ++it;
            if (FD_ISSET(fd, &rd))
                recv_data(fd);
            if (clients_.find(fd) != clients_.end() && FD_ISSET(fd, &wr))
                send_data(fd);
        }
    }
    save_database();
}

void Server::setup() {
    signal(SIGINT, signal_handler);
    signal(SIGPIPE, SIG_IGN);
    load_database();
    server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0)
        exit_with_error("socket");
    if (server >= FD_SETSIZE)
        exit_with_error("FD_SETSIZE");
    int opt = 1;
    if (setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        exit_with_error("setsockopt");
    set_nonblocking(server);
    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port_);
    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        exit_with_error("bind");
    if (listen(server, 128) < 0)
        exit_with_error("listen");
    std::cout << "ready" << std::endl;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        write(2, "Usage: ./a.out <port> <path>\n", 29);
        return 1;
    }
    Server server(argv[1], argv[2]);
    server.setup();
    server.run();
    return 0;
}
