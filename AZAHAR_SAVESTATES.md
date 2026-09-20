# Azahar web save states

The libretro core already serializes 3DS CPU, kernel, memory and GPU state. These
patches make manual RomM save/load integration practical without enabling rewind
or runahead, which would repeatedly serialize hundreds of megabytes during play.

## Changes

- `retroarch-web-savestates.patch` removes the duplicate serialize-size call from
  `save_state_info`. Azahar's size query creates the entire compressed snapshot;
  the snapshot is now created once per request, not twice.
- Azahar builds export `int load_state_sync(const char* path)`. It returns `1`
  only after the core accepts the file, or `0` on failure. The old `load_state`
  merely returns its caller-supplied value and runs restoration on a later task.
- The synchronous API preflights the complete RASTATE block structure, including
  bounds, alignment, duplicate core blocks and the terminator, before calling
  the existing RetroArch deserializer. Raw core archives remain supported.
- `azahar-web-savestates.patch` checks CST magic, title ID and exact core revision,
  rejects empty/invalid Zstd payloads before deserializing, and caps web snapshot
  decompression at 512 MiB. Zstd's 64-bit content length no longer truncates on
  wasm32 before bounds checking. The file API also rejects files over 512 MiB.
- C++ exceptions are enabled during both Azahar compilation and final linking.
  Archive errors can return failure rather than aborting the wasm VM. Save-state
  status and temporary presentation suppression are restored on exception paths.
- `azahar-webgl-readback.patch` replaces WebGL's unsupported D24S8
  `readPixels(DEPTH_STENCIL, UNSIGNED_INT_24_8)` download. It packs depth into an
  RGBA8 scratch texture and recovers stencil through eight read-only bit tests.
  The original texture is never sampled while attached to the drawing FBO;
  depth/stencil writes are disabled. GPU state and pixel-pack settings are
  restored afterwards, and the bytes are reordered for the existing 3DS codec.
  The shader path only changes Emscripten D24S8 downloads, not normal draw/vertex
  processing or native builds. Scratch allocations are bounded at 64 MiB.
- Surface downloads now set pixel `PACK_ROW_LENGTH`, not upload-side
  `UNPACK_ROW_LENGTH`, and restore its previous value on every return and
  exception path. Direct desktop downloads use the same guard. This corrects
  a readback state leak without changing pixel-upload behavior.

The new synchronous API is exported only by Azahar. Its frontend must detect
`Module._load_state_sync` before calling
`Module.cwrap("load_state_sync", "number", ["string"])`. The frontend can remove
its temporary MEMFS file immediately after this call completes. Other cores
continue to use their existing state-loading paths.

## Compatibility and limitations

Save states are snapshots, not a replacement for ordinary in-game saves. Keep
normal saves as the durable backup, especially before switching core builds.
Azahar rejects states from a different title or core Git revision. DSP settings
must also match; LLE audio/modules do not support state serialization.
The revision is the source Git SHA, not a hash of local build patches. Test new
patches with newly created snapshots; states created by the earlier broken GPU
readback candidate are not a valid rendering-compatibility baseline.

Preflight failures leave deserialization unstarted. A malformed Boost archive
that fails partway through deserialization may already have replaced some core
state: the frontend must report failure, and restarting the game may be required.
This is not a transactional rollback mechanism or a promise that arbitrary
untrusted state files are safe to open.

State save/load may briefly pause a large 3DS game. Rewind and runahead remain
disabled. Enabling C++ exception handling can affect generated code size and
performance; benchmark the rebuilt artifact in the target browser.

Actual GPU-enabled browser testing confirmed an `INVALID_ENUM` when saving a
256-by-416 D24S8 surface; the other seven RGBA readbacks succeeded. The WebGL
download patch targets that demonstrated failure. The current GPU implementation
already marks restored registers, uniforms and lookup tables dirty and
invalidates interpreter caches; those paths are unchanged. A same-build game
save/load round trip, with GPU acceleration both enabled and disabled, remains
necessary to establish game-specific rendering compatibility.

## Validation

Run from this repository:

```bash
node tools/test-azahar-savestates.mjs
node tools/test-azahar-readback-state.mjs
bash tools/test-azahar-shader-decoder.sh
```

