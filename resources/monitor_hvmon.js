// ═════════════════════════════════════════════════════════════════════
//  HV MONITOR TAB  —  VMon stability monitoring
//
//  Per-module data:
//    fast buffer:  up to FAST_N VMon samples from fast polling
//    ring buffer:  up to RING_N entries, each = {mean, rms} from a
//                  completed fast buffer epoch
//
//  Geo view shows sigma(VMon) color map.
//  Click a module to see its fast + ring buffer plots.
// ═════════════════════════════════════════════════════════════════════

// ── Config (overridden from gui_config.hvMonitor on init) ───────────
let FAST_N  = 5000;
let RING_N  = 100;
let MIN_SIGMA_PTS = 20;

// ── Per-module monitor ──────────────────────────────────────────────
class ModuleMonitor {
    constructor() {
        this.fastN  = 0;
        this.fastTs = new Float32Array(FAST_N);   // relative ms (from shared t0)
        this.fastV  = new Float32Array(FAST_N);   // VMon values
        this.fastT0 = null;                        // epoch-ms of first sample

        this.ringN    = 0;
        this.ringHead = 0;
        this.ringTs   = new Float32Array(RING_N);  // relative ms from ringT0
        this.ringMean = new Float32Array(RING_N);
        this.ringRms  = new Float32Array(RING_N);
        this.ringT0   = null;
    }

    push(epochMs, vmon) {
        if (this.fastN >= FAST_N) this.flush();
        if (this.fastT0 === null) this.fastT0 = epochMs;
        this.fastTs[this.fastN] = epochMs - this.fastT0;
        this.fastV[this.fastN]  = vmon;
        this.fastN++;
    }

    flush() {
        if (this.fastN === 0) return;
        const n = this.fastN;
        let sum = 0, sumSq = 0;
        for (let i = 0; i < n; i++) {
            const v = this.fastV[i];
            sum   += v;
            sumSq += v * v;
        }
        const mean = sum / n;
        const rms  = Math.sqrt(Math.max(0, sumSq / n - mean * mean));

        // Timestamp: midpoint of the fast buffer epoch
        const epochMs = this.fastT0 + this.fastTs[n - 1];
        if (this.ringT0 === null) this.ringT0 = epochMs;

        const idx = this.ringHead % RING_N;
        this.ringTs[idx]   = epochMs - this.ringT0;
        this.ringMean[idx] = mean;
        this.ringRms[idx]  = rms;
        this.ringHead++;
        this.ringN = Math.min(this.ringN + 1, RING_N);

        // Reset fast buffer
        this.fastN  = 0;
        this.fastT0 = null;
    }

    /** Running sigma for geo-view coloring. */
    std() {
        if (this.fastN < MIN_SIGMA_PTS) return null;
        let sum = 0, sumSq = 0;
        for (let i = 0; i < this.fastN; i++) {
            const v = this.fastV[i];
            sum   += v;
            sumSq += v * v;
        }
        const mean = sum / this.fastN;
        return Math.sqrt(Math.max(0, sumSq / this.fastN - mean * mean));
    }

    /** Ordered ring buffer arrays (oldest first). */
    ringOrdered() {
        const n = this.ringN;
        if (n === 0) return { ts: [], mean: [], rms: [] };
        const start = (this.ringHead >= RING_N) ? this.ringHead % RING_N : 0;
        const ts = new Float32Array(n), mean = new Float32Array(n), rms = new Float32Array(n);
        for (let i = 0; i < n; i++) {
            const j = (start + i) % RING_N;
            ts[i]   = this.ringTs[j];
            mean[i] = this.ringMean[j];
            rms[i]  = this.ringRms[j];
        }
        return { ts, mean, rms };
    }
}

const hvmonModules = {};   // name -> ModuleMonitor
let   hvmonSelected = null;
let   hvmonHover    = null;
let   hvmonGeoInited = false;

// Geo view state
let _hvmGeoCanvas, _hvmGeoCtx, _hvmGeoWrap;
let _hvmOx = 0, _hvmOy = 0, _hvmSc = 0.5;
let _hvmDrag = null;
let _hvmRmsHi = 0.1;
let _hvmFitted = false;

