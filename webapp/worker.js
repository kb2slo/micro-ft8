import { checkReport, encodeMessage, domainOf, bandOf, MAX_BODY as PSKR_MAX_BODY, PskrGate } from "./pskr_report.js";

// The Durable Object class wrangler.jsonc binds as PSKR_GATE: it must be exported from the entry.
export { PskrGate };

// The site's three dynamic routes. Everything else is a static file that
// Workers Static Assets serves without running this, with its headers from
// _headers (docs/architecture/webapp.md, Serving the site): wrangler.jsonc's
// run_worker_first names exactly these paths.
//
// /download/* is served from the DOWNLOADS R2 bucket (both channels'
// firmware, source and getting-started sheet). See
// docs/RELEASE_PROCESS.md. Each file it serves is counted, by its name and
// by whether the downloads page's flasher fetched it (countDownload()).
//
// /qrz/* relays three calls to QRZ's Logbook API for the web app, which
// cannot read that API's replies itself (serveQrz(), below).
//
// /pskr/report sends what a station heard to PSKReporter (servePskr(), below; pskr_report.js).
//
// A path with no file never lands here: Static Assets answers it with 404.html
// (wrangler.jsonc, not_found_handling). The last return is a backstop only.

// The app's name, as everything this sends names it: QRZ's User-Agent and the software PSKReporter
// lists (pskrSoftware()). One spelling everywhere, "micro-FT8" (docs/README.md, The app's name).
// Not exported: the runtime takes each named export of this module for an entrypoint, and refuses one
// that is not a function or a class ("Incorrect type for map entry", wrangler dev, 2026-11-23).
const APP_NAME = "micro-FT8";

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname.startsWith("/download/")) {
      return serveDownload(request, env, url);
    }
    if (url.pathname.startsWith("/qrz/")) {
      return serveQrz(request, url);
    }
    if (url.pathname === "/pskr/report") {
      return servePskr(request, env);
    }
    return new Response("Not found", {
      status: 404,
      headers: { "access-control-allow-origin": "*" },
    });
  },
};

async function serveDownload(request, env, url) {
  if (request.method !== "GET" && request.method !== "HEAD") {
    return new Response("Method not allowed", { status: 405 });
  }

  // Key is everything after "/download/": one file name, or one under ota/ (each OTA image by its
  // own name): docs/RELEASE_PROCESS.md.
  // Reject empty, traversal, and any other nesting.
  let key;
  try {
    key = decodeURIComponent(url.pathname.slice("/download/".length));
  } catch (e) {
    return new Response("Not found", { status: 404 });
  }
  const name = fileName(key);
  if (!name) {
    return new Response("Not found", { status: 404 });
  }

  const obj = await env.DOWNLOADS.get(key);
  if (!obj) {
    return new Response("Not found", { status: 404 });
  }

  const headers = new Headers();
  obj.writeHttpMetadata(headers);
  headers.set("etag", obj.httpEtag);
  headers.set("Access-Control-Allow-Origin", "*");
  if (!headers.has("content-type")) {
    headers.set("content-type", guessContentType(key));
  }
  // Binaries/tarballs: Save As with the stable filename. JSON stays inline for
  // the downloads page fetch (and for humans opening the manifest in a tab).
  const inline = key.endsWith(".json") || key.endsWith(".sha256");
  if (!inline) {
    headers.set(
      "content-disposition",
      `attachment; filename="${name.replace(/"/g, "")}"`,
    );
  }

  if (request.method === "HEAD") {
    return new Response(null, { status: 200, headers });
  }
  // A manifest or a checksum is a page or a station asking what is published, not a download.
  if (!inline) countDownload(env, key, url);
  return new Response(obj.body, { status: 200, headers });
}

// One Analytics Engine data point per file served: its key, and how it was fetched, "usb" by the
// downloads page's flasher (which asks with ?via=usb) and "download" by anything else, a station's
// update included (told by its key, under ota/). Nothing about who asked is written. The key is
// also the index, Analytics Engine's sampling key. A count never fails a download: with no binding
// (a local run) or a refused write, the file is still served.
function countDownload(env, key, url) {
  const via = url.searchParams.get("via") === "usb" ? "usb" : "download";
  try {
    env.DOWNLOAD_COUNTS.writeDataPoint({ blobs: [key, via], indexes: [key] });
  } catch (e) {
    // Uncounted, and served.
  }
}

