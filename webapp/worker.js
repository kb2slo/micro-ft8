// Adds CORS so the sidekick's plain-HTTP page (http://micro-ft8.local/) can
// fetch this bundle cross-origin -- Cloudflare Workers Static Assets serves
// files with no CORS header by default, unlike GitHub Pages, which is what
// RFC 0004 SS4's "publicly-trusted certificate, and a CORS header permitting
// a plain-HTTP origin to read the response" host requirement was measured
// against. Everything else about serving stays default asset behavior.
//
// /download/* is served from the DOWNLOADS R2 bucket (public continuous
// firmware + source mirror). See docs/RELEASE_PROCESS.md.
export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname.startsWith("/download/")) {
      return serveDownload(request, env, url);
    }
    if (url.pathname === "/pskreporter") {
      return servePskReporter(request, url);
    }

    const response = await env.ASSETS.fetch(request);
    const headers = new Headers(response.headers);
    headers.set("Access-Control-Allow-Origin", "*");
    // Static Assets' text/html responses carry no charset -- the sidekick's
    // own httpd (web_fs.c) always sends "text/html; charset=utf-8" for every
    // page it serves, so this host was the one silently relying on a
    // browser's own encoding guess instead. That guess is more reliable the
    // more valid UTF-8 it has to look at, which is exactly why this went
    // unnoticed on app.html/map.html (dense, tens of KB) but produced real
    // mojibake on geo.html (a few hundred bytes, one non-ASCII character
    // total, found live 2026-09-22 on a real phone) -- not a defect unique
    // to that one page, just the first one small enough to expose it.
    const contentType = headers.get("content-type");
    if (contentType && /^text\/html\b/i.test(contentType) && !/charset=/i.test(contentType)) {
      headers.set("Content-Type", "text/html; charset=utf-8");
    }
    // geo.html (RFC 0004 SS11's geolocation handoff) is the one page on this
    // site whose whole point is working with zero network -- an operator
    // reaching it at all already crossed the hard part (getting real
    // internet once). Static Assets' own default is
    // "public, max-age=0, must-revalidate" (measured 2026-09-22), which
    // would force a revalidation round-trip on *every* visit, defeating
    // that entirely offline. immutable + a long max-age means a browser
    // that already has a fresh copy skips the network outright -- not a
    // fallback-after-failure, a real "don't even try" -- for as long as it
    // keeps that cache entry. Scoped to this one path: every other asset
    // here (app.html, map.html, manifest.json, ...) still wants normal
    // revalidation, since those *do* change on every deploy.
    //
    // "/geo", not "/geo.html": Static Assets' default html_handling
    // (auto-trailing-slash) 307s any *.html request to its extensionless
    // canonical URL before this header would ever reach the real content --
    // confirmed live 2026-09-22, the header landed on the redirect while
    // "/geo" itself kept serving the default must-revalidate header. Callers
    // (map.html, app.html) link straight to "/geo" for the same reason: one
    // fewer round trip on the one page where a round trip is the whole cost
    // being avoided.
    if (url.pathname === "/geo") {
      headers.set("Cache-Control", "public, max-age=31536000, immutable");
    }
    return new Response(response.body, {
      status: response.status,
      statusText: response.statusText,
      headers,
    });
  },
};

