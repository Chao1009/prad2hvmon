// ═════════════════════════════════════════════════════════════════════
//  GEOMETRY MAP TAB
//
//  Thin wrapper around GeoView.  This file owns the tab-specific bits:
//    - Color-mode dropdown (VMon / VSet / ΔV / Status)
//    - Per-mode user-overridden range cache
//    - Per-module colour cache (1731 modules × several updates/s is hot)
//    - Search-highlight, booster / LMS special drawing
//    - Labels at high zoom
//    - Hover tooltip and floating popup windows
//  All viewport / palette / range-edit plumbing lives in GeoView.
// ═════════════════════════════════════════════════════════════════════

let _geoView = null;

function initGeoMap() {
    const canvas = document.getElementById('geo-canvas');
    const wrap   = document.getElementById('geo-canvas-wrap');
    _geoView = new GeoView({
        canvas, wrap,
        extent: GV.extent,
        getModules: () => MODULES,
        rangeLabel: 'V',
        colorFor: moduleColor,
        drawAfterModule: _geoDrawAfterModule,
        drawOverlay:     _geoDrawOverlay,
        onClick: (mod) => openModPopup(mod),
        onHoverChange: _geoOnHoverChange,
        // Legend / range DOM
        legendBar:    document.getElementById('leg-bar'),
        legendLo:     document.getElementById('leg-lo'),
        legendHi:     document.getElementById('leg-hi'),
        rangeMinBtn:  document.getElementById('geo-range-min-btn'),
        rangeMinEdit: document.getElementById('geo-range-min-edit'),
        rangeMaxBtn:  document.getElementById('geo-range-max-btn'),
        rangeMaxEdit: document.getElementById('geo-range-max-edit'),
        onRangeChange: (lo, hi) => {
            // Persist as the user's override for the current mode
            _geoRangeOverrides[geoColorMode()] = [lo, hi];
            rebuildColorCache();
        },
        onPaletteChange: () => rebuildColorCache(),
    });
    _applyModeRange();   // push default range for the initial mode into the view

    // Color mode — restore per-mode range (no reset; overrides persist)
    document.getElementById('geo-color-mode').addEventListener('change', () => {
        _applyModeRange();
        rebuildColorCache();
        _geoView.drawLegend();
        _geoView.render();
    });

    // Search
    document.getElementById('geo-search').addEventListener('input', e => {
        geoHighlight = e.target.value.trim().toUpperCase();
        _geoView.render();
        updateFooter();
    });

    // Reset view
    document.getElementById('geo-reset').addEventListener('click', () => {
        _geoView.fit();
        _geoView.render();
    });

    // Close all popups
    document.getElementById('geo-close-popups').addEventListener('click', () => {
        [...popups.keys()].forEach(name => closeModPopup(name));
    });

    // Trigger initial legend + render once MODULES has been loaded; the view
    // constructor drew the palette bar already, but MODULES may still be
    // empty at that point.
    _geoView.drawLegend();
}

// Legacy shim: monitor.js / other modules call renderGeo() when data changes.
function renderGeo() { if (_geoView) _geoView.render(); }

// Legacy shim: also used by monitor.js when it wants to request a hover
// refresh after a data update.
function resizeGeoCanvas() { if (_geoView) _geoView._resizeCanvas(); }
function resetGeoView() { if (_geoView) { _geoView.fit(); _geoView.render(); } }

// geoHover, geoDrag, geoTransform, geoHighlight, geoCanvas/Ctx/Wrap are all
// declared (let) in monitor.js — we just assign to them here so other
// modules (monitor_table.js, popups) can keep reading the same names.
// Range overrides persist per mode across switches.
const _geoRangeOverrides = {};   // mode -> [lo, hi]

function geoColorMode() { return document.getElementById('geo-color-mode').value; }

