// ═════════════════════════════════════════════════════════════════════
//  HV MONITOR TAB  —  dV stability monitoring (dV = VMon - V0Set)
//
//  Client side
//    FAST buffer  (per module, 1000 samples sliding FIFO): fed at every
//                  VMon-snapshot broadcast (~5 Hz).  Powers the geo
//                  sigma/mean colour map and the fast plot on the right.
//  Server side
//    AGGREGATION  (per module, 100 points × 5000-sample flush): the
//                  daemon keeps a running ring of {epoch_ms, mean, rms}
//                  and pushes deltas over the WebSocket.  The Aggregated
//                  plot on the right sources from this store so history
//                  survives a browser reload (it's still re-seeded from
//                  the daemon's in-memory ring, which restarts with the
//                  daemon; raw samples go to the VMDF recorder).
//
//  Power-off handling:
//    - Client: `onHVMonVMonData` skips off channels and resets the fast
//      ring so sigma/mean start clean on the next ON epoch.
//    - Server: same policy, enforced in HVAggregator::push.
// ═════════════════════════════════════════════════════════════════════

// ── Config (overridden from gui_config.hvMonitor on init) ───────────
let FAST_N  = 1000;
let MIN_SIGMA_PTS = 20;

// ── Per-module fast sample store ────────────────────────────────────
class ModuleMonitor {
    constructor() {
        this.fastTs = new Float32Array(FAST_N);
        this.fastV  = new Float32Array(FAST_N);
        this.head   = 0;              // next write index
        this.count  = 0;              // valid entries (≤ FAST_N)
        this.fastT0 = null;           // epoch-ms of first still-held sample
    }

    push(epochMs, dv) {
        if (this.count === 0) this.fastT0 = epochMs;
        this.fastTs[this.head] = epochMs - this.fastT0;
        this.fastV[this.head]  = dv;
        this.head = (this.head + 1) % FAST_N;
        if (this.count < FAST_N) this.count++;
    }

    resetFast() {
        this.head = 0; this.count = 0; this.fastT0 = null;
    }

    /** Running stats of the fast buffer — null if too few samples. */
    stats() {
        const n = this.count;
        if (n < MIN_SIGMA_PTS) return null;
        let sum = 0, sumSq = 0;
        for (let i = 0; i < n; i++) {
            const v = this.fastV[i];
            sum   += v;
            sumSq += v * v;
        }
        const mean  = sum / n;
        const sigma = Math.sqrt(Math.max(0, sumSq / n - mean * mean));
        return { n, mean, sigma };
    }

    /** Samples in chronological order (oldest first). */
    fastOrdered() {
        const n = this.count;
        if (n === 0) return { ts: new Float32Array(0), v: new Float32Array(0), n: 0 };
        if (n < FAST_N) {
            return { ts: this.fastTs.subarray(0, n),
                     v:  this.fastV.subarray(0, n),
                     n };
        }
        // Wrapped: reorder from head.
        const ts = new Float32Array(n);
        const v  = new Float32Array(n);
        for (let i = 0; i < n; i++) {
            const j = (this.head + i) % FAST_N;
            ts[i] = this.fastTs[j];
            v[i]  = this.fastV[j];
        }
        return { ts, v, n };
    }
}

const hvmonModules = {};       // name -> ModuleMonitor
const hvmonAgg     = {};       // name -> [{t,m,r,k}, ...]  (server-pushed)
let   hvmonSelected = null;
let   _hvmView     = null;

// Per-mode range overrides (user-editable, persist across mode switches)
const _hvmRanges = {
    sigma: [0, 0.05],
    mean:  [-0.05, 0.05],
};

function _hvmMode() {
    const el = document.getElementById('hvmon-color-mode');
    return el ? el.value : 'sigma';
}

// ── Data entry points ───────────────────────────────────────────────