async function serveDownload(request, env, url) {
  if (request.method !== "GET" && request.method !== "HEAD") {
    return new Response("Method not allowed", { status: 405 });
  }

  // Key is everything after "/download/"; reject empty, traversal, and nested paths.
  const key = decodeURIComponent(url.pathname.slice("/download/".length));
  if (!key || key.includes("/") || key.includes("\\") || key.includes("..")) {
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
  if (!key.endsWith(".json") && !key.endsWith(".sha256")) {
    headers.set(
      "content-disposition",
      `attachment; filename="${key.replace(/"/g, "")}"`,
    );
  }

  if (request.method === "HEAD") {
    return new Response(null, { status: 200, headers });
  }
  return new Response(obj.body, { status: 200, headers });
}

function guessContentType(key) {
  if (key.endsWith(".json")) return "application/json; charset=utf-8";
  if (key.endsWith(".tar.gz")) return "application/gzip";
  if (key.endsWith(".sha256")) return "text/plain; charset=utf-8";
  return "application/octet-stream";
}

// I37 (Tools: PSKReporter -- who is hearing me), RFC 0004 SS8. PSKReporter
// sends no Access-Control-Allow-Origin (measured 2026-09-10, re-confirmed
// 2026-09-21 against both retrieve.pskreporter.info/query and
// cgi-bin/pskquery5.pl), so the browser can't call it directly -- and even
// fetched here, where CORS doesn't apply, one query for a single callsign
// over the last hour measured 1.25 MB of XML, almost all of it an unrelated
// <activeReceiver> dump (every currently-active receiving station
// network-wide, not specific to the requested callsign) rather than the
// handful of <receptionReport> elements -- the actual "who heard this
// callsign" spots -- the card needs. This fetches server-side and re-serves
// only those, as small JSON: filter, not relay, per SS8's own rule for this
// route. No secrets either way -- PSKReporter needs no credentials.
const PSKREPORTER_QUERY_URL = "https://retrieve.pskreporter.info/query";
async function servePskReporter(request, url) {
  if (request.method !== "GET" && request.method !== "HEAD") {
    return jsonResponse({ error: "method_not_allowed" }, 405);
  }
  const call = (url.searchParams.get("call") || "").trim().toUpperCase();
  // Loose callsign shape, not real validation -- PSKReporter itself is the
  // actual validator; this only keeps obviously-wrong input (empty, a
  // sentence, path-traversal-shaped junk) from reaching it as a query param.
  if (!/^[A-Z0-9/]{3,15}$/.test(call)) {
    return jsonResponse({ error: "invalid_call" }, 400);
  }

  const upstream = new URL(PSKREPORTER_QUERY_URL);
  upstream.searchParams.set("senderCallsign", call);
  upstream.searchParams.set("flowStartSeconds", "-3600");
  upstream.searchParams.set("mode", "FT8");

  let xml;
  try {
    const r = await fetch(upstream);
    if (!r.ok) return jsonResponse({ error: "pskreporter_unreachable" }, 502);
    xml = await r.text();
  } catch (e) {
    return jsonResponse({ error: "pskreporter_unreachable" }, 502);
  }

  if (request.method === "HEAD") {
    return jsonResponse(null, 200);
  }
  return jsonResponse({ call, reports: parseReceptionReports(xml, call) }, 200);
}

// PSKReporter's query XML is flat, single-level, self-closing elements --
// no nesting, no CDATA -- exactly the shape a small regex extractor handles
// safely without a full XML parser. That matters here specifically: this
// Worker ships as one plain file with no bundler/npm dependency step to add
// one through, and the Workers runtime has no DOMParser. Only
// <receptionReport> is ever matched -- the <activeReceiver> dump that makes
// up nearly all of the real payload is never even looked at.
function parseReceptionReports(xml, expectedCall) {
  const reports = [];
  const tagRe = /<receptionReport\b([^>]*)\/>/g;
  let m;
  while ((m = tagRe.exec(xml)) !== null) {
    const attrs = {};
    const attrRe = /([a-zA-Z]+)="([^"]*)"/g;
    let am;
    while ((am = attrRe.exec(m[1])) !== null) {
      attrs[am[1]] = decodeXmlEntities(am[2]);
    }
    // Defense in depth, not the real filter -- senderCallsign is already
    // scoped by the upstream query param above.
    if ((attrs.senderCallsign || "").toUpperCase() !== expectedCall) continue;
    const freqHz = Number(attrs.frequency);
    const ts = Number(attrs.flowStartSeconds);
    if (!attrs.receiverCallsign || !Number.isFinite(freqHz) || !Number.isFinite(ts)) continue;
    reports.push({
      call: attrs.receiverCallsign,
      grid: attrs.receiverLocator || "",
      dxcc: attrs.receiverDXCC || "",
      freqHz,
      mode: attrs.mode || "",
      ts,
      snr: attrs.sNR !== undefined ? Number(attrs.sNR) : null,
    });
  }
  // Most recent first, same convention the app's own decode/QSO rows use.
  reports.sort((a, b) => b.ts - a.ts);
  return reports;
}

function decodeXmlEntities(s) {
  return s
    .replace(/&#(\d+);/g, (_, d) => String.fromCodePoint(Number(d)))
    .replace(/&#x([0-9a-fA-F]+);/g, (_, h) => String.fromCodePoint(parseInt(h, 16)))
    .replace(/&lt;/g, "<")
    .replace(/&gt;/g, ">")
    .replace(/&quot;/g, '"')
    .replace(/&apos;/g, "'")
    .replace(/&amp;/g, "&");
}

function jsonResponse(obj, status) {
  return new Response(obj === null ? null : JSON.stringify(obj), {
    status,
    headers: {
      "content-type": "application/json; charset=utf-8",
      "access-control-allow-origin": "*",
      "cache-control": "no-store",
    },
  });
}
