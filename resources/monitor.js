// ═════════════════════════════════════════════════════════════════════
//  Module geometry data (loaded from backend via getModuleGeometry)
// ═════════════════════════════════════════════════════════════════════
let MODULES = [];
const MOD_MAP = {};

// ═════════════════════════════════════════════════════════════════════
//  Global state
// ═════════════════════════════════════════════════════════════════════
let expertMode   = false;
let hvMonitor    = null;
let allChannels  = [];
let sortCol      = 'crate';
let sortAsc      = true;
let filterStatus = 'all';
let filterCrate  = null;
let searchText   = '';

// Geometry map state
let geoTransform = { x: 0, y: 0, scale: 1 };
let geoDrag      = null;
let geoHover     = null;   // module name under cursor
let geoHighlight = '';     // from search
let geoCanvas, geoCtx, geoWrap;

// Channel lookup by name for geometry
let chByName = {};

// ΔV thresholds (overridden by gui_config.json)
let DV = {
    warn_threshold: 2.0,
    table_ok:       0.5,
    table_warn:     2.0,
    geo_excellent:  0.2,
    geo_good:       1.0,
    geo_warn:       3.0,
    geo_bad:        10.0,
};

// Color scale ranges (overridden by gui_config.json)
let CR = {
    vmon_max: 2100,
    vset_max: 2100,
};

// Geometry view settings (overridden by gui_config.json)
let GV = {
    extent: 680,
};

// ═════════════════════════════════════════════════════════════════════
//  QWebChannel bootstrap
// ═════════════════════════════════════════════════════════════════════
document.addEventListener('DOMContentLoaded', () => {
    new QWebChannel(qt.webChannelTransport, channel => {
        hvMonitor = channel.objects.hvMonitor;
        hvMonitor.channelsUpdated.connect(jsonStr => {
            allChannels = JSON.parse(jsonStr);
            rebuildChMap();
            refreshAllPopups();
            renderActiveTab();
        });
        hvMonitor.readAll(jsonStr => {
            allChannels = JSON.parse(jsonStr);
            rebuildChMap();
            populateCrateChips();
            // Load module geometry from backend JSON file
            hvMonitor.getModuleGeometry(geoJson => {
                MODULES = JSON.parse(geoJson);
                MODULES.forEach(m => { MOD_MAP[m.n] = m; });
                console.log('Loaded ' + MODULES.length + ' modules');
                renderActiveTab();
            });
            // Load GUI config (ΔV thresholds, etc.)
            hvMonitor.getGuiConfig(cfgJson => {
                try {
                    const cfg = JSON.parse(cfgJson);
                    if (cfg.deltaV) Object.assign(DV, cfg.deltaV);
                    if (cfg.colorRange) Object.assign(CR, cfg.colorRange);
                    if (cfg.geoView)    Object.assign(GV, cfg.geoView);
                    console.log('GUI config loaded — DV:', DV, 'CR:', CR, 'GV:', GV);
                } catch (e) {
                    console.warn('Failed to parse gui_config.json, using defaults', e);
                }
                renderActiveTab();
            });
            // Sync poll interval slider with backend setting
            hvMonitor.getPollInterval(ms => {
                const sec = ms / 1000;
                const slider = document.getElementById('poll-slider');
                // Extend slider max if backend value exceeds it
                if (sec > parseFloat(slider.max)) slider.max = Math.ceil(sec);
                slider.value = sec;
                document.getElementById('poll-val').textContent = sec.toFixed(1);
            });
            renderActiveTab();
            document.getElementById('loading').classList.add('hidden');
            setPillConnected(true);
        });
    });

    initTableUI();
    initTabs();
    initGeoMap();
});

function rebuildChMap() {
    chByName = {};
    allChannels.forEach(ch => { chByName[ch.name] = ch; });
}

function renderActiveTab() {
    const active = document.querySelector('.tab-content.active');
    if (active.id === 'table-tab') renderTable();
    else if (active.id === 'geo-tab') renderGeo();
    updateFooter();
}

