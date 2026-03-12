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
let renderIntervalMs = 200;
let dataDirty        = false;  // set on data/state change, cleared after tbody rebuild
let lastPollTime     = null;   // performance.now() of last hardware poll
let filterStatus = 'all';
let filterCrate  = null;
let searchText   = '';

// Booster HV supplies (TDK-Lambda, via BoosterMonitor WebChannel object)
let boosterMonitor  = null;
let boosterSupplies = [];   // latest snapshot [{idx,name,ip,connected,on,mode,vmon,vset,error}]
let boosterByName   = {};   // modName ('Booster1'…) → live booster data
let boosterDirty    = false;

// Geometry map state
let geoTransform = { x: 0, y: 0, scale: 1 };
let geoDrag      = null;
let geoHover     = null;   // module name under cursor
let geoHighlight = '';     // from search
let geoCanvas, geoCtx, geoWrap;

// Channel lookup by name for geometry
let chByName = {};

// DAQ connection map lookup by module name (loaded from daq_map.json)
let daqByName = {};

// ΔV thresholds (overridden by gui_config.json)
let DV = {
    warn_threshold: 2.0,
    table_ok:       0.5,
    table_warn:     2.0,
};

// Color scale ranges (overridden by gui_config.json)
let CR = {
    vmon_max: 2100,
    vset_max: 2100,
    diff_max: 10.0,
};

// Geometry view settings (overridden by gui_config.json)
let GV = {
    extent: 780,   // half-width in mm for resetGeoView; must cover booster cards at x=680+40=720
};

// ═════════════════════════════════════════════════════════════════════
//  QWebChannel bootstrap
// ═════════════════════════════════════════════════════════════════════
document.addEventListener('DOMContentLoaded', () => {
    new QWebChannel(qt.webChannelTransport, channel => {
        hvMonitor = channel.objects.hvMonitor;
        boosterMonitor = channel.objects.boosterMonitor;
        if (boosterMonitor) {
            boosterMonitor.boosterUpdated.connect(jsonStr => {
                boosterSupplies = JSON.parse(jsonStr);
                boosterByName = {}; boosterSupplies.forEach(s => { boosterByName[s.name] = s; });
                boosterDirty = true;
            });
            boosterMonitor.readAll(jsonStr => {
                boosterSupplies = JSON.parse(jsonStr);
                boosterByName = {}; boosterSupplies.forEach(s => { boosterByName[s.name] = s; });
                boosterDirty = true;
            });
        }

        // Data update only — no render; the render loop handles display
        hvMonitor.channelsUpdated.connect(jsonStr => {
            allChannels = JSON.parse(jsonStr);
            lastPollTime = performance.now();
            rebuildChMap();
            const crateKey = [...new Set(allChannels.map(c => c.crate))].sort().join('|');
            if (crateKey !== window._crateKey) { window._crateKey = crateKey; populateCrateChips(); }
            dataDirty = true;
            // refreshAllPopups() is now called from renderActiveTab() when dirty,
            // so we don't rebuild popup DOM on every data tick outside the render loop.
        });

        hvMonitor.readAll(jsonStr => {
            allChannels = JSON.parse(jsonStr);
            rebuildChMap();
            populateCrateChips();
            dataDirty = true;
            // Load module geometry from backend JSON file
            hvMonitor.getModuleGeometry(geoJson => {
                MODULES = JSON.parse(geoJson);
                MODULES.forEach(m => { MOD_MAP[m.n] = m; });
                rebuildColorCache();   // MODULES just arrived — populate colour cache
                console.log('Loaded ' + MODULES.length + ' modules');
            });
            // Load DAQ connection map
            hvMonitor.getDAQMap(daqJson => {
                daqByName = {};
                JSON.parse(daqJson).forEach(e => {
                    daqByName[e.name] = { crate: e.crate, slot: e.slot, channel: e.channel };
                });
                console.log('Loaded DAQ map: ' + Object.keys(daqByName).length + ' entries');
            });
            // Load GUI config (ΔV thresholds, intervals, etc.)
            hvMonitor.getGuiConfig(cfgJson => {
                try {
                    const cfg = JSON.parse(cfgJson);
                    if (cfg.deltaV)     Object.assign(DV, cfg.deltaV);
                    if (cfg.colorRange) Object.assign(CR, cfg.colorRange);
                    if (cfg.geoView)    Object.assign(GV, cfg.geoView);
                    if (cfg.intervals) {
                        // Apply render interval
                        if (cfg.intervals.renderMs) {
                            renderIntervalMs = cfg.intervals.renderMs;
                        }
                        // Apply poll interval
                        if (cfg.intervals.pollMs) {
                            const sec = cfg.intervals.pollMs / 1000;
                            const ps = document.getElementById('poll-slider');
                            if (sec > parseFloat(ps.max)) ps.max = Math.ceil(sec);
                            ps.value = sec;
                            document.getElementById('poll-val').textContent = sec.toFixed(1);
                            if (hvMonitor) hvMonitor.setPollInterval(cfg.intervals.pollMs);
                        }
                    }
                    console.log('GUI config loaded — DV:', DV, 'CR:', CR, 'GV:', GV);
                } catch (e) {
                    console.warn('Failed to parse gui_config.json, using defaults', e);
                }
            });
            document.getElementById('loading').classList.add('hidden');
            setPillConnected(true);
        });
    });

    initTableUI();
    initTabs();
    initGeoMap();
    initBoosterTab();

    // Render loop — driven by requestAnimationFrame so it never stacks,
    // skips hidden tabs automatically, and self-throttles to display rate.
    // renderIntervalMs caps how often we actually re-render, so we don't
    // burn CPU on every 16 ms frame when data only changes every 2 s.
    let lastRenderTs  = 0;
    let lastFooterSec = -1;  // guards "X s ago" footer — only write on whole-second changes
    let lastClockSec  = -1;  // guards header clock — same
    function renderLoop(ts) {
        // Re-render if CAEN data changed, or if booster data changed and the
        // geo tab is active (booster blocks are drawn on the geo canvas).
        const geoActive = document.querySelector('.tab-content.active')?.id === 'geo-tab';
        const shouldRender = (dataDirty && allChannels.length > 0)
                           || (boosterDirty && geoActive);
        if (shouldRender && (ts - lastRenderTs) >= renderIntervalMs) {
            renderActiveTab();
            lastRenderTs = ts;
        }
        // Tick "X s ago" footer every second
        if (lastPollTime !== null) {
            const sec = Math.floor((ts - lastPollTime) / 1000);
            if (sec !== lastFooterSec) {
                lastFooterSec = sec;
                updatePollAge(sec);
            }
        }
        // Tick header clock every second
        const nowSec = Math.floor(Date.now() / 1000);
        if (nowSec !== lastClockSec) {
            lastClockSec = nowSec;
            updateHeaderClock();
        }
        requestAnimationFrame(renderLoop);
    }
    requestAnimationFrame(renderLoop);


});

