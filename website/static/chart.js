// DataPath website: interactive p99.9 graphs.
// Draws each graph from the JSON that build.py extracts from the README SVGs,
// at the width of its container. Drag (or swipe sideways) to pan, Ctrl/pinch
// or the buttons to zoom, double-click to zoom in, click a legend entry to
// hide a series. Without JavaScript the static README SVG is shown instead.
(function () {
	"use strict";
	var NS = "http://www.w3.org/2000/svg";
	var root = document.documentElement;
	var LOG = Math.log10 || function (v) { return Math.log(v) / Math.LN10; };

	function isDark() {
		var t = root.dataset.theme;
		return t ? t === "dark" : matchMedia("(prefers-color-scheme: dark)").matches;
	}
	function esc(s) {
		return String(s).replace(/[&<>"]/g, function (c) { return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]; });
	}
	function fmt(v) {
		if (v >= 1000) return (v / 1000).toFixed(v >= 10000 ? 1 : 2).replace(/\.?0+$/, "") + " ms";
		return (v >= 100 ? v.toFixed(0) : v >= 10 ? v.toFixed(1) : v.toFixed(2)).replace(/\.0+$|(\.\d*[1-9])0+$/, "$1") + " µs";
	}
	function tickLabel(v) {
		if (v >= 1e6) return v / 1e6 + "M";
		if (v >= 1000) return v / 1000 + "k";
		return String(v);
	}
	function shape(kind, x, y, r) {
		var d;
		switch (kind) {
		case "square": return '<rect x="' + (x - r * .85) + '" y="' + (y - r * .85) + '" width="' + r * 1.7 + '" height="' + r * 1.7 + '" rx="1.5"';
		case "diamond": d = "M" + x + "," + (y - r * 1.2) + " l" + r * 1.2 + "," + r * 1.2 + " l" + -r * 1.2 + "," + r * 1.2 + " l" + -r * 1.2 + "," + -r * 1.2 + "z"; break;
		case "triangle": d = "M" + x + "," + (y - r * 1.15) + " l" + r * 1.15 + "," + r * 1.9 + " l" + -r * 2.3 + ",0z"; break;
		default: return '<circle cx="' + x + '" cy="' + y + '" r="' + r + '"';
		}
		return '<path d="' + d + '"';
	}

	function Chart(host, data) {
		this.host = host;
		this.data = data;
		this.hidden = {};
		var lo = Infinity, hi = 0;
		data.panels.forEach(function (p) {
			p.rows.forEach(function (r) {
				for (var k in r.values) { lo = Math.min(lo, r.values[k][1], r.values[k][0]); hi = Math.max(hi, r.values[k][2], r.values[k][0]); }
			});
		});
		this.full = [Math.floor(LOG(lo)), Math.ceil(LOG(hi))];
		this.view = this.full.slice();
		this.build();
		var self = this;
		if (window.ResizeObserver) new ResizeObserver(function () { self.draw(); }).observe(host);
		else addEventListener("resize", function () { self.draw(); });
		new MutationObserver(function () { self.draw(); }).observe(root, { attributes: true, attributeFilter: ["data-theme"] });
		matchMedia("(prefers-color-scheme: dark)").addEventListener("change", function () { self.draw(); });
	}

	Chart.prototype.build = function () {
		var self = this, d = this.data;
		this.host.innerHTML =
			'<div class="chart-bar"><div class="chart-legend"></div><div class="chart-zoom">' +
			'<button type="button" data-z="in" aria-label="Zoom in">+</button>' +
			'<button type="button" data-z="out" aria-label="Zoom out">−</button>' +
			'<button type="button" data-z="reset" aria-label="Show everything">Reset</button></div></div>' +
			'<div class="chart-plot"><svg xmlns="' + NS + '" role="img" aria-label="' + esc(d.title) + '"></svg><div class="chart-tip" hidden></div></div>' +
			'<p class="chart-hint">Drag sideways to pan · Ctrl + scroll, pinch or double-click to zoom · tap a legend entry to hide it</p>';
		this.legend = this.host.querySelector(".chart-legend");
		this.svg = this.host.querySelector("svg");
		this.tip = this.host.querySelector(".chart-tip");
		this.plot = this.host.querySelector(".chart-plot");
		d.series.forEach(function (s) {
			var b = document.createElement("button");
			b.type = "button";
			b.setAttribute("aria-pressed", "true");
			b.dataset.id = s.id;
			b.innerHTML = '<svg viewBox="-7 -7 14 14" width="14" height="14" aria-hidden="true">' + shape(s.shape, 0, 0, 4.5) + ' class="sw"/></svg>' + esc(s.label);
			b.addEventListener("click", function () {
				self.hidden[s.id] = !self.hidden[s.id];
				b.setAttribute("aria-pressed", String(!self.hidden[s.id]));
				self.draw();
			});
			self.legend.appendChild(b);
		});
		this.host.querySelectorAll(".chart-zoom button").forEach(function (b) {
			b.addEventListener("click", function () {
				var z = b.dataset.z, c = (self.view[0] + self.view[1]) / 2;
				if (z === "reset") self.view = self.full.slice();
				else self.zoom(z === "in" ? 0.6 : 1 / 0.6, c);
				self.draw();
			});
		});
		this.bindPointer();
	};

	Chart.prototype.zoom = function (factor, center) {
		var span = Math.max(0.25, Math.min((this.view[1] - this.view[0]) * factor, this.full[1] - this.full[0] + 1));
		var t = (center - this.view[0]) / (this.view[1] - this.view[0]);
		this.view = [center - t * span, center + (1 - t) * span];
		this.clamp();
	};
	Chart.prototype.clamp = function () {
		var span = this.view[1] - this.view[0], min = this.full[0] - 0.5, max = this.full[1] + 0.5;
		if (this.view[0] < min) this.view = [min, min + span];
		if (this.view[1] > max) this.view = [max - span, max];
	};
	Chart.prototype.toValue = function (px) { // pixel x in the plot -> log10 value
		return this.view[0] + (px - this.L) / (this.R - this.L) * (this.view[1] - this.view[0]);
	};

	Chart.prototype.bindPointer = function () {
		var self = this, svg = this.svg, pts = {}, start = null;
		function localX(e) { return e.clientX - svg.getBoundingClientRect().left; }
		svg.addEventListener("wheel", function (e) {
			if (!e.ctrlKey && !e.metaKey) return; // plain scrolling scrolls the page
			e.preventDefault();
			self.zoom(Math.exp(e.deltaY * 0.01), self.toValue(localX(e)));
			self.draw();
		}, { passive: false });
		svg.addEventListener("dblclick", function (e) {
			self.zoom(e.shiftKey ? 1 / 0.5 : 0.5, self.toValue(localX(e)));
			self.draw();
		});
		svg.addEventListener("pointerdown", function (e) {
			pts[e.pointerId] = localX(e);
			svg.setPointerCapture(e.pointerId);
			start = { view: self.view.slice(), pts: Object.assign({}, pts), moved: false };
		});
		svg.addEventListener("pointermove", function (e) {
			if (!(e.pointerId in pts)) { self.hover(e); return; }
			pts[e.pointerId] = localX(e);
			var ids = Object.keys(pts);
			var per = (start.view[1] - start.view[0]) / (self.R - self.L);
			if (ids.length === 1) {
				var dx = pts[ids[0]] - start.pts[ids[0]];
				if (Math.abs(dx) > 3) start.moved = true;
				self.view = [start.view[0] - dx * per, start.view[1] - dx * per];
			} else if (ids.length === 2 && ids[0] in start.pts && ids[1] in start.pts) {
				var a0 = start.pts[ids[0]], b0 = start.pts[ids[1]], a1 = pts[ids[0]], b1 = pts[ids[1]];
				if (Math.abs(b0 - a0) < 10) return;
				var f = Math.abs(b0 - a0) / Math.max(10, Math.abs(b1 - a1));
				var c0 = start.view[0] + ((a0 + b0) / 2 - self.L) * per;
				var span = Math.max(0.25, (start.view[1] - start.view[0]) * f), pps = (self.R - self.L) / span;
				var left = c0 - ((a1 + b1) / 2 - self.L) / pps;
				self.view = [left, left + span];
				start.moved = true;
			}
			self.clamp();
			self.draw();
		});
		function up(e) {
			delete pts[e.pointerId];
			if (start && !start.moved && e.type === "pointerup") self.hover(e, true);
			start = Object.keys(pts).length ? { view: self.view.slice(), pts: Object.assign({}, pts), moved: true } : null;
		}
		svg.addEventListener("pointerup", up);
		svg.addEventListener("pointercancel", up);
		svg.addEventListener("pointerleave", function (e) { if (e.pointerType === "mouse") self.tip.hidden = true; });
	};

	Chart.prototype.hover = function (e, tap) {
		var r = this.svg.getBoundingClientRect(), x = e.clientX - r.left, y = e.clientY - r.top, best = null, bd = tap ? 22 : 14;
		(this.marks || []).forEach(function (m) {
			var d = Math.hypot(m.x - x, m.y - y);
			if (d < bd) { bd = d; best = m; }
		});
		if (!best) { this.tip.hidden = true; return; }
		this.tip.innerHTML = "<strong>" + esc(best.label) + "</strong><br>" + esc(best.panel) + ", " + esc(best.size) +
			"<br>p99.9 <b>" + fmt(best.v[0]) + "</b>" + (best.v[1] !== best.v[2] ? ' <span>(runs ' + fmt(best.v[1]) + " to " + fmt(best.v[2]) + ")</span>" : "");
		this.tip.hidden = false;
		var tw = this.tip.offsetWidth, left = Math.min(Math.max(best.x - tw / 2, 0), r.width - tw);
		this.tip.style.left = left + "px";
		var top = best.y - this.tip.offsetHeight - 12;
		this.tip.style.top = (top < 0 ? best.y + 14 : top) + "px";
	};

	Chart.prototype.draw = function () {
		var d = this.data, W = this.host.clientWidth;
		if (!W) return;
		var narrow = W < 520, pal = d.colors[isDark() ? "dark" : "light"] || d.colors.light;
		var L = this.L = narrow ? 64 : 72, R = this.R = W - (narrow ? 10 : 20);
		var ROW = narrow ? 22 : 26, S = narrow ? 4 : 5;
		var v0 = this.view[0], v1 = this.view[1], self = this;
		var X = function (v) { return L + (LOG(v) - v0) / (v1 - v0) * (R - L); };
		var visible = d.series.filter(function (s) { return !self.hidden[s.id]; });
		var lane = {}; // spread series vertically within a row so equal values stay visible
		visible.forEach(function (s, i) { lane[s.id] = (i - (visible.length - 1) / 2) * Math.min(5, (ROW - 10) / Math.max(1, visible.length - 1)); });
		var colors = {};
		d.series.forEach(function (s) { colors[s.id] = pal[s.id] || "#888"; });
		this.legend.querySelectorAll("button").forEach(function (b) { b.style.setProperty("--c", colors[b.dataset.id]); });

		// Ticks: 1-2-5 when there is room, else decades only.
		var pxPerDec = (R - L) / (v1 - v0), ticks = [];
		for (var e = Math.floor(v0); e <= Math.ceil(v1); e++) {
			[1, 2, 5].forEach(function (m) {
				var v = m * Math.pow(10, e), lv = LOG(v);
				if (lv < v0 - 1e-9 || lv > v1 + 1e-9) return;
				ticks.push({ v: v, major: m === 1, label: m === 1 || pxPerDec > (narrow ? 150 : 110) });
			});
		}

		var o = [], y = 0, marks = [];
		o.push('<defs><clipPath id="clip-' + this.host.dataset.chart + '"><rect x="' + (L - 8) + '" y="0" width="' + (R - L + 16) + '" height="100000"/></clipPath></defs>');
		d.panels.forEach(function (p) {
			y += 22;
			o.push('<text class="c-panel" x="0" y="' + y + '">' + esc(p.name) + "</text>");
			var top = y + 10, bottom = top + p.rows.length * ROW;
			ticks.forEach(function (t) {
				var gx = X(t.v).toFixed(1);
				o.push('<line class="c-grid' + (t.major ? "" : " minor") + '" x1="' + gx + '" x2="' + gx + '" y1="' + top + '" y2="' + bottom + '"/>');
			});
			p.rows.forEach(function (r, i) {
				o.push('<text class="c-size" x="' + (L - 10) + '" y="' + (top + i * ROW + ROW / 2 + 4) + '" text-anchor="end">' + esc(r.size) + "</text>");
			});
			o.push('<g clip-path="url(#clip-' + self.host.dataset.chart + ')">');
			p.rows.forEach(function (r, i) {
				var ry = top + i * ROW + ROW / 2;
				visible.forEach(function (s) {
					var v = r.values[s.id];
					if (!v) return;
					var my = ry + lane[s.id], c = colors[s.id];
					o.push('<line x1="' + X(v[1]).toFixed(1) + '" x2="' + X(v[2]).toFixed(1) + '" y1="' + my + '" y2="' + my + '" stroke="' + c + '" stroke-width="1.5" stroke-linecap="round" opacity=".55"/>');
				});
				visible.forEach(function (s) {
					var v = r.values[s.id];
					if (!v) return;
					var mx = X(v[0]), my = ry + lane[s.id];
					o.push(shape(s.shape, mx.toFixed(1), my, S) + ' fill="' + colors[s.id] + '" class="c-mark"/>');
					if (mx >= L - 8 && mx <= R + 8) marks.push({ x: mx, y: my, v: v, label: s.label, panel: p.name, size: r.size });
				});
			});
			o.push("</g>");
			y = bottom + 16;
			ticks.forEach(function (t) {
				if (!t.label) return;
				var tx = X(t.v), anchor = tx > W - 14 ? "end" : "middle";
				o.push('<text class="c-tick" x="' + (anchor === "end" ? W - 1 : tx).toFixed(1) + '" y="' + y + '" text-anchor="' + anchor + '">' + tickLabel(t.v) + "</text>");
			});
			y += 6;
		});
		this.marks = marks;
		this.svg.setAttribute("viewBox", "0 0 " + W + " " + (y + 4));
		this.svg.setAttribute("width", W);
		this.svg.setAttribute("height", y + 4);
		this.svg.innerHTML = o.join("");
		this.tip.hidden = true;
	};

	document.querySelectorAll(".chart[data-chart]").forEach(function (host) {
		var src = document.getElementById(host.dataset.chart);
		if (!src) return;
		try {
			new Chart(host, JSON.parse(src.textContent));
			host.closest(".graph").classList.add("live");
		} catch (e) {
			if (window.console) console.error(e);
		}
	});
	// Charts in hidden tabs have no width until shown.
	document.querySelectorAll(".graph-card .tab").forEach(function (t) {
		t.addEventListener("click", function () { dispatchEvent(new Event("resize")); });
	});
})();
