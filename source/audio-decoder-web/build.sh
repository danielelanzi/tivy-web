#!/bin/sh
# Builds the audio decoder of the browser player (decoder.c) to WebAssembly, with FFmpeg's
# libavcodec in LGPL form: only the AC3, E-AC3, MP2 and MP3 decoders and their parsers.
# Needs Emscripten 3.1.74 (emsdk) and git. The result, one JavaScript file with the
# WebAssembly inside, replaces packages/ui/src/components/player/audioDecoderWasm.js.
#   sh tools/audio-decoder/build.sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
work=${WORK:-/tmp/tivy-audio-decoder}
ffmpeg_tag=n7.1.1
mkdir -p "$work"
[ -d "$work/ffmpeg" ] || git clone --depth 1 --branch "$ffmpeg_tag" https://github.com/FFmpeg/FFmpeg.git "$work/ffmpeg"
cd "$work/ffmpeg"
emconfigure ./configure --prefix="$work/build" --target-os=none --arch=x86_32 --enable-cross-compile \
  --disable-asm --disable-x86asm --disable-inline-asm --disable-stripping --disable-programs \
  --disable-doc --disable-debug --disable-runtime-cpudetect --disable-autodetect \
  --disable-everything --disable-avformat --disable-avdevice --disable-avfilter \
  --disable-swscale --disable-swresample --disable-network --disable-pthreads \
  --disable-w32threads --disable-os2threads \
  --enable-decoder=ac3,eac3,mp2float,mp3float --enable-parser=ac3,mpegaudio \
  --cc=emcc --cxx=em++ --ar=emar --ranlib=emranlib --nm=emnm --extra-cflags=-Oz
emmake make -j8
emmake make install
emcc -Oz "$here/decoder.c" -I"$work/build/include" "$work/build/lib/libavcodec.a" \
  "$work/build/lib/libavutil.a" -o "$work/audioDecoderWasm.js" \
  -sMODULARIZE=1 -sEXPORT_NAME=createAudioDecoder -sSINGLE_FILE=1 -sENVIRONMENT=web,worker \
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=24MB -sFILESYSTEM=0 \
  -sEXPORTED_FUNCTIONS=_malloc,_free -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAP16,stringToNewUTF8
{
  echo '/* eslint-disable */'
  echo '// @ts-nocheck'
  echo "// Built by tools/audio-decoder/build.sh from decoder.c and FFmpeg $ffmpeg_tag (LGPL 2.1 or later)."
  cat "$work/audioDecoderWasm.js"
} > "$root/packages/ui/src/components/player/audioDecoderWasm.js"