// The file's own name when `key` is one this route serves, else "".
function fileName(key) {
  const name = key.startsWith("ota/") ? key.slice(4) : key;
  if (!name || name.includes("/") || name.includes("\\") || name.includes("..")) return "";
  return name;
}

function guessContentType(key) {
  if (key.endsWith(".json")) return "application/json; charset=utf-8";
  if (key.endsWith(".tar.gz")) return "application/gzip";
  if (key.endsWith(".pdf")) return "application/pdf";
  if (key.endsWith(".sha256")) return "text/plain; charset=utf-8";
  return "application/octet-stream";
}

// ---------------------------------------------------------------------------
// /qrz/*: the web app's way to an operator's QRZ logbook
// (docs/architecture/system.md, Third-party services).
//
// QRZ's Logbook API sends no CORS header, with a valid key or without (measured
// 2026-11-19), and asks every client for a User-Agent of its own, which a page
// cannot set. So the app posts here and this calls QRZ. The operator's logbook
// key and QSOs pass through and are not kept: nothing here stores or logs a
// request, and no reply repeats the key.
//
// Three calls and no others: status, fetch and insert. Never REPLACE, which
// overwrites confirmed QSOs, and never DELETE.
//
// A fetched record is cut down to what a sync matches on. QRZ sends about
// 1,250 bytes a record, among them the other operator's name, email, town and
// coordinates; none of that leaves here.
//
// The body is JSON sent as plain text, the default for a string body, which
// keeps the post a simple request: no CORS preflight, so one Worker request a
// call and not two.
// ---------------------------------------------------------------------------

const QRZ_API = "https://logbook.qrz.com/api";
// QRZ asks every application for an identifiable User-Agent, "ApplicationName/version" (its Logbook API
// page). The version is the web app's own, which the app sends with each call, so that QRZ, and we, can
// tell which release a problem came from; `relay` is this file's revision, raised by hand when what it
// sends QRZ changes. A version appVersion() does not take reads as 0.
const QRZ_RELAY_REVISION = 1;

export function qrzUserAgent(app) {
  return `${APP_NAME}/${appVersion(app) || "0"} (relay ${QRZ_RELAY_REVISION}; https://micro-ft8.kalikstein.com)`;
}

// The app's version as the page reports it (the config's `sk_app`), or "". The page's word for it is a
// caller's, so only a plain version string is passed on. Every shape a build reports is one
// (docs/RELEASE_PROCESS.md, An image carries its version): `0.1.0+0c5ccec`, `b1322+0c5ccec`,
// `0c5ccec-dirty`. A bench push names itself by its version, a space and when it was built; only the
// version is kept.
const APP_VERSION_FORM = /^[0-9A-Za-z][0-9A-Za-z.+_-]{0,31}$/;

export function appVersion(app) {
  const version = typeof app === "string" ? app.trim().split(/\s+/)[0] : "";
  return APP_VERSION_FORM.test(version) ? version : "";
}
const QRZ_PAGE = 250; // records asked of QRZ at a time, the page its documentation suggests
// QRZ takes one QSO a call, so a request's CPU time grows with its batch: about 0.4 ms an insert, and a
// batch of 50 took 20.7 ms against the free plan's 10 ms a request (Cloudflare's analytics, 2026-11-20;
// it was let through, as an occasional overrun is, and a steady one is refused with error 1102).
const QRZ_MAX_INSERTS = 10;
const QRZ_MAX_BODY = 64 * 1024;
const QRZ_MAX_RECORD = 2000;
// Not QRZ's rule for a key, which it does not publish: enough to turn away what is plainly not one before
// it costs a call.
const QRZ_KEY_FORM = /^[0-9A-Za-z-]{8,64}$/;
const QRZ_DATE_FORM = /^\d{4}-\d{2}-\d{2}$/;

