# Azahar WebAssembly performance investigation

Updated: 2026-09-23 (Asia/Seoul).

## Reused uploads and direct WebGL calls

`patches/azahar-webgl-submission.patch` adds byte-verified stream-buffer reuse for
hardware vertices, indices and three separately buffered uniform bindings. Cache
entries are invalidated on ring wrap or growth. Known WebGL calls in stream,
rasterizer and state code can call Emscripten directly, avoiding proc-address and
indirect dispatch wrappers. Asyncify, required exception handling and the ARM
interpreter remain unchanged. Ready specialized programs also bypass generic
configuration/preparation in the steady draw path.

The user's RomM state 29 was loaded normally after boot. In one visible Chrome
session, modes 0,3,0,1,2,3 each received ten seconds of warmup and fifteen fresh
samples, with no profiler, GL timing hooks or concurrent builds. Pooled medians:

| Mode | Core FPS | Core execution | Graphics-command CPU time |
| --- | ---: | ---: | ---: |
| Ordinary uploads / GLAD | 19.61 | 46.89 ms | 30.91 ms |
| Upload reuse | 20.85 | 44.35 ms | 27.82 ms |
| Direct calls | 20.00 | 45.92 ms | 29.81 ms |
| Both (default) | 21.63 | 42.64 ms | 25.88 ms |

Both features improved the measured core rate by 10.3%; the game itself remained
at 10.81 FPS and 36.1% speed. Ready-program generic bypass is common to all four
modes, so this does not separately measure that change. Reuse hit rates were
94.1% for vertex lookups, 97.7% for indices and 25.0% for uniforms, by count rather
than bytes. Seventeen async programs completed after restore, zero failed, and
152 generic draws occurred. No further generic draws, pending programs or captured
errors occurred during the steady comparison. First-use vertex/link waits remain.

Some captures show a black lower screen on both old and candidate cores; other
captures of the same modes show both screens, including the final default-mode
capture. This observation is not resolved or attributed to the optimization.
Cross-image results (cache-only 19.76 FPS, async R2 17.66 FPS) also have different
preload/cache and capture conditions and are reference measurements only.

Validation: native ASan/UBSan and Wasm stream tests; 2,048 real WebGL2 output checks
across all four modes, including reuse and orphaning; full core build, JS syntax,
Wasm validation, archive extraction, reverse-patch check, RomM Azahar runtime test
and served hash verification. `tools/test-azahar-webgl-submission.sh` creates the
isolated GPU fixture; `tools/test-azahar-hotpaths.sh` covers CPU/Wasm stream checks.
The incremental build now reverses submission, async, then cache layers before
validating underlying patches and reapplies them in dependency order.

Current image: `romm-custom-dosbox-pure:5.2.0-azahar-submit-20260923-r1`.
Served core: 3,503,007 bytes, SHA-256
`85d9558738c08ebf6f385875a78f91a1b0d4cf03d5a7d8939b9844f85b89e8b2`.
See [the Korean result](docs/AZAHAR_WEBGL_SUBMISSION.ko.md) and
[measurements](docs/azahar-webgl-submission-2026-09-23.json).

## Generic WebGL fragment fallback and asynchronous specialization

`patches/azahar-webgl-async.patch` adds a fragment-only fallback after the variant
and source-cache patch. A 144-byte uniform block describes supported PICA TEV,
texture, fog and framebuffer settings. The existing 1328-byte fragment block is
shared with the specialized generator. Unsupported effects retain synchronous
specialization. Native builds keep the existing renderer.

On `KHR_parallel_shader_compile`, at most four specialized programs are pending.
Compilation/linking does not immediately query status, logs or uniforms. Polling
checks completion at most every 8 ms; only completed programs are finalized and
persisted. Generic drawing continues while queued specializations are unfinished.
Novel vertex shaders and the generic program for each vertex shader are still
synchronous, so this implementation does not remove all first-use stalls.

The build reverses the async layer before the cache layer when checking an
incremental tree, then reapplies them in that order after the underlying patches.
`tools/test-azahar-webgl-generic.sh` checks the real GL utility with ASan/UBSan,
the generated fallback in Wasm and the 24-field telemetry ABI. Chrome passed
420 pixel comparisons across 108 fixtures, 24 depth checks and eight unsupported
configuration rejections. The prior 696 cache/variant GPU checks also passed.
The first experimental telemetry glue exceeded Emscripten's direct argument
handling; the deployed R2 build passes one pointer and reads the 24 doubles.

Game startup with the source cache disabled completed 16 async programs with no
failures and 31 generic draws, returning to approximately 60 core FPS. However,
vertex status waits still reached 9.69 seconds, generic links 1.53 seconds and a
startup frame 33.1 seconds. These runs are not a controlled before/after comparison.
A subsequent normal RomM Load Latest State succeeded after boot. An earlier
automatic startup restore timed out in DrainAsyncOperations after five seconds;
that timing issue remains unresolved. No memory capture/restore diagnostic was run.

