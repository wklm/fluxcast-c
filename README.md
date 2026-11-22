# wfd-fast: Wayland Miracast Streamer

A native C streaming engine for Linux Wayland compositors (Niri, Sway, Hyprland, wlroots) connecting to Smart TVs (Hisense VIDAA OS, Samsung Tizen, LG webOS) via Wi-Fi Display (Miracast / Anyview Cast).

---

## 1. Implementation Details

### A. Clock Drift Prevention via `CLOCK_MONOTONIC` PTS
* **The Problem:** Standard pipelines use frame counters (`PTS_n = PTS_{n-1} + 1/60`) or `genpts`. During static desktop moments or slight timing delays, incremental counters drift away from real wall-clock time. The TV's MPEG-TS PCR recovery PLL interprets this as late media and expands its internal jitter buffer, causing latency to grow linearly with session duration.
* **The Fix:** `wfd-fast` locks PTS to `CLOCK_MONOTONIC` microseconds when each damaged frame becomes ready:
  $$\text{PTS} = \frac{(t_{\text{now}} - t_{\text{start}})_{\mu s} \times 90}{1000}$$
  Every packet carries an absolute wall-clock timestamp, strictly monotonic, preventing buffer accumulation and timing drift.

### B. Hardware Encoding (VA-API / Intel QuickSync)
* **Fixed-Function Encoding:** Configured via `h264_vaapi` with `low_power=1`, High Profile (`profile=100`), and CABAC entropy coding.
* **Rate Control:** Uses Constant Quantization Parameter (`rc_mode=CQP`, `qp=22`), avoiding multi-frame rate control lookahead buffers.
* **GOP Synchronization:** Emits an IDR keyframe with in-band SPS/PPS every 30 frames (500 ms at 60 FPS), enabling recovery from packet drops without full pipeline teardowns.
* **Color Signaling:** Encodes BT.709 limited-range video and declares the matrix, primaries, transfer characteristics, and range in the H.264 SPS VUI.

### C. Wayland Screencopy & Color Conversion
* Integrates directly with `zwlr_screencopy_manager_v1` using zero-copy DMA-BUF capture and Intel VA-API VPP where supported.
* Uses `copy_with_damage` to capture changed frames and merges damage into up to three macroblock-aligned ROI regions with a QP delta of -3. Broad damage exceeding 25% of the frame uses the global QP to avoid an across-the-frame bitrate increase.
* Falls back to shared-memory capture and an AVX2 BT.709 limited-range converter with 2x2 chroma averaging.

### D. RTSP Contract & Source Port Binding
* Miracast sinks enforce strict incoming socket filters based on the negotiated RTSP `SETUP` transport header (`server_port=19002-19003`).
* Uses `avio_open2` with explicit `AVDictionary` bindings (`local_rtpport=19002`, `local_rtcpport=19003`) to guarantee that all RTP MPEG-TS packets originate from the exact contracted port.

---

## 2. System & Network Tuning

The following system-level adjustments are recommended:

1. **Eliminate Multi-Channel Concurrency (MCC) Radio Hopping:**
   * If the laptop's Wi-Fi is connected to a 5 GHz home network while P2P Wi-Fi Direct runs on 2.4 GHz, the single Wi-Fi radio synthesizer must hop frequencies, introducing periodic jitter spikes.
   * Fix: Force both home Wi-Fi and P2P onto the 2.4 GHz band:
     ```bash
     nmcli con modify <Home-WiFi-Name> 802-11-wireless.band bg
     ```
2. **Disable Wi-Fi Power Saving:**
   * Wireless power management sleeps the radio transceiver between beacons, introducing latency to packet bursts:
     ```bash
     sudo iw dev wlp1s0 set power_save off
     ```
3. **Set GPU Minimum Frequency Floor:**
   * Prevent Intel dynamic frequency scaling down-clocking during static frames:
     ```bash
     echo 800 | sudo tee /sys/class/drm/card0/gt_min_freq_mhz
     ```
4. **Set TV Picture Mode to "Game" or "PC":**
   * Standard TV picture modes apply Motion Estimation / Motion Compensation (MEMC) and noise filtering, adding buffering delays in TV panel firmware. Game Mode bypasses these buffers.

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

## 4. Diagnostics & Testing

Build the diagnostic test suite:
```bash
make tools
```

* `bin/test_capture_speed`: Tests Wayland screencopy capture roundtrip latency.
* `bin/test_fast_nv12`: Validates BT.709 conversion against a scalar reference and benchmarks the AVX2 path.
* `bin/test_damage_tracker`: Tests damage clipping, merging, alignment, and broad-damage suppression.
* `bin/test_roi_enc`: Checks selective hardware ROI encoding and measures A/B encode latency.
* `bin/inspect_pkt3`: Analyzes H.264 NAL units and MPEG-TS access units.
