#!/bin/sh
# Install the GXL H.264 encoder: firmware, kernel module (loads at boot),
# and the udev rule for /dev/amvenc_avc. Run as root, after build.sh.
#   sudo sh scripts/install.sh
set -e
cd "$(dirname "$0")/.."

python3 firmware/get-firmware.py

KDIR="/lib/modules/$(uname -r)"
mkdir -p "$KDIR/updates"
cp venc-h264/amvenc_avc.ko "$KDIR/updates/"
depmod -a

echo "amvenc_avc" > /etc/modules-load.d/amvenc_avc.conf
printf 'KERNEL=="amvenc_avc", GROUP="video", MODE="0660"\n' \
    > /etc/udev/rules.d/99-amvenc.rules
udevadm control --reload-rules 2>/dev/null || true

sync
echo "installed. Load now with: modprobe amvenc_avc"
echo "After a reboot the encoder is /dev/video0 (meson-venc-h264) and"
echo "the decoder is /dev/video1 (meson-video-decoder) on this box;"
echo "ffmpeg finds each by capability, not by number."
