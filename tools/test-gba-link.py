#!/usr/bin/env python3
"""Exercise the real libretro core's cable, SRAM and rollback contract."""
import ctypes as c
import hashlib
from pathlib import Path
import struct
import sys


class Game(c.Structure):
    _fields_ = [("path", c.c_char_p), ("data", c.c_void_p), ("size", c.c_size_t), ("meta", c.c_char_p)]


class Variable(c.Structure):
    _fields_ = [("key", c.c_char_p), ("value", c.c_char_p)]


rom_names = [name for name in sys.argv[2:] if name != "--replay-only"]
players = len(rom_names)
assert 2 <= players <= 4
player_ids = range(players)
memory_ids = [0, 256, 257, 258][:players]
core = c.CDLL(sys.argv[1])
options = {
    b"splitscreen_players": str(players).encode(), b"splitscreen_layout": b"2x2" if players > 2 else b"2x1",
    b"splitscreen_audio": b"player 1", b"splitscreen_link": b"enabled",
    b"splitscreen_fs_assist": b"off", b"splitscreen_overlays": b"off",
    b"splitscreen_audio_low_pass": b"off", b"splitscreen_audio_limiter": b"off",
}
buttons = [0] * players
picture = None
loading = True
early_av_updates = 0
audio_frames = 0
audio_peak = 0


@c.CFUNCTYPE(c.c_bool, c.c_uint, c.c_void_p)
def environment(command, pointer):
    global early_av_updates
    if command == 32 and loading:
        early_av_updates += 1
    if command == 15:
        var = c.cast(pointer, c.POINTER(Variable)).contents
        var.value = options.get(var.key)
        return bool(var.value)
    return command in (10, 11, 16, 32, 34, 37, 0x10002)


@c.CFUNCTYPE(None, c.c_void_p, c.c_uint, c.c_uint, c.c_size_t)
def video(data, width, height, pitch):
    global picture
    picture = (c.string_at(data, pitch * height), width, height, pitch)


@c.CFUNCTYPE(c.c_size_t, c.c_void_p, c.c_size_t)
def audio(pointer, frames):
    global audio_frames, audio_peak
    audio_frames += frames
    if frames and not audio_peak:
        samples = c.cast(pointer, c.POINTER(c.c_int16))
        audio_peak = max(abs(samples[i]) for i in range(min(frames * 2, 32)))
    return frames


@c.CFUNCTYPE(None)
def poll():
    pass


@c.CFUNCTYPE(c.c_int16, c.c_uint, c.c_uint, c.c_uint, c.c_uint)
def input_state(port, device, index, key):
    return int(port < players and bool(buttons[port] & (1 << key)))


core.retro_set_environment(environment)
core.retro_set_video_refresh(video)
core.retro_set_audio_sample_batch(audio)
core.retro_set_input_poll(poll)
core.retro_set_input_state(input_state)
core.retro_load_game_special.argtypes = [c.c_uint, c.POINTER(Game), c.c_size_t]
core.retro_load_game_special.restype = c.c_bool
core.retro_serialize_size.restype = c.c_size_t
core.retro_serialize.argtypes = [c.c_void_p, c.c_size_t]
core.retro_serialize.restype = c.c_bool
core.retro_unserialize.argtypes = [c.c_void_p, c.c_size_t]
core.retro_unserialize.restype = c.c_bool
core.retro_get_memory_data.argtypes = [c.c_uint]
core.retro_get_memory_data.restype = c.c_void_p
core.retro_get_memory_size.argtypes = [c.c_uint]
core.retro_get_memory_size.restype = c.c_size_t
roms = [c.create_string_buffer(Path(name).read_bytes()) for name in rom_names]
games = (Game * players)(*[Game(None, c.addressof(rom), len(rom) - 1, None) for rom in roms])
core.retro_init()
assert core.retro_load_game_special(0x1ff + players, games, players)
loading = False
assert early_av_updates == 0, "AV info during content load initializes the audio driver twice"
size = core.retro_serialize_size()
assert 0 < size < 16 * 1024 * 1024


def save():
    buf = c.create_string_buffer(size)
    assert core.retro_serialize(buf, size)
    return buf.raw


def load(state):
    assert core.retro_unserialize(state, size)


def memory(player):
    mid = memory_ids[player]
    return c.string_at(core.retro_get_memory_data(mid), core.retro_get_memory_size(mid))


def telemetry(player):
    data = memory(player)
    return {
        "cartridge": data[2],
        "savedPresses": struct.unpack_from("<I", data, 4)[0],
        **dict(zip(["frame", "received", "keys", "peer", "wire"], struct.unpack_from("<5I", data, 16))),
    }