function rebuildChMap() {
    chByName = {};
    allChannels.forEach(ch => { chByName[ch.name] = ch; });
    rebuildColorCache();   // invalidate geo colour cache on new data
}

// Safe number formatter — returns '—' if value is null/undefined/NaN
function fmt(val, decimals) {
    return (val == null || isNaN(val)) ? '—' : Number(val).toFixed(decimals);
}

function renderActiveTab() {
    const active = document.querySelector('.tab-content.active');
    if (active.id === 'table-tab') renderTable();     // calls updateFooter internally
    else if (active.id === 'geo-tab') { renderGeo(); updateFooter(); }
    else if (active.id === 'booster-tab') { if (boosterDirty) { renderBoosterCards(); boosterDirty = false; } }
    // Refresh open popups here, in the render loop, not on every data tick
    if (popups.size > 0) refreshAllPopups();
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
            dataDirty = true; renderActiveTab();
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
            populateCrateChips();
            dataDirty = true; renderActiveTab();
        });
    });
    document.getElementById('search').addEventListener('input', e => {
        searchText = e.target.value.trim().toLowerCase();
        dataDirty = true; renderTable();
    });
    document.getElementById('poll-slider').addEventListener('input', e => {
        const sec = parseFloat(e.target.value);
        document.getElementById('poll-val').textContent = sec.toFixed(1);
        if (hvMonitor) hvMonitor.setPollInterval(Math.round(sec * 1000));
    });
    document.querySelectorAll('#summary-strip .summary-item').forEach(item => {
        item.addEventListener('click', () => {
            document.querySelectorAll('#summary-strip .summary-item').forEach(s => s.classList.remove('active-filter'));
            item.classList.add('active-filter');
            filterStatus = item.dataset.filter;
            dataDirty = true; renderTable();
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
            dataDirty = true; renderTable();
        });
    });

    // ── Bulk ON / OFF buttons ───────────────────────────────────────────
    document.getElementById('btn-all-on').addEventListener('click', () => {
        if (!hvMonitor || allChannels.length === 0) return;
        const n = allChannels.length;
        if (!confirm(`Turn ON all ${n} channels?\n\nThis will energise every HV channel across all crates.`)) return;
        hvMonitor.setAllPower(true);
        allChannels.forEach(ch => { ch.on = true; });
        dataDirty = true; renderActiveTab();
    });

    document.getElementById('btn-all-off').addEventListener('click', () => {
        if (!hvMonitor || allChannels.length === 0) return;
        const n = allChannels.length;
        if (!confirm(`Turn OFF all ${n} channels?\n\nThis will de-energise every HV channel across all crates.`)) return;
        hvMonitor.setAllPower(false);
        allChannels.forEach(ch => { ch.on = false; });
        dataDirty = true; renderActiveTab();
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
        dataDirty = true; boosterDirty = true; renderActiveTab();
    });
}

function populateCrateChips() {
    const crates = [...new Set(allChannels.map(c => c.crate))].sort();
    const wrap = document.getElementById('crate-chips');
    wrap.innerHTML = '';
    const allChip = document.createElement('button');
    allChip.className = 'chip active'; allChip.dataset.crate = ''; allChip.textContent = `All ${crates.length} Crates`;
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
    dataDirty = true; renderTable();
}

function isPrimary(ch) {
    return ch.channel === 0 && ch.name && ch.name.toUpperCase().includes('PRIMARY');
}

