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
