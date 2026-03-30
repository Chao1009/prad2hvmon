// ═════════════════════════════════════════════════════════════════════
//  TABLE TAB (original logic)
// ═════════════════════════════════════════════════════════════════════
function initTableUI() {
    document.getElementById('btn-refresh').addEventListener('click', () => {
        if (!hvMonitor) return;
        hvMonitor.readAll(data => {
            allChannels = data;
            for (const ch of allChannels) ch._cc = classifyChannel(ch);
            rebuildChMap();
            populateCrateChips();
            dataDirty = true; renderActiveTab();
        });
    });
    document.getElementById('search').addEventListener('input', e => {
        searchText = e.target.value.trim().toLowerCase();
        dataDirty = true; renderTable(true);
    });
    document.querySelectorAll('#summary-strip .summary-item').forEach(item => {
        item.addEventListener('click', () => {
            document.querySelectorAll('#summary-strip .summary-item').forEach(s => s.classList.remove('active-filter'));
            item.classList.add('active-filter');
            filterStatus = item.dataset.filter;
            dataDirty = true; renderTable(true);
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
            dataDirty = true; renderTable(true);
        });
    });

    // ── Bulk ON / OFF buttons ───────────────────────────────────────────
    document.getElementById('btn-all-on').addEventListener('click', () => {
        if (!hvMonitor || allChannels.length === 0 || accessLevel < 1) return;
        const filtered = getFilteredChannels();
        const n = filtered.length;
        if (n === 0) return;
        const scope = (n === allChannels.length) ? 'ALL' : n + ' visible';
        if (!confirm(`Turn ON ${scope} channels?`)) return;
        const chListOn = filtered.map(ch => ({crate: ch.crate, slot: ch.slot, ch: ch.channel}));
        const filterCtx = { group: filterStatus, crate: filterCrate || 'all', search: searchText };
        hvMonitor.setPowerBatch(chListOn, true, filterCtx);
        filtered.forEach(ch => addPendingPower(ch.crate, ch.slot, ch.channel, true, true));
        dataDirty = true; renderActiveTab();
    });

    document.getElementById('btn-all-off').addEventListener('click', () => {
        if (!hvMonitor || allChannels.length === 0 || accessLevel < 1) return;
        const filtered = getFilteredChannels();
        const n = filtered.length;
        if (n === 0) return;
        const scope = (n === allChannels.length) ? 'ALL' : n + ' visible';
        if (!confirm(`Turn OFF ${scope} channels?`)) return;
        const chListOff = filtered.map(ch => ({crate: ch.crate, slot: ch.slot, ch: ch.channel}));
        const filterCtxOff = { group: filterStatus, crate: filterCrate || 'all', search: searchText };
        hvMonitor.setPowerBatch(chListOff, false, filterCtxOff);
        filtered.forEach(ch => addPendingPower(ch.crate, ch.slot, ch.channel, false, true));
        dataDirty = true; renderActiveTab();
    });

    // ── Alarm mute button ──────────────────────────────────────────────
    document.getElementById('btn-mute').addEventListener('click', () => {
        ensureAlarmCtx();   // AudioContext requires user gesture
        toggleMute();
    });

    // Expert toggle removed — access level is controlled via the login modal.
    // Show/hide bulk-setv-group based on initial access level
    const setvGroup = document.getElementById('bulk-setv-group');
    if (setvGroup) setvGroup.style.display = expertMode ? 'flex' : 'none';
    // Gate Load button based on initial access level
    const btnLoad = document.getElementById('btn-load-settings');
    if (btnLoad) { btnLoad.disabled = (accessLevel < 2); btnLoad.style.opacity = (accessLevel < 2) ? '0.35' : '1'; }

    // ── All Set V (expert mode bulk voltage set) ────────────────────────
    document.getElementById('btn-all-setv').addEventListener('click', () => {
        if (!hvMonitor || accessLevel < 2) return;
        const input = document.getElementById('bulk-setv-input');
        const rawVal = input.value.trim();
        const isRelative = rawVal.length > 1 && (rawVal[0] === '+' || rawVal[0] === '-');
        // For absolute mode, parse once; for relative mode, resolve per-channel below
        if (!isRelative) {
            const v = parseFloat(rawVal);
            if (isNaN(v) || v < 0) { input.style.borderColor = 'var(--red)'; return; }
            input.style.borderColor = '';
            const filtered = getFilteredChannels();
            const n = filtered.length;
            if (n === 0) return;
            const scope = (n === allChannels.length) ? 'ALL' : n + ' visible';
            if (!confirm(`Set VSet = ${v.toFixed(1)} V on ${scope} channels?`)) return;
            filtered.forEach(ch => {
                hvMonitor.setChannelVoltage(ch.crate, ch.slot, ch.channel, v);
            });
        } else {
            const delta = parseFloat(rawVal);
            if (isNaN(delta)) { input.style.borderColor = 'var(--red)'; return; }
            input.style.borderColor = '';
            const filtered = getFilteredChannels();
            const n = filtered.length;
            if (n === 0) return;
            const scope = (n === allChannels.length) ? 'ALL' : n + ' visible';
            const sign = delta >= 0 ? '+' : '';
            if (!confirm(`Adjust VSet by ${sign}${delta.toFixed(1)} V on ${scope} channels?`)) return;
            filtered.forEach(ch => {
                const v = resolveVSetInput(rawVal, ch.vset);
                if (!isNaN(v) && v >= 0) hvMonitor.setChannelVoltage(ch.crate, ch.slot, ch.channel, v);
            });
        }
        dataDirty = true; renderActiveTab();
    });

    // ── Save settings ───────────────────────────────────────────────────
    document.getElementById('btn-save-settings').addEventListener('click', () => {
        if (!hvMonitor) return;
        const btn = document.getElementById('btn-save-settings');
        btn.textContent = 'Saving…';
        btn.disabled = true;
        hvMonitor.saveSettings((data, savedPath) => {
            btn.textContent = 'Save';
            btn.disabled = false;
            try {
                const jsonStr = (typeof data === 'string') ? data : JSON.stringify(data, null, 2);
                const ts = new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19);
                const defaultName = `hv_settings_${ts}.json`;
                const blob = new Blob([jsonStr], { type: 'application/json' });

                // Chrome/Edge: native save dialog via File System Access API
                // (skip in Qt WebEngine — the API exists but is broken)
                const isQtWebEngine = navigator.userAgent.includes('QtWebEngine');
                if (window.showSaveFilePicker && !isQtWebEngine) {
                    window.showSaveFilePicker({
                        suggestedName: defaultName,
                        types: [{ description: 'JSON files', accept: { 'application/json': ['.json'] } }],
                    }).then(handle => {
                        return handle.createWritable().then(w => w.write(blob).then(() => w.close()))
                                     .then(() => handle.name);
                    }).then(name => {
                        showToast('Saved: ' + name);
                    }).catch(e => {
                        if (e.name !== 'AbortError')
                            alert('Save failed: ' + e.message);
                    });
                    return;
                }

                // Qt / Firefox / other: daemon already saved the file server-side
                if (savedPath) {
                    showToast('Settings saved: ' + savedPath, 6000);
                } else {
                    // Fallback: try browser download
                    const a = document.createElement('a');
                    a.href = URL.createObjectURL(blob);
                    a.download = defaultName;
                    document.body.appendChild(a);
                    a.click();
                    setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 500);
                    showToast('Saved: ' + defaultName + ' — check Downloads folder', 6000);
                }
            } catch (e) {
                alert('Save error: ' + e.message);
            }
        });
    });

    // ── Load settings (expert mode only) ────────────────────────────────
    document.getElementById('btn-load-settings').addEventListener('click', () => {
        if (accessLevel < 2) {
            alert('Expert access required to load settings.');
            return;
        }
        document.getElementById('load-settings-file').click();
    });
    document.getElementById('load-settings-file').addEventListener('change', e => {
        const file = e.target.files[0];
        if (!file) return;
        if (accessLevel < 2) { e.target.value = ''; return; }
        if (!confirm(`Load settings from "${file.name}"?\n\nThis will write all saved parameters to the HV hardware.`)) {
            e.target.value = '';
            return;
        }
        const reader = new FileReader();
        reader.onload = () => {
            try {
                const settings = JSON.parse(reader.result);
                if (!settings.format || !settings.channels) {
                    alert('Invalid settings file: missing "format" or "channels" field.');
                    return;
                }
                hvMonitor.loadSettings(settings, result => {
                    if (result && result.error) {
                        alert('Load failed: ' + result.error);
                    } else if (result) {
                        console.log('Load result:', result);
                        const u = result.unchanged || 0;
                        alert(`Settings applied: ${result.restored} restored, ${u} unchanged, ${result.skipped} skipped, ${result.errors} errors`);
                    }
                    dataDirty = true; renderActiveTab();
                });
                console.log('Settings sent:', settings.channels.length, 'channels');
            } catch (err) {
                alert('Failed to parse JSON: ' + err.message);
            }
        };
        reader.readAsText(file);
        e.target.value = '';  // allow re-selecting same file
    });
}