async function serveQrz(request, url) {
  if (request.method !== "POST") return qrzJson({ ok: false, error: "method" }, 405);
  let ask;
  try {
    const text = await request.text();
    if (text.length > QRZ_MAX_BODY) return qrzJson({ ok: false, error: "too_large" }, 413);
    ask = JSON.parse(text);
  } catch (e) {
    return qrzJson({ ok: false, error: "bad_request" }, 400);
  }
  if (!ask || typeof ask.key !== "string" || !QRZ_KEY_FORM.test(ask.key)) {
    return qrzJson({ ok: false, error: "bad_key" }, 400);
  }
  try {
    switch (url.pathname) {
      case "/qrz/status":
        return qrzJson(await qrzStatus(ask));
      case "/qrz/fetch":
        return qrzJson(await qrzFetch(ask));
      case "/qrz/insert":
        return qrzJson(await qrzInsert(ask));
      default:
        return qrzJson({ ok: false, error: "not_found" }, 404);
    }
  } catch (e) {
    // QRZ did not answer, or not with a reply this reads. The error's text is not passed on: it can
    // hold the request.
    return qrzJson({ ok: false, error: "unreachable" }, 502);
  }
}

function qrzJson(body, status) {
  const bad = body.error === "bad_request" ? 400 : 200;
  return new Response(JSON.stringify(body), {
    status: status || bad,
    headers: {
      "content-type": "application/json; charset=utf-8",
      "access-control-allow-origin": "*",
      "cache-control": "no-store",
    },
  });
}

// One call to QRZ: its reply as a map of its NAME=value pairs.
async function qrzCall(ask, action, more) {
  const form = new URLSearchParams({ KEY: ask.key, ACTION: action });
  for (const name of Object.keys(more || {})) form.set(name, more[name]);
  const reply = await fetch(QRZ_API, {
    method: "POST",
    headers: { "content-type": "application/x-www-form-urlencoded", "user-agent": qrzUserAgent(ask.app) },
    body: form.toString(),
  });
  return parseQrzReply(await reply.text());
}

// QRZ answers NAME=value pairs joined by &, in no fixed order, with the ADIF of a fetch HTML-escaped. A
// pair starts at an & followed by a name in capitals and =, which an escaped value cannot hold: its own
// & are &amp;, &lt; and &gt;.
export function parseQrzReply(text) {
  const out = {};
  for (const pair of String(text).split(/&(?=[A-Z_]+=)/)) {
    const eq = pair.indexOf("=");
    if (eq > 0) out[pair.slice(0, eq)] = pair.slice(eq + 1).trim();
  }
  return out;
}

function qrzError(reply) {
  if (reply.RESULT === "AUTH") return { ok: false, error: "auth", reason: reply.REASON || "" };
  return { ok: false, error: "qrz", reason: reply.REASON || reply.RESULT || "" };
}

async function qrzStatus(ask) {
  const reply = await qrzCall(ask, "STATUS");
  if (reply.RESULT !== "OK") return qrzError(reply);
  return {
    ok: true,
    callsign: reply.CALLSIGN || "",
    count: Number(reply.COUNT) || 0,
    confirmed: Number(reply.CONFIRMED) || 0,
    dxcc_count: Number(reply.DXCC_COUNT) || 0,
    start_date: reply.START_DATE || "",
  };
}

// The logbook's records dated `from` to `to`, a page at a time by QRZ's ID: `after` is 0, then the `next`
// of the page before, while `more` says there may be another.
async function qrzFetch(ask) {
  const after = Number(ask.after || 0);
  if (!QRZ_DATE_FORM.test(ask.from) || !QRZ_DATE_FORM.test(ask.to) || !Number.isSafeInteger(after) || after < 0) {
    return { ok: false, error: "bad_request" };
  }
  const reply = await qrzCall(ask, "FETCH", {
    OPTION: `BETWEEN:${ask.from}+${ask.to},MAX:${QRZ_PAGE},AFTERLOGID:${after}`,
  });
  // No record in the range is RESULT=FAIL with a COUNT of 0 and no reason (measured 2026-11-19).
  if (reply.RESULT === "FAIL" && reply.COUNT === "0" && !reply.REASON) {
    return { ok: true, records: [], more: false, next: after };
  }
  if (reply.RESULT !== "OK") return qrzError(reply);
  const records = trimQrzRecords(reply.ADIF || "");
  let last = after;
  for (const r of records) last = Math.max(last, Number(r.app_qrzlog_logid) || 0);
  return { ok: true, records, more: records.length >= QRZ_PAGE, next: last + 1 };
}

