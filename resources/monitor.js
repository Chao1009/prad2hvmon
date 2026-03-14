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

// Board status data
let allBoards    = [];
let boardDirty   = false;

// Booster HV supplies (TDK-Lambda, via BoosterMonitor WebChannel object)
let boosterMonitor  = null;
let boosterSupplies = [];   // latest snapshot [{idx,name,ip,connected,on,mode,vmon,vset,error}]
let boosterByName   = {};   // modName ('Booster1'…) → live booster data
let boosterDirty    = false;
let boosterConnected  = false;  // whether we have an active TCP connection to boosters
let boosterConnecting = false;  // true between Connect click and first real poll result
let boosterSeenClean  = false;  // true after seeing a "clean" disconnect snapshot (all disconnected, no errors)

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
                // Two-phase "connecting" detection for Retry robustness:
                //
                // On Retry, the worker thread queue may look like:
                //   [stale doPoll] → [disconnectAll] → [connectAll → doPoll]
                //
                // Phase 1: wait for the "clean" disconnect snapshot (all disconnected,
                //          no errors).  Stale poll snapshots (with errors from the
                //          previous attempt) are ignored during this phase.
                // Phase 2: wait for the first real poll result (connected or errored).
                //          This is the outcome of our connectAll request.
                //
                // For initial Connect (no Retry), boosterSeenClean is pre-set to
                // true so we skip Phase 1 and go straight to Phase 2.
                if (boosterConnecting) {
                    if (!boosterSeenClean) {
                        // Phase 1: look for the disconnect snapshot
                        const isClean = boosterSupplies.every(s => !s.connected && !s.error);
                        if (isClean) boosterSeenClean = true;
                        // ignore stale poll snapshots (with errors) — don't clear boosterConnecting
                    } else {
                        // Phase 2: look for a real poll result
                        const attemptDone = boosterSupplies.some(s => s.connected || s.error);
                        if (attemptDone) {
                            boosterConnecting = false;
                            boosterSeenClean  = false;
                            updateBoosterHeaderButtons();
                        }
                    }
                }
                boosterDirty = true;
                evaluateAlarm();
            });
            // Fetch static supply definitions (name, ip) to build cards now.
            // The cache is pre-populated by the C++ constructor with all supplies
            // in disconnected state, so cards appear immediately behind the overlay.
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

    // Detached-window mode: if launched with ?tab=<id>, auto-switch to that
    // tab and mark <body> so CSS hides header/tab-bar/footer.
    const _detachTab = new URLSearchParams(location.search).get('tab');
    if (_detachTab) {
        document.body.classList.add('detached');
        const btn = document.querySelector(`.tab-btn[data-tab="${_detachTab}"]`);
        if (btn) btn.click();
    }

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
        const activeTabId = document.querySelector('.tab-content.active')?.id;
        const geoActive     = activeTabId === 'geo-tab';
        const boosterActive = activeTabId === 'booster-tab';
        const boardActive   = activeTabId === 'board-tab';
        const shouldRender = (dataDirty && allChannels.length > 0)
                           || (boosterDirty && (geoActive || boosterActive))
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

    // Add ⤢ pop-out buttons to detachable tabs (board, geo, and booster)
    ['board-tab', 'geo-tab', 'booster-tab'].forEach(tabId => {
        const btn = document.querySelector(`.tab-btn[data-tab="${tabId}"]`);
        if (!btn) return;
        const pop = document.createElement('button');
        pop.className = 'tab-detach-btn';
        pop.title = 'Open in separate window';
        pop.textContent = '⤢';
        pop.addEventListener('click', e => {
            e.stopPropagation();   // don't also switch the tab
            if (hvMonitor) hvMonitor.openDetachedView(tabId);
        });
        btn.appendChild(pop);
    });
}


