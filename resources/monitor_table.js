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
        const filtered = getFilteredChannels();
        const n = filtered.length;
        if (n === 0) return;
        const scope = (n === allChannels.length) ? 'ALL' : n + ' visible';
        if (!confirm(`Turn ON ${scope} channels?`)) return;
        filtered.forEach(ch => {
            hvMonitor.setChannelPower(ch.crate, ch.slot, ch.channel, true);
            ch.on = true;
        });
        dataDirty = true; renderActiveTab();
    });

    document.getElementById('btn-all-off').addEventListener('click', () => {
        if (!hvMonitor || allChannels.length === 0) return;
        const filtered = getFilteredChannels();
        const n = filtered.length;
        if (n === 0) return;
        const scope = (n === allChannels.length) ? 'ALL' : n + ' visible';
        if (!confirm(`Turn OFF ${scope} channels?`)) return;
        filtered.forEach(ch => {
            hvMonitor.setChannelPower(ch.crate, ch.slot, ch.channel, false);
            ch.on = false;
        });
        dataDirty = true; renderActiveTab();
    });

    // ── Alarm mute button ──────────────────────────────────────────────
    document.getElementById('btn-mute').addEventListener('click', () => {
        ensureAlarmCtx();   // AudioContext requires user gesture
        toggleMute();
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
        // Show/hide All Set V group (expert only)
        const setvGroup = document.getElementById('bulk-setv-group');
        if (setvGroup) setvGroup.style.display = expertMode ? 'flex' : 'none';
        dataDirty = true; boosterDirty = true; renderActiveTab();
    });

    // ── All Set V (expert mode bulk voltage set) ────────────────────────
    document.getElementById('btn-all-setv').addEventListener('click', () => {
        if (!hvMonitor || !expertMode) return;
        const input = document.getElementById('bulk-setv-input');
        const v = parseFloat(input.value);
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
        dataDirty = true; renderActiveTab();
    });

    // ── Save settings ───────────────────────────────────────────────────
    document.getElementById('btn-save-settings').addEventListener('click', () => {
        if (!hvMonitor) return;
        const btn = document.getElementById('btn-save-settings');
        btn.textContent = 'Saving…';
        btn.disabled = true;
        hvMonitor.saveSettings(async data => {
            btn.textContent = 'Save';
            btn.disabled = false;
            const jsonStr = (typeof data === 'string') ? data : JSON.stringify(data, null, 2);
            const blob = new Blob([jsonStr], { type: 'application/json' });
            const ts = new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19);
            const defaultName = `hv_settings_${ts}.json`;

            // Use native save dialog if available (Chrome/Edge/Qt WebEngine)
            if (window.showSaveFilePicker) {
                try {
                    const handle = await window.showSaveFilePicker({
                        suggestedName: defaultName,
                        types: [{ description: 'JSON files', accept: { 'application/json': ['.json'] } }],
                    });
                    const writable = await handle.createWritable();
                    await writable.write(blob);
                    await writable.close();
                    return;
                } catch (e) {
                    if (e.name === 'AbortError') return; // user cancelled
                    console.warn('showSaveFilePicker failed, falling back:', e);
                }
            }
            // Fallback: auto-download
            const url = URL.createObjectURL(blob);
            const a = document.createElement('a');
            a.href = url;
            a.download = defaultName;
            a.click();
            URL.revokeObjectURL(url);
        });
    });

    // ── Load settings (expert mode only) ────────────────────────────────
    document.getElementById('btn-load-settings').addEventListener('click', () => {
        if (!expertMode) {
            alert('Enable Expert Mode to load settings.');
            return;
        }
        document.getElementById('load-settings-file').click();
    });
    document.getElementById('load-settings-file').addEventListener('change', e => {
        const file = e.target.files[0];
        if (!file) return;
        if (!expertMode) { e.target.value = ''; return; }
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

// Return the currently filtered/visible channel list (same logic as renderTable)
function getFilteredChannels() {
    let data = allChannels;
    if (filterStatus === 'on')           data = data.filter(c => c.on);
    else if (filterStatus === 'off')     data = data.filter(c => !c.on);
    else if (filterStatus === 'primary') data = data.filter(c => isPrimary(c));
    else if (filterStatus === 'warn')    data = data.filter(c => classifyChannel(c).isWarn);
    else if (filterStatus === 'fault')   data = data.filter(c => classifyChannel(c).isFault);
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

function renderTable() {

    // ── Filter & sort ─────────────────────────────────────────────────
    let data = allChannels;
    if (filterStatus === 'on')      data = data.filter(c => c.on);
    else if (filterStatus === 'off')data = data.filter(c => !c.on);
    else if (filterStatus === 'primary') data = data.filter(c => isPrimary(c));
    else if (filterStatus === 'warn')    data = data.filter(c => classifyChannel(c).isWarn);
    else if (filterStatus === 'fault')   data = data.filter(c => classifyChannel(c).isFault);
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

    // If a cell is being edited, skip all row reordering to preserve
    // the editing input's DOM position and focus.  Values still update.
    const lockOrder = !!document.querySelector('#ch-body td.editing');

    const fragment = lockOrder ? null : document.createDocumentFragment();

    for (const ch of data) {
        const key = ch.crate + '|' + ch.slot + '|' + ch.channel;
        const diff   = (ch.vmon != null && ch.vset != null) ? Math.abs(ch.vmon - ch.vset) : null;
        const dcls   = !ch.on ? 'diff-ok' : (diff == null || diff < DV.table_ok) ? 'diff-ok' : diff < DV.table_warn ? 'diff-warn' : 'diff-bad';
        const cc     = classifyChannel(ch);
        const dotCls = cc.dot;
        const pwrCls = ch.on ? 'on' : 'off';
        const prim   = isPrimary(ch);

        let tr = existingRows.get(key);
        if (!tr) {
            // ── Create new row ──────────────────────────────────────────────
            tr = document.createElement('tr');
            tr.dataset.key = key;
            existingRows.delete(key);

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
            td9.textContent = fmt(diff, 2);
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
            btn.textContent = ch.on ? 'ON' : 'OFF';
            btn.onclick = makeToggle(ch.crate, ch.slot, ch.channel, ch.on);
            td13.appendChild(btn);
            tr.appendChild(td13);

            tr.className = prim ? 'primary-row' : '';
        } else {
            // ── Patch existing row ─────────────────────────────────────────
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
            const diffTxt = fmt(diff, 2);
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
            const pwrTxt = ch.on ? 'ON' : 'OFF';
            if (pbtn.textContent !== pwrTxt) {
                pbtn.textContent = pwrTxt;
                pbtn.onclick = makeToggle(ch.crate, ch.slot, ch.channel, ch.on);
            }
        }
        if (fragment) {
            fragment.appendChild(tr);
        } else if (!tr.parentNode) {
            // lockOrder mode: only add genuinely new rows to tbody
            tbody.appendChild(tr);
        }
    }

    // Remove rows no longer in filtered set (but never remove the editing row)
    existingRows.forEach(tr => {
        if (!tr.querySelector('td.editing')) tr.remove();
    });

    // Reorder only when not editing
    if (fragment) tbody.appendChild(fragment);

    dataDirty = false;

    // ── Summary counts (computed once from the same pass) ─────────────
    const total   = allChannels.length;
    let primCnt=0, onCnt=0, warns=0, faults=0;
    for (const c of allChannels) {
        if (isPrimary(c)) primCnt++;
        if (c.on) onCnt++;
        const cc = classifyChannel(c);
        if (cc.isWarn)  warns++;
        if (cc.isFault) faults++;
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
        const v = parseFloat(rawVal);
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
    if (!hvMonitor) return;
    hvMonitor.setChannelPower(crate, slot, channel, on);
    const ch = allChannels.find(c => c.crate===crate && c.slot===slot && c.channel===channel);
    if (ch) ch.on = on;
    dataDirty = true; renderActiveTab();
}

function setPillConnected(ok) {
    const pill = document.getElementById('conn-pill');
    const text = document.getElementById('conn-text');
    if (ok) { pill.classList.remove('disconnected'); text.textContent = 'live'; }
    else    { pill.classList.add('disconnected');    text.textContent = 'disconnected'; }
}

