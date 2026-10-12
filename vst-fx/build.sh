#!/usr/bin/env bash
# Build Cadenza FX: the insert-effect variant of ../vst (same sources, see ../vst/build.sh).
#   vst-fx/build/cadenza_fx.so          -> /sdcard/vst/ on the device
#   vst-fx/build/pluginlist-entry.xml   the <PLUGIN ... category="Effect"> line
#   vst-fx/build/skin/                  -> /sdcard/Synths/ on the device
# Only vst.json differs ("effect": true -> PLUG_EFFECT in params.h).
set -euo pipefail
cd "$(dirname "$0")"
MPC_VST="$(cd "${MPC_VST:-../../mpc-vst}" && pwd)"
U="$(id -u):$(id -g)"
mkdir -p build

if command -v gcc >/dev/null; then
  gcc -O2 -I"$MPC_VST/tools/vendor/force-shadow/tools" -o build/shadow_art "$MPC_VST/tools/shadow_art.c" -lm
else
  docker run --rm -u "$U" -v "$PWD/..":/w -v "$MPC_VST":/mv:ro -w /w/vst-fx gcc:12 \
    gcc -O2 -I/mv/tools/vendor/force-shadow/tools -o build/shadow_art /mv/tools/shadow_art.c -lm
fi
if python3 -c "import PIL" 2>/dev/null; then
  python3 "$MPC_VST/tools/gen_vst.py" vst.json
  python3 ../vst/captions.py ../vst/layout.conf "build/skin/lunarscraper - VST - Cadenza FX/Plugin Skins" ../vst/fonts/TitilliumWeb-SemiBold.ttf   # all labels at the knob names' size (Titillium Web)
else
  docker run --rm -u "$U" -v "$PWD/..":/w -v "$MPC_VST":/mv:ro -w /w/vst-fx python:3.11-slim sh -c \
    "pip install -q --no-warn-script-location --target /tmp/p pillow >/dev/null 2>&1; PYTHONPATH=/tmp/p python3 /mv/tools/gen_vst.py vst.json && PYTHONPATH=/tmp/p python3 ../vst/captions.py ../vst/layout.conf \"build/skin/lunarscraper - VST - Cadenza FX/Plugin Skins\" ../vst/fonts/TitilliumWeb-SemiBold.ttf"
fi
grep -q "PLUG_EFFECT" build/params.h || { echo "params.h has no PLUG_EFFECT: this would build the instrument"; exit 1; }
cp "$MPC_VST/wrapper/popup.h" build/

docker run --rm --platform linux/arm/v7 -v "$PWD/..":/b -w /b/vst-fx arm32v7/gcc:12 bash -euxc '
  apt-get update -qq && apt-get install -y -qq libasound2-dev >/dev/null
  mkdir -p build/obj
  g++ -O2 -fPIC -fvisibility=hidden -std=c++17 -ffp-contract=off -Wall -Wextra -Wno-unused-parameter \
      -Ibuild -I../vst -c ../vst/cadenza_vst.cpp -o build/obj/vst.o
  g++ -shared -o build/cadenza_fx.so build/obj/vst.o \
      -static-libstdc++ -static-libgcc -lasound -lpthread -ldl -lm -Wl,--no-undefined
  strip build/cadenza_fx.so
  echo "-- exported --"; readelf --dyn-syms -W build/cadenza_fx.so | grep -E " GLOBAL .* [0-9]+ [A-Za-z]" | grep -v UND
  echo "-- needed --"; readelf -d build/cadenza_fx.so | grep NEEDED
  echo "-- highest glibc (device has 2.39) --"; readelf -V build/cadenza_fx.so | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1
  chown -R '"$U"' build
'
md5sum build/cadenza_fx.so