// ═════════════════════════════════════════════════════════════════════
//  TABS
// ═════════════════════════════════════════════════════════════════════
function initTabs() {
    document.querySelectorAll('.tab-btn').forEach(btn => {
        btn.addEventListener('click', () => {
            document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
            document.querySelectorAll('.tab-content').forEach(c => c.classList.remove('active'));
            btn.classList.add('active');
            document.getElementById(btn.dataset.tab).classList.add('active');
            renderActiveTab();
            if (btn.dataset.tab === 'geo-tab') {
                resizeGeoCanvas();
                resetGeoView();
            }
        });
    });
}

// ═════════════════════════════════════════════════════════════════════
//  TABLE TAB (original logic)
// ═════════════════════════════════════════════════════════════════════
function initTableUI() {
    document.getElementById('btn-refresh').addEventListener('click', () => {
        if (!hvMonitor) return;
        hvMonitor.readAll(jsonStr => {
            allChannels = JSON.parse(jsonStr);
            rebuildChMap();
            renderActiveTab();
        });
    });
    document.getElementById('search').addEventListener('input', e => {
        searchText = e.target.value.trim().toLowerCase();
        renderTable();
    });
    document.getElementById('poll-slider').addEventListener('input', e => {
        const sec = parseFloat(e.target.value);
        document.getElementById('poll-val').textContent = sec.toFixed(1);
        if (hvMonitor) hvMonitor.setPollInterval(Math.round(sec * 1000));
    });
    document.querySelectorAll('#status-chips .chip').forEach(chip => {
        chip.addEventListener('click', () => {
            document.querySelectorAll('#status-chips .chip').forEach(c => c.classList.remove('active'));
            chip.classList.add('active');
            filterStatus = chip.dataset.filter;
            renderTable();
        });
    });
    document.querySelectorAll('thead th[data-col]').forEach(th => {
        th.addEventListener('click', () => {
            const col = th.dataset.col;
            if (sortCol === col) sortAsc = !sortAsc;
            else { sortCol = col; sortAsc = true; }
            document.querySelectorAll('thead th').forEach(h => h.classList.remove('sorted'));
            th.classList.add('sorted');
            th.querySelector('.sort-arrow').textContent = sortAsc ? '▲' : '▼';
            renderTable();
        });
    });

    // ── Bulk ON / OFF buttons ───────────────────────────────────────────
    document.getElementById('btn-all-on').addEventListener('click', () => {
        if (!hvMonitor || allChannels.length === 0) return;
        const n = allChannels.length;
        if (!confirm(`Turn ON all ${n} channels?\n\nThis will energise every HV channel across all crates.`)) return;
        hvMonitor.setAllPower(true);
        allChannels.forEach(ch => { ch.on = true; });
        renderActiveTab();
    });

    document.getElementById('btn-all-off').addEventListener('click', () => {
        if (!hvMonitor || allChannels.length === 0) return;
        const n = allChannels.length;
        if (!confirm(`Turn OFF all ${n} channels?\n\nThis will de-energise every HV channel across all crates.`)) return;
        hvMonitor.setAllPower(false);
        allChannels.forEach(ch => { ch.on = false; });
        renderActiveTab();
    });

    // ── Expert mode toggle ──────────────────────────────────────────────
    document.getElementById('expert-switch').addEventListener('change', e => {
        if (e.target.checked) {
            const ok = confirm(
                '⚠ EXPERT MODE ⚠\n\n' +
                'This enables direct VSet editing on all channels.\n' +
                'Incorrect voltage settings can damage detector modules.\n\n' +
                'You must know what you are doing before enabling this.\n\n' +
                'Continue?'
            );
            if (!ok) { e.target.checked = false; return; }
        }
        expertMode = e.target.checked;
        document.getElementById('expert-label').classList.toggle('active', expertMode);
        renderActiveTab();
    });
}

function populateCrateChips() {
    const crates = [...new Set(allChannels.map(c => c.crate))].sort();
    const wrap = document.getElementById('crate-chips');
    wrap.innerHTML = '';
    const allChip = document.createElement('button');
    allChip.className = 'chip active'; allChip.dataset.crate = ''; allChip.textContent = 'All Crates';
    allChip.addEventListener('click', () => selectCrateChip(allChip, null));
    wrap.appendChild(allChip);
    crates.forEach(name => {
        const chip = document.createElement('button');
        chip.className = 'chip'; chip.dataset.crate = name;
        chip.textContent = name.replace('PRadHV_', 'HV');
        chip.addEventListener('click', () => selectCrateChip(chip, name));
        wrap.appendChild(chip);
    });
}

