// ═════════════════════════════════════════════════════════════════════
//  GEOMETRY MAP TAB
// ═════════════════════════════════════════════════════════════════════

function initGeoMap() {
    geoCanvas = document.getElementById('geo-canvas');
    geoCtx    = geoCanvas.getContext('2d');
    geoWrap   = document.getElementById('geo-canvas-wrap');

    // Pan
    geoWrap.addEventListener('mousedown', e => {
        if (e.button !== 0) return;
        geoDrag = { sx: e.clientX, sy: e.clientY, ox: geoTransform.x, oy: geoTransform.y };
    });
    // Throttle mousemove to one rAF per frame — avoids running a 1731-module
    // hit-test on every raw mouse event (can fire 200+ times/s).
    let pendingMouseEvent = null;
    window.addEventListener('mousemove', e => {
        if (geoDrag) {
            geoTransform.x = geoDrag.ox + (e.clientX - geoDrag.sx);
            geoTransform.y = geoDrag.oy + (e.clientY - geoDrag.sy);
            renderGeo();
        }
        pendingMouseEvent = e;
    });
    (function hoverLoop() {
        if (pendingMouseEvent) {
            updateGeoHover(pendingMouseEvent);
            pendingMouseEvent = null;
        }
        requestAnimationFrame(hoverLoop);
    })();
    window.addEventListener('mouseup', () => { geoDrag = null; });

    // Zoom
    geoWrap.addEventListener('wheel', e => {
        e.preventDefault();
        const rect = geoWrap.getBoundingClientRect();
        const mx = e.clientX - rect.left;
        const my = e.clientY - rect.top;
        const factor = e.deltaY < 0 ? 1.12 : 1/1.12;
        const ns = Math.max(0.15, Math.min(8, geoTransform.scale * factor));
        // zoom toward cursor
        geoTransform.x = mx - (mx - geoTransform.x) * (ns / geoTransform.scale);
        geoTransform.y = my - (my - geoTransform.y) * (ns / geoTransform.scale);
        geoTransform.scale = ns;
        renderGeo();
    }, { passive: false });

    // Click -> popup
    geoWrap.addEventListener('click', e => {
        if (geoDrag && (Math.abs(e.clientX - geoDrag?.sx) > 4 || Math.abs(e.clientY - geoDrag?.sy) > 4)) return;
        const mod = hitTestGeo(e);
        if (mod) openModPopup(mod);
    });

    // Hover tooltip
    geoWrap.addEventListener('mouseleave', () => {
        geoHover = null;
        document.getElementById('geo-tooltip').style.display = 'none';
    });

    // Color mode
    document.getElementById('geo-color-mode').addEventListener('change', () => {
        rebuildColorCache();   // mode changed — recompute all colours
        drawGeoLegend();
        renderGeo();
    });

    // Search
    document.getElementById('geo-search').addEventListener('input', e => {
        geoHighlight = e.target.value.trim().toUpperCase();
        renderGeo();
        updateFooter();
    });

    // Reset
    document.getElementById('geo-reset').addEventListener('click', () => { resetGeoView(); });

    // Close Popups
    document.getElementById('geo-close-popups').addEventListener('click', () => {
        [...popups.keys()].forEach(name => closeModPopup(name));
    });

    // Resize observer
    new ResizeObserver(() => { resizeGeoCanvas(); renderGeo(); }).observe(geoWrap);

    drawGeoLegend();
}

function resizeGeoCanvas() {
    const rect = geoWrap.getBoundingClientRect();
    geoCanvas.width  = rect.width  * devicePixelRatio;
    geoCanvas.height = rect.height * devicePixelRatio;
    geoCanvas.style.width  = rect.width + 'px';
    geoCanvas.style.height = rect.height + 'px';
    geoCtx.setTransform(devicePixelRatio, 0, 0, devicePixelRatio, 0, 0);
}

function resetGeoView() {
    const rect = geoWrap.getBoundingClientRect();
    if (!rect.width || !rect.height) return;
    // Fit all modules: x ∈ [-563, 563], y ∈ [-563, 563]
    const extent = GV.extent; // half-width in mm, with margin
    const scaleX = rect.width  / (2 * extent);
    const scaleY = rect.height / (2 * extent);
    geoTransform.scale = Math.min(scaleX, scaleY) * 0.92;
    geoTransform.x = rect.width  / 2;
    geoTransform.y = rect.height / 2;
    renderGeo();
}

