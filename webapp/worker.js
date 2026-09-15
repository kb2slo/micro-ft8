// Adds CORS so the sidekick's plain-HTTP page (http://microft8.local) can
// fetch this bundle cross-origin -- Cloudflare Workers Static Assets serves
// files with no CORS header by default, unlike GitHub Pages, which is what
// RFC 0004 SS4's "publicly-trusted certificate, and a CORS header permitting
// a plain-HTTP origin to read the response" host requirement was measured
// against. Everything else about serving stays default asset behavior.
export default {
  async fetch(request, env) {
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
