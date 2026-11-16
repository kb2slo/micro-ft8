#!/usr/bin/env node
// Settings › Radio & bands › In the radio (docs/architecture/webapp.md): the page refuses a value the radio's
// own menu editor would not take, in the main board's words (qmx_setting_range_text(), host_mock's
// test_qmx_menu_write.cpp), since the QMX stores whatever an MM write sends. Runs the real function from app.html.
'use strict';
const vm = require('vm');
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const src = fs.readFileSync(path.join(__dirname, '..', 'app', 'app.html'), 'utf8');

function extract(text, name) {
  const start = text.indexOf('function ' + name + '(');
  assert(start !== -1, 'function not found in app.html: ' + name);
  let depth = 0, i = text.indexOf('{', start);
  for (; i < text.length; i++) {
    if (text[i] === '{') depth++;
    if (text[i] === '}' && --depth === 0) { i++; break; }
  }
  return text.slice(start, i);
}
function extractVar(text, name) {
  const start = text.indexOf('var ' + name + ' = [');
  assert(start !== -1, 'var not found in app.html: ' + name);
  return text.slice(start, text.indexOf('];', start) + 2);
}

const sandbox = {};
vm.createContext(sandbox);
vm.runInContext(extractVar(src, 'RADIO_SETTINGS') + '\n' + extract(src, 'radioSettingRefusal') +
  '\nthis.RADIO_SETTINGS = RADIO_SETTINGS;', sandbox);
const [tune, volts] = sandbox.RADIO_SETTINGS;
const why = sandbox.radioSettingRefusal;

assert.strictEqual(tune.key, 'radio_tune_pct');
assert.strictEqual(volts.key, 'radio_pa_volts');
['0', '30', '99'].forEach((v) => assert.strictEqual(why(tune, v), null, 'Tune % ' + v));
assert.strictEqual(why(tune, '250'), 'Tune power: 0 to 99', 'the value a QMX stored on 2026-11-16');
assert.strictEqual(why(tune, '100'), 'Tune power: 0 to 99');
assert.strictEqual(why(tune, '30.5'), 'Tune power: 0 to 99');
['11.5', '9', '99.9', '11.0'].forEach((v) => assert.strictEqual(why(volts, v), null, 'PA voltage ' + v));
assert.strictEqual(why(volts, '9.25'), 'Max. PA voltage: 0 to 99.9, one decimal place');
assert.strictEqual(why(volts, '100'), 'Max. PA voltage: 0 to 99.9, one decimal place');
assert.strictEqual(why(volts, '-1'), 'Max. PA voltage: a number');
assert.strictEqual(why(tune, ''), 'Tune power: a number');

console.log('radio settings: ok');
