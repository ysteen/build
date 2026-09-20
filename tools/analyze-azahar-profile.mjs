import { existsSync, readFileSync } from 'node:fs';
import { join } from 'node:path';

const readJson = path => JSON.parse(readFileSync(path, 'utf8').replace(/^\uFEFF/, ''));

function summarize(directory) {
    const samples = readJson(join(directory, 'samples.json'));
    const seen = new Set();
    const windows = samples.filter(sample => {
        const perf = sample.perf;
        if (sample.visibility !== 'visible' || !perf || seen.has(perf.at) ||
            sample.at - perf.at < 0 || sample.at - perf.at > 1600 || perf.elapsedMs <= 0) {
            return false;
        }
        seen.add(perf.at);
        return true;
    });
    if (!windows.length) throw new Error(`${directory}: no fresh, visible telemetry windows`);
    const duration = windows.reduce((sum, sample) => sum + sample.perf.elapsedMs, 0);
    const frames = sample => sample.perf.fps * sample.perf.elapsedMs / 1000;
    const frameCount = windows.reduce((sum, sample) => sum + frames(sample), 0);
    const timeMean = key => windows.reduce((sum, sample) =>
        sum + sample.perf[key] * sample.perf.elapsedMs, 0) / duration;
    const frameMean = key => windows.reduce((sum, sample) =>
        sum + sample.perf[key] * frames(sample), 0) / frameCount;
    const first = windows[0], last = windows.at(-1);
    const underruns = first.audio && last.audio
        ? last.audio.underruns - first.audio.underruns : null;

    const optionsPath = join(directory, 'capture.json');
    const options = existsSync(optionsPath) ? readJson(optionsPath) : {};
    const profilePath = join(directory, 'cpu.cpuprofile');
    const profile = options.cpuProfile === false ? null : readJson(profilePath);
    const names = new Map((profile?.nodes || []).map(node => [node.id, node.callFrame.functionName]));
    const selfTime = new Map();
    for (let i = 0; i < (profile?.samples.length || 0); ++i) {
        const name = names.get(profile.samples[i]) || '(anonymous)';
        selfTime.set(name, (selfTime.get(name) || 0) + profile.timeDeltas[i]);
    }
    return {
        directory,
        samples: samples.length,
        validWindows: windows.length,
        telemetrySeconds: duration / 1000,
        coreFps: timeMean('fps'),
        gameFps: timeMean('gameFps'),
        coreRunMsPerFrame: frameMean('coreRunMs'),
        gpuMsPerFrame: frameMean('gpuMs'),
        audioUnderruns: underruns >= 0 ? underruns : null,
        audioObservationSeconds: (last.at - first.at) / 1000,
        // CPU sampling covers the whole capture, not only the filtered telemetry windows.
        cpuProfileSeconds: profile ? (profile.endTime - profile.startTime) / 1e6 : null,
        topCpuSelfMs: [...selfTime].sort((a, b) => b[1] - a[1]).slice(0, 10)
            .map(([name, us]) => ({ name, ms: us / 1000 })),
        // Optional upload counters also include the pre-capture VSync warmup.
        uploadTotals: last.uploads || null,
    };
}

if (process.argv.length < 3) {
    console.error('Usage: node tools/analyze-azahar-profile.mjs <capture-directory> [...]');
    process.exitCode = 1;
} else {
    for (const directory of process.argv.slice(2)) {
        console.log(JSON.stringify(summarize(directory), null, 2));
    }
}