// Default range per mode if the user hasn't overridden it.
function _defaultRangeFor(mode) {
    if (mode === 'vmon')  return [0, CR.vmon_max];
    if (mode === 'vset')  return [0, CR.vset_max];
    if (mode === 'diff')  return [-CR.diff_max, CR.diff_max];
    return [0, 1];   // status — unused for continuous scale
}

function _applyModeRange() {
    const mode = geoColorMode();
    const saved = _geoRangeOverrides[mode];
    const [lo, hi] = saved ?? _defaultRangeFor(mode);
    _geoView.setRange(lo, hi);
}

// Effective range for the current mode (respects user override).
function geoEffectiveRange() {
    const mode = geoColorMode();
    const over = _geoRangeOverrides[mode];
    return over ?? _defaultRangeFor(mode);
}


// ═════════════════════════════════════════════════════════════════════
//  MODULE COLOUR CACHE
//
//  moduleColor() is called for all 1731 modules on every render.  Parsing
//  hex in the palette function is too expensive at that call rate — so
//  we compute each module's colour once (when data arrives, colour mode
//  changes, or palette cycles) and cache it in _colorCache.
// ═════════════════════════════════════════════════════════════════════
let _colorCache = {};
let _colorCacheMode = '';
let _colorCachePaletteIdx = -1;
var _colorCacheDirty = true;

function rebuildColorCache() {
    const mode = geoColorMode();
    _colorCacheMode = mode;
    _colorCachePaletteIdx = _geoView ? _geoView.paletteIdx() : 0;
    _colorCacheDirty = false;
    _colorCache = {};
    for (const mod of MODULES) {
        _colorCache[mod.n] = _computeModuleColor(mod, mode);
    }
}

function moduleColor(mod) {
    const mode = geoColorMode();
    const pIdx = _geoView ? _geoView.paletteIdx() : 0;
    if (_colorCacheDirty || mode !== _colorCacheMode || pIdx !== _colorCachePaletteIdx)
        rebuildColorCache();
    return _colorCache[mod.n] ?? '#222';
}

function _computeModuleColor(mod, mode) {
    // Booster modules get their colour from live TCP data, not the palette.
    if (mod.t === 'booster') {
        const bs = boosterByName[mod.n];
        if (!bs || !bs.connected) return '#1a1a2e';
        if (bs.error)             return '#7f1d1d';
        if (!bs.on)               return '#1e293b';
        if (bs.mode === 'CC')     return '#78350f';
        return '#14532d';
    }

    const ch = chByName[mod.n];

    // Search "no-match" gets dimmed out.
    if (geoHighlight && !geoModuleMatches(mod, geoHighlight))
        return 'rgba(20,28,38,0.6)';

    if (mode === 'status') {
        if (!ch) return '#222';
        const cc = ch._cc || classifyChannel(ch);
        if (cc.isFault)  return '#f56565';
        if (cc.isWarn)   return '#eab308';
        if (!ch.on)      return '#4a5568';
        return '#2dd4a0';
    }

    if (!ch) return '#222';

    const [rLo, rHi] = geoEffectiveRange();

    if (mode === 'vmon') {
        if (!ch.on) return '#333';
        const v = Math.abs(ch.vmon ?? 0);
        const t = (rHi !== rLo) ? Math.min(1, Math.max(0, (v - rLo) / (rHi - rLo))) : 0;
        return _geoView.palette(t);
    }

    if (mode === 'vset') {
        const v = Math.abs(ch.vset ?? 0);
        const t = (rHi !== rLo) ? Math.min(1, Math.max(0, (v - rLo) / (rHi - rLo))) : 0;
        return _geoView.palette(t);
    }

    // diff (signed)
    if (!ch.on) return '#333';
    if (ch.vmon == null || ch.vset == null) return '#333';
    const diff = ch.vmon - ch.vset;
    const t = (rHi !== rLo) ? Math.min(1, Math.max(0, (diff - rLo) / (rHi - rLo))) : 0.5;
    return _geoView.palette(t);
}

