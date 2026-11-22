#!/usr/bin/env bash
# Connect to Hisense Smart TV via Miracast (Ultra-Low Latency 60 FPS VA-API, Video Only)

# 1. Elevate process priority to -15 (real-time video priority)
sudo renice -n -15 $$ >/dev/null 2>&1 || true

# 2. Teardown any stale sessions or port bindings
fuser -k 7236/tcp 2>/dev/null || true
nmcli con down "FluxCast WFD" 2>/dev/null || true
nmcli con delete "FluxCast WFD" 2>/dev/null || true
nmcli con delete "Wi-Fi P2P Peer 42:CD:7A:36:FB:F7" 2>/dev/null || true

# 3. Disable Wi-Fi power saving immediately (drops radio latency to 1ms)
sudo iw dev wlp1s0 set power_save off 2>/dev/null || true

# 4. Target the extended display HDMI-A-1 if present, otherwise eDP-1
MONITOR="eDP-1"
if niri msg -j outputs 2>/dev/null | grep -q "HDMI-A-1"; then
    MONITOR="HDMI-A-1"
fi

echo "Streaming monitor: $MONITOR to Hisense TV (60 FPS, Video-Only, Hardware VA-API, Priority -15)..."

exec /home/wojtek/.local/bin/fluxcast \
    --protocol wfd \
    --wfd-peer "42:CD:7A:36:FB:F7" \
    --monitor "$MONITOR" \
    --fps 60 \
    --wfd-no-audio \
    --wfd-capture-backend wf-recorder \
    "$@"
