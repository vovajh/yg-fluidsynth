#!/usr/bin/env bash
# Нативный (x86_64) прогон: настоящий FluidSynth + заглушка ХОСТА Yougram (только чат/файлы эмулируются).
set -euo pipefail
cd "$(dirname "$0")"
cmake -S . -B build_native -G Ninja -DYG_NATIVE_TEST=ON >/dev/null
cmake --build build_native --target yg_fluidsynth mock_host
python3 tools/gen_test_midi.py /tmp/test8.mid 8
python3 tools/gen_test_midi.py /tmp/test_long.mid 400
./build_native/mock_host build_native/libyg_fluidsynth.so /tmp/ygtest/data /tmp/test8.mid /tmp/test_long.mid \
  "$PWD/third_party/fluidsynth/sf2/VintageDreamsWaves-v2.sf2"
echo "WAV-результаты: /tmp/ygtest/sent/"
