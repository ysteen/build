#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CORE="$ROOT/compile/sameboy-netplay"
RETRO="$ROOT/compile/retroarch-netplay"
RGBDS="$ROOT/compile/rgbds-netplay"
OUT="$ROOT/output/input-netplay"
JOBS="${BUILD_JOBS:-8}"
for source in "$CORE/libretro/Makefile" "$RETRO/Makefile.emulatorjs" "$RGBDS/Makefile"; do
    if [[ ! -f "$source" ]]; then
        printf 'Missing source: %s. Run tools/prepare-input-netplay.sh first.\n' "$source" >&2
        exit 1
    fi
done
mkdir -p "$OUT"
python3 "$ROOT/tools/patch-sameboy-netplay.py" "$CORE"
make -C "$RGBDS" -j"$JOBS"
make -C "$CORE" -j"$JOBS" bootroms RGBDS="$RGBDS/"
emmake make -C "$CORE/libretro" -j"$JOBS" platform=emscripten STATIC_LINKING=1 AR=emar CC=emcc BOOTROMS_DIR="$CORE/build/bin/BootROMs"
cp "$CORE/libretro/sameboy_libretro_emscripten.bc" "$RETRO/libretro_emscripten.a"
cp "$ROOT/tools/ejs-input-netplay.inc" "$RETRO/ejs-input-netplay.inc"
touch "$RETRO/retroarch.c"
python3 - "$RETRO/retroarch.c" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
s = p.read_text()
include = '\n#ifdef EMULATORJS\n#include "ejs-input-netplay.inc"\n#endif\n'
if include not in s:
    p.write_text(s + include)
PY
emmake make -C "$RETRO" -f Makefile.emulatorjs -j"$JOBS" \
    TARGET=sameboy_libretro.js HAVE_THREADS=0 HAVE_OPENGLES3=1 \
    HAVE_CHD=0 HAVE_7ZIP=1 HAVE_AL=0 HAVE_RWEBAUDIO=1 ASYNC=0
cp "$RETRO/sameboy_libretro.js" "$RETRO/sameboy_libretro.wasm" "$OUT/"
python3 - "$OUT" "$CORE" "$RETRO" <<'PY'
from pathlib import Path
import hashlib, json, subprocess, sys
out, core, retro = map(Path, sys.argv[1:])
revision = lambda p: subprocess.check_output(['git', '-c', f'safe.directory={p}', '-C', str(p), 'rev-parse', 'HEAD'], text=True).strip()
(out/'core.json').write_text(json.dumps({'name':'sameboy','extensions':['gb','gbc'],'options':{},'save':'srm','repo':'https://github.com/LIJI32/SameBoy'}))
(out/'build.json').write_text(json.dumps({'minimumEJSVersion':'4.3.0','version':'input-netplay-1','sameboy':revision(core),'retroarch':revision(retro)}))
(out/'sameboy.json').write_text(json.dumps({'core': 'sameboy', 'buildStart': hashlib.sha256((out/'sameboy_libretro.wasm').read_bytes()).hexdigest(), 'options': {'defaultWebGL2': True}}))
(out/'license.txt').write_bytes((core/'LICENSE').read_bytes())
PY
cd "$OUT"
7z a sameboy-wasm.data sameboy_libretro.js sameboy_libretro.wasm core.json build.json license.txt
printf 'Built %s/sameboy-wasm.data\n' "$OUT"
