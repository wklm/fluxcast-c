# wfd-fast: Ultra-Low-Latency Wayland Miracast Streamer

A zero-abstraction, high-frequency, native C streaming engine engineered to reach the theoretical lower bound of wireless display latency (~41 ms) on Linux Wayland compositors (Niri, Sway, Hyprland, wlroots) connecting to Smart TVs (Hisense VIDAA OS, Samsung Tizen, LG webOS) via native Wi-Fi Display (Miracast / Anyview Cast).

---

## 1. Key Engineering Innovations

### A. Zero Clock Drift via Real-Time `CLOCK_MONOTONIC` PTS
* **The Problem:** Standard pipelines use frame counters (`PTS_n = PTS_{n-1} + 1/60`) or `genpts`. During static desktop moments or slight timing delays, incremental counters drift away from real wall-clock time. The TV's MPEG-TS PCR recovery PLL interprets this as late media and expands its internal jitter buffer, causing latency to grow linearly with session duration.
* **The Fix:** `wfd-fast` locks PTS to `CLOCK_MONOTONIC` microseconds at the exact moment of timer expiration:
  $$\text{PTS} = \frac{(t_{\text{now}} - t_{\text{start}})_{\mu s} \times 90}{1000}$$
  Every packet carries an absolute wall-clock timestamp, strictly monotonic, permanently preventing buffer accumulation and ensuring latency remains flat indefinitely.

### B. Fixed-Function Intel QuickSync VDENC ASIC Hardware Acceleration
* **Direct Fixed-Function Path:** Configured via `h264_vaapi` with `low_power=1`, bypassing GPU Execution Units (EU) shaders entirely and utilizing Intel's dedicated low-power silicon encoding engine.
* **Low-Complexity Entropy Coding:** Uses CAVLC (`coder=cavlc`) and Constrained Baseline Profile (`profile=578`) rather than CABAC, cutting TV decoder compute cycles by over 60%.
* **Zero Rate-Control Delay:** Utilizes Constant Quantization Parameter (`rc_mode=CQP`, `qp=28`), eliminating multi-frame rate control lookahead buffers.
* **Micro-GOP Sync:** Emits an IDR keyframe with in-band SPS/PPS every 15 frames (250 ms at 60 FPS), enabling instantaneous recovery from packet drops without full pipeline teardowns.

### C. Direct Wayland Screencopy & Multi-Threaded OpenMP Rec.601 Conversion
* Integrates directly with `zwlr_screencopy_manager_v1` via shared-memory (`memfd_create`) pools.
* Employs an OpenMP parallel Rec.601 color space converter (`xrgb_to_nv12_fast`) processing 1080p frames in ~2.2 ms across CPU cores.

### D. Exact RTSP Contract & Source Port Locking
* Miracast sinks enforce strict incoming socket filters based on the negotiated RTSP `SETUP` transport header (`server_port=19002-19003`).
* Uses `avio_open2` with explicit `AVDictionary` bindings (`local_rtpport=19002`, `local_rtcpport=19003`) to guarantee that all RTP MPEG-TS packets originate from the exact contracted port.

---

## 2. System & Network Tuning

To achieve the ~41 ms wireless floor, three system-level adjustments are required:

1. **Eliminate Multi-Channel Concurrency (MCC) Radio Hopping:**
   * If the laptop's Wi-Fi is connected to a 5 GHz home network while P2P Wi-Fi Direct runs on 2.4 GHz, the single Wi-Fi radio synthesizer must rapidly hop frequencies, introducing periodic 40–200 ms jitter spikes.
   * Fix: Force both home Wi-Fi and P2P onto the 2.4 GHz band:
     ```bash
     nmcli con modify <Home-WiFi-Name> 802-11-wireless.band bg
     ```
2. **Disable Wi-Fi Power Saving:**
   * Wireless power management sleeps the radio transceiver between beacons, adding 15–50 ms latency to every packet burst:
     ```bash
     sudo iw dev wlp1s0 set power_save off
     ```
3. **Set GPU Minimum Frequency Floor:**
   * Prevent Intel dynamic frequency scaling down-clocking during static frames:
     ```bash
     echo 800 | sudo tee /sys/class/drm/card0/gt_min_freq_mhz
     ```
4. **Set TV Picture Mode to "Game" or "PC":**
   * Standard TV picture modes apply Motion Estimation / Motion Compensation (MEMC) and noise filtering, buffering 80–140 ms inside the TV panel firmware. Game Mode disables these buffers completely.

---

## 3. Building & Running

### Prerequisites
* Debian / Ubuntu / Arch with:
  * `libavcodec-dev`, `libavformat-dev`, `libavutil-dev`, `libswscale-dev`
  * `libwayland-dev`
  * OpenMP (`libgomp`)
  * `wpa_supplicant`, `NetworkManager`

### Compilation
```bash
git clone git@github.com:wklm/wfd-fast.git
cd wfd-fast
make
make install  # Installs to ~/.local/bin/wfd-fast
```

### Running Standalone
```bash
# Stream directly to Miracast sink
wfd-fast --tv-ip 192.168.179.1 --tv-port 1010 --local-ip 192.168.179.200 --local-port 19002
```

### Running with FluxCast Launcher
```bash
scripts/fluxcast-tv.sh
```

---

## 4. Benchmarking & Diagnostics

Build the diagnostic test suite:
```bash
make tools
```

* `bin/test_capture_speed`: Measures raw Wayland screencopy capture roundtrip latency (typically 5.7 ms).
* `bin/test_fast_nv12`: Measures OpenMP SIMD color space converter speed (typically 2.2 ms for 1080p).
* `bin/inspect_pkt3`: Analyzes H.264 NAL units and MPEG-TS access units.
