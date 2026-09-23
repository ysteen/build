// Runs generated PICA control-flow fixtures in a separate WebGL2 context.
// Transform feedback is compared with the real software interpreter's outputs.
(fixtures, options = {}) => {
    const canvas = document.createElement('canvas');
    canvas.width = canvas.height = 1;
    const gl = canvas.getContext('webgl2', {antialias: false});
    if (!gl) throw new Error('WebGL2 unavailable');
    const buffers = [], programs = [], shaders = [];
    const timings = [];
    let current;
    const check = (condition, message) => { if (!condition) throw new Error(message); };
    const compile = (type, source) => {
        const shader = gl.createShader(type);
        shaders.push(shader);
        gl.shaderSource(shader, source);
        const start = performance.now();
        gl.compileShader(shader);
        check(gl.getShaderParameter(shader, gl.COMPILE_STATUS), gl.getShaderInfoLog(shader));
        if (current) {
            current.compileMs += performance.now() - start;
            current.fallthroughWarnings += (gl.getShaderInfoLog(shader).match(/non-empty fall-through/g) || []).length;
        }
        return shader;
    };
    const buffer = (target, data) => {
        const handle = gl.createBuffer();
        buffers.push(handle);
        gl.bindBuffer(target, handle);
        gl.bufferData(target, data, gl.DYNAMIC_COPY);
        return handle;
    };
    let cases = 0;
    try {
        const fragment = compile(gl.FRAGMENT_SHADER, `#version 300 es
            precision highp float;
            out vec4 color;
            void main() { color = vec4(1.0); }`);
        const outputBuffer = buffer(gl.TRANSFORM_FEEDBACK_BUFFER, 16);
        gl.bindBufferBase(gl.TRANSFORM_FEEDBACK_BUFFER, 0, outputBuffer);
        gl.vertexAttrib4f(1, 1, 1, 1, 1);
        gl.vertexAttrib4f(2, 3, 3, 3, 3);
        gl.enableVertexAttribArray(0);
        gl.enable(gl.RASTERIZER_DISCARD);
        for (const fixture of fixtures) {
            current = {name: fixture.name, compileMs: 0, linkMs: 0, drawMs: 0, fallthroughWarnings: 0};
            timings.push(current);
            const vertex = compile(gl.VERTEX_SHADER, fixture.source);
            const program = gl.createProgram();
            programs.push(program);
            gl.attachShader(program, vertex);
            gl.attachShader(program, fragment);
            gl.transformFeedbackVaryings(program, ['result0'], gl.INTERLEAVED_ATTRIBS);
            const linkStart = performance.now();
            gl.linkProgram(program);
            check(gl.getProgramParameter(program, gl.LINK_STATUS), gl.getProgramInfoLog(program));
            current.linkMs = performance.now() - linkStart;
            gl.useProgram(program);
            const block = gl.getUniformBlockIndex(program, 'vs_pica_data');
            const size = block === gl.INVALID_INDEX ? 80 :
                gl.getActiveUniformBlockParameter(program, block, gl.UNIFORM_BLOCK_DATA_SIZE);
            const uniforms = new Uint32Array(size / 4);
            // std140: b at byte 0, i[0] at byte 16. LOOP executes x + 1 times.
            uniforms[4] = 2;
            uniforms[6] = 1;
            const uniformBuffer = buffer(gl.UNIFORM_BUFFER, uniforms);
            gl.bindBufferBase(gl.UNIFORM_BUFFER, 0, uniformBuffer);
            if (block !== gl.INVALID_INDEX) gl.uniformBlockBinding(program, block, 0);
            for (const packed of [false, true]) {
                buffer(gl.ARRAY_BUFFER, packed ? new Int16Array([0, 1, 2, 3]) : new Float32Array([0, 1, 2, 3]));
                gl.vertexAttribPointer(0, 4, packed ? gl.SHORT : gl.FLOAT, false, 0, 0);
                for (const test of fixture.cases) {
                    uniforms[0] = test.bools;
                    gl.bindBuffer(gl.UNIFORM_BUFFER, uniformBuffer);
                    gl.bufferSubData(gl.UNIFORM_BUFFER, 0, uniforms);
                    const drawStart = performance.now();
                    gl.beginTransformFeedback(gl.POINTS);
                    gl.drawArrays(gl.POINTS, 0, 1);
                    gl.endTransformFeedback();
                    const actual = new Float32Array(4);
                    gl.bindBuffer(gl.TRANSFORM_FEEDBACK_BUFFER, outputBuffer);
                    gl.getBufferSubData(gl.TRANSFORM_FEEDBACK_BUFFER, 0, actual);
                    current.drawMs += performance.now() - drawStart;
                    check(gl.getError() === gl.NO_ERROR, `${fixture.name}: WebGL draw error: ${gl.getProgramInfoLog(program)}`);
                    check(actual.every((value, lane) => value === test.expected[lane]),
                        `${fixture.name}: mask=${test.bools}, packed=${packed}, expected=${test.expected}, actual=${actual}`);
                    ++cases;
                }
            }
            if (options.warmVertices) {
                const count = options.warmVertices;
                check(Number.isInteger(count) && count > 0 && count <= 100000, 'Invalid benchmark size');
                const inputs = new Float32Array(count * 4);
                for (let i = 0; i < inputs.length; ++i) inputs[i] = i % 4;
                buffer(gl.ARRAY_BUFFER, inputs);
                gl.vertexAttribPointer(0, 4, gl.FLOAT, false, 0, 0);
                gl.bindBuffer(gl.TRANSFORM_FEEDBACK_BUFFER, outputBuffer);
                gl.bufferData(gl.TRANSFORM_FEEDBACK_BUFFER, count * 16, gl.DYNAMIC_COPY);
                current.warmDraws = [];
                for (const mask of [0, 15]) {
                    uniforms[0] = mask;
                    gl.bindBuffer(gl.UNIFORM_BUFFER, uniformBuffer);
                    gl.bufferSubData(gl.UNIFORM_BUFFER, 0, uniforms);
                    const times = [];
                    const actual = new Float32Array(4);
                    const expected = fixture.cases.find(test => test.bools === mask).expected;
                    for (let run = 0; run < 4; ++run) {
                        const start = performance.now();
                        gl.beginTransformFeedback(gl.POINTS);
                        gl.drawArrays(gl.POINTS, 0, count);
                        gl.endTransformFeedback();
                        // Read back the last vertex so timing includes completed GPU work.
                        gl.bindBuffer(gl.TRANSFORM_FEEDBACK_BUFFER, outputBuffer);
                        gl.getBufferSubData(gl.TRANSFORM_FEEDBACK_BUFFER, (count - 1) * 16, actual);
                        if (run) times.push(performance.now() - start);
                        check(gl.getError() === gl.NO_ERROR && actual.every((x, i) => x === expected[i]),
                            `${fixture.name}: bulk draw differs from the interpreter`);
                    }
                    current.warmDraws.push({vertices: count, mask, ms: times});
                }
                // Restore the small feedback buffer for the next fixture.
                gl.bufferData(gl.TRANSFORM_FEEDBACK_BUFFER, 16, gl.DYNAMIC_COPY);
            }
        }
        return {passed: true, shaders: fixtures.length, cases, timings};
    } finally {
        programs.forEach(p => gl.deleteProgram(p));
        shaders.forEach(s => gl.deleteShader(s));
        buffers.forEach(b => gl.deleteBuffer(b));
        gl.getExtension('WEBGL_lose_context')?.loseContext();
    }
}
