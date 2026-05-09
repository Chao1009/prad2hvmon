// ─────────────────────────────────────────────────────────────────────────────
// report.js — Auto-report capture for the PRad-II HV Monitor.
//
// Port of prad2evviewer/resources/report.js, adapted for HV (no Plotly, no
// run number, UTC-timestamped title).  No external dependency: the
// screenshot pipeline clones the active tab's panel through an SVG
// <foreignObject>, swaps every visible <canvas> with its toDataURL, inlines
// stylesheets, and renders to PNG via Image+Canvas.
//
// On a server-sent {type:"capture_request", request_id, reason}:
//   1. walk REPORT_TABS — switch each tab into view, snap a PNG, switch back
//   2. build markdown body with summary lines
//   3. wrap in elog XML (buildElogXml)
//   4. POST {xml, auto:true, request_id} to /api/elog/post
// ─────────────────────────────────────────────────────────────────────────────

// =========================================================================
// Registry + state
// =========================================================================
const reportRegistry  = [];
let   elogConfig      = {url:'', logbook:'', author:'', tags:[]};
let   autoReportCfg   = {enabled:false, post_to_elog:false};
let   reportAttachments = [];

function registerReportSection(s) {
    reportRegistry.push(s);
    reportRegistry.sort((a,b)=>a.order-b.order);
}

function addAttachment(dataUrl, filename, caption) {
    if (!dataUrl) return;
    const b64 = dataUrl.split(',')[1];
    if (b64) reportAttachments.push({data:b64, filename, caption, type:'image/png'});
}

// =========================================================================
// Tabs to capture
// =========================================================================
// Order matches the operator's eyeline.  The render-side trigger
// (boosterDirty / boardDirty) is set in _activateTab so the cloned DOM
// reflects the latest snapshot, not the cached one from the previously-
// active tab.
const REPORT_TABS = [
    {tab:'table-tab',    title:'Channel Table',  filename:'tab_table.png'},
    {tab:'board-tab',    title:'Board Status',   filename:'tab_board.png'},
    {tab:'geo-tab',      title:'HyCal Geometry', filename:'tab_geo.png'},
    {tab:'booster-tab',  title:'Booster HV',     filename:'tab_booster.png'},
    {tab:'faultlog-tab', title:'Fault Log',      filename:'tab_faultlog.png'},
    {tab:'hvmon-tab',    title:'HV Monitor',     filename:'tab_hvmon.png'},
];

// Post-switch settling.  HV rendering is much lighter than the eviewer
// case (no Plotly, no async fetches inside switchTab) so 400 ms is plenty.
const TAB_SETTLE_MS = 400;

function _wait(ms){ return new Promise(r=>setTimeout(r,ms)); }

// Mirror initTabs() behavior so the captured DOM reflects exactly what the
// user would see if they clicked the tab — including geometry resize and
// hvmon init hooks.
function _activateTab(tabId){
    document.querySelectorAll('.tab-btn').forEach(b=>b.classList.remove('active'));
    document.querySelectorAll('.tab-content').forEach(c=>c.classList.remove('active'));
    const btn = document.querySelector('.tab-btn[data-tab="'+tabId+'"]');
    if (btn) btn.classList.add('active');
    const content = document.getElementById(tabId);
    if (content) content.classList.add('active');
    // Force-render dirty tabs (matches initTabs click handler).
    if (typeof boosterDirty !== 'undefined' && tabId==='booster-tab') boosterDirty = true;
    if (typeof boardDirty   !== 'undefined' && tabId==='board-tab')   boardDirty   = true;
    if (typeof renderActiveTab === 'function') renderActiveTab();
    if (tabId==='geo-tab' && typeof resizeGeoCanvas === 'function') resizeGeoCanvas();
    if (tabId==='hvmon-tab' && typeof initHVMonGeo === 'function') initHVMonGeo();
}

function _activeTabId(){
    return document.querySelector('.tab-content.active')?.id || null;
}

// =========================================================================
// Whole-tab screenshot
// =========================================================================
function _gatherCss(){
    const out = [];
    for (const sh of document.styleSheets) {
        try { for (const r of sh.cssRules) out.push(r.cssText); }
        catch (e) { /* CORS-protected sheet — skip */ }
    }
    return out.join('\n');
}

