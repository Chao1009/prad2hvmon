// ═════════════════════════════════════════════════════════════════════
//  Fault Log tab — split into Faults (top) and Warnings (bottom)
// ═════════════════════════════════════════════════════════════════════
// The daemon sends two separate ring buffers:
//   fault_log_faults  — hardware faults (OVC, TRIP, etc.)
//   fault_log_warns   — warnings (ΔV, suppressed errors)
// Each has its own capacity and version tracking on the daemon side,
// so a flood of warnings never flushes faults.

let FAULT_LOG_MAX = 200;   // per-buffer capacity (from daemon init)

// Two client-side buffers (newest-first)
let flFaults  = [];
let flWarns   = [];
let flFaultDirty = false;
let flWarnDirty  = false;
let flFaultIsInitial = true;
let flWarnIsInitial  = true;

// Separate unread tracking for tab dots
let flUnreadFaults = false;
let flUnreadWarns  = false;

// ── Patch DaemonClient to route fault log messages ───────────────────
(function patchDaemonClient() {
    const orig = DaemonClient.prototype._handleMessage;
    DaemonClient.prototype._handleMessage = function(raw) {
        if (raw.indexOf('fault_log') === -1) {
            return orig.call(this, raw);
        }
        let msg;
        try { msg = JSON.parse(raw); } catch(e) { return orig.call(this, raw); }

        if (msg.type === 'fault_log_faults') {
            receiveFaultBuffer(msg.data, true);
            return;
        }
        if (msg.type === 'fault_log_warns') {
            receiveFaultBuffer(msg.data, false);
            return;
        }
        // Pick up capacity from init message
        if (msg.type === 'init' && msg.fault_log_capacity) {
            FAULT_LOG_MAX = msg.fault_log_capacity;
        }
        return orig.call(this, raw);
    };
})();

// ── Clear buffers on reconnect ───────────────────────────────────────
(function hookReconnect() {
    const origConnect = DaemonClient.prototype._connect;
    DaemonClient.prototype._connect = function() {
        flFaults = [];
        flWarns  = [];
        flFaultIsInitial = true;
        flWarnIsInitial  = true;
        flUnreadFaults = false;
        flUnreadWarns  = false;
        updateFaultLogDots();
        return origConnect.call(this);
    };
})();

// ── Receive entries for one buffer ───────────────────────────────────
function receiveFaultBuffer(entries, isFault) {
    if (!Array.isArray(entries)) return;

    // Daemon sends oldest-first; reverse for newest-first display
    const incoming = entries.slice().reverse();

    const isInitial = isFault ? flFaultIsInitial : flWarnIsInitial;
    const wasInitial = isInitial;

    if (isInitial) {
        // First message after (re)connect — full buffer, replace everything
        if (isFault) { flFaults = incoming.slice(0, FAULT_LOG_MAX); flFaultIsInitial = false; }
        else         { flWarns  = incoming.slice(0, FAULT_LOG_MAX); flWarnIsInitial  = false; }
        if (incoming.length === 0) return;
    } else {
        if (incoming.length === 0) return;
        // Incremental: prepend new entries
        if (isFault) flFaults = incoming.concat(flFaults).slice(0, FAULT_LOG_MAX);
        else         flWarns  = incoming.concat(flWarns).slice(0, FAULT_LOG_MAX);
    }

    if (isFault) flFaultDirty = true;
    else         flWarnDirty  = true;

    const active = document.querySelector('.tab-content.active');
    if (active && active.id === 'faultlog-tab') {
        renderFaultLog();
    } else if (!wasInitial) {
        // Incremental entries while tab hidden — set unread dot
        if (isFault) flUnreadFaults = true;
        else         flUnreadWarns  = true;
        updateFaultLogDots();
    }
}

// ── Rendering ────────────────────────────────────────────────────────
function renderFaultLog() {
    if (flFaultDirty) {
        renderFaultPanel('fl-fault-body', flFaults, 'fl-fault-count', true);
        flFaultDirty = false;
    }
    if (flWarnDirty) {
        renderFaultPanel('fl-warn-body', flWarns, 'fl-warn-count', false);
        flWarnDirty = false;
    }
}

