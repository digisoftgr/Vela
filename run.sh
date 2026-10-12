#!/bin/bash
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

cmake --build "$BUILD_DIR" -j$(nproc)

XDG_DATA_DIRS="$BUILD_DIR/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}" \
    "$BUILD_DIR/bin/vela" "$@"
