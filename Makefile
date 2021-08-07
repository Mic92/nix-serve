PREFIX ?= /usr/local
EXTRA_CFLAGS ?= -O3 -g

all: nix-serve

install: all
	install -m755 -D nix-serve $(PREFIX)/bin/nix-serve

nix-serve: nix-serve.cpp
	$(CXX) $(EXTRA_CFLAGS) -o nix-serve -pthread $(shell pkg-config --cflags --libs nix-store nix-main) $<
