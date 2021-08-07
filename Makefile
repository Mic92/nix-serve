PREFIX ?= /usr/local
EXTRA_CXXFLAGS ?= -O3 -g
EXTRA_LDLAGS ?=

CXXFLAGS = $(shell pkg-config --cflags nix-store nix-main) $(EXTRA_CXXFLAGS)
LDFLAGS = -pthread -lhttplib $(shell pkg-config --libs nix-store nix-main) $(EXTRA_LDFLAGS)

TEST_CXXFLAGS ?= $(shell pkg-config --cflags --libs gtest) $(CXXFLAGS) $(LDFLAGS)

all: nix-serve

%.o: %.cpp
	$(CXX) -c -o $@ $^ $(CXXFLAGS)

%.o: %.cpp
	$(CXX) -c -o $@ $^ $(CXXFLAGS)

nix-serve: src/server.o src/main.o
	$(CXX) -o $@ $^ $(LDFLAGS) $(EXTRA_CXXFLAGS)

install: all
	install -m755 -D nix-serve $(PREFIX)/bin/nix-serve

test/test_server: test/test_server.cpp src/server.o
	$(CXX) -o $@ -lgtest_main $^ $(TEST_CXXFLAGS)

check: test/test_server
	test/test_server

clean:
	rm -rf src/*.o nix-serve