// ── Color helpers ────────────────────────────────────────────────────
function geoColorMode() { return document.getElementById('geo-color-mode').value; }

// ── Module colour cache ──────────────────────────────────────────────
// moduleColor() is called for all 1731 modules on every renderGeo() frame.
// Parsing hex strings in lerpColor() is expensive at that call rate.
// Instead we pre-compute and cache one colour string per module whenever
// data arrives (rebuildColorCache), keyed by module name + colour mode.
// renderGeo() reads the cache; a colour-mode change also triggers a rebuild.
let _colorCache = {};        // modName -> colour string
let _colorCacheMode = '';    // mode the cache was built for

function rebuildColorCache() {
    const mode = geoColorMode();
    _colorCacheMode = mode;
    _colorCache = {};
    for (const mod of MODULES) {
        _colorCache[mod.n] = _computeModuleColor(mod, mode);
    }
}

function moduleColor(mod) {
    // If mode changed since last build, rebuild first
    const mode = geoColorMode();
    if (mode !== _colorCacheMode) rebuildColorCache();
    return _colorCache[mod.n] ?? '#222';
}

function _computeModuleColor(mod, mode) {
    // Booster modules get colour from live TCP data, independent of colour mode
    if (mod.t === 'booster') {
        const bs = boosterByName[mod.n];
        if (!bs || !bs.connected) return '#1a1a2e';   // offline – very dark blue
        if (bs.error)             return '#7f1d1d';   // error – dark red
        if (!bs.on)               return '#1e293b';   // off – dark slate
        if (bs.mode === 'CC')     return '#78350f';   // CC – amber warning
        return '#14532d';                              // on + CV – solid green
    }

    const ch = chByName[mod.n];

    if (mode === 'status') {
        if (!ch) return '#222';
        const cc = classifyChannel(ch);
        if (cc.isFault)  return '#f56565';   // red
        if (cc.isWarn)   return '#eab308';   // amber (suppressed or ΔV)
        if (!ch.on)      return '#4a5568';   // dim grey
        return '#2dd4a0';                     // green
    }

    if (!ch) return '#222';

    if (mode === 'vmon') {
        if (!ch.on) return '#333';
        const t = Math.min(1, Math.max(0, Math.abs(ch.vmon ?? 0) / CR.vmon_max));
        return vmonColorScale(t);
    }

    if (mode === 'vset') {
        const t = Math.min(1, Math.max(0, Math.abs(ch.vset ?? 0) / CR.vset_max));
        return vmonColorScale(t);
    }

    // diff
    if (!ch.on) return '#333';
    if (ch.vmon == null || ch.vset == null) return '#333';
    const diff = Math.abs(ch.vmon - ch.vset);
    const t = Math.min(1, Math.max(0, diff / CR.diff_max));
    return diffColorScale(t);
}

function diffColorScale(t) {
    // 0 -> #2dd4a0 (green), 0.5 -> #eab308 (amber), 1.0 -> #f56565 (red)
    if (t < 0.5) return lerpColor('#2dd4a0', '#eab308', t * 2);
    else         return lerpColor('#eab308', '#f56565', (t - 0.5) * 2);
}

function vmonColorScale(t) {
    // 0 -> #0b1628 (off/dark), 0.5 -> #3b9eff (blue), 1.0 -> #eab308 (amber)
    if (t < 0.5) {
        const u = t * 2;
        return lerpColor('#0b1628', '#3b9eff', u);
    } else {
        const u = (t - 0.5) * 2;
        return lerpColor('#3b9eff', '#eab308', u);
    }
}

function lerpColor(a, b, t) {
    const ar = parseInt(a.slice(1,3),16), ag = parseInt(a.slice(3,5),16), ab = parseInt(a.slice(5,7),16);
    const br = parseInt(b.slice(1,3),16), bg = parseInt(b.slice(3,5),16), bb = parseInt(b.slice(5,7),16);
    const r = Math.round(ar + (br-ar)*t), g = Math.round(ag + (bg-ag)*t), bl = Math.round(ab + (bb-ab)*t);
    return `rgb(${r},${g},${bl})`;
}