function populateCrateChips() {
    const crates = [...new Set(allChannels.map(c => c.crate))].sort();
    const wrap = document.getElementById('crate-chips');
    wrap.innerHTML = '';
    const allChip = document.createElement('button');
    allChip.className = 'chip active'; allChip.dataset.crate = '';
    allChip.innerHTML = `All ${crates.length} Crates <span class="crate-dot dot-gray" data-crate-dot=""></span>`;
    allChip.addEventListener('click', () => selectCrateChip(allChip, null));
    wrap.appendChild(allChip);
    crates.forEach(name => {
        const chip = document.createElement('button');
        chip.className = 'chip'; chip.dataset.crate = name;
        chip.innerHTML = name.replace('PRadHV_', 'HV') +
            ` <span class="crate-dot dot-gray" data-crate-dot="${name}"></span>`;
        chip.addEventListener('click', () => selectCrateChip(chip, name));
        wrap.appendChild(chip);
    });
    updateCrateDots();
}

function selectCrateChip(chip, name) {
    document.querySelectorAll('#crate-chips .chip').forEach(c => c.classList.remove('active'));
    chip.classList.add('active');
    filterCrate = name;
    dataDirty = true; renderTable(true);
}

function isPrimary(ch) {
    return ch.channel === 0 && ch.name && ch.name.toUpperCase().includes('PRIMARY');
}

