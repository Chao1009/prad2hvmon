// ═════════════════════════════════════════════════════════════════════
//  GeoView — shared canvas viewport for the HyCal geometry displays.
//
//  Both the HyCal Geometry tab and the HV Monitor tab draw the same
//  ~1700 detector modules on a pan/zoom canvas.  The bits that are
//  common — DPR-aware sizing, pan/zoom/wheel/drag plumbing, hit-test,
//  double-click-to-fit, and the palette + range-edit UI — all live
//  here.  Each tab supplies only the parts that actually differ: a
//  `colorFor(mod)` callback, click / hover handlers, and optional
//  per-module / overlay drawing hooks.
//
//  The class owns its own transform, palette index and [lo, hi] range,
//  so two independent GeoView instances can coexist on the page with
//  different states.
// ═════════════════════════════════════════════════════════════════════

// ── Shared palette definitions (used by both tabs) ──────────────────
function _lerpHex(a, b, t) {
    const ar = parseInt(a.slice(1,3),16), ag = parseInt(a.slice(3,5),16), ab = parseInt(a.slice(5,7),16);
    const br = parseInt(b.slice(1,3),16), bg = parseInt(b.slice(3,5),16), bb = parseInt(b.slice(5,7),16);
    return `rgb(${Math.round(ar+(br-ar)*t)},${Math.round(ag+(bg-ag)*t)},${Math.round(ab+(bb-ab)*t)})`;
}
const GEO_PALETTES = {
    rainbow(t) {
        if (t < 0.25) return _lerpHex('#1e3a5f', '#3b82f6', t * 4);
        if (t < 0.50) return _lerpHex('#3b82f6', '#2dd4a0', (t - 0.25) * 4);
        if (t < 0.75) return _lerpHex('#2dd4a0', '#eab308', (t - 0.50) * 4);
        return _lerpHex('#eab308', '#f56565', (t - 0.75) * 4);
    },
    darkblue(t) {
        if (t < 0.5) return _lerpHex('#0b1628', '#3b9eff', t * 2);
        return _lerpHex('#3b9eff', '#eab308', (t - 0.5) * 2);
    },
    viridis(t) {
        return `rgb(${Math.round(255*Math.min(1,Math.max(0,-0.87+4.26*t-4.85*t*t+2.5*t*t*t)))},${Math.round(255*Math.min(1,Math.max(0,-0.03+0.77*t+1.32*t*t-1.87*t*t*t)))},${Math.round(255*Math.min(1,Math.max(0,0.33+1.74*t-4.26*t*t+3.17*t*t*t)))})`;
    },
    inferno(t) {
        return `rgb(${Math.round(255*Math.min(1,Math.max(0,-0.02+2.16*t+4.79*t*t-8.13*t*t*t+2.17*t*t*t*t)))},${Math.round(255*Math.min(1,Math.max(0,-0.02-0.35*t+5.87*t*t-8.29*t*t*t+3.7*t*t*t*t)))},${Math.round(255*Math.min(1,Math.max(0,0.01+3.1*t-9.34*t*t+12.45*t*t*t-5.24*t*t*t*t)))})`;
    },
    coolwarm(t) {
        const r = Math.round(255*Math.min(1,Math.max(0, 0.23+2.22*t-1.83*t*t)));
        const g = Math.round(255*Math.min(1,Math.max(0, 0.30+1.58*t-2.36*t*t+0.56*t*t*t)));
        const b = Math.round(255*Math.min(1,Math.max(0, 0.75-0.44*t-0.81*t*t+0.53*t*t*t)));
        return `rgb(${r},${g},${b})`;
    },
    hot(t) {
        const r = Math.round(255*Math.min(1, t*2.8));
        const g = Math.round(255*Math.max(0, Math.min(1, (t-0.35)*2.8)));
        const b = Math.round(255*Math.max(0, Math.min(1, (t-0.7)*3.3)));
        return `rgb(${r},${g},${b})`;
    },
    jet(t) {
        t = 0.125 + t * 0.75;
        const r = Math.round(255*Math.min(1, Math.max(0, 1.5-Math.abs(t-0.75)*4)));
        const g = Math.round(255*Math.min(1, Math.max(0, 1.5-Math.abs(t-0.5)*4)));
        const b = Math.round(255*Math.min(1, Math.max(0, 1.5-Math.abs(t-0.25)*4)));
        return `rgb(${r},${g},${b})`;
    },
    greyscale(t) {
        const v = Math.round(255*t);
        return `rgb(${v},${v},${v})`;
    },
};
const GEO_PALETTE_NAMES = Object.keys(GEO_PALETTES);


