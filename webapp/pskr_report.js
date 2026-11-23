// Reporting what a station hears to PSKReporter, for worker.js's /pskr/ route
// (docs/architecture/system.md, Third-party services; docs/architecture/webapp.md, Spotting).
//
// A page posts the decodes it saw; this checks them, a Durable Object per receiving station
// (PskrGate, below) decides what of them may go, and the Worker sends that to PSKReporter as one
// IPFIX message over TCP. Every page open on a station posts what it saw, so the gate is where two
// pages' reports become one: it holds when the station last sent, and the senders it reported in
// the last hour, and nothing else. It holds no spots: a page whose post is too soon keeps its own
// and posts again when told.
//
// PSKReporter's rules (pskdev.html, read 2026-11-23): at most one message per sender every five
// minutes unless it is full, each call at most once in five minutes and ideally once an hour while
// nothing about it changes, informationSource 1, the header's time within a minute of arrival.
// The templates are cached per source address and port, which a fresh TCP connection from
// Cloudflare never repeats, so every message carries them.
//
// Written from that page, not from any program's code.

export const WINDOW_MS = 5 * 60 * 1000; // between two messages of one station
export const RESEND_MS = 60 * 60 * 1000; // before a sender heard again on the same band and mode is reported again
export const MAX_SPOTS = 200;           // in one post, and so in one message
export const MAX_AGE_S = 60 * 60;       // PSKReporter drops a report older than about an hour
export const MAX_BODY = 32 * 1024;

const CALL_FORM = /^[A-Z0-9]+(\/[A-Z0-9]+)*$/;
const GRID_FORM = /^[A-R]{2}[0-9]{2}([A-X]{2})?$/;
const MODES = ["FT8", "FT4"];

// A callsign as a report carries it, upper-cased, or "". At least one letter and one digit, 3 to 15
// characters: what an amateur call can be, and not a word.
export function callOf(v) {
  if (typeof v !== "string") return "";
  const c = v.trim().toUpperCase();
  if (c.length < 3 || c.length > 15 || !CALL_FORM.test(c) || !/[A-Z]/.test(c) || !/[0-9]/.test(c)) return "";
  return c;
}

// A Maidenhead locator, 4 or 6 characters, as PSKReporter shows one ("EM10cg"), or "".
export function gridOf(v) {
  if (typeof v !== "string") return "";
  const g = v.trim();
  if (!GRID_FORM.test(g.slice(0, 4).toUpperCase() + g.slice(4).toUpperCase())) return "";
  return g.slice(0, 4).toUpperCase() + g.slice(4).toLowerCase();
}

// The amateur band a frequency in Hz is in, for the analytics and for "the same band" below; "" outside
// them.
const BANDS = [
  ["160m", 1.8e6, 2.0e6], ["80m", 3.5e6, 4.0e6], ["60m", 5.25e6, 5.45e6], ["40m", 7.0e6, 7.3e6],
  ["30m", 10.1e6, 10.15e6], ["20m", 14.0e6, 14.35e6], ["17m", 18.068e6, 18.168e6], ["15m", 21.0e6, 21.45e6],
  ["12m", 24.89e6, 24.99e6], ["10m", 28.0e6, 29.7e6], ["6m", 50e6, 54e6], ["2m", 144e6, 148e6],
];
export function bandOf(hz) {
  for (const [name, lo, hi] of BANDS) if (hz >= lo && hz <= hi) return name;
  return "";
}