function selectCrateChip(chip, name) {
    document.querySelectorAll('#crate-chips .chip').forEach(c => c.classList.remove('active'));
    chip.classList.add('active');
    filterCrate = name;
    renderTable();
}

function isPrimary(ch) {
    return ch.channel === 0 && ch.name && ch.name.toUpperCase().includes('PRIMARY');
}

function renderTable() {
    // Don't clobber an in-progress VSet edit
    if (document.activeElement && document.activeElement.classList.contains('vset-inline')) return;

    let data = allChannels.slice();
    if (filterStatus === 'on')   data = data.filter(c => c.on);
    if (filterStatus === 'off')  data = data.filter(c => !c.on);
    if (filterStatus === 'primary') data = data.filter(c => isPrimary(c));
    if (filterStatus === 'warn') data = data.filter(c => c.on && Math.abs(c.vmon - c.vset) > DV.warn_threshold);
    if (filterStatus === 'fault') data = data.filter(c => statusClass(c.status) === 'status-err');
    if (filterCrate)             data = data.filter(c => c.crate === filterCrate);
    if (searchText)              data = data.filter(c =>
        (c.crate+' '+c.slot+' '+c.channel+' '+c.name+' '+c.model).toLowerCase().includes(searchText));
    data.sort((a, b) => {
        let va = a[sortCol], vb = b[sortCol];
        if (sortCol === 'diff') { va = Math.abs(a.vmon - a.vset); vb = Math.abs(b.vmon - b.vset); }
        if (typeof va === 'string') return sortAsc ? va.localeCompare(vb) : vb.localeCompare(va);
        if (typeof va === 'boolean') { va = va?1:0; vb = vb?1:0; }
        return sortAsc ? va - vb : vb - va;
    });
    const tbody = document.getElementById('ch-body');
    tbody.innerHTML = data.map(ch => {
        const diff = Math.abs(ch.vmon - ch.vset);
        const dcls = !ch.on ? 'diff-ok' : DV.table_ok ? 'diff-ok' : diff < DV.table_warn ? 'diff-warn' : 'diff-bad';
        const onCls = ch.on ? 'on' : 'off';
        const prim = isPrimary(ch);
        return `<tr class="${prim ? 'primary-row' : ''}">
            <td><span class="status-dot ${onCls}"></span></td>
            <td title="${ch.ip}">${ch.crate}</td><td>${ch.slot}</td><td>${ch.channel}</td>
            <td>${ch.model||'—'}</td>
            <td>${ch.name||'—'}${prim ? '<span class="primary-badge">Primary</span>' : ''}</td>
            <td style="text-align:right">${ch.vmon.toFixed(2)}</td>
            <td style="text-align:right">${expertMode
                ? `<input class="vset-inline" type="number" step="0.1" value="${ch.vset.toFixed(1)}"
                     onchange="inlineSetVoltage('${ch.crate}',${ch.slot},${ch.channel},this.value)"
                   ><button class="vset-apply"
                     onclick="inlineSetVoltage('${ch.crate}',${ch.slot},${ch.channel},this.previousElementSibling.value)"
                   >✓</button>`
                : ch.vset.toFixed(2)}</td>
            <td class="${dcls}" style="text-align:right">${diff.toFixed(2)}</td>
	    <td class="${statusClass(ch.status)}"
                title="${ch.status ? ch.status.split('|')[1] : ''}"
            >${ch.status ? ch.status.split('|')[0] : ''}</td>
            <td style="text-align:center">
                <button class="pwr-btn ${onCls}"
                    onclick="togglePower('${ch.crate}',${ch.slot},${ch.channel},${ch.on?'false':'true'})"
                >${ch.on?'ON':'OFF'}</button>
            </td></tr>`;
    }).join('');

    // Summary
    const total   = allChannels.length;
    const nCr     = new Set(allChannels.map(c => c.crate)).size;
    const primCnt = allChannels.filter(c => isPrimary(c)).length;
    const onCnt   = allChannels.filter(c => c.on).length;
    const warns   = allChannels.filter(c => c.on && Math.abs(c.vmon - c.vset) > DV.warn_threshold).length;
    const faults  = allChannels.filter(c => statusClass(c.status) === 'status-err').length;
    document.getElementById('s-total').textContent   = total;
    document.getElementById('s-crates').textContent  = nCr;
    document.getElementById('s-primary').textContent = primCnt;
    document.getElementById('s-on').textContent      = onCnt;
    document.getElementById('s-off').textContent     = total - onCnt;
    document.getElementById('s-warn').textContent    = warns;
    document.getElementById('s-fault').textContent   = faults;
    updateFooter();
}