// ── Data entry point (called from monitor.js on hv_vmon_snapshot) ───
function onHVMonVMonData(data, ts) {
    for (const entry of data) {
        let mon = hvmonModules[entry.n];
        if (!mon) { mon = new ModuleMonitor(); hvmonModules[entry.n] = mon; }
        mon.push(ts, entry.v);
    }
}

// ── Tab render (called from renderActiveTab) ────────────────────────
let _hvmLastGeoRender = 0;
let _hvmLastPlotRender = 0;

function renderHVMonTab() {
    const now = performance.now();
    // Geo: throttle to ~1 Hz
    if (now - _hvmLastGeoRender > 900) {
        renderHVMonGeo();
        _hvmLastGeoRender = now;
    }
    // Plots: throttle to ~4 Hz
    if (now - _hvmLastPlotRender > 250) {
        renderHVMonPlots();
        _hvmLastPlotRender = now;
    }
}

// ═════════════════════════════════════════════════════════════════════
//  GEO VIEW  (sigma color map — reuses MODULES from monitor_geo.js)
// ═════════════════════════════════════════════════════════════════════

function initHVMonGeo() {
    if (hvmonGeoInited) { _hvmFit(); renderHVMonGeo(); return; }
    _hvmGeoWrap   = document.getElementById('hvmon-geo-wrap');
    _hvmGeoCanvas = document.getElementById('hvmon-geo-canvas');
    _hvmGeoCtx    = _hvmGeoCanvas.getContext('2d');

    // Mouse handlers — pan with drag, click to select
    let _hvmDownPos = null;
    _hvmGeoWrap.addEventListener('mousedown', e => {
        if (e.button !== 0) return;
        _hvmDownPos = { x: e.clientX, y: e.clientY };
        _hvmDrag = { sx: e.clientX, sy: e.clientY, ox: _hvmOx, oy: _hvmOy };
    });
    window.addEventListener('mousemove', e => {
        if (_hvmDrag) {
            _hvmOx = _hvmDrag.ox + e.clientX - _hvmDrag.sx;
            _hvmOy = _hvmDrag.oy + e.clientY - _hvmDrag.sy;
            renderHVMonGeo();
        }
    });
    window.addEventListener('mouseup', () => { _hvmDrag = null; });

    _hvmGeoWrap.addEventListener('wheel', e => {
        e.preventDefault();
        const rect = _hvmGeoWrap.getBoundingClientRect();
        const mx = e.clientX - rect.left, my = e.clientY - rect.top;
        const f = e.deltaY < 0 ? 1.12 : 1 / 1.12;
        const ns = Math.max(0.05, Math.min(50, _hvmSc * f));
        _hvmOx = mx - (mx - _hvmOx) * (ns / _hvmSc);
        _hvmOy = my - (my - _hvmOy) * (ns / _hvmSc);
        _hvmSc = ns;
        renderHVMonGeo();
    }, { passive: false });

    _hvmGeoWrap.addEventListener('click', e => {
        // Distinguish click from drag using distance from mousedown
        if (_hvmDownPos &&
            (Math.abs(e.clientX - _hvmDownPos.x) > 4 ||
             Math.abs(e.clientY - _hvmDownPos.y) > 4)) return;
        const mod = _hvmHitTest(e);
        if (mod) {
            hvmonSelected = mod.n;
            renderHVMonGeo();
            renderHVMonPlots();
        }
    });

    _hvmGeoWrap.addEventListener('dblclick', () => { _hvmFit(); renderHVMonGeo(); });

    new ResizeObserver(() => { _hvmResizeCanvas(); renderHVMonGeo(); }).observe(_hvmGeoWrap);

    hvmonGeoInited = true;
    _hvmResizeCanvas();
    _hvmFit();
    renderHVMonGeo();
    _buildHVMonLegend();
}

function _hvmResizeCanvas() {
    const rect = _hvmGeoWrap.getBoundingClientRect();
    _hvmGeoCanvas.width  = rect.width  * devicePixelRatio;
    _hvmGeoCanvas.height = rect.height * devicePixelRatio;
    _hvmGeoCanvas.style.width  = rect.width  + 'px';
    _hvmGeoCanvas.style.height = rect.height + 'px';
    _hvmGeoCtx.setTransform(devicePixelRatio, 0, 0, devicePixelRatio, 0, 0);
}

