// Shared pairing for every sidekick page (docs/architecture/system.md (Who may key the transmitter)).
//
// Served at /pairing.js. Pages include it with <script src=/pairing.js></script>
// and call MicroFT8Pairing.boot(...) so re-auth is the same gesture everywhere:
// press the pairing button (the sidekick's one button); this script claims the token while the disclosure
// window is open. No copy-paste, no prompt().
//
// The device-side window is one-shot: the first successful GET closes it, so a
// successful re-auth does not leave the secret readable for the rest of the
// old 120 s timer. The NVS token itself is not rotated -- that would log out
// every other already-paired browser.

(function (global) {
  var KEY = 'microft8_token';
  var HEADER = 'X-MicroFT8-Token';
  var POLL_MS = 1000;
  var polling = false;
  var statusEl = null;
  var onPaired = null;

  function getToken() {
    var t = localStorage.getItem(KEY);
    return t ? t.trim() : '';
  }

  function setToken(t) {
    if (t) localStorage.setItem(KEY, String(t).trim());
  }

  function clearToken() {
    localStorage.removeItem(KEY);
  }

  // The token is kept per address (localStorage is per origin), so a phone that paired at
  // 192.168.4.1 to save the home WiFi would read Not paired at micro-ft8.local. The Saved page's
  // link carries it there in its fragment, which no request sends (provision.html), and the page
  // that opens takes it at once and clears it from the address, before anything reads the hash.
  var CARRY_RE = /(?:^#|&)pair=([0-9a-f]{32})(?=&|$)/;

  // `href` with this browser's token in its fragment; `href` unchanged when there is none.
  function carryLink(href) {
    var t = getToken();
    return t ? String(href).split('#')[0] + '#pair=' + t : href;
  }

  function takeCarried() {
    var m = CARRY_RE.exec(global.location.hash || '');
    if (!m) return;
    setToken(m[1]);
    global.history.replaceState(null, '', global.location.pathname + global.location.search);
  }
  takeCarried();

  function setStatus(msg) {
    if (statusEl) statusEl.textContent = msg;
  }

  function authHeaders(extra) {
    var h = extra ? Object.assign({}, extra) : {};
    h[HEADER] = getToken();
    return h;
  }

  function authFetch(url, init) {
    init = init || {};
    init.headers = authHeaders(init.headers || {});
    return fetch(url, init);
  }

  async function tryClaim() {
    var r = await fetch('/api/pairing-token');
    if (r.status === 404) return null;
    if (!r.ok) return null;
    var j = await r.json();
    return j && j.token ? String(j.token).trim() : null;
  }

  async function pollUntilPaired() {
    if (polling) return;
    polling = true;
    setStatus('Press the pairing button');
    try {
      for (;;) {
        if (getToken()) {
          if (onPaired) onPaired(getToken());
          return;
        }
        try {
          var tok = await tryClaim();
          if (tok) {
            setToken(tok);
            setStatus('Paired');
            if (onPaired) onPaired(tok);
            return;
          }
        } catch (e) { /* keep waiting */ }
        setStatus('Press the pairing button');
        await new Promise(function (resolve) { setTimeout(resolve, POLL_MS) });
      }
    } finally {
      polling = false;
    }
  }

  // If a token is already stored, fire onPaired once. Otherwise start polling
  // for a button press. Safe to call from every page.
  function boot(opts) {
    opts = opts || {};
    statusEl = opts.status || null;
    onPaired = opts.onPaired || null;
    if (getToken()) {
      if (onPaired) onPaired(getToken());
      return;
    }
    pollUntilPaired();
  }

  // Drop a stored token that the server rejected, then wait for a new press.
  function needsReauth() {
    clearToken();
    pollUntilPaired();
  }

  global.MicroFT8Pairing = {
    KEY: KEY,
    HEADER: HEADER,
    getToken: getToken,
    setToken: setToken,
    clearToken: clearToken,
    carryLink: carryLink,
    authHeaders: authHeaders,
    authFetch: authFetch,
    boot: boot,
    needsReauth: needsReauth
  };
})(window);
