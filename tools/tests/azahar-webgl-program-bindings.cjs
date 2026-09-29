const assert = require('node:assert/strict');
const fs = require('node:fs');
const input = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
const bind = new Function('GL', 'GLctx', 'program', 'previous',
  `return (${input.programBindings})(program, previous);`);
const target = {}, previous = {};
let active = previous;
const uniforms = [], blocks = [];
const ctx = {
  currentProgram: previous,
  useProgram(program) { active = program; },
  getProgramParameter() { throw Error('Must not enumerate active uniforms'); },
  getActiveUniform() { throw Error('Must not enumerate active uniforms'); },
  getUniformLocation(program, name) {
    assert.equal(program, target);
    return ['tex0', 'texture_buffer_lut_lf', 'tex_color'].includes(name) ? {name} : null;
  },
  uniform1i(location, unit) {
    assert.equal(active, target);
    uniforms.push([location.name, unit]);
  },
  getUniformBlockIndex(program, name) {
    assert.equal(program, target);
    return name === 'fs_data' ? 0 : name === 'pica_generic_config' ? 7 : 0xffffffff;
  },
  uniformBlockBinding(program, index, binding) {
    assert.equal(program, target);
    blocks.push([index, binding]);
  },
};
const GL = {programs: [null, target, previous]};
bind(GL, ctx, 1, 2);
assert.deepEqual(uniforms, [['tex0', 0], ['texture_buffer_lut_lf', 3], ['tex_color', 7]]);
assert.deepEqual(blocks, [[0, 2], [7, 3]]);
assert.equal(active, previous);
assert.equal(ctx.currentProgram, previous);
bind(GL, ctx, 1, 0);
assert.equal(active, null);
ctx.getUniformLocation = () => { throw Error('lost context'); };
assert.throws(() => bind(GL, ctx, 1, 2), /lost context/);
assert.equal(active, previous);
assert.equal(ctx.currentProgram, previous);
console.log('PASS: named resource bindings skip inactive resources and uniform enumeration; state restored on success and exception');