// XMLSerializer copies attributes, not properties — so live <input>/<select>
// state is invisible until we mirror it back into attributes.
function _freezeFormState(root){
    for (const inp of root.querySelectorAll('input')) {
        if (inp.type==='checkbox' || inp.type==='radio') {
            if (inp.checked) inp.setAttribute('checked', '');
            else             inp.removeAttribute('checked');
        } else {
            inp.setAttribute('value', inp.value);
        }
    }
    for (const sel of root.querySelectorAll('select')) {
        for (const opt of sel.options) {
            if (opt.selected) opt.setAttribute('selected', '');
            else              opt.removeAttribute('selected');
        }
    }
}

function _pathFromRoot(el, root){
    const path = [];
    let n = el;
    while (n && n !== root) {
        const p = n.parentNode;
        if (!p) return null;
        path.unshift([...p.children].indexOf(n));
        n = p;
    }
    return n === root ? path : null;
}

function _resolveByPath(root, path){
    let n = root;
    for (const i of path) {
        if (!n || !n.children || i < 0 || i >= n.children.length) return null;
        n = n.children[i];
    }
    return n;
}

async function captureTabScreenshot(tabId){
    const prevTab = _activeTabId();
    if (tabId !== prevTab) {
        _activateTab(tabId);
        await _wait(TAB_SETTLE_MS);
    }

    let dataUrl = null;
    let svgUrl  = null;
    try {
        const root = document.getElementById(tabId);
        if (!root) return null;
        const rect = root.getBoundingClientRect();
        const W = Math.max(800, Math.ceil(rect.width));
        const H = Math.max(500, Math.ceil(rect.height));

        // 1) Snapshot every visible <canvas> inside the tab.
        const replacements = [];
        const seen = new Set();
        for (const c of root.querySelectorAll('canvas')) {
            if (seen.has(c)) continue; seen.add(c);
            const r = c.getBoundingClientRect();
            if (r.width === 0 || r.height === 0) continue;
            const path = _pathFromRoot(c, root);
            if (!path) continue;
            try {
                replacements.push({path, src:c.toDataURL('image/png'),
                                   w:r.width, h:r.height});
            } catch (e) { /* tainted canvas — skip */ }
        }

        // 2) Clone, freeze form state, swap canvases for <img>.
        const clone = root.cloneNode(true);
        _freezeFormState(clone);
        clone.style.width  = W + 'px';
        clone.style.height = H + 'px';
        const styles = getComputedStyle(document.body);
        const bg = styles.backgroundColor || '#1a1a1a';
        const fg = styles.color           || '#eee';
        clone.style.background = bg;
        for (const rep of replacements) {
            const twin = _resolveByPath(clone, rep.path);
            if (!twin || !twin.parentNode) continue;
            const img = document.createElement('img');
            img.setAttribute('src', rep.src);
            const w = Math.round(rep.w), h = Math.round(rep.h);
            img.setAttribute('width',  String(w));
            img.setAttribute('height', String(h));
            img.setAttribute('style',
                `display:block;width:${w}px;height:${h}px;border:none;margin:0;padding:0`);
            twin.parentNode.replaceChild(img, twin);
        }

        // 3) Inline stylesheets + minimal overrides so the cloned panel
        //    renders with the same theme it had on screen, and transient
        //    overlays (loading, tooltip, popups) are hidden.
        const cssText = _gatherCss();
        const overrides =
            `body{background:${bg}!important;}` +
            `.tab-content{display:block!important;}` +
            `#geo-tooltip,#loading,.popup,.modal,.backdrop{display:none!important;}`;

        const xhtml = new XMLSerializer().serializeToString(clone);
        const svg =
            `<svg xmlns="http://www.w3.org/2000/svg" width="${W}" height="${H}">` +
              `<foreignObject x="0" y="0" width="${W}" height="${H}">` +
                `<div xmlns="http://www.w3.org/1999/xhtml" ` +
                     `style="width:${W}px;height:${H}px;background:${bg};color:${fg};` +
                     `font-family:'Consolas','SF Mono',monospace;">` +
                  `<style>${cssText}\n${overrides}</style>` +
                  xhtml +
                `</div>` +
              `</foreignObject>` +
            `</svg>`;

        // 4) SVG → Blob URL → <img> → canvas → PNG dataURL.
        const blob = new Blob([svg], {type:'image/svg+xml'});
        svgUrl = URL.createObjectURL(blob);
        await new Promise((resolve, reject) => {
            const img = new Image();
            img.onload = () => {
                const out = document.createElement('canvas');
                out.width = W; out.height = H;
                const ctx = out.getContext('2d');
                ctx.fillStyle = bg;
                ctx.fillRect(0, 0, W, H);
                ctx.drawImage(img, 0, 0, W, H);
                try { dataUrl = out.toDataURL('image/png'); resolve(); }
                catch (e) { reject(e); }
            };
            img.onerror = () => reject(new Error('SVG image load failed'));
            img.src = svgUrl;
        });
    } catch (e) {
        console.error('captureTabScreenshot failed for', tabId, e);
    } finally {
        if (svgUrl) URL.revokeObjectURL(svgUrl);
        if (prevTab && prevTab !== tabId) {
            _activateTab(prevTab);
        }
    }
    return dataUrl;
}

