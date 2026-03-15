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
    btnSetV.style.display = 'none';
    rowV.append(vsetInput, btnSetV);

    const rowI = document.createElement('div');
    rowI.className = 'popup-action-row';
    const isetInput = document.createElement('input');
    isetInput.type = 'text'; isetInput.setAttribute('inputmode', 'decimal');
    isetInput.placeholder = 'ISet (A)';
    const btnSetI = document.createElement('button');
    btnSetI.className = 'btn-sm btn-set'; btnSetI.textContent = 'Set I';
    btnSetI.style.display = 'none';
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
                if (vsetInput.value === '' && bs.vset != null) {
                    vsetInput.value = bs.vset.toFixed(2);
                    vsetInput.dataset.orig = vsetInput.value;
                }
                if (isetInput.value === '' && bs.iset != null) {
                    isetInput.value = bs.iset.toFixed(3);
                    isetInput.dataset.orig = isetInput.value;
                }
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

    btnSetV.addEventListener('mousedown', e => e.preventDefault());
    btnSetV.addEventListener('click', () => {
        if (!boosterMonitor || !expertMode) return;
        const v = parseFloat(vsetInput.value);
        if (isNaN(v) || v < 0) { vsetInput.style.borderColor = 'var(--red)'; return; }
        vsetInput.style.borderColor = '';
        const idx = supplyIdx(); if (idx < 0) return;
        boosterMonitor.setVoltage(idx, v);
        const bs = boosterByName[mod.n]; if (bs) bs.vset = v;
        vsetInput.dataset.orig = vsetInput.value;
        btnSetV.style.display = 'none';
        boosterDirty = true; refresh();
    });
    vsetInput.addEventListener('input', () => {
        btnSetV.style.display = vsetInput.value !== (vsetInput.dataset.orig || '') ? '' : 'none';
    });
    vsetInput.addEventListener('keydown', e => { if (e.key === 'Enter') btnSetV.click(); });
    vsetInput.addEventListener('blur', () => {
        setTimeout(() => {
            vsetInput.value = vsetInput.dataset.orig || '';
            btnSetV.style.display = 'none';
        }, 150);
    });

    btnSetI.addEventListener('mousedown', e => e.preventDefault());
    btnSetI.addEventListener('click', () => {
        if (!boosterMonitor || !expertMode) return;
        const v = parseFloat(isetInput.value);
        if (isNaN(v) || v < 0) { isetInput.style.borderColor = 'var(--red)'; return; }
        isetInput.style.borderColor = '';
        const idx = supplyIdx(); if (idx < 0) return;
        boosterMonitor.setCurrent(idx, v);
        const bs = boosterByName[mod.n]; if (bs) bs.iset = v;
        isetInput.dataset.orig = isetInput.value;
        btnSetI.style.display = 'none';
        boosterDirty = true; refresh();
    });
    isetInput.addEventListener('input', () => {
        btnSetI.style.display = isetInput.value !== (isetInput.dataset.orig || '') ? '' : 'none';
    });
    isetInput.addEventListener('keydown', e => { if (e.key === 'Enter') btnSetI.click(); });
    isetInput.addEventListener('blur', () => {
        setTimeout(() => {
            isetInput.value = isetInput.dataset.orig || '';
            btnSetI.style.display = 'none';
        }, 150);
    });

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
function initBoosterTab() {
    // Nothing to wire — the daemon manages all booster connections.
    // Cards are rendered by renderBoosterCards() in the render loop.
}

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
    const connBadge = boosterConnBadgeHtml(s);
    const vsetVal = s.vset != null ? s.vset.toFixed(2) : '';
    const isetVal = s.iset != null ? s.iset.toFixed(3) : '';
    return `
<div class="booster-card-head">
  <div>
    <div class="booster-card-name">${escHtml(s.name)}</div>
    <div class="booster-card-ip">${escHtml(s.ip)}</div>
  </div>
  <span class="booster-conn-badge" data-field="conn-badge">${connBadge}</span>
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
           placeholder="VSet (V)" value="${escHtml(vsetVal)}" data-vset-input data-orig="${escHtml(vsetVal)}"
           ${!expertMode ? 'disabled' : ''} style="opacity:${expertMode ? '1' : '0.35'}">
    <button class="booster-btn b-btn-set" data-set-v style="display:none"
            ${!expertMode ? 'disabled' : ''}>Set V</button>
  </div>
  <div class="booster-controls-iset">
    <input class="booster-iset-input" type="text" inputmode="decimal"
           placeholder="ISet (A)" value="${escHtml(isetVal)}" data-iset-input data-orig="${escHtml(isetVal)}"
           ${!expertMode ? 'disabled' : ''} style="opacity:${expertMode ? '1' : '0.35'}">
    <button class="booster-btn b-btn-seti" data-set-i style="display:none"
            ${!expertMode ? 'disabled' : ''}>Set I</button>
    <span class="booster-controls-spacer"></span>
    <button class="booster-btn b-btn-on"  data-on>ON</button>
    <button class="booster-btn b-btn-off" data-off>OFF</button>
  </div>
</div>
<div class="booster-status" data-field="status">${boosterStatusHtml(s)}</div>`;
}