function renderTable() {
    // Don't clobber an in-progress edit
    if (document.activeElement && (
            document.activeElement.classList.contains('vset-inline') ||
            document.activeElement.classList.contains('name-inline'))) return;

    // ── Filter & sort ─────────────────────────────────────────────────
    let data = allChannels;
    if (filterStatus === 'on')      data = data.filter(c => c.on);
    else if (filterStatus === 'off')data = data.filter(c => !c.on);
    else if (filterStatus === 'primary') data = data.filter(c => isPrimary(c));
    else if (filterStatus === 'warn')    data = data.filter(c => isSettled(c) && c.vmon != null && c.vset != null && Math.abs(c.vmon - c.vset) > DV.warn_threshold);
    else if (filterStatus === 'fault')   data = data.filter(c => statusClass(c.status) === 'status-err');
    if (filterCrate) data = data.filter(c => c.crate === filterCrate);
    if (searchText) {
        const colonIdx = searchText.indexOf(':');
        if (colonIdx > 0) {
            const col = searchText.slice(0, colonIdx).trim();
            const val = searchText.slice(colonIdx + 1).trim();
            const colMap = { crate:'crate', slot:'slot', ch:'channel', channel:'channel',
                             name:'name', model:'model', vmon:'vmon', vset:'vset' };
            const field = colMap[col];
            data = field
                ? data.filter(c => String(c[field]).toLowerCase().includes(val))
                : data.filter(c => (c.crate+' '+c.slot+' '+c.channel+' '+c.name+' '+c.model).toLowerCase().includes(searchText));
        } else {
            data = data.filter(c => (c.crate+' '+c.slot+' '+c.channel+' '+c.name+' '+c.model).toLowerCase().includes(searchText));
        }
    }
    data = data.slice().sort((a, b) => {
        let va = a[sortCol], vb = b[sortCol];
        if (sortCol === 'diff') {
            va = (a.vmon != null && a.vset != null) ? Math.abs(a.vmon - a.vset) : -1;
            vb = (b.vmon != null && b.vset != null) ? Math.abs(b.vmon - b.vset) : -1;
        }
        if (typeof va === 'string') return sortAsc ? va.localeCompare(vb) : vb.localeCompare(va);
        if (typeof va === 'boolean') { va = va?1:0; vb = vb?1:0; }
        return sortAsc ? va - vb : vb - va;
    });

    // ── Surgical DOM patch ────────────────────────────────────────────
    // Build keyed rows once; on subsequent renders only update cells that
    // actually changed.  This avoids tearing down and rebuilding ~1200 rows
    // on every data tick.
    const tbody = document.getElementById('ch-body');

    // Build a map of currently displayed rows keyed by "crate|slot|channel"
    const existingRows = new Map();
    for (const tr of tbody.rows) {
        const k = tr.dataset.key;
        if (k) existingRows.set(k, tr);
    }

    const fragment = document.createDocumentFragment();

    for (const ch of data) {
        const key = ch.crate + '|' + ch.slot + '|' + ch.channel;
        const diff   = (ch.vmon != null && ch.vset != null) ? Math.abs(ch.vmon - ch.vset) : null;
        const dcls   = !ch.on ? 'diff-ok' : (diff == null || diff < DV.table_ok) ? 'diff-ok' : diff < DV.table_warn ? 'diff-warn' : 'diff-bad';
        const sc     = statusClass(ch.status);
        const isWarn = isSettled(ch) && ch.vmon != null && ch.vset != null && Math.abs(ch.vmon - ch.vset) > DV.warn_threshold;
        const dotCls = sc === 'status-err' ? 'fault' : isWarn ? 'warn' : ch.on ? 'on' : 'off';
        const pwrCls = ch.on ? 'on' : 'off';
        const prim   = isPrimary(ch);
        const stAbbr   = ch.status ? ch.status.split('|')[0] : '';
        const stDetail = ch.status ? ch.status.split('|')[1] : '';

        let tr = existingRows.get(key);
        if (!tr) {
            // ── Create new row (first render, or row newly visible after filter) ──
            tr = document.createElement('tr');
            tr.dataset.key = key;
            tr.dataset.expert = expertMode ? '1' : '0';
            existingRows.delete(key);  // mark as still-needed (same as patch branch)

            // td0: status dot
            const td0 = document.createElement('td');
            const dot = document.createElement('span');
            dot.className = 'status-dot ' + dotCls;
            td0.appendChild(dot);
            tr.appendChild(td0);

            // td1: crate  td2: slot  td3: channel  td4: model
            for (const txt of [ch.crate, ch.slot, ch.channel, ch.model||'—']) {
                const td = document.createElement('td');
                td.textContent = txt;
                tr.appendChild(td);
            }
            // td1 gets the IP title
            tr.cells[1].title = ch.ip;

            // td5: name
            const td5 = document.createElement('td');
            td5.innerHTML = buildNameCell(ch, prim);
            tr.appendChild(td5);

            // td6: vmon
            const td6 = document.createElement('td');
            td6.style.textAlign = 'right';
            td6.textContent = fmt(ch.vmon, 2);
            tr.appendChild(td6);

            // td7: vset
            const td7 = document.createElement('td');
            td7.style.textAlign = 'right';
            td7.innerHTML = buildVsetCell(ch);
            tr.appendChild(td7);

            // td8: diff
            const td8 = document.createElement('td');
            td8.className = dcls;
            td8.style.textAlign = 'right';
            td8.textContent = fmt(diff, 2);
            tr.appendChild(td8);

            // td9: imon
            const td9 = document.createElement('td');
            td9.style.textAlign = 'right';
            td9.style.color = ch.iSupported === false ? 'var(--text-dim)' : '';
            td9.textContent = ch.iSupported === false ? 'N/A' : fmt(ch.imon, 3);
            tr.appendChild(td9);

            // td10: iset
            const td10 = document.createElement('td');
            td10.style.textAlign = 'right';
            td10.innerHTML = buildIsetCell(ch);
            tr.appendChild(td10);

            // td11: status badges (no ON/OFF — Pwr column covers power state)
            const td11 = document.createElement('td');
            td11.innerHTML = statusBadgesHtml(ch);
            tr.appendChild(td11);

            // td12: power button
            const td12 = document.createElement('td');
            td12.style.textAlign = 'center';
            const btn = document.createElement('button');
            btn.className = 'pwr-btn ' + pwrCls;
            btn.textContent = ch.on ? 'ON' : 'OFF';
            btn.onclick = makeToggle(ch.crate, ch.slot, ch.channel, ch.on);
            td12.appendChild(btn);
            tr.appendChild(td12);

            tr.className = prim ? 'primary-row' : '';
        } else {
            // ── Patch only what changed ───────────────────────────────────────
            existingRows.delete(key);  // mark as still-needed

            // status dot
            const dot = tr.cells[0].firstElementChild;
            const wantDot = 'status-dot ' + dotCls;
            if (dot.className !== wantDot) dot.className = wantDot;

            // vmon (td6)
            const vmonTxt = fmt(ch.vmon, 2);
            if (tr.cells[6].textContent !== vmonTxt) tr.cells[6].textContent = vmonTxt;

            // diff class + value (td8)
            if (tr.cells[8].className !== dcls) tr.cells[8].className = dcls;
            const diffTxt = fmt(diff, 2);
            if (tr.cells[8].textContent !== diffTxt) tr.cells[8].textContent = diffTxt;

            // imon (td9)
            const imonTxt = ch.iSupported === false ? 'N/A' : fmt(ch.imon, 3);
            if (tr.cells[9].textContent !== imonTxt) tr.cells[9].textContent = imonTxt;

            // status badges (td11)
            const stHtml = statusBadgesHtml(ch);
            if (tr.cells[11].innerHTML !== stHtml) tr.cells[11].innerHTML = stHtml;

            // power button (td12)
            const pbtn = tr.cells[12].firstElementChild;
            const wantPwr = 'pwr-btn ' + pwrCls;
            if (pbtn.className !== wantPwr) pbtn.className = wantPwr;
            const pwrTxt = ch.on ? 'ON' : 'OFF';
            if (pbtn.textContent !== pwrTxt) {
                pbtn.textContent = pwrTxt;
                pbtn.onclick = makeToggle(ch.crate, ch.slot, ch.channel, ch.on);
            }

            // Expert mode cells (name td5, vset td7, iset td10) — rebuild if mode changed
            const trExpert = tr.dataset.expert === '1';
            if (trExpert !== expertMode) {
                tr.cells[5].innerHTML  = buildNameCell(ch, prim);
                tr.cells[7].innerHTML  = buildVsetCell(ch);
                tr.cells[10].innerHTML = buildIsetCell(ch);
                tr.dataset.expert = expertMode ? '1' : '0';
            }
        }
        fragment.appendChild(tr);
    }

    // Remove rows no longer in filtered set
    existingRows.forEach(tr => tr.remove());

    // Re-append in sorted order (moves don't re-parse HTML, just reorder)
    tbody.appendChild(fragment);

    dataDirty = false;

    // ── Summary counts (computed once from the same pass) ─────────────
    const total   = allChannels.length;
    let primCnt=0, onCnt=0, warns=0, faults=0;
    for (const c of allChannels) {
        if (isPrimary(c)) primCnt++;
        if (c.on) onCnt++;
        if (isSettled(c) && c.vmon != null && c.vset != null && Math.abs(c.vmon - c.vset) > DV.warn_threshold) warns++;
        if (statusClass(c.status) === 'status-err') faults++;
    }
    document.getElementById('s-total').textContent   = total;
    document.getElementById('s-primary').textContent = primCnt;
    document.getElementById('s-on').textContent      = onCnt;
    document.getElementById('s-off').textContent     = total - onCnt;
    document.getElementById('s-warn').textContent    = warns;
    document.getElementById('s-fault').textContent   = faults;

    // Pass filtered count to footer without re-filtering
    updateFooter(data.length, total);
}