// =========================================================================
// Markdown helpers
// =========================================================================
function mdTable(headers, rows, alignments){
    const aligns = alignments || headers.map(()=>'l');
    const sepMap = {l:':---', r:'---:', c:':---:'};
    let md = '| ' + headers.join(' | ') + ' |\n';
    md += '| ' + aligns.map(a=>sepMap[a]||':---').join(' | ') + ' |\n';
    for (const row of rows) md += '| ' + row.join(' | ') + ' |\n';
    return md + '\n';
}

// =========================================================================
// Per-tab text summaries
// =========================================================================
function _summaryTable(){
    if (!Array.isArray(allChannels) || !allChannels.length) return '';
    const total  = allChannels.length;
    const on     = allChannels.filter(ch=>ch.on).length;
    const off    = total - on;
    const fault  = allChannels.filter(ch=>ch.level === 'fault').length;
    const warn   = allChannels.filter(ch=>ch.level === 'warn').length;
    let s = `Channels: ${total} | ON: ${on} | OFF: ${off}`;
    s += ` | Faults: ${fault} | Warnings: ${warn}\n\n`;
    return s;
}

function _summaryBoard(){
    if (!Array.isArray(allBoards) || !allBoards.length) return '';
    return `Boards: ${allBoards.length}\n\n`;
}

function _summaryGeo(){
    return '';  // visual-only
}

function _summaryBooster(){
    if (!Array.isArray(boosterSupplies) || !boosterSupplies.length) return '';
    const on = boosterSupplies.filter(b => b.on).length;
    return `Booster supplies: ${boosterSupplies.length} | ON: ${on}\n\n`;
}

function _summaryFaultLog(){
    // flFaults / flWarns are exposed as window-scope globals by
    // monitor_faultlog.js (top-of-file `let`).  Treat absence as zero
    // so the report still posts cleanly even if the fault module is
    // late to register.
    const faults = (typeof flFaults !== 'undefined' && Array.isArray(flFaults)) ? flFaults.length : 0;
    const warns  = (typeof flWarns  !== 'undefined' && Array.isArray(flWarns))  ? flWarns.length  : 0;
    if (!faults && !warns) return '';
    return `In-buffer fault entries: ${faults} | warning entries: ${warns}\n\n`;
}

function _summaryHvMon(){
    const n = (typeof hvmonModules === 'object' && hvmonModules)
                ? Object.keys(hvmonModules).length : 0;
    if (!n) return '';
    return `HV monitor channels tracked: ${n}\n\n`;
}

const _SECTION_SUMMARIES = {
    'table-tab':    _summaryTable,
    'board-tab':    _summaryBoard,
    'geo-tab':      _summaryGeo,
    'booster-tab':  _summaryBooster,
    'faultlog-tab': _summaryFaultLog,
    'hvmon-tab':    _summaryHvMon,
};