class GeoView {
    constructor(opts) {
        // Required
        this.canvas        = opts.canvas;
        this.wrap          = opts.wrap;
        this.ctx           = this.canvas.getContext('2d');
        this.getModules    = opts.getModules;
        this.colorFor      = opts.colorFor;
        // Optional
        this.extent        = opts.extent ?? 600;
        this.moduleFilter  = opts.moduleFilter  ?? (() => true);
        this.includeInHitTest = opts.includeInHitTest ?? this.moduleFilter;
        this.onClick       = opts.onClick       ?? null;
        this.onHoverChange = opts.onHoverChange ?? null;
        this.onLeave       = opts.onLeave       ?? null;
        this.drawAfterModule = opts.drawAfterModule ?? null;
        this.drawOverlay   = opts.drawOverlay   ?? null;
        this.backgroundFill = opts.backgroundFill ?? null; // null = no clear beyond clearRect
        this.rangeLabel    = opts.rangeLabel    ?? '';
        this.onPaletteChange = opts.onPaletteChange ?? null;
        this.onRangeChange   = opts.onRangeChange   ?? null;

        // Transform / state
        this._transform = { x: 0, y: 0, scale: 1 };
        this._drag      = null;
        this._downPos   = null;
        this._hoverName = null;
        this._pendingHoverEvt = null;

        // Palette / range
        this._paletteIdx = opts.initialPaletteIdx ?? 0;
        const initR = opts.defaultRange ?? [0, 1];
        this._range = [initR[0], initR[1]];

        // Optional legend DOM — all pieces may be null
        this._legendBar  = opts.legendBar  ?? null;
        this._legendLo   = opts.legendLo   ?? null;
        this._legendHi   = opts.legendHi   ?? null;
        this._rangeMinBtn  = opts.rangeMinBtn  ?? null;
        this._rangeMinEdit = opts.rangeMinEdit ?? null;
        this._rangeMaxBtn  = opts.rangeMaxBtn  ?? null;
        this._rangeMaxEdit = opts.rangeMaxEdit ?? null;

        this._setupEvents();
        this._setupRangeEdit();
        this._resizeCanvas();
        this.fit();
        this.drawLegend();

        // Re-render on container resize
        new ResizeObserver(() => {
            this._resizeCanvas();
            this.render();
        }).observe(this.wrap);
    }

    // ── View transforms ─────────────────────────────────────────────
    fit() {
        const rect = this.wrap.getBoundingClientRect();
        if (!rect.width || !rect.height) return;
        const sx = rect.width  / (2 * this.extent);
        const sy = rect.height / (2 * this.extent);
        this._transform.scale = Math.min(sx, sy) * 0.92;
        this._transform.x = rect.width  / 2;
        this._transform.y = rect.height / 2;
    }

    worldTransform() { return { ...this._transform }; }
    hoveredName()    { return this._hoverName; }

    // ── Palette / range ─────────────────────────────────────────────
    palette(t) {
        t = Math.max(0, Math.min(1, t));
        return GEO_PALETTES[GEO_PALETTE_NAMES[this._paletteIdx]](t);
    }
    currentPaletteName() { return GEO_PALETTE_NAMES[this._paletteIdx]; }
    paletteIdx() { return this._paletteIdx; }

    setPalette(idx) {
        this._paletteIdx = ((idx % GEO_PALETTE_NAMES.length) + GEO_PALETTE_NAMES.length)
                           % GEO_PALETTE_NAMES.length;
        this.drawLegend();
        this.render();
        if (this.onPaletteChange) this.onPaletteChange(this._paletteIdx, this.currentPaletteName());
    }
    nextPalette() { this.setPalette(this._paletteIdx + 1); }

