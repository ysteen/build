"""Build two original serial/input test cartridges (requires RGBDS)."""
from pathlib import Path
import subprocess
import sys

rgbds, out = map(Path, sys.argv[1:])
out.mkdir(parents=True, exist_ok=True)
font = [
    "0000000000000000",  # blank
    "3c666e7666663c00", "1838181818187e00", "3c66060c30607e00", "3c66061c06663c00",
    "0c1c3c6c7e0c0c00", "7e607c0606663c00", "1c30607c66663c00", "7e66060c18181800",
    "3c66663c66663c00", "3c66663e060c3800", "183c66667e666600", "7c66667c66667c00",
    "3c66606060663c00", "786c6666666c7800", "7e60607c60607e00", "7e60607c60606000",
    "6060606060607e00", "7e18181818187e00", "6666767e6e666600", "666c7870786c6600",
]
tiles = [byte for glyph in font for byte in bytes.fromhex(glyph) for _ in range(2)]
source = """; Original diagnostic ROM, no external game assets.
SECTION "Header", ROM0[$100]
    nop
    jp Main
    ds $150 - @, 0
SECTION "Main", ROM0[$150]
Main:
    di
    ld sp, $dfff
.wait:
    ldh a, [$ff44]
    cp 144
    jr c, .wait
    xor a
    ldh [$ff40], a
    ld hl, $c000
    ld b, 16
.clear:
    ld [hli], a
    dec b
    jr nz, .clear
    ld a, $a5
    ld [$c004], a
    ld de, Tiles
    ld hl, $8000
    ld bc, TilesEnd - Tiles
.tiles:
    ld a, [de]
    inc de
    ld [hli], a
    dec bc
    ld a, b
    or c
    jr nz, .tiles
    ld hl, $9800
    ld bc, 1024
.map:
    xor a
    ld [hli], a
    dec bc
    ld a, b
    or c
    jr nz, .map
    ld hl, $9844
    ld a, 17
    ld [hli], a
    inc a
    ld [hli], a
    inc a
    ld [hli], a
    inc a
    ld [hli], a
    inc hl
    ld a, ROLE + 2
    ld [hl], a
    ld a, $e4
    ldh [$ff47], a
IF ROLE == 1
    ld a, $80
    ldh [$ff68], a
    ld hl, Palette
    ld b, 8
.palette:
    ld a, [hli]
    ldh [$ff69], a
    dec b
    jr nz, .palette
ENDC
    xor a
    ldh [$ff42], a
    ldh [$ff43], a
    ld a, $91
    ldh [$ff40], a
IF ROLE == 1
    ld a, $b0
    ldh [$ff01], a
    ld a, $80
    ldh [$ff02], a
    ld a, 1
    ld [$c006], a
ENDC
Loop:
    ldh a, [$ff44]
    cp 144
    jr nc, Loop
.vblank:
    ldh a, [$ff44]
    cp 144
    jr c, .vblank
    ld hl, $c005
    inc [hl]
    ld a, $10
    ldh [$ff00], a
    ldh a, [$ff00]
    ldh a, [$ff00]
    cpl
    and $0f
    ld [$c002], a
    ld b, a
    ld a, $30
    ldh [$ff00], a
    ldh a, [$ff02]
    bit 7, a
    jr nz, Draw
    ld a, [$c006]
    and a
    jr z, Prepare
    xor a
    ld [$c006], a
    ldh a, [$ff01]
    ld [$c000], a
    ld hl, $c001
    inc [hl]
Prepare:
    ld a, b
    or $a0 + ROLE * $10
    ld [$c003], a
    ldh [$ff01], a
IF ROLE == 0
    ld a, [$c005]
    and 7
    jr nz, Draw
    ld a, $81
ELSE
    ld a, $80
ENDC
    ldh [$ff02], a
    ld a, 1
    ld [$c006], a
Draw:
    ld a, [$c000]
    ld hl, $9885
    call Hex
    ld a, [$c001]
    ld hl, $98c5
    call Hex
    ld a, [$c002]
    ld hl, $9905
    call Hex
    jp Loop
Hex:
    ld c, a
    swap a
    and $0f
    inc a
    ld [hli], a
    ld a, c
    and $0f
    inc a
    ld [hl], a
    ret
Palette:
    db $ff,$7f,$b5,$56,$4a,$29,$00,$00
Tiles:
"""
source += "\n".join("    db " + ",".join(f"${b:02x}" for b in tiles[i:i + 16]) for i in range(0, len(tiles), 16))
source += "\nTilesEnd:\n"
assembly = out / "link-test.asm"
assembly.write_text(source)
for role, extension in [(0, "gb"), (1, "gbc")]:
    obj = out / f"link-{role + 1}.o"
    rom = out / f"link-{role + 1}.{extension}"
    subprocess.run([str(rgbds / "rgbasm"), f"-DROLE={role}", "-o", str(obj), str(assembly)], check=True)
    subprocess.run([str(rgbds / "rgblink"), "-p", "0", "-o", str(rom), str(obj)], check=True)
    subprocess.run([str(rgbds / "rgbfix"), "-v", "-p", "0", "-t", f"LINK TEST {role + 1}", *(["-c"] if role else []), str(rom)], check=True)
    obj.unlink()
