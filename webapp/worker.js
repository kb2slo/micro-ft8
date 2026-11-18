// The site's one dynamic route. Everything else is a static file that
// Workers Static Assets serves without running this, with its headers from
// _headers (docs/architecture/webapp.md, Serving the site): wrangler.jsonc's
// run_worker_first names exactly this path.
//
// /download/* is served from the DOWNLOADS R2 bucket (both channels'
// firmware, source and getting-started sheet). See
// docs/RELEASE_PROCESS.md. Each file it serves is counted, by its name and
// by whether the downloads page's flasher fetched it (countDownload()).
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