// Return the currently filtered/visible channel list (same logic as renderTable)
function getFilteredChannels() {
    let data = allChannels;
    if (filterStatus === 'on')           data = data.filter(c => c.on);
    else if (filterStatus === 'off')     data = data.filter(c => !c.on);
    else if (filterStatus === 'primary') data = data.filter(c => isPrimary(c));
    else if (filterStatus === 'warn')    data = data.filter(c => c._cc.isWarn);
    else if (filterStatus === 'fault')   data = data.filter(c => c._cc.isFault);
    if (filterCrate) data = data.filter(c => c.crate === filterCrate);
    if (searchText) {
        const colonIdx = searchText.indexOf(':');
        if (colonIdx > 0) {
            const col = searchText.slice(0, colonIdx).trim();
            const val = searchText.slice(colonIdx + 1).trim();
            const colMap = { crate:'crate', slot:'slot', ch:'channel', channel:'channel',
                             name:'name', model:'model', vmon:'vmon', vset:'vset', svmax:'svmax' };
            const field = colMap[col];
            data = field
                ? data.filter(c => String(c[field]).toLowerCase().includes(val))
                : data.filter(c => (c.crate+' '+c.slot+' '+c.channel+' '+c.name+' '+c.model).toLowerCase().includes(searchText));
        } else {
            data = data.filter(c => (c.crate+' '+c.slot+' '+c.channel+' '+c.name+' '+c.model).toLowerCase().includes(searchText));
        }
    }
    return data;
}

