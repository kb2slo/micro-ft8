// The four-band experiment's page: reads runs/index.json, then one run's runs/<id>/run.json
// (built by tools/decode_bench/build_four_band_run.py), and draws it with ../charts.js.
// docs/architecture/webapp.md, Experiments.
(function () {
  "use strict";
  var C = Charts;
  var SERIES = { "10m": "var(--s1)", "15m": "var(--s2)", "20m": "var(--s3)", "40m": "var(--s4)" };
  var LAND_URL = "/app/ne_110m_land.json"; // the web app's bundled Natural Earth 110m land
  var MONTHS = ["January", "February", "March", "April", "May", "June", "July", "August",
                "September", "October", "November", "December"];
  var run = null, land = null, mapBand = "all", tz = "CST";

  function $(id) { return document.getElementById(id); }
  function color(b) { return SERIES[b] || "var(--muted)"; }
  function bandLabel(b) { return b.replace("m", " m"); }
  function fmt(n) { return Number(n).toLocaleString("en-US"); }
  function fmt1(n) { return (Math.round(n * 10) / 10).toFixed(1); }
  function signed(n) { return (n > 0 ? "+" : n < 0 ? "−" : "") + Math.abs(n); }
  function longDate(iso) { var p = iso.split("-"); return Number(p[2]) + " " + MONTHS[Number(p[1]) - 1] + " " + p[0]; }
  // Local minutes since midnight -> "8:17 AM" (minutes past 1440 wrap to the next day).
  function clock(m) {
    m = ((Math.round(m) % 1440) + 1440) % 1440;
    var h = Math.floor(m / 60), mm = m % 60;
    return (h % 12 || 12) + ":" + (mm < 10 ? "0" : "") + mm + " " + (h < 12 ? "AM" : "PM");
  }
  function hourLabel(h) { h = h % 24; return (h % 12 || 12) + " " + (h < 12 ? "AM" : "PM"); }
  function hhmmToMin(s) { var p = s.split(":"); return Number(p[0]) * 60 + Number(p[1]); }
  function getJSON(url) {
    return fetch(url).then(function (r) { if (!r.ok) throw new Error(url + ": HTTP " + r.status); return r.json(); });
  }
  function clear(id) { var n = $(id); n.replaceChildren(); return n; }

  // ---- Headline -------------------------------------------------------------------------------
  function drawHeadline() {
    var t = run.totals, cov = run.covers;
    var busiest = run.per_band.slice().sort(function (a, b) { return b.decodes_per_slot - a.decodes_per_slot; })[0];
    var take = $("take");
    take.replaceChildren();
    function add(s, bold) {
      var n = bold ? document.createElement("b") : document.createTextNode(s);
      if (bold) n.textContent = s;
      take.appendChild(n);
    }
    add("On " + longDate(run.date) + ", from " + clock(hhmmToMin(cov.first_local)) + " to " +
        clock(hhmmToMin(cov.end_local)) + " " + tz + ", ");
    add("jt9 heard ");
    add(fmt(t.stations), true);
    add(" stations in ");
    add(fmt(t.grid_squares), true);
    add(" grid squares on four bands at once, " + bandLabel(busiest.band) + " the busiest at ");
    add(fmt1(busiest.decodes_per_slot), true);
    add(" decodes a slot, with ");
    add(fmt(t.lost_packets), true);
    add(" packets lost in ");
    add(fmt(t.slots_recorded), true);
    add(" slots recorded.");

    var tiles = [["Stations heard", fmt(t.stations)],
                 ["Messages from an unknown or other sender", fmt(t.unknown_sender_messages)],
                 ["Grid squares", fmt(t.grid_squares)],
                 ["Decodes in the sampled slots", fmt(t.decodes)], ["Slots recorded, each band", fmt(t.slots_recorded)],
                 ["Packets lost", fmt(t.lost_packets)]];
    var ul = clear("tiles");
    tiles.forEach(function (x) {
      var li = document.createElement("li"), s = document.createElement("span"), b = document.createElement("b");
      s.textContent = x[0];
      b.textContent = x[1];
      li.append(s, b);
      ul.appendChild(li);
    });
  }

  // ---- Heatmap: band x hour -------------------------------------------------------------------
  function drawHeat() {
    var wrap = clear("heat");
    var rows = run.heat, bands = run.bands;
    var hours = [];
    var h0 = Math.min.apply(null, rows.map(function (r) { return r.hour; }));
    var h1 = Math.max.apply(null, rows.map(function (r) { return r.hour; }));
    for (var h = h0; h <= h1; h++) hours.push(h);
    var m = { t: 4, r: 2, b: 22, l: 40 }, rowH = 34;
    var ch = C.svg(wrap, m.t + rowH * bands.length + m.b,
      "Heatmap of jt9 decodes per slot by band and hour; use the arrow keys to read each cell.");
    var cw = (ch.w - m.l - m.r) / hours.length;
    var max = Math.max.apply(null, rows.map(function (r) { return r.mean; })) || 1;
    var cells = [];
    // Where the cell's number turns from ink to paper: the ramp's mid-tones are darker in light.
    var flip = Number(getComputedStyle(document.documentElement).getPropertyValue("--heat-flip")) || 55;
    // One hue light to dark (--heat-0 to --heat-1), mixed in CSS so it follows the theme.
    rows.forEach(function (r) {
      var x = m.l + (r.hour - h0) * cw, y = m.t + bands.indexOf(r.band) * rowH, f = r.mean / max;
      var pct = Math.round(8 + 92 * f);
      C.el("rect", { x: x + 1, y: y + 1, width: Math.max(1, cw - 2), height: rowH - 2, rx: 3,
                     style: "fill: color-mix(in oklab, var(--heat-1) " + pct + "%, var(--heat-0))" }, ch.node);
      if (cw >= 22) {
        C.text(ch.node, x + cw / 2, y + rowH / 2, String(Math.round(r.mean)),
          { "text-anchor": "middle", dy: "0.35em", class: "chart-text strong",
            style: "fill:" + (pct > flip ? "var(--paper)" : "var(--ink)") });
      }
      cells.push({ x: x + cw / 2, y: y + rowH / 2, r: r });
    });
    bands.forEach(function (b, i) {
      C.text(ch.node, m.l - 6, m.t + i * rowH + rowH / 2, bandLabel(b), { "text-anchor": "end", dy: "0.35em", class: "chart-text ink" });
    });
    var step = Math.ceil(46 / cw);
    hours.forEach(function (hh, i) {
      if (i % step) return;
      C.text(ch.node, m.l + i * cw + cw / 2, ch.h - 6, hourLabel(hh), { "text-anchor": "middle" });
    });
    C.cursor({
      chart: ch, wrap: wrap, count: cells.length,
      at: function (i) { return { x: cells[i].x, y: cells[i].y - rowH / 2 + 4, r: 0 }; },
      tip: function (i) {
        var r = cells[i].r;
        return { head: bandLabel(r.band) + ", " + hourLabel(r.hour) + " " + tz,
                 lines: [[color(r.band), fmt1(r.mean) + " decodes a slot (" + r.slots + " slots sampled)"]] };
      },
      pick: function (x, y) {
        var best = -1, bd = Infinity;
        cells.forEach(function (c, i) {
          if (Math.abs(c.x - x) <= cw / 2 && Math.abs(c.y - y) <= rowH / 2) { var d = Math.abs(c.x - x); if (d < bd) { bd = d; best = i; } }
        });
        return best;
      },
    });
  }

  // ---- Line through the day -------------------------------------------------------------------
  function legendBands() {
    Array.prototype.forEach.call(document.querySelectorAll('[data-legend="bands"]'), function (ul) {
      ul.replaceChildren();
      run.bands.forEach(function (b) {
        var li = document.createElement("li"), i = document.createElement("i");
        i.style.background = color(b);
        li.append(i, document.createTextNode(bandLabel(b)));
        ul.appendChild(li);
      });
    });
  }

  function timeAxis(ch, x, lo, hi, m, y) {
    var span = hi - lo, want = Math.max(2, Math.floor((ch.w - m.l - m.r) / 70));
    var stepH = [1, 2, 3, 4, 6].filter(function (s) { return span / 60 / s <= want; })[0] || 6;
    for (var t = Math.ceil(lo / 60 / stepH) * stepH * 60; t <= hi; t += stepH * 60) {
      C.el("line", { class: "chart-axis", x1: x(t), x2: x(t), y1: y, y2: y + 4 }, ch.node);
      C.text(ch.node, x(t), y + 15, hourLabel(t / 60), { "text-anchor": "middle" });
    }
  }

  function drawTrend() {
    var wrap = clear("trend");
    var m = { t: 10, r: 40, b: 24, l: 32 };
    var ch = C.svg(wrap, 230, "Line chart of decodes per slot through the day, one line per band; use the arrow keys to step through the times.");
    var all = [], times = {};
    run.bands.forEach(function (b) {
      (run.trend.bands[b] || []).forEach(function (p) { all.push(p[1]); times[p[0]] = 1; });
    });
    var ts = Object.keys(times).map(Number).sort(function (a, b) { return a - b; });
    var lo = ts[0], hi = ts[ts.length - 1] + run.trend.bin_min;
    var ymax = Math.max.apply(null, all) * 1.08;
    var yt = C.ticks(0, ymax, 4);
    ymax = Math.max(ymax, yt[yt.length - 1]);
    var x = C.scale(lo, hi, m.l, ch.w - m.r), y = C.scale(0, ymax, ch.h - m.b, m.t);
    yt.forEach(function (v) {
      C.el("line", { class: "chart-grid", x1: m.l, x2: ch.w - m.r, y1: y(v), y2: y(v) }, ch.node);
      C.text(ch.node, m.l - 6, y(v), String(v), { "text-anchor": "end", dy: "0.35em" });
    });
    timeAxis(ch, x, lo, hi, m, ch.h - m.b);
    var half = run.trend.bin_min / 2, ends = [];
    run.bands.forEach(function (b) {
      var pts = run.trend.bands[b] || [];
      if (!pts.length) return;
      var d = "", prev = null;
      pts.forEach(function (p) {
        d += (prev !== null && p[0] - prev > run.trend.bin_min ? "M" : d ? "L" : "M") + x(p[0] + half).toFixed(1) + "," + y(p[1]).toFixed(1);
        prev = p[0];
      });
      C.el("path", { d: d, fill: "none", "stroke-width": 2, "stroke-linejoin": "round", "stroke-linecap": "round",
                     style: "stroke:" + color(b) }, ch.node);
      var last = pts[pts.length - 1];
      ends.push({ b: b, x: x(last[0] + half), y: y(last[1]) });
    });
    ends.sort(function (a, b) { return a.y - b.y; });
    for (var i = 1; i < ends.length; i++) ends[i].y = Math.max(ends[i].y, ends[i - 1].y + 12);
    ends.forEach(function (e) { C.text(ch.node, e.x + 5, e.y, bandLabel(e.b), { dy: "0.35em", class: "chart-text ink" }); });
    var xs = ts.map(function (t) { return x(t + half); });
    C.cursor({
      chart: ch, wrap: wrap, count: ts.length, rule: [m.t, ch.h - m.b],
      at: function (k) { return { x: xs[k], y: m.t + 20 }; },
      tip: function (k) {
        var t = ts[k];
        return { head: clock(t) + "–" + clock(t + run.trend.bin_min) + " " + tz,
                 lines: run.bands.map(function (b) {
                   var p = (run.trend.bands[b] || []).filter(function (q) { return q[0] === t; })[0];
                   return [color(b), bandLabel(b) + ": " + (p ? fmt1(p[1]) + " a slot" : "no slot sampled")];
                 }) };
      },
      pick: function (px) { return C.nearest(xs, px); },
    });
  }

  // ---- Map ------------------------------------------------------------------------------------
  function gridCentre(g) {
    return [(g.charCodeAt(0) - 65) * 20 - 180 + Number(g[2]) * 2 + 1, (g.charCodeAt(1) - 65) * 10 - 90 + Number(g[3]) + 0.5];
  }

  function mapRows() {
    if (mapBand !== "all") {
      return run.grids.filter(function (g) { return g[1] === mapBand; })
        .map(function (g) { return { grid: g[0], spots: g[2], best: g[3], by: [[g[1], g[2]]] }; });
    }
    var acc = {};
    run.grids.forEach(function (g) {
      var e = acc[g[0]] || (acc[g[0]] = { grid: g[0], spots: 0, best: -99, by: [] });
      e.spots += g[2];
      e.best = Math.max(e.best, g[3]);
      e.by.push([g[1], g[2]]);
    });
    return Object.keys(acc).map(function (k) { return acc[k]; }).sort(function (a, b) { return b.spots - a.spots; });
  }

  function drawMapButtons() {
    var seg = clear("mapBands");
    ["all"].concat(run.bands).forEach(function (b) {
      var btn = document.createElement("button");
      btn.type = "button";
      btn.textContent = b === "all" ? "All bands" : bandLabel(b);
      btn.setAttribute("aria-pressed", String(b === mapBand));
      btn.addEventListener("click", function () { mapBand = b; drawMapButtons(); drawMap(); });
      seg.appendChild(btn);
    });
  }

  function drawMap() {
    var wrap = clear("map");
    var rows = mapRows();
    var W = Math.max(240, Math.floor(wrap.clientWidth)), H = Math.round(W * 0.5);
    var lat0 = -58, lat1 = 80;
    var ch = C.svg(wrap, H, "World map of the grid squares heard" + (mapBand === "all" ? " on all bands" : " on " + bandLabel(mapBand)) +
      "; use the arrow keys to step through them, most heard first.");
    var x = function (lon) { return ((lon + 180) / 360) * ch.w; };
    var y = function (lat) { return ((lat1 - lat) / (lat1 - lat0)) * ch.h; };
    var clipId = "mapclip";
    C.el("rect", { id: clipId + "r", x: 0, y: 0, width: ch.w, height: ch.h }, C.el("clipPath", { id: clipId }, C.el("defs", {}, ch.node)));
    var g = C.el("g", { "clip-path": "url(#" + clipId + ")" }, ch.node);
    C.el("rect", { x: 0, y: 0, width: ch.w, height: ch.h, style: "fill: var(--panel)" }, g);
    [-30, 0, 30, 60].forEach(function (lat) { C.el("line", { class: "chart-grid", x1: 0, x2: ch.w, y1: y(lat), y2: y(lat) }, g); });
    if (land) {
      var d = "";
      land.features.forEach(function (f) {
        var polys = f.geometry.type === "MultiPolygon" ? f.geometry.coordinates : [f.geometry.coordinates];
        polys.forEach(function (poly) {
          var ring = poly[0];
          d += "M" + ring.map(function (p) { return x(p[0]).toFixed(1) + "," + y(p[1]).toFixed(1); }).join("L") + "Z";
        });
      });
      C.el("path", { d: d, style: "fill: var(--land); stroke: var(--land-edge); stroke-width: 0.5" }, g);
    }
    var maxNow = Math.max.apply(null, rows.map(function (r) { return r.spots; }).concat([1]));
    var rmax = Math.max(6, ch.w / 50);
    var fill = mapBand === "all" ? "var(--all)" : color(mapBand);
    var marks = rows.map(function (r) {
      var c = gridCentre(r.grid);
      return { r: r, x: x(c[0]), y: y(c[1]), rad: Math.max(2.5, rmax * Math.sqrt(r.spots / maxNow)) };
    });
    // Biggest first, so a small circle is drawn over a big one and stays visible.
    marks.forEach(function (k) {
      C.el("circle", { cx: k.x.toFixed(1), cy: k.y.toFixed(1), r: k.rad.toFixed(1),
                       style: "fill:" + fill + "; fill-opacity: 0.7; stroke: var(--paper); stroke-width: 1" }, g);
    });
    C.cursor({
      chart: ch, wrap: wrap, count: marks.length,
      at: function (i) { return { x: marks[i].x, y: marks[i].y, r: marks[i].rad }; },
      tip: function (i) {
        var r = marks[i].r;
        return { head: r.grid + (mapBand === "all" ? "" : ", " + bandLabel(mapBand)),
                 lines: [[null, fmt(r.spots) + " decodes, best " + signed(r.best) + " dB"]].concat(
                   mapBand === "all" ? r.by.map(function (p) { return [color(p[0]), bandLabel(p[0]) + ": " + fmt(p[1])]; }) : []) };
      },
      pick: function (px, py) {
        var best = -1, bd = Infinity;
        marks.forEach(function (k, i) {
          var dd = Math.hypot(k.x - px, k.y - py);
          if (dd <= Math.max(k.rad, 8) && dd - k.rad < bd) { bd = dd - k.rad; best = i; }
        });
        return best;
      },
    });
    $("mapNote").textContent = fmt(rows.length) + " grid squares" + (mapBand === "all" ? " on any band" : " on " + bandLabel(mapBand)) +
      ". Sized by decodes" + (mapBand === "all" ? ", all bands together; pick a band to see it alone." : " on this band.") +
      (land ? "" : " (The coastline did not load.)");
    var top = clear("topGrids");
    rows.slice(0, 12).forEach(function (r) {
      var li = document.createElement("li");
      li.textContent = r.grid + " · " + fmt(r.spots);
      top.appendChild(li);
    });
  }

  // ---- SNR histograms -------------------------------------------------------------------------
  function drawSnr() {
    var wrap = clear("snr");
    var bands = run.bands, lo = Infinity, hi = -Infinity, cmax = 1;
    bands.forEach(function (b) {
      run.snr[b].forEach(function (p) { lo = Math.min(lo, p[0]); hi = Math.max(hi, p[0]); cmax = Math.max(cmax, p[1]); });
    });
    var panelH = 78, m = { t: 6, r: 8, b: 24, l: 40 }, gap = 10;
    var ch = C.svg(wrap, m.t + bands.length * (panelH + gap) - gap + m.b,
      "Histograms of decodes by SNR, one per band on the same scales; use the arrow keys to read each bar.");
    var x = C.scale(lo - 0.5, hi + 0.5, m.l, ch.w - m.r), bw = Math.max(1, x(1) - x(0) - 1);
    var bars = [];
    var yt = C.ticks(0, cmax, 2);
    var top = Math.max(cmax, yt[yt.length - 1]);
    bands.forEach(function (b, i) {
      var y0 = m.t + i * (panelH + gap) + panelH, y = C.scale(0, top, y0, y0 - panelH + 14);
      C.el("line", { class: "chart-axis", x1: m.l, x2: ch.w - m.r, y1: y0, y2: y0 }, ch.node);
      yt.forEach(function (v) {
        if (!v) return;
        C.el("line", { class: "chart-grid", x1: m.l, x2: ch.w - m.r, y1: y(v), y2: y(v) }, ch.node);
        C.text(ch.node, m.l - 5, y(v), fmt(v), { "text-anchor": "end", dy: "0.35em" });
      });
      run.snr[b].forEach(function (p) {
        var bx = x(p[0]) - bw / 2, by = y(p[1]);
        C.el("rect", { x: bx.toFixed(1), y: by.toFixed(1), width: bw.toFixed(1), height: Math.max(0.5, y0 - by).toFixed(1),
                       style: "fill:" + color(b) }, ch.node);
        bars.push({ b: b, s: p[0], n: p[1], x: x(p[0]), y: by, y0: y0 });
      });
      var pb = run.per_band.filter(function (q) { return q.band === b; })[0];
      C.text(ch.node, m.l + 4, y0 - panelH + 8, bandLabel(b) + (pb && pb.snr_median_db !== null ? " · median " + signed(pb.snr_median_db) + " dB" : ""),
        { dy: "0.35em", class: "chart-text strong" });
    });
    var axisY = ch.h - m.b;
    C.ticks(lo, hi, Math.max(3, Math.floor(ch.w / 70))).forEach(function (v) {
      C.text(ch.node, x(v), axisY + 15, signed(v), { "text-anchor": "middle" });
      C.el("line", { class: "chart-axis", x1: x(v), x2: x(v), y1: axisY, y2: axisY + 4 }, ch.node);
    });
    C.text(ch.node, ch.w - m.r, axisY + 15, "dB", { "text-anchor": "end", class: "chart-text ink" });
    C.cursor({
      chart: ch, wrap: wrap, count: bars.length,
      at: function (i) { return { x: bars[i].x, y: bars[i].y, r: 0 }; },
      tip: function (i) { var k = bars[i]; return { head: bandLabel(k.b) + ", " + signed(k.s) + " dB", lines: [[color(k.b), fmt(k.n) + " decodes"]] }; },
      pick: function (px, py) {
        var best = -1, bd = Infinity;
        bars.forEach(function (k, i) {
          if (py > k.y0 + gap / 2 || py < k.y0 - panelH - gap / 2) return;
          var dd = Math.abs(k.x - px);
          if (dd < bd && dd <= Math.max(bw, 6)) { bd = dd; best = i; }
        });
        return best;
      },
    });
  }

  // ---- The radio's day ------------------------------------------------------------------------
  function radioChart(id, key, opts) {
    var wrap = clear(id), rd = run.radio;
    var m = { t: 8, r: 10, b: 24, l: 36 };
    var ch = C.svg(wrap, 130, opts.label + " for every slot through the day; use the arrow keys to step through the slots.");
    var mins = rd.i.map(function (i) { return rd.start_min + i * rd.slot_s / 60; });
    var v = rd[key];
    var lo = opts.lo !== undefined ? opts.lo : Math.min.apply(null, v), hi = Math.max.apply(null, v);
    if (opts.pad) { lo -= opts.pad; hi += opts.pad; }
    if (hi === lo) hi = lo + 1;
    var yt = opts.ticks || C.ticks(lo, hi, 3);
    lo = Math.min(lo, yt[0]);
    hi = Math.max(hi, yt[yt.length - 1]);
    var x = C.scale(mins[0], mins[mins.length - 1] + rd.slot_s / 60, m.l, ch.w - m.r), y = C.scale(lo, hi, ch.h - m.b, m.t);
    yt.forEach(function (t) {
      C.el("line", { class: "chart-grid", x1: m.l, x2: ch.w - m.r, y1: y(t), y2: y(t) }, ch.node);
      C.text(ch.node, m.l - 5, y(t), opts.tick ? opts.tick(t) : String(t), { "text-anchor": "end", dy: "0.35em" });
    });
    timeAxis(ch, x, mins[0], mins[mins.length - 1], m, ch.h - m.b);
    var d = "";
    if (opts.bars) {
      v.forEach(function (val, k) {
        if (val > 0) d += "M" + x(mins[k]).toFixed(1) + "," + y(0).toFixed(1) + "V" + y(val).toFixed(1);
      });
      C.el("path", { d: d, "stroke-width": 1.5, style: "stroke: var(--s2)" }, ch.node);
    } else {
      v.forEach(function (val, k) {
        var jump = k === 0 || rd.i[k] - rd.i[k - 1] > 1;
        if (val === null) return;
        d += (jump ? "M" : (opts.step ? "H" + x(mins[k]).toFixed(1) + "V" : "L" + x(mins[k]).toFixed(1) + ",")) +
             (jump ? x(mins[k]).toFixed(1) + "," : "") + y(val).toFixed(1);
      });
      C.el("path", { d: d, fill: "none", "stroke-width": 2, "stroke-linejoin": "round", style: "stroke: var(--s1)" }, ch.node);
    }
    var xs = mins.map(function (t) { return x(t); });
    C.cursor({
      chart: ch, wrap: wrap, count: xs.length, rule: [m.t, ch.h - m.b],
      at: function (k) { return { x: xs[k], y: m.t + 20 }; },
      tip: function (k) { return { head: clock(mins[k]) + " " + tz, lines: [[null, opts.say(v[k])]] }; },
      pick: function (px) { return C.nearest(xs, px); },
    });
  }

  function drawRadio() {
    radioChart("lna", "lna_db", { label: "LNA gain", step: true, pad: 1.5, ticks: [8, 11, 14, 17, 20].filter(function (t) {
      return t >= Math.min.apply(null, run.radio.lna_db) - 3 && t <= Math.max.apply(null, run.radio.lna_db) + 3; }),
      tick: function (t) { return signed(t); }, say: function (val) { return "LNA " + signed(val) + " dB"; } });
    radioChart("clip", "clipped", { label: "ADC clipping", bars: true, lo: 0,
      say: function (val) { return fmt(val) + " clipped status words"; } });
    radioChart("temp", "temp_c", { label: "HL2 temperature", pad: 0.2,
      tick: function (t) { return t.toFixed(1); }, say: function (val) { return val === null ? "no reading" : fmt1(val) + " °C"; } });
  }

  // ---- Baseline -------------------------------------------------------------------------------
  function drawBaseline() {
    var bl = run.baseline, wrap = clear("vs");
    var sec = $("baseline");
    if (!bl || !bl.bands.length) { sec.hidden = true; return; }
    sec.hidden = false;
    var link = document.createElement("a");
    link.href = bl.url;
    link.textContent = "survey of " + longDate(bl.date);
    var hint = $("baseHint");
    hint.replaceChildren(document.createTextNode("jt9 decodes per slot in the one-band "), link,
      document.createTextNode(" (each band for 30 minutes on the same antenna), against this run over the same clock window, " + tz + "."));
    $("baseLegendA").textContent = "One band at a time, " + longDate(bl.date);
    $("baseLegendB").textContent = "Four bands at once, " + longDate(run.date);
    var rowH = 52, m = { t: 4, r: 64, b: 4, l: 8 };
    var ch = C.svg(wrap, m.t + rowH * bl.bands.length + m.b, "Bar chart of decodes per slot, one band at a time against four at once, per band; use the arrow keys to read each band.");
    var max = 1;
    bl.bands.forEach(function (r) { max = Math.max(max, r.one_band, r.four_band || 0); });
    var x = C.scale(0, max, m.l, ch.w - m.r), barH = 12, marks = [];
    bl.bands.forEach(function (r, i) {
      var y0 = m.t + i * rowH;
      var win = clock(hhmmToMin(r.window[0])) + "–" + clock(hhmmToMin(r.window[1]));
      C.text(ch.node, m.l, y0 + 10, bandLabel(r.band) + " · " + win + " " + tz, { dy: "0.35em", class: "chart-text strong" });
      var yA = y0 + 20, yB = yA + barH + 3;
      C.el("rect", { x: m.l, y: yA, width: Math.max(1, x(r.one_band) - m.l).toFixed(1), height: barH, rx: 2, style: "fill: var(--base)" }, ch.node);
      C.text(ch.node, x(r.one_band) + 5, yA + barH / 2, fmt1(r.one_band), { dy: "0.35em", class: "chart-text ink" });
      if (r.four_band !== null) {
        C.el("rect", { x: m.l, y: yB, width: Math.max(1, x(r.four_band) - m.l).toFixed(1), height: barH, rx: 2, style: "fill: var(--s1)" }, ch.node);
        C.text(ch.node, x(r.four_band) + 5, yB + barH / 2, fmt1(r.four_band), { dy: "0.35em", class: "chart-text ink" });
      } else {
        C.text(ch.node, m.l, yB + barH / 2, "Four bands: not recorded at this hour", { dy: "0.35em", class: "chart-text" });
      }
      marks.push({ r: r, y: y0 + rowH / 2, win: win });
    });
    C.cursor({
      chart: ch, wrap: wrap, count: marks.length,
      at: function (i) { return { x: x(Math.max(marks[i].r.one_band, marks[i].r.four_band || 0)), y: marks[i].y, r: 0 }; },
      tip: function (i) {
        var r = marks[i].r;
        return { head: bandLabel(r.band) + ", " + marks[i].win + " " + tz, lines: [
          ["var(--base)", "One band: " + fmt1(r.one_band) + " a slot (" + r.one_band_slots + " slots)"],
          ["var(--s1)", r.four_band === null ? "Four bands: not recorded at this hour" : "Four bands: " + fmt1(r.four_band) + " a slot (" + r.four_band_slots + " sampled)"]] };
      },
      pick: function (px, py) { var i = Math.floor((py - m.t) / rowH); return i >= 0 && i < marks.length ? i : -1; },
    });
  }

  // ---- The run --------------------------------------------------------------------------------
  function drawAll() {
    if (!run) return;
    drawHeadline();
    legendBands();
    drawHeat();
    drawTrend();
    drawMapButtons();
    drawMap();
    drawSnr();
    drawRadio();
    drawBaseline();
  }

  function showRun(meta) {
    var body = $("runBody"), status = $("runStatus");
    $("run").setAttribute("aria-busy", "true");
    getJSON("runs/" + encodeURIComponent(meta.id) + "/run.json").then(function (r) {
      run = r;
      tz = r.tz.label;
      Array.prototype.forEach.call(document.querySelectorAll("[data-tz]"), function (n) { n.textContent = tz; });
      var parts = [longDate(r.date), r.covers.first_local + "–" + r.covers.end_local + " " + tz];
      parts.push(r.totals.slots_sampled + " slots sampled, one in " + r.sampling.every);
      $("runMeta").replaceChildren(document.createTextNode(parts.join(" · ")));
      if (r.corpus_url) {
        var a = document.createElement("a");
        a.href = r.corpus_url;
        a.textContent = "corpus";
        $("runMeta").append(" · ", a);
      }
      var note = $("runNote");
      note.hidden = !r.note;
      note.textContent = r.note || "";
      var lo = $("leftOut");
      Array.prototype.forEach.call(lo.querySelectorAll("[data-run]"), function (n) { n.remove(); });
      function leftOut(s) { var li = document.createElement("li"); li.setAttribute("data-run", ""); li.textContent = s; lo.appendChild(li); }
      if (!r.ours_scored) leftOut("Our own decoder: it has not scored this run yet, so every decode here is jt9's.");
      if (r.note) leftOut(r.note);
      if (!r.corpus_url) leftOut("The audio: this run is not released as a corpus yet.");
      status.hidden = true;
      body.hidden = false;
      $("run").removeAttribute("aria-busy");
      drawAll();
    }).catch(function (e) {
      status.hidden = false;
      body.hidden = true;
      status.textContent = "This run did not load (" + e.message + ").";
      $("run").removeAttribute("aria-busy");
    });
  }

  function init() {
    getJSON(LAND_URL).then(function (j) { land = j; if (run) drawMap(); }).catch(function () { land = null; if (run) drawMap(); });
    getJSON("runs/index.json").then(function (idx) {
      var runs = idx.runs || [];
      if (!runs.length) throw new Error("no runs");
      var pick = $("runPick");
      pick.replaceChildren();
      runs.forEach(function (r) {
        var o = document.createElement("option");
        o.value = r.id;
        o.textContent = longDate(r.date);
        pick.appendChild(o);
      });
      var want = new URLSearchParams(location.search).get("run");
      var cur = runs.filter(function (r) { return r.id === want; })[0] || runs[0];
      pick.value = cur.id;
      pick.disabled = runs.length < 2;
      pick.addEventListener("change", function () {
        var r = runs.filter(function (q) { return q.id === pick.value; })[0];
        var u = new URL(location.href);
        if (r === runs[0]) u.searchParams.delete("run"); else u.searchParams.set("run", r.id);
        history.replaceState(null, "", u);
        showRun(r);
      });
      showRun(cur);
    }).catch(function (e) {
      $("runStatus").textContent = "The runs did not load (" + e.message + ").";
    });
    var lastW = window.innerWidth, timer = null;
    window.addEventListener("resize", function () {
      if (window.innerWidth === lastW) return;
      lastW = window.innerWidth;
      clearTimeout(timer);
      timer = setTimeout(drawAll, 150);
    });
  }

  init();
})();
