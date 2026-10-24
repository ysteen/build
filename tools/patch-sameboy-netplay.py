"""Add deterministic link snapshots to the pinned SameBoy libretro source."""
from pathlib import Path
import subprocess
import sys

root = Path(sys.argv[1])
source = root / "libretro/libretro.c"
text = source.read_text()
if "EJS_LINK_STATE_V1" in text:
    text = subprocess.check_output(["git", "-c", f"safe.directory={root}", "-C", str(root), "show", "HEAD:libretro/libretro.c"], text=True)

text = text.replace("#include <Core/gb.h>", """#include <Core/gb.h>
#include <Core/random.h>
/* SameBoy's ordinary states omit host audio timing and held keys. Audio timing
 * also determines when the APU advances, so rollback must restore it exactly.
 * Exclude callbacks and FILE pointers from the cross-instance snapshot. */
typedef struct {
    uint8_t audio[offsetof(GB_apu_output_t, sample_callback)];
    double interference_highpass;
    uint32_t pending_cycles;
    bool keys[4][GB_KEY_MAX];
    bool joyp_accessed;
    bool joypad_is_stable;
    bool square_sweep_disable_stepping;
} ejs_link_runtime_t;
#define EJS_LINK_STATE_V1 (16 + 2 * sizeof(ejs_link_runtime_t))""")
text = text.replace("return maximum_save_size * 2;", "return maximum_save_size * 2 + EJS_LINK_STATE_V1;")
text = text.replace("    size_t offset = 0;\n\n    for (int i = 0; i < emulated_devices; i++)", """    if (size != retro_serialize_size()) return false;
    uint8_t *footer = (uint8_t *)data + size - EJS_LINK_STATE_V1;
    memset(data, 0, size);
    memcpy(footer, "LNK2", 4);
    footer[4] = bit_to_send1;
    footer[5] = bit_to_send2;
    footer[6] = emulated_devices;
    uint64_t random_state = GB_random_get_seed();
    for (unsigned n = 0; n < 8; n++) footer[8 + n] = random_state >> (n * 8);
    for (int i = 0; i < emulated_devices; i++) {
        ejs_link_runtime_t runtime = {0};
        memcpy(runtime.audio, &gameboy[i].apu_output, sizeof(runtime.audio));
        runtime.interference_highpass = gameboy[i].apu_output.interference_highpass;
        runtime.pending_cycles = gameboy[i].pending_cycles;
        memcpy(runtime.keys, gameboy[i].keys, sizeof(runtime.keys));
        runtime.joyp_accessed = gameboy[i].joyp_accessed;
        runtime.joypad_is_stable = gameboy[i].joypad_is_stable;
        runtime.square_sweep_disable_stepping = gameboy[i].apu_output.square_sweep_disable_stepping;
        memcpy(footer + 16 + i * sizeof(runtime), &runtime, sizeof(runtime));
    }
    size -= EJS_LINK_STATE_V1;
    size_t offset = 0;

    for (int i = 0; i < emulated_devices; i++)""")
text = text.replace("bool retro_unserialize(const void *data, size_t size)\n{", """bool retro_unserialize(const void *data, size_t size)
{
    if (!data || size != retro_serialize_size()) return false;
    const uint8_t *footer = (const uint8_t *)data + size - EJS_LINK_STATE_V1;
    if (memcmp(footer, "LNK2", 4) || footer[6] != emulated_devices) return false;
    bit_to_send1 = footer[4] != 0;
    bit_to_send2 = footer[5] != 0;
    uint64_t random_state = 0;
    for (unsigned n = 0; n < 8; n++) random_state |= (uint64_t)footer[8 + n] << (n * 8);
    GB_random_seed(random_state);
    size -= EJS_LINK_STATE_V1;""")
start = text.index("bool retro_unserialize")
end = text.index("void *retro_get_memory_data", start)
restore = text[start:end].replace("    return true;", """    for (int i = 0; i < emulated_devices; i++) {
        ejs_link_runtime_t runtime;
        memcpy(&runtime, footer + 16 + i * sizeof(runtime), sizeof(runtime));
        memcpy(&gameboy[i].apu_output, runtime.audio, sizeof(runtime.audio));
        gameboy[i].apu_output.interference_highpass = runtime.interference_highpass;
        gameboy[i].pending_cycles = runtime.pending_cycles;
        memcpy(gameboy[i].keys, runtime.keys, sizeof(runtime.keys));
        gameboy[i].joyp_accessed = runtime.joyp_accessed;
        gameboy[i].joypad_is_stable = runtime.joypad_is_stable;
        gameboy[i].apu_output.square_sweep_disable_stepping = runtime.square_sweep_disable_stepping;
    }
    output_audio_buffer.sizes[0] = output_audio_buffer.sizes[1] = 0;
    return true;""")
text = text[:start] + restore + text[end:]
# Dual mode omits system RAM from the stock memory API. Expose per-device
# IDs for diagnostics without changing the existing battery-save IDs.
text = text.replace("case RETRO_MEMORY_GAMEBOY_1_SRAM:", """case ((5 << 8) | RETRO_MEMORY_SYSTEM_RAM):
                data = gameboy[0].ram;
                break;
            case ((6 << 8) | RETRO_MEMORY_SYSTEM_RAM):
                data = gameboy[1].ram;
                break;
            case RETRO_MEMORY_GAMEBOY_1_SRAM:""", 1)
start = text.index("size_t retro_get_memory_size")
text = text[:start] + text[start:].replace("case RETRO_MEMORY_GAMEBOY_1_SRAM:", """case ((5 << 8) | RETRO_MEMORY_SYSTEM_RAM):
                size = gameboy[0].ram_size;
                break;
            case ((6 << 8) | RETRO_MEMORY_SYSTEM_RAM):
                size = gameboy[1].ram_size;
                break;
            case RETRO_MEMORY_GAMEBOY_1_SRAM:""", 1)
source.write_text(text)
header = root / "Core/random.h"
if "GB_random_get_seed" not in header.read_text():
    header.write_text(header.read_text() + "\nuint64_t GB_random_get_seed(void);\n")
random = root / "Core/random.c"
if "GB_random_get_seed" not in random.read_text():
    random.write_text(random.read_text() + "\nuint64_t GB_random_get_seed(void) { return seed; }\n")