// The post, checked: { ok: true, report: { call, grid, app, spots } }, or { ok: false, error }. `nowS`
// is the Worker's clock. A spot is { c: call, g: grid or "", f: Hz, s: SNR in dB, m: mode, t: Unix
// seconds }. A spot that is not one, out of the amateur bands, older than PSKReporter keeps or in the
// future is dropped, not refused: one bad decode does not cost a station its report. One spot a sender
// is kept, its latest.
export function checkReport(body, nowS) {
  if (!body || typeof body !== "object") return { ok: false, error: "bad_request" };
  const call = callOf(body.call);
  const grid = gridOf(body.grid);
  if (!call) return { ok: false, error: "bad_call" };
  if (!grid) return { ok: false, error: "bad_grid" };
  if (!Array.isArray(body.spots) || body.spots.length === 0 || body.spots.length > MAX_SPOTS) {
    return { ok: false, error: "bad_spots" };
  }
  const bySender = new Map();
  for (const s of body.spots) {
    if (!s || typeof s !== "object") continue;
    const c = callOf(s.c);
    const g = gridOf(s.g); // a sender's locator is optional: one that is not one is left out
    const f = s.f, snr = s.s, t = s.t;
    if (!c || c === call) continue;
    if (!Number.isSafeInteger(f) || !bandOf(f)) continue;
    if (!Number.isInteger(snr) || snr < -50 || snr > 50) continue;
    if (!MODES.includes(s.m)) continue;
    if (!Number.isSafeInteger(t) || t < nowS - MAX_AGE_S || t > nowS + 60) continue;
    const spot = { c, g, f, s: snr, m: s.m, t };
    const had = bySender.get(c);
    if (!had || had.t < t) bySender.set(c, spot);
  }
  return { ok: true, report: { call, grid, app: body.app, spots: [...bySender.values()] } };
}

// What may go of `spots` now, and the gate's state after: { action, state, spots, seq, retryMs }.
// `state` is { last: ms of the last message, seq: records sent before, sent: { key: ms } } (a fresh
// gate has none). `action` is "wait" while the station's last message is under five minutes old, with
// `retryMs` until it is not; "none" when every spot was reported within the hour; else "send", with the
// spots to send and the sequence number their message carries. A sender is the same report while it
// is heard on the same band in the same mode.
export function gate(state, spots, nowMs) {
  const st = {
    last: state && Number.isFinite(state.last) ? state.last : 0,
    seq: state && Number.isSafeInteger(state.seq) ? state.seq : 0,
    sent: {},
  };
  const had = (state && state.sent) || {};
  for (const k of Object.keys(had)) if (nowMs - had[k] < RESEND_MS) st.sent[k] = had[k];
  if (st.last && nowMs - st.last < WINDOW_MS) {
    return { action: "wait", state: st, spots: [], seq: st.seq, retryMs: WINDOW_MS - (nowMs - st.last) };
  }
  const fresh = spots.filter((s) => !st.sent[spotKey(s)]);
  if (fresh.length === 0) return { action: "none", state: st, spots: [], seq: st.seq, retryMs: 0 };
  const seq = st.seq;
  for (const s of fresh) st.sent[spotKey(s)] = nowMs;
  st.last = nowMs;
  st.seq = (seq + fresh.length) >>> 0;
  return { action: "send", state: st, spots: fresh, seq, retryMs: WINDOW_MS };
}

function spotKey(s) {
  return `${s.c} ${bandOf(s.f)} ${s.m}`;
}

// The IPFIX observation domain for a receiving station: PSKReporter tells receivers apart by it when
// many report from one address (pskdev.html). FNV-1a of the call, so one station keeps one.
export function domainOf(call) {
  let h = 0x811c9dc5;
  for (let i = 0; i < call.length; i++) {
    h ^= call.charCodeAt(i);
    h = Math.imul(h, 0x01000193) >>> 0;
  }
  return h >>> 0;
}

// ---- IPFIX (RFC 7011), as pskdev.html lays it out ----
// Enterprise 30351 is PSKReporter's. Template IDs are ours to choose, 256 to 65535.
const PEN = 30351;
const RX_TEMPLATE = 0x50e2;
const TX_TEMPLATE = 0x50e3;
const VAR = 0xffff;
// The receiver: an options template (set 3) with one scope field, as pskdev.html's own is.
const RX_FIELDS = [[2, VAR], [4, VAR], [8, VAR]]; // receiverCallsign, receiverLocator, decodingSoftware
// A sender: senderCallsign, frequency, sNR, mode, senderLocator, informationSource, flowStartSeconds.
const TX_FIELDS = [[1, VAR], [5, 4], [6, 1], [10, VAR], [3, VAR], [11, 1], [150, 4, false]];