function updateBoosterCard(card, idx, s) {
    const wantCls = 'booster-card ' + boosterCardClass(s);
    if (card.className !== wantCls) card.className = wantCls;

    // Connection badge (top-right)
    const badge = card.querySelector('[data-field="conn-badge"]');
    if (badge) {
        const wantBadge = boosterConnBadgeHtml(s);
        if (badge.innerHTML !== wantBadge) badge.innerHTML = wantBadge;
    }

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

    // Status line (bottom)
    const statusEl = card.querySelector('[data-field="status"]');
    if (statusEl) {
        const wantStatus = boosterStatusHtml(s);
        if (statusEl.innerHTML !== wantStatus) statusEl.innerHTML = wantStatus;
    }

    // Expert mode guard
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
    const vsetInp = card.querySelector('[data-vset-input]');
    const setVBtn = card.querySelector('[data-set-v]');
    const isetInp = card.querySelector('[data-iset-input]');
    const setIBtn = card.querySelector('[data-set-i]');

    // VSet: show button on dirty, apply on click/Enter, revert on blur
    vsetInp.addEventListener('input', () => {
        setVBtn.style.display = vsetInp.value !== (vsetInp.dataset.orig || '') ? '' : 'none';
    });
    vsetInp.addEventListener('keydown', e => { if (e.key === 'Enter') setVBtn.click(); });
    vsetInp.addEventListener('blur', () => {
        setTimeout(() => {
            vsetInp.value = vsetInp.dataset.orig || '';
            setVBtn.style.display = 'none';
        }, 150);
    });
    setVBtn.addEventListener('mousedown', e => e.preventDefault());
    setVBtn.addEventListener('click', () => {
        if (!boosterMonitor || !expertMode) return;
        const v = parseFloat(vsetInp.value);
        if (isNaN(v) || v < 0) { vsetInp.style.borderColor = 'var(--red)'; return; }
        vsetInp.style.borderColor = '';
        boosterMonitor.setVoltage(idx, v);
        vsetInp.dataset.orig = vsetInp.value;
        setVBtn.style.display = 'none';
    });

    // ISet: same pattern
    isetInp.addEventListener('input', () => {
        setIBtn.style.display = isetInp.value !== (isetInp.dataset.orig || '') ? '' : 'none';
    });
    isetInp.addEventListener('keydown', e => { if (e.key === 'Enter') setIBtn.click(); });
    isetInp.addEventListener('blur', () => {
        setTimeout(() => {
            isetInp.value = isetInp.dataset.orig || '';
            setIBtn.style.display = 'none';
        }, 150);
    });
    setIBtn.addEventListener('mousedown', e => e.preventDefault());
    setIBtn.addEventListener('click', () => {
        if (!boosterMonitor || !expertMode) return;
        const v = parseFloat(isetInp.value);
        if (isNaN(v) || v < 0) { isetInp.style.borderColor = 'var(--red)'; return; }
        isetInp.style.borderColor = '';
        boosterMonitor.setCurrent(idx, v);
        isetInp.dataset.orig = isetInp.value;
        setIBtn.style.display = 'none';
    });

    // ON/OFF — unchanged
    card.querySelector('[data-on]').addEventListener('click', () => {
        if (boosterMonitor) boosterMonitor.setOutput(idx, true);
    });
    card.querySelector('[data-off]').addEventListener('click', () => {
        if (boosterMonitor) boosterMonitor.setOutput(idx, false);
    });
}

function boosterCardClass(s) {
    if (!s.connected || s.error) return 'card-fault';
    if (s.on && s.mode === 'CC')  return 'card-warn';
    if (s.on)                      return 'card-on';
    return '';
}

function boosterConnBadgeHtml(s) {
    if (s.connected) return '<span class="conn-ok">Connected</span>';
    if (s.error)     return '<span class="conn-err">Error</span>';
    return '<span class="conn-off">Offline</span>';
}

function boosterStatusHtml(s) {
    if (s.error) return '<span class="bst-error">' + escHtml(s.error) + '</span>';
    if (s.connected) return '<span class="bst-ok">Connected</span>';
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
