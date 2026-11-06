// Small hand-drawn SVG chart helpers for the Experiments pages: no framework, no dependency
// (docs/architecture/webapp.md, Experiments). Each page draws its own charts with these.
var Charts = (function () {
  "use strict";
  var NS = "http://www.w3.org/2000/svg";

  function el(tag, attrs, parent) {
    var n = document.createElementNS(NS, tag);
    for (var k in attrs) {
      if (attrs[k] === null || attrs[k] === undefined) continue;
      if (k === "style") n.setAttribute("style", attrs[k]);
      else if (k === "text") n.textContent = attrs[k];
      else n.setAttribute(k, attrs[k]);
    }
    if (parent) parent.appendChild(n);
    return n;
  }

  // An SVG sized to its container's width, one user unit per CSS pixel; `label` is what a screen
  // reader says, and keyboard users focus it to step through the marks.
  function svg(wrap, height, label) {
    var w = Math.max(240, Math.floor(wrap.clientWidth));
    var s = el("svg", { viewBox: "0 0 " + w + " " + height, width: "100%", height: height,
                        role: "img", "aria-label": label, tabindex: "0", class: "chart-svg" });
    wrap.appendChild(s);
    return { node: s, w: w, h: height };
  }

  function text(parent, x, y, str, attrs) {
    var a = { x: x, y: y, text: str, class: "chart-text" };
    for (var k in attrs || {}) a[k] = attrs[k];
    return el("text", a, parent);
  }

  // Round ticks over [lo, hi], about n of them.
  function ticks(lo, hi, n) {
    if (hi <= lo) return [lo];
    var raw = (hi - lo) / Math.max(1, n);
    var mag = Math.pow(10, Math.floor(Math.log10(raw)));
    var step = [1, 2, 2.5, 5, 10].map(function (m) { return m * mag; }).filter(function (s) { return s >= raw; })[0];
    var out = [];
    for (var v = Math.ceil(lo / step) * step; v <= hi + step * 1e-9; v += step) out.push(Math.round(v * 1e6) / 1e6);
    return out;
  }

  function scale(d0, d1, r0, r1) {
    var k = d1 === d0 ? 0 : (r1 - r0) / (d1 - d0);
    return function (v) { return r0 + (v - d0) * k; };
  }

  // A tooltip box inside `wrap` (which must be position: relative).
  function tooltip(wrap) {
    var t = document.createElement("div");
    t.className = "chart-tip";
    t.hidden = true;
    t.setAttribute("role", "status");
    wrap.appendChild(t);
    return {
      show: function (head, lines, x, y) {
        t.replaceChildren();
        var h = document.createElement("div");
        h.className = "chart-tip-h";
        h.textContent = head;
        t.appendChild(h);
        (lines || []).forEach(function (ln) {
          var d = document.createElement("div");
          if (ln[0]) {
            var i = document.createElement("i");
            i.style.background = ln[0];
            d.appendChild(i);
          }
          d.appendChild(document.createTextNode(ln[1]));
          t.appendChild(d);
        });
        t.hidden = false;
        var W = wrap.clientWidth, w = t.offsetWidth, hh = t.offsetHeight;
        var left = x + 14 + w > W ? x - 14 - w : x + 14;
        t.style.left = Math.max(0, Math.min(W - w, left)) + "px";
        t.style.top = (y - hh - 12 >= 0 ? y - hh - 12 : y + 16) + "px"; // above the mark, or below it
      },
      hide: function () { t.hidden = true; },
    };
  }

  // Hover, tap and keyboard for one chart. opts: chart (from svg()), wrap, count, at(i) -> {x, y},
  // tip(i) -> {head, lines}, pick(x, y) -> index or -1, rule (draw a vertical rule at the mark),
  // start (the index arrow keys start from).
  function cursor(opts) {
    var s = opts.chart.node, tip = tooltip(opts.wrap), cur = -1;
    var mark = opts.rule
      ? el("line", { class: "chart-rule", y1: opts.rule[0], y2: opts.rule[1], visibility: "hidden" }, s)
      : el("circle", { class: "chart-ring", r: 6, visibility: "hidden" }, s);
    function place(i) {
      cur = i;
      if (i < 0) { tip.hide(); mark.setAttribute("visibility", "hidden"); return; }
      var p = opts.at(i), t = opts.tip(i);
      if (opts.rule) { mark.setAttribute("x1", p.x); mark.setAttribute("x2", p.x); }
      else { mark.setAttribute("cx", p.x); mark.setAttribute("cy", p.y); if (p.r) mark.setAttribute("r", p.r + 3); }
      mark.setAttribute("visibility", "visible");
      // An SVG element has no offsetLeft/offsetTop: place from the two boxes instead.
      var sr = s.getBoundingClientRect(), wr = opts.wrap.getBoundingClientRect(), k = sr.width / opts.chart.w;
      tip.show(t.head, t.lines, p.x * k + sr.left - wr.left, p.y * k + sr.top - wr.top);
    }
    function local(e) {
      var r = s.getBoundingClientRect();
      return [(e.clientX - r.left) * opts.chart.w / r.width, (e.clientY - r.top) * opts.chart.h / r.height];
    }
    s.addEventListener("pointermove", function (e) {
      if (e.pointerType === "touch") return;
      var p = local(e);
      place(opts.pick(p[0], p[1]));
    });
    s.addEventListener("pointerleave", function (e) { if (e.pointerType !== "touch") place(-1); });
    s.addEventListener("pointerdown", function (e) {
      if (e.pointerType !== "touch") return;
      var p = local(e);
      place(opts.pick(p[0], p[1]));
    });
    document.addEventListener("pointerdown", function (e) {
      if (e.pointerType === "touch" && !s.contains(e.target)) place(-1);
    });
    s.addEventListener("focus", function () { if (cur < 0 && opts.count > 0) place(opts.start || 0); });
    s.addEventListener("blur", function () { place(-1); });
    s.addEventListener("keydown", function (e) {
      var step = { ArrowRight: 1, ArrowDown: 1, ArrowLeft: -1, ArrowUp: -1 }[e.key];
      if (e.key === "Escape") { place(-1); return; }
      if (e.key === "Home") step = -opts.count;
      if (e.key === "End") step = opts.count;
      if (!step) return;
      e.preventDefault();
      place(Math.max(0, Math.min(opts.count - 1, (cur < 0 ? 0 : cur) + step)));
    });
  }

  // The index in `xs` (sorted ascending) nearest to x.
  function nearest(xs, x) {
    var lo = 0, hi = xs.length - 1;
    if (hi < 0) return -1;
    while (hi - lo > 1) {
      var mid = (lo + hi) >> 1;
      if (xs[mid] < x) lo = mid; else hi = mid;
    }
    return Math.abs(xs[lo] - x) <= Math.abs(xs[hi] - x) ? lo : hi;
  }

  return { el: el, svg: svg, text: text, ticks: ticks, scale: scale, tooltip: tooltip, cursor: cursor, nearest: nearest };
})();
