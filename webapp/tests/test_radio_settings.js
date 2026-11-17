#!/usr/bin/env node
// The radio's settings (webapp/app/radio_settings.js; docs/architecture/webapp.md, Settings › Radio & bands: In
// the radio): the page refuses a value the radio's own menu editor would not take, in the main board's words
// (qmx_setting_range_text(), host_mock's test_qmx_menu_write.cpp), since the QMX stores whatever an MM write
// sends. Runs the shared file as app.html and swr.html load it.
'use strict';
const vm = require('vm');
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const ctx = vm.createContext({});
vm.runInContext(fs.readFileSync(path.join(__dirname, '..', 'app', 'radio_settings.js'), 'utf8') +
  '\nthis.RADIO_SETTINGS = RADIO_SETTINGS;', ctx);
const why = ctx.radioSettingRefusal;
const tune = ctx.radioSetting('radio_tune_pct');
const volts = ctx.radioSetting('radio_pa_volts');

assert(tune && volts, 'both settings by key');
assert.strictEqual(ctx.radioSetting('radio_bands'), null, 'anything else is no setting');
assert.strictEqual(JSON.stringify(ctx.RADIO_SETTINGS.map((s) => s.key)), '["radio_tune_pct","radio_pa_volts"]',
  'the keys radio_setting_key() gives, in radio_setting_t order');
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
