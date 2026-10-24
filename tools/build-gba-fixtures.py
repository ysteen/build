#!/usr/bin/env python3
"""Build four distinct homebrew cartridges for cable, input and SRAM checks."""
from pathlib import Path
import os
import subprocess
import sys

core, out = map(Path, sys.argv[1:])
out.mkdir(parents=True, exist_ok=True)
fixture = core / "mgba-splitscreen/linktest"
helpers = (fixture / "main.c").read_text().split("/* ---- link protocol ---- */")[0]
source = out / "main.c"
source.write_text(helpers + Path(__file__).with_name("gba-link-fixture.c").read_text())
clang = os.environ.get("CLANG", "/usr/bin/clang")
flags = ["--target=arm-none-eabi", "-mcpu=arm7tdmi", "-marm", "-O2", "-ffreestanding", "-fno-builtin"]


def run(*args):
    subprocess.run(list(map(str, args)), check=True)


boot = out / "crt0.s"
boot.write_text((fixture / "crt0.s").read_text().replace('"MGBA_SPLITSCREEN LINK"', '"ROMM LINKTST"'))
run(clang, *flags, "-c", boot, "-o", out / "crt0.o")
for player in (1, 2, 3, 4):
    base = out / f"gba-link-{player}"
    run(clang, *flags, f"-DFIXTURE_ID={player}", "-c", source, "-o", out / "main.o")
    run("arm-none-eabi-ld", "-T", fixture / "link.ld", "-o", base.with_suffix(".elf"), out / "crt0.o", out / "main.o")
    run("arm-none-eabi-objcopy", "-O", "binary", base.with_suffix(".elf"), base.with_suffix(".gba"))
    run("python3", fixture / "fix_header.py", base.with_suffix(".gba"))
    data = bytearray(base.with_suffix(".gba").read_bytes())
    data[0xBD] = (-sum(data[0xA0:0xBD]) - 0x19) & 0xFF
    base.with_suffix(".gba").write_bytes(data)