// Diverging scale for ΔV tooltip / popup colouring (independent of palette).
function diffColorScale(t) {
    if (t < -0.5) return lerpColor('#1e3a5f', '#3b82f6', (t + 1) * 2);
    if (t < 0)    return lerpColor('#3b82f6', '#2dd4a0', (t + 0.5) * 2);
    if (t < 0.5)  return lerpColor('#2dd4a0', '#eab308', t * 2);
    return lerpColor('#eab308', '#f56565', (t - 0.5) * 2);
}

function lerpColor(a, b, t) {
    const ar = parseInt(a.slice(1,3),16), ag = parseInt(a.slice(3,5),16), ab = parseInt(a.slice(5,7),16);
    const br = parseInt(b.slice(1,3),16), bg = parseInt(b.slice(3,5),16), bb = parseInt(b.slice(5,7),16);
    const r = Math.round(ar + (br-ar)*t), g = Math.round(ag + (bg-ag)*t), bl = Math.round(ab + (bb-ab)*t);
    return `rgb(${r},${g},${bl})`;
}


// ═════════════════════════════════════════════════════════════════════
//  PER-MODULE DECORATIONS AND OVERLAY (called from GeoView.render)
// ═════════════════════════════════════════════════════════════════════

function _geoDrawAfterModule(ctx, m) {
    const t = _geoView.worldTransform();
    const hw = m.sx / 2 - 0.4, hh = m.sy / 2 - 0.4;
    const isHover  = (geoHover === m.n);
    const isSearch = geoHighlight && geoModuleMatches(m, geoHighlight);

    // LMS virtual blocks: dashed purple border + label
    if (m.t === 'LMS') {
        ctx.save();
        ctx.strokeStyle = '#c084fc';
        ctx.lineWidth = 1.5 / t.scale;
        ctx.setLineDash([4 / t.scale, 3 / t.scale]);
        ctx.strokeRect(m.x - hw, m.y - hh, hw * 2, hh * 2);
        ctx.setLineDash([]);
        ctx.scale(1, -1);
        ctx.fillStyle = '#e8edf3';
        ctx.font = `${10 / t.scale}px monospace`;
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        ctx.fillText(m.n, m.x, -m.y);
        ctx.restore();
    }

    // Booster blocks: thicker coloured border + live two-line label
    if (m.t === 'booster') {
        const bs = boosterByName[m.n];
        const bdrCol = (!bs || !bs.connected) ? '#475569'
                     : bs.error               ? '#f87171'
                     : !bs.on                 ? '#64748b'
                     : bs.mode === 'CC'       ? '#fbbf24'
                     :                          '#4ade80';
        ctx.save();
        ctx.strokeStyle = bdrCol;
        ctx.lineWidth = 2.5 / t.scale;
        ctx.strokeRect(m.x - hw, m.y - hh, hw * 2, hh * 2);
        ctx.scale(1, -1);
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        ctx.fillStyle = '#cbd5e1';
        ctx.font = `bold ${11 / t.scale}px sans-serif`;
        ctx.fillText(m.n, m.x, -(m.y + hh * 0.3));
        const bLine2 = (!bs || !bs.connected) ? 'OFFLINE'
                     : bs.vmon != null         ? bs.vmon.toFixed(1) + ' V' + (bs.imon != null ? ' / ' + bs.imon.toFixed(3) + ' A' : '')
                     :                           (bs.on ? 'ON' : 'OFF');
        ctx.fillStyle = bdrCol;
        ctx.font = `${10 / t.scale}px monospace`;
        ctx.fillText(bLine2, m.x, -(m.y - hh * 0.3));
        ctx.restore();
    }

    if (isHover || isSearch) {
        ctx.strokeStyle = isHover ? '#fff' : '#3b9eff';
        ctx.lineWidth = 1.5 / t.scale;
        ctx.strokeRect(m.x - hw, m.y - hh, hw * 2, hh * 2);
    }
}

