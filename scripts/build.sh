#!/bin/sh
# Build the GXL H.264 encoder module and the test CLIs.
# Run on the target box (needs kernel headers + gcc + make).
#   sh scripts/build.sh
set -e
cd "$(dirname "$0")/../venc-h264"

make -C "/lib/modules/$(uname -r)/build" M="$PWD" modules
echo "BUILD_RC=$?"

cc -O2 -Wall -o venc venc.c
cc -O2 -Wall -o venc-stage venc-stage.c
echo "VENC_RC=$?"

ls -la amvenc_avc.ko venc venc-stage
sync
echo BUILD_DONE