// Register the per-tab sections in display order.
REPORT_TABS.forEach((t, i) => {
    registerReportSection({
        id: t.tab, title: t.title, order: i + 1,
        generate: async () => {
            const summary = _SECTION_SUMMARIES[t.tab];
            const summaryMd = summary ? (summary() || '') : '';
            const img = await captureTabScreenshot(t.tab);
            if (img) addAttachment(img, t.filename, t.title);
            let md = `## ${t.title}\n\n`;
            if (summaryMd) md += summaryMd;
            if (img) md += `![${t.title}](${t.filename})\n\n`;
            return md;
        }
    });
});

// =========================================================================
// Report assembly
// =========================================================================
async function generateReport(reportBy){
    const sb = document.getElementById('auto-status-msg');
    if (sb) sb.textContent = 'Generating report…';
    try {
        reportAttachments = [];
        const tsLocal = new Date().toLocaleString();
        const tsUTC   = utcStampDisplay();
        const channelCount = (Array.isArray(allChannels) ? allChannels.length : 0);
        let header = `# PRad-II HV Monitor Report\n\n`;
        header += `- **Generated:** ${tsLocal} (${tsUTC} UTC)\n`;
        header += `- **Channels:** ${channelCount}\n`;
        if (reportBy) header += `- **Report by:** ${reportBy}\n`;
        let sectionsMd = '';
        for (const entry of reportRegistry) {
            try {
                const section = await entry.generate();
                if (section) sectionsMd += section;
            } catch (err) {
                sectionsMd += `## ${entry.title}\n\n*Error: ${err.message}*\n\n`;
            }
        }
        let md = header + `\n---\n\n` + sectionsMd;
        md += `---\n*PRad-II HV Online Monitor — Report generated ${tsLocal}*\n`;
        return {md, attachments: reportAttachments};
    } catch (err) {
        console.error('Report generation error:', err);
        return null;
    }
}

// =========================================================================
// Elog XML helpers
// =========================================================================
function escXml(s){
    return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;')
                    .replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}

function buildElogXml(title, logbook, author, tags, body, attachments){
    const parts = ['<?xml version="1.0" encoding="UTF-8"?>', '<Logentry>',
        `  <created>${new Date().toISOString()}</created>`,
        `  <Author><username>${escXml(author)}</username></Author>`,
        `  <title>${escXml(title)}</title>`,
        `  <body type="text"><![CDATA[${body}]]></body>`,
        '  <Logbooks>'];
    for (const lb of logbook.split(','))
        parts.push(`    <logbook>${escXml(lb.trim())}</logbook>`);
    parts.push('  </Logbooks>');
    if (tags && tags.length) {
        parts.push('  <Tags>');
        for (const t of tags) parts.push(`    <tag>${escXml(t.trim())}</tag>`);
        parts.push('  </Tags>');
    }
    if (attachments && attachments.length) {
        parts.push('  <Attachments>');
        for (const a of attachments)
            parts.push('    <Attachment>',
                `      <caption>${escXml(a.caption)}</caption>`,
                `      <filename>${escXml(a.filename)}</filename>`,
                `      <type>${escXml(a.type)}</type>`,
                `      <data encoding="base64">${a.data}</data>`,
                '    </Attachment>');
        parts.push('  </Attachments>');
    }
    parts.push('</Logentry>');
    return parts.join('\n');
}

// =========================================================================
// Title — `[YYYY-MM-DD HH:MM UTC] PRad-II HV Monitor Auto Report`
// =========================================================================
// Minute-grain timestamps keep titles unique across consecutive scheduled
// captures while still letting post_elog.py use exact-title dedup against
// the live elog.
function utcStampDisplay(){
    const d = new Date();
    const pad = n => String(n).padStart(2, '0');
    return `${d.getUTCFullYear()}-${pad(d.getUTCMonth()+1)}-${pad(d.getUTCDate())} ` +
           `${pad(d.getUTCHours())}:${pad(d.getUTCMinutes())}`;
}

function autoReportTitle(){
    return `[${utcStampDisplay()} UTC] PRad-II HV Monitor Auto Report`;
}

// =========================================================================
// Status pill
// =========================================================================
let autoIsReporting = false;

function autoStatusEl(){ return document.getElementById('auto-status'); }