function _hvmFit() {
    if (typeof MODULES === 'undefined' || !MODULES.length) return;
    const rect = _hvmGeoWrap.getBoundingClientRect();
    if (!rect.width) return;
    const extent = (typeof GV !== 'undefined' && GV.extent) ? GV.extent : 600;
    const sx = rect.width  / (2 * extent);
    const sy = rect.height / (2 * extent);
    _hvmSc = Math.min(sx, sy) * 0.92;
    _hvmOx = rect.width  / 2;
    _hvmOy = rect.height / 2;
    _hvmFitted = true;
}

function _hvmW2S(wx, wy) {
    return [wx * _hvmSc + _hvmOx, -wy * _hvmSc + _hvmOy];
}

function _hvmHitTest(e) {
    if (typeof MODULES === 'undefined') return null;
    const rect = _hvmGeoWrap.getBoundingClientRect();
    const sx = e.clientX - rect.left, sy = e.clientY - rect.top;
    const wx = (sx - _hvmOx) / _hvmSc, wy = -(sy - _hvmOy) / _hvmSc;
    for (let i = MODULES.length - 1; i >= 0; i--) {
        const m = MODULES[i];
        if (m.t === 'booster') continue;
        if (Math.abs(wx - m.x) <= m.sx / 2 && Math.abs(wy - m.y) <= m.sy / 2) return m;
    }
    return null;
}

function _hvmRmsColor(val) {
    if (val == null) return '#23263a';
    const t = Math.min(1, val / _hvmRmsHi);
    if (t < 0.25) return _hvmLerp([30,58,138],[59,130,246], t*4);
    if (t < 0.5)  return _hvmLerp([59,130,246],[34,197,94], (t-.25)*4);
    if (t < 0.75) return _hvmLerp([34,197,94],[234,179,8], (t-.5)*4);
    return _hvmLerp([234,179,8],[220,38,38], (t-.75)*4);
}

function _hvmLerp(a, b, t) {
    return `rgb(${Math.round(a[0]+(b[0]-a[0])*t)},${Math.round(a[1]+(b[1]-a[1])*t)},${Math.round(a[2]+(b[2]-a[2])*t)})`;
}

function renderHVMonGeo() {
    if (!_hvmGeoCtx || typeof MODULES === 'undefined') return;
    const ctx = _hvmGeoCtx;
    const W = _hvmGeoCanvas.width / devicePixelRatio;
    const H = _hvmGeoCanvas.height / devicePixelRatio;
    ctx.clearRect(0, 0, W, H);
    ctx.fillStyle = '#0e1118';
    ctx.fillRect(0, 0, W, H);

    // Compute RMS range
    const rmsVals = [];
    for (const name in hvmonModules) {
        const s = hvmonModules[name].std();
        if (s != null) rmsVals.push(s);
    }
    if (rmsVals.length > 0) {
        rmsVals.sort((a, b) => a - b);
        _hvmRmsHi = Math.max(rmsVals[Math.min(rmsVals.length - 1, Math.floor(rmsVals.length * 0.98))], 1e-6);
    }

    const gap = Math.max(0.3 * _hvmSc, 0.5);
    for (const m of MODULES) {
        if (m.t === 'booster') continue;
        const [sx, sy] = _hvmW2S(m.x, m.y);
        const w = m.sx * _hvmSc, h = m.sy * _hvmSc;
        const mon = hvmonModules[m.n];
        ctx.fillStyle = _hvmRmsColor(mon ? mon.std() : null);
        ctx.fillRect(sx - w/2 + gap, sy - h/2 + gap,
                     Math.max(1, w - 2*gap), Math.max(1, h - 2*gap));
    }

    // Highlight selected
    if (hvmonSelected) {
        const m = (typeof MOD_MAP !== 'undefined') ? MOD_MAP[hvmonSelected]
                : MODULES.find(m => m.n === hvmonSelected);
        if (m) {
            const [sx, sy] = _hvmW2S(m.x, m.y);
            ctx.strokeStyle = '#fff';
            ctx.lineWidth = 2;
            ctx.strokeRect(sx - m.sx*_hvmSc/2, sy - m.sy*_hvmSc/2,
                           m.sx*_hvmSc, m.sy*_hvmSc);
        }
    }

    // Legend bar (draw inline at bottom-left)
    const lx = 10, ly = H - 32, lw = Math.min(200, W - 20), lh = 12;
    if (lw > 40) {
        for (let i = 0; i < lw; i++) {
            ctx.fillStyle = _hvmRmsColor(_hvmRmsHi * i / lw);
            ctx.fillRect(lx + i, ly, 1, lh);
        }
        ctx.fillStyle = '#b0b8c4';
        ctx.font = '9px JetBrains Mono, monospace';
        ctx.fillText('0', lx, ly - 2);
        const hi = _hvmRmsHi < 1 ? _hvmRmsHi.toFixed(4) : _hvmRmsHi.toFixed(2);
        ctx.fillText(hi + ' V', lx + lw + 4, ly + lh);
        ctx.fillText('\u03c3(VMon)  [dbl-click reset]', lx, ly + lh + 12);
    }
}