    rangeLo() { return this._range[0]; }
    rangeHi() { return this._range[1]; }
    // Programmatic set — does NOT fire onRangeChange (that's reserved for
    // user-initiated edits via the pencil UI).  Callers that want to sync
    // an external store with a programmatic change should do so manually.
    setRange(lo, hi) {
        this._range = [lo, hi];
        this.drawLegend();
        this.render();
    }

    drawLegend() {
        if (this._legendBar) {
            const c = this._legendBar;
            const ctx = c.getContext('2d');
            const w = c.width, h = c.height;
            ctx.clearRect(0, 0, w, h);
            for (let i = 0; i < w; i++) {
                ctx.fillStyle = this.palette(i / w);
                ctx.fillRect(i, 0, 1, h);
            }
            c.title = this.currentPaletteName() + ' (click to change)';
        }
        if (this._legendLo) this._legendLo.textContent = this._fmtValue(this._range[0]);
        if (this._legendHi) this._legendHi.textContent = this._fmtValue(this._range[1]);
    }

    _fmtValue(v) {
        if (v == null || isNaN(v)) return '—';
        const abs = Math.abs(v);
        const s = abs === 0 ? '0'
                : abs >= 1  ? v.toFixed(2)
                : abs >= 0.01 ? v.toFixed(3)
                : v.toExponential(1);
        return s + (this.rangeLabel ? ' ' + this.rangeLabel : '');
    }

    // ── Event wiring ────────────────────────────────────────────────
    _setupEvents() {
        const self = this;

        this.wrap.addEventListener('mousedown', e => {
            if (e.button !== 0) return;
            this._downPos = { x: e.clientX, y: e.clientY };
            this._drag = { sx: e.clientX, sy: e.clientY,
                           ox: this._transform.x, oy: this._transform.y };
        });

        window.addEventListener('mousemove', e => {
            if (self._drag) {
                self._transform.x = self._drag.ox + (e.clientX - self._drag.sx);
                self._transform.y = self._drag.oy + (e.clientY - self._drag.sy);
                self.render();
            }
            self._pendingHoverEvt = e;
        });
        // Throttle hover handling to one rAF frame — avoids re-testing all
        // modules on every raw mousemove event (can fire 200+ times/s).
        (function hoverLoop() {
            if (self._pendingHoverEvt) {
                self._processHover(self._pendingHoverEvt);
                self._pendingHoverEvt = null;
            }
            requestAnimationFrame(hoverLoop);
        })();

        window.addEventListener('mouseup', () => { this._drag = null; });

        this.wrap.addEventListener('wheel', e => {
            e.preventDefault();
            const rect = this.wrap.getBoundingClientRect();
            const mx = e.clientX - rect.left;
            const my = e.clientY - rect.top;
            const factor = e.deltaY < 0 ? 1.12 : 1 / 1.12;
            const ns = Math.max(0.05, Math.min(50, this._transform.scale * factor));
            this._transform.x = mx - (mx - this._transform.x) * (ns / this._transform.scale);
            this._transform.y = my - (my - this._transform.y) * (ns / this._transform.scale);
            this._transform.scale = ns;
            this.render();
        }, { passive: false });

        this.wrap.addEventListener('click', e => {
            // Distinguish click from drag (4-pixel threshold)
            if (this._downPos &&
                (Math.abs(e.clientX - this._downPos.x) > 4 ||
                 Math.abs(e.clientY - this._downPos.y) > 4)) return;
            const mod = this.hitTest(e);
            if (mod && this.onClick) this.onClick(mod, e);
        });

        this.wrap.addEventListener('dblclick', () => {
            this.fit();
            this.render();
        });

        this.wrap.addEventListener('mouseleave', () => {
            if (this._hoverName) {
                this._hoverName = null;
                if (this.onHoverChange) this.onHoverChange(null, null);
                if (!this._drag) this.render();
            }
            if (this.onLeave) this.onLeave();
        });
    }

