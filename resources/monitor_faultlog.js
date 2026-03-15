// ═════════════════════════════════════════════════════════════════════
//  Fault Log tab — live fault transition log from the daemon
// ═════════════════════════════════════════════════════════════════════
// Receives fault_log_snapshot messages from the daemon and renders
// the latest 200 entries in a colour-coded table (newest first).
//
// The initial snapshot contains the full ring buffer (up to 200).
// Subsequent snapshots are incremental (only new entries since the
// last broadcast), appended to the front of the local buffer.

let FAULT_LOG_MAX   = 200;   // updated from daemon init message
let faultLogEntries = [];    // newest-first
let faultLogDirty   = false;
let faultLogIsInitial = true;  // true until first full snapshot received
let faultLogUnread  = false;   // true when new entries arrived while tab not active

// ── Patch DaemonClient to route fault_log_snapshot messages ──────────
// ws_client.js is loaded before this script.  We wrap the prototype's
// _handleMessage so the DaemonClient doesn't need modification.
(function patchDaemonClient() {
    const orig = DaemonClient.prototype._handleMessage;
    DaemonClient.prototype._handleMessage = function(raw) {
        // Quick string check before parsing — avoids double JSON.parse
        // for the vast majority of messages (hv_snapshot, etc.)
        if (raw.indexOf('fault_log') === -1) {
            return orig.call(this, raw);
        }
        let msg;
        try { msg = JSON.parse(raw); } catch(e) { return orig.call(this, raw); }
        if (msg.type === 'fault_log_snapshot') {
            receiveFaultLog(msg.data);
            return;
        }
        // Pick up capacity from init message (contains "fault_log_capacity")
        if (msg.type === 'init' && msg.fault_log_capacity) {
            FAULT_LOG_MAX = msg.fault_log_capacity;
        }
        // Pass through to original handler (init, or false-positive indexOf match)
        return orig.call(this, raw);
    };
})();

// ── Clear buffer on reconnect so the fresh full snapshot is handled
//    correctly (not treated as incremental) ────────────────────────────
(function hookReconnect() {
    // DaemonClient fires onDisconnect listeners when the socket closes.
    // We can't access the `client` variable (scoped in monitor.js's
    // DOMContentLoaded), but we can observe the conn-pill text change
    // or simply rely on the init message.  Simplest: just reset the
    // flag so the next full snapshot replaces the buffer.
    //
    // The daemon sends fault_log_snapshot right after init+snapshots
    // on every new connection.  We detect "initial" by tracking it:
    // on disconnect, mark as initial again.
    const origConnect = DaemonClient.prototype._connect;
    DaemonClient.prototype._connect = function() {
        faultLogEntries = [];
        faultLogIsInitial = true;
        faultLogUnread = false;
        updateFaultLogDot();
        return origConnect.call(this);
    };
})();

function receiveFaultLog(entries) {
    if (!Array.isArray(entries) || entries.length === 0) return;

    // Daemon sends entries oldest-first.  Reverse for newest-first display.
    const incoming = entries.slice().reverse();

    const wasInitial = faultLogIsInitial;

    if (faultLogIsInitial) {
        // First message after (re)connect: full buffer — replace everything
        faultLogEntries = incoming.slice(0, FAULT_LOG_MAX);
        faultLogIsInitial = false;
    } else {
        // Incremental: prepend new entries
        faultLogEntries = incoming.concat(faultLogEntries).slice(0, FAULT_LOG_MAX);
    }
    faultLogDirty = true;

    // If the fault log tab is currently active, render immediately
    // and mark entries as read (no red dot).
    const active = document.querySelector('.tab-content.active');
    if (active && active.id === 'faultlog-tab') {
        renderFaultLog();
    } else if (!wasInitial) {
        // Incremental entries arrived while tab is hidden — show unread dot.
        // (Skip the initial full-buffer load on connect — that's history, not new.)
        faultLogUnread = true;
        updateFaultLogDot();
    }
}

// ── Rendering ────────────────────────────────────────────────────────
function renderFaultLog() {
    if (!faultLogDirty) return;
    faultLogDirty = false;

    const tbody = document.getElementById('faultlog-body');
    if (!tbody) return;

    // Full rebuild — the table is small (≤200 rows) and updates are
    // infrequent (only on fault transitions), so this is cheap.
    const frag = document.createDocumentFragment();

    for (const e of faultLogEntries) {
        const tr = document.createElement('tr');

        const isFault     = e.level === 'FAULT';
        const isAppear    = e.direction === 'APPEAR';
        tr.className = isFault ? 'fl-row-fault' : 'fl-row-warn';

        // Time
        const tdTime = document.createElement('td');
        tdTime.className = 'fl-time';
        tdTime.textContent = e.time || '';
        tr.appendChild(tdTime);

        // Level badge
        const tdLevel = document.createElement('td');
        const lvlSpan = document.createElement('span');
        lvlSpan.className = isFault ? 'fl-badge fl-badge-fault' : 'fl-badge fl-badge-warn';
        lvlSpan.textContent = e.level || '';
        tdLevel.appendChild(lvlSpan);
        tr.appendChild(tdLevel);

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

        // Status (show abbreviation part prominently, detail as title)
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

    // Update the entry count
    const countEl = document.getElementById('faultlog-count');
    if (countEl) {
        countEl.textContent = faultLogEntries.length + ' entries'
            + (faultLogEntries.length >= FAULT_LOG_MAX ? ' (max)' : '');
    }
}

// ── Unread indicator dot on the Fault Log tab button ─────────────────
function updateFaultLogDot() {
    const btn = document.querySelector('.tab-btn[data-tab="faultlog-tab"]');
    if (btn) btn.classList.toggle('has-fault', faultLogUnread);
}

// ── Hook into renderActiveTab so switching TO the fault log tab
//    triggers a render of any pending dirty data and clears the dot ───
(function patchRenderActiveTab() {
    const orig = window.renderActiveTab;
    if (!orig) return;
    window.renderActiveTab = function() {
        orig();
        const active = document.querySelector('.tab-content.active');
        if (active && active.id === 'faultlog-tab') {
            renderFaultLog();
            if (faultLogUnread) {
                faultLogUnread = false;
                updateFaultLogDot();
            }
        }
    };
})();
