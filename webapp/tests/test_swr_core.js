#!/usr/bin/env node
// swr_core.js (RFC 0010): the sweep plan, the event reducer, the countdown and the plot's geometry,
// run under node without a page. bands.js is loaded first, as swr.html does.
'use strict';
const vm = require('vm');
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const app = path.join(__dirname, '..', 'app');
const root = path.join(__dirname, '..', '..');
const ctx = vm.createContext({});
vm.runInContext(fs.readFileSync(path.join(app, 'bands.js'), 'utf8'), ctx);
vm.runInContext(fs.readFileSync(path.join(app, 'swr_core.js'), 'utf8'), ctx);
const S = vm.runInContext('SwrCore', ctx);

let failed = false;
function check(ok, msg) {
  if (!ok) { console.error('FAIL: ' + msg); failed = true; }
}
function same(a, b, msg) {
  check(JSON.stringify(a) === JSON.stringify(b), msg + ' (got ' + JSON.stringify(a) + ', want ' + JSON.stringify(b) + ')');
}

// The host's numbers, read from its own header: a JS copy that drifts would show the wrong estimate
// or let through a request the host refuses.
const header = fs.readFileSync(path.join(root, 'components', 'swr_sweep', 'include', 'swr_sweep.h'), 'utf8');
const define = (name) => Number(header.match(new RegExp('#define ' + name + ' (\\d+)'))[1]);
check(S.POINT_MS === define('SWR_SWEEP_POINT_MS'), 'POINT_MS equals SWR_SWEEP_POINT_MS');
check(S.MAX_POINTS === define('SWR_SWEEP_MAX_POINTS'), 'MAX_POINTS equals SWR_SWEEP_MAX_POINTS');
check(S.MAX_RANGES === define('SWR_SWEEP_MAX_RANGES'), 'MAX_RANGES equals SWR_SWEEP_MAX_RANGES');

// Bands.
const all = S.sweepableBands();
same(all.map((b) => b.n), [160, 80, 60, 40, 30, 20, 17, 15, 12, 10, 6], 'every band a QMX could have, and not 2 m');
check(all.every((b) => b.hi <= 65535), 'every edge fits the wire\'s 16-bit kHz');
same(S.defaultBands('40 30 20 17 15 12 10'), [40, 30, 20, 17, 15, 12, 10], 'the ticked bands follow Settings');
same(S.defaultBands(''), all.map((b) => b.n), 'no Settings filter means every band');
same(S.defaultBands('40 2'), [40], 'a band that cannot sweep is not offered');

// Plans.
let plan = S.planSweep([20], false);
check(plan.error === null && plan.totalPoints === 15, '20 m is 15 points');
same(plan.ranges[0].points.slice(0, 2), [14000000, 14025000], '20 m starts at its lower edge in Hz');
check(plan.ranges[0].points[14] === 14350000, 'and ends on its upper edge');
check(plan.estimateMs === 15 * 800, 'the estimate is points times the host\'s cost per point');
check(S.requestBody(S.planSweep([40, 20], false)) === '7000-7300-20,14000-14350-25', 'the request body is lo-hi-step per band, in band order');
check(S.requestBody(S.planSweep([20, 40], false)) === '7000-7300-20,14000-14350-25', 'whatever order the boxes were ticked in');
plan = S.planSweep([20], true);
check(plan.totalPoints > 25 && plan.ranges[0].step < 25, 'Fine takes smaller steps, so more points');
check(S.planSweep([], false).error !== null, 'no band ticked cannot run');
const everything = S.planSweep(all.map((b) => b.n), true);
check(everything.error && /limit/.test(everything.error), 'more points than the host allows is refused before sending');
check(S.planSweep(all.map((b) => b.n), false).totalPoints <= 200, 'every band at the default density fits the host\'s cap');
check(S.fmtDuration(65000) === '1:05' && S.fmtDuration(0) === '0:00', 'durations read m:ss');

// The reducer.
plan = S.planSweep([40, 10], false);
let st = S.newState(plan, 1000);
S.applyEvent(st, { d: 5, k: 0, hz: 7000000, swr: 100 }, 0);
check(S.measuredCount(st) === 0, 'an event of another kind changes nothing');
S.applyEvent(st, { d: 6, k: 0, r: 0, hz: 7000000, swr: 131 }, 0);
check(st.readings[7000000] === 131, 'a point is stored by frequency');
S.applyEvent(st, { d: 6, k: 1, r: 1 }, 0);
check(st.skipped[1] === true, 'a skipped band is noted');
S.applyEvent(st, { d: 6, k: 2 }, 5000);
check(st.phase === 'done' && st.endedAt === 5000, 'DONE ends the run');
S.applyEvent(st, { d: 6, k: 0, r: 0, hz: 7020000, swr: 120 }, 6000);
check(st.readings[7020000] === undefined, 'nothing is added after the end');
st = S.newState(plan, 0);
S.applyEvent(st, { d: 6, k: 3, why: 1 }, 100);
check(st.phase === 'aborted' && /radio/.test(st.reason), 'ABORTED says why');

// A run the host never reported the end of is ended on the page, so it does not count down for ever.
st = S.newState(S.planSweep([20], false), 0);
S.applyEvent(st, { d: 6, k: 0, r: 0, hz: 14000000, swr: 120 }, 0);
S.abortLocally(st, 9000, 'Stopped.');
check(st.phase === 'aborted' && st.reason === 'Stopped.' && st.endedAt === 9000, 'abortLocally ends a running run with its reason');
check(S.remainingMs(st, 9500) === 0, 'and the countdown reads zero');
S.abortLocally(st, 9999, 'later');
check(st.reason === 'Stopped.', 'a run already ended is left alone');

