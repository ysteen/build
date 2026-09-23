async function verifyAzaharFragmentVariants(before, after) {
    if (before.length !== after.length) throw new Error("Fixture count mismatch");
    const canvas = document.createElement("canvas");
    canvas.width = canvas.height = 4;
    const gl = canvas.getContext("webgl2", { antialias: false, preserveDrawingBuffer: true });
    if (!gl) throw new Error("WebGL 2 unavailable");
    const prefix = "#version 300 es\nprecision highp float;\nprecision highp int;\n";
    const compile = (type, source) => {
        const shader = gl.createShader(type);
        gl.shaderSource(shader, prefix + source);
        gl.compileShader(shader);
        if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
            throw new Error(gl.getShaderInfoLog(shader));
        }
        return shader;
    };
    const vertex = compile(gl.VERTEX_SHADER, `
uniform vec4 test_primary;
layout(location = 0) in vec2 test_position;
out vec4 primary_color;
out vec2 texcoord0;
out vec2 texcoord1;
out vec2 texcoord2;
out float texcoord0_w;
out vec4 normquat;
out vec3 view;
void main() {
    gl_Position = vec4(test_position, 0, 1);
    primary_color = test_primary;
    texcoord0 = texcoord1 = texcoord2 = vec2(0.5);
    texcoord0_w = 1.0;
    normquat = vec4(0, 0, 0, 1);
    view = vec3(0, 0, 1);
}`);
    const vertices = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, vertices);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 3, -1, -1, 3]), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    const programs = new Map();
    let offsetsVerified = false;
    const getProgram = (source) => {
        if (programs.has(source)) return programs.get(source);
        const fragment = compile(gl.FRAGMENT_SHADER, source);
        const program = gl.createProgram();
        gl.attachShader(program, vertex);
        gl.attachShader(program, fragment);
        gl.linkProgram(program);
        if (!gl.getProgramParameter(program, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(program));
        gl.deleteShader(fragment);
        const block = gl.getUniformBlockIndex(program, "fs_data");
        if (block !== gl.INVALID_INDEX) gl.uniformBlockBinding(program, block, 2);
        if (source.includes("int alphatest_func;") && !offsetsVerified) {
            const indices = gl.getUniformIndices(program, ["alphatest_func", "scissor_mode"]);
            const offsets = gl.getActiveUniforms(program, indices, gl.UNIFORM_OFFSET);
            if (offsets[0] !== 72 || offsets[1] !== 76) throw new Error("Uniform layout mismatch");
            offsetsVerified = true;
        }
        const entry = { program, primary: gl.getUniformLocation(program, "test_primary") };
        programs.set(source, entry);
        return entry;
    };
    const buffer = gl.createBuffer();
    gl.bindBuffer(gl.UNIFORM_BUFFER, buffer);
    gl.bufferData(gl.UNIFORM_BUFFER, 0x530, gl.DYNAMIC_DRAW);
    gl.bindBufferBase(gl.UNIFORM_BUFFER, 2, buffer);
    const data = new ArrayBuffer(0x530);
    const ints = new Int32Array(data), floats = new Float32Array(data);
    ints[0] = 1;
    floats[2] = 1;
    ints[6] = ints[7] = 1;
    ints[8] = ints[9] = 3;
    floats.set([0.7, 0.3, 0.6, 0.2], 1136 / 4);
    const render = (row, primaryAlpha, reference) => {
        const check = (step) => {
            const error = gl.getError();
            if (error) throw new Error(`WebGL error ${error} during ${step}`);
        };
        check("setup");
        const entry = getProgram(row.source);
        check("program setup");
        gl.useProgram(entry.program);
        gl.uniform4f(entry.primary, 0.25, 0.5, 0.75, primaryAlpha);
        check("primary uniform");
        ints[1] = reference;
        ints[18] = row.alpha;
        ints[19] = row.scissor;
        gl.bufferSubData(gl.UNIFORM_BUFFER, 0, new Uint8Array(data));
        check("uniform upload");
        gl.clearColor(1 / 255, 2 / 255, 3 / 255, 4 / 255);
        gl.clear(gl.COLOR_BUFFER_BIT);
        check("clear");
        gl.drawArrays(gl.TRIANGLES, 0, 3);
        check(`draw ${row.op}/${row.alpha}/${row.scissor}/${row.variant}`);
        const pixels = new Uint8Array(64);
        gl.readPixels(0, 0, 4, 4, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
        check("readback");
        return pixels;
    };
    const verify = (i, alpha, reference) => {
        // ANGLE rejects the old all-discard shader at draw time on this device.
        // Compare Never against its specified result: the untouched framebuffer.
        const expected = before[i].alpha === 0
            ? Uint8Array.from({ length: 64 }, (_, index) => (index % 4) + 1)
            : render(before[i], alpha, reference);
        const actual = render(after[i], alpha, reference);
        if (expected.some((value, index) => value !== actual[index])) {
            throw new Error(`Pixel mismatch: fixture ${i}, alpha ${alpha}, reference ${reference}`);
        }
        if (before[i].alpha === 1 && before[i].scissor === 0 &&
            actual.every((value, index) => value === (index % 4) + 1)) {
            throw new Error("Draw did not change the framebuffer");
        }
    };
    let comparisons = 0;
    try {
        gl.viewport(0, 0, 4, 4);
        gl.disable(gl.DITHER);
        for (let i = 0; i < before.length; ++i) {
            verify(i, 0.5, 128);
            ++comparisons;
            if (before[i].op === 0 && before[i].variant === 0) {
                for (const alpha of [0, 0.5, 1]) {
                    for (const reference of [0, 128, 255]) {
                        verify(i, alpha, reference);
                        ++comparisons;
                    }
                }
            }
            if (i % 8 === 0) await new Promise(resolve => setTimeout(resolve, 0));
        }
        if (!offsetsVerified) throw new Error("Uniform offsets were not checked");
        return {
            fixtures: before.length,
            pixelComparisons: comparisons,
            pixelsPerComparison: 16,
            beforeUniqueShaders: new Set(before.map(row => row.source)).size,
            afterUniqueShaders: new Set(after.map(row => row.source)).size,
            uniformOffsets: [72, 76],
            passed: true,
        };
    } finally {
        gl.useProgram(null);
        for (const { program } of programs.values()) gl.deleteProgram(program);
        gl.deleteShader(vertex);
        gl.deleteBuffer(buffer);
        gl.deleteBuffer(vertices);
        gl.getExtension("WEBGL_lose_context")?.loseContext();
    }
}