// ── Virtual scroll state ─────────────────────────────────────────────
// Only ~40 visible rows are materialized in the DOM at any time.
// Spacer <tr> elements above and below maintain correct scroll height.
const VS_BUFFER = 10;         // extra rows rendered above/below viewport
let filteredData = [];        // persists between renders for scroll handler
let _vsRowH = 0;             // measured row height (0 = not yet measured)
let _vsScrollBound = false;   // scroll listener bound flag

function renderTable(resetScroll) {

    // ── Filter & sort ─────────────────────────────────────────────────
    let data = allChannels;
    if (filterStatus === 'on')      data = data.filter(c => c.on);
    else if (filterStatus === 'off')data = data.filter(c => !c.on);
    else if (filterStatus === 'primary') data = data.filter(c => isPrimary(c));
    else if (filterStatus === 'warn')    data = data.filter(c => c._cc.isWarn);
    else if (filterStatus === 'fault')   data = data.filter(c => c._cc.isFault);
    if (filterCrate) data = data.filter(c => c.crate === filterCrate);
    if (searchText) {
        const colonIdx = searchText.indexOf(':');
        if (colonIdx > 0) {
            const col = searchText.slice(0, colonIdx).trim();
            const val = searchText.slice(colonIdx + 1).trim();
            const colMap = { crate:'crate', slot:'slot', ch:'channel', channel:'channel',
                             name:'name', model:'model', vmon:'vmon', vset:'vset', svmax:'svmax' };
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
            va = (a.vmon != null && a.vset != null) ? (a.vmon - a.vset) : -Infinity;
            vb = (b.vmon != null && b.vset != null) ? (b.vmon - b.vset) : -Infinity;
        }
        if (typeof va === 'string') return sortAsc ? va.localeCompare(vb) : vb.localeCompare(va);
        if (typeof va === 'boolean') { va = va?1:0; vb = vb?1:0; }
        return sortAsc ? va - vb : vb - va;
    });

    filteredData = data;

    // Reset scroll to top on filter/sort changes — prevents the
    // virtual scroll window from landing past the end of the new data.
    if (resetScroll) document.getElementById('table-wrap').scrollTop = 0;

    // ── Render visible window ─────────────────────────────────────────
    renderVisibleRows();

    dataDirty = false;

    // ── Summary counts (computed once from the same pass) ─────────────
    const total   = allChannels.length;
    let primCnt=0, onCnt=0, warns=0, faults=0;
    for (const c of allChannels) {
        if (isPrimary(c)) primCnt++;
        if (c.on) onCnt++;
        if (c._cc.isWarn)  warns++;
        if (c._cc.isFault) faults++;
    }
    document.getElementById('s-total').textContent   = total;
    document.getElementById('s-primary').textContent = primCnt;
    document.getElementById('s-on').textContent      = onCnt;
    document.getElementById('s-off').textContent     = total - onCnt;
    document.getElementById('s-warn').textContent    = warns;
    document.getElementById('s-fault').textContent   = faults;

    // Pass filtered count to footer without re-filtering
    updateFooter(filteredData.length, total);
}