In the user's crowded-plaza scene, 42 fresh visible samples from a one-minute
capture had median 31.90 core FPS, 15.95 game FPS, 53.3% emulation speed, 29.17 ms
core execution and 17.13 ms GPU-command processing. There were no new shader/link
calls, no pending programs and no added generic draws. No observed GL call exceeded
20 ms; bufferSubData peaked at 0.40 ms. GPU-command time includes CPU processing,
not just device execution. A separate CPU sample placed 20.4% self time in the ARM
interpreter and 2.6% inclusive time in UseFragmentShader. Wasm/JS dispatch is also
substantial. Profiling FPS is excluded from the performance result. Function names
were recovered by matching unique normalized function bodies to a separate relink,
since symbol emission changed function order. The original scene is preserved.
After all GL hooks and the CPU profiler were removed, ten fresh samples in the
same scene still had median 32.06 core FPS, 53.6% speed and 28.91 ms core execution,
with unchanged program and generic-draw counts.

Historical R2 image: `romm-custom-dosbox-pure:5.2.0-azahar-async-20260923-r2`.
Served core: 3,508,233 bytes, SHA-256
`90c3ba8a2fc87c931187224130e8f456e2986e225c433a1f01f5a775229146a4`.
See [the Korean result](../result/AZAHAR_ASYNC_SHADER.ko.md) and
[measurement data](../result/azahar-webgl-async-2026-09-23.json).

## WebGL variants and persistent source/program cache on September 23

`patches/azahar-webgl-cache.patch` implements the requested first two steps:
reduce shader variants and persist observed GLSL vertex/fragment pairs for startup
compilation/linking. The measurements in this section predate the async layer above.

On WebGL, alpha comparison and scissor mode now use two uniform integers in the
existing std140 padding at offsets 72 and 76. The fragment block remains 1328
bytes; all later offsets are unchanged. Unused TEV operands are masked out of
configuration identity and omitted from generated code. Native GLSL is unchanged.

The source cache shares the core's existing IDBFS mount and lives in
`/data/saves/Azahar/Azahar/shaders/webgl-v1/`. It stores exact generated sources
and observed VS/FS index pairs, not driver binaries. Loading validates format,
shader-cache version, accurate-multiply mode, checksum, lengths and indices.
Limits per title/multiply mode are 8 MiB, 512 shaders, 256 programs and 512 KiB
per source. Successful stages and linked programs seed the existing in-memory
caches; bad cache files fall back to normal compilation. New pairs are written
through a temporary file and renamed, with the existing IDBFS auto-persist hooks
handling persistence. RomM's `sdmc` save bundle excludes these shader files.

The build applies this patch after the existing performance, state and readback
patches. Incremental builds first reverse this patch on a matching source tree
before validating the underlying patches, then apply it again.

Validation passed native ASan/UBSan and Wasm cache tests, 696 Chrome WebGL2 output
checks, native output equivalence for 480 configurations, full core compilation,
archive/Wasm/JS checks and deployed artifact hashing. The synthetic configuration
matrix produced 358 unique old sources and 10 new sources. Of the GPU checks,
87 Never cases use the specified untouched framebuffer as their reference because
the old unconditional-discard shader is rejected at draw time by this ANGLE device.

A repeat launch loaded 53 observed pairs (318.5 ms at initial startup and 251.5 ms
when state loading reconstructed the renderer), with zero new fragment compiles
and zero new program links in the observed gameplay. A separate 21–22 second
startup/state-restore long task remained; its exact blocking call is unresolved.
Browser/driver caches also contribute, so this is not an isolated timing comparison
of the source-cache patch alone.

In the user's subsequent marked transition, 27 unseen fragment shaders/programs
were needed. A 41.901-second long task included 21.849 seconds in shader-status
queries and 19.933 seconds in program-status queries. Uploads were at most 1.66 ms
per call and draws at most 0.05 ms. The cache grew from 53 to 80 pairs (710,893
bytes). First-use stalls therefore remain; reuse is working. The user also reported
that reused scenes transition quickly. The requested 60-second observation ended
after 66.015 seconds because its stop timer waited behind the blocking task.

A separate memory-state diagnostic failed in its temporary observer cleanup and
the user reported a browser failure. No new Chrome crash dump was found; neither
the state round trip nor the crash cause was established. That diagnostic was
disabled. The final user-requested timing capture did not capture or restore states.

Reproduce the bounded source/cache tests with
`bash tools/test-azahar-webgl-cache.sh`. It creates before/after fixtures from the
actual generator, checks native equivalence, and writes `verify-fragments.js` for
evaluation in an idle WebGL2 test page. The temporary baseline is made by reversing
only this patch on copies; it never edits the working core.

See [the Korean usage and results](../result/AZAHAR_SHADER_CACHE.ko.md),
[deployment and measurement data](../result/azahar-webgl-cache-2026-09-23.json), and
[the marked transition capture](../result/azahar-transition-capture-2026-09-23.json).

## Map-transition stalls observed on September 22

During a user-triggered map transition in ROM 17, timing the existing WebGL calls
recorded 18 calls exceeding 50 ms. The expensive calls were shader compile-status
and program link-status queries: `getShaderParameter` took 1.55-2.59 seconds and
`getProgramParameter` took 1.59-5.44 seconds. Together these queries blocked for
42.93 seconds across successive frames, including one 26.845-second core frame.
Program-cache entries increased from 25 to 36 during that interval. Buffer
uploads and draws were below 3.3 ms per call in the same capture. This directly
locates the long pause in synchronous shader preparation for the new scene.

A background native build was running during the detailed timing capture and
was paused to remove that CPU load. These times are diagnostic evidence, not a
clean performance benchmark. Before that build started, the user's same session
already contained an 11.127-second long task. After shader preparation, execution
resumed at about 50.5 core FPS in a sampled window. This does not establish a
stable frame rate for the new map.

