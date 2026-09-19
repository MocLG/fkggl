#!/bin/sh
# Build the JNI shared libraries from the repo-root photo.c (single source
# of truth) using the system clang against the NDK sysroot — no NDK
# binaries are executed, so this works on any host architecture.
#
# Usage: scripts/build_native.sh [ndk-path]
set -e

NDK="${1:-$HOME/android-sdk/ndk/27.2.12479018}"
SYS="$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/app/src/main/jniLibs"
SRC="$ROOT/app/src/main/cpp"

# app/src/main/cpp/photo.c is a build-time copy of the root photo.c
cp "$ROOT/photo.c" "$SRC/photo.c"
cp "$ROOT/photo.h" "$SRC/photo.h"

build() {
    abi="$1" target="$2" triple="$3"
    mkdir -p "$OUT/$abi"
    clang --target="$target" --sysroot="$SYS" -O2 -I"$SRC" -c "$SRC/photo.c" \
        -o "/tmp/photo_$abi.o"
    clang --target="$target" --sysroot="$SYS" -O2 -I"$SRC" -c "$SRC/photo_jni.c" \
        -o "/tmp/jni_$abi.o"
    ld.lld -shared -o "$OUT/$abi/libphoto_jni.so" --sysroot="$SYS" \
        "/tmp/jni_$abi.o" "/tmp/photo_$abi.o" \
        -L"$SYS/usr/lib/$triple/26" -lm -lc
    echo "built $OUT/$abi/libphoto_jni.so"
}

build arm64-v8a aarch64-linux-android26 aarch64-linux-android
build x86_64   x86_64-linux-android26   x86_64-linux-android