// ── Virtual-scroll row renderer ──────────────────────────────────────
// Called by renderTable() on data change, and by the scroll listener
// on user scroll.  Only creates/patches DOM rows for the visible
// window (~40 rows + buffer), with spacer <tr> elements above/below
// to maintain correct scrollbar height.
function renderVisibleRows() {
    const wrap = document.getElementById('table-wrap');
    const tbody = document.getElementById('ch-body');

    // Bind scroll listener once (rAF-throttled)
    if (!_vsScrollBound) {
        _vsScrollBound = true;
        let ticking = false;
        wrap.addEventListener('scroll', () => {
            if (!ticking) {
                ticking = true;
                requestAnimationFrame(() => { renderVisibleRows(); ticking = false; });
            }
        }, { passive: true });
    }

    // Measure row height from a real row, or use 29px default
    if (!_vsRowH) {
        const sample = tbody.querySelector('tr[data-key]');
        _vsRowH = (sample && sample.offsetHeight > 0) ? sample.offsetHeight : 29;
    }

    const totalRows = filteredData.length;
    const scrollTop = wrap.scrollTop;
    const viewH     = wrap.clientHeight || 600;

    const first = Math.min(totalRows, Math.max(0, Math.floor(scrollTop / _vsRowH) - VS_BUFFER));
    const last  = Math.min(totalRows, Math.ceil((scrollTop + viewH) / _vsRowH) + VS_BUFFER);

    // If a cell is being edited, skip all row reordering to preserve
    // the editing input's DOM position and focus.  Values still update.
    const lockOrder = !!tbody.querySelector('td.editing');

    // Build map of existing data rows (skip spacer rows)
    const existingRows = new Map();
    for (const tr of tbody.rows) {
        if (tr.dataset.key) existingRows.set(tr.dataset.key, tr);
    }

    const fragment = lockOrder ? null : document.createDocumentFragment();

    // ── Top spacer ──────────────────────────────────────────────────
    let topSp = document.getElementById('vs-top');
    if (!topSp) {
        topSp = document.createElement('tr');
        topSp.id = 'vs-top';
        const td = document.createElement('td');
        td.colSpan = 14;
        topSp.appendChild(td);
    }
    topSp.firstChild.style.height = (first * _vsRowH) + 'px';
    if (fragment) fragment.appendChild(topSp);

    // ── Bottom spacer (created early so lockOrder inserts can reference it)
    let botSp = document.getElementById('vs-bot');
    if (!botSp) {
        botSp = document.createElement('tr');
        botSp.id = 'vs-bot';
        const td = document.createElement('td');
        td.colSpan = 14;
        botSp.appendChild(td);
    }

    // ── Visible rows (same surgical-patch logic as before) ──────────
    const visibleKeys = new Set();

    for (let i = first; i < last; i++) {
        const ch = filteredData[i];
        const key = ch.crate + '|' + ch.slot + '|' + ch.channel;
        visibleKeys.add(key);
        const diff   = (ch.vmon != null && ch.vset != null) ? (ch.vmon - ch.vset) : null;
        const adiff  = diff != null ? Math.abs(diff) : null;
        const dcls   = !ch.on ? 'diff-ok' : (adiff == null || adiff < DV.table_ok) ? 'diff-ok' : adiff < DV.table_warn ? 'diff-warn' : 'diff-bad';
        const cc     = ch._cc;
        const dotCls = cc.dot;
        const pwrPending = hasPendingPower(ch.crate, ch.slot, ch.channel);
        const pwrCls = pwrPending ? 'pending' : (ch.on ? 'on' : 'off');
        const prim   = isPrimary(ch);

        let tr = existingRows.get(key);
        if (!tr) {
            // ── Create new row ──────────────────────────────────────────
            tr = document.createElement('tr');
            tr.dataset.key = key;

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
            tr.cells[1].title = ch.ip;

            // td5: name (plain text + edit icon)
            const td5 = document.createElement('td');
            td5.innerHTML = buildNameCell(ch, prim);
            tr.appendChild(td5);

            // td6: vmon
            const td6 = document.createElement('td');
            td6.style.textAlign = 'right';
            td6.textContent = fmt(ch.vmon, 2);
            tr.appendChild(td6);

            // td7: vset (plain text + edit icon)
            const td7 = document.createElement('td');
            td7.style.textAlign = 'right';
            td7.innerHTML = buildEditableCell(ch.vset, 2, expertMode, ch, 'vset');
            tr.appendChild(td7);

            // td8: svmax (plain text + edit icon)
            const td8 = document.createElement('td');
            td8.style.textAlign = 'right';
            td8.innerHTML = buildEditableCell(ch.svmax, 2, expertMode, ch, 'svmax');
            tr.appendChild(td8);

            // td9: diff
            const td9 = document.createElement('td');
            td9.className = dcls;
            td9.style.textAlign = 'right';
            td9.textContent = fmtSigned(diff, 2);
            tr.appendChild(td9);

            // td10: imon
            const td10 = document.createElement('td');
            td10.style.textAlign = 'right';
            td10.style.color = ch.iSupported === false ? 'var(--text-dim)' : '';
            td10.textContent = ch.iSupported === false ? 'N/A' : fmt(ch.imon, 3);
            tr.appendChild(td10);

            // td11: iset (plain text + edit icon)
            const td11 = document.createElement('td');
            td11.style.textAlign = 'right';
            if (ch.iSupported === false) {
                td11.innerHTML = '<span style="color:var(--text-dim)">N/A</span>';
            } else {
                td11.innerHTML = buildEditableCell(ch.iset, 1, expertMode, ch, 'iset');
            }
            tr.appendChild(td11);

            // td12: status badges
            const td12 = document.createElement('td');
            td12.innerHTML = cc.badgesHtml;
            tr.appendChild(td12);

            // td13: power button
            const td13 = document.createElement('td');
            td13.style.textAlign = 'center';
            const btn = document.createElement('button');
            btn.className = 'pwr-btn ' + pwrCls;
            btn.textContent = pwrPending ? '...' : (ch.on ? 'ON' : 'OFF');
            btn.onclick = pwrPending ? null : makeToggle(ch.crate, ch.slot, ch.channel, ch.on);
            btn.disabled = pwrPending || (accessLevel < 1);
            td13.appendChild(btn);
            tr.appendChild(td13);

            tr.className = prim ? 'primary-row' : '';
        } else {
            // ── Patch existing row ─────────────────────────────────────
            existingRows.delete(key);

            // status dot (td0)
            const dot = tr.cells[0].firstElementChild;
            const wantDot = 'status-dot ' + dotCls;
            if (dot.className !== wantDot) dot.className = wantDot;

            // name (td5) — skip if being edited
            if (!tr.cells[5].classList.contains('editing')) {
                const nameHtml = buildNameCell(ch, prim);
                if (tr.cells[5].innerHTML !== nameHtml) tr.cells[5].innerHTML = nameHtml;
            }

            // vmon (td6)
            const vmonTxt = fmt(ch.vmon, 2);
            if (tr.cells[6].textContent !== vmonTxt) tr.cells[6].textContent = vmonTxt;

            // vset (td7) — skip if being edited
            if (!tr.cells[7].classList.contains('editing')) {
                const vsetHtml = buildEditableCell(ch.vset, 2, expertMode, ch, 'vset');
                if (tr.cells[7].innerHTML !== vsetHtml) tr.cells[7].innerHTML = vsetHtml;
            }

            // svmax (td8) — skip if being edited
            if (!tr.cells[8].classList.contains('editing')) {
                const svmaxHtml = buildEditableCell(ch.svmax, 2, expertMode, ch, 'svmax');
                if (tr.cells[8].innerHTML !== svmaxHtml) tr.cells[8].innerHTML = svmaxHtml;
            }

            // diff (td9)
            if (tr.cells[9].className !== dcls) tr.cells[9].className = dcls;
            const diffTxt = fmtSigned(diff, 2);
            if (tr.cells[9].textContent !== diffTxt) tr.cells[9].textContent = diffTxt;

            // imon (td10)
            const imonTxt = ch.iSupported === false ? 'N/A' : fmt(ch.imon, 3);
            if (tr.cells[10].textContent !== imonTxt) tr.cells[10].textContent = imonTxt;

            // iset (td11) — skip if being edited
            if (!tr.cells[11].classList.contains('editing')) {
                let isetHtml;
                if (ch.iSupported === false) {
                    isetHtml = '<span style="color:var(--text-dim)">N/A</span>';
                } else {
                    isetHtml = buildEditableCell(ch.iset, 1, expertMode, ch, 'iset');
                }
                if (tr.cells[11].innerHTML !== isetHtml) tr.cells[11].innerHTML = isetHtml;
            }

            // status badges (td12)
            const stHtml = cc.badgesHtml;
            if (tr.cells[12].innerHTML !== stHtml) tr.cells[12].innerHTML = stHtml;

            // power button (td13)
            const pbtn = tr.cells[13].firstElementChild;
            const wantPwr = 'pwr-btn ' + pwrCls;
            if (pbtn.className !== wantPwr) pbtn.className = wantPwr;
            const pwrTxt = pwrPending ? '...' : (ch.on ? 'ON' : 'OFF');
            if (pbtn.textContent !== pwrTxt) {
                pbtn.textContent = pwrTxt;
                pbtn.onclick = pwrPending ? null : makeToggle(ch.crate, ch.slot, ch.channel, ch.on);
            }
            pbtn.disabled = pwrPending || (accessLevel < 1);
            pbtn.style.opacity = (pwrPending || accessLevel < 1) ? '0.35' : '1';
        }
        if (fragment) {
            fragment.appendChild(tr);
        } else if (!tr.parentNode) {
            // lockOrder mode: insert before bottom spacer to maintain order
            if (botSp.parentNode === tbody) tbody.insertBefore(tr, botSp);
            else tbody.appendChild(tr);
        }
    }

    // ── Bottom spacer ───────────────────────────────────────────────
    botSp.firstChild.style.height = ((totalRows - last) * _vsRowH) + 'px';
    if (fragment) fragment.appendChild(botSp);

    // Remove rows outside visible range (but never remove the editing row)
    existingRows.forEach(tr => {
        if (!visibleKeys.has(tr.dataset.key) && !tr.querySelector('td.editing')) {
            tr.remove();
        }
    });

    // Reorder only when not editing
    if (fragment) tbody.appendChild(fragment);

    // Re-measure row height from a real row now that DOM is populated
    if (_vsRowH === 29) {
        const sample = tbody.querySelector('tr[data-key]');
        if (sample && sample.offsetHeight > 0) _vsRowH = sample.offsetHeight;
    }
}