The separate Save & Quit failure was a pending `GL_INVALID_VALUE` from the shader
cache's zero-length program-binary-format query, later observed by GPU state
readback. See `AZAHAR_SAVESTATES.md` for the reproducer and fix. That correction
does not make shader compilation asynchronous or eliminate map-transition stalls.
Avoid repeatedly restarting during shader preparation: restarting discards the
current in-memory shader/program cache. The WebGL hardware-draw option remains
experimental; disabling it uses the slower CPU vertex path measured below.

The read-only timing capture is recorded in
`../result/azahar-stall-save-diagnosis-2026-09-22.json`.

## WebGL shader compilation improvement on September 22

The WebGL PICA shader generator now emits explicit transitions between jump
dispatcher cases. Previously, normal execution fell through to the next case.
ANGLE's [switch fall-through removal pass](https://chromium.googlesource.com/angle/angle/+/9d737966acdf628a7cb4e12e4781179ea3be5d7b/src/compiler/translator/tree_ops/RemoveSwitchFallThrough.cpp)
copies the remaining case bodies for each fall-through chain. Long dispatchers
therefore produce much more translated code and expensive driver compilation.
Explicit transitions avoid that duplication. The last block returns from the
subroutine, including when it contains only a NOP. Native GLSL is unchanged.

A paired synthetic test used the actual generated GLSL for 16 chained conditional
jumps, with distinct source identifiers to avoid identical-source cache hits.
It ran on the same Chrome/ANGLE device without a concurrent native build.

| Measurement | Before | After |
| --- | ---: | ---: |
| Vertex compile and program link | 1,537.7 ms | 79.9 ms |
| First 32 draws, including format specialization and readback | 1,665.2 ms | 188.5 ms |
| Warm 100,000-vertex draw and readback, no branches taken, median of 3 | 0.5 ms | 0.5 ms |
| Warm 100,000-vertex draw and readback, all branches taken, median of 3 | 0.6 ms | 0.6 ms |
| ANGLE non-empty fall-through warnings | 16 | 0 |

The compile/link reduction is approximately 94.8% for this synthetic shader. It
is not a measured map-transition improvement. Browser/driver caches were not
cleared, and the warm timings include synchronization rather than isolated GPU
timer queries. A larger 48-block baseline failed to finish linking successfully
and is excluded from speedup calculations. The modified 48-block fixture passed.

Validation uses the real PICA interpreter as the reference: native ASan/UBSan
and Wasm outputs match, and WebGL transform feedback passes all 256 cases
(eight programs, 16 uniform masks, float and packed-short attributes). Coverage
includes forward/backward jumps, calls, IF/LOOP entries, inverted conditions,
an empty final block, and the long chain. Native generated sources are identical
before and after. `tools/test-azahar-shader-jumps.sh` accepts `STRESS_BLOCKS`
(default 48) and `DECOMPILER_SOURCE` for bounded baseline comparisons.
`tools/tests/azahar-shader-jumps.js` optionally measures completed bulk draws with
`{warmVertices: 100000}`; every timed draw synchronizes through readback.

The optimization preserves hardware drawing and shader arithmetic. First-use
compilation is still synchronous, and other shaders or driver work can still
pause a new scene. The live follow-up below confirms substantial stalls remain;
the synthetic speedup must not be presented as a measured whole-game improvement.
Raw measurements and artifact validation are recorded in
`../result/azahar-shader-optimization-2026-09-22.json` and
`../result/azahar-shader-build-2026-09-22.json`.

## Live follow-up and diagnostic renderer crash

The user refreshed and reported unchanged loading. Resource timing recorded a
fresh 3,498,798-byte core download, matching the deployed artifact. A subsequent
capture of the shaders actually generated by the game contains the new explicit
jump transitions. The optimization is therefore active in gameplay. Nevertheless,
the first observed session still recorded a 37.102-second long task.

At 14:53:32 UTC on September 22, Chrome 153.0.8010.53's renderer crashed with
`0xc0000005` at `chrome.dll+0x70ab317`. The crash followed immediately after a
diagnostic `Runtime.queryObjects` request for `WebGLShader.prototype`. The request
timed out and returned no shader list. That heap enumeration is the leading
suspected trigger, not a proven application fault. The stack has not been
symbolized, so correlation does not establish the exact Chrome defect. Do not
repeat heap enumeration on this game renderer. The temporary collector was
disabled. Crash-dump contents stay local; only selected metadata is retained in
the report.

After the user authorized further non-production testing, the tab was recovered
and the existing game was launched again. A bounded observer recorded newly
created shader sources and ordinary compile/link calls without enumerating the
heap. It captured 41 shaders and 36 programs and restored its hooks afterward.
Shader-status queries accumulated 30.149 seconds, and program-status queries
7.154 seconds. The longest single queries were 3.362 and 1.167 seconds,
respectively. A 31.778-second long task and a later 5.777-second task remained.
These synchronous queries also wait for queued GPU work; their durations do not
isolate compilation of the named shader alone. The first four recorded draws
per program returned quickly, which does not exclude later driver specialization.

