// The amateur bands the web app knows, shared by app.html (the dock, Settings) and swr.html.
// Plain globals, like callinfo.js: there is no module system across these pages.
// Index order matches the Station.txt band keys. ft8 mirrors the P4's seeded
// table (kFt8DefaultKhz in poe_p4/main/porta_host.cpp) byte-for-byte,
// since CONFIG only ever reports an *override* (bandN=/ft4_bandN=) when one
// has actually been stored -- an un-set band has no value on the wire at all,
// and this is the only place that knows what it defaults to.
var BANDS = [
  {i: 0, name: '160m', n: 160, ft8: 1840,   ft4: 1843},
  {i: 1, name: '80m',  n: 80,  ft8: 3573,   ft4: 3575},
  {i: 2, name: '60m',  n: 60,  ft8: 5357,   ft4: 5357},
  {i: 3, name: '40m',  n: 40,  ft8: 7074,   ft4: 7047.5},
  {i: 4, name: '30m',  n: 30,  ft8: 10136,  ft4: 10140},
  {i: 5, name: '20m',  n: 20,  ft8: 14074,  ft4: 14080},
  {i: 6, name: '17m',  n: 17,  ft8: 18100,  ft4: 18104},
  {i: 7, name: '15m',  n: 15,  ft8: 21074,  ft4: 21140},
  {i: 8, name: '12m',  n: 12,  ft8: 24915,  ft4: 24919},
  {i: 9, name: '10m',  n: 10,  ft8: 28074,  ft4: 28180},
  {i: 10, name: '6m',  n: 6,   ft8: 50313,  ft4: 50318},
  {i: 11, name: '2m',  n: 2,   ft8: 144174, ft4: 144170}
];
// Region 2 amateur allocations (ITU RR Article 5), kHz -- a sanity check,
// not a licence check, applied only here: the main board takes any frequency
// (webapp.md, The dock). 60m is one range spanning the US channels and the
// WRC-15 allocation. Keyed by BANDS[].n.
var BAND_EDGES_KHZ = {
  160: [1800, 2000], 80: [3500, 4000], 60: [5330, 5407], 40: [7000, 7300],
  30: [10100, 10150], 20: [14000, 14350], 17: [18068, 18168], 15: [21000, 21450],
  12: [24890, 24990], 10: [28000, 29700], 6: [50000, 54000], 2: [144000, 148000]
};

// Parses active_bands leniently -- whitespace/comma/semicolon/slash separated
// band numbers, "m" suffix tolerated implicitly since parseInt stops at the
// first non-digit. Empty or unrecognized text means "no filter", i.e. every
// band is active -- the `any` flag.
function parseActiveBands(text) {
  var want = {};
  var any = false;
  String(text || '').split(/[\s,;\/]+/).forEach(function (t) {
    var n = parseInt(t, 10);
    if (n > 0) { want[n] = true; any = true; }
  });
  return { want: want, any: any };
}
