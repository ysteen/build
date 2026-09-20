// Runs generated PICA control-flow fixtures in a separate WebGL2 context.
// Transform feedback is compared with the real software interpreter's outputs.
(fixtures) => {
    const canvas = document.createElement('canvas');
    canvas.width = canvas.height = 1;
    const gl = canvas.getContext('webgl2', {antialias: false});
    if (!gl) throw new Error('WebGL2 unavailable');
    const buffers = [], programs = [], shaders = [];
    const check = (condition, message) => { if (!condition) throw new Error(message); };
    const compile = (type, source) => {
        const shader = gl.createShader(type);
        shaders.push(shader);
        gl.shaderSource(shader, source);
        gl.compileShader(shader);
        check(gl.getShaderParameter(shader, gl.COMPILE_STATUS), gl.getShaderInfoLog(shader));
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
            const vertex = compile(gl.VERTEX_SHADER, fixture.source);
            const program = gl.createProgram();
            programs.push(program);
            gl.attachShader(program, vertex);
            gl.attachShader(program, fragment);
            gl.transformFeedbackVaryings(program, ['result0'], gl.INTERLEAVED_ATTRIBS);
            gl.linkProgram(program);
            check(gl.getProgramParameter(program, gl.LINK_STATUS), gl.getProgramInfoLog(program));
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
                    gl.beginTransformFeedback(gl.POINTS);
                    gl.drawArrays(gl.POINTS, 0, 1);
                    gl.endTransformFeedback();
                    const actual = new Float32Array(4);
                    gl.bindBuffer(gl.TRANSFORM_FEEDBACK_BUFFER, outputBuffer);
                    gl.getBufferSubData(gl.TRANSFORM_FEEDBACK_BUFFER, 0, actual);
                    check(gl.getError() === gl.NO_ERROR, `${fixture.name}: WebGL draw error: ${gl.getProgramInfoLog(program)}`);
                    check(actual.every((value, lane) => value === test.expected[lane]),
                        `${fixture.name}: mask=${test.bools}, packed=${packed}, expected=${test.expected}, actual=${actual}`);
                    ++cases;
                }
            }
        }
        return {passed: true, shaders: fixtures.length, cases};
    } finally {
        programs.forEach(p => gl.deleteProgram(p));
        shaders.forEach(s => gl.deleteShader(s));
        buffers.forEach(b => gl.deleteBuffer(b));
        gl.getExtension('WEBGL_lose_context')?.loseContext();
    }
}