After preparation, gameplay resumed around 59-60 core FPS in sampled windows,
and a screenshot showed the battle scene rendering. No additional crash dump
appeared in this verification period. This limited run does not establish that
the core or browser is free of defects. A separate actual-shader compile/link
probe measured 1.113 seconds with immediate stage checks and 0.968 seconds with
checks deferred until linking. One sample per mode is insufficient to justify a
whole-game speedup claim or another deployment.

The test image remains `romm-custom-dosbox-pure:5.2.0-azahar-shader-20260922`.
This follow-up changes diagnostics and documentation, not the deployed core.
See `../result/azahar-shader-followup-2026-09-22.json` and
`../result/azahar-real-shader-trace-2026-09-23.json` for the evidence.

## Scope and September 21 baseline

Testing uses only `romm-test` at `http://127.0.0.1:8081`, with Mario & Luigi:
Dream Team (ROM 17), Chrome 153, ANGLE D3D11 and Radeon 680M. Native resolution,
Old 3DS, CPU scale 100% and HLE audio remain unchanged. VSync experiments restore
the original enabled setting. The user subsequently authorized reloads and game
inputs, explicitly approved temporary hardware-draw experiments, and then chose
to leave GPU acceleration enabled in the current test session after the signed
jump-dispatch fix below. The core's default remains disabled; no global or
production setting was changed. No saves were deleted and no passwords changed.

The source trees already contained extensive ARM, renderer, input, telemetry and
RetroArch changes. Those were preserved. The performance patch includes that
pre-existing work so the build remains reproducible; not all patch lines are new
optimizations from this investigation.

Previously browser-verified performance core: `output/azahar-thread-wasm.data`.

- Build start: `2026-09-20T17:42:07+00:00`.
- SHA-256: `c7bf4b725b7e987aeb076a00d6e2d9f7e4e716a7422b81a37f9c6cb59c989c9b`.
- Live interpreter marker: `operand-fastpaths-v1`.
- Captured live hardware vertex shaders use signed `int jmp_to`; the interpreter
  marker alone does not distinguish this build from its predecessor.
- Copied to the RomM custom-core directory and the 8081 test container, with its
  matching report JSON. No production container was started or updated.
- The original core, report and source snapshot are retained temporarily at
  `/tmp/azahar-original-8I41wT`. Original core SHA-256:
  `ce90eb5dcf7531e2353eb203ee5632be66e964fd7800215aa0fd1e43b1add8d9`.

## Changes in this iteration

1. Replace the shader interpreter's three per-vertex heap-allocated circular
   stacks with inline fixed-capacity stacks. Preserve overwrite-oldest overflow,
   mutable top entries and LIFO behavior.
2. Add a compact lookup hint to the existing fully associative 64-entry FIFO
   vertex cache. Hash collisions and stale hints fall back to the exact resident
   search. Cache hits, replacement order and transformed vertex values are not
   approximated.
3. Use smaller initial WebGL streaming buffers: vertices 2 MiB, indices 256 KiB
   and uniforms 64 KiB instead of 16 MiB, 2 MiB and 8 MiB. Preserve accelerated
   draw eligibility limits and grow buffers when needed. Existing orphan-on-wrap
   behavior is retained. Buffer growth now correctly reports invalidation to
   uniform upload callers, without allocating storage twice.
4. Correct presentation channel conversion: cached render targets and LCD fill
   colors are already in host channel order. Apply BGR/ABGR conversion only to
   raw framebuffer uploads. Track the selection per screen, including independent
   left/right stereo inputs. This fixes the observed red/blue inversion without
   undoing the streaming changes.
5. Make RetroArch link failures terminate `build.sh` instead of continuing to
   package an older artifact. Add repeatable capture, analysis and regression
   test tools.
6. Emit signed jump-dispatch variables and matching literals in Wasm-generated
   GLSL. This avoids a reproducible ANGLE/D3D11 draw-time compiler failure and
   makes the experimental hardware vertex path usable in the measured scene.
   Native GLSL output and PICA control-flow semantics are unchanged.

Native OpenGL buffer sizes and mapping paths are unchanged. Full Asyncify,
proxy-to-pthread, higher CPU clocks, fast-forward and lower visual quality were
not enabled to obtain the improvement.

## Measurements

Core FPS counts emulation frames; game FPS counts submitted game frames. This
title normally submits about one game frame per two emulation frames, so roughly
60 core FPS / 30 game FPS corresponds to normal speed, not 2x fast-forward.

| Original core, title screen | VSync on | VSync off |
| --- | ---: | ---: |
| Core FPS | 16.55 | 16.58 |
| Game FPS | 8.27 | 8.28 |
| Core execution per frame | 14.89 ms | 59.86 ms |
| Core GPU processing per frame | 8.10 ms | 53.27 ms |

Both captures were approximately 30 seconds. Turning VSync off did not improve
throughput: waiting moved into WebGL calls. The off capture attributed 23.10 of
31.18 CPU-profile seconds to `bufferSubData`. A separate instrumented capture
found about 9.97 seconds in array-buffer uploads and 3.23 seconds in uniform
uploads. These synchronous call times include waiting; they are not measurements
of GPU execution time alone.

The first optimized capture averaged 57.15 core FPS / 28.06 game FPS, with 76
audio-underrun events over 29.67 seconds. It began at the title screen, but the
shader count changed from 48 to 60 and vertex load increased during the capture.
Treat it as a mixed-scene result, not a controlled same-scene speedup ratio.
Its emulation-speed windows were approximately 0.83x to 1.02x.

