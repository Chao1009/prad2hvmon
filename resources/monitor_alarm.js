// ═════════════════════════════════════════════════════════════════════
//  Audible fault alarm (Web Audio API)
// ═════════════════════════════════════════════════════════════════════
// Produces a repeating two-tone "beep-beep" pattern while any HV
// channel reports a hardware fault or any booster supply has an error.
// The user can silence the alarm via the 🔇 button; it stays muted
// until all faults clear, then re-arms automatically.

let _userHasInteracted = false;
// Track the very first user gesture so we know when AudioContext creation is allowed.
function _markInteracted() {
    _userHasInteracted = true;
    document.removeEventListener('click',   _markInteracted);
    document.removeEventListener('keydown', _markInteracted);
    document.removeEventListener('pointerdown', _markInteracted);
    // If an alarm was waiting for a gesture, beep now
    if (alarmActive && !alarmMuted) playAlarmBeep();
}
document.addEventListener('click',   _markInteracted, { once: false });
document.addEventListener('keydown', _markInteracted, { once: false });
document.addEventListener('pointerdown', _markInteracted, { once: false });

function ensureAlarmCtx() {
    if (alarmCtx) return true;
    // Only create the AudioContext if we know a user gesture has occurred
    if (!_userHasInteracted) return false;
    alarmCtx = new (window.AudioContext || window.webkitAudioContext)();
    // Belt-and-suspenders: resume in case the browser still considers it suspended
    if (alarmCtx.state === 'suspended') alarmCtx.resume();
    return true;
}

// Play a single two-tone beep.  Called once per poll cycle while faults exist.
// Each call creates a short-lived oscillator that stops itself after ~350 ms,
// so there is no continuous tone and no cleanup needed between polls.
function playAlarmBeep() {
    if (!ensureAlarmCtx()) return;     // no user gesture yet

    const t = alarmCtx.currentTime;

    const gain = alarmCtx.createGain();
    gain.gain.setValueAtTime(0, t);
    gain.connect(alarmCtx.destination);

    const osc = alarmCtx.createOscillator();
    osc.type = 'square';
    osc.frequency.setValueAtTime(880, t);
    osc.connect(gain);

    // beep 1: 880 Hz, 120 ms
    gain.gain.setValueAtTime(0.25, t + 0.01);
    gain.gain.setValueAtTime(0, t + 0.13);

    // beep 2: 660 Hz, 120 ms (after 80 ms silence)
    osc.frequency.setValueAtTime(660, t + 0.21);
    gain.gain.setValueAtTime(0.25, t + 0.21);
    gain.gain.setValueAtTime(0, t + 0.33);

    osc.start(t);
    osc.stop(t + 0.4);   // auto-cleanup after the beep finishes

    // Disconnect nodes after playback to free resources
    osc.onended = () => { osc.disconnect(); gain.disconnect(); };
}

// Legacy helpers — stopAlarmTone is now a no-op since beeps are self-contained
function stopAlarmTone() { /* beeps stop themselves */ }

// Called after every poll to evaluate alarm state
function evaluateAlarm() {
    const chFaults = allChannels.filter(c => classifyChannel(c).isFault).length;
    const bdFaults = allBoards.filter(bd => {
        const abbr = bd.bdstatus ? bd.bdstatus.split('|')[0] : '';
        return abbr !== 'OK' && abbr !== '';
    }).length;
    const bstFaults = boosterSupplies.filter(s => !s.connected && s.error).length;

    const hasFaults = chFaults > 0 || bdFaults > 0 || bstFaults > 0;

    const wasActive = alarmActive;
    alarmActive = hasFaults;

    if (hasFaults && !wasActive) {
        // Faults just appeared — un-mute so the user hears the first beep
        alarmMuted = false;
    } else if (!hasFaults && wasActive) {
        // All faults cleared — re-arm
        alarmMuted = false;
    }

    // Play a single beep this poll cycle if faults exist and not muted
    if (alarmActive && !alarmMuted) {
        playAlarmBeep();
    }

    // Store fault counts for button text and tab dots
    window._faultCounts = { ch: chFaults, bd: bdFaults, bst: bstFaults };

    updateMuteButton();
    updateTabFaultDots();
}

function toggleMute() {
    alarmMuted = !alarmMuted;
    // When un-muting, play one beep as confirmation if faults are active
    if (!alarmMuted && alarmActive) playAlarmBeep();
    updateMuteButton();
}

function updateMuteButton() {
    const btn = document.getElementById('btn-mute');
    if (!btn) return;
    const fc = window._faultCounts || { ch: 0, bd: 0, bst: 0 };
    if (!alarmActive) {
        btn.classList.remove('alarming', 'muted');
        btn.textContent = '🔔 Alarm';
        btn.title = 'No active faults';
    } else if (alarmMuted) {
        btn.classList.remove('alarming');
        btn.classList.add('muted');
        btn.textContent = '🔇 Muted';
        btn.title = 'Alarm silenced — click to unmute';
    } else {
        btn.classList.add('alarming');
        btn.classList.remove('muted');
        // Build specific fault label
        const parts = [];
        if (fc.ch  > 0) parts.push('Ch Fault');
        if (fc.bd  > 0) parts.push('Bd Fault');
        if (fc.bst > 0) parts.push('Booster Err');
        btn.textContent = '⚠ ' + (parts.length ? parts.join(' | ') : 'Fault');
        btn.title = 'Faults detected — click to silence';
    }
}

// Add/remove red fault dot on tab buttons
function updateTabFaultDots() {
    const fc = window._faultCounts || { ch: 0, bd: 0, bst: 0 };
    const setDot = (tabId, hasFault) => {
        const btn = document.querySelector(`.tab-btn[data-tab="${tabId}"]`);
        if (btn) btn.classList.toggle('has-fault', hasFault);
    };
    setDot('table-tab',   fc.ch  > 0);
    setDot('board-tab',   fc.bd  > 0);
    setDot('booster-tab', fc.bst > 0);
}