function _geoDrawOverlay(ctx, view) {
    const t = view.worldTransform();

    // Small + marker at world origin (drawn in world coords).
    ctx.save();
    ctx.translate(t.x, t.y);
    ctx.scale(t.scale, -t.scale);
    const markerSize = 8 / t.scale;
    ctx.strokeStyle = 'rgba(255,255,255,0.35)';
    ctx.lineWidth = 1 / t.scale;
    ctx.beginPath();
    ctx.moveTo(-markerSize, 0); ctx.lineTo(markerSize, 0);
    ctx.moveTo(0, -markerSize); ctx.lineTo(0, markerSize);
    ctx.stroke();
    ctx.restore();

    // Labels at high zoom — only modules within viewport.
    if (t.scale > 2.2) {
        ctx.save();
        ctx.translate(t.x, t.y);
        const s = t.scale;
        const fontSize = Math.max(4, Math.min(9, 6));
        ctx.font = `${fontSize / s}px JetBrains Mono, monospace`;
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        ctx.fillStyle = 'rgba(255,255,255,0.7)';
        const W = view.canvas.width / devicePixelRatio;
        const H = view.canvas.height / devicePixelRatio;
        for (const m of MODULES) {
            if (m.t === 'booster') continue;   // boosters draw their own labels above
            const sx = m.x * s, sy = -m.y * s;
            if (sx < -t.x - 100 || sx > -t.x + W + 100) continue;
            if (sy < -t.y - 100 || sy > -t.y + H + 100) continue;
            ctx.save();
            ctx.scale(s, -s);
            ctx.fillText(m.n, m.x, -m.y);
            ctx.restore();
        }
        ctx.restore();
    }
}


// ═════════════════════════════════════════════════════════════════════
//  HOVER  →  TOOLTIP
// ═════════════════════════════════════════════════════════════════════

