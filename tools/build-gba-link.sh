#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CORE="$ROOT/compile/mgba-netplay"
RETRO="$ROOT/compile/retroarch-gba-netplay"
OUT="$ROOT/output/gba-link"
JOBS="${BUILD_JOBS:-8}"
test "$(git -c safe.directory="$CORE" -C "$CORE" rev-parse HEAD)" = 88e5f1a3cc350b3a76247f2312b78d9f85afd4b4
test "$(git -c safe.directory="$RETRO" -C "$RETRO" rev-parse HEAD)" = becfa4bad25132b6cb8068a0da463f67eefee44e
mkdir -p "$OUT"
python3 "$ROOT/tools/patch-mgba-netplay.py" "$CORE"
if [[ "${SKIP_FIXTURES:-0}" != 1 ]]; then
    python3 "$ROOT/tools/build-gba-fixtures.py" "$CORE" "$OUT/fixtures"
fi
emcmake cmake -S "$CORE" -B "$ROOT/compile/mgba-wasm" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-D_GNU_SOURCE -DLIBMGBA_ONLY=ON -DBUILD_LIBRETRO_SPLITSCREEN=ON \
    -DM_CORE_GBA=ON -DM_CORE_GB=OFF -DMINIMAL_CORE=ON -DBUILD_STATIC=OFF -DBUILD_SHARED=OFF \
    -DUSE_ZLIB=OFF -DUSE_PNG=OFF -DUSE_MINIZIP=OFF -DUSE_LIBZIP=OFF -DUSE_LZMA=OFF \
    -DENABLE_DEBUGGERS=OFF -DENABLE_SCRIPTING=OFF -DUSE_SQLITE3=OFF -DUSE_ELF=OFF
cmake --build "$ROOT/compile/mgba-wasm" --target mgba_splitscreen_libretro -j"$JOBS"
cp "$ROOT/compile/mgba-wasm/mgba_splitscreen_libretro.a" "$RETRO/libretro_emscripten.a"
cp "$ROOT/tools/ejs-input-netplay.inc" "$RETRO/ejs-input-netplay.inc"
python3 - "$RETRO/retroarch.c" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
s = p.read_text()
include = '\n#ifdef EMULATORJS\n#include "ejs-input-netplay.inc"\n#endif\n'
if include not in s:
    p.write_text(s + include)
PY
touch "$RETRO/retroarch.c"
emmake make -C "$RETRO" -f Makefile.emulatorjs -j"$JOBS" \
    TARGET=mgba_splitscreen_libretro.js HAVE_THREADS=0 HAVE_OPENGLES3=1 \
    HAVE_CHD=0 HAVE_7ZIP=1 HAVE_AL=0 HAVE_RWEBAUDIO=1 ASYNC=0
cp "$RETRO/mgba_splitscreen_libretro.js" "$RETRO/mgba_splitscreen_libretro.wasm" "$OUT/"
python3 - "$OUT" "$CORE" "$RETRO" <<'PY'
from pathlib import Path
import hashlib, json, subprocess, sys
out, core, retro = map(Path, sys.argv[1:])
revision = lambda p: subprocess.check_output(['git', '-c', f'safe.directory={p}', '-C', str(p), 'rev-parse', 'HEAD'], text=True).strip()
(out/'core.json').write_text(json.dumps({'name':'mgba_splitscreen','extensions':['gba'],'options':{},'save':'srm','repo':'https://github.com/Spuds0588/mgba-splitscreen'}))
(out/'build.json').write_text(json.dumps({'minimumEJSVersion':'4.3.0','version':'gba-link-2','mgba':revision(core),'retroarch':revision(retro)}))
(out/'mgba_splitscreen.json').write_text(json.dumps({'core':'mgba_splitscreen','buildStart':hashlib.sha256((out/'mgba_splitscreen_libretro.wasm').read_bytes()).hexdigest(),'options':{'defaultWebGL2':True}}))
(out/'license.txt').write_bytes((core/'LICENSE').read_bytes())
PY
cd "$OUT"
7z a mgba_splitscreen-wasm.data mgba_splitscreen_libretro.js mgba_splitscreen_libretro.wasm core.json build.json license.txt
