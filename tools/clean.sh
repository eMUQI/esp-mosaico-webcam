#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
: "${IDF_PATH:?Activate the pinned ESP-IDF environment before cleaning}"
export PATH="$IDF_PATH/tools:$PATH"
patch=vendor/esp-mosaico-bsp/components/mosaico_module_camera/patches/esp-cam-sensor-sc101iot-pclk-delay.patch
# The BSP patches a managed sensor file during configuration; restore only that known patch.
if git apply --no-index --reverse --check "$patch" 2>/dev/null; then
    git apply --no-index --reverse "$patch"
fi
idf.py fullclean
