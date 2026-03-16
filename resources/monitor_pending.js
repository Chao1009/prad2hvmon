// ═════════════════════════════════════════════════════════════════════
//  monitor_pending.js — Client-side pending-sets overlay
//
//  Prevents incoming hv_snapshot / booster_snapshot from overwriting
//  values the user just set via expert-mode inline editing.
//
//  Must be loaded AFTER monitor.js (which declares allChannels etc.)
//  and BEFORE monitor_table.js / monitor_geo.js / monitor_booster.js
//  (which call addPendingSet).
//
//  Multi-user safety:
//    Each entry stores the snapshot value at the moment of the edit
//    (origSnapshot). If a subsequent snapshot value differs from BOTH
//    the pending value AND the original, another user changed it —
//    the entry is cleared and the snapshot is trusted.
// ═════════════════════════════════════════════════════════════════════

// ── HV channel pending sets ──────────────────────────────────────────
// Key: "crate|slot|channel|param"
// Value: { value: Number, origSnapshot: Number, ts: Number }
const pendingSets = new Map();
const PENDING_TTL_MS = 5000;   // 5 seconds
const PENDING_TOL    = 0.01;

/**
 * Record a pending set-value for an HV channel parameter.
 *
 * @param {string} crate
 * @param {number} slot
 * @param {number} channel
 * @param {string} param   - "vset", "iset", or "svmax"
 * @param {number} value   - the value the user just wrote
 * @param {number} origSnapshot - the snapshot value BEFORE the edit
 */
function addPendingSet(crate, slot, channel, param, value, origSnapshot) {
    const key = `${crate}|${slot}|${channel}|${param}`;
    pendingSets.set(key, {
        value,
        origSnapshot: (origSnapshot != null && !isNaN(origSnapshot)) ? origSnapshot : null,
        ts: performance.now()
    });
}

/**
 * Apply pending overrides to the freshly-parsed allChannels array.
 * Called from the channelsUpdated handler in monitor.js, right after
 * allChannels = JSON.parse(jsonStr).
 *
 * Clears entries that have:
 *   - expired (TTL)
 *   - converged (snapshot matches pending value)
 *   - been superseded by another user (snapshot moved away from both
 *     the original and pending values)
 */
function applyPendingSets() {
    if (pendingSets.size === 0) return;
    const now = performance.now();

    for (const [key, entry] of pendingSets) {
        // Expire old entries
        if (now - entry.ts > PENDING_TTL_MS) {
            pendingSets.delete(key);
            continue;
        }

        const [crate, slotStr, chStr, param] = key.split('|');
        const slot    = parseInt(slotStr, 10);
        const channel = parseInt(chStr, 10);

        const ch = allChannels.find(c =>
            c.crate === crate && c.slot === slot && c.channel === channel);
        if (!ch) continue;

        // Get the snapshot value for this param
        let sv = null;
        if      (param === 'vset')  sv = ch.vset;
        else if (param === 'iset')  sv = ch.iset;
        else if (param === 'svmax') sv = ch.svmax;

        // Case 1: snapshot matches pending value — hardware caught up
        if (sv != null && Math.abs(sv - entry.value) < PENDING_TOL) {
            pendingSets.delete(key);
            continue;
        }

        // Case 2: snapshot moved away from the original value but does NOT
        // match our pending value — another user (or the daemon) changed it.
        // Trust the snapshot.
        if (entry.origSnapshot != null && sv != null &&
            Math.abs(sv - entry.origSnapshot) > PENDING_TOL) {
            pendingSets.delete(key);
            continue;
        }

        // Case 3: snapshot still shows the original (stale) value —
        // hardware hasn't caught up yet.  Override it.
        if      (param === 'vset')  ch.vset  = entry.value;
        else if (param === 'iset')  ch.iset  = entry.value;
        else if (param === 'svmax') ch.svmax = entry.value;
    }
}


// ── Booster pending sets ─────────────────────────────────────────────
// Key: "idx|param"
// Value: { value: Number, origSnapshot: Number, ts: Number }
const pendingBoosterSets = new Map();

function addPendingBoosterSet(idx, param, value, origSnapshot) {
    const key = `${idx}|${param}`;
    pendingBoosterSets.set(key, {
        value,
        origSnapshot: (origSnapshot != null && !isNaN(origSnapshot)) ? origSnapshot : null,
        ts: performance.now()
    });
}

function applyPendingBoosterSets() {
    if (pendingBoosterSets.size === 0) return;
    const now = performance.now();

    for (const [key, entry] of pendingBoosterSets) {
        if (now - entry.ts > PENDING_TTL_MS) {
            pendingBoosterSets.delete(key);
            continue;
        }

        const [idxStr, param] = key.split('|');
        const idx = parseInt(idxStr, 10);
        if (idx < 0 || idx >= boosterSupplies.length) continue;

        const s = boosterSupplies[idx];
        const sv = (param === 'vset') ? s.vset : s.iset;

        if (sv != null && Math.abs(sv - entry.value) < PENDING_TOL) {
            pendingBoosterSets.delete(key);
            continue;
        }

        if (entry.origSnapshot != null && sv != null &&
            Math.abs(sv - entry.origSnapshot) > PENDING_TOL) {
            pendingBoosterSets.delete(key);
            continue;
        }

        if (param === 'vset') s.vset = entry.value;
        else                  s.iset = entry.value;
    }
}

// ── Disconnect cleanup ───────────────────────────────────────────────
// If the WebSocket drops, no snapshots arrive, so applyPending*() never
// runs and stale entries would sit forever.  Clear both maps on disconnect
// so a reconnect starts fresh.
function clearAllPendingSets() {
    pendingSets.clear();
    pendingBoosterSets.clear();
}
