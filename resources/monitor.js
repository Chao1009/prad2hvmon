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
    });
    client._listeners.onDisconnect.push(() => setPillConnected(false));

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
        const activeTabId = document.querySelector('.tab-content.active')?.id;
        const geoActive     = activeTabId === 'geo-tab';
        const boosterActive = activeTabId === 'booster-tab';
        const boardActive   = activeTabId === 'board-tab';
        const shouldRender = (dataDirty && allChannels.length > 0)
                           || boosterDirty
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
    else if (active.id === 'booster-tab') updateFooter();
    // Always render booster cards when dirty — they live in the DOM even when
    // the tab is hidden, so they're ready when the user switches to it.
    if (boosterDirty) { renderBoosterCards(); boosterDirty = false; }
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