The final color-corrected capture averaged 58.94 core FPS / 25.50 game FPS,
with 16 audio-underrun events over 29.58 seconds. Fast-forward and slow-motion
were both disabled, with VSync enabled. The screenshot confirms the red Mario
logo and blue background are restored. This capture began during the title intro
and shader counts grew from 44 to 75, so it is also a startup/mixed-scene capture,
not a controlled same-scene speedup ratio.

The final upload trace verifies that the smaller rings were actually loaded:
array allocations peaked at 2,097,152 bytes and uniforms at 65,536 bytes. Total
`bufferSubData` call time was 0.306 seconds for array buffers and 0.172 seconds
for uniforms across the approximately 30-second capture, with no observed call
over 0.575 ms. This is a substantial reduction from the original upload trace,
despite processing more emulation frames; it does not isolate each optimization's
individual contribution.

The analyzer excludes hidden, stale and duplicate telemetry windows. FPS is
time-weighted; per-frame costs are frame-weighted. CPU samples and optional
upload counters cover their whole capture, including any VSync warmup, and are
reported separately. Temporary evidence directories:

- `/tmp/azahar-baseline-title`: original VSync-on baseline.
- `/tmp/azahar-vsync-off`: original VSync-off experiment, restored afterward.
- `/tmp/azahar-upload-targets-3`: original per-target upload timing.
- `/tmp/azahar-candidate-title`: first optimized, mixed-scene capture.
- `/tmp/azahar-color-fixed-title`: final color-corrected capture.
- `/tmp/azahar-color-tests`: isolated WebGL shader tests.

## Gameplay follow-up: instruction decode cache

The user reported about 48 FPS during gameplay. A new 30-second capture of the
opening airship deck, `/tmp/azahar-gameplay-before-2`, averaged 41.15 core FPS /
20.53 game FPS, with 22.57 ms of core execution and 16.23 ms of emulated GPU
processing per frame. The screenshot's instantaneous overlay was about 47 FPS.
The scene is substantially heavier than the earlier title-screen captures;
these are not interchangeable baselines.

Software vertex processing was active, at about 0.5 to 0.62 million shader
invocations per second. Vertex loading took roughly 524 to 638 ms per second.
`bufferSubData` accounted for only 0.505 seconds of the 30.88-second CPU profile.
This points to CPU vertex/shader work, not the old upload stall, as the next
optimization target. Emulated GPU timing is not a direct measurement of physical
GPU execution. Audio still underruns: 486 events over 29.76 seconds in this run.

The candidate caches decoded PICA instruction metadata, register locations and
swizzles, and skips the unused second operand for unary operations. It still
reads current uniforms and per-vertex registers on every execution. Arithmetic,
control flow, precision, render resolution, color conversion, clocks, VSync and
speed controls are unchanged. The cache is enabled only for the Wasm interpreter;
the original interpreter remains the native/debug path and test reference.

Each shader setup owns an independent, lazily populated cache. Program updates,
swizzle updates, fixups and archive loads invalidate it. Copies start with an
empty cache, preserving the existing shader disk-cache copy workflow. The cache
is transient and adds no serialized fields. Generation wrap is handled explicitly.

Validation in `/tmp/azahar-shader-decoder-1JyuCL` passed in native ASan/UBSan and
actual WebAssembly: 20,000 randomized shader comparisons plus control flow,
masked writes, relative uniforms, NaN/infinity/signed zero, bulk updates,
archive-load invalidation, generation wrap, independent copies and geometry
SETEMIT/EMIT output and winding. The archive test exercises the real field list
and load hook, not the frontend save-file format or an in-game save round trip.
The standalone harness uses a 1 MiB checked Wasm stack because its large local
shader fixtures exceed Emscripten's default 64 KiB; the game core already uses
a 4 MiB stack and its stack setting was not changed.

Wasm microbenchmarks execute 100,000 vertices through dependent 64-instruction
chains plus output/END, median of five, with matching checksums:

| Shader workload | Original interpreter | Decode cache | Time reduction |
| --- | ---: | ---: | ---: |
| Mixed dot/multiply/add/move | 79.94 ms | 62.57 ms | 21.7% |
| Multiply-add | 88.82 ms | 72.21 ms | 18.7% |
| Unary move/floor/reciprocal/root | 99.18 ms | 68.73 ms | 30.7% |

These are component measurements, not game-FPS gains. A separate explicit-SIMD
experiment was accurate but slower (79.12 vs 80.56 ms on the earlier synthetic
workload), so it was removed. The final scalar cache still compiles with the
existing `-msimd128` compiler option.

The candidate passed the full Docker/Emscripten build, archive integrity and
patch consistency checks. It was copied only to the custom-core directory and
8081 test container, with the matching report:

- Build start: `2026-09-20T16:21:27+00:00`.
- SHA-256: `f23ef071f5fcc5fe0f63b162320cb45700f5fcb45876f037fc9fc88e4b52468b`.
- Existing warnings listed below remain; no new build error remains.

At this stage, same-scene browser verification awaited a restart; the follow-up
below supersedes that status. The earlier browser-verified source/core/report
backup is `/tmp/azahar-gameplay-base-ENszGm`, SHA-256
`72ad74d585842475ac22bd3dbe73e6874b604d94e727a48dce27a53f6d0b02d4`.

