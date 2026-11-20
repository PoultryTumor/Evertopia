#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKSPACE_DIR="$(cd "$PROJECT_DIR/.." && pwd)"

export JAVA_HOME="${JAVA_HOME:-$HOME/Android/jdk-17}"
export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
export ANDROID_SDK_ROOT="$ANDROID_HOME"
export ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-$ANDROID_HOME/ndk/27.2.12479018}"
GRADLE_BIN="${GRADLE_BIN:-$HOME/Android/gradle-8.7/bin/gradle}"
export PATH="$JAVA_HOME/bin:$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools:$PATH"

VERSION="$(tr -d '[:space:]' < "$PROJECT_DIR/VERSION")"
BUILD_DIR="$PROJECT_DIR/build-android-arm64"
JNILIBS_DIR="$SCRIPT_DIR/app/src/main/jniLibs/arm64-v8a"
SDL3_SRC="$WORKSPACE_DIR/deps/SDL3-3.4.10"

echo "=== Building Evertopia $VERSION for Android (arm64-v8a) ==="

cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-24 \
    -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release \
    -DSNESRECOMP_CRC32_IMPL=SLICE8 \
    -DSNESRECOMP_SDL3_SOURCE_DIR="$SDL3_SRC"

cmake --build "$BUILD_DIR" --target SecretOfEvermoreSNESRecomp -j"$(nproc)"

mkdir -p "$JNILIBS_DIR"
cp -f "$BUILD_DIR/libmain.so" "$JNILIBS_DIR/libmain.so"
if [ -f "$BUILD_DIR/sdl3-android/libSDL3.so" ]; then
    cp -f "$BUILD_DIR/sdl3-android/libSDL3.so" "$JNILIBS_DIR/libSDL3.so"
elif [ -f "$BUILD_DIR/libSDL3.so" ]; then
    cp -f "$BUILD_DIR/libSDL3.so" "$JNILIBS_DIR/libSDL3.so"
else
    echo "ERROR: libSDL3.so not found in $BUILD_DIR" >&2
    exit 1
fi

# Strip debug symbols for a compact APK
LLVM_STRIP="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
if [ -x "$LLVM_STRIP" ]; then
    "$LLVM_STRIP" --strip-unneeded "$JNILIBS_DIR/libmain.so" "$JNILIBS_DIR/libSDL3.so"
fi

# Create a persistent signing keystore if needed
KEYSTORE="$SCRIPT_DIR/app/evertopia-release.keystore"
if [ ! -f "$KEYSTORE" ]; then
    "$JAVA_HOME/bin/keytool" -genkeypair -v \
        -keystore "$KEYSTORE" \
        -alias evertopia \
        -keyalg RSA -keysize 2048 -validity 10000 \
        -storepass evertopia -keypass evertopia \
        -dname "CN=Evertopia, OU=Android, O=Evertopia, L=Unknown, S=Unknown, C=US"
fi

# Build signed release APK
cd "$SCRIPT_DIR"
"$GRADLE_BIN" --no-daemon assembleRelease

OUT_APK="$WORKSPACE_DIR/Evertopia-${VERSION}-android-arm64.apk"
cp -f "$SCRIPT_DIR/app/build/outputs/apk/release/app-release.apk" "$OUT_APK"

echo "=== Built APK: $OUT_APK ==="
ls -lh "$OUT_APK"