class Bytes {
  constructor() { this.a = []; }
  u8(v) { this.a.push(v & 0xff); }
  u16(v) { this.u8(v >>> 8); this.u8(v); }
  u32(v) { this.u16(v >>> 16); this.u16(v & 0xffff); }
  str(s) {
    const b = new TextEncoder().encode(s).slice(0, 254);
    this.u8(b.length);
    for (const x of b) this.u8(x);
  }
  pad4(from) { while ((this.a.length - from) % 4) this.u8(0); }
  set(id, body) {
    const at = this.a.length;
    this.u16(id);
    this.u16(0);
    body();
    this.pad4(at);
    const len = this.a.length - at;
    this.a[at + 2] = len >>> 8;
    this.a[at + 3] = len & 0xff;
  }
}

function field(b, [id, len, enterprise]) {
  if (enterprise === false) {
    b.u16(id);
    b.u16(len);
  } else {
    b.u16(0x8000 | id);
    b.u16(len);
    b.u32(PEN);
  }
}

// One IPFIX message: the header, both templates, the receiver and every spot. One message carries a whole
// send: at most MAX_SPOTS spots, about 6 KB, far inside IPFIX's 65,535 bytes, which is WSJT-X's own
// ceiling over TCP; PSKReporter took messages of 1.2 to 2.5 KB from us (docs/architecture/system.md,
// Reporting). `exportS` is the
// message's time, Unix seconds; `seq` the records sent before it; `domain` the station's.
export function encodeMessage({ call, grid, software, spots, seq, domain, exportS }) {
  const b = new Bytes();
  b.u16(10); // IPFIX
  b.u16(0); // the message's length, filled in below
  b.u32(exportS);
  b.u32(seq);
  b.u32(domain);
  b.set(3, () => {
    b.u16(RX_TEMPLATE);
    b.u16(RX_FIELDS.length);
    b.u16(1); // scope fields
    RX_FIELDS.forEach((f) => field(b, f));
  });
  b.set(2, () => {
    b.u16(TX_TEMPLATE);
    b.u16(TX_FIELDS.length);
    TX_FIELDS.forEach((f) => field(b, f));
  });
  b.set(RX_TEMPLATE, () => {
    b.str(call);
    b.str(grid);
    b.str(software);
  });
  b.set(TX_TEMPLATE, () => {
    for (const s of spots) {
      b.str(s.c);
      b.u32(s.f);
      b.u8(s.s);
      b.str(s.m);
      b.str(s.g);
      b.u8(1); // informationSource: automatically extracted
      b.u32(s.t);
    }
  });
  b.a[2] = b.a.length >>> 8;
  b.a[3] = b.a.length & 0xff;
  return Uint8Array.from(b.a);
}

// ---- The gate: a Durable Object per receiving station ----
// Its state is one value, written once a message: requests arrive minutes apart, past the 70 to 140 s
// after which an idle object leaves memory, so nothing is kept there. It never opens a socket, which
// would keep it in memory, and billed, for up to 15 minutes: the Worker sends.
export class PskrGate {
  constructor(ctx) {
    this.ctx = ctx;
  }

  async fetch(request) {
    const { spots } = await request.json();
    const was = (await this.ctx.storage.get("s")) || null;
    const out = gate(was, spots, Date.now());
    // Only a send changes what matters: a wait or a "none" leaves the state as it was, its expired
    // senders included, which the next send drops.
    if (out.action === "send") await this.ctx.storage.put("s", out.state);
    return Response.json({ action: out.action, spots: out.spots, seq: out.seq, retryMs: out.retryMs });
  }
}
