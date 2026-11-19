#!/bin/bash
#
# Builds libffmpegJNI.so for every Android ABI into ../android/src/main/jniLibs.
# FFmpeg is configured as LGPL (no --enable-gpl, no --enable-nonfree) with only the audio
# decoders Android lacks. Used by .github/workflows/audio-decoders.yml.
#
# Usage: build.sh <ffmpeg source folder, release/6.0> <Android NDK folder>
set -eu

FFMPEG_PATH="$(cd "$1" && pwd)"
NDK_PATH="$(cd "$2" && pwd)"
HERE="$(cd "$(dirname "$0")" && pwd)"
OUTPUT="$HERE/../android/src/main/jniLibs"
# The lowest Android version of the app (minSdkVersion in app.json).
ANDROID_API=24
# MPEG audio layer 1-3 (the "mp3" decoder also reads MP2), Dolby Digital (Plus), TrueHD, DTS.
DECODERS=(mp3 mp3float mp2 mp2float ac3 ac3_fixed eac3 truehd mlp dca)
ABIS=(armeabi-v7a arm64-v8a x86 x86_64)

case "$(uname -s)" in
  Darwin) HOST_PLATFORM=darwin-x86_64 ;;
  *) HOST_PLATFORM=linux-x86_64 ;;
esac

# The script of media3 builds FFmpeg inside its own folder, next to a link named "ffmpeg".
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/jni"
cp "$HERE/build_ffmpeg.sh" "$HERE/CMakeLists.txt" "$HERE/ffmpeg_jni.cc" "$WORK/jni/"
ln -s "$FFMPEG_PATH" "$WORK/jni/ffmpeg"
(cd "$WORK/jni" && bash ./build_ffmpeg.sh "$WORK" "$NDK_PATH" "$HOST_PLATFORM" "$ANDROID_API" "${DECODERS[@]}")

for abi in "${ABIS[@]}"; do
  cmake -S "$WORK/jni" -B "$WORK/build/$abi" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK_PATH/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$abi" \
    -DANDROID_PLATFORM="android-$ANDROID_API" \
    -DCMAKE_BUILD_TYPE=Release
  cmake --build "$WORK/build/$abi" --target ffmpegJNI
  mkdir -p "$OUTPUT/$abi"
  cp "$WORK/build/$abi/libffmpegJNI.so" "$OUTPUT/$abi/"
  "$NDK_PATH/toolchains/llvm/prebuilt/$HOST_PLATFORM/bin/llvm-strip" "$OUTPUT/$abi/libffmpegJNI.so"
done

# The exact FFmpeg used, for the license notice (LGPL asks to say which source was used).
git -C "$FFMPEG_PATH" rev-parse HEAD > "$OUTPUT/FFMPEG_VERSION"
echo "Done: $OUTPUT"
