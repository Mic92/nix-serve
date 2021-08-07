#include "server.hh"

#include <stdio.h>

#include <nix/archive.hh>
#include <nix/config.h>
#include <nix/globals.hh>
#include <nix/path.hh>
#include <nix/shared.hh>
#include <nix/store-api.hh>

#include <httplib.h>
#include <nlohmann/json.hpp>

std::string stripTrailingSlash(std::string s) {
  if (!s.empty() && s.back() == '/') {
    s.pop_back();
  }
  return s;
}

void notFound(httplib::Response &res, std::string msg) {
  res.status = 404;
  res.set_content(msg, "text/plain");
}

struct HttpSink : nix::Sink {
  httplib::DataSink &inner;

  HttpSink(httplib::DataSink &sink) : inner(sink) {}

  void operator()(std::string_view data) override {
    inner.write(data.data(), data.size());
  }
};

Server::Server(std::string host, uint16_t port,
               std::optional<nix::SecretKey> secretKey)
    : host(host), port(port), store(nix::openStore()), secretKey(secretKey) {}

Server::~Server() {}

void Server::handleException(const httplib::Request &req,
                             httplib::Response &res, std::exception &e) {
  res.status = 500;
  auto fmt = "Error 500\n%s";
  char buf[BUFSIZ];
  snprintf(buf, sizeof(buf), fmt, e.what());
  res.set_content(buf, "text/plain");
};

void Server::getNixCacheInfo(const httplib::Request &req,
                             httplib::Response &res) {
  auto resp("StoreDir: " + nix::settings.nixStore +
            "\nWantMassQuery: 1\nPriority: 30\n");
  res.set_content(resp, "text/plain");
}

void Server::getNarInfo(const httplib::Request &req, httplib::Response &res) {
  auto hashPart = req.matches[1];
  auto storePath = store->queryPathFromHashPart(hashPart);
  if (!storePath) {
    return notFound(res, "No such path.\n");
  }
  auto info = store->queryPathInfo(*storePath);
  auto narHash = info->narHash.to_string(nix::Base32, false);
  auto resp = "StorePath: " + store->printStorePath(*storePath) + "\n" +
              "URL: nar/" + hashPart.str() + "-" + narHash + ".nar\n" +
              "Compression: none\n" + "NarHash: " + narHash + "\n" +
              "NarSize: " + std::to_string(info->narSize) + "\n";
  if (!info->references.empty()) {
    resp += "References:";
    for (auto &i : info->references) {
      resp += " " + stripTrailingSlash(store->printStorePath(i));
    }
    resp += "\n";
  }
  if (info->deriver) {
    resp += "Deriver: " +
            stripTrailingSlash(store->printStorePath(*info->deriver)) + "\n";
  }

  if (secretKey) {
    resp += "Sig: " + secretKey->signDetached(info->fingerprint(*store)) + "\n";
  }
  res.set_content(resp, "text/plain");
}

void Server::getNar(const httplib::Request &req, httplib::Response &res) {
  auto hashPart = req.matches[1];
  auto expectedNarHash =
      nix::Hash::parseAny(req.matches[2].str(), nix::htSHA256);
  auto storePath = store->queryPathFromHashPart(hashPart);
  if (!storePath) {
    return notFound(res, "No such path.\n");
  }
  auto info = store->queryPathInfo(*storePath);
  if (info->narHash != expectedNarHash) {
    return notFound(res,
                    "Incorrect NAR hash. Maybe the path has been recreated.\n");
  }
  res.set_chunked_content_provider(
      "text/plain", [this, storePath](size_t offset, httplib::DataSink &sink) {
        HttpSink s(sink);
        try {
          nix::dumpPath(store->printStorePath(*storePath), s);
        } catch (nix::Error &e) {
          std::cerr << e.what() << std::endl;
          sink.done();
          return false;
        }
        sink.done();
        return true;
      });
}

void Server::getNarDeprecated(const httplib::Request &req,
                              httplib::Response &res) {
  auto hashPart = req.matches[1];
  auto storePath = store->queryPathFromHashPart(hashPart);
  if (!storePath) {
    return notFound(res, "No such path.\n");
  }
  auto info = store->queryPathInfo(*storePath);
  res.set_chunked_content_provider(
      "text/plain", [this, storePath](size_t offset, httplib::DataSink &sink) {
        HttpSink s(sink);
        try {
          nix::dumpPath(store->printStorePath(*storePath), s);
        } catch (nix::Error &e) {
          std::cerr << e.what() << std::endl;
          sink.done();
          return false;
        }
        sink.done();
        return true;
      });
}

void Server::getRealisation(const httplib::Request &req,
                            httplib::Response &res) {
  auto outputId = req.matches[1];
  auto realisation = store->queryRealisation(nix::DrvOutput::parse(outputId));
  if (!realisation) {
    return notFound(res, "No such derivation output.\n");
  }
  res.set_content(realisation->toJSON().dump().c_str(), "application/json");
}

void Server::stop() { server.stop(); }

bool Server::isRunning() { return server.is_running(); }

int Server::bind() {
  if (port == 0) {
    port = server.bind_to_any_port(host.c_str());
  } else {
    server.bind_to_port(host.c_str(), port);
  }
  auto hostStr = host.find(":") == std::string::npos ? host : "[" + host + "]";
  std::cout << "Listen to " << hostStr << ":" << port << std::endl;
  return port;
}

void Server::listen() {
  auto s = &server;
  s->set_exception_handler(
      [this](const auto &req, auto &res, std::exception &e) {
        handleException(req, res, e);
      });

  s->Get("/nix-cache-info",
         [this](const auto &req, auto &res) { getNixCacheInfo(req, res); });

  s->Get(R"(^/([0-9a-z]+)\.narinfo$)",
         [this](const auto &req, auto &res) { getNarInfo(req, res); });

  s->Get(R"(^/nar/([0-9a-z]+)-([0-9a-z]+)\.nar$)",
         [this](const auto &req, auto &res) { getNar(req, res); });

  // FIXME: remove soon.
  s->Get(R"(^/nar/([0-9a-z]+)\.nar$)",
         [this](const auto &req, auto &res) { getNarDeprecated(req, res); });

  s->Get(R"(^/realisations/(.*)\.doi$)",
         [this](const auto &req, auto &res) { getRealisation(req, res); });

  s->listen_after_bind();
}
