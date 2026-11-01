#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
: "${IDF_PATH:?Activate the pinned ESP-IDF environment before building}"
expected=$(cat esp-idf.commit)
actual=$(git -C "$IDF_PATH" rev-parse HEAD)
if [ "$actual" != "$expected" ]; then
    echo "ESP-IDF commit mismatch: expected $expected; found $actual" >&2
    exit 1
fi
export PATH="$IDF_PATH/tools:$PATH"
git submodule update --init
patch="$PWD/docs/patches/esp-mosaico-bsp-display.patch"
migration="$PWD/docs/patches/esp-mosaico-bsp-display-migrate-v1.patch"
if git -C vendor/esp-mosaico-bsp apply --reverse --check "$patch" >/dev/null 2>&1; then
    :
elif git -C vendor/esp-mosaico-bsp apply --check "$patch" >/dev/null 2>&1; then
    git -C vendor/esp-mosaico-bsp apply "$patch"
else
    # Migrate the known previous patch atomically; preserve unrelated BSP edits.
    git -C vendor/esp-mosaico-bsp apply --check "$migration"
    git -C vendor/esp-mosaico-bsp apply "$migration"
fi
idf.py --preview build
python tools/package.py
