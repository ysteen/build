async function verifyVertexInput(fixtures) {
    const gl = document.createElement('canvas').getContext('webgl2');
    if (!gl) throw Error('WebGL2 unavailable');
    const shader = (type, source) => {
        const result = gl.createShader(type);
        gl.shaderSource(result, '#version 300 es\nprecision highp float;\n' + source);
        gl.compileShader(result);
        if (!gl.getShaderParameter(result, gl.COMPILE_STATUS)) throw Error(gl.getShaderInfoLog(result));
        return result;
    };
    const vs = shader(gl.VERTEX_SHADER, 'layout(location=0) in vec4 attr; out vec4 captured; void main(){captured=attr;gl_Position=vec4(0,0,0,1);}');
    const fs = shader(gl.FRAGMENT_SHADER, 'out vec4 color; void main(){color=vec4(1);}');
    const program = gl.createProgram();
    gl.attachShader(program, vs); gl.attachShader(program, fs);
    gl.transformFeedbackVaryings(program, ['captured'], gl.INTERLEAVED_ATTRIBS);
    gl.linkProgram(program);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) throw Error(gl.getProgramInfoLog(program));
    gl.useProgram(program);
    const vao = gl.createVertexArray(); gl.bindVertexArray(vao);
    const input = gl.createBuffer(), output = gl.createBuffer();
    const feedback = gl.createTransformFeedback(); gl.bindTransformFeedback(gl.TRANSFORM_FEEDBACK, feedback);
    gl.bindBuffer(gl.TRANSFORM_FEEDBACK_BUFFER, output);
    gl.bufferData(gl.TRANSFORM_FEEDBACK_BUFFER, 48, gl.STREAM_READ);
    gl.bindBufferBase(gl.TRANSFORM_FEEDBACK_BUFFER, 0, output);
    gl.enableVertexAttribArray(0); gl.enable(gl.RASTERIZER_DISCARD);
    const capture = (data, components, type, stride) => {
        gl.bindBuffer(gl.ARRAY_BUFFER, input);
        gl.bufferData(gl.ARRAY_BUFFER, new Uint8Array(data), gl.STREAM_DRAW);
        gl.vertexAttribPointer(0, components, type, false, stride, 0);
        gl.beginTransformFeedback(gl.POINTS); gl.drawArrays(gl.POINTS, 0, 3); gl.endTransformFeedback();
        const values = new Float32Array(12);
        gl.bindBuffer(gl.TRANSFORM_FEEDBACK_BUFFER, output);
        gl.getBufferSubData(gl.TRANSFORM_FEEDBACK_BUFFER, 0, values);
        if (gl.getError() !== gl.NO_ERROR) throw Error('GL error during attribute capture');
        return values;
    };
    try {
        for (const f of fixtures) {
            const original = capture(f.source, f.components, [gl.BYTE, gl.UNSIGNED_BYTE, gl.SHORT, gl.FLOAT][f.type], f.stride);
            const converted = capture(f.converted, 4, gl.FLOAT, 16);
            const expected = new Float32Array(new Uint8Array(f.converted).buffer);
            for (let i=0; i<12; ++i)
                if (!Object.is(original[i], converted[i]) || !Object.is(converted[i], expected[i]))
                    throw Error(`Attribute mismatch: type=${f.type} size=${f.components} stride=${f.stride} element=${i}`);
        }
        return {passed:true, fixtures:fixtures.length, floatComparisons:fixtures.length*12};
    } finally {
        gl.disable(gl.RASTERIZER_DISCARD); gl.bindTransformFeedback(gl.TRANSFORM_FEEDBACK, null);
        gl.bindVertexArray(null); gl.useProgram(null);
        gl.deleteTransformFeedback(feedback); gl.deleteBuffer(input); gl.deleteBuffer(output);
        gl.deleteVertexArray(vao); gl.deleteProgram(program); gl.deleteShader(vs); gl.deleteShader(fs);
        gl.getExtension('WEBGL_lose_context')?.loseContext();
    }
}