function autoUpdateStatus(){
    const el = autoStatusEl(); if (!el) return;
    if (autoIsReporting) {
        el.classList.add('reporting');
        el.classList.remove('on','off');
        el.textContent = 'Auto-reporting…';
        el.title = 'This browser is capturing + uploading the auto-report';
        return;
    }
    el.classList.remove('reporting');
    if (autoReportCfg && autoReportCfg.enabled) {
        el.classList.add('on'); el.classList.remove('off');
        el.textContent = 'Auto: ON';
        el.title = 'Auto-report ON — daemon fires on its scheduled cadence';
    } else {
        el.classList.add('off'); el.classList.remove('on');
        el.textContent = 'Auto: OFF';
        el.title = 'Auto-report disabled in gui_config.json';
    }
}

function autoSetReporting(on){
    autoIsReporting = !!on;
    autoUpdateStatus();
}

// =========================================================================
// Capture-request handler — invoked from ws_client.js when the daemon
// picks us as the on-demand reporter.  Only the chosen client receives
// this message; the rest ignore it.
// =========================================================================
function elogAvailable(){ return !!(elogConfig && elogConfig.url); }

async function handleCaptureRequest(msg){
    const reason    = (msg && msg.reason)     || 'auto';
    const requestId = (msg && msg.request_id) || '';
    const fullTitle = autoReportTitle();
    const sb = document.getElementById('auto-status-msg');

    if (!elogAvailable()) {
        if (sb) sb.textContent = 'Auto-report skipped: elog not configured';
        return;
    }
    autoSetReporting(true);
    if (sb) sb.textContent = `Auto-report (${reason}): capturing…`;

    try {
        const reportBy = elogConfig.author || 'auto';
        const report   = await generateReport(reportBy);
        if (!report) {
            if (sb) sb.textContent = 'Auto-report failed: report generation';
            return;
        }
        // The body in elog mirrors the generated markdown but strips
        // image links — the elog renders attachments separately, and a
        // dangling `![alt](file.png)` reads as garbage in the body view.
        const tags = (elogConfig.tags || []).slice();
        const body = `*Auto-posted: ${reason}*\n\n` +
                     report.md.replace(/!\[[^\]]*\]\([^)]+\)\n*/g, '');
        const xml  = buildElogXml(fullTitle, elogConfig.logbook || '',
                                  reportBy, tags, body, report.attachments);

        if (sb) sb.textContent = 'Auto-report uploading…';
        const resp = await fetch('/api/elog/post', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({xml, auto:true, request_id: requestId}),
        });
        const r = await resp.json();
        if (r.ok && r.dry_run) {
            if (sb) sb.textContent =
                `Auto-report saved (dry-run): ${r.saved_xml || r.saved_dir || fullTitle}`;
        } else if (r.ok && r.skipped) {
            if (sb) sb.textContent = `Auto-report skipped: ${r.detail || fullTitle}`;
        } else if (r.ok) {
            if (sb) sb.textContent = `Auto-report posted: ${fullTitle}`;
        } else {
            if (sb) sb.textContent =
                `Auto-report failed: ${r.error || ('HTTP '+(r.status||'?'))}`;
        }
    } catch (e) {
        if (sb) sb.textContent = `Auto-report error: ${e.message}`;
    } finally {
        // The daemon's auto_capture_done broadcast (handled below) is the
        // authoritative pill-clear signal; we reset locally too in case
        // that broadcast races the response.
        autoSetReporting(false);
    }
}

// Daemon's authoritative end-of-flow notice: every connected tab clears
// its reporter badge and the chosen reporter sees a one-line outcome
// in the status footer.
function handleAutoCaptureDone(msg){
    autoSetReporting(false);
    const sb = document.getElementById('auto-status-msg');
    if (!sb) return;
    if (msg.dry_run)        sb.textContent = `Auto report saved (dry-run)`;
    else if (msg.posted)    sb.textContent = `Auto report posted`;
    else                    sb.textContent = `Auto report saved locally`;
}

// =========================================================================
// Init — called from monitor.js once the daemon's init payload arrives.
// =========================================================================
function initReport(initData){
    const ar = initData && initData.auto_report;
    if (ar) {
        autoReportCfg = ar;
        if (ar.elog && ar.elog.url) elogConfig = ar.elog;
    }
    autoUpdateStatus();
}
