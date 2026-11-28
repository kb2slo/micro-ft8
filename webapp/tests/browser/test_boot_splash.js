#!/usr/bin/env node
// The loading screen (docs/architecture/webapp.md, Home Screen), in browsers that say they are a Home Screen app
// on a simulated station whose sidekick is slow to send one of the app's scripts: the screen covers the page
// from its first bytes, in the theme the browser chose, and goes once the app has started; a load that has not
// finished in BOOT_SLOW_MS offers Retry, which loads the page again. A browser tab shows no loading screen.
'use strict';
const { run, check, until, sleep } = require('./harness');

const STANDALONE = "Object.defineProperty(Navigator.prototype, 'standalone', { get: () => true });";

// Holds updates.js, one of the scripts the app waits for, for `ms` each load while hold.ms is set.
function slowScript(hold) {
  return (ctx) => ctx.route('**/app/updates.js', async (r) => {
    if (hold.ms) await sleep(hold.ms);
    return r.continue();
  });
}

const splashShown = (p) => p.evaluate(() => {
  const el = document.getElementById('bootSplash');
  return !!el && getComputedStyle(el).display !== 'none';
});
const booted = (p) => p.evaluate(() => typeof lastCfg === 'object' && lastCfg.call !== undefined).catch(() => false);
const slowShown = (p) => p.evaluate(() => {
  const el = document.getElementById('bootSlow');
  return !!el && !el.hidden;
});

run(async (st) => {
  // A slow load from the Home Screen, with Dark chosen on a phone that is light.
  const hold = { ms: 3000 };
  const A = await st.browser('A', {
    wait: false, route: slowScript(hold),
    init: STANDALONE + " try { localStorage.setItem('microft8_theme_v1', 'dark'); } catch (e) {}",
  });
  check('from the Home Screen, the loading screen covers the page while the app loads',
    !!(await until(() => splashShown(A), 2000)));
  check('in the theme chosen, before the app\'s script has run',
    await A.evaluate(() => document.documentElement.getAttribute('data-theme') === 'dark' && typeof lastCfg === 'undefined'));
  check('it offers no Retry yet', !(await slowShown(A)));
  check('the app starts', !!(await until(() => booted(A), 15000)));
  check('and the loading screen is gone',
    await A.evaluate(() => document.getElementById('bootSplash') === null));

  // A load slower than BOOT_SLOW_MS: Retry, which loads the page again.
  const hold2 = { ms: 11000 };
  const B = await st.browser('B', { wait: false, route: slowScript(hold2), init: STANDALONE });
  const slowMs = await B.evaluate(() => BOOT_SLOW_MS).catch(() => 8000);
  check('a load that has not finished offers Retry', !!(await until(() => slowShown(B), slowMs + 2000)));
  check('still on the loading screen', await splashShown(B));
  hold2.ms = 0;
  await B.click('#bootRetry');
  check('Retry loads the page again, and the app starts', !!(await until(() => booted(B), 15000)));
  check('with no loading screen left', await B.evaluate(() => document.getElementById('bootSplash') === null));

  // A browser tab: its own progress bar, no loading screen.
  const hold3 = { ms: 3000 };
  const C = await st.browser('C', { wait: false, route: slowScript(hold3) });
  await until(() => C.evaluate(() => !!document.getElementById('bootSplash')).catch(() => false), 2000);
  check('a browser tab shows no loading screen', !(await splashShown(C)));
  check('and the app starts there too', !!(await until(() => booted(C), 15000)));
});
