// The HL2's gateware row in the radio drawer (hl2GatewareRow(), webapp/app/app.html; docs/architecture/hl2.md,
// Gateware): the version the main board's discovery reply gave, so a report names it, marked when it is not the
// one Micro-FT8 is tested on.

'use strict';
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const src = fs.readFileSync(path.join(__dirname, '../app/app.html'), 'utf8');

function extractFunction(text, name) {
  const start = text.indexOf('function ' + name + '(');
  assert(start !== -1, 'function not found in app.html: ' + name);
  let depth = 0, i = text.indexOf('{', start);
  for (; i < text.length; i++) {
    if (text[i] === '{') depth++;
    if (text[i] === '}' && --depth === 0) { i++; break; }
  }
  return text.slice(start, i);
}
const tested = /var HL2_TESTED_GATEWARE = [^;]*;/.exec(src);
assert(tested, 'HL2_TESTED_GATEWARE not found in app.html');

const sb = {};
vm.createContext(sb);
vm.runInContext(tested[0] + '\n' + extractFunction(src, 'hl2GatewareRow'), sb);

// JSON, since an array made in the sandbox has the sandbox's Array, which deepStrictEqual tells apart.
const row = (cfg) => JSON.stringify(sb.hl2GatewareRow(cfg));
assert.strictEqual(row({ radio: '6', hl2_gw: '74.2' }), JSON.stringify(['Gateware', '74.2']), 'the tested version, plain');
assert.strictEqual(row({ radio: '6', hl2_gw: '74.3' }), JSON.stringify(['Gateware', '74.3 · untested: Micro-FT8 is tested on 74.2']), 'another version warns');
assert.strictEqual(sb.hl2GatewareRow({ radio: '6' }), null, 'no row before the HL2 has answered');
assert.strictEqual(sb.hl2GatewareRow({ radio: '1', hl2_gw: '74.2' }), null, 'no row when the radio is not the HL2');

console.log('PASS: HL2 gateware row');
