CC ?= gcc
CFLAGS ?= -O3 -march=native -fopenmp -fomit-frame-pointer -flto -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE
PKG_CFLAGS := $(shell pkg-config --cflags libavcodec libavformat libavutil wayland-client)
PKG_LIBS   := $(shell pkg-config --libs libavcodec libavformat libavutil wayland-client) -lpthread -lm

SRC = src/wfd_fast.c src/wlr-screencopy-unstable-v1-protocol.c
BIN = bin/wfd-fast
PREFIX ?= $(HOME)/.local

.PHONY: all clean install tools

all: $(BIN)

$(BIN): $(SRC)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(PKG_CFLAGS) $^ -o $@ $(PKG_LIBS)
	@echo "Built: $@"

install: $(BIN)
	@mkdir -p $(PREFIX)/bin
	install -m 755 $(BIN) $(PREFIX)/bin/wfd-fast
	@echo "Installed to $(PREFIX)/bin/wfd-fast"

tools:
	@mkdir -p bin
	$(CC) $(CFLAGS) $(PKG_CFLAGS) tools/test_capture_speed.c src/wlr-screencopy-unstable-v1-protocol.c -o bin/test_capture_speed $(PKG_LIBS)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) tools/test_fast_nv12.c -o bin/test_fast_nv12 $(PKG_LIBS)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) tools/inspect_pkt3.c -o bin/inspect_pkt3 $(PKG_LIBS)
	@echo "Tools built in bin/"

clean:
	rm -rf bin
