// Who hears me's listener, shared by app.html and map.html (docs/architecture/webapp.md, Who hears me): the socket
// to PSKReporter's live feed, its reconnects, and the phone's copy of the spots. Needs pskr_core.js first;
// webapp/tests/test_pskr_live.js drives it with a fake socket and clock.
//
// A page calls start() once with three hooks: call() names the call to follow ('' for none); want() says whether
// the page wants to listen beyond PskrCore.wanted() (Tools on screen, the map's layer on); changed(what) redraws,
// what being 'spots', 'status' or 'tick'. Both pages keep one copy in localStorage (same origin); a page loads it on
// start and saves while listening. Two tabs open at once each merge the other's spots when it saves (the storage
// event below).
//
// Plain global, like swr_core.js: no module system.
var PskrLive = (function () {
  'use strict';

  var KEY = 'microft8_pskr_v1';
  var TICK_MS = 15000;
  var hooks = null;
  var s = { state: null, ws: null, reader: null, subscribed: false, lastAliveMs: 0, pingTimer: null,
            saveTimer: null, status: 'idle', failures: 0, nextTryMs: 0 };

  function changed(what) { if (hooks) hooks.changed(what); }
  function setStatus(status) {
    if (s.status === status) return;
    s.status = status;
    changed('status');
  }
  function currentCall() {
    var c = String(hooks.call() || '').trim().toUpperCase();
    return PskrCore.topicFor(c) ? c : '';
  }

  function saveNow() {
    if (s.saveTimer) { clearTimeout(s.saveTimer); s.saveTimer = null; }
    if (!s.state) return;
    try { localStorage.setItem(KEY, JSON.stringify(s.state)); } catch (e) { /* full or blocked: this session still shows them */ }
  }
  function saveSoon() {
    if (!s.saveTimer) s.saveTimer = setTimeout(saveNow, 2000);
  }
  function load(call) {
    var saved = null;
    try { saved = JSON.parse(localStorage.getItem(KEY) || 'null'); } catch (e) { /* unreadable: start fresh */ }
    return PskrCore.restore(saved, call);
  }

  // Ends the socket. The listening range already ends at the last packet heard, so nothing is recorded here.
  function drop() {
    var ws = s.ws;
    s.ws = null;
    s.subscribed = false;
    if (s.pingTimer) { clearInterval(s.pingTimer); s.pingTimer = null; }
    if (!ws) return;
    ws.onopen = ws.onmessage = ws.onclose = ws.onerror = null;
    try {
      if (ws.readyState === 1) ws.send(PskrCore.DISCONNECT);
      ws.close();
    } catch (e) { /* already gone */ }
  }
  // Backs off 15 s, 30 s, 60 s... up to 5 min, so a broker that is down is not hammered by every open page.
  function failed() {
    drop();
    s.failures++;
    s.nextTryMs = Date.now() + Math.min(300000, TICK_MS * Math.pow(2, s.failures - 1));
    setStatus(navigator.onLine === false ? 'offline' : 'unreachable');
  }
  function refused() {
    drop();
    s.nextTryMs = Date.now() + 300000;
    setStatus('refused');
  }

  function connect() {
    var call = s.state.call, ws;
    s.subscribed = false;
    s.reader = PskrCore.newReader();
    s.lastAliveMs = Date.now(); // a socket that never opens is dead after DEAD_MS too
    try { ws = new WebSocket(PskrCore.URL, 'mqtt'); } catch (e) { failed(); return; }
    ws.binaryType = 'arraybuffer';
    s.ws = ws;
    setStatus('connecting');
    ws.onopen = function () {
      ws.send(PskrCore.connectPacket('mft8-' + Math.random().toString(16).slice(2, 12)));
    };
    ws.onmessage = function (ev) {
      var packets, now = Date.now(), spots = [];
      try { packets = PskrCore.readPackets(s.reader, new Uint8Array(ev.data)); } catch (e) { failed(); return; }
      s.lastAliveMs = now;
      for (var i = 0; i < packets.length && s.ws === ws; i++) {
        var p = packets[i];
        if (p.type === PskrCore.CONNACK) {
          if (p.body[1] !== 0) { refused(); return; }
          ws.send(PskrCore.subscribePacket(1, PskrCore.topicFor(call)));
        } else if (p.type === PskrCore.SUBACK) {
          if (p.body[2] === 0x80) { refused(); return; }
          s.subscribed = true;
          s.failures = 0;
          PskrCore.listenStart(s.state, now);
          s.pingTimer = setInterval(function () {
            if (s.ws === ws && ws.readyState === 1) ws.send(PskrCore.PINGREQ);
          }, PskrCore.PING_MS);
          setStatus('listening');
        } else if (p.type === PskrCore.PUBLISH) {
          var spot = PskrCore.spotOf(PskrCore.publishOf(p).payload, call);
          if (spot) spots.push(spot);
        }
      }
      if (!s.subscribed) return;
      PskrCore.listenAlive(s.state, now);
      saveSoon();
      if (spots.length && PskrCore.addSpots(s.state, spots)) changed('spots');
    };
    ws.onclose = function () { if (s.ws === ws) failed(); };
  }

  function tick() {
    if (!hooks) return; // a page that has not started it yet
    var now = Date.now(), call = currentCall();
    if (!call) {
      drop();
      s.state = null;
      setStatus('idle');
      changed('tick');
      return;
    }
    if (!s.state || s.state.call !== call) {
      drop();
      s.state = load(call);
      s.failures = 0;
      s.nextTryMs = 0;
    }
    PskrCore.prune(s.state, now);
    // A phone that locked wakes with a socket that has not noticed it is gone.
    if (s.ws && now - s.lastAliveMs > PskrCore.DEAD_MS) failed();
    if (!(PskrCore.wanted(s.state, now) || hooks.want())) {
      if (s.ws) { drop(); saveNow(); }
      setStatus('idle');
    } else if (!s.ws && now >= s.nextTryMs) {
      if (navigator.onLine === false) setStatus('offline');
      else connect();
    }
    changed('tick');
  }
  // A config read whose call differs from the one followed (the first read, or a call changed in Settings) acts
  // at once, not at the next tick.
  function sync() {
    if (hooks && currentCall() !== (s.state ? s.state.call : '')) tick();
  }
  // Called on every TX HUD event that says we are on the air.
  function noteTx() {
    if (!hooks) return;
    if (!s.state) tick();
    if (!s.state) return;
    var listening = PskrCore.wanted(s.state, Date.now()) && s.ws;
    PskrCore.noteTx(s.state, Date.now());
    saveSoon();
    if (!listening) tick();
  }

  function start(h) {
    hooks = h;
    setInterval(tick, TICK_MS);
    document.addEventListener('visibilitychange', function () { if (document.visibilityState === 'visible') tick(); });
    window.addEventListener('online', tick);
    window.addEventListener('offline', tick);
    window.addEventListener('pagehide', saveNow);
    // Another tab of this origin (the app and the map side by side) saved its copy: take any spots this one lacks,
    // so the next save here does not drop them. Its listening and transmitting ranges stay its own.
    window.addEventListener('storage', function (ev) {
      if (ev.key !== KEY || !s.state || !ev.newValue) return;
      var other;
      try { other = PskrCore.restore(JSON.parse(ev.newValue), s.state.call); } catch (e) { return; }
      if (PskrCore.addSpots(s.state, other.spots)) changed('spots');
    });
    tick();
  }

  return {
    KEY: KEY, TICK_MS: TICK_MS,
    start: start, tick: tick, sync: sync, noteTx: noteTx,
    state: function () { return s.state; },
    status: function () { return s.status; }
  };
})();
