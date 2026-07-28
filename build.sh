#!/usr/bin/env bash
set -e

# VVdeC/Makefile companion: build the shared library for Linux / macOS.
# Mirrors build.bat but uses a separate build-shared/ output dir to avoid
# clashing with the Windows build/ artifacts.

# Detect OS
OS="$(uname -s)"
if [ "$OS" = "Darwin" ]; then
  SHLIB_EXT=".dylib"
  NPROC=$(sysctl -n hw.logicalcpu 2>/dev/null || echo 1)
elif [ "$OS" = "Linux" ]; then
  SHLIB_EXT=".so"
  NPROC=$(nproc 2>/dev/null || echo 1)
else
  echo "ERROR: unsupported OS '$OS'"
  exit 1
fi

# Locate build tools
for tool in cmake make cc c++; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "ERROR: $tool not found. Please install cmake and a C/C++ compiler."
    exit 1
  fi
done

# Separate build dir from Windows build/
BUILD_DIR=build-shared
ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
ASAN=0

# Parse arguments
for arg in "$@"; do
  case "$arg" in
    --asan)
      BUILD_DIR=build-shared-asan
      ASAN=1
      ;;
    --help|-h)
      echo "Usage: build.sh [--asan]"
      echo "  (no args)  Normal build, output to build-shared/"
      echo "  --asan     AddressSanitizer build (ASan+UBSan), output to build-shared-asan/"
      exit 0
      ;;
    *)
      echo "ERROR: unknown argument '$arg'"
      echo "Usage: build.sh [--asan]"
      exit 1
      ;;
  esac
done

# Wipe stale cache if build options changed
if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
  CURRENT_SHARED=$(grep -E "^BUILD_SHARED_LIBS:BOOL=" "$BUILD_DIR/CMakeCache.txt" | cut -d= -f2)
  CURRENT_LIB_ONLY=$(grep -E "^VVDEC_LIBRARY_ONLY:BOOL=" "$BUILD_DIR/CMakeCache.txt" | cut -d= -f2)
  CURRENT_ASAN=$(grep -E "^VVDEC_USE_ADDRESS_SANITIZER:BOOL=" "$BUILD_DIR/CMakeCache.txt" | cut -d= -f2)
  WANT_ASAN=$([ "$ASAN" = "1" ] && echo "ON" || echo "OFF")
  if [ "$CURRENT_SHARED" != "ON" ] || [ "$CURRENT_LIB_ONLY" != "OFF" ] || [ "$CURRENT_ASAN" != "$WANT_ASAN" ]; then
    echo "Stale CMake cache detected (SHARED=$CURRENT_SHARED, LIB_ONLY=$CURRENT_LIB_ONLY, ASAN=$CURRENT_ASAN). Wiping $BUILD_DIR/"
    rm -rf "$BUILD_DIR"
  fi
fi

mkdir -p "$BUILD_DIR"

# Configure: shared library + compile_commands.json for IDE/clangd.
# VVDEC_LIBRARY_ONLY is NOT set so vvdecapp stays in the default build (handy
# for quick CLI decode tests); the script still only builds the vvdec target
# explicitly below.
ASAN_FLAG=""
if [ "$ASAN" = "1" ]; then
  ASAN_FLAG="-DVVDEC_USE_ADDRESS_SANITIZER=ON"
  echo "[ASan] Building with AddressSanitizer + UBSan + LeakSanitizer"
fi

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=ON \
  -DVVDEC_LIBRARY_ONLY=OFF \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  $ASAN_FLAG

# Build the shared library and the CLI app
cmake --build "$BUILD_DIR" -j"$NPROC" --target vvdec vvdecapp

SHARED_LIB="$ROOT_DIR/lib/release-shared/libvvdec$SHLIB_EXT"
if [ -f "$SHARED_LIB" ]; then
  cp -f "$SHARED_LIB" "$BUILD_DIR/libvvdecLib$SHLIB_EXT"
  echo "Copied: $SHARED_LIB -> $BUILD_DIR/libvvdecLib$SHLIB_EXT"
else
  echo "ERROR: $SHARED_LIB not found"
  exit 1
fi

echo
echo "=== vvdec build done ==="
echo "Output: $BUILD_DIR/libvvdecLib$SHLIB_EXT"
if [ "$ASAN" = "1" ]; then
  echo
  echo "[ASan] To run vvdecapp under ASan:"
  echo "  ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 $ROOT_DIR/bin/release-shared/vvdecapp -b <bitstream>"
  echo "[ASan] To load the ASan shared library into YUView, YUView itself must also be built with ASan."
fi