function updateFooter() {
    document.getElementById('last-update').textContent = `Updated ${new Date().toLocaleTimeString()}`;

    const active = document.querySelector('.tab-content.active');
    if (active && active.id === 'geo-tab') {
        // Geometry tab: count modules matching the geo search highlight
        const total = MODULES.length;
        if (geoHighlight) {
            const matched = MODULES.filter(m => m.n.toUpperCase().includes(geoHighlight)).length;
            document.getElementById('row-count').textContent =
                `${matched} / ${total} modules shown | ${allChannels.length} channels`;
        } else {
            document.getElementById('row-count').textContent =
                `${total} modules | ${allChannels.length} channels`;
        }
    } else {
        // Table tab: count visible (filtered) rows vs total channels
        const total = allChannels.length;
        let filtered = allChannels.slice();
        if (filterStatus === 'on')      filtered = filtered.filter(c => c.on);
        if (filterStatus === 'off')     filtered = filtered.filter(c => !c.on);
        if (filterStatus === 'warn')    filtered = filtered.filter(c => c.on && Math.abs(c.vmon - c.vset) > DV.warn_threshold);
        if (filterStatus === 'primary') filtered = filtered.filter(c => isPrimary(c));
        if (filterCrate)                filtered = filtered.filter(c => c.crate === filterCrate);
        if (searchText)                 filtered = filtered.filter(c =>
            (c.crate+' '+c.slot+' '+c.channel+' '+c.name+' '+c.model).toLowerCase().includes(searchText));
        const shown = filtered.length;
        if (shown === total) {
            document.getElementById('row-count').textContent =
                `${total} channels | ${MODULES.length} modules`;
        } else {
            document.getElementById('row-count').textContent =
                `${shown} / ${total} channels shown | ${MODULES.length} modules`;
        }
    }
}

function togglePower(crate, slot, channel, on) {
    if (!hvMonitor) return;
    hvMonitor.setChannelPower(crate, slot, channel, on);
    const ch = allChannels.find(c => c.crate===crate && c.slot===slot && c.channel===channel);
    if (ch) ch.on = on;
    renderActiveTab();
}

function inlineSetVoltage(crate, slot, channel, value) {
    if (!hvMonitor || !expertMode) return;
    const v = parseFloat(value);
    if (isNaN(v) || v < 0) return;
    hvMonitor.setChannelVoltage(crate, slot, channel, v);
    const ch = allChannels.find(c => c.crate===crate && c.slot===slot && c.channel===channel);
    if (ch) ch.vset = v;
}

function setPillConnected(ok) {
    const pill = document.getElementById('conn-pill');
    const text = document.getElementById('conn-text');
    if (ok) { pill.classList.remove('disconnected'); text.textContent = 'live'; }
    else    { pill.classList.add('disconnected');    text.textContent = 'disconnected'; }
}

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
    window.addEventListener('mousemove', e => {
        if (geoDrag) {
            geoTransform.x = geoDrag.ox + (e.clientX - geoDrag.sx);
            geoTransform.y = geoDrag.oy + (e.clientY - geoDrag.sy);
            renderGeo();
        }
        updateGeoHover(e);
    });
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

