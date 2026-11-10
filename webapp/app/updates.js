// Firmware updates, without the page (docs/architecture/webapp.md, Updates): what the release
// manifest offers each board, what to hand the sidekick, and what its progress reports mean in
// words. app.html draws what these return; webapp/tests/test_updates.js runs this file under node.
//
// Plain global, like swr_core.js: no module system.
var FwUpdates = (function () {
  'use strict';

  // The manifest is read over HTTPS, which is what makes its hashes worth trusting. The images are
  // fetched by the sidekick, which has no TLS, so their URLs are plain HTTP on the same host
  // (docs/architecture/system.md, The inversion).
  var MANIFEST_URL = 'https://micro-ft8.kalikstein.com/download/manifest.json';
  var IMAGE_BASE = 'http://micro-ft8.kalikstein.com/download/';

  // board: the name the sidekick's API uses. asset: its key under the manifest's `assets`.
  var BOARDS = [
    { board: 'sidekick', asset: 'sidekick', label: 'Sidekick' },
  ];

  function otaOk(o) {
    return !!o && typeof o.stable === 'string' && /^[A-Za-z0-9._-]+$/.test(o.stable) &&
      typeof o.size === 'number' && o.size > 0 && o.size % 1 === 0 &&
      typeof o.sha256 === 'string' && /^[0-9a-f]{64}$/.test(o.sha256) &&
      typeof o.version === 'string' && /^[!#-\[\]-~]{1,31}$/.test(o.version);
  }

  // The boards the manifest has a different build for. `running` maps a board to the version it
  // reports ({sidekick: 'e6e4d88'}); a board that reports none is not offered anything. Versions
  // are commits, with no order to compare, so "different from the published build" is the test.
  function offers(manifest, running) {
    var out = [];
    var assets = manifest && manifest.assets;
    if (!assets || !running) return out;
    BOARDS.forEach(function (b) {
      var from = running[b.board];
      var o = assets[b.asset] && assets[b.asset].ota;
      if (!from || !otaOk(o) || o.version === from) return;
      out.push({
        board: b.board, label: b.label, from: from, to: o.version,
        size: o.size, sha256: o.sha256, url: IMAGE_BASE + o.stable,
      });
    });
    return out;
  }

  // POST /api/fw/start's body (sidekick/main/fw_policy.h, fw_request_parse()).
  function startBody(offer) {
    return 'board=' + offer.board + '\nurl=' + offer.url + '\nsize=' + offer.size +
      '\nsha256=' + offer.sha256 + '\nversion=' + offer.to + '\n';
  }

  function sizeText(bytes) {
    return bytes >= 1048576 ? (bytes / 1048576).toFixed(1) + ' MB' : Math.round(bytes / 1024) + ' KB';
  }

  // Why the sidekick refused to start, or why an update failed, by its `why` code.
  var WHY_TEXT = {
    offline: 'Connect the sidekick to WiFi with internet',
    busy: 'An update is already running',
    confirming: 'The last update is still being confirmed; try again in a minute',
    unsupported: 'This sidekick cannot update that board',
    dns: 'Download failed: the sidekick’s WiFi has no internet',
    connect: 'Download failed: the sidekick could not reach the download server',
    download: 'Download failed',
    http: 'Download failed: the server refused',
    size: 'Verification failed',
    hash: 'Verification failed',
    image: 'Verification failed',
    version: 'Verification failed',
    flash: 'The sidekick could not store the image',
    cancelled: 'Cancelled',
  };
  function whyText(why) {
    return WHY_TEXT[why] || 'Update failed' + (why ? ' (' + why + ')' : '');
  }

  // One line for an update in flight, from GET /api/fw/status. `reachable` is false while the
  // sidekick is not answering, which during an update means it is restarting.
  function progressText(status, reachable) {
    if (!reachable) return 'Restarting the sidekick…';
    if (!status) return 'Starting…';
    if (status.state === 'downloading') {
      var pct = status.size > 0 ? Math.floor(status.got * 100 / status.size) : 0;
      return 'Downloading… ' + pct + '%';
    }
    if (status.state === 'verifying') return 'Verifying…';
    if (status.state === 'restarting') return 'Restarting the sidekick…';
    return 'Confirming the new version…';
  }

  // How an update to `target` ended, from GET /api/fw/status: null while it is still running,
  // else {ok, text}. After its restart the sidekick reports what came of it as `last`.
  function outcome(status, target) {
    if (!status) return null;
    if (status.state === 'failed') {
      return { ok: false, text: whyText(status.why) + ' — still on ' + status.running };
    }
    if (status.state !== 'idle') return null;
    var last = status.last;
    if (!last || last.version !== target) return null;
    if (last.result === 'ok') return { ok: true, text: 'Updated to ' + target };
    if (last.result === 'rolled_back') {
      return { ok: false, text: 'New version did not start — rolled back to ' + status.running };
    }
    return null;
  }

  return {
    MANIFEST_URL: MANIFEST_URL,
    offers: offers,
    startBody: startBody,
    sizeText: sizeText,
    whyText: whyText,
    progressText: progressText,
    outcome: outcome,
  };
})();