// Helper: creates a closure for the power button onclick (avoids string eval)
function makeToggle(crate, slot, channel, currentOn) {
    return function() { togglePower(crate, slot, channel, !currentOn); };
}

function buildNameCell(ch, prim) {
    const badge = prim ? '<span class="primary-badge">Primary</span>' : '';
    if (expertMode) {
        return `<input class="name-inline" type="text" maxlength="12" value="${ch.name||''}"
                     onchange="inlineSetName('${ch.crate}',${ch.slot},${ch.channel},this.value)"
                   ><button class="vset-apply"
                     onclick="inlineSetName('${ch.crate}',${ch.slot},${ch.channel},this.previousElementSibling.value)"
                   >\u2713</button>${badge}`;
    }
    return (ch.name||'—') + badge;
}

function buildVsetCell(ch) {
    if (expertMode) {
        return `<input class="vset-inline" type="text" value="${fmt(ch.vset, 2)}"
                     onchange="inlineSetVoltage('${ch.crate}',${ch.slot},${ch.channel},this.value)"
                   ><button class="vset-apply"
                     onclick="inlineSetVoltage('${ch.crate}',${ch.slot},${ch.channel},this.previousElementSibling.value)"
                   >\u2713</button>`;
    }
    return `<span style="color:var(--text-dim)">${fmt(ch.vset, 2)}</span>`;
}

function buildIsetCell(ch) {
    if (ch.iSupported === false) {
        return `<span style="color:var(--text-dim)">N/A</span>`;
    }
    if (expertMode) {
        return `<input class="vset-inline" type="text" value="${fmt(ch.iset, 1)}"
                     onchange="inlineSetCurrent('${ch.crate}',${ch.slot},${ch.channel},this.value)"
                   ><button class="vset-apply"
                     onclick="inlineSetCurrent('${ch.crate}',${ch.slot},${ch.channel},this.previousElementSibling.value)"
                   >\u2713</button>`;
    }
    return `<span style="color:var(--text-dim)">${fmt(ch.iset, 1)}</span>`;
}

// shownRows / totalRows are passed in by renderTable to avoid re-filtering.
// When called from renderGeo (no args), falls back to module counts only.
function updateFooter(shownRows, totalRows) {
    // footer age label is maintained by the rAF loop via updatePollAge()

    const active = document.querySelector('.tab-content.active');
    if (active && active.id === 'geo-tab') {
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
        const total = totalRows != null ? totalRows : allChannels.length;
        const shown = shownRows != null ? shownRows : total;
        if (shown === total) {
            document.getElementById('row-count').textContent =
                `${total} channels | ${MODULES.length} modules`;
        } else {
            document.getElementById('row-count').textContent =
                `${shown} / ${total} channels shown | ${MODULES.length} modules`;
        }
    }
}

function updateHeaderClock() {
    const now = new Date();
    const months = ['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'];
    const mon = months[now.getMonth()];
    const day = String(now.getDate()).padStart(2, '0');
    const yr  = now.getFullYear();
    const hh  = String(now.getHours()).padStart(2, '0');
    const mm  = String(now.getMinutes()).padStart(2, '0');
    const ss  = String(now.getSeconds()).padStart(2, '0');
    document.getElementById('header-clock').textContent = `${mon} ${day} ${yr}, ${hh}:${mm}:${ss}`;
}

