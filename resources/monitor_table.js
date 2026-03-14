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

    const fragment = document.createDocumentFragment();

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

            // td8: svmax
            const td8 = document.createElement('td');
            td8.style.textAlign = 'right';
            td8.innerHTML = buildSvmaxCell(ch);
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

            // td11: iset
            const td11 = document.createElement('td');
            td11.style.textAlign = 'right';
            td11.innerHTML = buildIsetCell(ch);
            tr.appendChild(td11);

            // td12: status badges (no ON/OFF — Pwr column covers power state)
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
            // ── Patch only what changed ───────────────────────────────────────
            existingRows.delete(key);  // mark as still-needed

            // status dot
            const dot = tr.cells[0].firstElementChild;
            const wantDot = 'status-dot ' + dotCls;
            if (dot.className !== wantDot) dot.className = wantDot;

            // vmon (td6)
            const vmonTxt = fmt(ch.vmon, 2);
            if (tr.cells[6].textContent !== vmonTxt) tr.cells[6].textContent = vmonTxt;

            // diff class + value (td9)
            if (tr.cells[9].className !== dcls) tr.cells[9].className = dcls;
            const diffTxt = fmt(diff, 2);
            if (tr.cells[9].textContent !== diffTxt) tr.cells[9].textContent = diffTxt;

            // imon (td10)
            const imonTxt = ch.iSupported === false ? 'N/A' : fmt(ch.imon, 3);
            if (tr.cells[10].textContent !== imonTxt) tr.cells[10].textContent = imonTxt;

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

            // Expert mode cells (name td5, vset td7, svmax td8, iset td11) — rebuild if mode changed
            const trExpert = tr.dataset.expert === '1';
            if (trExpert !== expertMode) {
                tr.cells[5].innerHTML  = buildNameCell(ch, prim);
                tr.cells[7].innerHTML  = buildVsetCell(ch);
                tr.cells[8].innerHTML  = buildSvmaxCell(ch);
                tr.cells[11].innerHTML = buildIsetCell(ch);
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
    if (expertMode) {
        return `<input class="name-inline" type="text" maxlength="12" value="${ch.name||''}"
                     data-orig="${ch.name||''}"
                     oninput="dirtyCheck(this)"
                     onkeydown="if(event.key==='Enter'){applyInline(this);}"
                     onblur="revertInline(this)"
                     data-apply="inlineSetName('${ch.crate}',${ch.slot},${ch.channel},this.value)"
                   ><button class="vset-apply" style="display:none"
                     onmousedown="event.preventDefault();applyInline(this.previousElementSibling)"
                   >✓</button>${badge}`;
    }
    return (ch.name||'—') + badge;
}

function buildVsetCell(ch) {
    if (expertMode) {
        return `<input class="vset-inline" type="text" value="${fmt(ch.vset, 2)}"
                     data-orig="${fmt(ch.vset, 2)}"
                     oninput="dirtyCheck(this)"
                     onkeydown="if(event.key==='Enter'){applyInline(this);}"
                     onblur="revertInline(this)"
                     data-apply="inlineSetVoltage('${ch.crate}',${ch.slot},${ch.channel},this.value)"
                   ><button class="vset-apply" style="display:none"
                     onmousedown="event.preventDefault();applyInline(this.previousElementSibling)"
                   >✓</button>`;
    }
    return `<span style="color:var(--text-dim)">${fmt(ch.vset, 2)}</span>`;
}

function buildSvmaxCell(ch) {
    if (ch.svmax == null) return `<span style="color:var(--text-dim)">—</span>`;
    if (expertMode) {
        return `<input class="vset-inline" type="text" value="${fmt(ch.svmax, 2)}"
                     data-orig="${fmt(ch.svmax, 2)}"
                     oninput="dirtyCheck(this)"
                     onkeydown="if(event.key==='Enter'){applyInline(this);}"
                     onblur="revertInline(this)"
                     data-apply="inlineSetSVMax('${ch.crate}',${ch.slot},${ch.channel},this.value)"
                   ><button class="vset-apply" style="display:none"
                     onmousedown="event.preventDefault();applyInline(this.previousElementSibling)"
                   >✓</button>`;
    }
    return `<span style="color:var(--text-dim)">${fmt(ch.svmax, 2)}</span>`;
}

function buildIsetCell(ch) {
    if (ch.iSupported === false) {
        return `<span style="color:var(--text-dim)">N/A</span>`;
    }
    if (expertMode) {
        return `<input class="vset-inline" type="text" value="${fmt(ch.iset, 1)}"
                     data-orig="${fmt(ch.iset, 1)}"
                     oninput="dirtyCheck(this)"
                     onkeydown="if(event.key==='Enter'){applyInline(this);}"
                     onblur="revertInline(this)"
                     data-apply="inlineSetCurrent('${ch.crate}',${ch.slot},${ch.channel},this.value)"
                   ><button class="vset-apply" style="display:none"
                     onmousedown="event.preventDefault();applyInline(this.previousElementSibling)"
                   >✓</button>`;
    }
    return `<span style="color:var(--text-dim)">${fmt(ch.iset, 1)}</span>`;
}

// ── Inline edit helpers (show/hide ✓ button, apply on Enter) ─────
function dirtyCheck(input) {
    const btn = input.nextElementSibling;
    if (!btn || !btn.classList.contains('vset-apply')) return;
    const dirty = input.value !== input.dataset.orig;
    btn.style.display = dirty ? 'inline-block' : 'none';
    input.classList.toggle('dirty', dirty);
}

function applyInline(input) {
    if (!input || !input.dataset.apply) return;
    const btn = input.nextElementSibling;
    // Execute the apply action
    new Function(input.dataset.apply)();
    // Update orig to new value so it's no longer dirty
    input.dataset.orig = input.value;
    input.classList.remove('dirty');
    if (btn && btn.classList.contains('vset-apply')) btn.style.display = 'none';
}

function revertInline(input) {
    // Delay slightly so mousedown on the ✓ button fires first
    setTimeout(() => {
        if (document.activeElement === input) return;
        input.value = input.dataset.orig || '';
        input.classList.remove('dirty');
        const btn = input.nextElementSibling;
        if (btn && btn.classList.contains('vset-apply')) btn.style.display = 'none';
    }, 150);
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

function inlineSetSVMax(crate, slot, channel, value) {
    if (!hvMonitor || !expertMode) return;
    const v = parseFloat(value);
    if (isNaN(v) || v < 0) return;
    hvMonitor.setChannelSVMax(crate, slot, channel, v);
    const ch = allChannels.find(c => c.crate===crate && c.slot===slot && c.channel===channel);
    if (ch) { ch.svmax = v; dataDirty = true; }
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

