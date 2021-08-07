#include <nix/config.h>
#include <nix/globals.hh>
#include <nix/shared.hh>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <thread>

#include "../src/server.hh"

class NixEnvironment : public ::testing::Environment {
public:
  ~NixEnvironment() override {}

  void SetUp() override {
    nix::loadConfFile();
    nix::settings.lockCPU = false;
  }

  // Override this to define how to tear down the environment.
  void TearDown() override {}
};
testing::Environment *const nixEnv =
    testing::AddGlobalTestEnvironment(new NixEnvironment);

class ServerFixture : public ::testing::Test {
private:
  void run() { server.listen(); }

public:
  uint16_t port;
  Server server = Server("::1", 0, {});
  std::optional<std::thread> listenThread = {};
  std::unique_ptr<httplib::Client> client = {};

protected:
  void SetUp() override {
    port = server.bind();
    listenThread = std::thread{&ServerFixture::run, this};
    client = std::make_unique<httplib::Client>("::1", port);
    while (!server.isRunning()) {
      usleep(5);
    }
  }
  void TearDown() override {
    server.stop();
    listenThread->join();
  }
};

TEST_F(ServerFixture, getNixCacheInfo) {
  auto res = client->Get("/nix-cache-info");
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(res->body,
            "StoreDir: /nix/store\nWantMassQuery: 1\nPriority: 30\n");
}

std::map<std::string, std::string> parseInfo(const std::string &s) {
  std::map<std::string, std::string> result;
  std::stringstream ss(s);
  std::string item;

  while (std::getline(ss, item, '\n')) {
    auto end = item.find(": ");
    if (end == std::string::npos) {
      throw std::runtime_error("invalid line in output: " + item);
    }
    auto key = item.substr(0, end);
    auto value = item.substr(end + 2, s.size() - end);
    result[key] = value;
  }

  return result;
}

TEST_F(ServerFixture, getNarInfo) {
  auto fp = popen("nix build --json --inputs-from .# nixpkgs#hello", "r");
  EXPECT_NE(fp, nullptr);

  char line[255];
  std::string buf;
  while (fgets(line, sizeof(255), fp)) {
    buf.append(line);
  }
  fclose(fp);
  auto storePath =
      nlohmann::json::parse(buf)[0]["outputs"]["out"].get<std::string>();

  auto res = client->Get(("/" + storePath.substr(11, 32) + ".narinfo").c_str());
  EXPECT_EQ(res->status, 200);

  auto info = parseInfo(res->body);
  EXPECT_EQ(info["Compression"], "none");
  EXPECT_NE(stoi(info["NarSize"]), 0);
  EXPECT_EQ(info.count("NarHash"), 1);
  EXPECT_EQ(info.count("References"), 1);
  EXPECT_EQ(info.count("Deriver"), 1);
  EXPECT_EQ(info.count("StorePath"), 1);

  auto res2 = client->Get(("/" + info["URL"]).c_str());
  EXPECT_EQ(res2->status, 200);

  auto tmpName = std::make_unique<char *>(strdup("tmp.XXXXXX"));
  EXPECT_NE(tmpName, nullptr);
  int fd = mkstemp(*tmpName);
  EXPECT_NE(fd, -1);
  close(fd);

  auto cmd = "nix nar ls /dev/stdin /bin/hello > " + std::string(*tmpName);
  auto fp2 = popen(cmd.c_str(), "w");
  EXPECT_NE(fp2, nullptr);
  fwrite(res2->body.c_str(), sizeof(char), res2->body.size(), fp2);
  fclose(fp2);

  std::ifstream t(*tmpName);
  std::string out((std::istreambuf_iterator<char>(t)),
                  std::istreambuf_iterator<char>());
  unlink(*tmpName);
  EXPECT_EQ(out, "hello\n");
}
