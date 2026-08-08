#include "mini_db.hpp"

volatile sig_atomic_t _run = 1;

void _handler(int sig) {
  (void)sig;
  _run = 0;
}

Server::Server(char *po, char *pa) : \
  _po(atoi(po)), _pa(pa), _svFd(-1), _epFd(-1) {};
Server::~Server() {
  if(_epFd != -1)
    close(_epFd);
  for (std::map<int, std::string>::iterator it = _cs.begin();
    it != _cs.end(); ++it) {
      close(it->first);
  }
  if (_svFd != -1)
    close(_svFd);
};
void Server::setup() {
  signal(SIGINT, _handler);
  signal(SIGPIPE, SIG_IGN);
  _svFd = socket(AF_INET, SOCK_STREAM, 0);
  if (_svFd < 0)
    error_exit("socket");
  int opt = 1;
  if (setsockopt(_svFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    error_exit("setsockopt");
  snb(_svFd);
  struct sockaddr_in addr;
  bzero(&addr, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(_po);
  if (bind(_svFd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    error_exit("bind");
  if (listen(_svFd, 128) < 0)
    error_exit("listen");
  _epFd = epoll_create1(0);
  if (_epFd < 0)
    error_exit("epoll_create1");
  rep(_svFd);
  rdb();
  std::cout << "ready" << std::endl; 
};
void Server::run() {
  struct epoll_event e[64];

  while (_run) {
    int n = epoll_wait(_epFd, e, 64, -1);
    if (n > 0) {
      for (int i = 0; i < n; ++i) {
        if (e[i].data.fd == _svFd)
          ncon();
        else
          rcv(e[i].data.fd);
      }
    } else if (n < 0) {
      if (errno == EINTR)
        continue;
      else
        error_exit("epoll_wait");
    }
  }
  wdb();
};
void Server::error_exit(const std::string &r) {
  std::cerr << "Fatal error: " << r << std::endl;
  exit(1);
};
void Server::snb(int fd) {
  int f = fcntl(fd, F_GETFL, 0);
  if (f < 0)
    error_exit("fcntl");
  if (fcntl(fd, F_SETFL, f | O_NONBLOCK) < 0)
    error_exit("fcntl");
};
void Server::rep(int fd) {
  struct epoll_event e;
  e.events = EPOLLIN;
  e.data.fd = fd;
  if (epoll_ctl(_epFd, EPOLL_CTL_ADD, fd, &e) < 0)
    error_exit("epoll_ctl");
};
void Server::uep(int fd) {
  if (epoll_ctl(_epFd, EPOLL_CTL_DEL, fd, NULL) < 0)
    error_exit("epoll_ctl");
};
void Server::ncon() {
  while (1) {
    int fd = accept(_svFd, NULL, NULL);
    if (fd >= 0) {
      snb(fd);
      rep(fd);
      _cs[fd] = "";
    }
    else {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return;
      else
        error_exit("accept");
    }
  }
};
void Server::dcon(int fd) {
  _cs.erase(fd);
  uep(fd);
  close(fd);
};
void Server::rdb() {
  std::ifstream ifs(_pa);
  if (!ifs.is_open())
    return;
  std::string l;
  while (std::getline(ifs, l)) {
    std::istringstream iss(l);
    std::string k;
    std::string v;
    if (iss >> k >> v)
      _db[k] = v;
  }
  ifs.close();
};
void Server::wdb() {
  std::ofstream ofs(_pa);
  if (!ofs.is_open())
    error_exit("ofs");
  for (std::map<std::string, std::string>::iterator it = _db.begin();
    it != _db.end(); ++it) {
      ofs << it->first << " " << it->second << "\n";
  }
  ofs.close();
};
std::vector<std::string> Server::pars(std::string c) {
  std::istringstream iss(c);
  std::vector<std::string> ts;
  std::string t;
  while (iss >> t)
    ts.push_back(t);
  return ts;
};
std::string Server::com(std::string c) {
  std::vector<std::string> ts = pars(c);

  if (ts.empty()) {
    return "2\n";
  } else if (ts[0] == "POST" && ts.size() == 3) {
    _db[ts[1]] = ts[2];
    return "0\n";
  } else if (ts[0] == "GET" && ts.size() == 2) {
    if (_db.count(ts[1]))
      return "0 " + _db[ts[1]] + "\n";
    else
      return "1\n";
  } else if (ts[0] == "DELETE" && ts.size() == 2) {
    if (_db.erase(ts[1]))
      return "0\n";
    else
      return "1\n";
  } else {
    return "2\n";
  }
};
void Server::coms(int fd) {
  std::string &b = _cs[fd];
  size_t pos;
  while ((pos = b.find("\n")) != std::string::npos) {
    std::string c = b.substr(0, pos);
    b.erase(0, pos + 1);
    std::string r = com(c);
    send(fd, r.c_str(), r.length(), 0);
  }
};
void Server::rcv(int fd) {
  char b[1024];

  while (1) {
    int n = recv(fd, b, sizeof(b) - 1, 0);
    if (n > 0) {
      b[n] = '\0';
      _cs[fd] += b;
    } else if (n == 0) {
      dcon(fd);
      return;
    } else {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      } else {
        dcon(fd);
        return;
      }
    }
  }
  coms(fd);
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
};
