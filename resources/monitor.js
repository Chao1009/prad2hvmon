// ═════════════════════════════════════════════════════════════════════
//  Module geometry data (loaded from backend via getModuleGeometry)
// ═════════════════════════════════════════════════════════════════════
let MODULES = [];
const MOD_MAP = {};

// ═════════════════════════════════════════════════════════════════════
//  Global state
// ═════════════════════════════════════════════════════════════════════
let accessLevel  = 0;        // 0=guest, 1=user, 2=expert
let authRequired = false;    // true if daemon has passwords configured
let expertMode   = false;    // computed alias: accessLevel >= 2
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

// Board status data
let allBoards    = [];
let boardDirty   = false;

// Booster HV supplies (TDK-Lambda, via BoosterMonitor WebChannel object)
let boosterMonitor  = null;
let boosterSupplies = [];   // latest snapshot [{idx,name,ip,connected,on,mode,vmon,vset,error}]
let boosterByName   = {};   // modName ('Booster1'…) → live booster data
let boosterDirty    = false;

// ── Audible alarm state ──────────────────────────────────────────────
let alarmMuted    = false;   // true when the user clicks the mute button
let alarmActive   = false;   // true while at least one fault/error exists
let alarmCtx      = null;    // AudioContext, created on first interaction

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

// ΔV display thresholds for table cell coloring (overridden by gui_config.json)
// NOTE: status classification (fault/warn/ok) is determined by the daemon,
// not by these thresholds.  These only control the green/amber/red text color
// of the ΔV column in the channel table.
let DV = {
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
//  Bootstrap (works with QWebChannel inside Qt OR WebSocket to daemon)
// ═════════════════════════════════════════════════════════════════════
document.addEventListener('DOMContentLoaded', () => {

    function bootstrap(hv, booster) {
        hvMonitor = hv;
        boosterMonitor = booster;
        if (boosterMonitor) {
            boosterMonitor.boosterUpdated.connect(jsonStr => {
                boosterSupplies = JSON.parse(jsonStr);
                applyPendingBoosterSets();
                boosterByName = {}; boosterSupplies.forEach(s => { boosterByName[s.name] = s; });
                boosterDirty = true;
                evaluateAlarm();
            });
            // Fetch static supply definitions to build cards immediately.
            boosterMonitor.readAll(jsonStr => {
                boosterSupplies = JSON.parse(jsonStr);
                boosterByName = {}; boosterSupplies.forEach(s => { boosterByName[s.name] = s; });
                boosterDirty = true;
            });
        }

        // Data update only — no render; the render loop handles display
        hvMonitor.channelsUpdated.connect(jsonStr => {
            allChannels = JSON.parse(jsonStr);
            applyPendingSets();
            const pwrRejected = applyPendingPower();
            if (pwrRejected.length > 0) {
                showToast(`Power ON rejected: ${pwrRejected.join(', ')} — check interlock`);
            }
            // Pre-compute classification once per tick — avoids thousands of
            // redundant classifyChannel() calls across table, geo, alarm, summary.
            for (const ch of allChannels) ch._cc = classifyChannel(ch);
            lastPollTime = performance.now();
            rebuildChMap();
            const crateKey = [...new Set(allChannels.map(c => c.crate))].sort().join('|');
            if (crateKey !== window._crateKey) { window._crateKey = crateKey; populateCrateChips(); }
            dataDirty = true;
            evaluateAlarm();
            // refreshAllPopups() is now called from renderActiveTab() when dirty,
            // so we don't rebuild popup DOM on every data tick outside the render loop.
        });

        hvMonitor.boardsUpdated.connect(jsonStr => {
            allBoards = JSON.parse(jsonStr);
            boardDirty = true;
            evaluateAlarm();
        });

        hvMonitor.readAll(jsonStr => {
            allChannels = JSON.parse(jsonStr);
            for (const ch of allChannels) ch._cc = classifyChannel(ch);
            rebuildChMap();
            populateCrateChips();
            dataDirty = true;
            hvMonitor.readBoards(bdJson => {
                allBoards = JSON.parse(bdJson);
                boardDirty = true;
            });
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
                        if (cfg.intervals.renderMs) {
                            renderIntervalMs = cfg.intervals.renderMs;
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
    }

    // ── Connect to daemon via WebSocket ─────────────────────────────
    const _params = new URLSearchParams(location.search);
    const wsHost = _params.get('host') || location.hostname || 'localhost';
    const wsPort = _params.get('port') || '8765';
    const client = new DaemonClient(`ws://${wsHost}:${wsPort}`);
    client._listeners.onConnect.push(() => {
        bootstrap(client.hvMonitor, client.boosterMonitor);
        // Read initial auth state from daemon
        client._withInit(init => {
            authRequired = !!init.auth_required;
            accessLevel = init.access_level != null ? init.access_level : 2;
            expertMode = (accessLevel >= 2);
            updateAccessPill();
            updateAccessUI();
        });
    });
    client._listeners.onDisconnect.push(() => {
        setPillConnected(false);
        clearAllPendingSets();
        // Reset to guest on disconnect — daemon will send fresh access_level on reconnect
        accessLevel = 0;
        expertMode = false;
        updateAccessPill();
        dataDirty = true;
        boosterDirty = true;
    });
    client._listeners.onAuthResult.push(onAuthResult);

    initTableUI();
    initTabs();
    initGeoMap();
    initBoosterTab();
    initLoginModal(client);

    // Render loop — driven by requestAnimationFrame so it never stacks,
    // skips hidden tabs automatically, and self-throttles to display rate.
    // renderIntervalMs caps how often we actually re-render, so we don't
    // burn CPU on every 16 ms frame when data only changes every 2 s.
    let lastRenderTs  = 0;
    let lastFooterSec = -1;  // guards "X s ago" footer — only write on whole-second changes
    let lastClockSec  = -1;  // guards header clock — same
    function renderLoop(ts) {
        // Re-render if CAEN data changed, if booster data changed and its
        // tab/geo tab is active (booster blocks are drawn on the geo canvas),
        // or if booster data changed while any popup is open (popups are
        // position-fixed and visible on top of any tab).
        const activeTabId = document.querySelector('.tab-content.active')?.id;
        const geoActive     = activeTabId === 'geo-tab';
        const boosterActive = activeTabId === 'booster-tab';
        const boardActive   = activeTabId === 'board-tab';
        const shouldRender = (dataDirty && allChannels.length > 0)
                           || (boosterDirty && (boosterActive || geoActive || popups.size > 0))
                           || (boardDirty && boardActive);
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

// ═════════════════════════════════════════════════════════════════════
//  Access control — auth result handler + login modal
// ═════════════════════════════════════════════════════════════════════

function onAuthResult(msg) {
    accessLevel = msg.granted;
    expertMode = (accessLevel >= 2);
    updateAccessPill();
    updateAccessUI();

    const modal   = document.getElementById('login-overlay');
    const errorEl = document.getElementById('login-error');

    if (msg.granted === msg.requested) {
        // Success — close modal
        modal.classList.add('hidden');
        errorEl.textContent = '';
    } else if (msg.reason) {
        // Denied — show error, keep modal open
        errorEl.textContent = msg.reason;
        errorEl.classList.add('shake');
        setTimeout(() => errorEl.classList.remove('shake'), 400);
    }
}

function updateAccessPill() {
    const icon  = document.getElementById('access-icon');
    const label = document.getElementById('access-label');
    const pill  = document.getElementById('access-pill');
    if (!pill) return;
    const names = ['Guest', 'User', 'Expert'];
    const icons = ['👁', '⚡', '🔧'];
    const cls   = ['access-guest', 'access-user', 'access-expert'];
    label.textContent = names[accessLevel] || 'Guest';
    icon.textContent  = icons[accessLevel] || '👁';
    pill.className    = 'access-pill ' + (cls[accessLevel] || 'access-guest');
}

// Update all access-gated UI elements when accessLevel changes
function updateAccessUI() {
    dataDirty = true;
    boosterDirty = true;
    boardDirty = true;
    // Load button: Expert only
    const btnLoad = document.getElementById('btn-load-settings');
    if (btnLoad) { btnLoad.disabled = (accessLevel < 2); btnLoad.style.opacity = (accessLevel < 2) ? '0.35' : '1'; }
    // Bulk ON/OFF: User or higher
    const btnAllOn  = document.getElementById('btn-all-on');
    const btnAllOff = document.getElementById('btn-all-off');
    if (btnAllOn)  { btnAllOn.disabled  = (accessLevel < 1); btnAllOn.style.opacity  = (accessLevel < 1) ? '0.35' : '1'; }
    if (btnAllOff) { btnAllOff.disabled = (accessLevel < 1); btnAllOff.style.opacity = (accessLevel < 1) ? '0.35' : '1'; }
    // Bulk Set V group: Expert only
    const bulkSetv = document.getElementById('bulk-setv-group');
    if (bulkSetv) bulkSetv.style.display = expertMode ? '' : 'none';
}

function initLoginModal(client) {
    const pill    = document.getElementById('access-pill');
    const overlay = document.getElementById('login-overlay');
    if (!pill || !overlay) return;
    const radios    = overlay.querySelectorAll('input[name="login-level"]');
    const pwRow     = document.getElementById('login-pw-row');
    const pwInput   = document.getElementById('login-pw');
    const errorEl   = document.getElementById('login-error');
    const btnGo     = document.getElementById('login-go');
    const btnCancel = document.getElementById('login-cancel');

    function getSelectedLevel() {
        const checked = overlay.querySelector('input[name="login-level"]:checked');
        return checked ? parseInt(checked.value) : 0;
    }

    pill.addEventListener('click', () => {
        // Pre-select current level
        radios.forEach(r => { r.checked = (parseInt(r.value) === accessLevel); });
        pwInput.value = '';
        errorEl.textContent = '';
        pwRow.style.display = getSelectedLevel() > 0 ? '' : 'none';
        overlay.classList.remove('hidden');
        if (getSelectedLevel() > 0) pwInput.focus();
    });

    radios.forEach(r => r.addEventListener('change', () => {
        const lv = getSelectedLevel();
        pwRow.style.display = lv > 0 ? '' : 'none';
        errorEl.textContent = '';
        if (lv > 0) setTimeout(() => pwInput.focus(), 0);
    }));

    btnGo.addEventListener('click', () => {
        const lv = getSelectedLevel();
        if (lv === 0) {
            client.authenticate(0);
        } else {
            client.authenticate(lv, pwInput.value);
        }
    });

    pwInput.addEventListener('keydown', e => {
        if (e.key === 'Enter') btnGo.click();
    });

    btnCancel.addEventListener('click', () => {
        overlay.classList.add('hidden');
    });

    overlay.addEventListener('click', e => {
        if (e.target === overlay) overlay.classList.add('hidden');
    });
}

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
    else if (active.id === 'board-tab') { if (boardDirty) { renderBoardTable(); boardDirty = false; } updateFooter(); }
    else if (active.id === 'geo-tab') { renderGeo(); updateFooter(); }
    else if (active.id === 'booster-tab') updateFooter();
    // Render booster cards only when their tab is active (4 cards = cheap,
    // but avoids rebuilding card DOM while viewing other tabs).
    if (boosterDirty) {
        if (active.id === 'booster-tab') renderBoosterCards();
        boosterDirty = false;
    }
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
            dataDirty = true;
            // Ensure deferred content renders fresh when switching to its tab
            if (btn.dataset.tab === 'booster-tab') boosterDirty = true;
            if (btn.dataset.tab === 'board-tab')   boardDirty = true;
            renderActiveTab();
            if (btn.dataset.tab === 'geo-tab') {
                resizeGeoCanvas();
                resetGeoView();
            }
        });
    });
}


// ═════════════════════════════════════════════════════════════════════
//  Unified channel status classification
// ═════════════════════════════════════════════════════════════════════
// classifyChannel(ch) reads the daemon-provided authoritative status.
// The daemon sets ch.level ('fault'|'suppressed'|'warn'|'ramp'|'on'|'off')
// and ch.dv_warn (bool).  This function derives CSS classes, badge HTML,
// and convenience booleans for the rendering layers.
//
// No classification logic lives here — all decisions are made by the daemon.

const _working = new Set(['OFF', 'ON', 'RUP', 'RDN']);

function classifyChannel(ch) {
    const abbr   = ch.status ? ch.status.split('|')[0].trim() : '';
    const detail = ch.status ? (ch.status.split('|')[1] || '').trim() : '';
    const tokens = abbr ? abbr.split(/\s+/).filter(t => t !== '') : [];

    // Token categories (for badge rendering only — NOT for classification)
    const faultTokens      = tokens.filter(t => !_working.has(t) && !t.startsWith('~'));
    const suppressedTokens = tokens.filter(t => t.startsWith('~')).map(t => t.slice(1));
    const isRamping        = tokens.includes('RUP') || tokens.includes('RDN');

    // Daemon-authoritative fields
    const level  = ch.level || 'off';
    const dvWarn = ch.dv_warn || false;

    // Derive CSS class and dot from daemon level
    let cssClass, dot;
    switch (level) {
        case 'fault':      cssClass = 'status-err';  dot = 'fault'; break;
        case 'suppressed': cssClass = 'status-warn'; dot = 'warn';  break;
        case 'warn':       cssClass = 'status-work'; dot = 'warn';  break;
        case 'ramp':       cssClass = 'status-work'; dot = 'on';    break;
        case 'on':         cssClass = 'status-work'; dot = 'on';    break;
        default:           cssClass = 'status-ok';   dot = 'off';   break;
    }

    // Build badges HTML (visual rendering of the daemon's status tokens)
    const badges = [];
    if (isRamping) {
        const rampDir = tokens.includes('RUP') ? 'RUP' : 'RDN';
        badges.push(`<span class="st-ramp">${rampDir}</span>`);
    }
    if (faultTokens.length > 0) {
        const tip = detail ? ` title="${detail}"` : '';
        badges.push(`<span class="st-fault"${tip}>${faultTokens.join('\u2009')}</span>`);
    }
    if (suppressedTokens.length > 0) {
        const display = suppressedTokens.join('\u2009');
        badges.push(`<span class="st-warn" title="Suppressed: ${display}">${display}</span>`);
    }
    if (dvWarn) badges.push('<span class="st-warn">\u0394V</span>');

    const badgesHtml = badges.length ? `<span class="st-badges">${badges.join(' ')}</span>` : '';

    return {
        level, cssClass, dot,
        isFault: level === 'fault',
        isWarn:  level === 'suppressed' || level === 'warn',
        isSettled: level === 'on' || level === 'warn' || level === 'suppressed',
        faultTokens, suppressedTokens,
        dvWarn, badgesHtml
    };
}

// Power state badge for tooltip / popup Pwr row
function pwrHtml(ch) {
    const abbr = ch.status ? ch.status.split('|')[0].trim() : '';
    if (abbr === 'RUP' || abbr === 'RDN') return `<span class="st-ramp">${abbr}</span>`;
    return ch.on ? '<span class="st-on">ON</span>' : '<span class="st-off">OFF</span>';
}


// ═════════════════════════════════════════════════════════════════════
//  Toast notifications
// ═════════════════════════════════════════════════════════════════════
function showToast(message, durationMs) {
    durationMs = durationMs || 5000;
    let container = document.getElementById('toast-container');
    if (!container) {
        container = document.createElement('div');
        container.id = 'toast-container';
        document.body.appendChild(container);
    }
    const el = document.createElement('div');
    el.className = 'toast-msg';
    el.textContent = message;
    container.appendChild(el);
    // Trigger CSS entrance animation
    requestAnimationFrame(() => el.classList.add('visible'));
    setTimeout(() => {
        el.classList.remove('visible');
        el.addEventListener('transitionend', () => el.remove());
    }, durationMs);
}
