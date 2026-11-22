CC ?= gcc
CFLAGS ?= -O3 -march=native -fopenmp -fomit-frame-pointer -flto -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE
PKG_CFLAGS := $(shell pkg-config --cflags libavcodec libavformat libavutil wayland-client libdrm gbm libva libva-drm)
PKG_LIBS   := $(shell pkg-config --libs libavcodec libavformat libavutil wayland-client libdrm gbm libva libva-drm) -lpthread -lm

SRC = src/wfd_fast.c src/realtime_sys.c src/video_enc.c src/net_pacer.c src/dmabuf_capture.c src/wlr-screencopy-unstable-v1-protocol.c src/linux-dmabuf-unstable-v1-protocol.c
BIN = bin/wfd-fast
BENCH_BIN = bin/bench_video_enc
BENCH_KERNEL = bin/bench_kernel_sys
BENCH_PACER = bin/bench_net_pacer
PREFIX ?= $(HOME)/.local

.PHONY: all clean install tools bench_net_pacer

all: $(BIN) $(BENCH_KERNEL) $(BENCH_PACER)

$(BIN): $(SRC)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -Isrc $^ -o $@ $(PKG_LIBS)
	@echo "Built: $@"

$(BENCH_BIN): tools/bench_video_enc.c src/video_enc.c src/video_enc.h
	@mkdir -p bin
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -Isrc tools/bench_video_enc.c src/video_enc.c -o $@ $(PKG_LIBS)
	@echo "Built: $@"

$(BENCH_KERNEL): tools/bench_kernel_sys.c src/realtime_sys.c src/realtime_sys.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -Isrc tools/bench_kernel_sys.c src/realtime_sys.c -o $@ -lpthread -lm
	@echo "Built: $@"

$(BENCH_PACER): tools/bench_net_pacer.c src/net_pacer.c src/net_pacer.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -Isrc tools/bench_net_pacer.c src/net_pacer.c -o $@ -lpthread -lm
	@echo "Built: $@"

bench_net_pacer: $(BENCH_PACER)

install: $(BIN)
	@mkdir -p $(PREFIX)/bin
	install -m 755 $(BIN) $(PREFIX)/bin/wfd-fast
	@echo "Installed to $(PREFIX)/bin/wfd-fast"

tools: $(BENCH_BIN) $(BENCH_KERNEL) $(BENCH_PACER)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -Isrc tools/test_dmabuf_screencopy.c src/video_enc.c src/wlr-screencopy-unstable-v1-protocol.c src/linux-dmabuf-unstable-v1-protocol.c -o bin/test_dmabuf_screencopy $(PKG_LIBS)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -Isrc tools/test_capture_speed.c src/wlr-screencopy-unstable-v1-protocol.c -o bin/test_capture_speed $(PKG_LIBS)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) tools/test_fast_nv12.c -o bin/test_fast_nv12 $(PKG_LIBS)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) tools/inspect_pkt3.c -o bin/inspect_pkt3 $(PKG_LIBS)
	$(CC) $(CFLAGS) tools/query_va_caps.c -o bin/query_va_caps -lva -lva-drm
	@echo "Tools built in bin/"

clean:
	rm -rf bin