// Format a signed number with +/− prefix
function fmtSigned(val, decimals) {
    if (val == null || isNaN(val)) return '—';
    const sign = val > 0 ? '+' : '';
    return sign + val.toFixed(decimals);
}

// Helper: creates a closure for the power button onclick (avoids string eval)
function makeToggle(crate, slot, channel, currentOn) {
    return function() { togglePower(crate, slot, channel, !currentOn); };
}

function buildNameCell(ch, prim) {
    const badge = prim ? '<span class="primary-badge">Primary</span>' : '';
    const name = ch.name || '—';
    if (expertMode) {
        return `${escHtml(name)}${badge}<button class="edit-icon" onclick="enterEdit(this.parentElement,'${ch.crate}',${ch.slot},${ch.channel},'name')" title="Edit name">✏</button>`;
    }
    return escHtml(name) + badge;
}

// Generic builder for editable numeric cells (vset, svmax, iset)
function buildEditableCell(value, decimals, expert, ch, param) {
    if (value == null) return '<span style="color:var(--text-dim)">—</span>';
    const txt = fmt(value, decimals);
    if (expert) {
        return `<span style="color:var(--text-dim)">${txt}</span><button class="edit-icon" onclick="enterEdit(this.parentElement,'${ch.crate}',${ch.slot},${ch.channel},'${param}')" title="Edit ${param}">✏</button>`;
    }
    return `<span style="color:var(--text-dim)">${txt}</span>`;
}

