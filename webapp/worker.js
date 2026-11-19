// The site's two dynamic routes. Everything else is a static file that
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
// A path with no file never lands here: Static Assets answers it with 404.html
// (wrangler.jsonc, not_found_handling). The last return is a backstop only.
export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname.startsWith("/download/")) {
      return serveDownload(request, env, url);
    }
    if (url.pathname.startsWith("/qrz/")) {
      return serveQrz(request, url);
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
// sends QRZ changes. The app's word for its version is a caller's: only a plain version string is put in
// a header, and anything else reads as 0.
const QRZ_RELAY_REVISION = 1;
const QRZ_APP_VERSION_FORM = /^[0-9A-Za-z][0-9A-Za-z._-]{0,31}$/;

export function qrzUserAgent(app) {
  const version = typeof app === "string" && QRZ_APP_VERSION_FORM.test(app) ? app : "0";
  return `Micro-FT8/${version} (relay ${QRZ_RELAY_REVISION}; https://micro-ft8.kalikstein.com)`;
}
const QRZ_PAGE = 250; // records asked of QRZ at a time, the page its documentation suggests
const QRZ_MAX_INSERTS = 50; // QRZ takes one QSO a call, and the free plan allows 50 calls out a request
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
  for (const r of records) last = Math.max(last, Number(r.logid) || 0);
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

function trimmedQrzRecord(f) {
  return {
    logid: f.app_qrzlog_logid || "",
    call: f.call,
    qso_date: f.qso_date || "",
    time_on: f.time_on || "",
    band: f.band || "",
    // QRZ writes FT4 as a mode; a record that came in as MFSK with SUBMODE FT4 is the same QSO.
    mode: f.submode || f.mode || "",
    station_callsign: f.station_callsign || "",
    confirmed: f.app_qrzlog_status === "C",
  };
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
