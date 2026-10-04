#!/usr/bin/env python3
"""Extract production save methods for the native integration harness."""

from pathlib import Path
import re
import sys


source_dir = Path(sys.argv[1]).resolve()
output_dir = Path(sys.argv[2]).resolve()
output_dir.mkdir(parents=True, exist_ok=True)


def write_fragment(name, path, start, end):
    source = path.read_text()
    begin = source.index(start)
    finish = source.index(end, begin)
    line = source.count("\n", 0, begin) + 1
    output_dir.joinpath(name).write_text(
        f'#line {line} "{path}"\n' + source[begin:finish]
    )


cart = source_dir / "NDSCart.cpp"
write_fragment(
    "melonds-save-auto-globals.inc", cart,
    "static int SaveMemoryOverride", "u32 Key1_KeyBuf",
)
write_fragment(
    "melonds-save-auto-cart.inc", cart,
    "CartRetail::CartRetail(", "CartRetailNAND::CartRetailNAND(",
)
manager = source_dir / "NDSCart_SRAMManager.cpp"
manager_source = manager.read_text()
manager_source = re.sub(r"^#include[^\n]*", "", manager_source, flags=re.MULTILINE)
output_dir.joinpath("melonds-save-auto-manager.inc").write_text(
    f'#line 1 "{manager}"\n' + manager_source
)
