#!/usr/bin/env bash
# Consume capnp-janet the four ways a downstream project can, and fail if
# any of them stops working. Shipping install rules that nothing exercises
# is how a broken Config.cmake or a stale .pc reaches users.
#
#   1. CMake add_subdirectory  (the FetchContent_MakeAvailable path)
#   2. CMake find_package      (against a real staged install)
#   3. pkg-config              (for non-CMake build systems)
#   4. meson subproject        (the .wrap path)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
PREFIX="$WORK/prefix"

echo "== 1. CMake add_subdirectory (FetchContent path)"
cmake -S "$ROOT/cmake/consumer_smoke" -B "$WORK/sub" \
  -DCAPNP_JANET_SOURCE_DIR="$ROOT" -DCMAKE_BUILD_TYPE=Release >"$WORK/sub.log" 2>&1
cmake --build "$WORK/sub" >>"$WORK/sub.log" 2>&1
"$WORK/sub/smoke"

echo "== 2. CMake find_package against a staged install"
cmake -S "$ROOT" -B "$WORK/build" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" >"$WORK/build.log" 2>&1
cmake --build "$WORK/build" >>"$WORK/build.log" 2>&1
cmake --install "$WORK/build" >>"$WORK/build.log" 2>&1

mkdir -p "$WORK/fp"
cp "$ROOT/cmake/consumer_smoke/smoke.c" "$WORK/fp/"
cat >"$WORK/fp/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.16)
project(capnp_janet_find_package_smoke LANGUAGES C)
find_package(capnp_janet REQUIRED)
add_executable(smoke smoke.c)
target_link_libraries(smoke PRIVATE capnp_janet::capnp_janet)
EOF
cmake -S "$WORK/fp" -B "$WORK/fpb" -DCMAKE_PREFIX_PATH="$PREFIX" \
  -DCMAKE_BUILD_TYPE=Release >"$WORK/fp.log" 2>&1
cmake --build "$WORK/fpb" >>"$WORK/fp.log" 2>&1
"$WORK/fpb/smoke"
test -x "$PREFIX/bin/capnpc-janet"

echo "== 3. pkg-config"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/lib64/pkgconfig"
pkg-config --exists capnp-janet
echo "   version $(pkg-config --modversion capnp-janet)"
# shellcheck disable=SC2046
cc -o "$WORK/pc-smoke" "$ROOT/cmake/consumer_smoke/smoke.c" \
  $(pkg-config --cflags --libs capnp-janet)
"$WORK/pc-smoke"

echo "== 4. meson subproject (.wrap path)"
mkdir -p "$WORK/mw/subprojects"
# A local wrap; the published one points at the git tag instead.
ln -s "$ROOT" "$WORK/mw/subprojects/capnp-janet"
cp "$ROOT/cmake/consumer_smoke/smoke.c" "$WORK/mw/"
cat >"$WORK/mw/meson.build" <<'EOF'
project('capnp-janet-wrap-smoke', 'c', default_options: ['c_std=c11'])
dep = dependency('capnp-janet', fallback: ['capnp-janet', 'capnp_janet_dep'])
executable('smoke', 'smoke.c', dependencies: dep)
EOF
meson setup "$WORK/mwb" "$WORK/mw" >"$WORK/mw.log" 2>&1 || { tail -25 "$WORK/mw.log"; exit 1; }
meson compile -C "$WORK/mwb" >>"$WORK/mw.log" 2>&1 || { tail -25 "$WORK/mw.log"; exit 1; }
"$WORK/mwb/smoke"

echo "ok packaging-smoke"