function _geoOnHoverChange(mod, e) {
    const tooltip = document.getElementById('geo-tooltip');
    if (!mod || !e) {
        geoHover = null;
        if (tooltip) tooltip.style.display = 'none';
        return;
    }
    if (!document.getElementById('geo-tab').classList.contains('active')) return;
    geoHover = mod.n;

    const ch = chByName[mod.n];
    let html = `<div class="tt-name">${mod.n}</div>`;
    html += `<div class="tt-row"><span class="tt-label">Type</span><span class="tt-val">${mod.t}</span></div>`;

    // Booster tooltip (no HV rows)
    if (mod.t === 'booster') {
        html += `<div class="tt-row"><span class="tt-label">IP</span><span class="tt-val" style="font-family:monospace">${escHtml(mod.ip || '\u2014')}</span></div>`;
        const bs = boosterByName[mod.n];
        if (bs) {
            const connSpan = bs.connected
                ? '<span class="st-on">Connected</span>'
                : '<span class="st-off">Offline</span>';
            html += `<div class="tt-row"><span class="tt-label">Link</span><span class="tt-val">${connSpan}</span></div>`;
            if (bs.connected) {
                html += `<div class="tt-row"><span class="tt-label">VMon / VSet</span><span class="tt-val"><span class="tt-live">${bs.vmon != null ? bs.vmon.toFixed(2)+' V' : '\u2014'}</span> / ${bs.vset != null ? bs.vset.toFixed(2)+' V' : '\u2014'}</span></div>`;
                html += `<div class="tt-row"><span class="tt-label">IMon / ISet</span><span class="tt-val"><span class="tt-live">${bs.imon != null ? bs.imon.toFixed(3)+' A' : '\u2014'}</span> / ${bs.iset != null ? bs.iset.toFixed(3)+' A' : '\u2014'}</span></div>`;
                html += `<div class="tt-row"><span class="tt-label">Mode</span><span class="tt-val">${escHtml(bs.mode||'\u2014')}</span></div>`;
                html += `<div class="tt-row"><span class="tt-label">Pwr</span><span class="tt-val">${bs.on ? '<span class="st-on">ON</span>' : '<span class="st-off">OFF</span>'}</span></div>`;
                if (bs.error) html += `<div class="tt-row"><span class="tt-label">Error</span><span class="tt-val" style="color:var(--red)">${escHtml(bs.error)}</span></div>`;
            }
        } else {
            html += `<div class="tt-row"><span class="tt-label">Link</span><span class="tt-val" style="color:var(--text-dim)">waiting\u2026</span></div>`;
        }
    } else if (ch) {
        html += `<div class="tt-row"><span class="tt-label">HV</span><span class="tt-val">${ch.crate} s${ch.slot} ch${ch.channel}</span></div>`;
        html += `<div class="tt-row"><span class="tt-label">VMon / VSet</span><span class="tt-val"><span class="tt-live">${fmt(ch.vmon, 2)}</span> / ${fmt(ch.vset, 2)} V</span></div>`;
        const diff = (ch.vmon != null && ch.vset != null) ? (ch.vmon - ch.vset) : null;
        const diffColor = (diff != null && ch.on) ? diffColorScale(Math.max(-1, Math.min(1, diff / CR.diff_max))) : null;
        const diffStyle = diffColor ? (' style="color:' + diffColor + ';font-weight:600"') : '';
        const diffSign = (diff != null && diff > 0) ? '+' : '';
        html += `<div class="tt-row"><span class="tt-label">\u0394V</span><span class="tt-val"${diffStyle}>${diffSign}${fmt(diff, 2)} V</span></div>`;
        if (ch.iSupported !== false && ch.imon != null)
            html += `<div class="tt-row"><span class="tt-label">IMon</span><span class="tt-val"><span class="tt-live">${fmt(ch.imon, 3)}</span> \u00b5A</span></div>`;
        html += `<div class="tt-row"><span class="tt-label">Pwr</span><span class="tt-val">${pwrHtml(ch)}</span></div>`;
        const ttSt = (ch._cc || classifyChannel(ch)).badgesHtml;
        if (ttSt) html += `<div class="tt-row"><span class="tt-label">Status</span><span class="tt-val">${ttSt}</span></div>`;
    } else {
        html += `<div class="tt-row"><span class="tt-label">HV</span><span class="tt-val" style="color:var(--text-dim)">not linked</span></div>`;
    }

    tooltip.innerHTML = html;
    tooltip.style.display = 'block';
    tooltip.style.left = (e.clientX + 14) + 'px';
    tooltip.style.top  = (e.clientY + 14) + 'px';
}


function geoModuleMatches(mod, query) {
    const colonIdx = query.indexOf(':');
    if (colonIdx > 0) {
        const col = query.slice(0, colonIdx).trim();
        const val = query.slice(colonIdx + 1).trim();
        switch (col) {
            case 'type': case 't':    return mod.t.toUpperCase().includes(val);
            case 'name': case 'n':    return mod.n.toUpperCase().includes(val);
            case 'x':                 return String(mod.x).includes(val);
            case 'y':                 return String(mod.y).includes(val);
            default:                  return mod.n.toUpperCase().includes(query);
        }
    }
    return mod.n.toUpperCase().includes(query);
}


// ═════════════════════════════════════════════════════════════════════
//  FLOATING POPUP WINDOWS  (unchanged from the previous implementation)
// ═════════════════════════════════════════════════════════════════════
const popups = new Map();   // modName → { el, refresh }
let popupZTop = 210;

function bringToFront(el) {
    popupZTop++;
    el.style.zIndex = popupZTop;
    document.querySelectorAll('.mod-popup').forEach(p => p.classList.remove('focused'));
    el.classList.add('focused');
}

