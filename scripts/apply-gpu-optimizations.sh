#!/usr/bin/env bash
set -euo pipefail

echo "==> 1. Setting runtime GPU clock floor (800 MHz) and CPU performance preference..."
echo 800 > /sys/class/drm/card0/gt_min_freq_mhz
for f in /sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference; do
    echo performance > "$f"
done

echo "==> 2. Persisting 800 MHz GPU floor across reboots via systemd-tmpfiles..."
mkdir -p /etc/tmpfiles.d
cat << 'EOF' > /etc/tmpfiles.d/gpu-performance.conf
w /sys/class/drm/card0/gt_min_freq_mhz - - - - 800
w /sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference - - - - performance
EOF

echo "==> 3. Configuring kernel parameters in GRUB..."
rm -f /etc/default/grub.d/virtual-hdmi.cfg
cat << 'EOF' > /etc/default/grub.d/gpu-performance.cfg
GRUB_CMDLINE_LINUX_DEFAULT="quiet resume=/dev/mapper/dell--vg-swap_1 mitigations=off nmi_watchdog=0 nowatchdog pcie_aspm.policy=performance i915.enable_psr=0 i915.enable_fbc=1 i915.enable_dc=2 i915.enable_guc=3 video=HDMI-A-1:1920x1080@60e"
EOF

echo "==> 4. Updating GRUB..."
update-grub

echo "==> Done! Optimizations applied immediately and persisted for next boot."
