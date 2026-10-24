#!/usr/bin/env bash
# Prepare sources, then run:
# docker compose -f compose.dosbox-pure.yml -f compose.input-netplay.yml run --rm builder
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

checkout_remote() {
    local destination="$1" repository="$2" revision="$3"
    if [[ ! -d "$destination/.git" ]]; then
        git init "$destination"
        git -C "$destination" remote add origin "$repository"
        git -C "$destination" fetch --depth 1 origin "$revision"
        git -C "$destination" checkout --detach FETCH_HEAD
    fi
    if [[ "$(git -C "$destination" rev-parse HEAD)" != "$revision" ]]; then
        printf 'Unexpected revision in %s; use a separate checkout.\n' "$destination" >&2
        exit 1
    fi
}

mkdir -p "$ROOT/compile"
checkout_remote "$ROOT/compile/sameboy-netplay" https://github.com/LIJI32/SameBoy.git 26a56bd6a122a6318a456abd80558408742d5adf
checkout_remote "$ROOT/compile/rgbds-netplay" https://github.com/gbdev/rgbds.git d766efe95d82f0d64dd95dece60b7681c7a14de9

RETRO_SOURCE="${INPUT_NETPLAY_RETRO_SOURCE:-https://github.com/ysteen/RetroArch.git}"
checkout_remote "$ROOT/compile/retroarch-netplay" "$RETRO_SOURCE" becfa4bad25132b6cb8068a0da463f67eefee44e
