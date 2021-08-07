#include <nix/config.h>
#include <nix/crypto.hh>
#include <nix/store-api.hh>

#include <httplib.h>
#include <optional>

class Server {
public:
  Server(std::string host, uint16_t port,
         std::optional<nix::SecretKey> secretKey);
  ~Server();

  int bind();
  void listen();
  void stop();
  bool isRunning();

private:
  std::string host;
  uint64_t port;
  nix::ref<nix::Store> store;
  std::optional<nix::SecretKey> secretKey;
  httplib::Server server;

  void handleException(const httplib::Request &req, httplib::Response &res,
                       std::exception &e);
  void getNixCacheInfo(const httplib::Request &req, httplib::Response &res);
  void getNarInfo(const httplib::Request &req, httplib::Response &res);
  void getNar(const httplib::Request &req, httplib::Response &res);
  void getNarDeprecated(const httplib::Request &req, httplib::Response &res);
  void getRealisation(const httplib::Request &req, httplib::Response &res);
};
