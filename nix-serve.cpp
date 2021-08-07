#include <stdio.h>

#include <nix/config.h>
#include <nix/shared.hh>
#include <nix/globals.hh>
#include <nix/store-api.hh>
#include <nix/path.hh>
#include <nix/archive.hh>
#include <nix/common-args.hh>

#include <nlohmann/json.hpp>
#include <httplib.h>

std::string stripTrailingSlash(std::string s) {
    if (!s.empty() && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

void notFound(httplib::Response& res, std::string msg) {
    res.status = 404;
    res.set_content(msg, "text/plain");
}


struct MyArgs : nix::MixCommonArgs
{
    uint16_t port = 5000;
    std::string host = "::";

    MyArgs() : nix::MixCommonArgs("nix-serve")
    {
        addFlag({
            .longName = "help",
            .description = "show usage information",
            .handler = {[&]() {
                printf("USAGE: nix-serve [options] expr\n\n");
                for (const auto & [name, flag] : longFlags) {
                    if (hiddenCategories.count(flag->category)) {
                        continue;
                    }
                    printf("  --%-20s %s\n", name.c_str(), flag->description.c_str());
                }
                std::exit(0);
            }},
        });

        addFlag({
            .longName = "listen",
            .description = "Host:port to listen to",
            .labels = {"listen"},
            .handler = {[=](std::string s) {
                auto portStart = s.rfind(":");
                if (portStart == std::string::npos) {
                    throw nix::UsageError("Invalid listen address. Expected: host:port");
                }
                int p = std::stoi(s.substr(portStart + 1));
                if (p > static_cast<int>(UINT16_MAX) || p <= 0) {
                    throw nix::UsageError("Invalid port number");
                }
                port = p;
                auto start = s[0] == '[' ? 1 : 0;
                auto end = s[portStart - 1] == ']' ? portStart - 2 : portStart - 1;
                host = s.substr(start, end);
            }}
        });
    }
};

static MyArgs myArgs;

struct HttpSink : nix::Sink {
    httplib::DataSink &inner;

    HttpSink(httplib::DataSink &sink)
        : inner(sink) { }

    void operator () (std::string_view data) override {
        inner.write(data.data(), data.size());
    }
};

void serve(nix::ref<nix::Store> store, std::optional<nix::SecretKey> secretKey) {
    httplib::Server svr;
    svr.set_exception_handler([](const auto& req, auto& res, std::exception &e) {
        res.status = 500;
        auto fmt = "Error 500\n%s";
        char buf[BUFSIZ];
        snprintf(buf, sizeof(buf), fmt, e.what());
        res.set_content(buf, "text/plain");
    });

    svr.Get("/nix-cache-info", [store](const httplib::Request& req, httplib::Response& res) {
        auto resp("StoreDir: " + nix::settings.nixStore + "\nWantMassQuery: 1\nPriority: 30\n");
        res.set_content(resp, "text/plain");
    });
    svr.Get(R"(^/([0-9a-z]+)\.narinfo$)", [store, secretKey](const httplib::Request& req, httplib::Response& res) {
        auto hashPart = req.matches[1];
        auto storePath = store->queryPathFromHashPart(hashPart);
        if (!storePath) {
            return notFound(res, "No such path.\n");
        }
        auto info = store->queryPathInfo(*storePath);
        auto narHash = info->narHash.to_string(nix::Base32, false);
        auto resp = "StorePath: " + store->printStorePath(*storePath) + "\n" +
            "URL: nar/" + hashPart.str() + "-" + narHash + ".nar\n" +
            "Compression: none\n" +
            "NarHash: " + narHash + "\n" +
            "NarSize: " + std::to_string(info->narSize) + "\n";
        if (!info->references.empty()) {
            resp += "References:";
            for (auto & i : info->references) {
                resp += " " + stripTrailingSlash(store->printStorePath(i));
            }
            resp += "\n";
        }
        if (info->deriver) {
            resp += "Deriver: " + stripTrailingSlash(store->printStorePath(*info->deriver)) + "\n";
        }

        if (secretKey) {
            resp += "Sig: " + secretKey->signDetached(info->fingerprint(*store)) + "\n";
        }
        res.set_content(resp, "text/plain");
    });
    svr.Get(R"(^/nar/([0-9a-z]+)-([0-9a-z]+)\.nar$)", [store](const httplib::Request& req, httplib::Response& res) {
        auto hashPart = req.matches[1];
        auto expectedNarHash = nix::Hash::parseAny(req.matches[2].str(), nix::htSHA256);
        auto storePath = store->queryPathFromHashPart(hashPart);
        if (!storePath) {
            return notFound(res, "No such path.\n");
        }
        auto info = store->queryPathInfo(*storePath);
        if (info->narHash != expectedNarHash) {
            return notFound(res, "Incorrect NAR hash. Maybe the path has been recreated.\n");
        }
        res.set_chunked_content_provider("text/plain", [store, storePath](size_t offset, httplib::DataSink &sink) {
            HttpSink s(sink);
            try {
                nix::dumpPath(store->printStorePath(*storePath), s);
            } catch (nix::Error & e) {
                std::cerr << e.what() << std::endl;
                sink.done();
                return false;
            }
            sink.done();
            return true;
        });
    });

    svr.Get(R"(^/realisations/(.*)\.doi$)", [store](const httplib::Request& req, httplib::Response& res) {
        auto outputId = req.matches[1];
        auto realisation = store->queryRealisation(nix::DrvOutput::parse(outputId));
        if (!realisation) {
            return notFound(res, "No such derivation output.\n");
        }
        res.set_content(realisation->toJSON().dump().c_str(), "application/json");
    });

    // FIXME: remove soon.
    svr.Get(R"(^/nar/([0-9a-z]+)\.nar$)", [store](const httplib::Request& req, httplib::Response& res) {
        auto hashPart = req.matches[1];
        auto storePath = store->queryPathFromHashPart(hashPart);
        if (!storePath) {
            return notFound(res, "No such path.\n");
        }
        auto info = store->queryPathInfo(*storePath);
        res.set_chunked_content_provider("text/plain", [store, storePath](size_t offset, httplib::DataSink &sink) {
            HttpSink s(sink);
            try {
                nix::dumpPath(store->printStorePath(*storePath), s);
            } catch (nix::Error & e) {
                std::cerr << e.what() << std::endl;
                sink.done();
                return false;
            }
            sink.done();
            return true;
        });
    });

    auto host = myArgs.host.find(":") == std::string::npos ? myArgs.host : "[" + myArgs.host + "]";
    std::cout << "Listen to " << host << ":" << myArgs.port << std::endl;
    svr.listen(myArgs.host.c_str(), myArgs.port);
}

int main(int argc, char *argv[]) {
    if (!getenv("NIX_REMOTE")) {
        nix::settings.storeUri = "auto?path-info-cache-size=0";
    }

    return nix::handleExceptions(argv[0], [&]() {
        nix::loadConfFile();
        nix::settings.lockCPU = false;
        auto store = nix::openStore();

        myArgs.parseCmdline(nix::argvToStrings(argc, argv));

        char* secretFile = getenv("NIX_SECRET_KEY_FILE");
        std::optional<nix::SecretKey> secretKey = {};
        if (secretFile) {
            try {
                secretKey = nix::SecretKey(nix::readFile(secretFile));
            } catch (nix::Error & e) {
                std::cerr << "Could not read " << secretFile << ": " << e.what() << std::endl;
                std::exit(1);
            }
        }

        serve(store, secretKey);
    });
}
