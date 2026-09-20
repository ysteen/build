// Runs the repository's depth/stencil readback shaders in an unattached WebGL2 canvas.
// Input keys: webgl_readback.vert, webgl_readback_depth.frag, webgl_readback_stencil.frag.
// Never accesses the running emulator's canvas, context, textures, or framebuffers.
(sources) => {
    const canvas = document.createElement('canvas');
    canvas.width = canvas.height = 1;
    const gl = canvas.getContext('webgl2', { antialias: false });
    if (!gl) throw new Error('WebGL2 is unavailable');

    const programs = [], shaders = [], textures = [], framebuffers = [];
    const check = (condition, message) => { if (!condition) throw new Error(message); };
    const checkError = (label) => {
        const error = gl.getError();
        check(error === gl.NO_ERROR, `${label}: WebGL error 0x${error.toString(16)}`);
    };
    const compile = (type, name, source = sources[name]) => {
        check(typeof source === 'string', `Missing shader source: ${name}`);
        const shader = gl.createShader(type);
        shaders.push(shader);
        gl.shaderSource(shader, '#version 300 es\nprecision highp float;\n' +
            'precision highp int;\n' + source);
        gl.compileShader(shader);
        check(gl.getShaderParameter(shader, gl.COMPILE_STATUS),
            `${name}: ${gl.getShaderInfoLog(shader)}`);
        return shader;
    };
    const link = (vertex, fragment, name) => {
        const program = gl.createProgram();
        programs.push(program);
        gl.attachShader(program, vertex);
        gl.attachShader(program, fragment);
        gl.linkProgram(program);
        check(gl.getProgramParameter(program, gl.LINK_STATUS),
            `${name}: ${gl.getProgramInfoLog(program)}`);
        return program;
    };
    const uniform = (program, name) => {
        const location = gl.getUniformLocation(program, name);
        check(location !== null, `Missing active uniform: ${name}`);
        return location;
    };
    const makeTexture = (format, width, height, levels = 1) => {
        const texture = gl.createTexture();
        textures.push(texture);
        gl.bindTexture(gl.TEXTURE_2D, texture);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST_MIPMAP_NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_COMPARE_MODE, gl.NONE);
        gl.texStorage2D(gl.TEXTURE_2D, levels, format, width, height);
        return texture;
    };
    const makeFramebuffer = () => {
        const framebuffer = gl.createFramebuffer();
        framebuffers.push(framebuffer);
        return framebuffer;
    };
    const complete = (label) => {
        const status = gl.checkFramebufferStatus(gl.FRAMEBUFFER);
        check(status === gl.FRAMEBUFFER_COMPLETE,
            `${label}: incomplete framebuffer 0x${status.toString(16)}`);
    };

    let cases = 0, pixels = 0, immutableChecks = 0;
    let vertexArray;
    try {
        const vertex = compile(gl.VERTEX_SHADER, 'webgl_readback.vert');
        const depthProgram = link(vertex,
            compile(gl.FRAGMENT_SHADER, 'webgl_readback_depth.frag'), 'depth readback');
        const stencilProgram = link(vertex,
            compile(gl.FRAGMENT_SHADER, 'webgl_readback_stencil.frag'), 'stencil readback');
        const sourceDepth = uniform(depthProgram, 'source_depth');
        const sourceLevel = uniform(depthProgram, 'source_level');
        const depthScale = uniform(depthProgram, 'depth_scale');
        const stencilValue = uniform(stencilProgram, 'stencil_value');
        let diagnosticProgram;
        const diagnoseDepth = (source, level, x, y) => {
            if (!diagnosticProgram) {
                diagnosticProgram = link(vertex, compile(gl.FRAGMENT_SHADER,
                    'depth sample diagnostic', `
                    uniform highp sampler2D source_depth;
                    uniform int source_level;
                    out vec4 result;
                    void main() {
                        float depth = texelFetch(source_depth,
                            ivec2(gl_FragCoord.xy), source_level).r;
                        uint bits = floatBitsToUint(depth);
                        result = vec4(float(bits & 255u), float((bits >> 8u) & 255u),
                            float((bits >> 16u) & 255u), float(bits >> 24u)) / 255.0;
                    }`), 'depth sample diagnostic');
            }
            gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.DEPTH_STENCIL_ATTACHMENT,
                gl.TEXTURE_2D, null, 0);
            gl.disable(gl.STENCIL_TEST);
            gl.disable(gl.DEPTH_TEST);
            gl.disable(gl.BLEND);
            gl.colorMask(true, true, true, true);
            gl.viewport(x, y, 1, 1);
            gl.useProgram(diagnosticProgram);
            gl.bindTexture(gl.TEXTURE_2D, source);
            gl.uniform1i(uniform(diagnosticProgram, 'source_depth'), 0);
            gl.uniform1i(uniform(diagnosticProgram, 'source_level'), level);
            gl.drawArrays(gl.TRIANGLES, 0, 3);
            const bytes = new Uint8Array(4);
            gl.readPixels(x, y, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, bytes);
            checkError('Depth sample diagnostic');
            gl.bindTexture(gl.TEXTURE_2D, null);
            const view = new DataView(bytes.buffer);
            const value = view.getFloat32(0, true);
            return { bits: `0x${view.getUint32(0, true).toString(16).padStart(8, '0')}`,
                value, scaled24: value * 0xffffff,
                scaled24Float: Math.fround(value * 0xffffff),
                scaledPower24: value * 0x1000000 };
        };

        vertexArray = gl.createVertexArray();
        gl.bindVertexArray(vertexArray);
        gl.activeTexture(gl.TEXTURE0);
        gl.disable(gl.DITHER);
        gl.disable(gl.DEPTH_TEST);
        gl.disable(gl.CULL_FACE);
        gl.disable(gl.SCISSOR_TEST);
        gl.disable(gl.RASTERIZER_DISCARD);
        gl.depthMask(false);
        gl.stencilMask(0);
        gl.stencilOp(gl.KEEP, gl.KEEP, gl.KEEP);
        gl.pixelStorei(gl.PACK_ALIGNMENT, 1);
        gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
        gl.pixelStorei(gl.PACK_ROW_LENGTH, 0);
        gl.pixelStorei(gl.UNPACK_ROW_LENGTH, 0);

        const formats = [
            { name: 'D24S8', internal: gl.DEPTH24_STENCIL8,
                format: gl.DEPTH_STENCIL, type: gl.UNSIGNED_INT_24_8, max: 0xffffff },
            { name: 'D16', internal: gl.DEPTH_COMPONENT16,
                format: gl.DEPTH_COMPONENT, type: gl.UNSIGNED_SHORT, max: 0xffff },
            { name: 'D24', internal: gl.DEPTH_COMPONENT24,
                format: gl.DEPTH_COMPONENT, type: gl.UNSIGNED_INT, max: 0xffffff },
        ];
        const fullWidth = 32, fullHeight = 16;
        for (const format of formats) {
            const source = makeTexture(format.internal, fullWidth, fullHeight, 2);
            const data = [];
            for (let level = 0; level < 2; ++level) {
                const width = fullWidth >> level, height = fullHeight >> level;
                const depths = new Uint32Array(width * height);
                const stencils = new Uint8Array(width * height);
                const upload = format.name === 'D16'
                    ? new Uint16Array(width * height) : new Uint32Array(width * height);
                const boundaries = [0, format.max, 1, 2, 0xfe, 0xff, 0x100, 0x101,
                    0xfffe, 0xffff, 0x10000, 0x10001, format.max - 1,
                    format.max >> 1, (format.max >> 1) + 1];
                for (let i = 0; i < upload.length; ++i) {
                    const depth = i < boundaries.length
                        ? Math.min(boundaries[i], format.max)
                        : Math.imul(i + level * 131, 0x9e3779b1) >>> (format.name === 'D16' ? 16 : 8);
                    depths[i] = depth;
                    stencils[i] = format.name === 'D24S8' ? i & 0xff : 0;
                    // Unsigned-int depth uploads normalize all 32 bits, unlike packed D24S8.
                    // Replicate the high depth byte into the low byte to retain the 24-bit value.
                    upload[i] = format.name === 'D16' ? depth
                        : format.name === 'D24' ? ((depth << 8) | (depth >>> 16)) >>> 0
                        : ((depth << 8) | stencils[i]) >>> 0;
                }
                gl.texSubImage2D(gl.TEXTURE_2D, level, 0, 0, width, height,
                    format.format, format.type, upload);
                checkError(`${format.name} level ${level} upload`);
                data.push({ width, height, depths, stencils });
            }

            for (let level = 0; level < data.length; ++level) {
                const { width, height, depths, stencils } = data[level];
                const label = `${format.name} level ${level}`;
                const scratch = makeTexture(gl.RGBA8, width, height);
                const framebuffer = makeFramebuffer();
                const readback = (rect) => {
                    gl.bindFramebuffer(gl.FRAMEBUFFER, framebuffer);
                    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0,
                        gl.TEXTURE_2D, scratch, 0);
                    // The source may only be sampled when it is not attached to this FBO.
                    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.DEPTH_STENCIL_ATTACHMENT,
                        gl.TEXTURE_2D, null, 0);
                    gl.drawBuffers([gl.COLOR_ATTACHMENT0]);
                    gl.readBuffer(gl.COLOR_ATTACHMENT0);
                    complete(`${label} depth`);
                    gl.viewport(rect.x, rect.y, rect.width, rect.height);
                    gl.disable(gl.BLEND);
                    gl.disable(gl.STENCIL_TEST);
                    gl.colorMask(true, true, true, true);
                    gl.useProgram(depthProgram);
                    gl.bindTexture(gl.TEXTURE_2D, source);
                    gl.uniform1i(sourceDepth, 0);
                    gl.uniform1i(sourceLevel, level);
                    gl.uniform1f(depthScale, format.max);
                    gl.drawArrays(gl.TRIANGLES, 0, 3);
                    checkError(`${label} depth draw`);
                    gl.bindTexture(gl.TEXTURE_2D, null);

                    if (format.name === 'D24S8') {
                        // Only stencil testing reads the original; no depth/stencil writes occur.
                        gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.DEPTH_STENCIL_ATTACHMENT,
                            gl.TEXTURE_2D, source, level);
                        complete(`${label} stencil`);
                        gl.useProgram(stencilProgram);
                        gl.colorMask(false, false, false, true);
                        gl.enable(gl.STENCIL_TEST);
                        gl.enable(gl.BLEND);
                        gl.blendEquation(gl.FUNC_ADD);
                        gl.blendFunc(gl.ONE, gl.ONE);
                        for (let bit = 0; bit < 8; ++bit) {
                            gl.stencilFunc(gl.NOTEQUAL, 0, 1 << bit);
                            gl.uniform1f(stencilValue, (1 << bit) / 255);
                            gl.drawArrays(gl.TRIANGLES, 0, 3);
                        }
                        checkError(`${label} stencil draws`);
                        gl.disable(gl.STENCIL_TEST);
                        gl.disable(gl.BLEND);
                        gl.colorMask(true, true, true, true);
                    }

                    const output = new Uint8Array(rect.width * rect.height * 4);
                    gl.readPixels(rect.x, rect.y, rect.width, rect.height,
                        gl.RGBA, gl.UNSIGNED_BYTE, output);
                    checkError(`${label} readPixels`);
                    ++cases;
                    for (let y = 0; y < rect.height; ++y) {
                        for (let x = 0; x < rect.width; ++x) {
                            const sourceIndex = (rect.y + y) * width + rect.x + x;
                            const targetIndex = (y * rect.width + x) * 4;
                            const actualDepth = output[targetIndex] |
                                (output[targetIndex + 1] << 8) | (output[targetIndex + 2] << 16);
                            let diagnostic = '';
                            if (actualDepth !== depths[sourceIndex]) {
                                try {
                                    diagnostic = `; sampled depth ${JSON.stringify(diagnoseDepth(
                                        source, level, rect.x + x, rect.y + y))}`;
                                    const boundaries = [];
                                    for (let boundaryX = 0; boundaryX < Math.min(15, width); ++boundaryX) {
                                        boundaries.push({ x: boundaryX, y: 0,
                                            expected: depths[boundaryX],
                                            ...diagnoseDepth(source, level, boundaryX, 0) });
                                    }
                                    diagnostic += `; boundaries ${JSON.stringify(boundaries)}`;
                                } catch (error) {
                                    diagnostic = `; depth diagnostic failed: ${String(error)}`;
                                }
                            }
                            check(actualDepth === depths[sourceIndex],
                                `${label} (${rect.x + x},${rect.y + y}) depth: ` +
                                `expected ${depths[sourceIndex]}, received ${actualDepth}${diagnostic}`);
                            check(output[targetIndex + 3] === stencils[sourceIndex],
                                `${label} (${rect.x + x},${rect.y + y}) stencil: ` +
                                `expected ${stencils[sourceIndex]}, received ${output[targetIndex + 3]}`);
                            ++pixels;
                        }
                    }
                    return output;
                };

                const fullRect = { x: 0, y: 0, width, height };
                const initial = readback(fullRect);
                readback({ x: 3, y: 2, width: width - 7, height: height - 3 });
                // Repeating after all stencil passes detects accidental source modifications.
                const repeated = readback(fullRect);
                check(initial.every((value, i) => value === repeated[i]),
                    `${label}: original depth/stencil source changed`);
                ++immutableChecks;
                gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.DEPTH_STENCIL_ATTACHMENT,
                    gl.TEXTURE_2D, null, 0);
                gl.bindFramebuffer(gl.FRAMEBUFFER, null);
            }
        }

        checkError('Readback fixture completion');
        return { passed: true, shaders: shaders.length, formats: formats.map(f => f.name),
            cases, pixels, immutableChecks, stencilValues: 256, mipLevels: 2,
            croppedReadback: true, isolatedContext: true };
    } finally {
        for (const framebuffer of framebuffers) gl.deleteFramebuffer(framebuffer);
        for (const texture of textures) gl.deleteTexture(texture);
        for (const program of programs) gl.deleteProgram(program);
        for (const shader of shaders) gl.deleteShader(shader);
        if (vertexArray) gl.deleteVertexArray(vertexArray);
        gl.getExtension('WEBGL_lose_context')?.loseContext();
    }
}
