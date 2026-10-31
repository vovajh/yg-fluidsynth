#!/data/data/com.termux/files/usr/bin/bash
# НЕ ПРОВЕРЕНО на устройстве: сборка прямо в Termux (только архитектура телефона, обычно arm64-v8a).
# Основной путь — build.sh с NDK или GitHub Actions (.github/workflows/build.yml).
set -euo pipefail
pkg install -y cmake ninja clang zip
cd "$(dirname "$0")"
rm -rf out && mkdir -p out/pkg/lib/arm64-v8a
cmake -S . -B out/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build out/build --target yg_fluidsynth
cp out/build/libyg_fluidsynth.so out/pkg/lib/arm64-v8a/
cp manifest.json out/pkg/
(cd out/pkg && zip -qr ../fluidsynth.ygplugin manifest.json lib)
echo "Готово: out/fluidsynth.ygplugin"
