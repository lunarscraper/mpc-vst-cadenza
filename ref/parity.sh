#!/usr/bin/env bash
# Parity test: the C++ port (vst/cadenza_engine.h) against the Go original, event by event.
# ref/go is a dependency-free copy of Cadenza's offline path (backend/internal/{theory,generator/offline*,
# renderer,midi,schema/pattern_spec.go}, MIT, github.com/Andrea-Cavallo/cadenza) plus a small dump command;
# only change: the sub-bass density loop fix marked "PARITY-TEST FIX" (the original hangs there).
# Needs Go >= 1.22 and g++. Usage: ref/parity.sh [cases]   (default 3000 random key/scale/bpm/seed/style)
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
(cd go && GOTOOLCHAIN=local GOFLAGS=-mod=mod go build -o ../build/dump-go ./cmd/dump)
g++ -O2 -std=c++17 -ffp-contract=off -o build/dump-cpp ref_dump.cpp
python3 parity.py build/dump-go build/dump-cpp "${1:-3000}"