function updatePollAge(sec) {
    let label;
    if (sec < 2)       label = 'Updated just now';
    else if (sec < 60) label = `Updated ${sec} s ago`;
    else               label = `Updated ${Math.floor(sec / 60)} min ago`;
    document.getElementById('last-update').textContent = label;
}

function togglePower(crate, slot, channel, on) {
    if (!hvMonitor) return;
    hvMonitor.setChannelPower(crate, slot, channel, on);
    const ch = allChannels.find(c => c.crate===crate && c.slot===slot && c.channel===channel);
    if (ch) ch.on = on;
    dataDirty = true; renderActiveTab();
}

function inlineSetVoltage(crate, slot, channel, value) {
    if (!hvMonitor || !expertMode) return;
    const v = parseFloat(value);
    if (isNaN(v) || v < 0) return;
    hvMonitor.setChannelVoltage(crate, slot, channel, v);
    const ch = allChannels.find(c => c.crate===crate && c.slot===slot && c.channel===channel);
    if (ch) { ch.vset = v; dataDirty = true; }
}

function inlineSetCurrent(crate, slot, channel, value) {
    if (!hvMonitor || !expertMode) return;
    const ch = allChannels.find(c => c.crate===crate && c.slot===slot && c.channel===channel);
    if (!ch || ch.iSupported === false) return;
    const v = parseFloat(value);
    if (isNaN(v) || v < 0) return;
    hvMonitor.setChannelCurrent(crate, slot, channel, v);
    ch.iset = v; dataDirty = true;
}

