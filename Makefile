CC ?= gcc
CFLAGS ?= -O3 -march=native -fomit-frame-pointer -flto -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE
PKG_CFLAGS := $(shell pkg-config --cflags libavcodec libavformat libavutil wayland-client libdrm gbm libva libva-drm)
PKG_LIBS   := $(shell pkg-config --libs libavcodec libavformat libavutil wayland-client libdrm gbm libva libva-drm) -lpthread -lm

SRC = src/wfd_fast.c src/realtime_sys.c src/video_enc.c src/net_pacer.c src/dmabuf_capture.c src/color_convert.c src/damage_tracker.c src/wlr-screencopy-unstable-v1-protocol.c src/linux-dmabuf-unstable-v1-protocol.c
BIN = bin/wfd-fast
BENCH_BIN = bin/bench_video_enc
BENCH_KERNEL = bin/bench_kernel_sys
BENCH_PACER = bin/bench_net_pacer
TOOL_BINS = bin/test_dmabuf_screencopy bin/test_capture_speed bin/test_fast_nv12 bin/test_damage_tracker bin/test_roi_enc bin/query_va_caps bin/bench_video_enc_suite
PREFIX ?= $(HOME)/.local

.PHONY: all clean install tools test bench_net_pacer

all: $(BIN) $(BENCH_BIN) $(BENCH_KERNEL) $(BENCH_PACER)

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

bin/test_dmabuf_screencopy: tools/test_dmabuf_screencopy.c src/dmabuf_capture.c src/dmabuf_capture.h src/damage_tracker.c src/damage_tracker.h src/video_enc.c src/video_enc.h src/color_convert.c src/color_convert.h src/wlr-screencopy-unstable-v1-protocol.c src/linux-dmabuf-unstable-v1-protocol.c
	@mkdir -p bin
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -Isrc tools/test_dmabuf_screencopy.c src/dmabuf_capture.c src/damage_tracker.c src/video_enc.c src/color_convert.c src/wlr-screencopy-unstable-v1-protocol.c src/linux-dmabuf-unstable-v1-protocol.c -o $@ $(PKG_LIBS)

bin/test_capture_speed: tools/test_capture_speed.c src/wlr-screencopy-unstable-v1-protocol.c
	@mkdir -p bin
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -Isrc $^ -o $@ -lwayland-client

bin/test_fast_nv12: tools/test_fast_nv12.c src/color_convert.c src/color_convert.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -Isrc tools/test_fast_nv12.c src/color_convert.c -o $@

bin/test_damage_tracker: tools/test_damage_tracker.c src/damage_tracker.c src/damage_tracker.h
	@mkdir -p bin
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -Isrc tools/test_damage_tracker.c src/damage_tracker.c -o $@

bin/test_roi_enc: tools/test_roi_enc.c src/video_enc.c src/video_enc.h
	@mkdir -p bin
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -Isrc tools/test_roi_enc.c src/video_enc.c -o $@ $(PKG_LIBS)

bin/query_va_caps: tools/query_va_caps.c
	@mkdir -p bin
	$(CC) $(CFLAGS) $^ -o $@ -lva -lva-drm

bin/bench_video_enc_suite: tools/bench_video_enc_suite.c
	@mkdir -p bin
	$(CC) $(CFLAGS) $(PKG_CFLAGS) $^ -o $@ $(PKG_LIBS)

tools: $(BENCH_BIN) $(BENCH_KERNEL) $(BENCH_PACER) $(TOOL_BINS)
	@echo "Tools built in bin/"

test: bin/test_fast_nv12 bin/test_damage_tracker bin/test_roi_enc
	./bin/test_fast_nv12
	./bin/test_damage_tracker
	./bin/test_roi_enc

install: $(BIN)
	@mkdir -p $(PREFIX)/bin
	install -m 755 $(BIN) $(PREFIX)/bin/wfd-fast
	@echo "Installed to $(PREFIX)/bin/wfd-fast"

clean:
	rm -rf bin
