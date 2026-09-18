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