## Gameplay follow-up: operand fast paths

The current candidate specializes identity and broadcast source swizzles and
full four-component destination masks. It hoists the decode-cache program/version
lookup out of the instruction loop and omits unused diagnostic maxima from the
Wasm decoded path. Generic swizzles, partial writes, relative uniform addressing,
NaN/signed-zero behavior and native/debug arithmetic remain covered by the
reference comparison. A branchless sign-bit variant and extra single-component
destination specializations were slower and were removed.

The expanded native ASan/UBSan and actual Wasm suite passed in
`/tmp/azahar-shader-decoder-lfxFnR`. The cached-interpreter baseline ran the same
suite in `/tmp/azahar-shader-decoder-jLL0Cg`. Both include 20,000 randomized
comparisons, numerical edge cases, cache invalidation and geometry-shader output.
The same 100,000-vertex, 64-instruction, median-of-five Wasm benchmark measured:

| Workload | Previous decode cache | Operand fast paths | Time reduction |
| --- | ---: | ---: | ---: |
| Mixed | 62.984 ms | 59.158 ms | 6.1% |
| Multiply-add | 76.643 ms | 65.999 ms | 13.9% |
| Unary | 72.023 ms | 68.849 ms | 4.4% |
| Swizzled sources | 65.977 ms | 63.068 ms | 4.4% |
| Masked destinations | 64.774 ms | 61.296 ms | 5.4% |
| Relative uniforms | 65.488 ms | 59.673 ms | 8.9% |

These are component gains, not measured gameplay gains. The full core build,
archive check and performance-patch consistency checks passed. Its core and
matching report were installed only in the custom-core directory and `romm-test`.
The previous candidate and source/report backup are retained at
`/tmp/azahar-gameplay-specialize-base-6RhYoN` (core SHA-256 `f23ef071...468b`).

After a normal game restart, the live marker confirmed that the new interpreter
was active. A roughly 30-second capture at
`/tmp/azahar-gameplay-specialize-after-light-1` averaged 46.23 core FPS; the more
stable 20-second hardware-draw baseline below averaged 47.93. Both had normal
character/map rendering. Neither establishes a controlled old/new speedup, and
neither reaches 60 core FPS.

Temporary emulator-state restores on the previous core caused missing character
and map rendering. Consequently, captures
`/tmp/azahar-gameplay-specialize-restored-before` and
`/tmp/azahar-gameplay-specialize-before-light` are excluded from normal-scene
comparisons. The temporary snapshots were not restored into the new core. Normal
in-game saved progress was used to resume play; persistent saves were not deleted
or overwritten by the diagnostic restore operation.

## Initial GPU hardware-draw comparison (before the jump-dispatch fix)

With explicit user approval, `citra_use_webgl_hw_draw` was temporarily enabled
on the operand-fast-path candidate, in the same visible gameplay scene. Each phase sampled
telemetry for approximately 20 seconds with CPU profiling disabled to reduce
measurement overhead. VSync, clocks and resolution were unchanged.

| Phase | Core FPS | Result |
| --- | ---: | --- |
| Hardware draws disabled | 47.93 | Normal scene; audio still underruns |
| Hardware draws enabled | 2.21 | Repeated long stalls; settled near 0.8 FPS |
| Disabled again | 48.11 | Normal characters, terrain, colors and minimap restored |

The enabled phase includes the transition/initial shader work. Only eight unique,
fresh telemetry windows were usable because counters updated slowly; duplicate
or stale samples are excluded. Later windows repeatedly contained approximately
2.4-second frame gaps. About 99% of attempted draws and 96% of attempted vertices
used the accelerated path, so the option did move work off the software vertex
interpreter. However, normal core callbacks then took only about 6 ms while the
average interval between callbacks exceeded 1.2 seconds. Program count stayed at
95 and fragment configuration misses at 76 throughout the samples.

These initial counters pointed to expensive browser/GPU/presentation work outside
the measured core callback, not simply a lack of accelerated draws. They did not
identify the exact ANGLE/driver stall. The subsequent trace and shader isolation
below established a draw-time compiler failure that the stable program count
could not detect. Shader prewarming alone was not a demonstrated fix.
The end-of-experiment screenshot briefly showed missing terrain/map; its timing
may overlap the automatic restoration watchdog and is not a clean enabled-only
rendering sample. The separate restored capture confirms normal rendering.

The original disabled option was read back after restoration, and fresh PICA
telemetry reported zero hardware draws. An old `__AZAHAR_GPU_DRAWS__` object can
remain after disabling; always check its timestamp and fresh PICA counters.
Restored audio still had 226 underruns over 19.74 seconds: the low underrun count
during the near-frozen enabled phase is not an audio improvement. The
experimental GPU path was left disabled pending the investigation below.

Evidence directories: `/tmp/azahar-webgl-draw-off-baseline`,
`/tmp/azahar-webgl-draw-on-trial`, `/tmp/azahar-webgl-draw-off-restored`.
The unrecorded earlier airship-crash slowdown cannot be reconstructed from these
captures; the live telemetry globals retain only their most recent window.

## GPU follow-up: signed jump dispatch

