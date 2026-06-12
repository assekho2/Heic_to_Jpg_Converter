#!/usr/bin/env bash
set -euo pipefail

# Builds a portable heic_converter_mt binary with libheif, libde265 and libjpeg
# linked statically, so end users don't need any libraries installed.
#
# Windows: run from an MSYS2 UCRT64 shell with these packages installed:
#            pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake \
#                      mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-pkgconf \
#                      mingw-w64-ucrt-x86_64-libde265 mingw-w64-ucrt-x86_64-libjpeg-turbo
#          Produces a fully static heic_converter_mt.exe (no DLLs needed).
#
# macOS:   needs Xcode command line tools and: brew install cmake ninja jpeg-turbo
#          Produces heic_converter_mt that depends only on macOS system libraries.
#
# Dependency sources are downloaded and built once into build/deps; reruns
# only relink the app.

LIBHEIF_VERSION=1.23.0
LIBDE265_VERSION=1.1.0
LIBJPEG_TURBO_VERSION=3.0.4

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="$ROOT/build/deps"
PREFIX="$DEPS/prefix"
mkdir -p "$DEPS"

case "$(uname -s)" in
    MINGW*|MSYS*) PLATFORM=windows ;;
    Darwin)       PLATFORM=macos ;;
    *) echo "Unsupported platform: $(uname -s) (use the Makefile for a normal build)" >&2; exit 1 ;;
esac

export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
if [ "$PLATFORM" = macos ]; then
    # Allow the binary to run on older macOS versions than the build machine
    export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-11.0}"
fi

fetch() { # fetch <url> <dir-name-inside-tarball>
    local url=$1 name=$2
    if [ ! -d "$DEPS/$name" ]; then
        echo "== Downloading $name"
        curl -fsSL "$url" -o "$DEPS/$name.tar.gz"
        tar -xzf "$DEPS/$name.tar.gz" -C "$DEPS"
    fi
}

cmake_build() { # cmake_build <src-dir> [extra cmake args...]
    local src=$1; shift
    echo "== Building $(basename "$src")"
    cmake -S "$src" -B "$src/build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        "$@"
    cmake --build "$src/build"
    cmake --install "$src/build" >/dev/null
}

# ---- libde265 (HEVC decoder; on Windows pacman already provides libde265.a) ----
if [ "$PLATFORM" = macos ] && [ ! -f "$PREFIX/lib/libde265.a" ]; then
    fetch "https://github.com/strukturag/libde265/releases/download/v$LIBDE265_VERSION/libde265-$LIBDE265_VERSION.tar.gz" \
          "libde265-$LIBDE265_VERSION"
    cmake_build "$DEPS/libde265-$LIBDE265_VERSION" -DENABLE_SDL=OFF
fi

# ---- libjpeg ----
if [ "$PLATFORM" = windows ]; then
    JPEG_LINK="-ljpeg"   # pacman's mingw-w64-ucrt-x86_64-libjpeg-turbo ships libjpeg.a
else
    BREW_JT="$(brew --prefix jpeg-turbo 2>/dev/null || true)"
    if [ -n "$BREW_JT" ] && [ -f "$BREW_JT/lib/libjpeg.a" ]; then
        JPEG_LINK="$BREW_JT/lib/libjpeg.a"
    else
        # Fallback: build from source. SIMD is disabled to avoid requiring nasm;
        # install jpeg-turbo via brew if you want the faster SIMD build.
        if [ ! -f "$PREFIX/lib/libjpeg.a" ]; then
            fetch "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/$LIBJPEG_TURBO_VERSION/libjpeg-turbo-$LIBJPEG_TURBO_VERSION.tar.gz" \
                  "libjpeg-turbo-$LIBJPEG_TURBO_VERSION"
            cmake_build "$DEPS/libjpeg-turbo-$LIBJPEG_TURBO_VERSION" -DWITH_SIMD=OFF -DENABLE_SHARED=OFF
        fi
        JPEG_LINK="$PREFIX/lib/libjpeg.a"
    fi
fi

# ---- libheif (static, HEIC decode only - no AVIF/encoders/plugins) ----
if [ ! -f "$PREFIX/lib/libheif.a" ]; then
    fetch "https://github.com/strukturag/libheif/releases/download/v$LIBHEIF_VERSION/libheif-$LIBHEIF_VERSION.tar.gz" \
          "libheif-$LIBHEIF_VERSION"
    HEIF_EXTRA=""
    if [ "$PLATFORM" = windows ]; then
        # de265.h declares dllimport symbols unless told we link libde265.a
        HEIF_EXTRA="-DCMAKE_CXX_FLAGS=-DLIBDE265_STATIC_BUILD"
    fi
    cmake_build "$DEPS/libheif-$LIBHEIF_VERSION" \
        -DENABLE_PLUGIN_LOADING=OFF \
        -DWITH_LIBDE265=ON \
        -DWITH_X265=OFF \
        -DWITH_AOM_DECODER=OFF -DWITH_AOM_ENCODER=OFF \
        -DWITH_DAV1D=OFF -DWITH_RAV1E=OFF -DWITH_SvtEnc=OFF \
        -DWITH_JPEG_DECODER=OFF -DWITH_JPEG_ENCODER=OFF \
        -DWITH_OpenJPEG_DECODER=OFF -DWITH_OpenJPEG_ENCODER=OFF \
        -DWITH_OPENJPH_ENCODER=OFF -DWITH_OPENJPH_DECODER=OFF \
        -DWITH_X264=OFF \
        -DWITH_OpenH264_DECODER=OFF -DWITH_OpenH264_ENCODER=OFF \
        -DWITH_FFMPEG_DECODER=OFF -DWITH_VVDEC=OFF -DWITH_VVENC=OFF -DWITH_UVG266=OFF \
        -DWITH_UNCOMPRESSED_CODEC=OFF -DWITH_LIBSHARPYUV=OFF \
        -DWITH_EXAMPLES=OFF -DBUILD_TESTING=OFF \
        $HEIF_EXTRA
fi

# ---- link the app ----
echo "== Linking heic_converter_mt"
if [ "$PLATFORM" = windows ]; then
    # LIBHEIF_STATIC_BUILD stops heif.h from declaring dllimport symbols
    g++ -std=c++17 -O2 -Wall -pthread -static -DLIBHEIF_STATIC_BUILD=1 \
        -o "$ROOT/heic_converter_mt.exe" "$ROOT/heic_converter_mt.cpp" \
        -I"$PREFIX/include" -L"$PREFIX/lib" \
        -lheif -lde265 $JPEG_LINK -lz
    echo "== Built heic_converter_mt.exe; dynamic dependencies:"
    ldd "$ROOT/heic_converter_mt.exe" || true
else
    g++ -std=c++17 -O2 -Wall -pthread \
        -o "$ROOT/heic_converter_mt" "$ROOT/heic_converter_mt.cpp" \
        -I"$PREFIX/include" \
        "$PREFIX/lib/libheif.a" "$PREFIX/lib/libde265.a" $JPEG_LINK -lz
    echo "== Built heic_converter_mt; dynamic dependencies:"
    otool -L "$ROOT/heic_converter_mt"
fi
