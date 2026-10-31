#!/usr/bin/env bash
# Сборка и упаковка fluidsynth.ygplugin (arm64-v8a + armeabi-v7a).
# Нужны: Android NDK (ANDROID_NDK_HOME), cmake >= 3.22, ninja, zip.
#   ANDROID_NDK_HOME=~/Android/Sdk/ndk/27.0.12077973 ./build.sh
#   ABIS="arm64-v8a" ./build.sh     # только одна архитектура
set -euo pipefail
: "${ANDROID_NDK_HOME:?Укажи путь к NDK, например ~/Android/Sdk/ndk/27.0.12077973}"
cd "$(dirname "$0")"
ENTRY=libyg_fluidsynth.so
ABIS=${ABIS:-"arm64-v8a armeabi-v7a"}
rm -rf out && mkdir -p out/pkg
for ABI in $ABIS; do
  cmake -S . -B out/build-$ABI -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-31 -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release
  cmake --build out/build-$ABI --target yg_fluidsynth
  mkdir -p out/pkg/lib/$ABI
  cp out/build-$ABI/$ENTRY out/pkg/lib/$ABI/
  "$ANDROID_NDK_HOME"/toolchains/llvm/prebuilt/*/bin/llvm-strip --strip-unneeded out/pkg/lib/$ABI/$ENTRY || true
done
cp manifest.json out/pkg/
(cd out/pkg && zip -qr ../fluidsynth.ygplugin manifest.json lib)
echo "Готово: out/fluidsynth.ygplugin"
