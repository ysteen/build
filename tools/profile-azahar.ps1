param(
    [string]$CdpBase = "http://127.0.0.1:9222",
    [string]$TargetId = "",
    [Parameter(Mandatory = $true)][string]$OutputDir,
    [ValidateRange(1, 600)][int]$DurationSeconds = 30,
    [switch]$Trace,
    [string]$TraceCategories = "devtools.timeline,v8",
    [switch]$InspectOnly,
    [switch]$NoCpuProfile,
    [switch]$Screenshot,
    [switch]$TraceUploads,
    [switch]$VerifyPresentShaders,
    [string]$VerifyJumpFixtures = "",
    [ValidateSet("unchanged", "enabled", "disabled")][string]$VSync = "unchanged",
    [ValidateSet("unchanged", "enabled", "disabled")][string]$WebGLHwDraw = "unchanged"
)

$ErrorActionPreference = "Stop"
$targets = (Invoke-WebRequest -UseBasicParsing "$CdpBase/json/list" -TimeoutSec 5).Content | ConvertFrom-Json
if ($targets.PSObject.Properties.Name -contains "value") { $targets = $targets.value }
$pages = @($targets | Where-Object {
    $_.type -eq "page" -and ([Uri]$_.url).Port -eq 8081 -and
    ([Uri]$_.url).Host -in @("127.0.0.1", "localhost")
})
if ($TargetId) { $pages = @($pages | Where-Object { $_.id -eq $TargetId }) }
if ($pages.Count -ne 1) { throw "Select exactly one localhost:8081 test page using -TargetId." }
$target = $pages[0]
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$encoding = [Text.UTF8Encoding]::new($false)
$socket = [Net.WebSockets.ClientWebSocket]::new()
$socket.ConnectAsync([Uri]$target.webSocketDebuggerUrl, [Threading.CancellationToken]::None).GetAwaiter().GetResult() | Out-Null
$script:sequence = 0
$script:traceStream = $null
$restoreVsync = $null
$restoreHwDraw = $null
$uploadsInstrumented = $false
$profilingStarted = $false
$tracingStarted = $false

function Receive-Cdp {
    $buffer = [byte[]]::new(262144)
    $stream = [IO.MemoryStream]::new()
    $timeout = [Threading.CancellationTokenSource]::new(30000)
    try {
        do {
            $received = $socket.ReceiveAsync([ArraySegment[byte]]::new($buffer), $timeout.Token).GetAwaiter().GetResult()
            if ($received.MessageType -eq [Net.WebSockets.WebSocketMessageType]::Close) {
                throw "Chrome closed the profiling connection."
            }
            $stream.Write($buffer, 0, $received.Count)
        } until ($received.EndOfMessage)
        return [Text.Encoding]::UTF8.GetString($stream.ToArray()) | ConvertFrom-Json
    } finally {
        $stream.Dispose()
        $timeout.Dispose()
    }
}

function Process-Event($message) {
    if ($message.method -eq "Tracing.tracingComplete") {
        $script:traceStream = $message.params.stream
    }
}

function Invoke-Cdp([string]$Method, [hashtable]$Params = @{}) {
    $id = ++$script:sequence
    $payload = @{ id = $id; method = $Method; params = $Params } | ConvertTo-Json -Depth 12 -Compress
    $bytes = [Text.Encoding]::UTF8.GetBytes($payload)
    $socket.SendAsync([ArraySegment[byte]]::new($bytes), [Net.WebSockets.WebSocketMessageType]::Text, $true, [Threading.CancellationToken]::None).GetAwaiter().GetResult() | Out-Null
    while ($true) {
        $message = Receive-Cdp
        if ($message.id -eq $id) {
            if ($message.error) { throw "${Method}: $($message.error.message)" }
            if ($message.result.exceptionDetails) {
                throw "${Method}: $($message.result.exceptionDetails.exception.description)"
            }
            return $message.result
        }
        Process-Event $message
    }
}

