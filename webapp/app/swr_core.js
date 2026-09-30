// The SWR sweep's logic, without the page (RFC 0010): which ranges to sweep, how long that takes,
// what the host's events add up to, and where every mark on the plot goes. swr.html draws what
// plotModel() returns; webapp/tests/test_swr_core.js runs this file under node.
//
// Needs bands.js first (BANDS, BAND_EDGES_KHZ). Plain global, like callinfo.js: no module system.
var SwrCore = (function () {
  'use strict';

  // These three are the host's limits and cost, in components/swr_sweep/include/swr_sweep.h; the
  // test reads that header and fails if they drift apart.
  var POINT_MS = 800;
  var MAX_POINTS = 200;
  var MAX_RANGES = 12;

  var SWR_LOW = 1;
  var SWR_HIGH = 6;
  var GOOD_SWR = 2;

  // Step between points, kHz, chosen so a band reads in about 10 to 20 points (the bench tuned these
  // on 40 m to 10 m). 2 m is left out: its edges do not fit the wire's 16-bit kHz, and no QMX has it.
  var STEP_KHZ = { 160: 20, 80: 25, 60: 10, 40: 20, 30: 10, 20: 25, 17: 10, 15: 30, 12: 10, 10: 100, 6: 250 };

  // What the host's ABORTED event says, by its `why` value (porta_sweep_reason_t).
  var ABORT_TEXT = {
    0: 'Stopped.',
    1: 'Lost contact with the radio.',
    2: 'The station stopped responding and the radio was un-keyed.'
  };

  function sweepableBands() {
    var out = [];
    BANDS.forEach(function (b) {
      var edges = BAND_EDGES_KHZ[b.n];
      if (edges && STEP_KHZ[b.n]) out.push({ n: b.n, name: b.name, lo: edges[0], hi: edges[1], step: STEP_KHZ[b.n] });
    });
    return out;
  }

  // The bands to tick when the page opens: the ones enabled in Settings.
  function defaultBands(activeBandsText) {
    var parsed = parseActiveBands(activeBandsText);
    return sweepableBands().filter(function (b) { return !parsed.any || parsed.want[b.n]; }).map(function (b) { return b.n; });
  }

  function rangePoints(lo, hi, step) {
    var pts = [];
    for (var f = lo; f < hi; f += step) pts.push(f);
    pts.push(hi);
    return pts;
  }

  // ns: band numbers to sweep. fine: halve every step. Returns the ranges in BANDS order with each
  // point's frequency in Hz, the total, the estimate, and `error` (null, or why it cannot run).
  function planSweep(ns, fine) {
    var want = {};
    (ns || []).forEach(function (n) { want[n] = true; });
    var ranges = [];
    var total = 0;
    sweepableBands().forEach(function (b) {
      if (!want[b.n]) return;
      var step = fine ? Math.max(1, Math.round(b.step / 2)) : b.step;
      var pts = rangePoints(b.lo, b.hi, step).map(function (khz) { return khz * 1000; });
      ranges.push({ n: b.n, name: b.name, lo: b.lo, hi: b.hi, step: step, points: pts });
      total += pts.length;
    });
    var error = null;
    if (!ranges.length) error = 'Tick at least one band.';
    else if (ranges.length > MAX_RANGES) error = 'Too many bands.';
    else if (total > MAX_POINTS) error = 'That is ' + total + ' points; the limit is ' + MAX_POINTS + '. Untick a band' + (fine ? ' or turn Fine off.' : '.');
    return { ranges: ranges, totalPoints: total, estimateMs: total * POINT_MS, error: error };
  }

  // The body of POST /api/swr/start.
  function requestBody(plan) {
    return plan.ranges.map(function (r) { return r.lo + '-' + r.hi + '-' + r.step; }).join(',');
  }

  function fmtDuration(ms) {
    var s = Math.max(0, Math.round(ms / 1000));
    var m = Math.floor(s / 60);
    var r = s % 60;
    return m + ':' + (r < 10 ? '0' : '') + r;
  }

  function newState(plan, startedAt) {
    return { phase: 'running', plan: plan, readings: {}, skipped: {}, startedAt: startedAt, endedAt: null, reason: null };
  }

  // One event row from /api/events; only the sweep's (d == 6) change anything.
  function applyEvent(state, ev, nowMs) {
    if (!state || !ev || ev.d !== 6 || state.phase !== 'running') return state;
    switch (ev.k) {
      case 0:
        state.readings[ev.hz] = ev.swr;
        break;
      case 1:
        state.skipped[ev.r] = true;
        break;
      case 2:
        state.phase = 'done';
        state.endedAt = nowMs;
        break;
      case 3:
        state.phase = 'aborted';
        state.endedAt = nowMs;
        state.reason = ABORT_TEXT[ev.why] || 'Stopped.';
        break;
    }
    return state;
  }

  // Ends a run that the host never reported the end of (a missed DONE or ABORTED on a poll that slept,
  // or a Stop whose answer never came back), so the page does not count down for a sweep that is over.
  function abortLocally(state, nowMs, text) {
    if (!state || state.phase !== 'running') return state;
    state.phase = 'aborted';
    state.endedAt = nowMs;
    state.reason = text;
    return state;
  }

  function measuredCount(state) {
    return Object.keys(state.readings).length;
  }

  // Milliseconds left, from the points still to do and what they have cost so far (the estimate
  // until two have arrived). A band the radio does not have costs nothing, so its points leave the count.
  function remainingMs(state, nowMs) {
    if (!state || state.phase !== 'running') return 0;
    var skippedPoints = 0;
    state.plan.ranges.forEach(function (r, i) { if (state.skipped[i]) skippedPoints += r.points.length; });
    var done = measuredCount(state);
    var left = Math.max(0, state.plan.totalPoints - skippedPoints - done);
    var per = POINT_MS;
    if (done >= 2) per = Math.max(1, (nowMs - state.startedAt) / done);
    return left * per;
  }

  // One row per swept band for the list under the plot: what an antenna tuner needs.
  function summary(state) {
    var rows = [];
    state.plan.ranges.forEach(function (r, i) {
      if (state.skipped[i]) {
        rows.push({ n: r.n, name: r.name, swept: false });
        return;
      }
      var vals = [];
      r.points.forEach(function (hz) { if (state.readings[hz] !== undefined) vals.push({ hz: hz, swr: state.readings[hz] / 100 }); });
      if (!vals.length) {
        rows.push({ n: r.n, name: r.name, swept: state.phase === 'running' ? null : false });
        return;
      }
      var lo = vals.reduce(function (a, b) { return b.swr < a.swr ? b : a; });
      var ok = vals.filter(function (v) { return v.swr <= GOOD_SWR; }).length;
      rows.push({ n: r.n, name: r.name, swept: true, points: vals.length, planned: r.points.length,
                  minSwr: lo.swr, minHz: lo.hz, goodShare: ok / vals.length });
    });
    return rows;
  }

  // Everything the page draws, in pixels. `prev` is an earlier run's saved readings (saveable()), drawn
  // faintly wherever their frequencies fall in a band of this plan. Bands sit side by side in BANDS
  // order, each as wide as its frequency span with a floor, so a 50 kHz band is not a sliver.
  function plotModel(state, width, height, prev) {
    var margin = { left: 40, right: 10, top: 22, bottom: 36 };
    var innerW = Math.max(40, width - margin.left - margin.right);
    var innerH = Math.max(40, height - margin.top - margin.bottom);
    var ranges = state.plan.ranges;
    var spanSum = ranges.reduce(function (a, r) { return a + (r.hi - r.lo); }, 0);
    var floor = Math.max(1, spanSum * 0.08);
    var weights = ranges.map(function (r) { return Math.max(r.hi - r.lo, floor); });
    var weightSum = weights.reduce(function (a, b) { return a + b; }, 0);

    function yOf(swr) {
      var c = Math.min(Math.max(swr, SWR_LOW), SWR_HIGH);
      return margin.top + (SWR_HIGH - c) / (SWR_HIGH - SWR_LOW) * innerH;
    }

    var x = margin.left;
    var bands = [];
    ranges.forEach(function (r, i) {
      var w = weights[i] / weightSum * innerW;
      bands.push({ index: i, n: r.n, name: r.name, lo: r.lo, hi: r.hi, x0: x, x1: x + w, skipped: !!state.skipped[i] });
      x += w;
    });

    function xOf(b, hz) {
      var khz = hz / 1000;
      return b.x0 + (khz - b.lo) / Math.max(1, b.hi - b.lo) * (b.x1 - b.x0);
    }

    var dots = [];
    var lines = [];
    var marks = [];
    bands.forEach(function (b) {
      if (b.skipped) return;
      var run = [];
      var best = null;
      ranges[b.index].points.forEach(function (hz) {
        var raw = state.readings[hz];
        if (raw === undefined) {
          if (run.length > 1) lines.push(run);
          run = [];
          return;
        }
        var swr = raw / 100;
        var dot = { x: xOf(b, hz), y: yOf(swr), hz: hz, swr: swr, over: swr > SWR_HIGH, good: swr <= GOOD_SWR, band: b.n };
        dots.push(dot);
        run.push({ x: dot.x, y: dot.y });
        if (!best || swr < best.swr) best = dot;
      });
      if (run.length > 1) lines.push(run);
      if (best) marks.push({ x: best.x, y: best.y, hz: best.hz, swr: best.swr, band: b.n });
    });

    var prevDots = [];
    if (prev && prev.readings) {
      Object.keys(prev.readings).forEach(function (key) {
        var hz = Number(key);
        var khz = hz / 1000;
        for (var i = 0; i < bands.length; i++) {
          var b = bands[i];
          if (!b.skipped && khz >= b.lo && khz <= b.hi) {
            prevDots.push({ x: xOf(b, hz), y: yOf(prev.readings[key] / 100), hz: hz, swr: prev.readings[key] / 100 });
            break;
          }
        }
      });
    }

    var yTicks = [];
    for (var v = SWR_LOW; v <= SWR_HIGH; v++) yTicks.push({ value: v, y: yOf(v), good: v === GOOD_SWR });
    var boundaries = bands.map(function (b) { return b.x0; });
    if (bands.length) boundaries.push(bands[bands.length - 1].x1);

    return { width: width, height: height, margin: margin, plot: { x0: margin.left, x1: margin.left + innerW, y0: margin.top, y1: margin.top + innerH },
             bands: bands, boundaries: boundaries, yTicks: yTicks, dots: dots, lines: lines, marks: marks, prevDots: prevDots };
  }

  // What is kept on the phone to draw behind the next run: readings only, never the record.
  function saveable(state) {
    return { at: state.endedAt || state.startedAt, readings: state.readings };
  }

  return {
    POINT_MS: POINT_MS, MAX_POINTS: MAX_POINTS, MAX_RANGES: MAX_RANGES, STEP_KHZ: STEP_KHZ,
    SWR_LOW: SWR_LOW, SWR_HIGH: SWR_HIGH, GOOD_SWR: GOOD_SWR,
    sweepableBands: sweepableBands, defaultBands: defaultBands, planSweep: planSweep, requestBody: requestBody,
    newState: newState, applyEvent: applyEvent, abortLocally: abortLocally, remainingMs: remainingMs, measuredCount: measuredCount,
    summary: summary, plotModel: plotModel, saveable: saveable, fmtDuration: fmtDuration
  };
})();