The new test compiles actual source excerpts for CST preflight, Zstd handling,
libretro entry points, async presentation cleanup, RASTATE validation and the
synchronous file wrapper. Both native ASan/UBSan and Emscripten execution cover
invalid magic/title/revision, truncation, malformed compressed data, size limits
including a greater-than-4-GiB Zstd header, exception handling, duplicate blocks,
one serialization per request, and correct synchronous success/failure results.
Heavy CPU/GPU serialization and file I/O internals are mocked in this focused
test, so it is not an end-to-end game compatibility test.

The readback-state test compiles the actual `Surface::Download` and
`DownloadWithoutFbo` functions against GL mocks in both native and web modes. It
checks the FBO, desktop direct-read, depth-helper, scaled-surface and exception
paths, requiring the requested pack row length during the read, restoration of
the caller's pack row length afterwards, and no mutation of unpack state.

`tools/tests/azahar-webgl-depth-readback.js` is a browser function accepting the
three `webgl_readback*` shader sources from the patched core. It uses a fresh,
unattached WebGL2 canvas, never an emulator's context. It checks exact packed
bytes for depth boundaries, all 256 stencil values, nonzero-origin crops and two
mip levels, then repeats readbacks to check source immutability. D16 and D24 are
included as shader tests; the production hook remains limited to D24S8.
The target Chrome passed all 18 cases (4,950 pixels, six immutability checks)
with strict equality. An initial 24-bit half-unit tie exposed implementation-
dependent GLSL `round()` behavior. The shader now uses explicit half-up rounding
with integer correction, avoiding an additional floating-point rounding step for
large odd integers. No depth tolerance was added to the tests.

Rebuild using the existing builder image and cached source tree:

```bash
docker compose -f compose.dosbox-pure.yml run --rm --no-deps \
  -e INCREMENTAL_CMAKE=1 -e BUILD_JOBS=4 \
  -v "$PWD/build.sh:/build/build.sh:ro" \
  builder bash -lc 'source /opt/emsdk/emsdk_env.sh && ./build.sh --core=azahar'
```

The new compiler flag causes C++ objects to rebuild even in incremental mode.
`BUILD_JOBS=4` limits the CMake compilation to avoid exhausting typical WSL memory.
Retain the previous core before deploying the new archive for browser testing.

## Verified candidate (2026-09-21)

The four-job incremental rebuild including the WebGL depth/stencil fix completed
successfully in 1 minute 49 seconds. The resulting archive is
`output/azahar-thread-wasm.data` (3,498,221 bytes), SHA-256
`d9b1e7b84e043776d2707f4664306fd0d397e36ce524bf676596c99f91377f51`.
Archive integrity passed, and the extracted 12,734,997-byte wasm module passed
validation and compilation. Its JavaScript `_load_state_sync` binding maps to
the actual wasm function export `Xj` in this particular minified build.

Both focused native/wasm save-state tests and the existing shader-decoder suite
passed. The prior performance patch and EmulatorJS runtime patch remain applied
alongside the three new patches. The isolated Chrome shader fixture also passed
as described above. This artifact validation does not replace the
gameplay save/load and rendering checks described above.

The subsequent PACK-row-length fix rebuilt successfully in 67 seconds. The new
archive is 3,498,208 bytes, SHA-256
`1805ec643a8df54b9121c1ea153714b4c1df6e77c0c3af65c3288a7b06996c11`.
Archive integrity and validation/compilation of its 12,735,480-byte wasm module
passed. The save-state suite, pixel-store regression and shader-decoder tests
also passed.

A fresh Chrome GPU-enabled Mario & Luigi: Dream Team title-screen round trip
measured 60.30 FPS before saving, an 8,858,800-byte snapshot in 926 ms, and a
successful synchronous restore in 659 ms. `UNPACK_ROW_LENGTH` remained zero.
The previously black lower-screen background remained blue after restoration,
and the visible character/logo colors were correct. Invalid state data was
rejected; no WebGL readback error or native state-load exception was recorded.

However, the three post-load measurement windows were 24.96, 9.58 and 42.53 FPS,
not a stable 60 FPS. The logs show repeated shader compilation after restore,
which may contribute to these stalls but was not separately profiled. Keep the
feature experimental: this title-screen result does not prove steady-state
performance, arbitrary scene compatibility or cross-build state compatibility.
