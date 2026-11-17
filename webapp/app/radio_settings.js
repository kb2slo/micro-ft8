// The radio's own settings that the app shows and writes, shared by app.html (Settings › Radio & bands ›
// In the radio, and Operate's Tune readout) and swr.html (the sweep's Tune power). Plain globals, like
// bands.js. Each is held by the radio, read when it connects and written only on the operator's change
// (POST /api/radio/setting, docs/architecture/radio.md, The radio's settings in Radio & bands). Each
// takes what the radio's own menu editor takes, digits before the point and decimals after, as the main
// board checks (kQmxSettingShape, components/radio_control/include/qmx_menu_read.h): the QMX stores
// anything an MM write sends.
var RADIO_SETTINGS = [
  { key: 'radio_tune_pct', name: 'Tune power', digits: 2, decimals: 0 },
  { key: 'radio_pa_volts', name: 'Max. PA voltage', digits: 2, decimals: 1 },
];

// The Tune % the SWR sweep reads true at (docs/architecture/radio.md, QMX SWR Tune over CAT: 25% the
// floor, 10% biased high and noisy). Settings and the sweep both offer to set it in one tap.
var TUNE_PCT_FOR_SWEEP = 25;

function radioSetting(key) {
  for (var i = 0; i < RADIO_SETTINGS.length; i++) if (RADIO_SETTINGS[i].key === key) return RADIO_SETTINGS[i];
  return null;
}

// Why a value cannot be sent for setting `s`, or null when it can: the words the main board uses.
function radioSettingRefusal(s, v) {
  if (!/^\d+(\.\d+)?$/.test(v)) return s.name + ': a number';
  var parts = v.split('.');
  if (parts[0].length <= s.digits && (parts[1] || '').length <= s.decimals) return null;
  var max = new Array(s.digits + 1).join('9') + (s.decimals ? '.' + new Array(s.decimals + 1).join('9') : '');
  return s.name + ': 0 to ' + max + (s.decimals === 1 ? ', one decimal place' : s.decimals > 1 ? ', two or more decimal places' : '');
}
