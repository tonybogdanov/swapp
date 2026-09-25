#!/usr/bin/env bash
# Kills any running swapp, rebuilds it, and starts the new binary.
# No sudo: system packages are z_install-deps.sh's job, the build needs none.
set -euo pipefail

CONFIG="Release"
BUILD_DIR="build-linux"

usage() {
    echo "Usage: $0 [--debug|--release] [--build-dir DIR]"
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --debug) CONFIG="Debug"; shift ;;
        --release) CONFIG="Release"; shift ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) echo "Unknown option: $1" >&2; usage ;;
    esac
done

cd "$(dirname "${BASH_SOURCE[0]}")"

# The old binary has to go before linking: the linker writes the executable
# in place, and an image still mapped by a running process refuses that.
if pkill -x swapp 2>/dev/null; then
    echo "Stopped the running swapp"
    # pkill returns as soon as the signal is sent, not once the process is
    # gone; give it a moment to actually exit and unmap the binary.
    sleep 1
fi

GENERATOR="Unix Makefiles"
command -v ninja >/dev/null 2>&1 && GENERATOR="Ninja"

cmake -S . -B "$BUILD_DIR" -G "$GENERATOR" -DCMAKE_BUILD_TYPE="$CONFIG"
cmake --build "$BUILD_DIR" --config "$CONFIG"

echo "Build succeeded: $BUILD_DIR/swapp"

# setsid so the app outlives this shell and its terminal.
setsid nohup "./$BUILD_DIR/swapp" >/dev/null 2>&1 < /dev/null &
echo "Started $BUILD_DIR/swapp"