def run(first, end):
    for frame in range(first, end):
        buttons[:] = [(1 << [8, 7, 6, 5][p]) if frame % (17 + p * 6) < (5 + p * 4) else 0 for p in player_ids]
        core.retro_run()


def equal(a, b, label):
    if a != b:
        diffs = [i for i, (x, y) in enumerate(zip(a, b)) if x != y]
        Path("/tmp/gba-state-expected.bin").write_bytes(a)
        Path("/tmp/gba-state-actual.bin").write_bytes(b)
        raise AssertionError(f"{label}: {len(diffs)} different bytes, first offsets {diffs[:30]}")


def check_rollback(first, end):
    for start in range(first, end, 13):
        before = save()
        expected = []
        for frame in range(start, start + 13):
            run(frame, frame + 1)
            expected.append(save())
        load(before)
        equal(before, save(), f"Immediate restore at {start}")
        for index, frame in enumerate(range(start, start + 13)):
            run(frame, frame + 1)
            equal(expected[index], save(), f"Rollback frame {frame}")


if "--replay-only" in sys.argv:
    core.retro_run()
    check_rollback(1, 3600)
    core.retro_unload_game()
    core.retro_deinit()
    print(f"PASS: 3,601 exactly replayed frames ({size} byte snapshots)")
    sys.exit(0)


for player in player_ids:
    mid = memory_ids[player]
    battery_seed = bytearray(memory(player))
    battery_seed[:3] = bytes([0x47, 0x42, player + 1])
    struct.pack_into("<I", battery_seed, 4, 41 + player)
    c.memmove(core.retro_get_memory_data(mid), bytes(battery_seed), len(battery_seed))
initial = save()
for mid in memory_ids:
    c.memset(core.retro_get_memory_data(mid), 0, core.retro_get_memory_size(mid))
load(initial)
assert [telemetry(p)["savedPresses"] for p in player_ids] == [41 + p for p in player_ids]
run(0, 180)
assert audio_frames > 0 and audio_peak > 0, "The fixture must produce audible samples"
print("Cable telemetry:", [telemetry(p) for p in player_ids], flush=True)
assert [telemetry(p)["cartridge"] for p in player_ids] == [p + 1 for p in player_ids]
assert all(telemetry(p)["received"] > 100 for p in player_ids)
assert telemetry(0)["savedPresses"] > 41
assert all(telemetry(p)["savedPresses"] == 41 + p for p in range(1, players))
for p in player_ids:
    wires = struct.unpack_from("<4I", memory(p), 36)
    assert [value >> 12 for value in wires[:players]] == list(range(1, players + 1)), wires
assert picture is not None
pixels, width, height, pitch = picture
assert (width, height) == (480, 160 if players == 2 else 320)
for player in player_ids:
    for row in (40, 56, 72, 88, 104):
        visible = sum(
            struct.unpack_from("<H", pixels, (y + (player // 2) * 160) * pitch + ((player % 2) * 240 + x) * 2)[0] != 0
            for y in range(row, row + 7) for x in range(112, 160)
        )
        assert visible > 30, f"Missing visible telemetry: player {player}, row {row}"
check_rollback(180, 1200)
print("Repeated cable rollback passed", flush=True)
saved = save()
options[b"splitscreen_audio"] = b"player 2"
load(initial)
run(0, 180)
audio_independent = save()
options[b"splitscreen_audio"] = b"player 1"
load(initial)
run(0, 180)
equal(audio_independent, save(), "Per-player audio")
load(saved)
battery = [memory(p) for p in player_ids]
options[b"splitscreen_link"] = b"disabled"
run(400, 410)
assert all(telemetry(p)["frame"] > 300 for p in player_ids)
options[b"splitscreen_link"] = b"enabled"
load(saved)
equal(saved, save(), "Restore linked state after disconnection")
load(initial)
run(0, 180)
equal(audio_independent, save(), "Reconnect")
core.retro_unload_game()
assert core.retro_load_game_special(0x1ff + players, games, players)
for p in player_ids:
    c.memmove(core.retro_get_memory_data(memory_ids[p]), battery[p], len(battery[p]))
buttons[:] = [0] * players
for _ in range(30):
    core.retro_run()
assert all(telemetry(p)["savedPresses"] == struct.unpack_from("<I", battery[p], 4)[0] for p in player_ids)
core.retro_unload_game()
core.retro_deinit()
print(f"PASS: distinct ROMs, cable, rollback, audio isolation, reconnect and battery reload ({size} byte snapshots)")
