#!/usr/bin/env python3
"""Apply the pinned libretro GBA link integration patches."""
from pathlib import Path
import subprocess
import sys

root = Path(sys.argv[1])
revision = "88e5f1a3cc350b3a76247f2312b78d9f85afd4b4"


def original(name):
    return subprocess.check_output(
        ["git", "-c", f"safe.directory={root}", "-C", str(root), "show", f"{revision}:{name}"],
        text=True,
    )


def replace(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError(f"Expected one patch anchor: {old[:100]}")
    return text.replace(old, new)


name = "src/platform/libretro-splitscreen/instances.h"
s = original(name)
s = replace(s, "/* Deinit everything", """bool sp_load_roms(struct sp_manager* sp, int nPlayers,
                  const struct retro_game_info* games, struct sp_video* vid);
void sp_set_link(struct sp_manager* sp, bool enabled);

/* Deinit everything""")
(root / name).write_text(s)

name = "src/platform/libretro-splitscreen/instances.c"
s = original(name)
s = replace(s, '#include <mgba/internal/gba/memory.h>', """#include <mgba/internal/gba/memory.h>
#include <mgba/internal/arm/macros.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/serialize.h>""")
s = replace(s, "\tp->core->init(p->core);", """\tp->core->init(p->core);
\t/* A host snapshot supplies the epoch; emulated frames advance the RTC. */
\tp->core->rtc.override = RTC_FAKE_EPOCH;
\tp->core->rtc.value = 946684800000LL;
\t((struct GBA*) p->core->board)->idleOptimization = IDLE_LOOP_IGNORE;""")
s = replace(s, """bool sp_load(struct sp_manager* sp, int nPlayers, const void* rom, size_t romSize,
             struct sp_video* vid) {""", """bool sp_load_roms(struct sp_manager* sp, int nPlayers,
                  const struct retro_game_info* games, struct sp_video* vid) {""")
s = replace(s, "|| !rom || !romSize", "|| !games")
s = replace(s, "\n\t\tGBASIOLockstepCoordinatorAttach(&sp->coordinator, &sp->players[i].driver);", """\n\t\t/* Detaching renumbers survivors; a new cable keeps cartridge slot order. */
\t\tsp->players[i].playerId = i;
\t\tGBASIOLockstepCoordinatorAttach(&sp->coordinator, &sp->players[i].driver);""")
s = replace(s, "\t\tok = _spLoadOne(&sp->players[i], i, rom, romSize);", """\t\tok = games[i].data && games[i].size >= 0xC0 && games[i].size <= 0x2000000
\t\t\t&& _spLoadOne(&sp->players[i], i, games[i].data, games[i].size);""")
s = replace(s, "void sp_deinit(struct sp_manager* sp) {", """bool sp_load(struct sp_manager* sp, int nPlayers, const void* rom, size_t romSize,
             struct sp_video* vid) {
\tstruct retro_game_info games[SP_MAX_PLAYERS] = {0};
\tfor (int i = 0; i < SP_MAX_PLAYERS; ++i) {
\t\tgames[i].data = rom;
\t\tgames[i].size = romSize;
\t}
\treturn sp_load_roms(sp, nPlayers, games, vid);
}

void sp_set_link(struct sp_manager* sp, bool enabled) {
\tif (enabled == sp->linkAttached || sp->nPlayers < 2) return;
\tif (enabled) {
\t\t_spAttachLink(sp);
\t\tfor (int i = 0; i < sp->nPlayers; ++i) {
\t\t\tstruct sp_player* p = &sp->players[i];
\t\t\tp->core->setPeripheral(p->core, mPERIPH_GBA_LINK_PORT, &p->driver);
\t\t}
\t\tsp_set_fs_assist(sp, !sp->fsSuppressed);
\t} else {
\t\tfor (int i = 0; i < sp->nPlayers; ++i) {
\t\t\tstruct sp_player* p = &sp->players[i];
\t\t\tp->core->setPeripheral(p->core, mPERIPH_GBA_LINK_PORT, NULL);
\t\t\tp->asleep = false;
\t\t}
\t\tGBASIOLockstepCoordinatorDeinit(&sp->coordinator);
\t\tsp->linkAttached = false;
\t}
\tsp->stallRun = 0;
}

void sp_deinit(struct sp_manager* sp) {""")
# Reserve battery memory even before the game identifies its save hardware.
start = s.index("size_t sp_serialize_size(")
end = s.index("\nbool sp_serialize(", start)
s = s[:start] + """enum { SP_RUNTIME_WORDS = 16 };

static size_t _runtimeSize(struct sp_manager* sp) {
\treturn 8 + (size_t) sp->nPlayers * (SP_RUNTIME_WORDS * sizeof(uint32_t) + GBA_SIZE_FLASH1M);
}

static void _saveRuntime(struct sp_manager* sp, uint8_t* out) {
\tuint32_t header[2] = { sp->linkAttached, sp->stallRun };
\tmemcpy(out, header, sizeof(header));
\tout += sizeof(header);
\tfor (int i = 0; i < sp->nPlayers; ++i) {
\t\tstruct sp_player* p = &sp->players[i];
\t\tstruct GBA* gba = p->core->board;
\t\tstruct GBAudio* psg = &gba->audio.psg;
\t\t/* Write-only frequencies and pending PSG samples are absent from mGBA states. */
\t\tuint32_t fields[SP_RUNTIME_WORDS] = { p->asleep, p->lastFrameCounter, p->framesProduced,
\t\t\tgba->earlyExit | (gba->haltPending << 1) | ((gba->timing.reroot != NULL) << 2),
\t\t\tgba->video.stallMask, psg->ch1.control.frequency, psg->ch2.control.frequency,
\t\t\tpsg->ch3.rate, psg->ch1.sweep.step, psg->ch1.sample, psg->ch2.sample,
\t\t\tpsg->ch3.sample, psg->ch3.window, psg->ch4.sample, psg->ch4.nSamples,
\t\t\tpsg->ch4.samples };
\t\tmemcpy(out, fields, sizeof(fields)); out += sizeof(fields);
\t\t/* Save hardware can still be undetected when the initial SRAM is imported. */
\t\tmemcpy(out, p->saveData, GBA_SIZE_FLASH1M); out += GBA_SIZE_FLASH1M;
\t}
}

static void _loadRuntime(struct sp_manager* sp, const uint8_t* in) {
\tuint32_t header[2];
\tmemcpy(header, in, sizeof(header)); in += sizeof(header);
\tsp->stallRun = header[1];
\tfor (int i = 0; i < sp->nPlayers; ++i) {
\t\tstruct sp_player* p = &sp->players[i];
\t\tuint32_t fields[SP_RUNTIME_WORDS];
\t\tmemcpy(fields, in, sizeof(fields)); in += sizeof(fields);
\t\tp->asleep = fields[0];
\t\tp->lastFrameCounter = fields[1];
\t\tp->framesProduced = fields[2];
\t\tstruct GBA* gba = p->core->board;
\t\tgba->earlyExit = fields[3] & 1;
\t\tgba->haltPending = (fields[3] >> 1) & 1;
\t\t/* VRAM contention depends on the current scanline, including VBlank. */
\t\tgba->video.stallMask = fields[4];
\t\tstruct GBAudio* psg = &gba->audio.psg;
\t\tpsg->ch1.control.frequency = fields[5];
\t\tpsg->ch2.control.frequency = fields[6];
\t\tpsg->ch3.rate = fields[7];
\t\tpsg->ch1.sweep.step = fields[8];
\t\tpsg->ch1.sample = (int32_t) fields[9];
\t\tpsg->ch2.sample = (int32_t) fields[10];
\t\tpsg->ch3.sample = (int32_t) fields[11];
\t\tpsg->ch3.window = (int32_t) fields[12];
\t\tpsg->ch4.sample = (int32_t) fields[13];
\t\tpsg->ch4.nSamples = fields[14];
\t\tpsg->ch4.samples = fields[15];
\t\tif (!(fields[3] & 4) && gba->timing.reroot) {
\t\t\tgba->timing.root = gba->timing.reroot;
\t\t\tgba->timing.reroot = NULL;
\t\t}
\t\tmemcpy(p->saveData, in, GBA_SIZE_FLASH1M); in += GBA_SIZE_FLASH1M;
\t}
\t_spResetAudioPost();
}

size_t sp_serialize_size(struct sp_manager* sp) {
\tsize_t total = 16 + (size_t) sp->nPlayers * 4;
\tfor (int i = 0; i < sp->nPlayers; ++i) {
\t\ttotal += sp->players[i].core->stateSize(sp->players[i].core)
\t\t\t+ GBA_SIZE_FLASH1M + 65536;
\t}
\treturn total + _runtimeSize(sp);
}
""" + s[end:]
s = replace(s, "#define SP_STATE_VERSION 1", "#define SP_STATE_VERSION 3")
s = replace(s, "\tsize_t toRead = (size_t) ema;", "\tsize_t toRead = avail;")
s = replace(s, """\tsize_t headerSize = 3 * sizeof(uint32_t) + (size_t) sp->nPlayers * sizeof(uint32_t);
\tif (size < headerSize) {""", """\tif (size != sp_serialize_size(sp)) return false;
\tuint8_t* runtime = (uint8_t*) data + size - _runtimeSize(sp);
\tsize -= _runtimeSize(sp);
\tsize_t headerSize = 3 * sizeof(uint32_t) + (size_t) sp->nPlayers * sizeof(uint32_t);
\tif (size < headerSize) {""")
s = replace(s, "\t\tmemset((char*) data + offset, 0, size - offset);", """\t\tmemset((char*) data + offset, 0, size - offset);
\t\t_saveRuntime(sp, runtime);""")
s = replace(s, """bool sp_unserialize(struct sp_manager* sp, const void* data, size_t size) {""", """bool sp_unserialize(struct sp_manager* sp, const void* data, size_t size) {
\tif (size != sp_serialize_size(sp)) return false;
\tconst uint8_t* runtime = (const uint8_t*) data + size - _runtimeSize(sp);
\tuint32_t linked;
\tmemcpy(&linked, runtime, sizeof(linked));
\tif (linked > 1) return false;
\tsize -= _runtimeSize(sp);""")
s = replace(s, "\t/* NOTE: no coordinator reset here.", "\t_loadRuntime(sp, runtime);\n\t/* NOTE: no coordinator reset here.")
# Restoring a linked state after solo play must restore the drivers first.
start = s.index("bool sp_unserialize(")
body = s[start:]
body = replace(body, "\tsize_t offset = headerSize;", "\tsp_set_link(sp, linked != 0);\n\tsize_t offset = headerSize;")
body = replace(body, "\t\tbool ok = mCoreLoadStateNamed(sp->players[i].core, vfm, SAVESTATE_SAVEDATA | SAVESTATE_RTC);", """\t\tstruct mCore* core = sp->players[i].core;
\t\tstruct GBA* gba = core->board;
\t\tstruct GBASIODriver* driver = gba->sio.driver;
\t\t/* Restoring I/O must not enqueue transfers in the other console. */
\t\tgba->sio.driver = NULL;
\t\tbool ok = mCoreLoadStateNamed(core, vfm, SAVESTATE_SAVEDATA | SAVESTATE_RTC);
\t\tgba->sio.driver = driver;
\t\tif (ok) {
\t\t\tconst struct GBASerializedState* raw = (const void*) ((const char*) data + offset);
\t\t\tLOAD_16(gba->sio.rcnt, GBA_REG_RCNT, raw->io);
\t\t\tLOAD_16(gba->sio.siocnt, GBA_REG_SIOCNT, raw->io);
\t\t\tgba->memory.io[GBA_REG(RCNT)] = gba->sio.rcnt;
\t\t\tgba->memory.io[GBA_REG(SIOCNT)] = gba->sio.siocnt;
\t\t}""")
body = replace(body, "\t_loadRuntime(sp, runtime);", """\toffset = headerSize;
\tfor (int i = 0; i < sp->nPlayers; ++i) {
\t\tuint32_t len;
\t\tmemcpy(&len, (const char*) data + 12 + i * 4, sizeof(len));
\t\tstruct mCore* core = sp->players[i].core;
\t\tstruct mStateExtdata extra;
\t\tmStateExtdataInit(&extra);
\t\tstruct VFile* vfm = VFileFromConstMemory((const char*) data + offset, len);
\t\tbool ok = mCoreExtractExtdata(core, vfm, &extra) && core->loadExtraState(core, &extra);
\t\tvfm->close(vfm);
\t\tmStateExtdataDeinit(&extra);
\t\tif (!ok) return false;
\t\toffset += len;
\t}
\t_loadRuntime(sp, runtime);""")
s = s[:start] + body
(root / name).write_text(s)

name = "src/platform/libretro-splitscreen/libretro.c"
s = original(name)
s = replace(s, "static unsigned advertisedRate;", "static bool loadingSubsystem;\nstatic unsigned advertisedRate;")
s = replace(s, """\t\tenvironCallback(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &info);
\t\tlastWidth = wantW;""", """\t\t/* The frontend initializes drivers after retro_load_game_special returns. */
\t\tif (!loadingSubsystem) environCallback(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &info);
\t\tlastWidth = wantW;""")
s = replace(s, "void retro_init(void) {", """static void _coreLog(struct mLogger* logger, int category, enum mLogLevel level,
                     const char* format, va_list args) {
\tUNUSED(logger);
\tUNUSED(category);
\tif (!(level & (mLOG_FATAL | mLOG_ERROR | mLOG_WARN))) return;
\tchar message[256];
\tvsnprintf(message, sizeof(message), format, args);
\t_spLogLine(message);
}

void retro_init(void) {""")
s = replace(s, "\tlogger.log = 0;\n\t(void) logger;", "\tlogger.log = _coreLog;\n\tmLogSetDefaultLogger(&logger);")
s = replace(s, "\t{ NULL, NULL, NULL, NULL, NULL, NULL, { { NULL, NULL } }, NULL },", """\t{ "splitscreen_link", "Link cable", NULL, "Connect the emulated consoles.",
\t  NULL, NULL, { { "enabled", "Enabled" }, { "disabled", "Disabled" }, { NULL, NULL } }, "enabled" },
\t{ NULL, NULL, NULL, NULL, NULL, NULL, { { NULL, NULL } }, NULL },""")
s = replace(s, "\t\t\t\t{ NULL, NULL },", """\t\t\t\t{ "splitscreen_link", "Link cable; enabled|disabled" },
\t\t\t\t{ NULL, NULL },""")
s = replace(s, "\t_applyFsAssist();\n}", """\tvar.key = "splitscreen_link";
\tif (environCallback(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
\t\tsp_set_link(&sp, strcmp(var.value, "disabled") != 0);
\t}
\t_applyFsAssist();
}""")
start = s.index("\t/* All entries must be GBA ROMs;")
end = s.index("\n}\n\nvoid retro_unload_game", start)
s = s[:start] + """\tif (!info || num_info != (size_t) n) return false;
\t_readOptions();
\tif (!sp_load_roms(&sp, n, info, &vid)) return false;
\tpendingLoad = false;
\tmenuActive = false;
\tadvertisedRate = sp.players[0].core->audioSampleRate(sp.players[0].core);
\tloadingSubsystem = true;
\t_geometryFromPlayers(n);
\tloadingSubsystem = false;
\t_applyFsAssist();
\t_setLayoutValues(n);
\t_updateOptionVisibility();
\treturn true;""" + s[end:]
start = s.index("\t/* Fixed at first call per session")
end = s.index("\n}", start)
s = s[:start] + "\treturn sp_serialize_size(&sp);" + s[end:]
(root / name).write_text(s)

name = "src/gba/sio/lockstep.c"
s = original(name)
start = s.index("static bool GBASIOLockstepDriverLoadState(struct GBASIODriver* driver, const void* data, size_t size) {")
end = s.index("static void GBASIOLockstepDriverSaveState", start)
body = s[start:end]
body = replace(body, "\tplayer->freeList = &player->buffer[0];", """\tplayer->buffer[MAX_LOCKSTEP_EVENTS - 1].next = NULL;
\tplayer->freeList = &player->buffer[0];""")
body = replace(body, "\n\tif (player->playerId == 0) {", """\n\t/* Restored pending events must not link into the unused event pool. */
\t*lastEvent = NULL;
\n\tif (player->playerId == 0) {""")
s = s[:start] + body + s[end:]
s = replace(s, "\tSTORE_32LE(lockstep->event.when - mTimingCurrentTime(&driver->p->p->timing), 0, &state->driver.nextEvent);", """\tSTORE_32LE(mTimingIsScheduled(&driver->p->p->timing, &lockstep->event)
\t\t? lockstep->event.when - mTimingCurrentTime(&driver->p->p->timing) : 0,
\t\t0, &state->driver.nextEvent);""")
s = replace(s, "\tcoordinator->fsSuppressed = suppressed;", """\tbool changed = coordinator->fsSuppressed != suppressed;
\tcoordinator->fsSuppressed = suppressed;""")
for message in ("suppressed", "re-enabled"):
    s = replace(s, f'\t\tmLOG(GBA_SIO, WARN, "FS assist: {message} by host");',
                f'\t\tif (changed) mLOG(GBA_SIO, WARN, "FS assist: {message} by host");')
(root / name).write_text(s)

name = "src/gba/cart/gpio.c"
s = original(name)
s = replace(s, "\tSTORE_32(hw->p->sio.completeEvent.when - mTimingCurrentTime(&hw->p->timing), 0, &state->hw.sioNextEvent);", """\t/* A busy linked port is completed by its driver, not the dummy SIO event. */
\tSTORE_32(mTimingIsScheduled(&hw->p->timing, &hw->p->sio.completeEvent)
\t\t? hw->p->sio.completeEvent.when - mTimingCurrentTime(&hw->p->timing) : UINT32_MAX,
\t\t0, &state->hw.sioNextEvent);""")
(root / name).write_text(s)

name = "src/gba/io.c"
s = original(name)
s = replace(s, "\tSTORE_32(gba->bus, 0, &state->bus);", """\t/* The write-only serial send latch is sampled by the cable driver. */
\tSTORE_16(gba->memory.io[GBA_REG(SIOMLT_SEND)], GBA_REG_SIOMLT_SEND, state->io);
\tSTORE_32(gba->bus, 0, &state->bus);""")
s = replace(s, "\tLOAD_32(gba->bus, 0, &state->bus);", """\tLOAD_16(gba->memory.io[GBA_REG(SIOMLT_SEND)], GBA_REG_SIOMLT_SEND, state->io);
\tLOAD_32(gba->bus, 0, &state->bus);""")
(root / name).write_text(s)

name = "src/gba/audio.c"
s = original(name)
s = replace(s, """void GBAAudioDeserialize(struct GBAAudio* audio, const struct GBASerializedState* state) {
\tGBAudioPSGDeserialize(&audio->psg, &state->audio.psg, &state->audio.flags);
""", """void GBAAudioDeserialize(struct GBAAudio* audio, const struct GBASerializedState* state) {
""")
s = replace(s, """\tGBAIOWrite(audio->p, GBA_REG_SOUND4CNT_HI, reg & 0x7FFF);
""", """\tGBAIOWrite(audio->p, GBA_REG_SOUND4CNT_HI, reg & 0x7FFF);
\t/* Register writes advance channels; restore their exact phase afterwards. */
\tLOAD_16(audio->p->memory.io[GBA_REG(SOUNDCNT_X)], GBA_REG_SOUNDCNT_X, state->io);
\tGBAudioPSGDeserialize(&audio->psg, &state->audio.psg, &state->audio.flags);
""")
(root / name).write_text(s)

name = "CMakeLists.txt"
s = original(name)
s = replace(s, "\tadd_library(mgba_splitscreen_libretro SHARED", """\tset(SP_LIBRARY_TYPE SHARED)
\tif(EMSCRIPTEN)
\t\tset(SP_LIBRARY_TYPE STATIC)
\tendif()
\tadd_library(mgba_splitscreen_libretro ${SP_LIBRARY_TYPE}""")
(root / name).write_text(s)
print("Applied mGBA libretro link patches")