function _buildHVMonLegend() {
    // The legend is drawn directly on the canvas; the div is left for
    // potential future controls.
    const el = document.getElementById('hvmon-legend');
    if (el) el.textContent = '';
}


// ═════════════════════════════════════════════════════════════════════
//  CANVAS PLOT HELPER
// ═════════════════════════════════════════════════════════════════════

/**
 * Draw a simple line/errorbar plot on a Canvas element.
 * @param {HTMLCanvasElement} canvas
 * @param {Object} opts
 *   .xs      Float32Array  x values
 *   .ys      Float32Array  y values (line mode)
 *   .yLo     Float32Array  lower y (errorbar mode)
 *   .yHi     Float32Array  upper y (errorbar mode)
 *   .n       int           number of points
 *   .xLabel  string        x-axis label
 *   .yLabel  string        y-axis label
 *   .color   string        line/marker color
 *   .errorbar bool         draw error bars instead of line
 */
function hvmonPlot(canvas, opts) {
    const dpr = devicePixelRatio;
    const rect = canvas.getBoundingClientRect();
    canvas.width  = rect.width  * dpr;
    canvas.height = rect.height * dpr;
    canvas.style.width  = rect.width  + 'px';
    canvas.style.height = rect.height + 'px';

    const ctx = canvas.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    const W = rect.width, H = rect.height;

    // Background
    ctx.fillStyle = '#0e1118';
    ctx.fillRect(0, 0, W, H);

    const n = opts.n || 0;
    if (n < 2) {
        ctx.fillStyle = '#64748b';
        ctx.font = '12px JetBrains Mono, monospace';
        ctx.textAlign = 'center';
        ctx.fillText(n === 0 ? 'No data' : 'Waiting for data\u2026', W/2, H/2);
        return;
    }

    // Margins
    const ml = 64, mr = 16, mt = 8, mb = 32;
    const pw = W - ml - mr, ph = H - mt - mb;
    if (pw < 20 || ph < 20) return;

    // Compute ranges
    let xMin = opts.xs[0], xMax = opts.xs[0];
    let yMin, yMax;
    for (let i = 0; i < n; i++) {
        if (opts.xs[i] < xMin) xMin = opts.xs[i];
        if (opts.xs[i] > xMax) xMax = opts.xs[i];
    }

    if (opts.errorbar) {
        yMin = opts.yLo[0]; yMax = opts.yHi[0];
        for (let i = 0; i < n; i++) {
            if (opts.yLo[i] < yMin) yMin = opts.yLo[i];
            if (opts.yHi[i] > yMax) yMax = opts.yHi[i];
        }
    } else {
        yMin = opts.ys[0]; yMax = opts.ys[0];
        for (let i = 0; i < n; i++) {
            if (opts.ys[i] < yMin) yMin = opts.ys[i];
            if (opts.ys[i] > yMax) yMax = opts.ys[i];
        }
    }

    // Pad y range
    let yPad = (yMax - yMin) * 0.08;
    if (yPad < 0.001) yPad = 0.01;
    yMin -= yPad; yMax += yPad;
    if (xMax === xMin) xMax = xMin + 1;

    const xScale = pw / (xMax - xMin);
    const yScale = ph / (yMax - yMin);
    const toSx = x => ml + (x - xMin) * xScale;
    const toSy = y => mt + ph - (y - yMin) * yScale;

    // Grid + ticks
    ctx.strokeStyle = 'rgba(148,163,184,0.12)';
    ctx.lineWidth = 0.5;
    ctx.fillStyle = '#94a3b8';
    ctx.font = '9px JetBrains Mono, monospace';

    // Y ticks
    const yTicks = _niceRange(yMin, yMax, 5);
    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    for (const v of yTicks) {
        const sy = toSy(v);
        ctx.beginPath(); ctx.moveTo(ml, sy); ctx.lineTo(ml + pw, sy); ctx.stroke();
        ctx.fillText(v.toPrecision(6).replace(/\.?0+$/, ''), ml - 4, sy);
    }

    // X ticks
    const xTicks = _niceRange(xMin, xMax, 6);
    ctx.textAlign = 'center';
    ctx.textBaseline = 'top';
    for (const v of xTicks) {
        const sx = toSx(v);
        ctx.beginPath(); ctx.moveTo(sx, mt); ctx.lineTo(sx, mt + ph); ctx.stroke();
        // Format: show as seconds if range > 10000 ms
        const label = (xMax - xMin) > 10000 ? (v / 1000).toFixed(1) + 's' : v.toFixed(0);
        ctx.fillText(label, sx, mt + ph + 4);
    }

    // Axes
    ctx.strokeStyle = 'rgba(148,163,184,0.3)';
    ctx.lineWidth = 1;
    ctx.strokeRect(ml, mt, pw, ph);

    // Labels
    ctx.fillStyle = '#94a3b8';
    ctx.font = '10px JetBrains Mono, monospace';
    ctx.textAlign = 'center';
    ctx.fillText(opts.xLabel || '', ml + pw / 2, H - 2);
    ctx.save();
    ctx.translate(12, mt + ph / 2);
    ctx.rotate(-Math.PI / 2);
    ctx.fillText(opts.yLabel || '', 0, 0);
    ctx.restore();

    // Draw data
    if (opts.errorbar) {
        // Error bars: whiskers + dot at mean
        ctx.strokeStyle = opts.color || '#3b82f6';
        ctx.fillStyle   = opts.color || '#3b82f6';
        ctx.lineWidth = 1;
        const capW = Math.max(2, Math.min(6, pw / n * 0.3));
        for (let i = 0; i < n; i++) {
            const sx = toSx(opts.xs[i]);
            const syLo = toSy(opts.yLo[i]);
            const syHi = toSy(opts.yHi[i]);
            const syMid = toSy(opts.ys[i]);
            // Whisker
            ctx.beginPath(); ctx.moveTo(sx, syLo); ctx.lineTo(sx, syHi); ctx.stroke();
            // Caps
            ctx.beginPath(); ctx.moveTo(sx - capW, syLo); ctx.lineTo(sx + capW, syLo); ctx.stroke();
            ctx.beginPath(); ctx.moveTo(sx - capW, syHi); ctx.lineTo(sx + capW, syHi); ctx.stroke();
            // Dot
            ctx.beginPath(); ctx.arc(sx, syMid, 2.5, 0, Math.PI * 2); ctx.fill();
        }
    } else {
        // Line plot
        ctx.strokeStyle = opts.color || '#3b82f6';
        ctx.lineWidth = 1.2;
        ctx.beginPath();
        ctx.moveTo(toSx(opts.xs[0]), toSy(opts.ys[0]));
        for (let i = 1; i < n; i++) {
            ctx.lineTo(toSx(opts.xs[i]), toSy(opts.ys[i]));
        }
        ctx.stroke();
    }
}

