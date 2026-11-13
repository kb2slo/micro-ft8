// The station's one Update, without the page (docs/architecture/webapp.md, Settings › Software
// update): what the release manifest offers each board and the published app offers the page, the
// plan, what to hand the sidekick, the app's relay, and what progress and failure mean in words.
// app.html draws what these return; webapp/tests/test_updates.js runs this file under node.
//
// Plain global, like swr_core.js: no module system.
var FwUpdates = (function () {
  'use strict';

  // The manifest is read over HTTPS, which is what makes its hashes worth trusting. The images are
  // fetched by the sidekick, which has no TLS, so their URLs are plain HTTP on the same host
  // (docs/architecture/system.md, The inversion). Each is fetched by its dated name under ota/,
  // which a later publish never overwrites, so an update installs the build the operator accepted
  // however many are published while it runs (docs/RELEASE_PROCESS.md, Public Cloudflare mirror).
  var MANIFEST_URL = 'https://micro-ft8.kalikstein.com/download/manifest.json';
  var IMAGE_BASE = 'http://micro-ft8.kalikstein.com/download/ota/';

  // board: the name the sidekick's API uses. asset: its key under the manifest's `assets`. The main
  // board is first: when both need updating it goes first, while the sidekick still speaks its
  // protocol (docs/roadmap/ota-updates.md, Decided).
  var BOARDS = [
    { board: 'main', asset: 'poe_p4', label: 'Main board' },
    { board: 'sidekick', asset: 'sidekick', label: 'Sidekick' },
  ];

  function otaOk(o) {
    return !!o && typeof o.filename === 'string' && /^[A-Za-z0-9][A-Za-z0-9._-]*$/.test(o.filename) &&
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
        size: o.size, sha256: o.sha256, url: IMAGE_BASE + o.filename, proto: o.proto | 0,
      });
    });
    return out;
  }

  // One image's lines in POST /api/fw/start's body (sidekick/main/fw_policy.h, fw_request_parse()).
  function startBody(offer) {
    return 'board=' + offer.board + '\nurl=' + offer.url + '\nsize=' + offer.size +
      '\nsha256=' + offer.sha256 + '\nversion=' + offer.to + '\n';
  }

  // The app's build id, as build.py stamps it into app.html and writes it to build.json:
  // '<short sha>[-dirty] <UTC date>'. null for anything else, the unbuilt page's 'source' included.
  function buildCommit(id) {
    var m = /^([0-9a-f]{7,40})(-dirty)? \d{4}-\d{2}-\d{2}$/.exec(typeof id === 'string' ? id : '');
    return m ? { sha: m[1], dirty: !!m[2] } : null;
  }

  // Whether the published app (build.json beside the bundle's manifest: {build, size}) differs from
  // the one this page runs (`installed`, its stamped build id). Commits have no order, so different
  // is the test; a short sha matches a longer one it begins. A page that is unbuilt or -dirty is
  // always offered the published app. null: nothing to offer, or build.json not read or not sane.
  function appOffer(published, installed) {
    var to = published && buildCommit(published.build);
    if (!to || to.dirty || typeof published.size !== 'number' || !(published.size > 0)) return null;
    var from = buildCommit(installed);
    if (from && !from.dirty &&
        (from.sha.indexOf(to.sha) === 0 || to.sha.indexOf(from.sha) === 0)) return null;
    return { board: 'app', label: 'App', from: from ? from.sha + (from.dirty ? '-dirty' : '') : 'source',
      to: to.sha, size: published.size };
  }

  // What one tap of Update does (docs/roadmap/ota-updates.md, antu): the app first, by this
  // page's relay, then every firmware offer, in order, in one request the sidekick carries out by
  // itself (fw_plan_parse()), so a locked phone stops nothing once the app is in. `link` is the
  // station now: {hostLinked, ap, mainProto, sidekickProto}; hostLinked is a main board linked
  // and reporting its version, ap the sidekick on its own access point, the protos /api/config's
  // host_proto and sk_proto (0 unknown). `app` is appOffer()'s.
  //
  // unavailable: '' or why no update can be offered at all: 'no_host' (no main board: the boards
  // update together, and a sidekick alone is updated over USB) or 'ap' (no internet to download
  // from). blocked: '' or why this plan must not run: 'pair', afterwards the two boards would speak
  // different Port A protocol versions, and a main board the sidekick cannot talk to can only be
  // recovered over USB. Nothing checks for a radio: the review asks for it to be off.
  function plan(manifest, running, link, app) {
    var steps = offers(manifest, running);
    var after = { main: link.mainProto | 0, sidekick: link.sidekickProto | 0 };
    steps.forEach(function (o) { after[o.board] = o.proto; });
    var blocked = link.hostLinked && after.main && after.sidekick && after.main !== after.sidekick ? 'pair' : '';
    var main = steps.some(function (o) { return o.board === 'main'; });
    return {
      app: app || null,
      steps: steps,
      any: !!app || steps.length > 0,
      unavailable: !link.hostLinked ? 'no_host' : link.ap ? 'ap' : '',
      blocked: blocked,
      body: steps.map(startBody).join('next\n'),
      // Measured 2026-11-10: a main board's update 98 s (74 s of it the transfer), the sidekick's 28
      // to 42 s. The app's relay is seconds on a phone with signal.
      time: !steps.length ? 'under a minute' : !main ? 'about a minute'
        : steps.length > 1 ? 'about three minutes' : 'about two minutes',
    };
  }

  // Why Software update is unavailable, from plan().unavailable.
  var UNAVAILABLE_TEXT = {
    no_host: 'Software update needs the main board connected to the sidekick.',
    ap: 'Software update needs the sidekick on WiFi with internet; it is on its own network.',
  };

  // The review, before anything happens, as paragraphs. What is in progress was ended already, by
  // the band change's dialog before this one, so this stops nothing and says only that nothing
  // transmits until the update is done. Not said: that the station keeps receiving, since the main
  // board decodes nothing while its image is sent (main_board.md, Firmware updates).
  function reviewText(p) {
    var out = ['Takes ' + p.time + '. Nothing transmits until the update is done.'];
    if (p.steps.length) {
      out.push('Switch the radio off, or unplug its USB, before you update. If it stays connected it may ' +
        'need switching off and on afterwards.');
    }
    out.push(p.app ? 'Keep this page open until the app is installed' +
      (p.steps.length ? '; the rest goes on if this phone locks.' : '.') : 'The update goes on if this phone locks.');
    return out;
  }

  // The app's relay (docs/architecture/sidekick.md, Installing the app bundle): fetch the bundle's
  // manifest and each asset from the release host over HTTPS, which is what authenticates them, and
  // write them to the sidekick, which promotes nothing until every file is staged and checked. The
  // manifest is read afresh each run, so a retry gets whatever is published then.
  //
  // io: {get(url) -> Response from the host; begin(text), put(path, bytes), commit() -> Response from
  // the sidekick; progress(done, total) in bytes}. Resolves {ok: true} or {ok: false, why, detail}:
  // why 'network' (the host could not be reached or refused), 'pairing' (401), 'firmware' (the
  // sidekick's firmware is too old for this app: api_min), 'changed' (a file differed from the
  // manifest: published again mid-relay, or altered on the way), 'refused' (anything else the
  // sidekick said no to, in detail).
  async function relayApp(base, io) {
    // Manifest paths are relative and never climb (web_manifest.c refuses one that does), so each
    // asset is the base and its path.
    var root = base.replace(/\/+$/, '') + '/';
    var manUrl = root + 'manifest.json';
    var manifest, text;
    try {
      var mr = await io.get(manUrl);
      if (!mr.ok) return { ok: false, why: 'network', detail: 'manifest HTTP ' + mr.status };
      text = await mr.text();
      manifest = JSON.parse(text);
    } catch (e) {
      return { ok: false, why: 'network', detail: String(e && e.message ? e.message : e) };
    }
    var assets = (manifest && manifest.assets) || [];
    if (!assets.length) return { ok: false, why: 'network', detail: 'manifest has no assets' };
    var total = assets.reduce(function (n, a) { return n + (a.size | 0); }, 0);
    var refused = async function (r, what) {
      if (r.status === 401) return { ok: false, why: 'pairing' };
      var said = '';
      try { said = await r.text(); } catch (e) { /* no body */ }
      if (what === 'begin' && /api_min/.test(said)) return { ok: false, why: 'firmware', detail: said };
      if (what === 'put' && /sha256 mismatch/.test(said)) return { ok: false, why: 'changed', detail: said };
      return { ok: false, why: 'refused', detail: what + ': ' + said };
    };
    var r = await io.begin(text);
    if (!r.ok) return refused(r, 'begin');
    var done = 0;
    io.progress(0, total);
    for (var i = 0; i < assets.length; i++) {
      var a = assets[i], bytes;
      try {
        var ar = await io.get(root + a.path);
        if (!ar.ok) return { ok: false, why: 'network', detail: a.path + ' HTTP ' + ar.status };
        bytes = await ar.arrayBuffer();
      } catch (e) {
        return { ok: false, why: 'network', detail: String(e && e.message ? e.message : e) };
      }
      if (bytes.byteLength !== a.size) {
        return { ok: false, why: 'changed', detail: a.path + ' size ' + bytes.byteLength + ' != ' + a.size };
      }
      r = await io.put(a.path, bytes);
      if (!r.ok) return refused(r, 'put');
      done += a.size;
      io.progress(done, total);
    }
    r = await io.commit();
    if (!r.ok) return refused(r, 'commit');
    return { ok: true };
  }

  // The app's failure in words, by relayApp()'s `why`.
  var APP_WHY_TEXT = {
    network: 'The app could not be downloaded: this phone needs internet',
    pairing: 'Press the sidekick button to pair',
    firmware: 'The app needs newer sidekick firmware',
    changed: 'The app was published again while it installed',
    refused: 'The sidekick refused the app',
  };
  function appWhyText(why) {
    return APP_WHY_TEXT[why] || 'The app could not be installed';
  }

  // The app's row on the progress sheet. run: {at, done, total}, at 'now', 'done', 'after' (left
  // for after the firmware, which the app needs) or 'failed'.
  function appRow(run) {
    var text = 'Install the app (' + sizeText(run.total) + ')';
    if (run.at === 'now' && run.total > 0) text += ' \u00b7 ' + Math.floor(run.done * 100 / run.total) + '%';
    if (run.at === 'after') return { text: text + ', after the firmware', at: 'next' };
    return { text: text, at: run.at === 'failed' ? 'now' : run.at };
  }

  // The steps of an update as the progress screen lists them, from GET /api/fw/status: its `plan`
  // (the boards, in order), `step` (the one in hand) and `state`. Each {text, at}: at is 'done',
  // 'now' or 'next'; a failure leaves its step 'now'. `reachable` false means the sidekick is not
  // answering, which at the end of its own update is its restart.
  function stepRows(status, reachable) {
    var rows = [];
    var boards = (status && status.plan) || [];
    boards.forEach(function (b, i) {
      var name = b.board === 'main' ? 'main board' : 'sidekick';
      rows.push({ key: i + ':downloading', text: 'Download the ' + name + '\u2019s image (' + sizeText(b.size) + ')' });
      rows.push({ key: i + ':verifying', text: 'Verify it' });
      if (b.board === 'main') rows.push({ key: i + ':transferring', text: 'Send it to the main board' });
      rows.push({ key: i + ':restarting', text: 'Restart the ' + name + ' into ' + b.version });
    });
    var state = status ? status.state : 'idle';
    // A failure is shown at the step it happened in (`at`).
    var at = state === 'failed' ? status.at : state;
    var now = (status ? status.step | 0 : 0) + ':' + at;
    if (!reachable && boards.length) now = (boards.length - 1) + ':restarting';
    var idx = -1;
    rows.forEach(function (r, i) { if (r.key === now) idx = i; });
    var allDone = state === 'done';
    return rows.map(function (r, i) {
      var pct = '';
      if (i === idx && (state === 'downloading' || state === 'transferring') && status.size > 0) {
        pct = ' \u00b7 ' + Math.floor(status.got * 100 / status.size) + '%';
      }
      return { text: r.text + pct, at: allDone || (idx >= 0 && i < idx) ? 'done' : (i === idx ? 'now' : 'next') };
    });
  }

  // Whether an update is in flight, from GET /api/fw/status.
  function busy(status) {
    return !!status && ['downloading', 'verifying', 'transferring', 'restarting'].indexOf(status.state) >= 0;
  }

  function sizeText(bytes) {
    return bytes >= 1048576 ? (bytes / 1048576).toFixed(1) + ' MB' : Math.round(bytes / 1024) + ' KB';
  }

  // Why the sidekick refused to start, or why an update failed, by its `why` code.
  var WHY_TEXT = {
    offline: 'Connect the sidekick to WiFi with internet',
    busy: 'An update is already running',
    confirming: 'The last update is still being confirmed; try again in a minute',
    pair: 'This update would leave the two boards unable to talk to each other',
    plan: 'The sidekick did not understand the request',
    no_host: 'The main board is not answering',
    radio: 'The main board refused because a radio is attached; switch it off and try again',
    no_slot: 'This main board needs one flash over USB before it can update over the air',
    link: 'The main board stopped answering during the transfer',
    host_restarted: 'The main board restarted during the transfer',
    host_flash: 'The main board could not store the image',
    host: 'The main board refused the image',
    rolled_back: 'New version did not start',
    connect: 'Download failed: the sidekick could not reach the download server; check its WiFi has internet',
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
    var pct = status.size > 0 ? Math.floor(status.got * 100 / status.size) : 0;
    if (status.state === 'downloading') return 'Downloading… ' + pct + '%';
    if (status.state === 'verifying') return 'Verifying…';
    if (status.state === 'transferring') return 'Sending to the main board… ' + pct + '%';
    if (status.state === 'restarting') {
      return status.board === 'main' ? 'Restarting the main board…' : 'Restarting the sidekick…';
    }
    return 'Confirming the new version…';
  }

  // How an update of `board` to `target` ended, from GET /api/fw/status: null while it is still
  // running, else {ok, text}. A main board's ends in the state `done` or `failed`. The sidekick's
  // own ends in its restart, after which it reports what came of it as `last`.
  function outcome(status, board, target) {
    if (!status) return null;
    if (status.state === 'failed') {
      if (status.board === 'main') {
        var on = !status.host ? '' :
          (status.why === 'rolled_back' ? ' — rolled back to ' : ' — still on ') + status.host;
        return { ok: false, text: whyText(status.why) + on };
      }
      return { ok: false, text: whyText(status.why) + ' — still on ' + status.running };
    }
    if (board === 'main') {
      return status.state === 'done' && status.board === 'main' && status.version === target
        ? { ok: true, text: 'Updated to ' + target } : null;
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
    buildCommit: buildCommit,
    appOffer: appOffer,
    plan: plan,
    unavailableText: function (why) { return UNAVAILABLE_TEXT[why] || ''; },
    reviewText: reviewText,
    relayApp: relayApp,
    appWhyText: appWhyText,
    appRow: appRow,
    stepRows: stepRows,
    busy: busy,
    sizeText: sizeText,
    whyText: whyText,
    progressText: progressText,
    outcome: outcome,
  };
})();
