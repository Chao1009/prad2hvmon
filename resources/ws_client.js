// ─────────────────────────────────────────────────────────────────────────────
// ws_client.js — WebSocket client adapter for prad2hvd
//
// Provides hvMonitor and boosterMonitor objects with the same API surface
// as the QWebChannel versions, so monitor.js requires minimal changes.
//
// Usage:
//   Replace the QWebChannel bootstrap block with:
//     const ws = new DaemonClient('ws://hostname:8765');
//     const hvMonitor = ws.hvMonitor;
//     const boosterMonitor = ws.boosterMonitor;
//
// The daemon sends:
//   {type:"init",       module_geometry:[...], gui_config:{...}, daq_map:[...]}
//   {type:"hv_snapshot",      data:[...]}
//   {type:"board_snapshot",   data:[...]}
//   {type:"booster_snapshot", data:[...]}
//
// The client sends commands as JSON:
//   {type:"set_power", crate:"...", slot:N, ch:N, on:bool}
//   {type:"set_voltage", crate:"...", slot:N, ch:N, value:F}
//   etc.
// ─────────────────────────────────────────────────────────────────────────────

class DaemonClient {
    constructor(url) {
        this._url = url;
        this._ws = null;
        this._connected = false;

        // Callback registries (mimic QWebChannel signal.connect())
        this._listeners = {
            channelsUpdated: [],
            boardsUpdated:   [],
            boosterUpdated:  [],
            onConnect:       [],
            onDisconnect:    [],
        };

        // Cached data for request-response style calls
        this._initData       = null;    // from "init" message
        this._lastHV         = '[]';
        this._lastBoards     = '[]';
        this._lastBooster    = '[]';

        // Pending callbacks for readAll/readBoards/etc. (called once init arrives)
        this._pendingInit    = [];

        // Public API objects (same shape as QWebChannel objects)
        this.hvMonitor       = this._createHVMonitor();
        this.boosterMonitor  = this._createBoosterMonitor();

        this._connect();
    }

    // ── WebSocket lifecycle ──────────────────────────────────────────────

    _connect() {
        this._ws = new WebSocket(this._url);

        this._ws.onopen = () => {
            console.log('Daemon connected:', this._url);
            this._connected = true;
            this._listeners.onConnect.forEach(fn => fn());
        };

        this._ws.onclose = (ev) => {
            console.warn('Daemon disconnected:', ev.code, ev.reason);
            this._connected = false;
            this._listeners.onDisconnect.forEach(fn => fn());
            // Auto-reconnect after 2s
            setTimeout(() => this._connect(), 2000);
        };

        this._ws.onerror = (err) => {
            console.error('WebSocket error:', err);
        };

        this._ws.onmessage = (ev) => {
            this._handleMessage(ev.data);
        };
    }

    _send(obj) {
        if (this._ws && this._ws.readyState === WebSocket.OPEN) {
            this._ws.send(JSON.stringify(obj));
        }
    }

    _handleMessage(raw) {
        let msg;
        try { msg = JSON.parse(raw); } catch (e) { console.error('Bad JSON from daemon:', e); return; }

        switch (msg.type) {
        case 'init':
            this._initData = msg;
            // Flush any pending readAll / getModuleGeometry / etc. callbacks
            for (const cb of this._pendingInit) cb(msg);
            this._pendingInit = [];
            break;

        case 'hv_snapshot':
            this._lastHV = JSON.stringify(msg.data);
            this._listeners.channelsUpdated.forEach(fn => fn(this._lastHV));
            break;

        case 'board_snapshot':
            this._lastBoards = JSON.stringify(msg.data);
            this._listeners.boardsUpdated.forEach(fn => fn(this._lastBoards));
            break;

        case 'booster_snapshot':
            this._lastBooster = JSON.stringify(msg.data);
            this._listeners.boosterUpdated.forEach(fn => fn(this._lastBooster));
            break;

        default:
            console.warn('Unknown daemon message type:', msg.type);
        }
    }

    // ── Helper: call cb now if init data is available, else queue ────────
    _withInit(cb) {
        if (this._initData) cb(this._initData);
        else this._pendingInit.push(cb);
    }

    // ── Build hvMonitor object (same API as QWebChannel hvMonitor) ──────
    _createHVMonitor() {
        const self = this;
        return {
            // Signals (same .connect() API)
            channelsUpdated: {
                connect(fn) { self._listeners.channelsUpdated.push(fn); }
            },
            boardsUpdated: {
                connect(fn) { self._listeners.boardsUpdated.push(fn); }
            },

            // Request-response (callback receives JSON string)
            readAll(cb) {
                // If we already have data, return it immediately
                if (self._lastHV !== '[]') { cb(self._lastHV); return; }
                // Otherwise wait for first snapshot
                const once = (json) => {
                    self._listeners.channelsUpdated =
                        self._listeners.channelsUpdated.filter(f => f !== once);
                    cb(json);
                };
                self._listeners.channelsUpdated.push(once);
            },

            readBoards(cb) {
                if (self._lastBoards !== '[]') { cb(self._lastBoards); return; }
                const once = (json) => {
                    self._listeners.boardsUpdated =
                        self._listeners.boardsUpdated.filter(f => f !== once);
                    cb(json);
                };
                self._listeners.boardsUpdated.push(once);
            },

            getModuleGeometry(cb) {
                self._withInit(init => cb(JSON.stringify(init.module_geometry || [])));
            },

            getGuiConfig(cb) {
                self._withInit(init => cb(JSON.stringify(init.gui_config || {})));
            },

            getDAQMap(cb) {
                self._withInit(init => cb(JSON.stringify(init.daq_map || [])));
            },

            // Fire-and-forget commands
            setChannelPower(crate, slot, ch, on) {
                self._send({type: 'set_power', crate, slot, ch, on});
            },
            setAllPower(on) {
                self._send({type: 'set_all_power', on});
            },
            setChannelVoltage(crate, slot, ch, value) {
                self._send({type: 'set_voltage', crate, slot, ch, value});
            },
            setChannelName(crate, slot, ch, name) {
                self._send({type: 'set_name', crate, slot, ch, name});
            },
            setChannelCurrent(crate, slot, ch, value) {
                self._send({type: 'set_current', crate, slot, ch, value});
            },
            setChannelSVMax(crate, slot, ch, value) {
                self._send({type: 'set_svmax', crate, slot, ch, value});
            },

            // No-op for compatibility (detached views are a Qt-only feature)
            openDetachedView(tabId) {
                console.log('openDetachedView not available in web mode:', tabId);
            },
        };
    }

    // ── Build boosterMonitor object ─────────────────────────────────────
    _createBoosterMonitor() {
        const self = this;
        return {
            boosterUpdated: {
                connect(fn) { self._listeners.boosterUpdated.push(fn); }
            },

            readAll(cb) {
                if (self._lastBooster !== '[]') { cb(self._lastBooster); return; }
                const once = (json) => {
                    self._listeners.boosterUpdated =
                        self._listeners.boosterUpdated.filter(f => f !== once);
                    cb(json);
                };
                self._listeners.boosterUpdated.push(once);
            },

            setOutput(idx, on) {
                self._send({type: 'booster_set_output', idx, on});
            },
            setVoltage(idx, value) {
                self._send({type: 'booster_set_voltage', idx, value});
            },
            setCurrent(idx, value) {
                self._send({type: 'booster_set_current', idx, value});
            },
        };
    }
}
