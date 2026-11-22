# FluxCast-C

## Introduction

FluxCast-C is a native C Miracast sender for Linux Wayland. It captures a compositor output, encodes H.264 video with VA-API, and transmits RTP/MPEG-TS over Wi-Fi Direct to a compatible display.

The project is intended for desktop content such as terminals, text editors, and source code. Capture, color conversion, encoding, and Miracast transport are integrated in one process. Color range, frame timestamps, and packet scheduling are handled explicitly rather than left to a chain of unrelated tools.

## Requirements

- Linux with a Wayland compositor supporting `zwlr_screencopy_manager_v1`.
- A VA-API device with H.264 encoding support. The current defaults target Intel VDENC.
- FFmpeg development libraries: `libavcodec`, `libavformat`, and `libavutil`.
- Development files for Wayland, libdrm, GBM, and VA-API.
- A C compiler, Make, pkg-config, and OpenMP support.

The `linux-dmabuf-v1` protocol is optional. FluxCast-C falls back to shared-memory capture when DMA-BUF capture is unavailable or disabled.

## Building

```sh
git clone https://github.com/wklm/fluxcast-c.git
cd fluxcast-c
make
```

Install the `wfd-fast` executable under `~/.local/bin`:

```sh
make install
```

Set `PREFIX` to select a different installation prefix.

## Running

Start the RTSP server and wait for a Miracast sink to connect:

```sh
wfd-fast \
  --tv-ip 192.168.179.1 --tv-port 1010 \
  --local-ip 192.168.179.200 --local-port 19002
```

These addresses are examples. Use the addresses assigned to the Wi-Fi Direct link. The RTSP service listens on TCP port 7236.

Options:

- `--stream-only`: use an existing negotiated session without starting the RTSP server.
- `--no-dmabuf`: use shared-memory capture and the AVX2 conversion path.

The supplied `scripts/fluxcast-tv.sh` integrates with the FluxCast launcher. Review its peer and output settings before use; they are specific to the current test setup.

## Implementation

```text
Wayland screencopy
        |
        v
DMA-BUF + VA-API VPP, or SHM + AVX2
        |
        v
VA-API H.264 encoder
        |
        v
RTP/MPEG-TS packet pacer
        |
        v
Miracast display
```

- DMA-BUF capture imports compositor buffers into VA-API without a host copy.
- Both conversion paths produce limited-range BT.709 NV12 and declare the color properties in the H.264 VUI.
- The encoder uses High Profile, CABAC, CQP, and a 30-frame GOP. The current default QP is 18.
- Compositor damage is aligned and merged into up to three regions of interest with a QP delta of -3. Damage covering more than 25% of the frame uses the global QP.
- Presentation timestamps use `CLOCK_MONOTONIC` when a captured frame becomes ready.
- RTP packets are scheduled against monotonic deadlines and sent from the negotiated source port.

## Validation

Build the diagnostic programs:

```sh
make tools
```

Run the focused regression and hardware tests:

```sh
./bin/test_fast_nv12
./bin/test_damage_tracker
./bin/test_roi_enc
```

Additional capture, encoder, and network diagnostic programs are built under `bin/`.

## Limitations

- Video only; audio is not transmitted.
- The streaming frontend currently targets 1920x1080 at 60 frames per second.
- X11 is not supported.
- Validation has been performed with Intel Kaby Lake graphics and a Hisense VIDAA display. Other encoder and sink combinations require separate validation.
