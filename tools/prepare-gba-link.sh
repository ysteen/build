#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CORE="$ROOT/compile/mgba-netplay"
REVISION=88e5f1a3cc350b3a76247f2312b78d9f85afd4b4
if [[ ! -d "$CORE/.git" ]]; then
    git init "$CORE"
    git -C "$CORE" remote add origin https://github.com/Spuds0588/mgba-splitscreen.git
    git -C "$CORE" fetch --depth 1 origin "$REVISION"
    git -C "$CORE" checkout --detach FETCH_HEAD
fi
test "$(git -C "$CORE" rev-parse HEAD)" = "$REVISION"
RETRO="$ROOT/compile/retroarch-gba-netplay"
if [[ ! -d "$RETRO/.git" ]]; then
    git init "$RETRO"
    git -C "$RETRO" remote add origin "${INPUT_NETPLAY_RETRO_SOURCE:-https://github.com/ysteen/RetroArch.git}"
    git -C "$RETRO" fetch --depth 1 origin becfa4bad25132b6cb8068a0da463f67eefee44e
    git -C "$RETRO" checkout --detach becfa4bad25132b6cb8068a0da463f67eefee44e
fi
test "$(git -C "$RETRO" rev-parse HEAD)" = becfa4bad25132b6cb8068a0da463f67eefee44e
