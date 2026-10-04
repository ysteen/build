#!/usr/bin/env bash
set -euo pipefail

BUILD_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MELONDS_SOURCE="${MELONDS_SOURCE:-$BUILD_ROOT/compile/melonds}"
TEST_OUTPUT="$(mktemp -d /tmp/melonds-save-detection-test-XXXXXX)"
mkdir -p "$TEST_OUTPUT/src/libretro"
for file in NDSCart.cpp NDSCart.h NDS_Header.h NDSCart_SRAMManager.cpp NDSCart_SRAMManager.h Savestate.h ROMList.h types.h libretro/libretro.cpp libretro/libretro_core_options.h; do
    git -c safe.directory="$MELONDS_SOURCE" -C "$MELONDS_SOURCE" show "HEAD:src/$file" > "$TEST_OUTPUT/src/$file"
done

cd "$TEST_OUTPUT"
initialPath="$BUILD_ROOT"
name=melonds
MELONDS_PATCH_BLOCK="$(sed -n '/^    if \[ "$name" = "melonds" \]; then$/,/^    fi$/p' "$BUILD_ROOT/build.sh")"
test -n "$MELONDS_PATCH_BLOCK"
git apply "$BUILD_ROOT/patches/melonds-layton-korean-save.patch"
eval "$MELONDS_PATCH_BLOCK"
eval "$MELONDS_PATCH_BLOCK"
git apply --reverse --check "$BUILD_ROOT/patches/melonds-save-autodetect.patch"
git apply --reverse "$BUILD_ROOT/patches/melonds-save-autodetect.patch"
git apply --reverse --check "$BUILD_ROOT/patches/melonds-save-detection.patch"
git apply --reverse "$BUILD_ROOT/patches/melonds-save-detection.patch"
eval "$MELONDS_PATCH_BLOCK"

"${CXX:-g++}" -std=c++11 -O2 -Wall -Wextra -Werror -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I"$TEST_OUTPUT/src" \
    "$BUILD_ROOT/tools/tests/melonds-save-detection.cpp" -o "$TEST_OUTPUT/native"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$TEST_OUTPUT/native"

"${CXX:-g++}" -std=c++11 -O2 -Wall -Wextra -Werror -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I"$TEST_OUTPUT/src" \
    "$BUILD_ROOT/tools/tests/melonds-save-auto.cpp" -o "$TEST_OUTPUT/auto"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$TEST_OUTPUT/auto"

python3 "$BUILD_ROOT/tools/tests/extract-melonds-save-auto-integration.py" "$TEST_OUTPUT/src" "$TEST_OUTPUT"
"${CXX:-g++}" -std=c++11 -D__LIBRETRO__ -O2 -Wall -Wextra -Werror -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I"$TEST_OUTPUT/src" -I"$TEST_OUTPUT" \
    "$BUILD_ROOT/tools/tests/melonds-save-auto-integration.cpp" -o "$TEST_OUTPUT/integration"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$TEST_OUTPUT/integration" "$TEST_OUTPUT"

python3 - "$TEST_OUTPUT" <<'PY'
from pathlib import Path
import sys

root = Path(sys.argv[1])
parts = []
for filename, signature in [
    ("NDSCart.cpp", "bool LoadROM(const u8* romdata,"),
    ("libretro/libretro.cpp", "void retro_reset(void)"),
]:
    source = (root / "src" / filename).read_text()
    start = source.index(signature)
    function = source[start:source.index("\n}", start) + 2]
    if filename == "NDSCart.cpp":
        function = "namespace NDSCart {\n" + function + "\n}"
    parts.append(function)
(root / "melonds-save-restart.inc").write_text("\n".join(parts) + "\n")
PY
"${CXX:-g++}" -std=c++11 -O2 -Wall -Wextra -Werror -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I"$TEST_OUTPUT/src" -I"$TEST_OUTPUT" \
    "$BUILD_ROOT/tools/tests/melonds-save-restart.cpp" -o "$TEST_OUTPUT/restart"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$TEST_OUTPUT/restart" "$TEST_OUTPUT/restart.sav"
echo "Fresh, legacy, and incremental patch checks passed; artifacts: $TEST_OUTPUT"