    _setupRangeEdit() {
        const wire = (btn, edit, getVal, setVal) => {
            if (!btn || !edit) return;
            let editing = false;
            const start = () => {
                editing = true;
                btn.classList.add('editing'); btn.textContent = '\u2713';
                edit.classList.add('active');
                edit.value = getVal() ?? '';
                edit.focus(); edit.select();
            };
            const apply = () => {
                if (!editing) return;
                editing = false;
                btn.classList.remove('editing'); btn.textContent = '\u270E';
                edit.classList.remove('active');
                const v = parseFloat(edit.value);
                setVal(isNaN(v) ? null : v);
                this.drawLegend();
                this.render();
                if (this.onRangeChange) this.onRangeChange(this._range[0], this._range[1]);
            };
            btn.addEventListener('mousedown', e => e.preventDefault());
            btn.onclick = () => { editing ? apply() : start(); };
            edit.addEventListener('keydown', e => {
                if (e.key === 'Enter') apply();
                if (e.key === 'Escape') {
                    editing = false;
                    btn.classList.remove('editing'); btn.textContent = '\u270E';
                    edit.classList.remove('active');
                }
            });
            edit.addEventListener('blur', apply);
        };
        wire(this._rangeMinBtn, this._rangeMinEdit,
             () => this._range[0], v => { this._range[0] = v; });
        wire(this._rangeMaxBtn, this._rangeMaxEdit,
             () => this._range[1], v => { this._range[1] = v; });
        // Click-on-legend-bar cycles the palette
        if (this._legendBar) {
            this._legendBar.addEventListener('click', () => this.nextPalette());
        }
    }

    // ── Hit testing ─────────────────────────────────────────────────
    _screenToWorld(e) {
        const rect = this.wrap.getBoundingClientRect();
        const sx = e.clientX - rect.left;
        const sy = e.clientY - rect.top;
        return {
            x: (sx - this._transform.x) / this._transform.scale,
            y: -(sy - this._transform.y) / this._transform.scale, // physics coords
        };
    }

    hitTest(e) {
        const modules = this.getModules();
        if (!modules) return null;
        const w = this._screenToWorld(e);
        for (let i = modules.length - 1; i >= 0; i--) {
            const m = modules[i];
            if (!this.includeInHitTest(m)) continue;
            if (Math.abs(w.x - m.x) <= m.sx / 2 && Math.abs(w.y - m.y) <= m.sy / 2)
                return m;
        }
        return null;
    }

    _processHover(e) {
        const mod = this.hitTest(e);
        const newName = mod ? mod.n : null;
        const changed = newName !== this._hoverName;
        if (changed) {
            this._hoverName = newName;
            if (!this._drag) this.render();
        }
        // Always forward — callers may want to update tooltip position
        // even when the module under cursor is unchanged.
        if (this.onHoverChange) this.onHoverChange(mod, e);
    }

    // ── Rendering ───────────────────────────────────────────────────
    _resizeCanvas() {
        const rect = this.wrap.getBoundingClientRect();
        const dpr = devicePixelRatio;
        this.canvas.width  = rect.width  * dpr;
        this.canvas.height = rect.height * dpr;
        this.canvas.style.width  = rect.width  + 'px';
        this.canvas.style.height = rect.height + 'px';
        this.ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    }

    render() {
        if (!this.canvas.width) return;
        const ctx = this.ctx;
        const W = this.canvas.width  / devicePixelRatio;
        const H = this.canvas.height / devicePixelRatio;
        ctx.clearRect(0, 0, W, H);
        if (this.backgroundFill) {
            ctx.fillStyle = this.backgroundFill;
            ctx.fillRect(0, 0, W, H);
        }

        const modules = this.getModules();
        if (!modules || !modules.length) return;

        const t = this._transform;
        ctx.save();
        ctx.translate(t.x, t.y);
        ctx.scale(t.scale, -t.scale);  // flip Y for physics coords

        const gap = 0.4;  // mm gap between modules
        for (let i = 0; i < modules.length; i++) {
            const m = modules[i];
            if (!this.moduleFilter(m)) continue;
            const hw = m.sx / 2 - gap;
            const hh = m.sy / 2 - gap;
            ctx.fillStyle = this.colorFor(m);
            ctx.fillRect(m.x - hw, m.y - hh, hw * 2, hh * 2);
            if (this.drawAfterModule) this.drawAfterModule(ctx, m);
        }

        ctx.restore();

        if (this.drawOverlay) this.drawOverlay(ctx, this);
    }
}