// ── Edit mode: click ✏ to enter, ✓/Enter to commit, ✕/Escape to cancel ──

function enterEdit(td, crate, slot, channel, param) {
    if (td.classList.contains('editing')) return;
    const ch = allChannels.find(c => c.crate === crate && c.slot === slot && c.channel === channel);
    if (!ch) return;

    let currentVal = '', placeholder = '';
    if (param === 'vset')       { currentVal = ch.vset  != null ? ch.vset.toFixed(2) : ''; placeholder = 'VSet (V)'; }
    else if (param === 'svmax') { currentVal = ch.svmax != null ? ch.svmax.toFixed(2) : ''; placeholder = 'SVMax (V)'; }
    else if (param === 'iset')  { currentVal = ch.iset  != null ? ch.iset.toFixed(1) : ''; placeholder = 'ISet (µA)'; }
    else if (param === 'name')  { currentVal = ch.name || ''; placeholder = 'Name'; }

    td.classList.add('editing');
    td.innerHTML = '';

    const input = document.createElement('input');
    input.className = param === 'name' ? 'name-inline' : 'vset-inline';
    input.type = 'text';
    input.value = currentVal;
    input.placeholder = placeholder;
    input.dataset.crate = crate;
    input.dataset.slot = slot;
    input.dataset.channel = channel;
    input.dataset.param = param;

    const commitBtn = document.createElement('button');
    commitBtn.className = 'vset-apply';
    commitBtn.style.display = 'inline-block';
    commitBtn.textContent = '✓';
    commitBtn.onmousedown = e => e.preventDefault();
    commitBtn.onclick = () => commitEdit(td, input);

    const cancelBtn = document.createElement('button');
    cancelBtn.className = 'vset-apply';
    cancelBtn.style.display = 'inline-block';
    cancelBtn.style.color = 'var(--red)';
    cancelBtn.style.borderColor = 'var(--red-dim)';
    cancelBtn.style.background = 'var(--red-dim)';
    cancelBtn.textContent = '✕';
    cancelBtn.onmousedown = e => e.preventDefault();
    cancelBtn.onclick = () => cancelEdit(td);

    input.addEventListener('keydown', e => {
        if (e.key === 'Enter') commitEdit(td, input);
        else if (e.key === 'Escape') cancelEdit(td);
    });

    td.appendChild(input);
    td.appendChild(commitBtn);
    td.appendChild(cancelBtn);
    input.focus();
    input.select();
}

