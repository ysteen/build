#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
docker compose -f compose.dosbox-pure.yml run --rm builder

printf '\nDOSBox Pure build artifacts:\n'
find output -maxdepth 1 -type f -name 'dosbox_pure*' -printf '  %p (%s bytes)\n' | sort