// QRZ's records, each cut down to what a sync matches on and its ID. A field's value is taken by its
// declared length, which counts characters (measured 2026-11-19, on names with accents), so a value that
// holds a < or an <eor> of its own is passed over whole.
export function trimQrzRecords(escaped) {
  const adif = escaped.replace(/&lt;/g, "<").replace(/&gt;/g, ">").replace(/&quot;/g, '"').replace(/&#0?39;/g, "'")
    .replace(/&amp;/g, "&");
  const records = [];
  const tag = /<([A-Za-z0-9_]+)(?::(\d+)(?::[A-Za-z0-9_]+)?)?>/g;
  let fields = {};
  let m;
  while ((m = tag.exec(adif))) {
    const name = m[1].toLowerCase();
    if (m[2] === undefined) {
      if (name === "eor") {
        if (fields.call) records.push(trimmedQrzRecord(fields));
        fields = {};
      }
      continue;
    }
    const len = Number(m[2]);
    fields[name] = adif.substr(tag.lastIndex, len);
    tag.lastIndex += len;
  }
  return records;
}

// The fields of a QRZ record that are passed on, under QRZ's own names and as QRZ wrote them. A record
// is named fields from end to end: the app reads the names it needs, and when it comes to need another
// the name is added here and nothing else about the reply changes. Never a name that says who the other
// operator is.
const QRZ_PASS = ["app_qrzlog_logid", "call", "qso_date", "time_on", "band", "mode", "submode",
  "station_callsign", "app_qrzlog_status"];

function trimmedQrzRecord(f) {
  const out = {};
  for (const name of QRZ_PASS) {
    if (f[name] !== undefined && f[name] !== "") out[name] = f[name];
  }
  return out;
}

// Each record in turn, since QRZ takes one a call. A QSO QRZ already holds is refused as a duplicate, with
// no ID for it (measured 2026-11-19). A refused key stops the batch: the rest would be refused too.
async function qrzInsert(ask) {
  const records = ask.records;
  if (!Array.isArray(records) || records.length === 0 || records.length > QRZ_MAX_INSERTS ||
      records.some((r) => typeof r !== "string" || r.length === 0 || r.length > QRZ_MAX_RECORD)) {
    return { ok: false, error: "bad_request" };
  }
  const results = [];
  for (const adif of records) {
    const reply = await qrzCall(ask, "INSERT", { ADIF: adif });
    if (reply.RESULT === "OK") {
      results.push({ result: "inserted", logid: reply.LOGID || "" });
    } else if (reply.RESULT === "AUTH") {
      return { ok: false, error: "auth", reason: reply.REASON || "", results };
    } else if (/duplicate/i.test(reply.REASON || "")) {
      results.push({ result: "duplicate" });
    } else {
      results.push({ result: "refused", reason: reply.REASON || reply.RESULT || "" });
    }
  }
  return { ok: true, results };
}

// ---------------------------------------------------------------------------
// /pskr/report: what a station heard, to PSKReporter
// (docs/architecture/system.md, Third-party services; pskr_report.js).
//
// A page posts { call, grid, app, spots } as JSON in plain text, a simple request like /qrz/'s. The
// station's gate says what of it may go; that goes to PSKReporter in one IPFIX message over TCP, which
// a page cannot open. The reply says how many went and, as `retry_in`, the milliseconds before the
// station's next message may: a page whose post was too soon keeps its spots and posts them then.
//
// Each post is counted (countPskr()): the station's call, its grid's square, the app's version, the
// bands and modes in the post, what came of it, and how many spots it offered and sent. All of it is
// what PSKReporter shows the world once the report goes, and it is how we see who reports and what the
// route costs of the plan's daily requests.
// ---------------------------------------------------------------------------

const PSKR_HOST = "report.pskreporter.info";
const PSKR_PORT = 4739;
const PSKR_CONNECT_MS = 5000;

async function servePskr(request, env) {
  if (request.method !== "POST") return pskrJson({ ok: false, error: "method" }, 405);
  let body;
  try {
    const text = await request.text();
    if (text.length > PSKR_MAX_BODY) return pskrJson({ ok: false, error: "too_large" }, 413);
    body = JSON.parse(text);
  } catch (e) {
    return pskrJson({ ok: false, error: "bad_request" }, 400);
  }
  const checked = checkReport(body, Math.floor(Date.now() / 1000));
  if (!checked.ok) return pskrJson(checked, 400);
  const report = checked.report;
  const offered = Array.isArray(body.spots) ? body.spots.length : 0;
  if (!env.PSKR_GATE) return pskrJson({ ok: false, error: "unavailable" }, 503);
  if (report.spots.length === 0) {
    countPskr(env, report, report.spots, "empty", offered, 0);
    return pskrJson({ ok: true, sent: 0, retry_in: 0 });
  }

  let decision;
  try {
    const stub = env.PSKR_GATE.get(env.PSKR_GATE.idFromName(report.call));
    const r = await stub.fetch("https://gate/", { method: "POST", body: JSON.stringify({ spots: report.spots }) });
    decision = await r.json();
  } catch (e) {
    countPskr(env, report, report.spots, "gate_failed", offered, 0);
    return pskrJson({ ok: false, error: "unavailable" }, 503);
  }
  if (decision.action !== "send") {
    countPskr(env, report, report.spots, decision.action, offered, 0);
    return pskrJson({ ok: true, sent: 0, retry_in: decision.retryMs || 0 });
  }

  const message = encodeMessage({
    call: report.call, grid: report.grid, software: pskrSoftware(report.app), spots: decision.spots,
    seq: decision.seq, domain: domainOf(report.call), exportS: Math.floor(Date.now() / 1000),
  });
  try {
    await pskrSend(env, message);
  } catch (e) {
    // The gate counts these sent: they are lost, as a UDP report would be, and the page is told to
    // move on rather than send them twice.
    countPskr(env, report, decision.spots, "send_failed", offered, 0);
    return pskrJson({ ok: false, error: "unreachable", retry_in: decision.retryMs });
  }
  countPskr(env, report, decision.spots, "sent", offered, decision.spots.length);
  return pskrJson({ ok: true, sent: decision.spots.length, retry_in: decision.retryMs });
}

// What PSKReporter's statistics list the station under: one name, so every build counts together,
// and the build's own version beside it (docs/architecture/system.md, Third-party services).
export function pskrSoftware(app) {
  const version = appVersion(app);
  return version ? `${APP_NAME} ${version}` : APP_NAME;
}

// One message on its own connection, closed once written. `env.PSKR_CONNECT` stands in for
// cloudflare:sockets' connect() in a test, which runs under node.
async function pskrSend(env, bytes) {
  const connect = env.PSKR_CONNECT || (await import("cloudflare:sockets")).connect;
  const socket = connect({ hostname: PSKR_HOST, port: PSKR_PORT });
  let timer;
  const timeout = new Promise((_, reject) => {
    timer = setTimeout(() => reject(new Error("timeout")), PSKR_CONNECT_MS);
  });
  try {
    await Promise.race([socket.opened, timeout]);
    const writer = socket.writable.getWriter();
    await Promise.race([writer.write(bytes), timeout]);
    await Promise.race([writer.close(), timeout]);
  } finally {
    clearTimeout(timer);
    try {
      await socket.close();
    } catch (e) {
      // Already closed.
    }
  }
}

// One Analytics Engine data point per post. A count never fails a report.
function countPskr(env, report, spots, outcome, offered, sent) {
  const bands = [...new Set(spots.map((s) => bandOf(s.f)))].sort().join(" ");
  const modes = [...new Set(spots.map((s) => s.m))].sort().join(" ");
  try {
    env.PSKR_REPORTS.writeDataPoint({
      blobs: [report.call, report.grid.slice(0, 4), appVersion(report.app), bands, modes, outcome],
      doubles: [offered, sent],
      indexes: [report.call],
    });
  } catch (e) {
    // Uncounted.
  }
}

function pskrJson(body, status) {
  return new Response(JSON.stringify(body), {
    status: status || 200,
    headers: {
      "content-type": "application/json; charset=utf-8",
      "access-control-allow-origin": "*",
      "cache-control": "no-store",
    },
  });
}