function renderFaultPanel(tbodyId, entries, countId, isFaultPanel) {
    const tbody = document.getElementById(tbodyId);
    if (!tbody) return;

    const frag = document.createDocumentFragment();

    for (const e of entries) {
        const tr = document.createElement('tr');
        const isAppear = e.direction === 'APPEAR';
        tr.className = isFaultPanel ? 'fl-row-fault' : 'fl-row-warn';

        // Time
        const tdTime = document.createElement('td');
        tdTime.className = 'fl-time';
        tdTime.textContent = e.time || '';
        tr.appendChild(tdTime);

        // Direction badge
        const tdDir = document.createElement('td');
        const dirSpan = document.createElement('span');
        dirSpan.className = isAppear ? 'fl-badge fl-badge-appear' : 'fl-badge fl-badge-disappear';
        dirSpan.textContent = e.direction || '';
        tdDir.appendChild(dirSpan);
        tr.appendChild(tdDir);

        // Type
        const tdType = document.createElement('td');
        tdType.textContent = e.type || '';
        tr.appendChild(tdType);

        // Name
        const tdName = document.createElement('td');
        tdName.className = 'fl-name';
        tdName.textContent = e.name || '';
        tr.appendChild(tdName);

        // VMon
        const tdVmon = document.createElement('td');
        tdVmon.style.textAlign = 'right';
        if (e.vmon != null) {
            tdVmon.textContent = Number(e.vmon).toFixed(2);
        } else {
            tdVmon.textContent = '';
            tdVmon.style.color = 'var(--text-dim)';
        }
        tr.appendChild(tdVmon);

        // VSet
        const tdVset = document.createElement('td');
        tdVset.style.textAlign = 'right';
        if (e.vset != null) {
            tdVset.textContent = Number(e.vset).toFixed(2);
        } else {
            tdVset.textContent = '';
            tdVset.style.color = 'var(--text-dim)';
        }
        tr.appendChild(tdVset);

        // Status
        const tdStatus = document.createElement('td');
        tdStatus.className = 'fl-status';
        const statusStr = e.status || '';
        const pipeIdx = statusStr.indexOf('|');
        if (pipeIdx >= 0) {
            tdStatus.textContent = statusStr.substring(0, pipeIdx);
            tdStatus.title = statusStr.substring(pipeIdx + 1);
        } else {
            tdStatus.textContent = statusStr;
        }
        tr.appendChild(tdStatus);

        frag.appendChild(tr);
    }

    tbody.innerHTML = '';
    tbody.appendChild(frag);

    const countEl = document.getElementById(countId);
    if (countEl) {
        countEl.textContent = entries.length + ' entries'
            + (entries.length >= FAULT_LOG_MAX ? ' (max)' : '');
    }
}

// ── Tab dots — red for faults, amber for warnings ────────────────────
function updateFaultLogDots() {
    const btn = document.querySelector('.tab-btn[data-tab="faultlog-tab"]');
    if (!btn) return;
    btn.classList.toggle('has-fault', flUnreadFaults);
    btn.classList.toggle('has-warn',  flUnreadWarns);
}

// ── Hook into renderActiveTab ────────────────────────────────────────
(function patchRenderActiveTab() {
    const orig = window.renderActiveTab;
    if (!orig) return;
    window.renderActiveTab = function() {
        orig();
        const active = document.querySelector('.tab-content.active');
        if (active && active.id === 'faultlog-tab') {
            renderFaultLog();
            if (flUnreadFaults || flUnreadWarns) {
                flUnreadFaults = false;
                flUnreadWarns  = false;
                updateFaultLogDots();
            }
        }
    };
})();

// ── Draggable divider ────────────────────────────────────────────────
(function initDivider() {
    document.addEventListener('DOMContentLoaded', () => {
        const divider    = document.getElementById('fl-divider');
        const faultPanel = document.getElementById('fl-fault-panel');
        const warnPanel  = document.getElementById('fl-warn-panel');
        if (!divider || !faultPanel || !warnPanel) return;

        let dragging = false;

        divider.addEventListener('mousedown', e => {
            e.preventDefault();
            dragging = true;
            document.body.style.cursor = 'row-resize';
            document.body.style.userSelect = 'none';
        });

        window.addEventListener('mousemove', e => {
            if (!dragging) return;
            const tab = document.getElementById('faultlog-tab');
            if (!tab) return;
            const rect = tab.getBoundingClientRect();
            const y = e.clientY - rect.top;
            const minPx = 60;
            const maxPx = rect.height - divider.offsetHeight - 60;
            const clamped = Math.max(minPx, Math.min(maxPx, y));
            const pct = (clamped / rect.height) * 100;
            faultPanel.style.flex = 'none';
            warnPanel.style.flex  = 'none';
            faultPanel.style.height = pct + '%';
            warnPanel.style.height  = (100 - pct) + '%';
        });

        window.addEventListener('mouseup', () => {
            if (!dragging) return;
            dragging = false;
            document.body.style.cursor = '';
            document.body.style.userSelect = '';
        });
    });
})();