// Fast VMon snapshot (called from monitor.js on every hv_vmon_snapshot).
// V0Set comes from the slow hv_snapshot and is cached as ch.vset.
function onHVMonVMonData(data, ts) {
    const chMap = (typeof chByName !== 'undefined') ? chByName : null;
    for (const entry of data) {
        const ch = chMap ? chMap[entry.n] : null;
        if (!ch || ch.vset == null) continue;     // wait for V0Set
        let mon = hvmonModules[entry.n];
        if (!mon) { mon = new ModuleMonitor(); hvmonModules[entry.n] = mon; }
        if (!ch.on) { mon.resetFast(); continue; }
        mon.push(ts, entry.v - ch.vset);
    }
}

// Initial full aggregation history (called once on connect).
function onHVMonAggHistory(history) {
    for (const entry of history) {
        if (!entry || !Array.isArray(entry.p)) continue;
        hvmonAgg[entry.n] = entry.p.slice();   // shallow copy
    }
}

// Incremental aggregation deltas (called whenever the daemon flushes).
function onHVMonAggUpdate(delta) {
    if (!Array.isArray(delta)) return;
    for (const entry of delta) {
        if (!entry || !Array.isArray(entry.p) || entry.p.length === 0) continue;
        const arr = hvmonAgg[entry.n] || (hvmonAgg[entry.n] = []);
        for (const pt of entry.p) arr.push(pt);
        // Defensive cap — server enforces its own ring_n, but this prevents
        // runaway growth if the daemon is reconfigured to a huge ring.
        if (arr.length > 500) arr.splice(0, arr.length - 500);
    }
}

// ── Tab render ──────────────────────────────────────────────────────
let _hvmLastGeoRender  = 0;
let _hvmLastPlotRender = 0;

function renderHVMonTab() {
    const now = performance.now();
    if (now - _hvmLastGeoRender > 900) {
        if (_hvmView) _hvmView.render();
        _hvmLastGeoRender = now;
    }
    if (now - _hvmLastPlotRender > 250) {
        renderHVMonPlots();
        _hvmLastPlotRender = now;
    }
}

// ═════════════════════════════════════════════════════════════════════
//  GEO VIEW  (uses the shared GeoView class)
// ═════════════════════════════════════════════════════════════════════

function initHVMonGeo() {
    if (_hvmView) { _hvmView.fit(); _hvmView.render(); return; }

    const canvas = document.getElementById('hvmon-geo-canvas');
    const wrap   = document.getElementById('hvmon-geo-wrap');

    _hvmView = new GeoView({
        canvas, wrap,
        extent: (typeof GV !== 'undefined' && GV.extent) ? GV.extent : 600,
        getModules: () => (typeof MODULES !== 'undefined') ? MODULES : [],
        moduleFilter:     m => m.t !== 'booster',
        includeInHitTest: m => m.t !== 'booster',
        backgroundFill: '#0e1118',
        colorFor: _hvmColorFor,
        drawAfterModule: _hvmDrawSelection,
        rangeLabel: 'V',
        defaultRange: _hvmRanges.sigma,
        legendBar:    document.getElementById('hvmon-leg-bar'),
        legendLo:     document.getElementById('hvmon-leg-lo'),
        legendHi:     document.getElementById('hvmon-leg-hi'),
        rangeMinBtn:  document.getElementById('hvmon-range-min-btn'),
        rangeMinEdit: document.getElementById('hvmon-range-min-edit'),
        rangeMaxBtn:  document.getElementById('hvmon-range-max-btn'),
        rangeMaxEdit: document.getElementById('hvmon-range-max-edit'),
        onClick: (mod) => {
            hvmonSelected = mod.n;
            _hvmView.render();
            renderHVMonPlots();
        },
        onRangeChange: (lo, hi) => {
            _hvmRanges[_hvmMode()] = [lo, hi];
        },
    });

    document.getElementById('hvmon-color-mode').addEventListener('change', () => {
        const mode = _hvmMode();
        const [lo, hi] = _hvmRanges[mode];
        _hvmView.setRange(lo, hi);
    });

    document.getElementById('hvmon-autoscale').addEventListener('click', _hvmAutoscale);
}

