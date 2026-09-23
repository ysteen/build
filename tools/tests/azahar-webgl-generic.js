async function verifyAzaharGeneric(input) {
    const canvas = document.createElement('canvas');
    canvas.width = canvas.height = 4;
    const gl = canvas.getContext('webgl2', {antialias: false, preserveDrawingBuffer: true});
    if (!gl) throw new Error('WebGL2 unavailable');
    const extension = gl.getExtension('KHR_parallel_shader_compile');
    const prefix = '#version 300 es\nprecision highp float;\nprecision highp int;\n';
    const programs = new Map(), shaders = [], buffers = [], textures = [];
    const compile = (type, source) => {
        const shader = gl.createShader(type);
        shaders.push(shader);
        gl.shaderSource(shader, prefix + source);
        gl.compileShader(shader);
        return shader;
    };
    const vertex = compile(gl.VERTEX_SHADER, `
layout(location=0) in vec2 position;
uniform vec4 test_primary;
uniform vec4 test_coords;
uniform vec2 test_depth;
out vec4 primary_color;
out vec2 texcoord0;
out vec2 texcoord1;
out vec2 texcoord2;
out float texcoord0_w;
out vec4 normquat;
out vec3 view;
void main() {
    gl_Position = vec4(position * test_depth.y, test_depth.x * test_depth.y, test_depth.y);
    primary_color = test_primary;
    texcoord0 = test_coords.xy + position * 0.7;
    texcoord1 = test_coords.yx + position * 0.3;
    texcoord2 = test_coords.zw - position * 0.5;
    texcoord0_w = 1.4;
    normquat = vec4(0,0,0,1);
    view = vec3(0,0,1);
}`);
    let maxCompileCallMs = 0, maxLinkCallMs = 0;
    const getProgram = async (source) => {
        if (programs.has(source)) return programs.get(source);
        let start = performance.now();
        const fragment = compile(gl.FRAGMENT_SHADER, source);
        maxCompileCallMs = Math.max(maxCompileCallMs, performance.now() - start);
        const program = gl.createProgram();
        gl.attachShader(program, vertex);
        gl.attachShader(program, fragment);
        start = performance.now();
        gl.linkProgram(program);
        maxLinkCallMs = Math.max(maxLinkCallMs, performance.now() - start);
        if (extension) {
            while (!gl.getProgramParameter(program, extension.COMPLETION_STATUS_KHR))
                await new Promise(resolve => setTimeout(resolve, 4));
        }
        if (!gl.getProgramParameter(program, gl.LINK_STATUS))
            throw new Error(gl.getProgramInfoLog(program) + '\n' + gl.getShaderInfoLog(fragment));
        gl.useProgram(program);
        for (const [name, unit] of [['tex0',0], ['tex1',1], ['tex2',2], ['texture_buffer_lut_lf',3]])
            gl.uniform1i(gl.getUniformLocation(program, name), unit);
        for (const [name, binding] of [['fs_data',2], ['pica_generic_config',3]]) {
            const index = gl.getUniformBlockIndex(program, name);
            if (index !== gl.INVALID_INDEX) {
                const size = gl.getActiveUniformBlockParameter(program, index, gl.UNIFORM_BLOCK_DATA_SIZE);
                if (size !== (binding === 2 ? 1328 : 144)) throw new Error(`Block ${name} size ${size}`);
                gl.uniformBlockBinding(program, index, binding);
            }
        }
        const entry = {program, primary: gl.getUniformLocation(program,'test_primary'),
            coords: gl.getUniformLocation(program,'test_coords'), depth: gl.getUniformLocation(program,'test_depth')};
        programs.set(source, entry);
        return entry;
    };
    const makeBuffer = (target, data, binding) => {
        const buffer = gl.createBuffer(); buffers.push(buffer);
        gl.bindBuffer(target, buffer); gl.bufferData(target, data, gl.DYNAMIC_DRAW);
        if (binding !== undefined) gl.bindBufferBase(target, binding, buffer);
        return buffer;
    };
    makeBuffer(gl.ARRAY_BUFFER, new Float32Array([-1,-1,3,-1,-1,3]));
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0,2,gl.FLOAT,false,0,0);
    const uniformBuffer = makeBuffer(gl.UNIFORM_BUFFER,1328,2);
    const configBuffer = makeBuffer(gl.UNIFORM_BUFFER,144,3);
    const uniformData = new ArrayBuffer(1328), ints = new Int32Array(uniformData), floats = new Float32Array(uniformData);
    ints[0] = 1; ints[6] = ints[7] = 1; ints[8] = ints[9] = 3;
    floats.set([0.11,0.33,0.77],176/4);
    for (let stage=0; stage<6; ++stage)
        floats.set([0.13 + stage*0.09, 0.27 + stage*0.04, 0.63 - stage*0.07, 0.79 - stage*0.05],1136/4+stage*4);
    floats.set([0.31,0.43,0.19,0.61],1232/4);
    for (let unit=0; unit<4; ++unit) {
        const texture = gl.createTexture(); textures.push(texture);
        gl.activeTexture(gl.TEXTURE0+unit); gl.bindTexture(gl.TEXTURE_2D,texture);
        gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.NEAREST);
        if (unit<3) {
            const texels = Uint8Array.from({length:64},(_,i)=>(17+unit*61+i*37)%256);
            gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA8,4,4,0,gl.RGBA,gl.UNSIGNED_BYTE,texels);
            floats.set([0.17+unit*0.21,0.71-unit*0.13,0.41,0.83],1264/4+unit*4);
        } else {
            const lut = new Float32Array(2048*2);
            for(let i=0;i<128;++i) { lut[i*2] = i/128; lut[i*2+1] = 1/128; }
            gl.texImage2D(gl.TEXTURE_2D,0,gl.RG32F,2048,1,0,gl.RG,gl.FLOAT,lut);
        }
    }
    let comparisons=0, changedPixels=0, depthComparisons=0;
    const check = label => { const error=gl.getError(); if(error) throw new Error(`${label}: WebGL ${error}`); };
    const genericStart = performance.now();
    const generic = await getProgram(input.generic);
    const genericCompileMs = performance.now()-genericStart;
    const render = (entry,row,seed,alpha,scissor,ref,depthTest) => {
        gl.useProgram(entry.program);
        gl.uniform4f(entry.primary,0.25+seed*0.07,0.5-seed*0.11,0.75-seed*0.17,0.5+seed*0.19);
        gl.uniform4f(entry.coords,0.5-seed*0.4,0.5+seed*0.5,0.3+seed*0.3,0.8-seed*0.6);
        gl.uniform2f(entry.depth,0.17+seed*0.13,0.75+seed*0.65);
        ints[1]=ref; ints[18]=alpha; ints[19]=scissor;
        floats[2]=0.7; floats[3]=0.61;
        gl.bindBuffer(gl.UNIFORM_BUFFER,uniformBuffer);
        gl.bufferSubData(gl.UNIFORM_BUFFER,0,new Uint8Array(uniformData));
        gl.bindBuffer(gl.UNIFORM_BUFFER,configBuffer);
        gl.bufferSubData(gl.UNIFORM_BUFFER,0,new Uint32Array(row.config));
        if(depthTest) gl.enable(gl.DEPTH_TEST); else gl.disable(gl.DEPTH_TEST);
        gl.depthFunc(gl.LESS); gl.depthMask(true);
        gl.clearDepth(0.48);
        gl.clearColor(1/255,2/255,3/255,4/255);
        gl.clear(gl.COLOR_BUFFER_BIT|gl.DEPTH_BUFFER_BIT);
        gl.drawArrays(gl.TRIANGLES,0,3);
        const pixels=new Uint8Array(64);
        gl.readPixels(0,0,4,4,gl.RGBA,gl.UNSIGNED_BYTE,pixels);
        check(row.label);
        return pixels;
    };
    const verify = (entry,row,seed,alpha=1,scissor=0,ref=128,depth=false) => {
        const expected=render(entry,row,seed,alpha,scissor,ref,depth);
        const actual=render(generic,row,seed,alpha,scissor,ref,depth);
        const mismatch=expected.findIndex((value,i)=>value!==actual[i]);
        if(mismatch>=0) throw new Error(`Mismatch ${row.label}, seed=${seed}, alpha=${alpha}, scissor=${scissor}, ref=${ref}, depth=${depth}, byte=${mismatch}: ${expected[mismatch]} != ${actual[mismatch]}`);
        changedPixels += actual.filter((v,i)=>v!==(i%4)+1).length;
        ++comparisons; if(depth) ++depthComparisons;
    };
    try {
        gl.viewport(0,0,4,4); gl.disable(gl.DITHER); check('setup');
        for(const row of input.fixtures) {
            const entry=await getProgram(row.source);
            for(let seed=0; seed<3; ++seed) verify(entry,row,seed);
            if(row.label.startsWith('depth-'))
                for(let seed=0;seed<3;++seed) verify(entry,row,seed,1,0,128,true);
            if(row===input.fixtures[0]) {
                for(let alpha=0;alpha<8;++alpha)
                    for(const scissor of [0,1,3])
                        for(const ref of [0,128,255]) verify(entry,row,0,alpha,scissor,ref);
            }
            await new Promise(resolve=>setTimeout(resolve,0));
        }
        if(!changedPixels) throw new Error('No rendered pixels');
        return {passed:true,fixtures:input.fixtures.length,comparisons,pixelsPerComparison:16,
            depthComparisons,unsupportedRejected:input.unsupportedRejected,uniformSizes:[1328,144],
            parallelCompile:!!extension,genericCompileMs,maxCompileCallMs,maxLinkCallMs};
    } finally {
        gl.useProgram(null);
        for(const entry of programs.values()) gl.deleteProgram(entry.program);
        for(const shader of shaders) gl.deleteShader(shader);
        for(const buffer of buffers) gl.deleteBuffer(buffer);
        for(const texture of textures) gl.deleteTexture(texture);
        gl.getExtension('WEBGL_lose_context')?.loseContext();
    }
}