function Restore-WebGLHwDraw {
    if (-not $script:restoreHwDraw) { return }
    if ($socket.State -ne [Net.WebSockets.WebSocketState]::Open) {
        Write-Warning "CDP disconnected before restoration. Verify the option after the in-page watchdog runs or reload the test page."
        return
    }
    try {
        $expression = "(() => { const t = globalThis.__AZAHAR_HW_DRAW_EXPERIMENT__; if (t) { t.restore(); clearTimeout(t.timer); delete globalThis.__AZAHAR_HW_DRAW_EXPERIMENT__; } else { globalThis.EJS_emulator.gameManager.functions.setVariable('citra_use_webgl_hw_draw', '$script:restoreHwDraw'); } return globalThis.EJS_emulator.gameManager.getCoreOptions().split('\n').find(l => l.startsWith('citra_use_webgl_hw_draw|')); })()"
        $restored = Invoke-Cdp "Runtime.evaluate" @{expression=$expression; returnByValue=$true}
        $expected = "citra_use_webgl_hw_draw|$script:restoreHwDraw;"
        if (-not $restored.result.value -or -not $restored.result.value.StartsWith($expected)) {
            throw "The hardware-draw option did not read back as $script:restoreHwDraw."
        }
        [IO.File]::WriteAllText((Join-Path $OutputDir "restored-option.txt"), $restored.result.value, $encoding)
        Write-Output "Restored $($restored.result.value)"
        $script:restoreHwDraw = $null
    } catch {
        Write-Warning "Unable to confirm hardware-draw restoration. Verify the option before continuing gameplay."
    }
}

$probe = @'
(() => ({
  at: Date.now(),
  url: location.origin + location.pathname,
  visibility: document.visibilityState,
  focused: document.hasFocus(),
  crossOriginIsolated,
  core: globalThis.EJS_core || globalThis.EJS_emulator?.config?.system || null,
  webglHwDraw: globalThis.EJS_emulator?.gameManager?.getCoreOptions?.()
    ?.match(/^citra_use_webgl_hw_draw\|(enabled|disabled);/m)?.[1] || null,
  canvas: (() => { const c = document.querySelector('canvas'); return c ? { width: c.width, height: c.height } : null; })(),
  perf: globalThis.__AZAHAR_PERF__ || null,
  pica: globalThis.__AZAHAR_PICA_DRAWS__ || null,
  gpu: globalThis.__AZAHAR_GPU_DRAWS__ || null,
  shaders: globalThis.__AZAHAR_SHADER_CACHE__ || null,
  shaderInterpreter: globalThis.__AZAHAR_SHADER_INTERPRETER__ || null,
  audio: globalThis.__EJS_AUDIO_STATS__ || null,
  mainloop: globalThis.__RA_MAINLOOP_STATS__ || null,
  asyncify: globalThis.__EJS_ASYNCIFY_STATS__ || null,
  uploads: globalThis.__AZAHAR_UPLOAD_STATS__ || null
}))()
'@

