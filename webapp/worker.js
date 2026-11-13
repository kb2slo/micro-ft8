// The site's one dynamic route. Everything else is a static file that
// Workers Static Assets serves without running this, with its headers from
// _headers (docs/architecture/webapp.md, Serving the site): wrangler.jsonc's
// run_worker_first names exactly this path.
//
// /download/* is served from the DOWNLOADS R2 bucket (public continuous
// firmware, source and getting-started sheet mirror). See docs/RELEASE_PROCESS.md.
//
// A path with no file never lands here: Static Assets answers it with 404.html
// (wrangler.jsonc, not_found_handling). The last return is a backstop only.
export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname.startsWith("/download/")) {
      return serveDownload(request, env, url);
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

  // Key is everything after "/download/": one file name, or one under ota/ (each OTA image by
  // its dated name: docs/RELEASE_PROCESS.md). Reject empty, traversal, and any other nesting.
  const key = decodeURIComponent(url.pathname.slice("/download/".length));
  const name = key.startsWith("ota/") ? key.slice(4) : key;
  if (!name || name.includes("/") || name.includes("\\") || name.includes("..")) {
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
      `attachment; filename="${name.replace(/"/g, "")}"`,
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