function _hvmColorFor(m) {
    const mon = hvmonModules[m.n];
    const st  = mon ? mon.stats() : null;
    if (!st) return '#23263a';
    const mode = _hvmMode();
    const v = (mode === 'mean') ? st.mean : st.sigma;
    const [lo, hi] = [_hvmView.rangeLo(), _hvmView.rangeHi()];
    if (hi === lo) return _hvmView.palette(0.5);
    const t = Math.min(1, Math.max(0, (v - lo) / (hi - lo)));
    return _hvmView.palette(t);
}

function _hvmDrawSelection(ctx, m) {
    if (!hvmonSelected || hvmonSelected !== m.n) return;
    const t = _hvmView.worldTransform();
    const hw = m.sx / 2 - 0.4, hh = m.sy / 2 - 0.4;
    ctx.strokeStyle = '#fff';
    ctx.lineWidth = 2 / t.scale;
    ctx.strokeRect(m.x - hw, m.y - hh, hw * 2, hh * 2);
}

function _hvmAutoscale() {
    const mode = _hvmMode();
    const vals = [];
    for (const name in hvmonModules) {
        const st = hvmonModules[name].stats();
        if (!st) continue;
        vals.push(mode === 'mean' ? st.mean : st.sigma);
    }
    if (vals.length === 0) return;
    vals.sort((a, b) => a - b);
    const pct = q => vals[Math.max(0, Math.min(vals.length - 1,
                          Math.floor(q * (vals.length - 1))))];
    let lo, hi;
    if (mode === 'sigma') {
        lo = 0;
        hi = Math.max(pct(0.98), 1e-6);
    } else {
        lo = pct(0.02);
        hi = pct(0.98);
        if (hi - lo < 1e-6) { lo -= 0.5e-6; hi += 0.5e-6; }
    }
    _hvmRanges[mode] = [lo, hi];
    _hvmView.setRange(lo, hi);
}


