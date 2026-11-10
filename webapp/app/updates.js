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

  // board: the name the sidekick's API uses. asset: its key under the manifest's `assets`. The main
  // board is first: when both need updating it goes first, while the sidekick still speaks its
  // protocol (docs/roadmap/ota-updates.md, Decided).
  var BOARDS = [
    { board: 'main', asset: 'poe_p4', label: 'Main board' },
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
        size: o.size, sha256: o.sha256, url: IMAGE_BASE + o.stable, proto: o.proto | 0,
      });
    });
    return out;
  }

  // One image's lines in POST /api/fw/start's body (sidekick/main/fw_policy.h, fw_request_parse()).
  function startBody(offer) {
    return 'board=' + offer.board + '\nurl=' + offer.url + '\nsize=' + offer.size +
      '\nsha256=' + offer.sha256 + '\nversion=' + offer.to + '\n';
  }

  // What one tap of Update does: every offer, in order, in one request the sidekick carries out by
  // itself (fw_plan_parse()), so a locked phone stops nothing. `link` is what the boards speak now:
  // {hostLinked, mainProto, sidekickProto} from /api/config's host_proto and sk_proto (0 unknown).
  // blocked: '' or why the plan must not run. The one reason today is 'pair': afterwards the two
  // boards would speak different Port A protocol versions, and a main board the sidekick cannot
  // talk to can only be recovered over USB.
  function plan(manifest, running, link) {
    var steps = offers(manifest, running);
    var after = { main: link.mainProto | 0, sidekick: link.sidekickProto | 0 };
    steps.forEach(function (o) { after[o.board] = o.proto; });
    var blocked = link.hostLinked && after.main && after.sidekick && after.main !== after.sidekick ? 'pair' : '';
    return {
      steps: steps,
      blocked: blocked,
      body: steps.map(startBody).join('next\n'),
      // Unmeasured for the main board: a guess until a bench run times the transfer.
      time: steps.some(function (o) { return o.board === 'main'; }) ? 'a few minutes' : 'about a minute',
    };
  }

  // What stopping the radio costs right now, as sentences, from the queue the app shows: entries
  // {dxcall, state, tries_sent}, state being autoseq's (0 calling CQ, 1 replying, 2 and up a QSO
  // under way). Empty when nothing is in progress.
  function stopCosts(queue) {
    var out = [];
    var queued = 0;
    (queue || []).forEach(function (q) {
      if (q.dxcall === 'CQ') out.unshift('Your CQ will end');
      else if (q.state >= 2 || (q.tries_sent | 0) > 0) out.push('QSO with ' + q.dxcall + ' will be abandoned');
      else queued++;
    });
    if (queued) out.push(queued + (queued === 1 ? ' queued reply' : ' queued replies') + ' will be cleared');
    return out;
  }

  // The steps of an update as the progress screen lists them, from GET /api/fw/status: its `plan`
  // (the boards, in order), `step` (the one in hand) and `state`. Each {text, at}: at is 'done',
  // 'now' or 'next'; a failure leaves its step 'now'. `reachable` false means the sidekick is not
  // answering, which at the end of its own update is its restart.
  function stepRows(status, reachable) {
    var rows = [{ key: 'stopping', text: 'Stop the radio' }];
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
    var step = status ? status.step | 0 : 0;
    // Before a later image the main board is stopped again; that moment belongs to the image's download.
    var now = at === 'stopping' ? (step ? step + ':downloading' : 'stopping') : step + ':' + at;
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
    return !!status && ['stopping', 'downloading', 'verifying', 'transferring', 'restarting'].indexOf(status.state) >= 0;
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
    host_mode: 'The main board would not stop for the update',
    tx: 'The main board did not stop transmitting',
    no_slot: 'This main board needs one flash over USB before it can update over the air',
    link: 'The main board stopped answering during the transfer',
    host_restarted: 'The main board restarted during the transfer',
    host_flash: 'The main board could not store the image',
    host: 'The main board refused the image',
    rolled_back: 'New version did not start',
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
    var pct = status.size > 0 ? Math.floor(status.got * 100 / status.size) : 0;
    if (status.state === 'stopping') return 'Stopping the radio…';
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
    plan: plan,
    stopCosts: stopCosts,
    stepRows: stepRows,
    busy: busy,
    sizeText: sizeText,
    whyText: whyText,
    progressText: progressText,
    outcome: outcome,
  };
})();
