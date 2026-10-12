#!/usr/bin/env bash
# Offline x86 test (ASan/UBSan) of the effect variant: same host_test as ../vst; it sees numInputs == 2
# and additionally checks the audio pass-through. Prints PASSED/FAILED; exit code follows.
set -euo pipefail
cd "$(dirname "$0")"
docker run --rm -v "$PWD/..":/b -w /b/vst-fx gcc:12 bash -euc 'set -o pipefail
  mkdir -p build/x86
  g++ -O0 -g -fsanitize=address,undefined -std=c++17 -ffp-contract=off -Wall -Wextra -Wno-unused-parameter -DNO_ALSA -fPIC \
      -Ibuild -I../vst -shared -o build/x86/cadenza_fx-x86.so ../vst/cadenza_vst.cpp -lpthread -ldl
  g++ -O0 -g -fsanitize=address,undefined -std=c++17 -ffp-contract=off -I../vst -o build/x86/host_test ../vst/host_test.cpp -ldl -lpthread
  ASAN_OPTIONS=detect_leaks=0 ./build/x86/host_test ./build/x86/cadenza_fx-x86.so | grep -v "^MIDI"
'