// The countdown.
st = S.newState(S.planSweep([20], false), 0);
check(S.remainingMs(st, 0) === 15 * 800, 'it starts at the estimate');
for (let i = 0; i < 4; i++) S.applyEvent(st, { d: 6, k: 0, r: 0, hz: 14000000 + i * 25000, swr: 120 }, 0);
check(S.remainingMs(st, 2000) === 11 * 500, 'then uses the rate measured so far (4 points in 2 s leaves 11 at 0.5 s)');
st = S.newState(S.planSweep([80, 20], false), 0);
const before = S.remainingMs(st, 0);
S.applyEvent(st, { d: 6, k: 1, r: 0 }, 0);
check(S.remainingMs(st, 0) === before - st.plan.ranges[0].points.length * 800, 'a band the radio lacks leaves the countdown');
S.applyEvent(st, { d: 6, k: 2 }, 0);
check(S.remainingMs(st, 0) === 0, 'and it reads zero once the run has ended');

// The plot.
plan = S.planSweep([30, 10], false);
st = S.newState(plan, 0);
plan.ranges[1].points.forEach((hz, i) => S.applyEvent(st, { d: 6, k: 0, r: 1, hz: hz, swr: i === 3 ? 998 : 150 }, 0));
const m = S.plotModel(st, 800, 400, null);
check(m.bands.length === 2 && m.bands[0].name === '30m' && m.bands[1].name === '10m', 'bands sit in band order');
check(m.boundaries.length === 3 && m.boundaries[0] === m.plot.x0 && m.boundaries[2] === m.plot.x1, 'a vertical line at every band boundary');
check(m.bands[0].x1 === m.bands[1].x0, 'and the bands touch');
const w30 = (m.bands[0].x1 - m.bands[0].x0) / (m.plot.x1 - m.plot.x0);
check(w30 >= 0.05, 'a 50 kHz band is not a sliver next to a 1.7 MHz one (got ' + w30.toFixed(3) + ')');
check(m.dots.every((d) => d.band === 10), 'no dot in the band with no readings');
const over = m.dots.find((d) => d.swr > 6);
check(over && over.over && over.y === m.plot.y0, 'a reading above 6 sits on the top edge and is marked over-range');
check(m.dots.find((d) => d.swr === 1.5).y > m.plot.y0 && m.dots.find((d) => d.swr === 1.5).y < m.plot.y1, 'a 1.5 sits inside the plot');
same(m.yTicks.map((t) => t.value), [1, 2, 3, 4, 5, 6], 'the vertical axis runs 1 to 6');
check(Math.abs(m.yTicks[0].y - m.plot.y1) < 1e-9 && Math.abs(m.yTicks[5].y - m.plot.y0) < 1e-9, 'with 1 at the bottom and 6 at the top');

// A missing measurement is a gap, not a point and not a bridge.
plan = S.planSweep([20], false);
st = S.newState(plan, 0);
plan.ranges[0].points.forEach((hz, i) => { if (i !== 5 && i !== 6) S.applyEvent(st, { d: 6, k: 0, r: 0, hz: hz, swr: 140 }, 0); });
let g = S.plotModel(st, 800, 400, null);
check(g.dots.length === 13, 'no dot where there is no measurement (15 planned, 2 missing)');
check(g.lines.length === 2, 'and the line breaks across the gap (two runs)');
check(g.lines[0].length === 5 && g.lines[1].length === 8, 'of 5 and 8 points');
check(g.marks.length === 1, 'one minimum is marked for the band');

// A band the radio lacks has no dots or line, only its label and its flag.
plan = S.planSweep([40, 20], false);
st = S.newState(plan, 0);
S.applyEvent(st, { d: 6, k: 1, r: 0 }, 0);
plan.ranges[1].points.forEach((hz) => S.applyEvent(st, { d: 6, k: 0, r: 1, hz: hz, swr: 130 }, 0));
g = S.plotModel(st, 800, 400, null);
check(g.bands[0].skipped && !g.bands[1].skipped, 'a skipped band is flagged for the "not swept" label');
check(g.dots.every((d) => d.band === 20), 'and gets no points');

// Rotating the phone re-lays out the same readings; nothing is lost or re-run.
const landscape = S.plotModel(st, 800, 300, null);
const portrait = S.plotModel(st, 360, 400, null);
check(landscape.dots.length === portrait.dots.length && landscape.lines.length === portrait.lines.length, 'portrait and landscape draw the same readings');
check(landscape.dots[0].x !== portrait.dots[0].x, 'at different positions');
check(JSON.stringify(landscape.dots.map((d) => d.hz)) === JSON.stringify(portrait.dots.map((d) => d.hz)), 'in the same order');

// The previous run, faint behind this one, only where this plan has a band.
const prev = { at: 1, readings: { 14100000: 180, 7100000: 150, 3700000: 300 } };
g = S.plotModel(st, 800, 400, prev);
same(g.prevDots.map((d) => d.hz), [14100000], 'the previous run shows only inside the bands now swept (40 m was skipped, 80 m not in the plan)');

// The list under the plot.
const rows = S.summary(st);
check(rows.length === 2 && rows[0].swept === false, 'a skipped band reads as not swept');
check(rows[1].swept === true && rows[1].minSwr === 1.3 && rows[1].goodShare === 1, 'a swept band gives its minimum and the share at or below 2');
const sav = S.saveable(st);
check(sav.readings[14100000] === 130 && Object.keys(sav).length === 2, 'what is kept is readings and a time');

if (failed) process.exit(1);
console.log('All swr_core checks passed');