function inlineSetName(crate, slot, channel, value) {
    if (!hvMonitor || !expertMode) return;
    const n = value.trim();
    if (!n) return;
    hvMonitor.setChannelName(crate, slot, channel, n);
    const ch = allChannels.find(c => c.crate===crate && c.slot===slot && c.channel===channel);
    if (ch) ch.name = n;
    rebuildChMap();   // chByName keyed by name — must rebuild after rename
    dataDirty = true; renderActiveTab();
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
        if (statusClass(ch.status) === 'status-err')  return '#f56565';
        if (!ch.on)                                    return '#4a5568';
        if (isSettled(ch) && ch.vmon != null && ch.vset != null && Math.abs(ch.vmon - ch.vset) > DV.warn_threshold)
                                                       return '#eab308';
        return '#2dd4a0';
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
            const ttSt = statusBadgesHtml(ch);
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
            const ppSt = statusBadgesHtml(c);
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

// A channel is "settled" (eligible for ΔV warning) only when fully ON —
// not while ramping up (RUP), ramping down (RDN), or off (OFF).
function isSettled(ch) {
    if (!ch.status) return false;
    const tokens = ch.status.split('|')[0].trim().split(/\s+/);
    return tokens.length > 0 && tokens.every(t => t === 'ON');
}

function dotClass(ch) {
    if (statusClass(ch.status) === 'status-err') return 'fault';
    if (isSettled(ch) && ch.vmon != null && ch.vset != null &&
        Math.abs(ch.vmon - ch.vset) > DV.warn_threshold) return 'warn';
    return ch.on ? 'on' : 'off';
}

// ── Status display helpers ───────────────────────────────────────────
// statusBadgesHtml(ch)  — RUP/RDN (green) | fault tokens (red) | ΔV (amber)
//   Returns '' for a clean ON/OFF channel.
//   Used identically by the table Status column, tooltip, and popup.
// pwrHtml(ch)           — ON (green) or OFF (dim).
//   Used by the tooltip and popup Pwr row; table has its own Pwr column.

function statusBadgesHtml(ch) {
    const abbr   = ch.status ? ch.status.split('|')[0].trim() : '';
    const detail = ch.status ? (ch.status.split('|')[1] || '').trim() : '';
    const sc     = statusClass(ch.status);
    const isWarn = isSettled(ch) && ch.vmon != null && ch.vset != null
                   && Math.abs(ch.vmon - ch.vset) > DV.warn_threshold;

    const badges = [];

    // Ramp states — meaningful independent of power context
    if (abbr === 'RUP' || abbr === 'RDN') {
        badges.push(`<span class="st-ramp">${abbr}</span>`);
    }

    // Fault tokens — strip pure power-state tokens, keep the rest
    if (sc === 'status-err') {
        const faultTokens = abbr.split(/\s+/).filter(
            t => t !== 'ON' && t !== 'OFF' && t !== 'RUP' && t !== 'RDN' && t !== '');
        if (faultTokens.length > 0) {
            const tip = detail ? ` title="${detail}"` : '';
            badges.push(`<span class="st-fault"${tip}>${faultTokens.join('\u2009')}</span>`);
        } else if (abbr && abbr !== 'ON' && abbr !== 'OFF' && abbr !== 'RUP' && abbr !== 'RDN') {
            const tip = detail ? ` title="${detail}"` : '';
            badges.push(`<span class="st-fault"${tip}>${abbr}</span>`);
        }
    }

    // ΔV warning
    if (isWarn) badges.push('<span class="st-warn">\u0394V</span>');

    return badges.length ? `<span class="st-badges">${badges.join(' ')}</span>` : '';
}

// Power state badge for tooltip / popup Pwr row
function pwrHtml(ch) {
    const abbr = ch.status ? ch.status.split('|')[0].trim() : '';
    if (abbr === 'RUP' || abbr === 'RDN') return `<span class="st-ramp">${abbr}</span>`;
    return ch.on ? '<span class="st-on">ON</span>' : '<span class="st-off">OFF</span>';
}



// ═════════════════════════════════════════════════════════════════════
//  BOOSTER – geo popup + tab card rendering
// ═════════════════════════════════════════════════════════════════════

function escHtml(str) {
    return String(str)
        .replace(/&/g, '&amp;').replace(/</g, '&lt;')
        .replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

// ── Booster floating popup (opened from geo view click) ───────────────
function openBoosterPopup(mod) {
    if (popups.has(mod.n)) { bringToFront(popups.get(mod.n).el); return; }

    const el = document.createElement('div');
    el.className = 'mod-popup';
    const offset = (popups.size % 8) * 26;
    el.style.left = (120 + offset) + 'px';
    el.style.top  = (100 + offset) + 'px';

    // Header
    const header = document.createElement('div');
    header.className = 'popup-header';
    header.innerHTML = `<h2>${escHtml(mod.n)}</h2><button class="popup-close" title="Close">✕</button>`;
    el.appendChild(header);

    // Body
    const body = document.createElement('div');
    body.className = 'popup-body';

    const grid = document.createElement('div');
    grid.className = 'popup-grid';
    body.appendChild(grid);

    // Actions
    const actions = document.createElement('div');
    actions.className = 'popup-actions';

    const rowV = document.createElement('div');
    rowV.className = 'popup-action-row';
    const vsetInput = document.createElement('input');
    vsetInput.type = 'text'; vsetInput.setAttribute('inputmode', 'decimal');
    vsetInput.placeholder = 'VSet (V)';
    const btnSetV = document.createElement('button');
    btnSetV.className = 'btn-sm btn-set'; btnSetV.textContent = 'Set V';
    rowV.append(vsetInput, btnSetV);

    const rowI = document.createElement('div');
    rowI.className = 'popup-action-row';
    const isetInput = document.createElement('input');
    isetInput.type = 'text'; isetInput.setAttribute('inputmode', 'decimal');
    isetInput.placeholder = 'ISet (A)';
    const btnSetI = document.createElement('button');
    btnSetI.className = 'btn-sm btn-set'; btnSetI.textContent = 'Set I';
    rowI.append(isetInput, btnSetI);

    const rowPwr = document.createElement('div');
    rowPwr.className = 'popup-action-row';
    const btnOn  = document.createElement('button');
    btnOn.className  = 'btn-sm btn-on';  btnOn.textContent  = 'ON';
    const btnOff = document.createElement('button');
    btnOff.className = 'btn-sm btn-off'; btnOff.textContent = 'OFF';
    rowPwr.append(btnOn, btnOff);

    actions.append(rowV, rowI, rowPwr);
    body.appendChild(actions);
    el.appendChild(body);

    // Find this supply's index from boosterSupplies
    function supplyIdx() {
        const i = boosterSupplies.findIndex(s => s.name === mod.n);
        return i;  // -1 if not yet polled
    }

    function refresh() {
        const bs = boosterByName[mod.n];
        let html = '';
        html += `<span class="plbl">IP</span><span class="pval" style="font-family:monospace">${escHtml(mod.ip||'—')}</span>`;
        if (bs) {
            const connSpan = bs.connected
                ? '<span class="st-on">Connected</span>'
                : '<span class="st-off">Offline</span>';
            html += `<span class="plbl">Link</span><span class="pval">${connSpan}</span>`;
            if (bs.connected) {
                html += `<span class="plbl">VMon / VSet</span>`
                       + `<span class="pval"><span class="pval-live">${bs.vmon != null ? bs.vmon.toFixed(2)+' V' : '—'}</span>`
                       + ` / ${bs.vset != null ? bs.vset.toFixed(2)+' V' : '—'}</span>`;
                html += `<span class="plbl">IMon / ISet</span>`
                       + `<span class="pval"><span class="pval-live">${bs.imon != null ? bs.imon.toFixed(3)+' A' : '—'}</span>`
                       + ` / ${bs.iset != null ? bs.iset.toFixed(3)+' A' : '—'}</span>`;
                const modeBadge = bs.mode
                    ? `<span class="booster-mode-badge ${bs.mode.toUpperCase()==='CV'?'mode-cv':'mode-cc'}">${escHtml(bs.mode)}</span>`
                    : '—';
                html += `<span class="plbl">Mode</span><span class="pval">${modeBadge}</span>`;
                html += `<span class="plbl">Pwr</span><span class="pval">${bs.on ? '<span class="st-on">ON</span>' : '<span class="st-off">OFF</span>'}</span>`;
                if (bs.error) html += `<span class="plbl">Error</span><span class="pval" style="color:var(--red)">${escHtml(bs.error)}</span>`;
                if (vsetInput.value === '' && bs.vset != null)
                    vsetInput.value = bs.vset.toFixed(2);
                if (isetInput.value === '' && bs.iset != null)
                    isetInput.value = bs.iset.toFixed(3);
            }
        } else {
            html += `<span class="plbl">Link</span><span class="pval" style="color:var(--text-dim)">waiting…</span>`;
        }
        grid.innerHTML = html;
        const hasConn = !!(bs && bs.connected);
        // SetV / SetI require both connection and expert mode
        vsetInput.disabled = !hasConn || !expertMode;
        btnSetV.disabled   = !hasConn || !expertMode;
        vsetInput.style.opacity = (hasConn && expertMode) ? '1' : '0.35';
        btnSetV.style.opacity   = (hasConn && expertMode) ? '1' : '0.35';
        isetInput.disabled = !hasConn || !expertMode;
        btnSetI.disabled   = !hasConn || !expertMode;
        isetInput.style.opacity = (hasConn && expertMode) ? '1' : '0.35';
        btnSetI.style.opacity   = (hasConn && expertMode) ? '1' : '0.35';
        // ON/OFF only requires connection
        btnOn.disabled  = !hasConn;
        btnOff.disabled = !hasConn;
        btnOn.style.opacity  = hasConn ? '1' : '0.35';
        btnOff.style.opacity = hasConn ? '1' : '0.35';
    }
    refresh();
    popups.set(mod.n, { el, refresh });

    header.querySelector('.popup-close').addEventListener('click', () => closeModPopup(mod.n));

    btnSetV.addEventListener('click', () => {
        if (!boosterMonitor || !expertMode) return;
        const v = parseFloat(vsetInput.value);
        if (isNaN(v) || v < 0) { vsetInput.style.borderColor = 'var(--red)'; return; }
        vsetInput.style.borderColor = '';
        const idx = supplyIdx(); if (idx < 0) return;
        boosterMonitor.setVoltage(idx, v);
        const bs = boosterByName[mod.n]; if (bs) bs.vset = v;
        boosterDirty = true; refresh();
    });
    vsetInput.addEventListener('keydown', e => { if (e.key === 'Enter') btnSetV.click(); });

    btnSetI.addEventListener('click', () => {
        if (!boosterMonitor || !expertMode) return;
        const v = parseFloat(isetInput.value);
        if (isNaN(v) || v < 0) { isetInput.style.borderColor = 'var(--red)'; return; }
        isetInput.style.borderColor = '';
        const idx = supplyIdx(); if (idx < 0) return;
        boosterMonitor.setCurrent(idx, v);
        const bs = boosterByName[mod.n]; if (bs) bs.iset = v;
        boosterDirty = true; refresh();
    });
    isetInput.addEventListener('keydown', e => { if (e.key === 'Enter') btnSetI.click(); });

    btnOn.addEventListener('click', () => {
        if (!boosterMonitor) return;
        const idx = supplyIdx(); if (idx < 0) return;
        boosterMonitor.setOutput(idx, true);
        const bs = boosterByName[mod.n]; if (bs) bs.on = true;
        boosterDirty = true; refresh();
    });
    btnOff.addEventListener('click', () => {
        if (!boosterMonitor) return;
        const idx = supplyIdx(); if (idx < 0) return;
        boosterMonitor.setOutput(idx, false);
        const bs = boosterByName[mod.n]; if (bs) bs.on = false;
        boosterDirty = true; refresh();
    });

    // Drag via header
    let drag = null;
    header.addEventListener('mousedown', e => {
        if (e.target.classList.contains('popup-close')) return;
        bringToFront(el);
        drag = { sx: e.clientX, sy: e.clientY, ox: el.offsetLeft, oy: el.offsetTop };
        e.preventDefault();
    });
    window.addEventListener('mousemove', e => {
        if (!drag) return;
        let nx = drag.ox + (e.clientX - drag.sx);
        let ny = drag.oy + (e.clientY - drag.sy);
        nx = Math.max(0, Math.min(window.innerWidth  - el.offsetWidth,  nx));
        ny = Math.max(0, Math.min(window.innerHeight - el.offsetHeight, ny));
        el.style.left = nx + 'px'; el.style.top = ny + 'px';
    });
    window.addEventListener('mouseup', () => { drag = null; });
    el.addEventListener('mousedown', () => bringToFront(el));

    document.body.appendChild(el);
    bringToFront(el);
}

// ── refreshAllPopups: extend to also refresh booster popups ───────────
// (The existing refreshAllPopups calls popup.refresh() for all entries
//  in the popups map, which now includes booster popups — no change needed.)

// ── Booster tab card rendering ────────────────────────────────────────
function initBoosterTab() { /* cards built lazily by renderBoosterCards */ }

function renderBoosterCards() {
    const container = document.getElementById('booster-cards');
    if (!container) return;
    boosterSupplies.forEach((s, idx) => {
        let card = document.getElementById('booster-card-' + idx);
        if (!card) {
            card = buildBoosterCard(idx, s);
            container.appendChild(card);
        } else {
            updateBoosterCard(card, idx, s);
        }
    });
}

function buildBoosterCard(idx, s) {
    const card = document.createElement('div');
    card.id = 'booster-card-' + idx;
    card.className = 'booster-card ' + boosterCardClass(s);
    card.innerHTML = boosterCardInnerHtml(s);
    wireBoosterCard(card, idx);
    return card;
}

function boosterCardInnerHtml(s) {
    const connCls = s.connected ? 'conn-ok' : 'conn-err';
    const connTxt = s.connected ? 'Connected' : 'Offline';
    const vsetVal = s.vset != null ? s.vset.toFixed(2) : '';
    const isetVal = s.iset != null ? s.iset.toFixed(3) : '';
    return `
<div class="booster-card-head">
  <div>
    <div class="booster-card-name">${escHtml(s.name)}</div>
    <div class="booster-card-ip">${escHtml(s.ip)}</div>
  </div>
  <span class="booster-conn-badge ${connCls}">${connTxt}</span>
</div>
<div class="booster-readout">
  <span class="booster-lbl">VMon</span>
  <span class="booster-val val-live" data-field="vmon">${s.vmon!=null?s.vmon.toFixed(2)+' V':'—'}</span>
  <span class="booster-lbl">VSet</span>
  <span class="booster-val" data-field="vset">${s.vset!=null?s.vset.toFixed(2)+' V':'—'}</span>
  <span class="booster-lbl">IMon</span>
  <span class="booster-val val-live" data-field="imon">${s.imon!=null?s.imon.toFixed(3)+' A':'—'}</span>
  <span class="booster-lbl">ISet</span>
  <span class="booster-val" data-field="iset">${s.iset!=null?s.iset.toFixed(3)+' A':'—'}</span>
  <span class="booster-lbl booster-lbl-full">Mode</span>
  <span class="booster-val booster-val-full" data-field="mode">${boosterModeBadge(s.mode)}</span>
  <span class="booster-lbl booster-lbl-full">Output</span>
  <span class="booster-val booster-val-full" data-field="pwr">${boosterPwrBadge(s)}</span>
</div>
<div class="booster-controls">
  <div class="booster-controls-vset">
    <input class="booster-vset-input" type="text" inputmode="decimal"
           placeholder="VSet (V)" value="${escHtml(vsetVal)}" data-vset-input
           ${!expertMode ? 'disabled' : ''} style="opacity:${expertMode ? '1' : '0.35'}">
    <button class="booster-btn b-btn-set" data-set-v
            ${!expertMode ? 'disabled' : ''} style="opacity:${expertMode ? '1' : '0.35'}">Set V</button>
  </div>
  <div class="booster-controls-iset">
    <input class="booster-iset-input" type="text" inputmode="decimal"
           placeholder="ISet (A)" value="${escHtml(isetVal)}" data-iset-input
           ${!expertMode ? 'disabled' : ''} style="opacity:${expertMode ? '1' : '0.35'}">
    <button class="booster-btn b-btn-seti" data-set-i
            ${!expertMode ? 'disabled' : ''} style="opacity:${expertMode ? '1' : '0.35'}">Set I</button>
    <span class="booster-controls-spacer"></span>
    <button class="booster-btn b-btn-on"  data-on>ON</button>
    <button class="booster-btn b-btn-off" data-off>OFF</button>
  </div>
</div>
<div class="booster-error ${s.error?'visible':''}" data-field="error">${escHtml(s.error||'')}</div>`;
}

function updateBoosterCard(card, idx, s) {
    const wantCls = 'booster-card ' + boosterCardClass(s);
    if (card.className !== wantCls) card.className = wantCls;

    const badge = card.querySelector('.booster-conn-badge');
    const connCls = s.connected ? 'conn-ok' : 'conn-err';
    const connTxt = s.connected ? 'Connected' : 'Offline';
    if (badge.className !== 'booster-conn-badge ' + connCls) badge.className = 'booster-conn-badge ' + connCls;
    if (badge.textContent !== connTxt) badge.textContent = connTxt;

    function sf(field, text) {
        const el = card.querySelector(`[data-field="${field}"]`);
        if (el && el.textContent !== text) el.textContent = text;
    }
    function sh(field, html2) {
        const el = card.querySelector(`[data-field="${field}"]`);
        if (el && el.innerHTML !== html2) el.innerHTML = html2;
    }
    sf('vmon', s.vmon != null ? s.vmon.toFixed(2) + ' V' : '—');
    sf('vset', s.vset != null ? s.vset.toFixed(2) + ' V' : '—');
    sf('imon', s.imon != null ? s.imon.toFixed(3) + ' A' : '—');
    sf('iset', s.iset != null ? s.iset.toFixed(3) + ' A' : '—');
    sh('mode', boosterModeBadge(s.mode));
    sh('pwr',  boosterPwrBadge(s));
    const errEl = card.querySelector('[data-field="error"]');
    if (errEl) {
        if (errEl.textContent !== (s.error||'')) errEl.textContent = s.error||'';
        const wantVis = s.error ? 'booster-error visible' : 'booster-error';
        if (errEl.className !== wantVis) errEl.className = wantVis;
    }
    // Expert mode guard — SetV input and button only
    const vsetEl = card.querySelector('[data-vset-input]');
    const setVEl = card.querySelector('[data-set-v]');
    if (vsetEl) { vsetEl.disabled = !expertMode; vsetEl.style.opacity = expertMode ? '1' : '0.35'; }
    if (setVEl) { setVEl.disabled = !expertMode; setVEl.style.opacity = expertMode ? '1' : '0.35'; }
    const isetEl = card.querySelector('[data-iset-input]');
    const setIEl = card.querySelector('[data-set-i]');
    if (isetEl) { isetEl.disabled = !expertMode; isetEl.style.opacity = expertMode ? '1' : '0.35'; }
    if (setIEl) { setIEl.disabled = !expertMode; setIEl.style.opacity = expertMode ? '1' : '0.35'; }
}

function wireBoosterCard(card, idx) {
    card.querySelector('[data-set-v]').addEventListener('click', () => {
        if (!boosterMonitor || !expertMode) return;
        const inp = card.querySelector('[data-vset-input]');
        const v = parseFloat(inp.value);
        if (isNaN(v) || v < 0) { inp.style.borderColor = 'var(--red)'; return; }
        inp.style.borderColor = '';
        boosterMonitor.setVoltage(idx, v);
    });
    card.querySelector('[data-set-i]').addEventListener('click', () => {
        if (!boosterMonitor || !expertMode) return;
        const inp = card.querySelector('[data-iset-input]');
        const v = parseFloat(inp.value);
        if (isNaN(v) || v < 0) { inp.style.borderColor = 'var(--red)'; return; }
        inp.style.borderColor = '';
        boosterMonitor.setCurrent(idx, v);
    });
    card.querySelector('[data-on]').addEventListener('click', () => {
        if (boosterMonitor) boosterMonitor.setOutput(idx, true);
    });
    card.querySelector('[data-off]').addEventListener('click', () => {
        if (boosterMonitor) boosterMonitor.setOutput(idx, false);
    });
    card.querySelector('[data-vset-input]').addEventListener('keydown', e => {
        if (e.key === 'Enter') card.querySelector('[data-set-v]').click();
    });
    card.querySelector('[data-iset-input]').addEventListener('keydown', e => {
        if (e.key === 'Enter') card.querySelector('[data-set-i]').click();
    });
}

function boosterCardClass(s) {
    if (!s.connected || s.error) return 'card-fault';
    if (s.on && s.mode === 'CC')  return 'card-warn';
    if (s.on)                      return 'card-on';
    return '';
}

function boosterModeBadge(mode) {
    if (!mode) return '<span class="booster-val val-dim">—</span>';
    const cls = mode.toUpperCase() === 'CV' ? 'mode-cv' : 'mode-cc';
    return `<span class="booster-mode-badge ${cls}">${escHtml(mode)}</span>`;
}

function boosterPwrBadge(s) {
    if (!s.connected) return '<span class="st-off">OFFLINE</span>';
    return s.on ? '<span class="st-on">ON</span>' : '<span class="st-off">OFF</span>';
}