function openModPopup(mod) {
    if (popups.has(mod.n)) { bringToFront(popups.get(mod.n).el); return; }

    // Booster supplies have their own popup implementation.
    if (mod.t === 'booster') { openBoosterPopup(mod); return; }

    const ch = chByName[mod.n];

    const el = document.createElement('div');
    el.className = 'mod-popup';
    const offset = (popups.size % 8) * 26;
    el.style.left = (80 + offset) + 'px';
    el.style.top  = (80 + offset) + 'px';

    const header = document.createElement('div');
    header.className = 'popup-header';
    header.innerHTML = `<h2>${mod.n}</h2><button class="popup-close" title="Close">\u2715</button>`;
    el.appendChild(header);

    const body = document.createElement('div');
    body.className = 'popup-body';

    const grid = document.createElement('div');
    grid.className = 'popup-grid';
    body.appendChild(grid);

    const actions = document.createElement('div');
    actions.className = 'popup-actions';
    const rowV = document.createElement('div');
    rowV.className = 'popup-action-row';
    const vsetInput = document.createElement('input');
    vsetInput.type = 'text'; vsetInput.placeholder = 'VSet (V)';
    const btnSetV = document.createElement('button');
    btnSetV.className = 'btn-sm btn-set'; btnSetV.textContent = 'Set V';
    rowV.append(vsetInput, btnSetV);

    const rowI = document.createElement('div');
    rowI.className = 'popup-action-row';
    const isetInput = document.createElement('input');
    isetInput.type = 'text'; isetInput.placeholder = 'ISet (\u00b5A)';
    const btnSetI = document.createElement('button');
    btnSetI.className = 'btn-sm btn-set'; btnSetI.textContent = 'Set I';
    rowI.append(isetInput, btnSetI);

    const rowPwr = document.createElement('div');
    rowPwr.className = 'popup-action-row';
    const btnOn = document.createElement('button');
    btnOn.className = 'btn-sm btn-on'; btnOn.textContent = 'ON';
    const btnOff = document.createElement('button');
    btnOff.className = 'btn-sm btn-off'; btnOff.textContent = 'OFF';
    rowPwr.append(btnOn, btnOff);

    actions.append(rowV, rowI, rowPwr); body.appendChild(actions);
    el.appendChild(body);

    let _popupBuilt = false;
    function refresh() {
        const c = chByName[mod.n];
        if (_popupBuilt && c) {
            const liveVmon = grid.querySelector('[data-pp="vmon"]');
            const liveDiff = grid.querySelector('[data-pp="diff"]');
            const liveImon = grid.querySelector('[data-pp="imon"]');
            const livePwr  = grid.querySelector('[data-pp="pwr"]');
            const liveSt   = grid.querySelector('[data-pp="st"]');
            if (liveVmon) liveVmon.textContent = fmt(c.vmon, 2);
            if (liveDiff) {
                const popupDiff = (c.vmon != null && c.vset != null) ? (c.vmon - c.vset) : null;
                const popupDiffColor = (popupDiff != null && c.on) ? diffColorScale(Math.max(-1, Math.min(1, popupDiff / CR.diff_max))) : null;
                const popupDiffSign = (popupDiff != null && popupDiff > 0) ? '+' : '';
                liveDiff.textContent = popupDiffSign + fmt(popupDiff, 2) + ' V';
                liveDiff.style.color = popupDiffColor || '';
                liveDiff.style.fontWeight = popupDiffColor ? '600' : '';
            }
            if (liveImon) liveImon.textContent = fmt(c.imon, 3);
            if (livePwr) livePwr.innerHTML = pwrHtml(c);
            if (liveSt) {
                const ppSt = (c._cc || classifyChannel(c)).badgesHtml;
                liveSt.innerHTML = ppSt || '';
            }
            if (document.activeElement !== vsetInput) {
                vsetInput.value = c.vset != null ? c.vset.toFixed(1) : '';
                vsetInput.dataset.orig = vsetInput.value;
            }
            if (document.activeElement !== isetInput) {
                isetInput.value = (c.iSupported !== false && c.iset != null) ? c.iset.toFixed(1) : '';
                isetInput.dataset.orig = isetInput.value;
            }
        } else {
            let html = '';
            html += `<span class="plbl">Type</span><span class="pval">${mod.t}</span>`;
            html += `<span class="plbl">Position</span><span class="pval">(${mod.x.toFixed(1)}, ${mod.y.toFixed(1)}) mm</span>`;
            if (c) {
                html += `<span class="plbl">HV</span><span class="pval">${c.crate} s${c.slot} ch${c.channel}</span>`;
                html += `<span class="plbl">HV Board</span><span class="pval">${c.model || '\u2014'}</span>`;
                html += `<span class="plbl">VMon / VSet</span><span class="pval"><span class="pval-live" data-pp="vmon">${fmt(c.vmon, 2)}</span> / ${fmt(c.vset, 2)} V</span>`;
                const popupDiff = (c.vmon != null && c.vset != null) ? (c.vmon - c.vset) : null;
                const popupDiffColor = (popupDiff != null && c.on) ? diffColorScale(Math.max(-1, Math.min(1, popupDiff / CR.diff_max))) : null;
                const popupDiffStyle = popupDiffColor ? ('color:' + popupDiffColor + ';font-weight:600') : '';
                const popupDiffSign = (popupDiff != null && popupDiff > 0) ? '+' : '';
                html += `<span class="plbl">\u0394V</span><span class="pval" data-pp="diff" ${popupDiffStyle ? ('style="' + popupDiffStyle + '"') : ''}>${popupDiffSign}${fmt(popupDiff, 2)} V</span>`;
                if (c.iSupported === false) {
                    html += `<span class="plbl">IMon / ISet</span><span class="pval" style="color:var(--text-dim)">N/A</span>`;
                } else {
                    html += `<span class="plbl">IMon / ISet</span><span class="pval"><span class="pval-live" data-pp="imon">${fmt(c.imon, 3)}</span> / ${fmt(c.iset, 1)} \u00b5A</span>`;
                }
                html += `<span class="plbl">Pwr</span><span class="pval" data-pp="pwr">${pwrHtml(c)}</span>`;
                const ppSt = (c._cc || classifyChannel(c)).badgesHtml;
                if (ppSt) html += `<span class="plbl">Status</span><span class="pval" data-pp="st">${ppSt}</span>`;
                if (document.activeElement !== vsetInput) {
                    vsetInput.value = c.vset != null ? c.vset.toFixed(1) : '';
                    vsetInput.dataset.orig = vsetInput.value;
                }
                if (document.activeElement !== isetInput) {
                    isetInput.value = (c.iSupported !== false && c.iset != null) ? c.iset.toFixed(1) : '';
                    isetInput.dataset.orig = isetInput.value;
                }
                _popupBuilt = true;
            } else {
                html += `<span class="plbl">HV</span><span class="pval" style="color:var(--text-dim)">No linked channel</span>`;
                vsetInput.value = '';
                vsetInput.dataset.orig = '';
                isetInput.value = '';
                isetInput.dataset.orig = '';
                _popupBuilt = false;
            }
            grid.innerHTML = html;
        }

        const hasChannel = !!c;
        const canEdit = (accessLevel >= 2);
        vsetInput.disabled = !hasChannel || !canEdit;
        btnSetV.disabled   = !hasChannel || !canEdit;
        btnSetV.style.display = (hasChannel && canEdit) ? '' : 'none';
        vsetInput.style.opacity = (hasChannel && canEdit) ? '1' : '0.35';

        const iOK = hasChannel && c.iSupported !== false;
        isetInput.disabled = !iOK || !canEdit;
        btnSetI.disabled   = !iOK || !canEdit;
        btnSetI.style.display = (iOK && canEdit) ? '' : 'none';
        rowI.style.display = (hasChannel && c.iSupported === false) ? 'none' : '';
        isetInput.style.opacity = (iOK && canEdit) ? '1' : '0.35';

        const canPwr = (accessLevel >= 1);
        btnOn.disabled    = !hasChannel || !canPwr;
        btnOff.disabled   = !hasChannel || !canPwr;
        btnOn.style.opacity  = (hasChannel && canPwr) ? '1' : '0.35';
        btnOff.style.opacity = (hasChannel && canPwr) ? '1' : '0.35';
    }
    refresh();
    popups.set(mod.n, { el, refresh });

    header.querySelector('.popup-close').addEventListener('click', () => closeModPopup(mod.n));

    btnSetV.addEventListener('mousedown', e => e.preventDefault());
    btnSetV.addEventListener('click', () => {
        if (!hvMonitor || accessLevel < 2) return;
        const c = chByName[mod.n]; if (!c) return;
        const v = resolveVSetInput(vsetInput.value, c.vset); if (isNaN(v) || v < 0) return;
        const orig = c.vset;
        hvMonitor.setChannelVoltage(c.crate, c.slot, c.channel, v);
        c.vset = v; dataDirty = true;
        addPendingSet(c.crate, c.slot, c.channel, 'vset', v, orig);
        vsetInput.value = v.toFixed(1);
        vsetInput.dataset.orig = vsetInput.value;
        _popupBuilt = false; refresh();
    });
    vsetInput.addEventListener('keydown', e => { if (e.key === 'Enter') btnSetV.click(); });

    btnSetI.addEventListener('mousedown', e => e.preventDefault());
    btnSetI.addEventListener('click', () => {
        if (!hvMonitor || accessLevel < 2) return;
        const c = chByName[mod.n]; if (!c || c.iSupported === false) return;
        const v = parseFloat(isetInput.value); if (isNaN(v) || v < 0) return;
        const orig = c.iset;
        hvMonitor.setChannelCurrent(c.crate, c.slot, c.channel, v);
        c.iset = v; dataDirty = true;
        addPendingSet(c.crate, c.slot, c.channel, 'iset', v, orig);
        isetInput.dataset.orig = isetInput.value;
        _popupBuilt = false; refresh();
    });
    isetInput.addEventListener('keydown', e => { if (e.key === 'Enter') btnSetI.click(); });

    btnOn.addEventListener('click', () => {
        if (!hvMonitor || accessLevel < 1) return;
        const c = chByName[mod.n]; if (!c) return;
        hvMonitor.setChannelPower(c.crate, c.slot, c.channel, true);
        addPendingPower(c.crate, c.slot, c.channel, true);
        dataDirty = true; _popupBuilt = false; refresh();
    });
    btnOff.addEventListener('click', () => {
        if (!hvMonitor || accessLevel < 1) return;
        const c = chByName[mod.n]; if (!c) return;
        hvMonitor.setChannelPower(c.crate, c.slot, c.channel, false);
        addPendingPower(c.crate, c.slot, c.channel, false);
        dataDirty = true; _popupBuilt = false; refresh();
    });

    // Drag via header
    let drag = null;
    header.addEventListener('mousedown', e => {
        if (e.target.classList.contains('popup-close')) return;
        bringToFront(el);
        drag = { sx: e.clientX, sy: e.clientY,
                 ox: el.offsetLeft, oy: el.offsetTop };
        e.preventDefault();
    });
    window.addEventListener('mousemove', e => {
        if (!drag) return;
        let nx = drag.ox + (e.clientX - drag.sx);
        let ny = drag.oy + (e.clientY - drag.sy);
        nx = Math.max(0, Math.min(window.innerWidth  - el.offsetWidth,  nx));
        ny = Math.max(0, Math.min(window.innerHeight - el.offsetHeight, ny));
        el.style.left = nx + 'px';
        el.style.top  = ny + 'px';
    });
    window.addEventListener('mouseup', () => { drag = null; });

    el.addEventListener('mousedown', () => bringToFront(el));

    document.body.appendChild(el);
    bringToFront(el);
}

function closeModPopup(name) {
    const entry = popups.get(name);
    if (!entry) return;
    entry.el.remove();
    popups.delete(name);
}

function refreshAllPopups() {
    popups.forEach(({ refresh }) => refresh());
}
