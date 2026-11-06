// The site's two dynamic routes. Everything else is a static file that
// Workers Static Assets serves without running this, with its headers from
// _headers (docs/architecture/webapp.md, Serving the site): wrangler.jsonc's
// run_worker_first names exactly these two paths.
//
// /download/* is served from the DOWNLOADS R2 bucket (public continuous
// firmware, source and getting-started sheet mirror). See docs/RELEASE_PROCESS.md.
// /pskreporter is the PSKReporter filter (below).
//
// A path with no file never lands here: Static Assets answers it with 404.html
// (wrangler.jsonc, not_found_handling). The last return is a backstop only.
export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname.startsWith("/download/")) {
      return serveDownload(request, env, url);
    }
    if (url.pathname === "/pskreporter") {
      return servePskReporter(request, url);
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
  if (key.endsWith(".pdf")) return "application/pdf";
  if (key.endsWith(".sha256")) return "text/plain; charset=utf-8";
  return "application/octet-stream";
}

// I37 (Tools: PSKReporter -- who is hearing me), docs/architecture/system.md (Third-party services). PSKReporter
// sends no Access-Control-Allow-Origin (measured 2026-09-10, re-confirmed
// 2026-09-21 against both retrieve.pskreporter.info/query and
// cgi-bin/pskquery5.pl), so the browser can't call it directly -- and even
// fetched here, where CORS doesn't apply, one query for a single callsign
// over the last hour measured 1.25 MB of XML, almost all of it an unrelated
// <activeReceiver> dump (every currently-active receiving station
// network-wide, not specific to the requested callsign) rather than the
// handful of <receptionReport> elements -- the actual "who heard this
// callsign" spots -- the card needs. This fetches server-side and re-serves
// only those, as small JSON: filter, not relay, per docs/architecture/system.md (Third-party services)'s own rule for this
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
    if (!r.ok) return jsonResponse({ error: "pskreporter_unreachable", detail: "HTTP " + r.status }, 502);
    xml = await r.text();
  } catch (e) {
    // PSKReporter needs no credentials and this carries none, so there is
    // nothing sensitive in the underlying error -- surfaced rather than
    // swallowed, same "degrade visibly" instinct docs/architecture/webapp.md (Tools) applies to
    // every online Tools card, one layer further in so the card itself can
    // eventually show *why* (timeout vs. DNS vs. a hard refusal) rather than
    // a single undifferentiated "unreachable".
    return jsonResponse({ error: "pskreporter_unreachable", detail: String((e && e.message) || e) }, 502);
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