function commitEdit(td, input) {
    if (!input) return;
    const crate   = input.dataset.crate;
    const slot    = parseInt(input.dataset.slot, 10);
    const channel = parseInt(input.dataset.channel, 10);
    const param   = input.dataset.param;
    const rawVal  = input.value.trim();

    if (param === 'name') {
        if (rawVal) {
            hvMonitor.setChannelName(crate, slot, channel, rawVal);
            const ch = allChannels.find(c => c.crate===crate && c.slot===slot && c.channel===channel);
            if (ch) ch.name = rawVal;
            rebuildChMap();
        }
    } else {
        const ch = allChannels.find(c => c.crate===crate && c.slot===slot && c.channel===channel);
        const cur = ch ? ch[param] : null;
        const v = (param === 'vset') ? resolveVSetInput(rawVal, cur) : parseFloat(rawVal);
        if (!isNaN(v) && v >= 0) {
            if (param === 'vset')       hvMonitor.setChannelVoltage(crate, slot, channel, v);
            else if (param === 'svmax') hvMonitor.setChannelSVMax(crate, slot, channel, v);
            else if (param === 'iset')  hvMonitor.setChannelCurrent(crate, slot, channel, v);
        }
    }

    td.classList.remove('editing');
    td.innerHTML = '';
    dataDirty = true;
    renderActiveTab();
}

function cancelEdit(td) {
    td.classList.remove('editing');
    td.innerHTML = '';
    dataDirty = true;
    renderActiveTab();
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
    if (!hvMonitor || accessLevel < 1) return;
    hvMonitor.setChannelPower(crate, slot, channel, on);
    addPendingPower(crate, slot, channel, on);
    dataDirty = true; renderActiveTab();
}

function setPillConnected(ok) {
    const pill = document.getElementById('conn-pill');
    const text = document.getElementById('conn-text');
    if (ok) { pill.classList.remove('disconnected'); text.textContent = 'live'; }
    else    { pill.classList.add('disconnected');    text.textContent = 'disconnected'; }
}

