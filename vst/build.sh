#!/usr/bin/env bash
# Build Cadenza as a VST2 plugin for the MPC OS plugin host (armhf).
#   vst/build/cadenza.so            -> /sdcard/vst/ on the device
#   vst/build/pluginlist-entry.xml  the <PLUGIN> line for MPC.settings' pluginList-arm
#   vst/build/skin/                 -> /sdcard/Synths/ on the device
# Hand-written VST2 shell (cadenza_vst.cpp, after mpc-vst-drumgen); vst.json/params.json/layout.conf
# only feed mpc-vst-plugins' tools/gen_vst.py for params.h + the skin. Offline test: test.sh.
set -euo pipefail
cd "$(dirname "$0")"
MPC_VST="$(cd "${MPC_VST:-../../mpc-vst}" && pwd)"
U="$(id -u):$(id -g)"
mkdir -p build

# 1. skin artwork renderer (host binary; the renderer vendored in mpc-vst-plugins)
if command -v gcc >/dev/null; then
  gcc -O2 -I"$MPC_VST/tools/vendor/force-shadow/tools" -o build/shadow_art "$MPC_VST/tools/shadow_art.c" -lm
else
  docker run --rm -u "$U" -v "$PWD":/w -v "$MPC_VST":/mv:ro -w /w gcc:12 \
    gcc -O2 -I/mv/tools/vendor/force-shadow/tools -o build/shadow_art /mv/tools/shadow_art.c -lm
fi

# 2. params.h, skin, pluginlist-entry.xml (needs Pillow); captions.py then redraws the small baked
#    bitmap labels in Titillium Web at the size of the knob names
if python3 -c "import PIL" 2>/dev/null; then
  python3 "$MPC_VST/tools/gen_vst.py" vst.json
  python3 captions.py layout.conf "build/skin/lunarscraper - VST - Cadenza/Plugin Skins" fonts/TitilliumWeb-SemiBold.ttf   # all labels at the knob names' size (Titillium Web)
else
  docker run --rm -u "$U" -v "$PWD":/w -v "$MPC_VST":/mv:ro -w /w python:3.11-slim sh -c \
    "pip install -q --no-warn-script-location --target /tmp/p pillow >/dev/null 2>&1; PYTHONPATH=/tmp/p python3 /mv/tools/gen_vst.py vst.json && PYTHONPATH=/tmp/p python3 captions.py layout.conf \"build/skin/lunarscraper - VST - Cadenza/Plugin Skins\" fonts/TitilliumWeb-SemiBold.ttf"
fi
cp "$MPC_VST/wrapper/popup.h" build/

# 3. the plugin (armhf, glibc 2.36 so it loads on the device's 2.39)
docker run --rm --platform linux/arm/v7 -v "$PWD/..":/b -w /b/vst arm32v7/gcc:12 bash -euxc '
  apt-get update -qq && apt-get install -y -qq libasound2-dev >/dev/null
  mkdir -p build/obj
  g++ -O2 -fPIC -fvisibility=hidden -std=c++17 -ffp-contract=off -Wall -Wextra -Wno-unused-parameter \
      -Ibuild -I. -c cadenza_vst.cpp -o build/obj/vst.o
  g++ -shared -o build/cadenza.so build/obj/vst.o \
      -static-libstdc++ -static-libgcc -lasound -lpthread -ldl -lm -Wl,--no-undefined
  strip build/cadenza.so
  echo "-- exported --"; readelf --dyn-syms -W build/cadenza.so | grep -E " GLOBAL .* [0-9]+ [A-Za-z]" | grep -v UND
  echo "-- needed --"; readelf -d build/cadenza.so | grep NEEDED
  echo "-- highest glibc (device has 2.39) --"; readelf -V build/cadenza.so | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1
  chown -R '"$U"' build
'
md5sum build/cadenza.so