/** Generate ~count nice tick values spanning [lo, hi]. */
function _niceRange(lo, hi, count) {
    const range = hi - lo;
    if (range <= 0) return [lo];
    const rough = range / count;
    const mag = Math.pow(10, Math.floor(Math.log10(rough)));
    let step;
    const r = rough / mag;
    if (r < 1.5)      step = mag;
    else if (r < 3.5)  step = 2 * mag;
    else if (r < 7.5)  step = 5 * mag;
    else               step = 10 * mag;

    const ticks = [];
    let v = Math.ceil(lo / step) * step;
    while (v <= hi + step * 0.01) {
        ticks.push(parseFloat(v.toPrecision(12)));
        v += step;
    }
    return ticks;
}


// ═════════════════════════════════════════════════════════════════════
//  PLOT RENDERING
// ═════════════════════════════════════════════════════════════════════

function renderHVMonPlots() {
    const name = hvmonSelected;
    const infoEl = document.getElementById('hvmon-info');
    if (!name || !hvmonModules[name]) {
        // Clear plots
        const fc = document.getElementById('hvmon-fast-canvas');
        const rc = document.getElementById('hvmon-ring-canvas');
        if (fc) hvmonPlot(fc, { n: 0 });
        if (rc) hvmonPlot(rc, { n: 0 });
        return;
    }

    const mon = hvmonModules[name];

    // Info bar
    const n = mon.fastN;
    const cur = n > 0 ? mon.fastV[n - 1] : NaN;
    let meanF = NaN, stdF = NaN;
    if (n > 0) {
        let s = 0, s2 = 0;
        for (let i = 0; i < n; i++) { s += mon.fastV[i]; s2 += mon.fastV[i]*mon.fastV[i]; }
        meanF = s / n;
        stdF = n >= 2 ? Math.sqrt(Math.max(0, s2/n - meanF*meanF)) : 0;
    }
    const t0str = mon.fastT0 ? new Date(mon.fastT0).toLocaleTimeString() : '\u2014';
    if (infoEl) {
        infoEl.innerHTML =
            `<b>${name}</b> &nbsp; ` +
            `VMon=${isNaN(cur)?'\u2014':cur.toFixed(3)}&thinsp;V &nbsp; ` +
            `mean=${isNaN(meanF)?'\u2014':meanF.toFixed(3)}&thinsp;V &nbsp; ` +
            `\u03c3=${isNaN(stdF)?'\u2014':stdF.toFixed(4)}&thinsp;V &nbsp; ` +
            `fast=${n}/${FAST_N} &nbsp; ring=${mon.ringN}/${RING_N} &nbsp; ` +
            `<span style="color:#64748b">start ${t0str}</span>`;
    }

    // Fast buffer plot
    const fc = document.getElementById('hvmon-fast-canvas');
    if (fc && n > 0) {
        hvmonPlot(fc, {
            xs: mon.fastTs, ys: mon.fastV, n,
            xLabel: 'Relative time [ms]', yLabel: 'VMon [V]',
            color: '#3b82f6',
        });
    } else if (fc) {
        hvmonPlot(fc, { n: 0 });
    }

    // Ring buffer plot (error bars)
    const rc = document.getElementById('hvmon-ring-canvas');
    const ring = mon.ringOrdered();
    if (rc && ring.ts.length > 0) {
        const rn = ring.ts.length;
        const yLo = new Float32Array(rn), yHi = new Float32Array(rn);
        for (let i = 0; i < rn; i++) {
            yLo[i] = ring.mean[i] - ring.rms[i];
            yHi[i] = ring.mean[i] + ring.rms[i];
        }
        hvmonPlot(rc, {
            xs: ring.ts, ys: ring.mean, yLo, yHi, n: rn,
            xLabel: 'Relative time [ms]', yLabel: 'VMon [V]',
            color: '#22c55e', errorbar: true,
        });
    } else if (rc) {
        hvmonPlot(rc, { n: 0 });
    }
}


// ═════════════════════════════════════════════════════════════════════
//  INIT  (called once on page load, reads config from gui_config)
// ═════════════════════════════════════════════════════════════════════

(function() {
    // Read hvMonitor config when init data arrives
    if (typeof hvMonitor !== 'undefined' && hvMonitor.getGuiConfig) {
        // Will be called after bootstrap
        const origGetGuiConfig = hvMonitor.getGuiConfig;
        // Monkey-patch not needed — we read from the global `GV` / config
    }

    // Deferred init: wait for MODULES to be available, then read config
    const _checkReady = setInterval(() => {
        if (typeof MODULES !== 'undefined' && MODULES.length > 0) {
            clearInterval(_checkReady);
            // Read config if available (gui_config is loaded by monitor.js)
            if (typeof window._guiConfig !== 'undefined' && window._guiConfig.hvMonitor) {
                const hm = window._guiConfig.hvMonitor;
                if (hm.fast_buffer_size) FAST_N = hm.fast_buffer_size;
                if (hm.ring_buffer_size) RING_N = hm.ring_buffer_size;
                if (hm.min_sigma_points) MIN_SIGMA_PTS = hm.min_sigma_points;
            }
        }
    }, 500);
})();
