// Runs the repository's presentation shaders in a separate, unattached WebGL2 canvas.
// Never binds or changes any resource belonging to the running emulator.
(sources) => {
    const canvas = document.createElement('canvas');
    canvas.width = canvas.height = 1;
    const gl = canvas.getContext('webgl2', { antialias: false });
    if (!gl) throw new Error('WebGL2 is unavailable');
    const programs = [];
    const textures = [];
    const shaders = [];
    const check = (condition, message) => { if (!condition) throw new Error(message); };
    const compile = (type, text) => {
        const shader = gl.createShader(type);
        shaders.push(shader);
        gl.shaderSource(shader, text);
        gl.compileShader(shader);
        check(gl.getShaderParameter(shader, gl.COMPILE_STATUS), gl.getShaderInfoLog(shader));
        return shader;
    };
    const encode = (rgba, mode) => mode === 1
        ? [rgba[2], rgba[1], rgba[0], rgba[3]] : mode === 2 ? [...rgba].reverse() : rgba;
    const equal = (a, b) => a.every((value, i) => Math.abs(value - b[i]) <= 1);
    let cases = 0;
    let framebuffer;
    try {
        const vertex = compile(gl.VERTEX_SHADER, `#version 300 es
            out vec2 frag_tex_coord;
            void main() {
                vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
                gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
                frag_tex_coord = vec2(0.5);
            }`);
        for (let i = 0; i < 3; ++i) {
            const texture = gl.createTexture();
            textures.push(texture);
            gl.activeTexture(gl.TEXTURE0 + i);
            gl.bindTexture(gl.TEXTURE_2D, texture);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
            gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
            gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, 1, 1, 0, gl.RGBA,
                          gl.UNSIGNED_BYTE, null);
        }
        framebuffer = gl.createFramebuffer();
        gl.bindFramebuffer(gl.FRAMEBUFFER, framebuffer);
        gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D,
                                textures[2], 0);
        check(gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE,
              'Color test framebuffer is incomplete');
        gl.viewport(0, 0, 1, 1);
        gl.disable(gl.DITHER);
        const left = [224, 17, 53, 201], right = [31, 119, 211, 157];
        for (const [name, source] of Object.entries(sources)) {
            const fragment = compile(gl.FRAGMENT_SHADER, '#version 300 es\nprecision highp float;\n' +
                source.replace(/layout\s*\(\s*binding\s*=\s*\d+\s*\)/g, '')
                    .replace(/layout\s*\(\s*location\s*=\s*\d+\s*\)\s*in\b/g, 'in'));
            const program = gl.createProgram();
            programs.push(program);
            gl.attachShader(program, vertex);
            gl.attachShader(program, fragment);
            gl.linkProgram(program);
            check(gl.getProgramParameter(program, gl.LINK_STATUS), gl.getProgramInfoLog(program));
            gl.useProgram(program);
            const uniform = key => gl.getUniformLocation(program, key);
            gl.uniform1i(uniform('color_texture'), 0);
            gl.uniform1i(uniform('color_texture_r'), 1);
            const draw = (leftMode, rightMode, eye) => {
                for (let i = 0; i < 2; ++i) {
                    gl.activeTexture(gl.TEXTURE0 + i);
                    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 1, 1, gl.RGBA,
                        gl.UNSIGNED_BYTE, new Uint8Array(encode(i ? right : left,
                                                               i ? rightMode : leftMode)));
                }
                gl.uniform1i(uniform('color_swizzle'), leftMode);
                gl.uniform1i(uniform('color_swizzle_r'), rightMode);
                gl.uniform1i(uniform('reverse_interlaced'), 0);
                gl.uniform4f(uniform('o_resolution'), eye ? 3 : 1, 1, 1, 1);
                gl.drawArrays(gl.TRIANGLES, 0, 3);
                const pixel = new Uint8Array(4);
                gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, pixel);
                check(gl.getError() === gl.NO_ERROR, `${name}: WebGL error`);
                return pixel;
            };
            for (const eye of name.includes('interlaced') ? [0, 1] : [0]) {
                const expected = draw(0, 0, eye);
                if (!name.includes('anaglyph')) {
                    check(equal(expected, eye ? right : left), `${name}: host RGBA colors`);
                }
                for (let l = 0; l < 3; ++l) {
                    for (const r of name === 'opengl_present.frag' ? [0] : [0, 1, 2]) {
                        check(equal(draw(l, r, eye), expected),
                              `${name}: channel order left=${l}, right=${r}, eye=${eye}`);
                        ++cases;
                    }
                }
            }
        }
        return { passed: true, shaders: programs.length, cases };
    } finally {
        for (const program of programs) gl.deleteProgram(program);
        for (const shader of shaders) gl.deleteShader(shader);
        for (const texture of textures) gl.deleteTexture(texture);
        if (framebuffer) gl.deleteFramebuffer(framebuffer);
        gl.getExtension('WEBGL_lose_context')?.loseContext();
    }
}
