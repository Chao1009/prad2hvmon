// ═════════════════════════════════════════════════════════════════════
//  BOARD STATUS TAB
// ═════════════════════════════════════════════════════════════════════

function renderBoardTable() {
    const tbody = document.getElementById('board-body');
    const existingRows = new Map();
    for (const tr of tbody.rows) {
        const k = tr.dataset.key;
        if (k) existingRows.set(k, tr);
    }

    const fragment = document.createDocumentFragment();

    for (const bd of allBoards) {
        const key = bd.crate + '|' + bd.slot;
        const stAbbr   = bd.bdstatus ? bd.bdstatus.split('|')[0] : '';
        const stDetail = bd.bdstatus ? bd.bdstatus.split('|')[1] : '';
        const isOk     = stAbbr === 'OK';

        // Temperature coloring:
        //   red:   any board with temperature-related status flags (UNDRT, OVERT, TCAL)
        //   amber: outside 5–40 °C range
        //   green: 5–40 °C normal range
        const tempVal  = bd.temp;
        const tempTxt  = fmt(tempVal, 1);
        const hasTempError = stAbbr && (stAbbr.includes('UNDRT') || stAbbr.includes('OVERT') || stAbbr.includes('TCAL'));
        const tempCls  = hasTempError ? 'bd-temp-err'
                       : (tempVal == null || isNaN(tempVal)) ? 'bd-temp-ok'
                       : (tempVal >= 5 && tempVal <= 40) ? 'bd-temp-ok'
                       : 'bd-temp-warn';

        let tr = existingRows.get(key);
        if (!tr) {
            tr = document.createElement('tr');
            tr.dataset.key = key;

            const cells = [
                bd.crate,
                bd.slot,
                bd.model || '—',
                bd.nChan != null ? bd.nChan : '—',
                bd.serial != null ? bd.serial : '—',
                bd.firmware != null ? bd.firmware : '—',
            ];
            for (const txt of cells) {
                const td = document.createElement('td');
                td.textContent = txt;
                tr.appendChild(td);
            }
            // HVMax
            const tdHv = document.createElement('td');
            tdHv.style.textAlign = 'right';
            tdHv.textContent = fmt(bd.hvmax, 1);
            tr.appendChild(tdHv);

            // Temp
            const tdT = document.createElement('td');
            tdT.style.textAlign = 'right';
            tdT.className = tempCls;
            tdT.textContent = tempTxt;
            tr.appendChild(tdT);

            // Status
            const tdS = document.createElement('td');
            tdS.innerHTML = bdStatusHtml(stAbbr, stDetail, isOk);
            tr.appendChild(tdS);
        } else {
            existingRows.delete(key);

            // Patch dynamic fields: Temp (col 7) and Status (col 8)
            const tdT = tr.cells[7];
            if (tdT.textContent !== tempTxt) tdT.textContent = tempTxt;
            if (tdT.className !== tempCls) tdT.className = tempCls;

            const wantSt = bdStatusHtml(stAbbr, stDetail, isOk);
            if (tr.cells[8].innerHTML !== wantSt) tr.cells[8].innerHTML = wantSt;
        }
        fragment.appendChild(tr);
    }

    existingRows.forEach(tr => tr.remove());
    tbody.appendChild(fragment);
}

function bdStatusHtml(abbr, detail, isOk) {
    if (isOk) return '<span class="bd-ok">OK</span>';
    const tip = detail ? ` title="${detail}"` : '';
    return `<span class="bd-fault"${tip}>${abbr}</span>`;
}