// ═════════════════════════════════════════════════════════════════════
//  CANVAS PLOT HELPER (unchanged)
// ═════════════════════════════════════════════════════════════════════

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

    ctx.fillStyle = '#0e1118';
    ctx.fillRect(0, 0, W, H);

    const n = opts.n || 0;
    if (n < 1) {
        ctx.fillStyle = '#64748b';
        ctx.font = '12px JetBrains Mono, monospace';
        ctx.textAlign = 'center';
        ctx.fillText('No data', W/2, H/2);
        return;
    }

    const ml = 64, mr = 16, mt = 8, mb = 32;
    const pw = W - ml - mr, ph = H - mt - mb;
    if (pw < 20 || ph < 20) return;

    let xMin, xMax;
    if (opts.xRange) {
        xMin = opts.xRange[0]; xMax = opts.xRange[1];
    } else {
        xMin = opts.xs[0]; xMax = opts.xs[0];
        for (let i = 0; i < n; i++) {
            if (opts.xs[i] < xMin) xMin = opts.xs[i];
            if (opts.xs[i] > xMax) xMax = opts.xs[i];
        }
        if (xMax === xMin) xMax = xMin + 1;
    }

    let yMin, yMax;
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

    let yPad = (yMax - yMin) * 0.08;
    if (yPad < 0.001) yPad = 0.01;
    yMin -= yPad; yMax += yPad;

    const xScale = pw / (xMax - xMin);
    const yScale = ph / (yMax - yMin);
    const toSx = x => ml + (x - xMin) * xScale;
    const toSy = y => mt + ph - (y - yMin) * yScale;

    ctx.strokeStyle = 'rgba(148,163,184,0.12)';
    ctx.lineWidth = 0.5;
    ctx.fillStyle = '#94a3b8';
    ctx.font = '9px JetBrains Mono, monospace';

    const yTicks = _niceRange(yMin, yMax, 5);
    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    for (const v of yTicks) {
        const sy = toSy(v);
        ctx.beginPath(); ctx.moveTo(ml, sy); ctx.lineTo(ml + pw, sy); ctx.stroke();
        ctx.fillText(v.toPrecision(6).replace(/\.?0+$/, ''), ml - 4, sy);
    }

    const xTicks = _niceRange(xMin, xMax, 6);
    ctx.textAlign = 'center';
    ctx.textBaseline = 'top';
    for (const v of xTicks) {
        const sx = toSx(v);
        ctx.beginPath(); ctx.moveTo(sx, mt); ctx.lineTo(sx, mt + ph); ctx.stroke();
        const label = opts.xFormat ? opts.xFormat(v)
                    : (xMax - xMin) > 10000 ? (v / 1000).toFixed(1) + 's'
                    : v.toFixed(0);
        ctx.fillText(label, sx, mt + ph + 4);
    }

    ctx.strokeStyle = 'rgba(148,163,184,0.3)';
    ctx.lineWidth = 1;
    ctx.strokeRect(ml, mt, pw, ph);

    ctx.fillStyle = '#94a3b8';
    ctx.font = '10px JetBrains Mono, monospace';
    ctx.textAlign = 'center';
    ctx.fillText(opts.xLabel || '', ml + pw / 2, H - 2);
    ctx.save();
    ctx.translate(12, mt + ph / 2);
    ctx.rotate(-Math.PI / 2);
    ctx.fillText(opts.yLabel || '', 0, 0);
    ctx.restore();

    if (opts.errorbar) {
        ctx.strokeStyle = opts.color || '#3b82f6';
        ctx.fillStyle   = opts.color || '#3b82f6';
        ctx.lineWidth = 1;
        const capW = Math.max(2, Math.min(6, pw / n * 0.3));
        for (let i = 0; i < n; i++) {
            const sx = toSx(opts.xs[i]);
            const syLo = toSy(opts.yLo[i]);
            const syHi = toSy(opts.yHi[i]);
            const syMid = toSy(opts.ys[i]);
            ctx.beginPath(); ctx.moveTo(sx, syLo); ctx.lineTo(sx, syHi); ctx.stroke();
            ctx.beginPath(); ctx.moveTo(sx - capW, syLo); ctx.lineTo(sx + capW, syLo); ctx.stroke();
            ctx.beginPath(); ctx.moveTo(sx - capW, syHi); ctx.lineTo(sx + capW, syHi); ctx.stroke();
            ctx.beginPath(); ctx.arc(sx, syMid, 2.5, 0, Math.PI * 2); ctx.fill();
        }
    } else {
        ctx.strokeStyle = opts.color || '#3b82f6';
        ctx.fillStyle   = opts.color || '#3b82f6';
        if (n === 1) {
            ctx.beginPath();
            ctx.arc(toSx(opts.xs[0]), toSy(opts.ys[0]), 2.5, 0, Math.PI * 2);
            ctx.fill();
        } else {
            ctx.lineWidth = 1.2;
            ctx.beginPath();
            ctx.moveTo(toSx(opts.xs[0]), toSy(opts.ys[0]));
            for (let i = 1; i < n; i++) {
                ctx.lineTo(toSx(opts.xs[i]), toSy(opts.ys[i]));
            }
            ctx.stroke();
        }
    }
}

