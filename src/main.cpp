#include <stdio.h>

#include <nix/common-args.hh>
#include <nix/config.h>
#include <nix/globals.hh>
#include <nix/shared.hh>

#include "server.hh"

struct MyArgs : nix::MixCommonArgs {
  uint16_t port = 5000;
  std::string host = "::";

  MyArgs() : nix::MixCommonArgs("nix-serve") {
    addFlag({
        .longName = "help",
        .description = "show usage information",
        .handler = {[&]() {
          printf("USAGE: nix-serve [options] expr\n\n");
          for (const auto &[name, flag] : longFlags) {
            if (hiddenCategories.count(flag->category)) {
              continue;
            }
            printf("  --%-20s %s\n", name.c_str(), flag->description.c_str());
          }
          std::exit(0);
        }},
    });

    addFlag({.longName = "listen",
             .description = "Host:port to listen to",
             .labels = {"listen"},
             .handler = {[=](std::string s) {
               auto portStart = s.rfind(":");
               if (portStart == std::string::npos) {
                 throw nix::UsageError(
                     "Invalid listen address. Expected: host:port");
               }
               int p = std::stoi(s.substr(portStart + 1));
               if (p > static_cast<int>(UINT16_MAX) || p < 0) {
                 throw nix::UsageError("Invalid port number");
               }
               port = p;
               auto start = s[0] == '[' ? 1 : 0;
               auto end =
                   s[portStart - 1] == ']' ? portStart - 2 : portStart - 1;
               host = s.substr(start, end);
             }}});
  }
};

int main(int argc, char *argv[]) {
  if (!getenv("NIX_REMOTE")) {
    nix::settings.storeUri = "auto?path-info-cache-size=0";
  }
  char *secretFile = getenv("NIX_SECRET_KEY_FILE");

  return nix::handleExceptions(argv[0], [&]() {
    nix::loadConfFile();
    nix::settings.lockCPU = false;

    MyArgs myArgs;
    myArgs.parseCmdline(nix::argvToStrings(argc, argv));

    std::optional<nix::SecretKey> secretKey = {};
    if (secretFile) {
      try {
        secretKey = nix::SecretKey(nix::readFile(secretFile));
      } catch (nix::Error &e) {
        e.addTrace({}, "while reading %1%", secretFile);
        throw;
      }
    }

    Server server(myArgs.host, myArgs.port, secretKey);
    server.bind();
    server.listen();
  });
}
