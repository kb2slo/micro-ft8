// Shared callsign-info card, operator-requested 2026-09-22: "make it a
// common experience any time we select a callsign anywhere in the web
// app." Loaded by both app.html (Operate's decode rows, QSOs' log rows)
// and map.html (QSO/Decode/active-spot pins) via a plain <script> tag --
// there's no module system across these pages (same reasoning every other
// shared helper on this page set already accepts), so this file is a
// single self-contained IIFE that injects its own markup/styles into
// whichever page loads it and exposes one small API, `window.CallInfo`,
// rather than assuming the host page already defines any particular
// global. The host supplies what only *it* can know (the operator's own
// grid, a cached grid-lookup, QSO history, decode history) through a
// small options object; this file owns everything that's genuinely the
// same everywhere it's shown -- grid/distance/bearing math, and now
// (operator-requested, same day) live POTA/SOTA spot awareness, since a
// callsign's current activation is exactly the kind of thing that
// shouldn't depend on which page happened to already have that data
// loaded.
(function () {
'use strict';

function esc(s) {
  return String(s == null ? '' : s).replace(/[&<>"]/g, function (c) {
    return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c];
  });
}
function toRad(d) { return d * Math.PI / 180; }
function gridToLatLon(grid) {
  if (!grid || grid.length < 4) return null;
  var g = grid.toUpperCase();
  var A = 'A'.charCodeAt(0), Z0 = '0'.charCodeAt(0);
  var fl = g.charCodeAt(0) - A, fu = g.charCodeAt(1) - A;
  var sl = g.charCodeAt(2) - Z0, su = g.charCodeAt(3) - Z0;
  if (fl < 0 || fl > 17 || fu < 0 || fu > 17 || sl < 0 || sl > 9 || su < 0 || su > 9) return null;
  var lon = fl * 20 - 180 + sl * 2;
  var lat = fu * 10 - 90 + su * 1;
  if (g.length >= 6) {
    var tl = g.charCodeAt(4) - A, tu = g.charCodeAt(5) - A;
    if (tl >= 0 && tl <= 23 && tu >= 0 && tu <= 23) {
      lon += tl * (2 / 24) + (2 / 24) / 2;
      lat += tu * (1 / 24) + (1 / 24) / 2;
      return { lat: lat, lon: lon };
    }
  }
  lon += 1;
  lat += 0.5;
  return { lat: lat, lon: lon };
}
function greatCircle(a, b) {
  var R = 6371;
  var dLat = toRad(b.lat - a.lat), dLon = toRad(b.lon - a.lon);
  var la1 = toRad(a.lat), la2 = toRad(b.lat);
  var h = Math.sin(dLat / 2) ** 2 + Math.cos(la1) * Math.cos(la2) * Math.sin(dLon / 2) ** 2;
  var km = R * 2 * Math.atan2(Math.sqrt(h), Math.sqrt(1 - h));
  var y = Math.sin(dLon) * Math.cos(la2);
  var x = Math.cos(la1) * Math.sin(la2) - Math.sin(la1) * Math.cos(la2) * Math.cos(dLon);
  var bearing = (Math.atan2(y, x) * 180 / Math.PI + 360) % 360;
  return { km: km, bearing: bearing };
}
var distUnit = 'km';
try {
  var du = localStorage.getItem('microft8_dist_unit_v1');
  if (du === 'km' || du === 'mi') distUnit = du;
} catch (e) { /* default stands */ }
function fmtKmBearing(km, bearingDeg) {
  var value = distUnit === 'mi' ? km * 0.621371 : km;
  var bearing = String(Math.round(bearingDeg)).padStart(3, '0') + '°';
  return Math.round(value).toLocaleString() + ' ' + distUnit + ' ' + bearing;
}
function fmtDuration(totalSeconds) {
  var s = Math.max(0, Math.floor(totalSeconds));
  var d = Math.floor(s / 86400); s -= d * 86400;
  var h = Math.floor(s / 3600); s -= h * 3600;
  var m = Math.floor(s / 60); s -= m * 60;
  if (d > 0) return d + 'd ' + h + 'h';
  if (h > 0) return h + 'h ' + m + 'm';
  if (m > 0) return m + 'm ' + s + 's';
  return s + 's';
}
function fmtAgo(ts) {
  if (!ts) return '';
  var secs = (Date.now() - ts) / 1000;
  return secs < 1 ? 'just now' : fmtDuration(secs) + ' ago';
}
function parseUtcIso(s) {
  if (!s) return NaN;
  return Date.parse(/Z$/.test(s) ? s : s + 'Z');
}
function fmtRst(v) {
  return (v === -99 || v == null) ? '—' : (v > 0 ? '+' + v : String(v));
}
// Same {grid, ts} shape app.html's learnCallsignGrid() writes -- read
// directly from the shared localStorage key rather than depending on the
// host page to hand it over, since this is exactly the kind of "same
// everywhere" data this file exists to own.
function cachedGrid(call) {
  try {
    var raw = localStorage.getItem('microft8_callsign_grids_v1');
    var cache = raw ? JSON.parse(raw) : null;
    var entry = cache && cache[call];
    if (!entry) return '';
    return typeof entry === 'string' ? entry : (entry.grid || '');
  } catch (e) { return ''; }
}

// ---- live POTA/SOTA spot awareness (operator-requested 2026-09-22) ----
// On-demand, not a background poll: fetched the first time a card actually
// needs it, cached for a short window (SPOT_CACHE_MS) so browsing several
// decode rows in a row doesn't refetch on every tap, but never so long
// that a stale "still active" reading would mislead. Filtered to FT8 --
// this app only operates FT8/FT4, so a CW/SSB/FM activation isn't
// actionable here the way an FT8 one is, same reasoning the map's own
// Spots layer already applies.
var POTA_ACTIVATOR_URL = 'https://api.pota.app/spot/activator';
var SOTA_SPOTS_URL = 'https://api-db2.sota.org.uk/api/spots/50/all/all';
var SPOT_CACHE_MS = 60000;
var spotCache = null; // [{call, program, ref, name, band, ts, points}]
var spotCacheAt = 0;
var spotFetchPromise = null;
var BAND_EDGES_KHZ = [
  { name: '160m', lo: 1800, hi: 2000 }, { name: '80m', lo: 3500, hi: 4000 },
  { name: '60m', lo: 5250, hi: 5450 }, { name: '40m', lo: 7000, hi: 7300 },
  { name: '30m', lo: 10100, hi: 10150 }, { name: '20m', lo: 14000, hi: 14350 },
  { name: '17m', lo: 18068, hi: 18168 }, { name: '15m', lo: 21000, hi: 21450 },
  { name: '12m', lo: 24890, hi: 24990 }, { name: '10m', lo: 28000, hi: 29700 },
  { name: '6m', lo: 50000, hi: 54000 }, { name: '2m', lo: 144000, hi: 148000 }
];
function khzToBandName(khz) {
  for (var i = 0; i < BAND_EDGES_KHZ.length; i++) {
    var e = BAND_EDGES_KHZ[i];
    if (khz >= e.lo && khz <= e.hi) return e.name;
  }
  return '';
}
async function loadActiveSpots() {
  if (spotCache && Date.now() - spotCacheAt < SPOT_CACHE_MS) return spotCache;
  if (spotFetchPromise) return spotFetchPromise;
  spotFetchPromise = (async function () {
    var rows = [];
    try {
      var pr = await fetch(POTA_ACTIVATOR_URL, { cache: 'no-store' });
      var pj = await pr.json();
      pj.forEach(function (s) {
        if (!s.mode || s.mode.toUpperCase() !== 'FT8') return;
        rows.push({ call: (s.activator || '').toUpperCase(), program: 'POTA', ref: s.reference,
                    name: s.parkName || s.name, band: khzToBandName(parseFloat(s.frequency)), ts: s.spotTime });
      });
    } catch (e) { /* POTA down this tick -- SOTA half may still be good */ }
    try {
      var sr = await fetch(SOTA_SPOTS_URL, { cache: 'no-store' });
      var sj = await sr.json();
      sj.forEach(function (s) {
        if (!s.summitCode || s.summitCode === 'DEPRECATED') return;
        if (!s.mode || s.mode.toUpperCase() !== 'FT8') return;
        rows.push({ call: (s.activatorCallsign || '').toUpperCase(), program: 'SOTA', ref: s.summitCode,
                    name: s.summitName, band: khzToBandName(s.frequency * 1000), ts: s.timeStamp, points: s.points });
      });
    } catch (e) { /* SOTA down this tick -- POTA half may still be good */ }
    spotCache = rows;
    spotCacheAt = Date.now();
    spotFetchPromise = null;
    return rows;
  })();
  return spotFetchPromise;
}
function officialLink(program, ref) {
  if (program === 'POTA') return 'https://pota.app/#/park/' + encodeURIComponent(ref);
  // A SOTA summit code is itself two path segments ("W7O/CC-001" ==
  // association/summit) -- encodeURIComponent on the whole string escapes
  // that meaningful "/" to %2F, which SOTA's own router doesn't decode
  // back (found live 2026-09-22: %2F lands on the site's generic homepage,
  // not the summit page). Encode each segment on its own and rejoin with a
  // literal "/" instead.
  if (program === 'SOTA') return 'https://www.sotadata.org.uk/en/summit/' + ref.split('/').map(encodeURIComponent).join('/');
  return '';
}

// ---- modal markup/styles, injected once ----
// #callInfoModal[hidden] is not redundant with the UA stylesheet's own
// [hidden]{display:none} -- #callInfoModal's own display:flex (an author
// rule) wins over it purely on origin precedence regardless of specificity,
// so the modal would stay visible despite hidden=true without this. Found
// live 2026-09-22 on a host page (map.html) that had no global
// [hidden]{display:none!important} override of its own (app.html does, so
// it never hit there) -- self-contained here rather than assuming every
// future host page defines one, same reasoning this whole file exists for.
var STYLE = '#callInfoModal{position:fixed;inset:0;z-index:20;display:flex;align-items:flex-end;justify-content:center}' +
  '#callInfoModal[hidden]{display:none}' +
  '#callInfoBackdrop{position:absolute;inset:0;background:rgba(0,0,0,.4)}' +
  '#callInfoCard{position:relative;width:100%;max-width:26rem;max-height:80vh;overflow-y:auto;background:Canvas;color:CanvasText;border-radius:.75rem .75rem 0 0;padding:1rem;box-shadow:0 -2px 16px rgba(0,0,0,.25);box-sizing:border-box}' +
  '#callInfoHead{display:flex;justify-content:space-between;align-items:center;margin-bottom:.4rem}' +
  '#callInfoCallName{font-family:ui-monospace,monospace;font-weight:700;font-size:1.2rem}' +
  '#callInfoClose{border:0;background:transparent;font-size:1.5rem;line-height:1;padding:.2rem .6rem;color:GrayText}' +
  '.ci-row{padding:.5rem 0;border-top:1px solid color-mix(in srgb, GrayText 20%, transparent);font-size:.85rem}' +
  '.ci-label{font-size:.7rem;color:GrayText;text-transform:uppercase;letter-spacing:.04em;margin-bottom:.15rem}' +
  '.ci-heard{font-family:ui-monospace,monospace;font-size:.75rem;padding:.15rem 0}' +
  '.ci-link{display:inline-block;margin-top:.3rem;color:var(--accent, #1d4ed8);font-size:.8rem}' +
  '#callInfoQrz{display:block;text-align:center;margin-top:.8rem;padding:.6rem;background:ButtonFace;color:var(--accent, #1d4ed8);border:1px solid GrayText;border-radius:.4rem;text-decoration:none;font-weight:700}';
var HTML = '<div id=callInfoModal hidden>' +
  '<div id=callInfoBackdrop></div>' +
  '<div id=callInfoCard>' +
    '<div id=callInfoHead><span id=callInfoCallName></span>' +
      '<button id=callInfoClose type=button aria-label=Close>&times;</button></div>' +
    '<div id=callInfoBody></div>' +
    '<a id=callInfoQrz href=# target=_blank rel="noopener">View on QRZ.com</a>' +
  '</div>' +
'</div>';
function inject() {
  var styleEl = document.createElement('style');
  styleEl.textContent = STYLE;
  document.head.appendChild(styleEl);
  var wrap = document.createElement('div');
  wrap.innerHTML = HTML;
  document.body.appendChild(wrap.firstChild);
  document.getElementById('callInfoClose').onclick = hide;
  document.getElementById('callInfoBackdrop').onclick = hide;
}
function hide() { document.getElementById('callInfoModal').hidden = true; }

// ---- card assembly ----
// opts (all optional -- a section is simply omitted when the host page
// can't supply it, e.g. map.html has no session decode history at all):
//   myGrid            operator's own grid (string)
//   qsoHistory(call)  -> [{when, band, freq, rstSent, rstRcvd}] | null
//   heardThisSession(call) -> [{when, band, snr, text}] | null
async function show(call, opts) {
  if (!call) return;
  call = call.toUpperCase();
  opts = opts || {};

  var grid = cachedGrid(call);
  var distBearing = (grid && opts.myGrid) ? (function () {
    var a = gridToLatLon(opts.myGrid), b = gridToLatLon(grid);
    if (!a || !b) return '';
    var gc = greatCircle(a, b);
    return fmtKmBearing(gc.km, gc.bearing);
  })() : '';

  document.getElementById('callInfoCallName').textContent = call;
  var body = '';
  body += '<div class=ci-row><div class=ci-label>Grid square</div>' +
    (grid
      ? esc(grid) + (distBearing ? ' · ' + esc(distBearing) : '') +
        ' <span style="color:GrayText;font-size:.75rem">(cached)</span>'
      : 'Not seen sending a grid yet this session') +
    '</div>';

  if (typeof opts.qsoHistory === 'function') {
    var qsoHist = opts.qsoHistory(call) || [];
    body += '<div class=ci-row><div class=ci-label>QSOs in log (' + qsoHist.length + ')</div>' +
      (qsoHist.length
        ? qsoHist.slice(0, 6).map(function (q) {
            return '<div class=ci-heard>' + esc(q.when) +
              (q.band ? ' · ' + esc(q.band) : '') + (q.freq ? ' · ' + esc(q.freq) + ' MHz' : '') +
              ' · ' + fmtRst(q.rstSent) + '/' + fmtRst(q.rstRcvd) + '</div>';
          }).join('') +
          ' <span style="color:GrayText;font-size:.75rem">(only what has loaded so far)</span>'
        : 'No QSOs with this callsign in the loaded log') +
      '</div>';
  }

  if (typeof opts.heardThisSession === 'function') {
    var heard = opts.heardThisSession(call) || [];
    body += '<div class=ci-row><div class=ci-label>Heard this session (' + heard.length + ')</div>' +
      (heard.length
        ? heard.slice(0, 6).map(function (h) {
            return '<div class=ci-heard>' + esc(h.when) + (h.band ? ' · ' + esc(h.band) : '') +
              ' · ' + (h.snr > 0 ? '+' : '') + h.snr + ' dB · ' + esc(h.text) + '</div>';
          }).join('')
        : 'No decodes from this callsign in the current decode history') +
      '</div>';
  }

  body += '<div class=ci-row><div class=ci-label>Country / DXCC</div>' +
    'Not looked up yet in this version -- check QRZ below.</div>';

  // POTA/SOTA row: placeholder immediately (never blocks the rest of the
  // card on a network round trip), filled in once the fetch resolves --
  // the modal may already be closed by then, so this only ever touches the
  // DOM if it's still showing this same callsign.
  body += '<div class=ci-row id=ciPotaSota><div class=ci-label>POTA / SOTA</div>' +
    '<span style="color:GrayText">checking…</span></div>';

  document.getElementById('callInfoBody').innerHTML = body;
  document.getElementById('callInfoQrz').href = 'https://www.qrz.com/db/' + encodeURIComponent(call);
  document.getElementById('callInfoModal').hidden = false;

  loadActiveSpots().then(function (rows) {
    var modal = document.getElementById('callInfoModal');
    var nameEl = document.getElementById('callInfoCallName');
    var section = document.getElementById('ciPotaSota');
    // Still open, and still the same callsign -- the operator may have
    // closed this or opened a different one while the fetch was in flight.
    if (!section || modal.hidden || nameEl.textContent !== call) return;
    var spot = rows.filter(function (r) { return r.call === call; })
      .sort(function (a, b) { return parseUtcIso(b.ts) - parseUtcIso(a.ts); })[0];
    if (!spot) {
      section.innerHTML = '<div class=ci-label>POTA / SOTA</div>No active spot right now';
      return;
    }
    var link = officialLink(spot.program, spot.ref);
    section.innerHTML = '<div class=ci-label>POTA / SOTA</div>' +
      'Active on ' + esc(spot.program) + ' ' + esc(spot.ref) +
      (spot.name ? ' · ' + esc(spot.name) : '') +
      (spot.program === 'SOTA' && spot.points != null ? ' · ' + esc(spot.points) + ' pts' : '') +
      '<div class=ci-heard>' + esc(spot.band || '') + ' · ' + esc(fmtAgo(parseUtcIso(spot.ts))) + '</div>' +
      (link ? '<a class=ci-link href="' + esc(link) + '" target=_blank rel=noopener>' +
        esc(spot.program === 'POTA' ? 'View park on POTA' : 'View summit on SOTA') + ' →</a>' : '');
  });
}

// Injected at load, not lazily on first show() -- map.html's own lighter
// static-location popup (#mapPopup, not this file's modal) reuses this
// file's .ci-link class for its "official page" link, so that class needs
// to exist as soon as the page can tap anything, not only after the first
// time a real callsign card actually opens.
if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', inject);
} else {
  inject();
}

window.CallInfo = { show: show, hide: hide };
})();