function _niceRange(lo, hi, count) {
    const range = hi - lo;
    if (range <= 0) return [lo];
    const rough = range / count;
    const mag = Math.pow(10, Math.floor(Math.log10(rough)));
    let step;
    const r = rough / mag;
    if (r < 1.5)       step = mag;
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
        const fc = document.getElementById('hvmon-fast-canvas');
        const rc = document.getElementById('hvmon-ring-canvas');
        if (fc) hvmonPlot(fc, { n: 0 });
        if (rc) hvmonPlot(rc, { n: 0 });
        return;
    }

    const mon = hvmonModules[name];
    const n = mon.count;
    const fast = mon.fastOrdered();

    let cur = NaN, meanF = NaN, stdF = NaN;
    if (n > 0) {
        cur = fast.v[fast.n - 1];
        let s = 0, s2 = 0;
        for (let i = 0; i < fast.n; i++) { s += fast.v[i]; s2 += fast.v[i]*fast.v[i]; }
        meanF = s / fast.n;
        stdF = fast.n >= 2 ? Math.sqrt(Math.max(0, s2/fast.n - meanF*meanF)) : 0;
    }

    const t0str = mon.fastT0 ? new Date(mon.fastT0).toLocaleTimeString() : '\u2014';
    const aggArr = hvmonAgg[name] || [];
    if (infoEl) {
        infoEl.innerHTML =
            `<b>${name}</b> &nbsp; ` +
            `dV=${isNaN(cur)?'\u2014':cur.toFixed(4)}&thinsp;V &nbsp; ` +
            `mean=${isNaN(meanF)?'\u2014':meanF.toFixed(4)}&thinsp;V &nbsp; ` +
            `\u03c3=${isNaN(stdF)?'\u2014':stdF.toFixed(4)}&thinsp;V &nbsp; ` +
            `fast=${n}/${FAST_N} &nbsp; agg=${aggArr.length} &nbsp; ` +
            `<span style="color:#64748b">oldest sample ${t0str}</span>`;
    }

    // Fast plot (line): samples in chronological order.
    const fc = document.getElementById('hvmon-fast-canvas');
    if (fc && fast.n > 0) {
        hvmonPlot(fc, {
            xs: fast.ts, ys: fast.v, n: fast.n,
            xLabel: 'Relative time [ms]', yLabel: 'dV [V]',
            color: '#3b82f6',
        });
    } else if (fc) {
        hvmonPlot(fc, { n: 0 });
    }

    // Aggregated plot: x-axis is time before now, always ending at 0.
    // Unit (h / m / s) picked from the oldest point's age so ticks stay
    // readable as the buffer grows.
    const rc = document.getElementById('hvmon-ring-canvas');
    if (rc && aggArr.length > 0) {
        const rn = aggArr.length;
        const now = Date.now();
        const oldestAgeS = Math.max(1, (now - aggArr[0].t) / 1000);
        let unit, divisor;
        if (oldestAgeS >= 3600)      { unit = 'h'; divisor = 3600000; }
        else if (oldestAgeS >= 60)   { unit = 'm'; divisor = 60000; }
        else                          { unit = 's'; divisor = 1000; }

        const xs  = new Float32Array(rn);
        const ys  = new Float32Array(rn);
        const yLo = new Float32Array(rn), yHi = new Float32Array(rn);
        for (let i = 0; i < rn; i++) {
            xs[i]  = (aggArr[i].t - now) / divisor;
            ys[i]  = aggArr[i].m;
            yLo[i] = aggArr[i].m - aggArr[i].r;
            yHi[i] = aggArr[i].m + aggArr[i].r;
        }

        const xLo = xs[0];
        const xPad = (0 - xLo) * 0.02;
        const xFormat = v => {
            if (Math.abs(v) < 0.005) return '0';
            const av = Math.abs(v);
            const s = av >= 10 ? av.toFixed(0) : av.toFixed(1);
            return '\u2212' + s + unit;
        };

        hvmonPlot(rc, {
            xs, ys, yLo, yHi, n: rn,
            xLabel: 'time before now', yLabel: 'dV [V]',
            color: '#22c55e', errorbar: true,
            xRange: [xLo - xPad, 0],
            xFormat,
        });
    } else if (rc) {
        hvmonPlot(rc, { n: 0 });
    }
}


// ═════════════════════════════════════════════════════════════════════
//  INIT  (called once on page load, reads config from gui_config)
// ═════════════════════════════════════════════════════════════════════

(function() {
    const _checkReady = setInterval(() => {
        if (typeof MODULES !== 'undefined' && MODULES.length > 0) {
            clearInterval(_checkReady);
            if (typeof window._guiConfig !== 'undefined' && window._guiConfig.hvMonitor) {
                const hm = window._guiConfig.hvMonitor;
                if (hm.fast_buffer_size) FAST_N = hm.fast_buffer_size;
                if (hm.min_sigma_points) MIN_SIGMA_PTS = hm.min_sigma_points;
            }
        }
    }, 500);
})();