try {
    if (-not $InspectOnly) {
        $deadline = [DateTime]::UtcNow.AddSeconds(60)
        $readyExpression = "!!globalThis.__AZAHAR_PERF__ && Date.now() - globalThis.__AZAHAR_PERF__.at < 1600"
        do {
            $ready = Invoke-Cdp "Runtime.evaluate" @{ expression = $readyExpression; returnByValue = $true }
            if ($ready.result.value) { break }
            if ([DateTime]::UtcNow -gt $deadline) { throw "No fresh Azahar telemetry after 60 seconds." }
            Start-Sleep -Seconds 1
        } while ($true)
    }
    $details = Invoke-Cdp "Runtime.evaluate" @{ returnByValue = $true; expression = @'
(() => {
  const e = globalThis.EJS_emulator;
  const m = e?.gameManager?.Module;
  const loop = m?.Browser?.mainLoop;
  const gl = document.querySelector('canvas')?.getContext('webgl2');
  const ext = gl?.getExtension('WEBGL_debug_renderer_info');
  return {
    url: location.origin + location.pathname,
    title: document.title,
    visibility: document.visibilityState,
    userAgent: navigator.userAgent,
    coreDownloads: performance.getEntriesByType('resource').flatMap(r => {
      const u = new URL(r.name);
      return u.pathname.endsWith('/cores/azahar-thread-wasm.data')
        ? [{ path: u.pathname, build: u.searchParams.get('v') }] : [];
    }),
    renderer: ext ? gl.getParameter(ext.UNMASKED_RENDERER_WEBGL) : null,
    vendor: ext ? gl.getParameter(ext.UNMASKED_VENDOR_WEBGL) : null,
    contextAttributes: gl?.getContextAttributes(),
    scheduler: loop ? { timingMode: loop.timingMode, timingValue: loop.timingValue, currentFrameNumber: loop.currentFrameNumber } : null,
    coreOptions: e?.gameManager?.getCoreOptions?.() || null,
    vsync: e?.getSettingValue?.('vsync') || null,
    speedControls: e ? {
      fastForward: !!e.isFastForward,
      slowMotion: !!e.isSlowMotion,
      fastForwardSetting: e.getSettingValue?.('fastForward'),
      slowMotionSetting: e.getSettingValue?.('slowMotion')
    } : null,
    perf: globalThis.__AZAHAR_PERF__ || null,
    mainloop: globalThis.__RA_MAINLOOP_STATS__ || null,
    retroarchConfig: m?.FS?.readFile('/home/web_user/.config/retroarch/retroarch.cfg', {encoding:'utf8'})
      ?.split('\n').filter(line => /^(video_(vsync|swap_interval|refresh_rate|frame_delay|threaded)|audio_(sync|latency))\s*=/.test(line)) || null
  };
})()
'@ }
    [IO.File]::WriteAllText((Join-Path $OutputDir "details.json"), ($details.result.value | ConvertTo-Json -Depth 20), $encoding)
    if ($Screenshot) {
        $capture = Invoke-Cdp "Page.captureScreenshot" @{ format = "png"; captureBeyondViewport = $false }
        [IO.File]::WriteAllBytes((Join-Path $OutputDir "screen.png"), [Convert]::FromBase64String($capture.data))
    }
    if ($VerifyPresentShaders) {
        $shaderRoot = Join-Path $PSScriptRoot "../compile/azahar/src/video_core/host_shaders"
        $shaderSources = @{}
        foreach ($name in @("opengl_present.frag", "opengl_present_anaglyph.frag", "opengl_present_interlaced.frag")) {
            $shaderSources[$name] = [IO.File]::ReadAllText((Join-Path $shaderRoot $name))
        }
        $testCode = [IO.File]::ReadAllText((Join-Path $PSScriptRoot "tests/azahar-present-shaders.js"))
        $testExpression = "($testCode)(" + (ConvertTo-Json -InputObject $shaderSources -Compress) + ")"
        $tested = Invoke-Cdp "Runtime.evaluate" @{ expression = $testExpression; returnByValue = $true }
        [IO.File]::WriteAllText((Join-Path $OutputDir "shader-tests.json"), ($tested.result.value | ConvertTo-Json), $encoding)
        Write-Output "Presentation shader tests: $($tested.result.value.cases) cases passed"
    }
    if ($VerifyJumpFixtures) {
        $fixtures = [IO.File]::ReadAllText($VerifyJumpFixtures)
        $testCode = [IO.File]::ReadAllText((Join-Path $PSScriptRoot "tests/azahar-shader-jumps.js"))
        $tested = Invoke-Cdp "Runtime.evaluate" @{ expression = "($testCode)($fixtures)"; returnByValue = $true }
        [IO.File]::WriteAllText((Join-Path $OutputDir "shader-jump-tests.json"), ($tested.result.value | ConvertTo-Json), $encoding)
        Write-Output "Shader jump tests: $($tested.result.value.cases) cases passed"
    }
    if ($InspectOnly) { Write-Output "Saved runtime details in $OutputDir"; return }
    if ($TraceUploads) {
        Invoke-Cdp "Runtime.evaluate" @{ expression = @'
(() => {
  const gl = document.querySelector('canvas').getContext('webgl2');
  if (globalThis.__AZAHAR_RESTORE_UPLOADS__) throw new Error('Upload instrumentation already active');
  const originals = { bufferData: gl.bufferData, bufferSubData: gl.bufferSubData };
  const stats = globalThis.__AZAHAR_UPLOAD_STATS__ = {};
  for (const name of Object.keys(originals)) {
    gl[name] = function(...args) {
      const start = performance.now();
      const result = originals[name].apply(this, args);
      const elapsed = performance.now() - start;
      const key = name + ':' + args[0];
      const s = stats[key] ||= { calls: 0, ms: 0, maxMs: 0, bytes: 0, maxBytes: 0, maxOffset: 0 };
      const data = args[name === 'bufferData' ? 1 : 2];
      const bytes = typeof data === 'number' ? data : args.length >= 5 ? args[4] * data.BYTES_PER_ELEMENT : data?.byteLength || 0;
      s.calls++; s.ms += elapsed; s.maxMs = Math.max(s.maxMs, elapsed);
      s.bytes += bytes; s.maxBytes = Math.max(s.maxBytes, bytes);
      if (name === 'bufferSubData') s.maxOffset = Math.max(s.maxOffset, args[1]);
      return result;
    };
  }
  globalThis.__AZAHAR_RESTORE_UPLOADS__ = () => {
    Object.assign(gl, originals);
    delete globalThis.__AZAHAR_RESTORE_UPLOADS__;
    delete globalThis.__AZAHAR_UPLOAD_STATS__;
  };
})()
'@ } | Out-Null
        $uploadsInstrumented = $true
    }
    if ($VSync -ne "unchanged") {
        if ($details.result.value.vsync -notin @("enabled", "disabled")) {
            throw "Cannot determine the original VSync setting."
        }
        $restoreVsync = $details.result.value.vsync
        $enabled = if ($VSync -eq "enabled") { "true" } else { "false" }
        Invoke-Cdp "Runtime.evaluate" @{ expression = "globalThis.EJS_emulator.gameManager.setVSync($enabled)" } | Out-Null
        Start-Sleep -Seconds 2
    }
    if ($WebGLHwDraw -ne "unchanged") {
        $original = [regex]::Match($details.result.value.coreOptions, '(?m)^citra_use_webgl_hw_draw\|(enabled|disabled);')
        if (-not $original.Success) { throw "Unable to determine the original WebGL hardware-draw option." }
        $restoreHwDraw = $original.Groups[1].Value
        $watchdogMs = ($DurationSeconds + 15) * 1000
        $expression = @"
(() => {
  if (globalThis.__AZAHAR_HW_DRAW_EXPERIMENT__) throw new Error('A hardware-draw experiment is already active.');
  const g = globalThis.EJS_emulator.gameManager;
  const restore = () => { g.functions.setVariable('citra_use_webgl_hw_draw', '$restoreHwDraw'); };
  globalThis.__AZAHAR_HW_DRAW_EXPERIMENT__ = { restore, timer: setTimeout(restore, $watchdogMs) };
  g.functions.setVariable('citra_use_webgl_hw_draw', '$WebGLHwDraw');
  return true;
})()
"@
        Invoke-Cdp "Runtime.evaluate" @{expression=$expression; returnByValue=$true} | Out-Null
        Write-Output "WebGL hardware draw=$WebGLHwDraw (restore=$restoreHwDraw)"
    }
    Invoke-Cdp "Performance.enable" | Out-Null
    if (-not $NoCpuProfile) {
        Invoke-Cdp "Profiler.enable" | Out-Null
        Invoke-Cdp "Profiler.setSamplingInterval" @{ interval = 2000 } | Out-Null
    }
    $captureOptions = @{ cpuProfile = !$NoCpuProfile; trace = [bool]$Trace; traceCategories = $TraceCategories; traceUploads = [bool]$TraceUploads; webglHwDraw = $WebGLHwDraw }
    [IO.File]::WriteAllText((Join-Path $OutputDir "capture.json"), ($captureOptions | ConvertTo-Json), $encoding)
    $initial = Invoke-Cdp "Runtime.evaluate" @{ expression = $probe; returnByValue = $true }
    if (-not $initial.result.value.perf) { throw "The selected tab has no running Azahar telemetry." }
    [IO.File]::WriteAllText((Join-Path $OutputDir "initial.json"), ($initial.result.value | ConvertTo-Json -Depth 20), $encoding)
    if ($Trace) {
        Invoke-Cdp "Tracing.start" @{ categories = $TraceCategories; transferMode = "ReturnAsStream" } | Out-Null
        $tracingStarted = $true
    }
    if (-not $NoCpuProfile) {
        Invoke-Cdp "Profiler.start" | Out-Null
        $profilingStarted = $true
    }
    $samples = [Collections.Generic.List[object]]::new()
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($timer.Elapsed.TotalSeconds -lt $DurationSeconds) {
        $sample = Invoke-Cdp "Runtime.evaluate" @{ expression = $probe; returnByValue = $true }
        $samples.Add($sample.result.value)
        Start-Sleep -Milliseconds 1000
    }
    if ($profilingStarted) {
        $profile = Invoke-Cdp "Profiler.stop"
        $profilingStarted = $false
        [IO.File]::WriteAllText((Join-Path $OutputDir "cpu.cpuprofile"), ($profile.profile | ConvertTo-Json -Depth 30 -Compress), $encoding)
    }
    # Screenshots and trace downloads can wait on a stalled GPU. Restore first.
    Restore-WebGLHwDraw
    [IO.File]::WriteAllText((Join-Path $OutputDir "samples.json"), (ConvertTo-Json -InputObject $samples.ToArray() -Depth 20), $encoding)
    $metrics = Invoke-Cdp "Performance.getMetrics"
    [IO.File]::WriteAllText((Join-Path $OutputDir "metrics.json"), ($metrics | ConvertTo-Json -Depth 10), $encoding)
    if ($Screenshot -and $WebGLHwDraw -ne "unchanged") {
        $capture = Invoke-Cdp "Page.captureScreenshot" @{format="png"; captureBeyondViewport=$false}
        [IO.File]::WriteAllBytes((Join-Path $OutputDir "screen-after-restoration.png"), [Convert]::FromBase64String($capture.data))
    }
    if ($Trace) {
        Invoke-Cdp "Tracing.end" | Out-Null
        $tracingStarted = $false
        while (-not $script:traceStream) { Process-Event (Receive-Cdp) }
        $file = [IO.File]::Create((Join-Path $OutputDir "trace.json"))
        try {
            do {
                $chunk = Invoke-Cdp "IO.read" @{ handle = $script:traceStream; size = 262144 }
                if ($chunk.data) {
                    [byte[]]$data = if ($chunk.base64Encoded) { [Convert]::FromBase64String($chunk.data) } else { $encoding.GetBytes($chunk.data) }
                    $file.Write($data, 0, $data.Length)
                }
            } until ($chunk.eof)
        } finally { $file.Dispose() }
        Invoke-Cdp "IO.close" @{ handle = $script:traceStream } | Out-Null
    }
    Write-Output "Captured $($samples.Count) samples (CPU profile: $(!$NoCpuProfile)) in $OutputDir"
} finally {
    Restore-WebGLHwDraw
    if ($tracingStarted -and $socket.State -eq [Net.WebSockets.WebSocketState]::Open) {
        try { Invoke-Cdp "Tracing.end" | Out-Null }
        catch { Write-Warning "Unable to stop tracing after capture failed." }
    }
    if ($profilingStarted -and $socket.State -eq [Net.WebSockets.WebSocketState]::Open) {
        try { Invoke-Cdp "Profiler.stop" | Out-Null }
        catch { Write-Warning "Unable to stop the CPU profile after capture failed." }
    }
    if ($uploadsInstrumented -and $socket.State -eq [Net.WebSockets.WebSocketState]::Open) {
        try {
            Invoke-Cdp "Runtime.evaluate" @{ expression = "globalThis.__AZAHAR_RESTORE_UPLOADS__?.()" } | Out-Null
        } catch { Write-Warning "Upload instrumentation could not be removed; reload after saving progress." }
    }
    if ($restoreVsync -and $socket.State -eq [Net.WebSockets.WebSocketState]::Open) {
        $enabled = if ($restoreVsync -eq "enabled") { "true" } else { "false" }
        try {
            Invoke-Cdp "Runtime.evaluate" @{ expression = "globalThis.EJS_emulator.gameManager.setVSync($enabled)" } | Out-Null
            Write-Output "Restored VSync=$restoreVsync"
        } catch {
            Write-Warning "Unable to restore VSync=$restoreVsync. Restore it in the emulator settings."
        }
    }
    $socket.Dispose()
}
