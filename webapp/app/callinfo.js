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
// Every place a grid square is printed anywhere in the app (operator-
// requested 2026-09-23: "universal ... decodes, QSOs, call sign detail
// cards") should route through here rather than a bare esc(grid), so the
// deep link (map.html?grid=..., handleGridDeepLink() in map.html) and its
// exact URL shape live in exactly one place. A relative href, so it
// resolves correctly whether the caller is app.html or map.html itself.
function gridLink(grid) {
  return '<a class=gridlink href="map.html?grid=' + encodeURIComponent(grid) + '">' + esc(grid) + '</a>';
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
// Same field/square/subsquare parsing as gridToLatLon() above, but returns
// the square's real extent rather than collapsing it to one point -- needed
// below to tell whether a grid straddles a border, and to find a named
// place that falls inside the square itself rather than just near its
// center.
function gridToBBox(grid) {
  if (!grid || grid.length < 4) return null;
  var g = grid.toUpperCase();
  var A = 'A'.charCodeAt(0), Z0 = '0'.charCodeAt(0);
  var fl = g.charCodeAt(0) - A, fu = g.charCodeAt(1) - A;
  var sl = g.charCodeAt(2) - Z0, su = g.charCodeAt(3) - Z0;
  if (fl < 0 || fl > 17 || fu < 0 || fu > 17 || sl < 0 || sl > 9 || su < 0 || su > 9) return null;
  var lonMin = fl * 20 - 180 + sl * 2, latMin = fu * 10 - 90 + su * 1;
  var lonSpan = 2, latSpan = 1;
  if (g.length >= 6) {
    var tl = g.charCodeAt(4) - A, tu = g.charCodeAt(5) - A;
    if (tl >= 0 && tl <= 23 && tu >= 0 && tu <= 23) {
      lonMin += tl * (2 / 24);
      latMin += tu * (1 / 24);
      lonSpan = 2 / 24;
      latSpan = 1 / 24;
    }
  }
  return { lonMin: lonMin, lonMax: lonMin + lonSpan, latMin: latMin, latMax: latMin + latSpan };
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
  if (m > 0) return totalSeconds > 300 ? m + 'm' : m + 'm ' + s + 's';
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
// Write side of the cache above (operator-requested 2026-09-22: "save the
// callsign to grid square mapping as long as possible" -- every reliable
// call+grid pairing anywhere in the app should land here, not just decodes).
// Same last-write-wins policy app.html's own learnCallsignGrid() already
// established for decodes: ts means "last seen with this grid," not "first
// learned," so a later sighting always re-stamps rather than being compared
// against what's already cached. Validates the grid itself (cheap, already
// have gridToLatLon here) so a caller can pass anything without checking
// first; a bad or empty grid is silently a no-op.
function learnGrid(call, grid) {
  if (!call || !grid || !gridToLatLon(grid)) return;
  try {
    var raw = localStorage.getItem('microft8_callsign_grids_v1');
    var cache = raw ? (JSON.parse(raw) || {}) : {};
    cache[call] = { grid: grid.toUpperCase(), ts: Date.now() };
    localStorage.setItem('microft8_callsign_grids_v1', JSON.stringify(cache));
  } catch (e) { /* localStorage unavailable -- cache just stays cold */ }
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
// The cached activator spot for call, or null; never fetches (loadActiveSpots() does), so a decode
// row can ask while it draws.
function activeSpotFor(call) {
  if (!spotCache) return null;
  call = String(call).toUpperCase();
  for (var i = 0; i < spotCache.length; i++) if (spotCache[i].call === call) return spotCache[i];
  return null;
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
// QRZ's /db/ page is keyed by the home call. A portable call ("K1ABC/2", "K1ABC/P") or a foreign-prefixed
// one ("PJ4/K1ABC") escapes to %2F and lands nowhere, so link the longest segment: the suffix or the
// prefix is always the shorter part.
function qrzUrl(call) {
  var parts = String(call).replace(/[<>]/g, '').toUpperCase().split('/').filter(Boolean);
  var base = parts.reduce(function (a, b) { return b.length > a.length ? b : a; }, parts[0] || '');
  return 'https://www.qrz.com/db/' + encodeURIComponent(base);
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

// ---- grid square -> location text (operator-requested 2026-09-23) ----
// Static reference data (country/state borders, world cities), not spot
// data -- fetched once and cached for the life of the page, unlike the
// POTA/SOTA cache above which has to keep expiring. These are the same
// three files map.html already draws its own map from (ne_110m_*.json),
// but this file fetches its own copy rather than depending on map.html
// having loaded them first: app.html has no map and never loads these on
// its own, same "host supplies what only it can know, this file owns
// what's genuinely the same everywhere" reasoning the rest of this file
// already follows for POTA/SOTA.
var geoData = null;
var geoFetchPromise = null;
async function loadGeoData() {
  if (geoData) return geoData;
  if (geoFetchPromise) return geoFetchPromise;
  geoFetchPromise = (async function () {
    var files = await Promise.all(
      ['ne_110m_countries.json', 'ne_110m_us_states.json', 'ne_110m_places.json'].map(function (f) {
        return fetch(f).then(function (r) { return r.ok ? r.json() : null; }).catch(function () { return null; });
      })
    );
    geoData = { countries: files[0], states: files[1], places: files[2] };
    geoFetchPromise = null;
    return geoData;
  })();
  return geoFetchPromise;
}
// Standard even-odd ray-cast: XOR-ing every ring of a Polygon (exterior
// plus any holes) gives "inside exterior, outside every hole" for free,
// since a valid hole is wound inside its own exterior. A MultiPolygon
// (Hawaii, an island nation, Russia split at the antimeridian) is a match
// if any one of its pieces contains the point -- its pieces are separate
// landmasses, not holes of each other, so those are OR'd rather than XOR'd.
function pointInRing(lon, lat, ring) {
  var inside = false;
  for (var i = 0, j = ring.length - 1; i < ring.length; j = i++) {
    var xi = ring[i][0], yi = ring[i][1], xj = ring[j][0], yj = ring[j][1];
    if (((yi > lat) !== (yj > lat)) && (lon < (xj - xi) * (lat - yi) / (yj - yi) + xi)) inside = !inside;
  }
  return inside;
}
function pointInGeometry(lon, lat, geometry) {
  var polys = geometry.type === 'Polygon' ? [geometry.coordinates]
    : geometry.type === 'MultiPolygon' ? geometry.coordinates : [];
  return polys.some(function (poly) {
    var inside = false;
    poly.forEach(function (ring) { if (pointInRing(lon, lat, ring)) inside = !inside; });
    return inside;
  });
}
// A grid square can straddle a border -- resolving only its center point
// would silently pick a side and hide the other one. Sample a 3x3 grid of
// points across the square's real extent (corners, edge midpoints, center)
// instead, and rank every country/state that owns at least one sample by
// how many of the 9 it owns. "More likely first" = whichever owns more of
// the square's area, which this approximates cheaply without real
// polygon-clipping; ties keep whatever order Array#sort leaves them in.
function sampleBBox(bbox) {
  var pts = [];
  [0, 0.5, 1].forEach(function (fx) {
    [0, 0.5, 1].forEach(function (fy) {
      pts.push({ lon: bbox.lonMin + fx * (bbox.lonMax - bbox.lonMin), lat: bbox.latMin + fy * (bbox.latMax - bbox.latMin) });
    });
  });
  return pts;
}
function rankByPoints(features, points) {
  var counts = [];
  features.forEach(function (f) {
    var n = 0;
    points.forEach(function (p) { if (pointInGeometry(p.lon, p.lat, f.geometry)) n++; });
    if (n > 0) counts.push({ props: f.properties, n: n });
  });
  counts.sort(function (a, b) { return b.n - a.n; });
  return counts.slice(0, 2).map(function (c) { return c.props; });
}
var COUNTRY_DISPLAY_NAME = { 'United States of America': 'United States' };
function displayCountryName(name) { return COUNTRY_DISPLAY_NAME[name] || name; }
// Nearest-named-place rubric: Natural Earth's ~243 largest cities/capitals
// are single points, not areas, so no sampling is needed -- any of them
// that land inside the grid square's own extent qualify, ranked by `rank`
// (0 = most prominent, Natural Earth's own scalerank) and, among equal
// ranks, by distance to the square's center.
function placeInGrid(places, bbox, center) {
  var hits = places.filter(function (f) {
    var lon = f.geometry.coordinates[0], lat = f.geometry.coordinates[1];
    return lon >= bbox.lonMin && lon <= bbox.lonMax && lat >= bbox.latMin && lat <= bbox.latMax;
  });
  if (!hits.length) return null;
  hits.sort(function (a, b) {
    if (a.properties.rank !== b.properties.rank) return a.properties.rank - b.properties.rank;
    var pa = { lat: a.geometry.coordinates[1], lon: a.geometry.coordinates[0] };
    var pb = { lat: b.geometry.coordinates[1], lon: b.geometry.coordinates[0] };
    return greatCircle(center, pa).km - greatCircle(center, pb).km;
  });
  return hits[0].properties.name;
}
// The point-sampling/ranking above is all synchronous once geoData itself
// is in hand -- split out so both the modal's full-text row (locateGrid(),
// async: waits on the fetch) and the decode/QSO rows' compact badge
// (shortLocation(), sync: skips silently if the fetch hasn't landed yet
// rather than blocking a row render on it) can share one implementation
// instead of the ranking logic drifting between two copies.
function resolveGridSync(grid) {
  if (!geoData) return null;
  var bbox = gridToBBox(grid), center = gridToLatLon(grid);
  if (!bbox || !center) return null;
  var pts = sampleBBox(bbox);
  var countries = geoData.countries ? rankByPoints(geoData.countries.features, pts) : [];
  if (!countries.length) return { countries: [], states: [], place: null };
  var isUS = countries.some(function (c) { return c.name === 'United States of America'; });
  var states = (isUS && geoData.states) ? rankByPoints(geoData.states.features, pts) : [];
  var place = geoData.places ? placeInGrid(geoData.places.features, bbox, center) : null;
  return { countries: countries, states: states, place: place };
}
async function locateGrid(grid) {
  await loadGeoData();
  var r = resolveGridSync(grid);
  if (!r || !r.countries.length) return null;
  var parts = [];
  if (r.place) parts.push(r.place);
  if (r.states.length) parts.push(r.states.map(function (s) { return s.name; }).join('/'));
  parts.push(r.countries.map(function (c) { return displayCountryName(c.name); }).join('/'));
  return parts.join(' · ');
}
// Compact form for decode/QSO rows, where a full "Texas · United States"
// doesn't fit: 2-letter codes only (state postal abbreviation, country
// ISO 3166-1 alpha-2), no place name -- "TX·US", or "NM/AZ·US" for a
// straddling grid, or a bare "CA" (Canada) when there's no US state to
// pair it with. A bare 2-letter code is always a *country* in this
// scheme -- a US state code never appears without its "·US" suffix -- so
// "CA" alone can't be misread as California. Returns '' rather than
// blocking the caller when the geo data hasn't finished loading yet or
// the grid doesn't resolve to anything (open ocean); every call site
// already treats '' as "nothing to show this render", same as an unfilled
// distance/bearing badge, so the row just quietly gains the badge on its
// next redraw once the fetch lands.
// Natural Earth leaves iso_a2 as the placeholder '-99' for France, Norway,
// Kosovo, N. Cyprus and Somaliland, and as 'CN-TW' for Taiwan (ne_110m_countries.json).
// The badge named these by the placeholder ("-99" for JN07, in France), so
// they get their codes here and any other non-2-letter value shows nothing.
var COUNTRY_CODE_FIX = { 'France': 'FR', 'Norway': 'NO', 'Kosovo': 'XK', 'Taiwan': 'TW', 'N. Cyprus': 'CY', 'Somaliland': 'SO' };
function countryCode(c) {
  var code = COUNTRY_CODE_FIX[c.name] || c.iso_a2;
  return /^[A-Z]{2}$/.test(code || '') ? code : '';
}
function shortLocation(grid) {
  var r = resolveGridSync(grid);
  if (!r || !r.countries.length) return '';
  var countryCodes = r.countries.map(countryCode).filter(Boolean);
  var stateCodes = r.states.map(function (s) { return s.postal; }).filter(Boolean);
  if (stateCodes.length) return stateCodes.join('/') + (countryCodes.length ? '·' + countryCodes.join('/') : '');
  return countryCodes.join('/');
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
  // Same affordance app.html's own .callink already uses for a clickable
  // callsign (dotted underline, no color shift) -- a grid square link is
  // the same kind of "this plain-looking text is tappable" case, just a
  // real <a href> rather than a JS-driven modal open, so it gets its own
  // class instead of literally reusing .callink (app.html's click-handler
  // rebinding for that class expects a data-call attribute this isn't).
  '.gridlink{text-decoration:underline;text-decoration-style:dotted;cursor:pointer;color:inherit}' +
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
//   heardUs(call)     -> [{band, snr, t, count, grid}]: PSKReporter reports of this station hearing us
//                        (PskrCore.heardBy(), docs/architecture/webapp.md, Who hears me); the section shows only
//                        when there are some
async function show(call, opts) {
  if (!call) return;
  call = call.toUpperCase();
  opts = opts || {};

  var heardUs = typeof opts.heardUs === 'function' ? (opts.heardUs(call) || []) : [];
  var grid = cachedGrid(call);
  // A station that reported hearing us sent its own locator with the report: a reliable pairing for the cache.
  if (!grid) {
    var reported = heardUs.filter(function (h) { return h.grid; })[0];
    if (reported) { learnGrid(call, reported.grid); grid = cachedGrid(call); }
  }
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
      ? gridLink(grid) + (distBearing ? ' · ' + esc(distBearing) : '') +
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

  if (heardUs.length) {
    body += '<div class=ci-row><div class=ci-label>PSKReporter: heard you</div>' +
      heardUs.map(function (h) {
        return '<div class=ci-heard>' + esc(h.band) +
          (h.snr == null ? '' : ' · ' + (h.snr > 0 ? '+' : '') + h.snr + ' dB') +
          ' · <span class=ago data-ts="' + h.t + '">' + esc(fmtAgo(h.t)) + '</span>' +
          (h.count > 1 ? ' · ' + h.count + ' reports' : '') + '</div>';
      }).join('') + '</div>';
  }

  // Country/state/place row: placeholder immediately when a grid is cached
  // (same never-block-on-network pattern as the POTA/SOTA row below), or a
  // plain message when there's no grid to look up at all.
  body += '<div class=ci-row id=ciCountry><div class=ci-label>Country / DXCC</div>' +
    (grid ? '<span style="color:GrayText">checking…</span>' : 'No grid square cached yet -- check QRZ below.') +
    '</div>';

  // POTA/SOTA row: placeholder immediately (never blocks the rest of the
  // card on a network round trip), filled in once the fetch resolves --
  // the modal may already be closed by then, so this only ever touches the
  // DOM if it's still showing this same callsign.
  body += '<div class=ci-row id=ciPotaSota><div class=ci-label>POTA / SOTA</div>' +
    '<span style="color:GrayText">checking…</span></div>';

  document.getElementById('callInfoBody').innerHTML = body;
  document.getElementById('callInfoQrz').href = qrzUrl(call);
  document.getElementById('callInfoModal').hidden = false;

  if (grid) {
    locateGrid(grid).then(function (text) {
      var modal = document.getElementById('callInfoModal');
      var nameEl = document.getElementById('callInfoCallName');
      var section = document.getElementById('ciCountry');
      // Still open, and still the same callsign -- same guard as the
      // POTA/SOTA fill-in below, for the same reason.
      if (!section || modal.hidden || nameEl.textContent !== call) return;
      section.innerHTML = '<div class=ci-label>Country / DXCC</div>' +
        (text ? esc(text) : 'No country match for this grid square -- check QRZ below.');
    });
  }

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
    var progRef = esc(spot.program) + ' ' + esc(spot.ref);
    section.innerHTML = '<div class=ci-label>POTA / SOTA</div>' +
      'Active on ' + (link
        ? '<a href="' + esc(link) + '" target=_blank rel=noopener style="color:var(--accent, #1d4ed8)">' + progRef + '</a>'
        : progRef) +
      (spot.name ? ' · ' + esc(spot.name) : '') +
      (spot.program === 'SOTA' && spot.points != null ? ' · ' + esc(spot.points) + ' pts' : '') +
      '<div class=ci-heard>' + esc(spot.band || '') + ' · <span class=ago data-ts="' +
        parseUtcIso(spot.ts) + '">' + esc(fmtAgo(parseUtcIso(spot.ts))) + '</span></div>';
  });
}

// Keeps the card's "X ago"s counting while it is open (the POTA/SOTA spot, PSKReporter's reports) -- cheap (a few
// elements) and only does anything while the modal is actually visible, so it costs nothing the rest of the time.
setInterval(function () {
  var modal = document.getElementById('callInfoModal');
  if (!modal || modal.hidden) return;
  modal.querySelectorAll('.ci-heard .ago[data-ts]').forEach(function (el) {
    el.textContent = fmtAgo(parseInt(el.getAttribute('data-ts'), 10));
  });
}, 5000);

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

// Fired eagerly at load, not lazily on first show()/shortLocation() call --
// shortLocation() is synchronous and gives up rather than waiting on a
// fetch (a decode/QSO row can't block its own render on one), so the
// sooner this lands the sooner rows actually get their badge instead of
// silently going without it until whatever happens to open the modal first.
loadGeoData();

window.CallInfo = { loadActiveSpots: loadActiveSpots, activeSpotFor: activeSpotFor, show: show, hide: hide, learnGrid: learnGrid, cachedGrid: cachedGrid, shortLocation: shortLocation, countryCode: countryCode, gridLink: gridLink, qrzUrl: qrzUrl };
})();