The old path repeatedly logged `GL_INVALID_OPERATION` from
`Context11::triggerDrawCallProgramRecompilation` and an error compiling the dynamic
vertex executable. Captured program logs exposed the D3D compiler diagnostic
`internal error: l-value expected`, followed by unsuccessful retries with different
compiler flags. A 12-second trace attributed approximately 11.69 seconds to GPU
process command-buffer/WebGL processing, while renderer JavaScript occupied only
about 0.12 seconds. ANGLE performs additional vertex executable specialization at
draw time; a stable WebGL program count therefore does not rule out these retries.
See the primary [ANGLE Context11 implementation](https://chromium.googlesource.com/angle/angle/%2B/cdecd97ceefa28409793d04caf873e7b0d0d723c/src/libANGLE/renderer/d3d/d3d11/Context11.cpp).

Isolated copies of the captured game shader reproduced the failure with both
float and packed-short attributes. Changing only `uint jmp_to` and its unsigned
assignment/case literals to signed integers allowed the draw to succeed. Removing
switch fallthrough or splitting loop increments did not fix the failure and those
variants were not applied. PICA instruction offsets fit in a signed integer, so
the applied change does not alter their values or arithmetic. All four emission
sites are covered, including synthetic jumps across IF/LOOP boundaries. The
existing shader-cache version already hashes the decompiler source; no manual
cache deletion was necessary.

Six synthetic PICA control-flow fixtures cover forward/backward jumps, subroutine
jumps, jumps into IF blocks, jumps within loops and inverted uniform conditions.
Native ASan/UBSan and actual Wasm runs generate matching software-interpreter
reference results. A separate Chrome WebGL2 context checks transform-feedback
results against these references: six fixtures, four boolean masks and two vertex
attribute layouts, **48 cases passed**. Native output retains unsigned dispatch.
No proprietary game shader is embedded in the regression tests.

### Same-scene browser measurements

The user resumed normal gameplay in the opening airship plaza. The new core was
tested with VSync on, native resolution, CPU scale 100%, and fast-forward and
slow-motion off. No game inputs were sent during the comparisons. CPU profiling
was disabled to reduce overhead. The enabled screenshot confirms Mario, terrain,
red/blue colors and the minimap; the option read back as enabled both immediately
before and after the screenshot.

| Phase | Core FPS | Game FPS | Core execution/frame | Audio underruns / observed time |
| --- | ---: | ---: | ---: | ---: |
| Hardware draws off, initial baseline | 46.63 | 23.28 | 19.17 ms | 244 / 19.65 s |
| Hardware draws on, shader cache warm | 59.75 | 29.84 | 6.45 ms | 0 / 59.82 s |
| Hardware draws off, restored comparison | 41.24 | 20.61 | 21.80 ms | 299 / 19.65 s |
| Hardware draws on, final session setting | 60.12 | 30.04 | 8.09 ms | 0 / 59.80 s |

The first enabled capture contains 60 samples and 58 unique fresh visible windows;
its first windows include the off-to-on transition, making the reported mean
conservative. Every sample reads the runtime option as enabled. Once settled,
about 99.4% of draws use the hardware path and the program count stays at 139.
No new dynamic-vertex compilation error, Wasm abort or JavaScript exception was
observed in the separate five-second enabled console capture. Historical console
entries include warnings produced by inspecting already-deleted diagnostic shader
handles; these are not game draw failures. The off baselines vary, so these results
establish the roughly 60 FPS outcome in this scene, not a universal speedup ratio.
The final enabled run has 60 unique fresh visible windows, all with the option
enabled, and a one-second-window range of 58.76 to 61.00 core FPS. Its longest
reported frame is 49.11 ms, so the mean is not a claim of perfect frame pacing.
The program count remains 139 and audio underruns do not increase. Both enabled
one-minute runs have zero new underruns; the intervening off periods do not.

Cold shader preparation remains expensive. The first fixed-core activation
exceeded the CDP command timeout; another transition contained a 23.97-second
telemetry window. That capture later crossed its automatic restoration deadline,
so `/tmp/azahar-signed-jump-on-steady` is a **mixed warmup/restored capture**, not
the steady-state benchmark. The later one-minute capture above remained enabled
throughout. New shader/input-layout combinations can still stall gameplay, and
cache survival across browser/game restarts has not been established.

The comparison script restored the original disabled option. After the user
explicitly chose to keep acceleration on, it was enabled again for the current
test session without a restoration timer. Temporary shader-capture hooks were
removed. The core-wide default, persistent frontend settings and production
deployment remain unchanged.

Evidence and recovery paths:

- `/tmp/azahar-gpu-stall-initial`, `/tmp/azahar-gpu-stall-trace`,
  `/tmp/azahar-gpu-programs.json`: original logs, trace and compiler diagnostics.
- `/tmp/azahar-gpu-isolated-dispatch.json`: controlled shader variants.
- `/tmp/azahar-shader-jumps-tvv89A`, `/tmp/azahar-shader-jumps-browser`:
  native/Wasm fixtures and 48 browser checks.
- `/tmp/azahar-signed-jump-off-steady`, `/tmp/azahar-signed-jump-on-verified`,
  `/tmp/azahar-signed-jump-off-restored`, `/tmp/azahar-signed-jump-on-final`:
  comparison telemetry and final enabled-session confirmation.
- `/tmp/azahar-signed-jump-on-screen.json` and its PNG/readback files:
  enabled-only rendering evidence.
- `/tmp/azahar-signed-jump-live-sources.json`: signed dispatch in the live core.
- `/tmp/azahar-signed-jump-base-MB2cSA`: previous core/report/performance patch;
  previous core SHA-256 `b5524ceb25979039644361f42f9d458796ba183442b7233bb1332f3c43d42c4b`.

## Verification

- Complete Docker/Emscripten build and archive integrity check passed.
- `git diff --check`, shell syntax checks and reverse application checks for the
  generated performance patch passed.
- Native ASan/UBSan and actual WebAssembly tests passed: 300,000 stack operations
  and 1,000,000 vertex-cache lookups matched the original implementations.
- Tests compiling the real WebGL stream-buffer implementation passed for
  alignment, partial/empty uploads, wrap, growth and invalidation, for all four
  buffer targets, in native sanitizers and WebAssembly.
- Three presentation shaders compiled in a separate Chrome WebGL2 context and
  passed 30 host-RGBA, guest-BGR/ABGR and independent stereo channel-order cases.
- Signed jump-dispatch fixtures passed native ASan/UBSan and actual Wasm checks,
  plus 48 transform-feedback comparisons in Chrome WebGL2. Full core rebuild,
  archive integrity, deployed hashes and performance-patch reverse checks passed.
- The capture tool's PowerShell parse and mocked hardware-draw restoration,
  readback mismatch, retry and disconnected-CDP checks passed. Restoration now
  precedes potentially slow screenshots and trace downloads.
- The earlier color-fix browser capture found no JavaScript exceptions, Wasm aborts or
  WebGL invalid-operation messages, and no failed requests during its five-second
  observation window. Historical console messages included optional localization
  and core-report parsing warnings.

Representative Wasm microbenchmarks (`-O3 -flto=thin -pthread -msimd128`, median
of five) measured stack work at 52.96 -> 8.74 ms, and cache workloads at
1.2x to 6.5x faster. These are isolated component tests, not game-FPS multipliers.
Results: `/tmp/azahar-hotpaths-oUHWTr/results.txt`; the expanded stream suite also
passed at `/tmp/azahar-hotpaths-wqqOEZ`.

## Reproduce

From this build directory:

```bash
bash tools/test-azahar-hotpaths.sh
bash tools/test-azahar-shader-decoder.sh
bash tools/test-azahar-shader-jumps.sh
docker compose -f compose.dosbox-pure.yml run --rm --no-deps \
  -e INCREMENTAL_CMAKE=1 \
  -v "$PWD/build.sh:/build/build.sh:ro" \
  builder bash -lc 'source /opt/emsdk/emsdk_env.sh && ./build.sh --core=azahar'
node tools/analyze-azahar-profile.mjs /tmp/azahar-baseline-title /tmp/azahar-color-fixed-title
```

In Windows PowerShell, use `tools/profile-azahar.ps1` with `-OutputDir` and
`-DurationSeconds 30`. Options include `-Screenshot`, `-Trace`, `-TraceUploads`,
`-InspectOnly`, `-VerifyPresentShaders`, `-VerifyJumpFixtures <wasm.json>` and
`-NoCpuProfile`. The last option
collects telemetry without a CPU profiler; `capture.json` records this, and the
analyzer reports a null profile duration rather than expecting a profile file.
`-VSync disabled` and `-WebGLHwDraw enabled` temporarily change runtime settings;
obtain approval before using either. Original values are restored in `finally`.
Hardware-draw experiments additionally use an in-page timeout and attempt
restoration before screenshots/trace downloads, which may wait on the GPU.
`restored-option.txt` records the readback and
`screen-after-restoration.png` is explicitly a post-restoration image, not an
enabled-phase comparison. Verify the option manually if CDP or restoration fails.
Samples now record the runtime hardware-draw option to identify watchdog restores
inside a capture; telemetry can still describe the previous interval immediately
after switching. `-TraceCategories` permits selecting GPU-process trace events.
The script never reloads a page or sends game inputs.

Keep the game visible, use the same scene, stop builds during measurement and
restart the game manually when testing a new core. Resource timing may omit
downloads performed by the loader's worker/cache, so an empty `coreDownloads`
list does not prove that an old core is loaded. Verify the served artifact hash,
rendering fix and observed stream allocation sizes as well.

## Remaining limits

- The warmed GPU path had zero new audio underruns in two one-minute captures.
  This is not a ten-minute audio stability test or a guarantee for other scenes.
- Other games, heavy gameplay, hardware PICA draws, inputs and save round trips
  were not comprehensively verified in this performance pass.
- Software vertex processing remains below 60 core FPS with persistent audio
  underruns. The corrected GPU path reaches roughly 60 core FPS in the measured
  gameplay scene, but cold shader compilation still causes long stalls. Other
  shader programs, browser/GPU backends and the earlier unrecorded airship-crash
  sequence remain unverified. No controlled old/new gameplay speedup has been
  established for the operand fast paths alone.
- The loader logged that it could not parse/fetch the core report and fell back
  to a random cache version. Startup caching still needs separate investigation;
  the server artifact hash and the final rendering/upload behavior were verified.
- Existing build warnings remain: RetroArch `get_core_options()` returns a stack
  address, and wasm-ld reports a `SaveDataArchive::OpenDirectory` signature
  mismatch. Neither warning was introduced or silently treated as resolved.
- Smaller streaming rings were exercised on this ANGLE/D3D11 device, not all
  browser/GPU backends. Large hardware draws can still grow the rings.