// ═════════════════════════════════════════════════════════════════════
//  Unified channel status classification
// ═════════════════════════════════════════════════════════════════════
// classifyChannel(ch) is the SINGLE source of truth for all status
// decisions across table, geo, alarm, and summary.
//
// Returns: {
//   level:     'fault' | 'suppressed' | 'warn' | 'on' | 'ramp' | 'off'
//   cssClass:  'status-err' | 'status-warn' | 'status-work' | 'status-ok'
//   dot:       'fault' | 'warn' | 'on' | 'off'
//   isFault:   bool    — triggers alarm, red tab dots, red count
//   isWarn:    bool    — ΔV warning or suppressed errors (amber)
//   isSettled: bool    — fully ON, not ramping (eligible for ΔV check)
//   faultTokens:     string[]  — unsuppressed error abbreviations
//   suppressedTokens: string[] — suppressed error abbreviations (without ~)
//   dvWarn:    bool    — ΔV exceeds threshold
//   badgesHtml: string — pre-rendered status badges HTML
// }

const _working = new Set(['OFF', 'ON', 'RUP', 'RDN']);

function classifyChannel(ch) {
    const abbr   = ch.status ? ch.status.split('|')[0].trim() : '';
    const detail = ch.status ? (ch.status.split('|')[1] || '').trim() : '';
    const tokens = abbr ? abbr.split(/\s+/).filter(t => t !== '') : [];

    // Separate token categories
    const faultTokens     = tokens.filter(t => !_working.has(t) && !t.startsWith('~'));
    const suppressedTokens = tokens.filter(t => t.startsWith('~')).map(t => t.slice(1));
    const isRamping       = tokens.includes('RUP') || tokens.includes('RDN');
    const isOn            = tokens.includes('ON') || ch.on;
    const isSettled        = tokens.length > 0 && tokens.every(t => t === 'ON' || t.startsWith('~'));

    // ΔV warning (only when settled)
    const dvWarn = isSettled && ch.vmon != null && ch.vset != null
                   && Math.abs(ch.vmon - ch.vset) > DV.warn_threshold;

    // Determine level (priority: fault > suppressed > dvWarn > ramp > on > off)
    const hasFault      = faultTokens.length > 0;
    const hasSuppressed = suppressedTokens.length > 0;

    let level, cssClass, dot;
    if (hasFault) {
        level = 'fault'; cssClass = 'status-err'; dot = 'fault';
    } else if (hasSuppressed) {
        level = 'suppressed'; cssClass = 'status-warn'; dot = 'warn';
    } else if (dvWarn) {
        level = 'warn'; cssClass = 'status-work'; dot = 'warn';
    } else if (isRamping) {
        level = 'ramp'; cssClass = 'status-work'; dot = 'on';
    } else if (isOn) {
        level = 'on'; cssClass = 'status-work'; dot = 'on';
    } else {
        level = 'off'; cssClass = 'status-ok'; dot = 'off';
    }

    // Build badges HTML
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
        isFault: hasFault,
        isWarn: hasSuppressed || dvWarn,
        isSettled,
        faultTokens, suppressedTokens,
        dvWarn, badgesHtml
    };
}

// ── Convenience wrappers (for backward compat with callers) ──────────
// These are thin wrappers; new code should use classifyChannel() directly.
function statusClass(s) {
    // Accepts a status string directly (not a channel object)
    return classifyChannel({status: s, on: false, vmon: null, vset: null}).cssClass;
}
function isSettled(ch) { return classifyChannel(ch).isSettled; }
function dotClass(ch)  { return classifyChannel(ch).dot; }
function statusBadgesHtml(ch) { return classifyChannel(ch).badgesHtml; }

// Power state badge for tooltip / popup Pwr row
function pwrHtml(ch) {
    const abbr = ch.status ? ch.status.split('|')[0].trim() : '';
    if (abbr === 'RUP' || abbr === 'RDN') return `<span class="st-ramp">${abbr}</span>`;
    return ch.on ? '<span class="st-on">ON</span>' : '<span class="st-off">OFF</span>';
}



