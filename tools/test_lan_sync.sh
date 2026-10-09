#!/bin/bash
# Build the LAN sync test standalone (does not require the full game build).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"

mkdir -p "$BUILD_DIR"

ENET_CFLAGS=$(pkg-config --cflags libenet 2>/dev/null || echo "")
ENET_LIBS=$(pkg-config --libs libenet 2>/dev/null || echo "-lenet")

echo "Building test_lan_sync..."
clang++ -std=c++20 -O2 \
    -I"$PROJECT_ROOT/include" \
    $ENET_CFLAGS \
    "$PROJECT_ROOT/src/network/lan_session.cpp" \
    "$SCRIPT_DIR/test_lan_sync.cpp" \
    $ENET_LIBS \
    -lstdc++ \
    -o "$BUILD_DIR/test_lan_sync"

echo "Build successful: $BUILD_DIR/test_lan_sync"