function drawGeoLegend() {
    const canvas = document.getElementById('leg-bar');
    const ctx = canvas.getContext('2d');
    const w = canvas.width, h = canvas.height;
    ctx.clearRect(0, 0, w, h);
    const mode = geoColorMode();

    if (mode === 'status') {
        // status: four-segment (OFF / Good / Warn / Fault)
        const seg = w / 4;
        ctx.fillStyle = '#4a5568'; ctx.fillRect(0,     0, seg, h);
        ctx.fillStyle = '#2dd4a0'; ctx.fillRect(seg,   0, seg, h);
        ctx.fillStyle = '#eab308'; ctx.fillRect(seg*2, 0, seg, h);
        ctx.fillStyle = '#f56565'; ctx.fillRect(seg*3, 0, seg, h);
        document.getElementById('leg-lo').textContent = 'OFF';
        document.getElementById('leg-hi').textContent = 'FAULT';
    } else if (mode === 'vmon' || mode === 'vset') {
        for (let i = 0; i < w; i++) {
            ctx.fillStyle = vmonColorScale(i / w);
            ctx.fillRect(i, 0, 1, h);
        }
        const rangeMax = mode === 'vmon' ? CR.vmon_max : CR.vset_max;
        document.getElementById('leg-lo').textContent = '0 V';
        document.getElementById('leg-hi').textContent = rangeMax + ' V';
    } else {
        // diff: continuous green -> amber -> red
        for (let i = 0; i < w; i++) {
            ctx.fillStyle = diffColorScale(i / w);
            ctx.fillRect(i, 0, 1, h);
        }
        document.getElementById('leg-lo').textContent = '0 V';
        document.getElementById('leg-hi').textContent = CR.diff_max + '+ V';
    }
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

// ── Rendering ────────────────────────────────────────────────────────
function renderGeo() {
    if (!geoCanvas.width) return;
    const ctx = geoCtx;
    const W = geoCanvas.width / devicePixelRatio;
    const H = geoCanvas.height / devicePixelRatio;
    ctx.clearRect(0, 0, W, H);

    ctx.save();
    ctx.translate(geoTransform.x, geoTransform.y);
    ctx.scale(geoTransform.scale, -geoTransform.scale); // flip Y for physics coords

    const gap = 0.4; // mm gap between modules for visual separation

    for (let i = 0; i < MODULES.length; i++) {
        const m = MODULES[i];
        const hw = m.sx / 2 - gap;
        const hh = m.sy / 2 - gap;

        const isHover = (geoHover === m.n);
        const isSearch = geoHighlight && geoModuleMatches(m, geoHighlight);
        const noMatch = geoHighlight && !isSearch;

        ctx.fillStyle = noMatch ? 'rgba(20,28,38,0.6)' : moduleColor(m);
        ctx.fillRect(m.x - hw, m.y - hh, hw * 2, hh * 2);

        // LMS virtual blocks: draw distinctive border + label
        if (m.t === 'LMS' && !noMatch) {
            ctx.save();
            ctx.strokeStyle = '#c084fc';  // purple accent
            ctx.lineWidth = 1.5 / geoTransform.scale;
            ctx.setLineDash([4 / geoTransform.scale, 3 / geoTransform.scale]);
            ctx.strokeRect(m.x - hw, m.y - hh, hw * 2, hh * 2);
            ctx.setLineDash([]);
            ctx.scale(1, -1);
            ctx.fillStyle = '#e8edf3';
            ctx.font = `${10 / geoTransform.scale}px monospace`;
            ctx.textAlign = 'center';
            ctx.textBaseline = 'middle';
            ctx.fillText(m.n, m.x, -m.y);
            ctx.restore();
        }

        // Booster supply blocks: distinctive border + two-line live label
        if (m.t === 'booster' && !noMatch) {
            const bs = boosterByName[m.n];
            const bdrCol = (!bs || !bs.connected) ? '#475569'
                         : bs.error               ? '#f87171'
                         : !bs.on                 ? '#64748b'
                         : bs.mode === 'CC'        ? '#fbbf24'
                         :                          '#4ade80';
            ctx.save();
            ctx.strokeStyle = bdrCol;
            ctx.lineWidth = 2.5 / geoTransform.scale;
            ctx.strokeRect(m.x - hw, m.y - hh, hw * 2, hh * 2);
            ctx.scale(1, -1);
            ctx.textAlign = 'center';
            ctx.textBaseline = 'middle';
            // Name line
            ctx.fillStyle = '#cbd5e1';
            ctx.font = `bold ${11 / geoTransform.scale}px sans-serif`;
            ctx.fillText(m.n, m.x, -(m.y + hh * 0.3));
            // VMon / status line
            const bLine2 = (!bs || !bs.connected) ? 'OFFLINE'
                         : bs.vmon != null         ? bs.vmon.toFixed(1) + ' V' + (bs.imon != null ? ' / ' + bs.imon.toFixed(3) + ' A' : '')
                         :                           (bs.on ? 'ON' : 'OFF');
            ctx.fillStyle = bdrCol;
            ctx.font = `${10 / geoTransform.scale}px monospace`;
            ctx.fillText(bLine2, m.x, -(m.y - hh * 0.3));
            ctx.restore();
        }

        if (isHover || isSearch) {
            ctx.strokeStyle = isHover ? '#fff' : '#3b9eff';
            ctx.lineWidth = 1.5 / geoTransform.scale;
            ctx.strokeRect(m.x - hw, m.y - hh, hw * 2, hh * 2);
        }
    }

    // Draw axis cross at center
    // ctx.strokeStyle = 'rgba(255,255,255,0.12)';
    // ctx.lineWidth = 1 / geoTransform.scale;
    // ctx.beginPath();
    // ctx.moveTo(-600, 0); ctx.lineTo(600, 0);
    // ctx.moveTo(0, -600); ctx.lineTo(0, 600);
    // ctx.stroke();

    // Draw a small + marker at the world origin
    const markerSize = 8 / geoTransform.scale;
    ctx.strokeStyle = 'rgba(255,255,255,0.35)';
    ctx.lineWidth = 1 / geoTransform.scale;
    ctx.beginPath();
    ctx.moveTo(-markerSize, 0); ctx.lineTo(markerSize, 0);
    ctx.moveTo(0, -markerSize); ctx.lineTo(0, markerSize);
    ctx.stroke();

    ctx.restore();

    // Labels at high zoom
    if (geoTransform.scale > 2.2) {
        ctx.save();
        ctx.translate(geoTransform.x, geoTransform.y);
        const s = geoTransform.scale;
        const fontSize = Math.max(4, Math.min(9, 6));
        ctx.font = `${fontSize / s}px JetBrains Mono, monospace`;
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        ctx.fillStyle = 'rgba(255,255,255,0.7)';
        for (const m of MODULES) {
            if (m.t === 'booster') continue;   // booster blocks draw their own labels
            // only draw labels that are within viewport
            const sx = m.x * s, sy = -m.y * s;
            if (sx < -geoTransform.x - 100 || sx > -geoTransform.x + geoCanvas.width/devicePixelRatio + 100) continue;
            if (sy < -geoTransform.y - 100 || sy > -geoTransform.y + geoCanvas.height/devicePixelRatio + 100) continue;
            ctx.save();
            ctx.scale(s, -s);
            ctx.fillText(m.n, m.x, -m.y);
            ctx.restore();
        }
        ctx.restore();
    }
}

// ── Hit testing ──────────────────────────────────────────────────────
function screenToWorld(e) {
    const rect = geoWrap.getBoundingClientRect();
    const sx = e.clientX - rect.left;
    const sy = e.clientY - rect.top;
    return {
        x: (sx - geoTransform.x) / geoTransform.scale,
        y: -(sy - geoTransform.y) / geoTransform.scale   // flip Y
    };
}

function hitTestGeo(e) {
    const w = screenToWorld(e);
    for (let i = MODULES.length - 1; i >= 0; i--) {
        const m = MODULES[i];
        const hw = m.sx / 2, hh = m.sy / 2;
        if (w.x >= m.x - hw && w.x <= m.x + hw && w.y >= m.y - hh && w.y <= m.y + hh)
            return m;
    }
    return null;
}

function updateGeoHover(e) {
    if (!document.getElementById('geo-tab').classList.contains('active')) return;
    const mod = hitTestGeo(e);
    const tooltip = document.getElementById('geo-tooltip');
    if (mod) {
        if (geoHover !== mod.n) {
            geoHover = mod.n;
            if (!geoDrag) renderGeo();  // skip redraw if pan is already rendering
        }
        const ch = chByName[mod.n];
        let html = `<div class="tt-name">${mod.n}</div>`;
        html += `<div class="tt-row"><span class="tt-label">Type</span><span class="tt-val">${mod.t}</span></div>`;

        // ── Booster tooltip (no HV/DAQ rows) ─────────────────────────────
        if (mod.t === 'booster') {
            html += `<div class="tt-row"><span class="tt-label">IP</span><span class="tt-val" style="font-family:monospace">${escHtml(mod.ip || '—')}</span></div>`;
            const bs = boosterByName[mod.n];
            if (bs) {
                const connSpan = bs.connected
                    ? '<span class="st-on">Connected</span>'
                    : '<span class="st-off">Offline</span>';
                html += `<div class="tt-row"><span class="tt-label">Link</span><span class="tt-val">${connSpan}</span></div>`;
                if (bs.connected) {
                    html += `<div class="tt-row"><span class="tt-label">VMon / VSet</span><span class="tt-val"><span class="tt-live">${bs.vmon != null ? bs.vmon.toFixed(2)+' V' : '—'}</span> / ${bs.vset != null ? bs.vset.toFixed(2)+' V' : '—'}</span></div>`;
                    html += `<div class="tt-row"><span class="tt-label">IMon / ISet</span><span class="tt-val"><span class="tt-live">${bs.imon != null ? bs.imon.toFixed(3)+' A' : '—'}</span> / ${bs.iset != null ? bs.iset.toFixed(3)+' A' : '—'}</span></div>`;
                    html += `<div class="tt-row"><span class="tt-label">Mode</span><span class="tt-val">${escHtml(bs.mode||'—')}</span></div>`;
                    html += `<div class="tt-row"><span class="tt-label">Pwr</span><span class="tt-val">${bs.on ? '<span class="st-on">ON</span>' : '<span class="st-off">OFF</span>'}</span></div>`;
                    if (bs.error) html += `<div class="tt-row"><span class="tt-label">Error</span><span class="tt-val" style="color:var(--red)">${escHtml(bs.error)}</span></div>`;
                }
            } else {
                html += `<div class="tt-row"><span class="tt-label">Link</span><span class="tt-val" style="color:var(--text-dim)">waiting…</span></div>`;
            }
            tooltip.innerHTML = html;
            tooltip.style.display = 'block';
            tooltip.style.left = (e.clientX + 14) + 'px';
            tooltip.style.top  = (e.clientY + 14) + 'px';
            return;
        }

        if (ch) {
            html += `<div class="tt-row"><span class="tt-label">HV</span><span class="tt-val">${ch.crate} s${ch.slot} ch${ch.channel}</span></div>`;
            const daqEntry = daqByName[mod.n];
            const daqStr = daqEntry ? ('c' + daqEntry.crate + ' s' + daqEntry.slot + ' ch' + daqEntry.channel) : '<span style="color:var(--text-dim)">—</span>';
            html += `<div class="tt-row"><span class="tt-label">DAQ</span><span class="tt-val">${daqStr}</span></div>`;
            html += `<div class="tt-row"><span class="tt-label">VMon / VSet</span><span class="tt-val"><span class="tt-live">${fmt(ch.vmon, 2)}</span> / ${fmt(ch.vset, 2)} V</span></div>`;
            const diff = (ch.vmon != null && ch.vset != null) ? Math.abs(ch.vmon - ch.vset) : null;
            const diffColor = (diff != null && ch.on) ? diffColorScale(Math.min(1, diff / CR.diff_max)) : null;
            const diffStyle = diffColor ? (' style="color:' + diffColor + ';font-weight:600"') : '';
            html += `<div class="tt-row"><span class="tt-label">ΔV</span><span class="tt-val"${diffStyle}>${fmt(diff, 2)} V</span></div>`;
            if (ch.iSupported !== false && ch.imon != null)
                html += `<div class="tt-row"><span class="tt-label">IMon</span><span class="tt-val"><span class="tt-live">${fmt(ch.imon, 3)}</span> µA</span></div>`;
            html += `<div class="tt-row"><span class="tt-label">Pwr</span><span class="tt-val">${pwrHtml(ch)}</span></div>`;
            const ttSt = classifyChannel(ch).badgesHtml;
            if (ttSt) html += `<div class="tt-row"><span class="tt-label">Status</span><span class="tt-val">${ttSt}</span></div>`;
        } else {
            html += `<div class="tt-row"><span class="tt-label">HV</span><span class="tt-val" style="color:var(--text-dim)">not linked</span></div>`;
            const daqEntry = daqByName[mod.n];
            const daqStr = daqEntry ? ('c' + daqEntry.crate + ' s' + daqEntry.slot + ' ch' + daqEntry.channel) : '<span style="color:var(--text-dim)">—</span>';
            html += `<div class="tt-row"><span class="tt-label">DAQ</span><span class="tt-val">${daqStr}</span></div>`;
        }
        tooltip.innerHTML = html;
        tooltip.style.display = 'block';
        tooltip.style.left = (e.clientX + 14) + 'px';
        tooltip.style.top  = (e.clientY + 14) + 'px';
    } else {
        if (geoHover) { geoHover = null; if (!geoDrag) renderGeo(); }
        tooltip.style.display = 'none';
    }
}

// ── Floating multi-window popup system ──────────────────────────────
const popups = new Map();   // modName → { el, ch ref }
let popupZTop = 210;        // z-index counter

function bringToFront(el) {
    popupZTop++;
    el.style.zIndex = popupZTop;
    document.querySelectorAll('.mod-popup').forEach(p => p.classList.remove('focused'));
    el.classList.add('focused');
}

function openModPopup(mod) {
    // If already open, just bring to front
    if (popups.has(mod.n)) {
        bringToFront(popups.get(mod.n).el);
        return;
    }

    // Booster supply popup — entirely different layout
    if (mod.t === 'booster') { openBoosterPopup(mod); return; }

    const ch = chByName[mod.n];

    // Build element
    const el = document.createElement('div');
    el.className = 'mod-popup';

    // Stagger new windows
    const offset = (popups.size % 8) * 26;
    el.style.left = (80 + offset) + 'px';
    el.style.top  = (80 + offset) + 'px';

    // Header
    const header = document.createElement('div');
    header.className = 'popup-header';
    header.innerHTML = `<h2>${mod.n}</h2><button class="popup-close" title="Close">✕</button>`;
    el.appendChild(header);

    // Body
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
    isetInput.type = 'text'; isetInput.placeholder = 'ISet (µA)';
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

    actions.append(rowV, rowI, rowPwr);body.appendChild(actions);
    el.appendChild(body);

    // Populate grid + wire actions
    function refresh() {
        const c = chByName[mod.n];
        let html = '';
        html += `<span class="plbl">Type</span><span class="pval">${mod.t}</span>`;
        html += `<span class="plbl">Position</span><span class="pval">(${mod.x.toFixed(1)}, ${mod.y.toFixed(1)}) mm</span>`;
        if (c) {
            html += `<span class="plbl">HV</span><span class="pval">${c.crate} s${c.slot} ch${c.channel}</span>`;
            html += `<span class="plbl">HV Board</span><span class="pval">${c.model || '—'}</span>`;
            const daqC = daqByName[mod.n];
            const daqStr = daqC ? ('c' + daqC.crate + ' s' + daqC.slot + ' ch' + daqC.channel) : '—';
            html += `<span class="plbl">DAQ</span><span class="pval">${daqStr}</span>`;
            html += `<span class="plbl">VMon / VSet</span><span class="pval"><span class="pval-live">${fmt(c.vmon, 2)}</span> / ${fmt(c.vset, 2)} V</span>`;
            const popupDiff = (c.vmon != null && c.vset != null) ? Math.abs(c.vmon - c.vset) : null;
            const popupDiffColor = (popupDiff != null && c.on) ? diffColorScale(Math.min(1, popupDiff / CR.diff_max)) : null;
            const popupDiffStyle = popupDiffColor ? ('color:' + popupDiffColor + ';font-weight:600') : '';
            html += `<span class="plbl">ΔV</span><span class="pval" ${popupDiffStyle ? ('style="' + popupDiffStyle + '"') : ''}>${fmt(popupDiff, 2)} V</span>`;
            if (c.iSupported === false) {
                html += `<span class="plbl">IMon / ISet</span><span class="pval" style="color:var(--text-dim)">N/A</span>`;
            } else {
                html += `<span class="plbl">IMon / ISet</span><span class="pval"><span class="pval-live">${fmt(c.imon, 3)}</span> / ${fmt(c.iset, 1)} µA</span>`;
            }
            html += `<span class="plbl">Pwr</span><span class="pval">${pwrHtml(c)}</span>`;
            const ppSt = classifyChannel(c).badgesHtml;
            if (ppSt) html += `<span class="plbl">Status</span><span class="pval">${ppSt}</span>`;
            vsetInput.value = c.vset != null ? c.vset.toFixed(1) : '';
            isetInput.value = (c.iSupported !== false && c.iset != null) ? c.iset.toFixed(1) : '';
        } else {
            html += `<span class="plbl">HV</span><span class="pval" style="color:var(--text-dim)">No linked channel</span>`;
            const daqC = daqByName[mod.n];
            const daqStr = daqC ? ('c' + daqC.crate + ' s' + daqC.slot + ' ch' + daqC.channel) : '—';
            html += `<span class="plbl">DAQ</span><span class="pval">${daqStr}</span>`;
            vsetInput.value = '';
            isetInput.value = '';
        }
        grid.innerHTML = html;
        const hasChannel = !!c;
        vsetInput.disabled = !hasChannel || !expertMode;
        btnSetV.disabled   = !hasChannel || !expertMode;
        vsetInput.style.opacity = (hasChannel && !expertMode) ? '0.35' : '1';
        btnSetV.style.opacity   = (hasChannel && !expertMode) ? '0.35' : '1';

        const iOK = hasChannel && c.iSupported !== false;
        isetInput.disabled = !iOK || !expertMode;
        btnSetI.disabled   = !iOK || !expertMode;
        rowI.style.display = (hasChannel && c.iSupported === false) ? 'none' : '';
        isetInput.style.opacity = (iOK && !expertMode) ? '0.35' : '1';
        btnSetI.style.opacity   = (iOK && !expertMode) ? '0.35' : '1';

        btnOn.disabled     = !hasChannel;
        btnOff.disabled    = !hasChannel;
    }
    refresh();
    popups.set(mod.n, { el, refresh });

    // Close button
    header.querySelector('.popup-close').addEventListener('click', () => closeModPopup(mod.n));

    // Action buttons
    btnSetV.addEventListener('click', () => {
        if (!hvMonitor || !expertMode) return;
        const c = chByName[mod.n]; if (!c) return;
        const v = parseFloat(vsetInput.value); if (isNaN(v)) return;
        hvMonitor.setChannelVoltage(c.crate, c.slot, c.channel, v);
        c.vset = v; dataDirty = true; refresh();
    });
    btnSetI.addEventListener('click', () => {
        if (!hvMonitor || !expertMode) return;
        const c = chByName[mod.n]; if (!c || c.iSupported === false) return;
        const v = parseFloat(isetInput.value); if (isNaN(v) || v < 0) return;
        hvMonitor.setChannelCurrent(c.crate, c.slot, c.channel, v);
        c.iset = v; dataDirty = true; refresh();
    });
    btnOn.addEventListener('click', () => {
        if (!hvMonitor) return;
        const c = chByName[mod.n]; if (!c) return;
        hvMonitor.setChannelPower(c.crate, c.slot, c.channel, true);
        c.on = true; dataDirty = true; refresh();
    });
    btnOff.addEventListener('click', () => {
        if (!hvMonitor) return;
        const c = chByName[mod.n]; if (!c) return;
        hvMonitor.setChannelPower(c.crate, c.slot, c.channel, false);
        c.on = false; dataDirty = true; refresh();
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
        // Keep within viewport
        nx = Math.max(0, Math.min(window.innerWidth  - el.offsetWidth,  nx));
        ny = Math.max(0, Math.min(window.innerHeight - el.offsetHeight, ny));
        el.style.left = nx + 'px';
        el.style.top  = ny + 'px';
    });
    window.addEventListener('mouseup', () => { drag = null; });

    // Focus on click anywhere in popup
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

// Refresh all open popups when channel data updates
function refreshAllPopups() {
    popups.forEach(({ refresh }) => refresh());
}

