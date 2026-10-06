#!/bin/sh
set -eu
APP_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$APP_ROOT/build"}
FORGE_ROOT=${FORGE_ROOT:-"$HOME/.forge/current"}
FORGE_BIN=${FORGE_BIN:-"$FORGE_ROOT/build/bin/forge"}
FORGE_LIB_DIR=${FORGE_LIB_DIR:-"$FORGE_ROOT/build/lib"}
if [ ! -x "$FORGE_BIN" ]; then
 echo "Set FORGE_BIN to the Forge compiler and FORGE_ROOT/FORGE_LIB_DIR to its headers and libraries." >&2
 exit 1
fi
mkdir -p "$BUILD_DIR"
cmake -S "$APP_ROOT/vendor-web" -B "$BUILD_DIR/web" -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR/web" -j "${BUILD_JOBS:-2}"
cmake -S "$APP_ROOT" -B "$BUILD_DIR/library" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR/library" -j "${BUILD_JOBS:-2}"
"$FORGE_BIN" "$APP_ROOT/service/main.fg" --emit-c -o "$BUILD_DIR/main.c" -I "$APP_ROOT" -I "$APP_ROOT/service" -I "$APP_ROOT/vendor-web" --forge-root "$FORGE_ROOT" --lib-dir "$FORGE_LIB_DIR"
cat "$APP_ROOT/src/http.c" >> "$BUILD_DIR/main.c"
${CC:-cc} -std=gnu11 -O2 -Wall -Wextra -Wno-unused-function -I "$FORGE_ROOT/include" "$BUILD_DIR/main.c" "$BUILD_DIR/library/libforge_storage.a" "$BUILD_DIR/web/libforge_web.a" "$FORGE_LIB_DIR/libforge_runtime.a" "$FORGE_LIB_DIR/libforge_std.a" $(pkg-config --cflags --libs libmicrohttpd json-c libcurl openssl) -lpthread -lm -o "$BUILD_DIR/forge-storage"
echo "Built $BUILD_DIR/forge-storage and $BUILD_DIR/library/libforge_storage.a"
