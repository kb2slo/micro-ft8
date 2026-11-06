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
  function bandLabel(b) { return b.replace("m", "\u00a0m"); }
  function fmt(n) { return Number(n).toLocaleString("en-US"); }
  function fmt1(n) { return (Math.round(n * 10) / 10).toFixed(1); }
  function signed(n) { return (n > 0 ? "+" : n < 0 ? "\u2212" : "") + Math.abs(n); }
  function longDate(iso) { var p = iso.split("-"); return Number(p[2]) + " " + MONTHS[Number(p[1]) - 1] + " " + p[0]; }
  // Local minutes since midnight -> "8:17 AM" (minutes past 1440 wrap to the next day).
  function clock(m) {
    m = ((Math.round(m) % 1440) + 1440) % 1440;
    var h = Math.floor(m / 60), mm = m % 60;
    return (h % 12 || 12) + ":" + (mm < 10 ? "0" : "") + mm + " " + (h < 12 ? "AM" : "PM");
  }
  function km(n) { return fmt(Math.round(n)) + "\u00a0km"; }
  function mi(n) { return fmt(Math.round(n / 1.609344)) + "\u00a0mi"; }
  function kmMi(n) { return km(n) + " (" + mi(n) + ")"; }
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
                 lines: [[color(r.band), fmt1(r.mean) + " decodes a slot (" + r.slots + (r.slots === 1 ? " slot" : " slots") + " sampled)"]] };
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
        return { head: clock(t) + "\u2013" + clock(t + run.trend.bin_min) + " " + tz,
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
        .map(function (g) { return { grid: g[0], spots: g[2], best: g[3], km: g[4], by: [[g[1], g[2]]] }; });
    }
    var acc = {};
    run.grids.forEach(function (g) {
      var e = acc[g[0]] || (acc[g[0]] = { grid: g[0], spots: 0, best: -99, km: g[4], by: [] });
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
    // Distance rings around the station, and the station itself: ink, not a band colour.
    var st = run.station, R = 6371;
    function ringPath(dkm) {
      var la1 = st.lat * Math.PI / 180, lo1 = st.lon * Math.PI / 180, dr = dkm / R, out = "", prevX = null;
      for (var b = 0; b <= 360; b += 2) {
        var br = b * Math.PI / 180;
        var la2 = Math.asin(Math.sin(la1) * Math.cos(dr) + Math.cos(la1) * Math.sin(dr) * Math.cos(br));
        var lo2 = lo1 + Math.atan2(Math.sin(br) * Math.sin(dr) * Math.cos(la1), Math.cos(dr) - Math.sin(la1) * Math.sin(la2));
        var lon = ((lo2 * 180 / Math.PI + 540) % 360) - 180, px = x(lon), py = y(la2 * 180 / Math.PI);
        out += (prevX === null || Math.abs(px - prevX) > ch.w / 2 ? "M" : "L") + px.toFixed(1) + "," + py.toFixed(1);
        prevX = px;
      }
      return out;
    }
    if (st) {
      [5000, 10000].forEach(function (dkm) {
        C.el("path", { d: ringPath(dkm), class: "map-ring" }, g);
        var la = st.lat + dkm / 111.2;
        if (la < lat1 - 4) C.text(g, x(st.lon) + 3, y(la) + 10, fmt(dkm) + " km", { class: "chart-text map-ring-label" });
      });
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
    if (st) {
      var sx = x(st.lon), sy = y(st.lat), k = 7;
      C.el("path", { d: "M" + sx + "," + (sy - k) + "L" + (sx + k) + "," + sy + "L" + sx + "," + (sy + k) + "L" + (sx - k) + "," + sy + "Z",
                     class: "map-station" }, g);
      C.text(g, sx - k - 3, sy, st.grid, { "text-anchor": "end", dy: "0.35em", class: "chart-text map-station-label" });
    }
    C.cursor({
      chart: ch, wrap: wrap, count: marks.length,
      at: function (i) { return { x: marks[i].x, y: marks[i].y, r: marks[i].rad }; },
      tip: function (i) {
        var r = marks[i].r;
        return { head: r.grid + (mapBand === "all" ? "" : ", " + bandLabel(mapBand)) + (r.km !== undefined ? " \u00b7 " + kmMi(r.km) : ""),
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
      (run.station ? " The diamond is the station, " + run.station.grid + "; the dashed rings are 5,000 and 10,000 km from it." : "") +
      (land ? "" : " (The coastline did not load.)");
    var top = clear("topGrids");
    rows.slice(0, 12).forEach(function (r) {
      var li = document.createElement("li");
      li.textContent = r.grid + " \u00b7 " + fmt(r.spots);
      top.appendChild(li);
    });
  }

  // ---- How far: decodes by distance, shared bins and scales --------------------------------------
  function distAxis(ch, x, lo, hi, y) {
    C.ticks(lo, hi, Math.max(3, Math.floor(ch.w / 80))).forEach(function (v) {
      C.el("line", { class: "chart-axis", x1: x(v), x2: x(v), y1: y, y2: y + 4 }, ch.node);
      C.text(ch.node, x(v), y + 15, fmt(v), { "text-anchor": "middle", class: "chart-text ink" });
      C.text(ch.node, x(v), y + 28, mi(v), { "text-anchor": "middle" });
    });
    C.text(ch.node, ch.w - 8, y + 15, "km", { "text-anchor": "end", class: "chart-text ink" });
  }

  function distMax() {
    var hi = 0, bin = run.distance.bin_km;
    run.bands.forEach(function (b) { (run.distance.bands[b].hist || []).forEach(function (p) { hi = Math.max(hi, p[0] + bin); }); });
    return hi;
  }

  function drawDist() {
    var wrap = clear("dist"), D = run.distance, bin = D.bin_km, bands = run.bands;
    var placed = 0, all = 0;
    bands.forEach(function (b) { placed += D.bands[b].placed; all += D.bands[b].decodes; });
    $("farPlaced").textContent = fmt(placed) + " of " + fmt(all) + " decodes (" + Math.round(100 * placed / all) +
      "%) had a sender whose square is known.";
    var hi = distMax(), cmax = 1;
    bands.forEach(function (b) { D.bands[b].hist.forEach(function (p) { cmax = Math.max(cmax, p[1]); }); });
    var panelH = 70, gap = 10, m = { t: 6, r: 8, b: 34, l: 44 };
    var ch = C.svg(wrap, m.t + bands.length * (panelH + gap) - gap + m.b,
      "Histograms of decodes by distance, one per band on the same scales; use the arrow keys to read each bar.");
    var x = C.scale(0, hi, m.l, ch.w - m.r), bw = Math.max(1, x(bin) - x(0) - 1), bars = [];
    var yt = C.ticks(0, cmax, 2), top = Math.max(cmax, yt[yt.length - 1]);
    bands.forEach(function (b, i) {
      var y0 = m.t + i * (panelH + gap) + panelH, y = C.scale(0, top, y0, y0 - panelH + 14), d = D.bands[b];
      C.el("line", { class: "chart-axis", x1: m.l, x2: ch.w - m.r, y1: y0, y2: y0 }, ch.node);
      yt.forEach(function (v) {
        if (!v) return;
        C.el("line", { class: "chart-grid", x1: m.l, x2: ch.w - m.r, y1: y(v), y2: y(v) }, ch.node);
        C.text(ch.node, m.l - 5, y(v), fmt(v), { "text-anchor": "end", dy: "0.35em" });
      });
      d.hist.forEach(function (p) {
        var by = y(p[1]);
        C.el("rect", { x: (x(p[0]) + 0.5).toFixed(1), y: by.toFixed(1), width: bw.toFixed(1), height: Math.max(0.5, y0 - by).toFixed(1),
                       style: "fill:" + color(b) }, ch.node);
        bars.push({ b: b, k: p[0], n: p[1], x: x(p[0] + bin / 2), y: by, y0: y0 });
      });
      C.text(ch.node, m.l + 4, y0 - panelH + 8, bandLabel(b) + (d.median_km !== null ? " \u00b7 median " + kmMi(d.median_km) : ""),
        { dy: "0.35em", class: "chart-text strong" });
    });
    distAxis(ch, x, 0, hi, ch.h - m.b);
    C.cursor({
      chart: ch, wrap: wrap, count: bars.length,
      at: function (i) { return { x: bars[i].x, y: bars[i].y, r: 0 }; },
      tip: function (i) {
        var k = bars[i];
        return { head: bandLabel(k.b) + ", " + fmt(k.k) + "\u2013" + km(k.k + bin), lines: [[null, mi(k.k) + "\u2013" + mi(k.k + bin)], [color(k.b), fmt(k.n) + " decodes"]] };
      },
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

  function drawFarthest() {
    var box = clear("farthest");
    run.bands.forEach(function (b) {
      var t = document.createElement("table"), cap = document.createElement("caption");
      t.className = "data";
      cap.textContent = bandLabel(b);
      t.appendChild(cap);
      var head = document.createElement("thead");
      head.innerHTML = "<tr><th scope=\"col\">Call</th><th scope=\"col\">Grid</th><th scope=\"col\">Distance</th><th scope=\"col\">Best SNR</th></tr>";
      t.appendChild(head);
      var body = document.createElement("tbody");
      run.distance.bands[b].farthest.forEach(function (f) {
        var tr = document.createElement("tr");
        [f[0], f[1], null, signed(f[3]) + " dB"].forEach(function (v, j) {
          var td = document.createElement(j === 0 ? "th" : "td");
          if (j === 0) td.scope = "row";
          if (v === null) {
            td.textContent = km(f[2]);
            var sub = document.createElement("span");
            sub.className = "sub";
            sub.textContent = mi(f[2]);
            td.appendChild(sub);
          } else td.textContent = v;
          tr.appendChild(td);
        });
        body.appendChild(tr);
      });
      t.appendChild(body);
      box.appendChild(t);
    });
  }

  // ---- Distance against SNR: binned counts and the median line, shared scales ----------------------
  function drawSnrDist() {
    var wrap = clear("snrdist"), D = run.distance, bin = D.bin_km, bands = run.bands;
    var hi = distMax(), slo = Infinity, shi = -Infinity;
    bands.forEach(function (b) { D.bands[b].snr_cells.forEach(function (c) { slo = Math.min(slo, c[1]); shi = Math.max(shi, c[1]); }); });
    var panelH = 120, gap = 14, m = { t: 6, r: 8, b: 34, l: 44 };
    var ch = C.svg(wrap, m.t + bands.length * (panelH + gap) - gap + m.b,
      "Distance against SNR, one panel per band on the same scales, with the median SNR by distance; use the arrow keys to read the medians.");
    var x = C.scale(0, hi, m.l, ch.w - m.r), cw = Math.max(1, x(bin) - x(0));
    var yt = C.ticks(slo, shi, 3), pts = [];
    bands.forEach(function (b, i) {
      var top = m.t + i * (panelH + gap) + 14, y0 = top + panelH - 14;
      var y = C.scale(slo - 0.5, shi + 0.5, y0, top), ch1 = Math.max(1, y(0) - y(1)), d = D.bands[b];
      var cmax = Math.max.apply(null, d.snr_cells.map(function (c) { return c[2]; }).concat([1]));
      C.el("line", { class: "chart-axis", x1: m.l, x2: ch.w - m.r, y1: y0, y2: y0 }, ch.node);
      yt.forEach(function (v) {
        C.el("line", { class: "chart-grid", x1: m.l, x2: ch.w - m.r, y1: y(v), y2: y(v) }, ch.node);
        C.text(ch.node, m.l - 5, y(v), signed(v), { "text-anchor": "end", dy: "0.35em" });
      });
      d.snr_cells.forEach(function (c) {
        C.el("rect", { x: x(c[0]).toFixed(1), y: (y(c[1]) - ch1 / 2).toFixed(1), width: cw.toFixed(1), height: ch1.toFixed(1),
                       style: "fill:" + color(b) + "; fill-opacity:" + (0.12 + 0.88 * Math.sqrt(c[2] / cmax)).toFixed(2) }, ch.node);
      });
      var line = "";
      d.snr_median.forEach(function (p, j) {
        var px = x(p[0] + bin / 2), py = y(p[1]);
        line += (j && p[0] - d.snr_median[j - 1][0] > bin ? "M" : j ? "L" : "M") + px.toFixed(1) + "," + py.toFixed(1);
        pts.push({ b: b, k: p[0], med: p[1], n: p[2], x: px, y: py, top: top, y0: y0 });
      });
      C.el("path", { d: line, class: "median-line" }, ch.node);
      C.text(ch.node, m.l + 4, top - 7, bandLabel(b) + " \u00b7 SNR, dB", { dy: "0.35em", class: "chart-text strong" });
    });
    distAxis(ch, x, 0, hi, ch.h - m.b);
    C.cursor({
      chart: ch, wrap: wrap, count: pts.length,
      at: function (i) { return { x: pts[i].x, y: pts[i].y, r: 3 }; },
      tip: function (i) {
        var p = pts[i];
        return { head: bandLabel(p.b) + ", " + fmt(p.k) + "\u2013" + km(p.k + bin), lines: [[null, mi(p.k) + "\u2013" + mi(p.k + bin)],
          [color(p.b), "median " + signed(p.med) + " dB over " + fmt(p.n) + " decodes"]] };
      },
      pick: function (px, py) {
        var best = -1, bd = Infinity;
        pts.forEach(function (p, i) {
          if (py < p.top - gap / 2 || py > p.y0 + gap / 2) return;
          var dd = Math.abs(p.x - px);
          if (dd < bd) { bd = dd; best = i; }
        });
        return best;
      },
    });
  }

  // ---- What the shared filter and the LNA cost ------------------------------------------------------
  function drawLnaFind() {
    var ab = run.lna_ab, sec = $("lnafind"), bl = run.baseline;
    var wins = bl ? bl.bands.filter(function (r) { return r.four_band && r.four_band.source === "scored"; }) : [];
    sec.hidden = !ab && !wins.length;
    if (sec.hidden) return;
    var verdict = [];
    if (ab && ab.paired && ab.arms.length === 2) {
      var A = ab.arms[0], B = ab.arms[1], pj = ab.paired.jt9, po = ab.paired.ours;
      var seen = Math.abs(pj[0]) > 2 * pj[1] || Math.abs(po[0]) > 2 * po[1];
      var gain = Math.abs(A.lna_db - B.lna_db);
      verdict.push(seen
        ? gain + " dB of LNA gain on " + bandLabel(ab.band) + " after dark made a difference these counts can see."
        : gain + " dB of LNA gain on " + bandLabel(ab.band) + " after dark made no difference these counts can see, though the higher gain flagged clipping in " +
          Math.round(100 * A.clipped_share_median) + "% of the ADC's status words (median) against " + Math.round(100 * B.clipped_share_median) + "%.");
      $("abHead").textContent = bandLabel(ab.band) + ", the LNA at +" + A.lna_db + " and +" + B.lna_db + " dB, after dark";
      $("abHint").textContent = "One receiver, " + clock(hhmmToMin(ab.window[0])) + "\u2013" + clock(hhmmToMin(ab.window[1])) + " " + tz + ", " +
        fmt(ab.slots) + " slots: the LNA held at one gain for " + ab.turn_slots + " slots, then the other, in turn. Every slot scored with jt9 and our decoder.";
      var t = clear("abTable");
      t.innerHTML = "<thead><tr><th scope=\"col\">LNA</th><th scope=\"col\">Slots</th><th scope=\"col\">jt9 a slot</th><th scope=\"col\">Ours a slot</th><th scope=\"col\">Clipped words, median</th></tr></thead>";
      var tb = document.createElement("tbody");
      ab.arms.forEach(function (a) {
        var tr = document.createElement("tr");
        [["th", "+" + a.lna_db + " dB"], ["td", fmt(a.slots)], ["td", fmt1(a.jt9)], ["td", fmt1(a.ours) + " (" + fmt1(a.ours_pct) + "%)"],
         ["td", fmt(a.clipped_median) + " of " + fmt(a.status_words_median) + " (" + Math.round(100 * a.clipped_share_median) + "%)"]].forEach(function (c) {
          var n = document.createElement(c[0]);
          if (c[0] === "th") n.scope = "row";
          n.textContent = c[1];
          tr.appendChild(n);
        });
        tb.appendChild(tr);
      });
      t.appendChild(tb);
      Array.prototype.forEach.call(document.querySelectorAll("[data-arm-a]"), function (n) { n.textContent = A.lna_db; });
      Array.prototype.forEach.call(document.querySelectorAll("[data-arm-b]"), function (n) { n.textContent = B.lna_db; });
      drawPaired(A, B, ab.paired);
    }
    if (wins.length) {
      var lnas = wins.map(function (r) { return r.four_band.lna; }).filter(function (v, i, a) { return v && a.indexOf(v) === i; });
      var names = function (rs) { return rs.map(function (r) { return bandLabel(r.band); }).join(", ").replace(/, ([^,]*)$/, " and $1"); };
      var costly = wins.filter(function (r) { return r.four_band.jt9 < 0.9 * r.one_band.jt9; });
      verdict.push(costly.length
        ? "On " + names(costly) + ", four bands at once heard under 90% of what one band at a time did: a cost these counts can see, or that day's band."
        : "On " + names(wins) + ", four receivers sharing the 10\u00a0m low-pass, with the LNA at " + lnas.join(", ") +
          "\u00a0dB, cost nothing these counts can see against one band at a time (a cost over about 10% would show).");
      var dOurs = Math.max.apply(null, wins.map(function (r) { return Math.abs(r.four_band.ours_pct - r.one_band.ours_pct); }));
      if (dOurs <= 6) verdict.push("Our decoder's share of jt9's moved by " + fmt1(dOurs) + " points at most, so its gap to jt9 is the decoder's own, not the radio's.");
      var p = clear("winVerdict");
      p.textContent = wins.map(function (r) {
        var o = r.one_band, f = r.four_band;
        return bandLabel(r.band) + ": jt9 " + fmt1(f.jt9) + " a slot against " + fmt1(o.jt9) + " (" + signed(Math.round(100 * (f.jt9 / o.jt9 - 1))) +
          "%), ours " + fmt1(f.ours_pct) + "% of jt9's against " + fmt1(o.ours_pct) + "%";
      }).join("; ") + ".";
    }
    $("lnaVerdict").textContent = verdict.join(" ");
  }

  function drawPaired(A, B, pr) {
    var wrap = clear("abPaired"), m = { t: 8, r: 16, b: 24, l: 64 }, rowH = 28;
    var rows = [["jt9", pr.jt9, "var(--muted)"], ["Ours", pr.ours, "var(--s1)"]];
    var ch = C.svg(wrap, m.t + rows.length * rowH + m.b, "Paired differences per turn, with one standard error, for jt9 and our decoder; use the arrow keys to read each.");
    var lim = 0;
    rows.forEach(function (r) { lim = Math.max(lim, Math.abs(r[1][0]) + r[1][1]); });
    lim = Math.max(1, Math.ceil(lim * 1.3));
    var x = C.scale(-lim, lim, m.l, ch.w - m.r);
    C.ticks(-lim, lim, 4).forEach(function (v) {
      C.el("line", { class: v === 0 ? "chart-axis" : "chart-grid", x1: x(v), x2: x(v), y1: m.t, y2: ch.h - m.b }, ch.node);
      C.text(ch.node, x(v), ch.h - m.b + 15, signed(v), { "text-anchor": "middle" });
    });
    var pts = [];
    rows.forEach(function (r, i) {
      var cy = m.t + i * rowH + rowH / 2, mean = r[1][0], se = r[1][1];
      C.text(ch.node, m.l - 8, cy, r[0], { "text-anchor": "end", dy: "0.35em", class: "chart-text ink" });
      C.el("line", { x1: x(mean - se), x2: x(mean + se), y1: cy, y2: cy, "stroke-width": 2, style: "stroke:" + r[2] }, ch.node);
      C.el("circle", { cx: x(mean), cy: cy, r: 4.5, style: "fill:" + r[2] }, ch.node);
      pts.push({ x: x(mean), y: cy, r: r });
    });
    C.cursor({
      chart: ch, wrap: wrap, count: pts.length,
      at: function (i) { return { x: pts[i].x, y: pts[i].y, r: 4.5 }; },
      tip: function (i) {
        var r = pts[i].r;
        return { head: r[0] + ", +" + A.lna_db + " minus +" + B.lna_db + " dB", lines: [[r[2], (r[1][0] >= 0 ? "+" : "\u2212") + Math.abs(r[1][0]).toFixed(2) +
          " \u00b1 " + r[1][1].toFixed(2) + " decodes a slot, " + pr.pairs + " pairs"]] };
      },
      pick: function (px, py) { var i = Math.floor((py - m.t) / rowH); return i >= 0 && i < pts.length ? i : -1; },
    });
  }

  // ---- Our decoder against jt9, per stream and band -----------------------------------------------
  function drawOurs() {
    var hs = run.hour_scores, sec = $("ours");
    sec.hidden = !hs || !hs.length;
    if (sec.hidden) return;
    // By hour: a line per band through each stream's middle.
    var wrap = clear("oursHour"), m = { t: 10, r: 40, b: 24, l: 40 };
    var ch = C.svg(wrap, 220, "Line chart of our decoder's share of jt9's decodes by hour, one line per band; use the arrow keys to step through the hours.");
    var mid = function (h) { return h.start_min + h.slots * 0.125; };  // 15 s slots: half the stream, in minutes
    var lo = Math.min.apply(null, hs.map(function (h) { return h.start_min; }));
    var hi = Math.max.apply(null, hs.map(function (h) { return h.start_min + h.slots / 4; }));
    var pl = Math.min.apply(null, hs.map(function (h) { return h.ours_pct; })), ph = Math.max.apply(null, hs.map(function (h) { return h.ours_pct; }));
    var yt = C.ticks(Math.floor(pl / 10) * 10, Math.ceil(ph / 10) * 10, 4);
    var x = C.scale(lo, hi, m.l, ch.w - m.r), y = C.scale(yt[0], yt[yt.length - 1], ch.h - m.b, m.t);
    yt.forEach(function (v) {
      C.el("line", { class: "chart-grid", x1: m.l, x2: ch.w - m.r, y1: y(v), y2: y(v) }, ch.node);
      C.text(ch.node, m.l - 6, y(v), v + "%", { "text-anchor": "end", dy: "0.35em" });
    });
    timeAxis(ch, x, lo, hi, m, ch.h - m.b);
    var ends = [], starts = [];
    run.bands.forEach(function (b) {
      var v = hs.filter(function (h) { return h.band === b; });
      if (!v.length) return;
      var d = v.map(function (h, i) { return (i ? "L" : "M") + x(mid(h)).toFixed(1) + "," + y(h.ours_pct).toFixed(1); }).join("");
      C.el("path", { d: d, fill: "none", "stroke-width": 2, "stroke-linejoin": "round", style: "stroke:" + color(b) }, ch.node);
      v.forEach(function (h) { C.el("circle", { cx: x(mid(h)).toFixed(1), cy: y(h.ours_pct).toFixed(1), r: 3, style: "fill:" + color(b) }, ch.node); });
      var last = v[v.length - 1];
      ends.push({ b: b, x: x(mid(last)), y: y(last.ours_pct) });
    });
    ends.sort(function (a, b) { return a.y - b.y; });
    for (var i = 1; i < ends.length; i++) ends[i].y = Math.max(ends[i].y, ends[i - 1].y + 12);
    ends.forEach(function (e) { C.text(ch.node, e.x + 6, e.y, bandLabel(e.b), { dy: "0.35em", class: "chart-text ink" }); });
    var times = [];
    hs.forEach(function (h) { if (times.indexOf(h.start_min) < 0) times.push(h.start_min); });
    times.sort(function (a, b) { return a - b; });
    var xs = times.map(function (t) { var h = hs.filter(function (q) { return q.start_min === t; })[0]; return x(mid(h)); });
    C.cursor({
      chart: ch, wrap: wrap, count: times.length, rule: [m.t, ch.h - m.b],
      at: function (k) { return { x: xs[k], y: m.t + 20 }; },
      tip: function (k) {
        var v = hs.filter(function (h) { return h.start_min === times[k]; });
        return { head: clock(times[k]) + "\u2013" + clock(times[k] + v[0].slots / 4) + " " + tz + ", " + v[0].slots + " slots",
                 lines: run.bands.map(function (b) {
                   var h = v.filter(function (q) { return q.band === b; })[0];
                   return [color(b), bandLabel(b) + ": " + (h ? fmt1(h.ours_pct) + "% of " + fmt1(h.jt9) + " a slot" : "not scored")];
                 }) };
      },
      pick: function (px) { return C.nearest(xs, px); },
    });

    // Against crowding: a dot per band and hour; shape as well as colour tells the bands apart.
    var wrap2 = clear("oursCrowd"), m2 = { t: 10, r: 14, b: 34, l: 40 };
    var ch2 = C.svg(wrap2, 240, "Scatter of our decoder's share against jt9's decodes a slot, a dot per band and hour; use the arrow keys to read each.");
    var jmax = Math.max.apply(null, hs.map(function (h) { return h.jt9; }));
    var xt = C.ticks(0, jmax, 5), x2 = C.scale(0, Math.max(jmax, xt[xt.length - 1]), m2.l, ch2.w - m2.r);
    var y2 = C.scale(yt[0], yt[yt.length - 1], ch2.h - m2.b, m2.t);
    yt.forEach(function (v) {
      C.el("line", { class: "chart-grid", x1: m2.l, x2: ch2.w - m2.r, y1: y2(v), y2: y2(v) }, ch2.node);
      C.text(ch2.node, m2.l - 6, y2(v), v + "%", { "text-anchor": "end", dy: "0.35em" });
    });
    xt.forEach(function (v) {
      C.el("line", { class: "chart-axis", x1: x2(v), x2: x2(v), y1: ch2.h - m2.b, y2: ch2.h - m2.b + 4 }, ch2.node);
      C.text(ch2.node, x2(v), ch2.h - m2.b + 15, String(v), { "text-anchor": "middle" });
    });
    C.text(ch2.node, ch2.w - m2.r, ch2.h - 4, "jt9 decodes a slot", { "text-anchor": "end", class: "chart-text ink" });
    var shapes = ["circle", "square", "diamond", "triangle"], pts = [];
    function mark(kind, cx, cy, fill) {
      var r = 4.5, d;
      if (kind === "circle") return C.el("circle", { cx: cx, cy: cy, r: r, style: fill }, ch2.node);
      if (kind === "square") d = "M" + (cx - r) + "," + (cy - r) + "h" + 2 * r + "v" + 2 * r + "h" + -2 * r + "Z";
      else if (kind === "diamond") d = "M" + cx + "," + (cy - r - 1) + "L" + (cx + r + 1) + "," + cy + "L" + cx + "," + (cy + r + 1) + "L" + (cx - r - 1) + "," + cy + "Z";
      else d = "M" + cx + "," + (cy - r - 1) + "L" + (cx + r + 1) + "," + (cy + r) + "L" + (cx - r - 1) + "," + (cy + r) + "Z";
      return C.el("path", { d: d, style: fill }, ch2.node);
    }
    hs.slice().sort(function (a, b) { return a.jt9 - b.jt9; }).forEach(function (h) {
      var px = x2(h.jt9), py = y2(h.ours_pct), k = run.bands.indexOf(h.band);
      mark(shapes[k % 4], px, py, "fill:" + color(h.band) + "; stroke: var(--paper); stroke-width: 1");
      pts.push({ h: h, x: px, y: py });
    });
    C.cursor({
      chart: ch2, wrap: wrap2, count: pts.length,
      at: function (i) { return { x: pts[i].x, y: pts[i].y, r: 5 }; },
      tip: function (i) {
        var h = pts[i].h;
        return { head: bandLabel(h.band) + ", " + clock(h.start_min) + " " + tz,
                 lines: [[color(h.band), fmt1(h.ours_pct) + "% of jt9's " + fmt1(h.jt9) + " a slot (ours " + fmt1(h.ours) + ")"]] };
      },
      pick: function (px, py) {
        var best = -1, bd = 12;
        pts.forEach(function (p, i) { var d = Math.hypot(p.x - px, p.y - py); if (d < bd) { bd = d; best = i; } });
        return best;
      },
    });
    var lg = document.createElement("ul");
    lg.className = "legend";
    run.bands.forEach(function (b, k) {
      var li = document.createElement("li"), sp = document.createElement("span");
      var ic = document.createElement("span");
      ic.textContent = ["\u25cf", "\u25a0", "\u25c6", "\u25b2"][k % 4] + " ";
      ic.style.color = color(b);
      ic.setAttribute("aria-hidden", "true");
      sp.textContent = bandLabel(b);
      li.append(ic, sp);
      lg.appendChild(li);
    });
    wrap2.insertBefore(lg, wrap2.firstChild);
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
      C.text(ch.node, m.l + 4, y0 - panelH + 8, bandLabel(b) + (pb && pb.snr_median_db !== null ? " \u00b7 median " + signed(pb.snr_median_db) + " dB" : ""),
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
      tick: function (t) { return t.toFixed(1); }, say: function (val) { return val === null ? "no reading" : fmt1(val) + " \u00b0C"; } });
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
      document.createTextNode(" (each band for 30 minutes on the same antenna), against this run over the same clock window, " + tz +
        ". \u2014 means not measured."));
    $("baseLegendA").textContent = "One band at a time, " + longDate(bl.date);
    $("baseLegendB").textContent = "Four bands at once, " + longDate(run.date);
    var rowH = 52, m = { t: 4, r: 64, b: 4, l: 8 };
    var ch = C.svg(wrap, m.t + rowH * bl.bands.length + m.b, "Bar chart of jt9 decodes per slot, one band at a time against four at once, per band; use the arrow keys to read each band.");
    var max = 1;
    bl.bands.forEach(function (r) { max = Math.max(max, r.one_band.jt9, r.four_band ? r.four_band.jt9 : 0); });
    var x = C.scale(0, max, m.l, ch.w - m.r), barH = 12, marks = [];
    function win(r) { return clock(hhmmToMin(r.window[0])) + "\u2013" + clock(hhmmToMin(r.window[1])); }
    bl.bands.forEach(function (r, i) {
      var y0 = m.t + i * rowH, o = r.one_band, f = r.four_band;
      C.text(ch.node, m.l, y0 + 10, bandLabel(r.band) + " \u00b7 " + win(r) + " " + tz, { dy: "0.35em", class: "chart-text strong" });
      var yA = y0 + 20, yB = yA + barH + 3;
      C.el("rect", { x: m.l, y: yA, width: Math.max(1, x(o.jt9) - m.l).toFixed(1), height: barH, rx: 2, style: "fill: var(--base)" }, ch.node);
      C.text(ch.node, x(o.jt9) + 5, yA + barH / 2, fmt1(o.jt9), { dy: "0.35em", class: "chart-text ink" });
      if (f) {
        C.el("rect", { x: m.l, y: yB, width: Math.max(1, x(f.jt9) - m.l).toFixed(1), height: barH, rx: 2, style: "fill: var(--s1)" }, ch.node);
        C.text(ch.node, x(f.jt9) + 5, yB + barH / 2, fmt1(f.jt9), { dy: "0.35em", class: "chart-text ink" });
      } else {
        C.text(ch.node, m.l, yB + barH / 2, "Four bands: not recorded at this hour", { dy: "0.35em", class: "chart-text" });
      }
      marks.push({ r: r, y: y0 + rowH / 2 });
    });
    C.cursor({
      chart: ch, wrap: wrap, count: marks.length,
      at: function (i) { var r = marks[i].r; return { x: x(Math.max(r.one_band.jt9, r.four_band ? r.four_band.jt9 : 0)), y: marks[i].y, r: 0 }; },
      tip: function (i) {
        var r = marks[i].r, f = r.four_band;
        return { head: bandLabel(r.band) + ", " + win(r) + " " + tz, lines: [
          ["var(--base)", "One band: " + fmt1(r.one_band.jt9) + " a slot (" + r.one_band.slots + " slots)"],
          ["var(--s1)", !f ? "Four bands: not recorded at this hour" : "Four bands: " + fmt1(f.jt9) + " a slot (" + f.slots + (f.source === "scored" ? " slots, all scored)" : " sampled)")]] };
      },
      pick: function (px, py) { var i = Math.floor((py - m.t) / rowH); return i >= 0 && i < marks.length ? i : -1; },
    });
    // One row a band, one column a run: each cell stacks its figures, so it reads at phone width.
    var t = clear("vsTable");
    t.innerHTML = "<thead><tr><th scope=\"col\">Band</th><th scope=\"col\">One band, " + longDate(bl.date) +
      "</th><th scope=\"col\">Four bands, " + longDate(run.date) + "</th></tr></thead>";
    var tb = document.createElement("tbody");
    function cell(f) {
      var td = document.createElement("td");
      if (!f) { td.textContent = "not recorded at this hour"; td.className = "muted"; return td; }
      [[fmt1(f.jt9) + " a slot (jt9)", true],
       ["ours " + (f.ours_pct !== null && f.ours_pct !== undefined ? fmt1(f.ours_pct) + "% of jt9's" : "\u2014"), false],
       ["SNR median " + (f.snr_median_db !== null && f.snr_median_db !== undefined ? signed(f.snr_median_db) + " dB" : "\u2014"), false],
       ["LNA " + (f.lna ? f.lna + " dB" : "\u2014"), false]].forEach(function (x) {
        var sp = document.createElement("span");
        sp.className = x[1] ? "lead-fig" : "sub";
        sp.textContent = x[0];
        td.appendChild(sp);
      });
      return td;
    }
    bl.bands.forEach(function (r) {
      var tr = document.createElement("tr"), th = document.createElement("th");
      th.scope = "row";
      th.textContent = bandLabel(r.band);
      tr.append(th, cell(r.one_band), cell(r.four_band));
      tb.appendChild(tr);
    });
    t.appendChild(tb);
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
    if (run.distance) { drawDist(); drawFarthest(); drawSnrDist(); }
    $("far").hidden = $("snrfar").hidden = !run.distance;
    drawSnr();
    drawOurs();
    drawRadio();
    drawLnaFind();
    drawBaseline();
  }

  function showRun(meta) {
    var body = $("runBody"), status = $("runStatus");
    $("run").setAttribute("aria-busy", "true");
    getJSON("runs/" + encodeURIComponent(meta.id) + "/run.json").then(function (r) {
      run = r;
      tz = r.tz.label;
      Array.prototype.forEach.call(document.querySelectorAll("[data-tz]"), function (n) { n.textContent = tz; });
      var parts = [longDate(r.date), r.covers.first_local + "\u2013" + r.covers.end_local + " " + tz];
      parts.push(r.totals.slots_sampled + " slots sampled, one in " + r.sampling.every);
      $("runMeta").replaceChildren(document.createTextNode(parts.join(" \u00b7 ")));
      if (r.corpus_url) {
        var a = document.createElement("a");
        a.href = r.corpus_url;
        a.textContent = "corpus README";
        $("runMeta").append(" \u00b7 ", a);
      }
      var sw = $("stationWhere");
      sw.replaceChildren();
      if (r.station) {
        var gb = document.createElement("b");
        gb.textContent = r.station.grid;
        sw.append("In this run it was listening from ", gb, (r.station.label ? ", " + r.station.label : "") + ". " +
          (r.station.grid.length >= 6
            ? "That six-character square places the station to a few km; the uncertainty in a distance is the heard station's four-character square, about 100 by 200\u00a0km, so short distances are still rough."
            : "Distances run from that four-character square's centre to each heard square's centre, both about 100 by 200\u00a0km, so distances are rough under about 150\u00a0km."));
      }
      Array.prototype.forEach.call(document.querySelectorAll("[data-station]"), function (n) { n.textContent = r.station ? r.station.grid : ""; });
      Array.prototype.forEach.call(document.querySelectorAll("[data-bin]"), function (n) { n.textContent = r.distance ? fmt(r.distance.bin_km) : ""; });
      var note = $("runNote");
      note.hidden = !r.note;
      note.textContent = r.note || "";
      var lo = $("leftOut");
      Array.prototype.forEach.call(lo.querySelectorAll("[data-run]"), function (n) { n.remove(); });
      function leftOut(s) { var li = document.createElement("li"); li.setAttribute("data-run", ""); li.textContent = s; lo.appendChild(li); }
      if (!r.ours_scored) leftOut("Our own decoder on the day's sampled slots: those figures are jt9's. Ours scored only " +
        (r.lna_ab || (r.baseline && r.baseline.bands.some(function (b) { return b.four_band && b.four_band.source === "scored"; }))
          ? "the scored windows and the LNA A/B." : "nothing in this run yet."));
      if (!r.station) leftOut("Distances: the run does not say where it listened from.");
      if (r.note) leftOut(r.note);
      if (!r.release_url) leftOut("The audio: " + (r.corpus_url ? "its release is to come; the corpus README holds the manifests and every score." : "this run is not released as a corpus yet."));
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
