#!/usr/bin/env bash
# Offline x86 test (ASan/UBSan): run after build.sh (needs build/params.h, build/popup.h).
# Prints PASSED/FAILED; exit code follows.
set -euo pipefail
cd "$(dirname "$0")"
docker run --rm -v "$PWD/..":/b -w /b/vst gcc:12 bash -euc 'set -o pipefail
  mkdir -p build/x86
  g++ -O0 -g -fsanitize=address,undefined -std=c++17 -ffp-contract=off -Wall -Wextra -Wno-unused-parameter -DNO_ALSA -fPIC \
      -Ibuild -I. -shared -o build/x86/cadenza-x86.so cadenza_vst.cpp -lpthread -ldl
  g++ -O0 -g -fsanitize=address,undefined -std=c++17 -ffp-contract=off -I. -o build/x86/host_test host_test.cpp -ldl -lpthread
  ASAN_OPTIONS=detect_leaks=0 ./build/x86/host_test ./build/x86/cadenza-x86.so | grep -v "^MIDI"
'