function moduleColor(mod) {
    const ch = chByName[mod.n];
    const mode = geoColorMode();

    if (mode === 'status') {
        if (!ch) return '#222';
        return ch.on ? '#2dd4a0' : '#993333';
    }

    if (!ch) return '#222';

    if (mode === 'vmon') {
        if (!ch.on) return '#333';      // off: grey out
        const t = Math.min(1, Math.max(0, Math.abs(ch.vmon) / CR.vmon_max));
        return vmonColorScale(t);
    }

    if (mode === 'vset') {
        const t = Math.min(1, Math.max(0, Math.abs(ch.vset) / CR.vset_max));
        return vmonColorScale(t);
    }

    // mode === 'diff'
    if (!ch.on) return '#333';          // off: grey out
    const diff = Math.abs(ch.vmon - ch.vset);
    if (diff < DV.geo_excellent) return '#1a5c44';
    if (diff < DV.geo_good)      return '#2dd4a0';
    if (diff < DV.geo_warn)      return '#eab308';
    if (diff < DV.geo_bad)       return '#f56565';
    return '#ff2222';                   // very bad: bright red
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
        // simple two-color
        ctx.fillStyle = '#993333'; ctx.fillRect(0, 0, w/2, h);
        ctx.fillStyle = '#2dd4a0'; ctx.fillRect(w/2, 0, w/2, h);
        document.getElementById('leg-lo').textContent = 'OFF';
        document.getElementById('leg-hi').textContent = 'ON';
    } else if (mode === 'vmon' || mode === 'vset') {
        for (let i = 0; i < w; i++) {
            ctx.fillStyle = vmonColorScale(i / w);
            ctx.fillRect(i, 0, 1, h);
        }
        const rangeMax = mode === 'vmon' ? CR.vmon_max : CR.vset_max;
        document.getElementById('leg-lo').textContent = '0 V';
        document.getElementById('leg-hi').textContent = rangeMax + ' V';
    } else {
        // diff: green -> amber -> red
        const stops = [ [0,'#1a5c44'], [0.15,'#2dd4a0'], [0.4,'#eab308'], [0.7,'#f56565'], [1,'#ff2222'] ];
        const grad = ctx.createLinearGradient(0,0,w,0);
        stops.forEach(([s,c]) => grad.addColorStop(s,c));
        ctx.fillStyle = grad;
        ctx.fillRect(0, 0, w, h);
        document.getElementById('leg-lo').textContent = '0 V';
        document.getElementById('leg-hi').textContent = DV.geo_bad + '+ V';
    }
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
        const isSearch = geoHighlight && m.n.toUpperCase().includes(geoHighlight);
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
            // Draw label inside block
            ctx.scale(1, -1); // un-flip Y for text
            ctx.fillStyle = '#e8edf3';
            ctx.font = `${10 / geoTransform.scale}px monospace`;
            ctx.textAlign = 'center';
            ctx.textBaseline = 'middle';
            ctx.fillText(m.n, m.x, -m.y); // negated y because we un-flipped
            ctx.restore();
        }

        if (isHover || isSearch) {
            ctx.strokeStyle = isHover ? '#fff' : '#3b9eff';
            ctx.lineWidth = 1.5 / geoTransform.scale;
            ctx.strokeRect(m.x - hw, m.y - hh, hw * 2, hh * 2);
        }
    }

    // Draw axis cross at center
    ctx.strokeStyle = 'rgba(255,255,255,0.12)';
    ctx.lineWidth = 1 / geoTransform.scale;
    ctx.beginPath();
    ctx.moveTo(-600, 0); ctx.lineTo(600, 0);
    ctx.moveTo(0, -600); ctx.lineTo(0, 600);
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
            renderGeo();
        }
        const ch = chByName[mod.n];
        let html = `<div class="tt-name">${mod.n}</div>`;
        html += `<div class="tt-row"><span class="tt-label">Type</span><span class="tt-val">${mod.t}</span></div>`;
        if (ch) {
            html += `<div class="tt-row"><span class="tt-label">Crate</span><span class="tt-val">${ch.crate} s${ch.slot} ch${ch.channel}</span></div>`;
            html += `<div class="tt-row"><span class="tt-label">VMon</span><span class="tt-val">${ch.vmon.toFixed(2)} V</span></div>`;
            html += `<div class="tt-row"><span class="tt-label">VSet</span><span class="tt-val">${ch.vset.toFixed(2)} V</span></div>`;
            const diff = Math.abs(ch.vmon - ch.vset);
            html += `<div class="tt-row"><span class="tt-label">ΔV</span><span class="tt-val">${diff.toFixed(2)} V</span></div>`;
            html += `<div class="tt-row"><span class="tt-label">Status</span><span class="${ch.on?'tt-on':'tt-off'}">${ch.on?'ON':'OFF'}</span></div>`;
        } else {
            html += `<div class="tt-row"><span class="tt-label">HV</span><span class="tt-val" style="color:var(--text-dim)">not linked</span></div>`;
        }
        tooltip.innerHTML = html;
        tooltip.style.display = 'block';
        tooltip.style.left = (e.clientX + 14) + 'px';
        tooltip.style.top  = (e.clientY + 14) + 'px';
    } else {
        if (geoHover) { geoHover = null; renderGeo(); }
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
    const vsetInput = document.createElement('input');
    vsetInput.type = 'number'; vsetInput.step = '0.1'; vsetInput.placeholder = 'VSet (V)';
    const btnSetV = document.createElement('button');
    btnSetV.className = 'btn-sm btn-set'; btnSetV.textContent = 'Set V';
    const btnOn = document.createElement('button');
    btnOn.className = 'btn-sm btn-on'; btnOn.textContent = 'ON';
    const btnOff = document.createElement('button');
    btnOff.className = 'btn-sm btn-off'; btnOff.textContent = 'OFF';
    actions.append(vsetInput, btnSetV, btnOn, btnOff);
    body.appendChild(actions);
    el.appendChild(body);

    // Populate grid + wire actions
    function refresh() {
        const c = chByName[mod.n];
        let html = '';
        html += `<span class="plbl">Type</span><span class="pval">${mod.t}</span>`;
        html += `<span class="plbl">Position</span><span class="pval">(${mod.x.toFixed(1)}, ${mod.y.toFixed(1)}) mm</span>`;
        html += `<span class="plbl">Size</span><span class="pval">${mod.sx} × ${mod.sy} mm</span>`;
        if (c) {
            html += `<span class="plbl">Crate</span><span class="pval">${c.crate}</span>`;
            html += `<span class="plbl">Slot / Ch</span><span class="pval">${c.slot} / ${c.channel}</span>`;
            html += `<span class="plbl">Model</span><span class="pval">${c.model || '—'}</span>`;
            html += `<span class="plbl">VMon</span><span class="pval">${c.vmon.toFixed(2)} V</span>`;
            html += `<span class="plbl">VSet</span><span class="pval">${c.vset.toFixed(2)} V</span>`;
            html += `<span class="plbl">ΔV</span><span class="pval">${Math.abs(c.vmon - c.vset).toFixed(2)} V</span>`;
            const stAbbr   = c.status ? c.status.split('|')[0] : '';
            const stDetail = c.status ? c.status.split('|')[1] : '';
            html += `<span class="plbl">Status</span><span class="pval ${statusClass(c.status)}" title="${stDetail}">${stAbbr}</span>`;
            vsetInput.value = c.vset.toFixed(1);
        } else {
            html += `<span class="plbl">HV</span><span class="pval" style="color:var(--text-dim)">No linked channel</span>`;
            vsetInput.value = '';
        }
        grid.innerHTML = html;
        const hasChannel = !!c;
        vsetInput.disabled = !hasChannel || !expertMode;
        btnSetV.disabled   = !hasChannel || !expertMode;
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
        c.vset = v; refresh();
    });
    btnOn.addEventListener('click', () => {
        if (!hvMonitor) return;
        const c = chByName[mod.n]; if (!c) return;
        hvMonitor.setChannelPower(c.crate, c.slot, c.channel, true);
        c.on = true; refresh();
    });
    btnOff.addEventListener('click', () => {
        if (!hvMonitor) return;
        const c = chByName[mod.n]; if (!c) return;
        hvMonitor.setChannelPower(c.crate, c.slot, c.channel, false);
        c.on = false; refresh();
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

function statusClass(s) {
    if (!s) return 'status-ok';
    const abbrs = s.split('|')[0];
    // fault if any token is not one of the working-state abbreviations
    const working = new Set(['OFF', 'ON', 'RUP', 'RDN']);
    const isOffOnly = abbrs === 'OFF';
    const hasFault = abbrs.split(' ').some(t => !working.has(t));
    if (hasFault)   return 'status-err';
    if (isOffOnly)  return 'status-ok';
    return 'status-work';   // ON, RUP, RDN
}
