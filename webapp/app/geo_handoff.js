'use strict';
// The geolocation hand-off, shared by app.html's Set from GPS and map.html's
// locate button (docs/architecture/webapp.md, A non-secure origin): this
// origin cannot call navigator.geolocation, so the page navigates to geo.html
// on the HTTPS release host, which reads the position and navigates back
// with ?lat=&lon=. "/geo", not "/geo.html": Static Assets 307s the .html form
// before webapp/worker.js's cache override applies.
//
// Leaving the page is the step that needs internet, and on the sidekick's AP
// the phone often has none (docs/architecture/system.md, Connectivity). A
// navigation that cannot load leaves Safari spinning or on its own error
// page, with nothing of ours left running to say why (field-found
// 2026-10-09). So the page leaves only after a short fetch shows the host is
// reachable. no-store, because a cached answer says nothing about now.
// When geo.html works offline (row rrsp, a service worker on the release
// host), this check would refuse a hand-off that works: that row replaces it.
var MicroFT8Geo = (function () {
  var URL_ = 'https://micro-ft8.kalikstein.com/geo';
  var PROBE_MS = 4000;
  var probing = false;

  // say(text) puts a status line where the caller's control is; offlineText
  // is what it says when the host cannot be reached.
  function handoff(returnUrl, say, offlineText) {
    if (probing) return;
    probing = true;
    say('Checking for internet…');
    var ctl = new AbortController();
    var t = setTimeout(function () { ctl.abort(); }, PROBE_MS);
    fetch(URL_, { mode: 'no-cors', cache: 'no-store', signal: ctl.signal }).then(function () {
      clearTimeout(t);
      probing = false;
      say('Opening the location page…');
      location.href = URL_ + '?return=' + encodeURIComponent(returnUrl);
    }, function () {
      clearTimeout(t);
      probing = false;
      say(offlineText);
    });
  }

  return { handoff: handoff };
})();
