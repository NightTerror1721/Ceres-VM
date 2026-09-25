// Regenerates plan/v2/STATUS.md from the phase files, keeping the state and commits already recorded.
//
//   node plan/v2/tools/gen_status.js
//
// Run it after adding, removing or renaming tasks in a phase file. A task's state and commits survive as long
// as its ID does; a task that disappeared is listed at the end so nobody loses track of it.
const fs = require('fs');
const path = require('path');

const root = path.resolve(__dirname, '..');
const phasesDir = path.join(root, 'phases');
const statusPath = path.join(root, 'STATUS.md');

// What STATUS.md already says: ID -> { state, commits }.
const known = new Map();
if (fs.existsSync(statusPath)) {
    for (const line of fs.readFileSync(statusPath, 'utf8').replace(/\r\n/g, '\n').split('\n')) {
        const cells = line.split('|').map(c => c.trim());
        if (cells.length >= 7 && /^F\d+\.\d+$/.test(cells[1]))
            known.set(cells[1], { state: cells[4] || 'TODO', commits: cells[5] || '' });
    }
}

const depRe = /\*\*Depende de\*\*:\s*([^·\n]*)/;
const files = fs.readdirSync(phasesDir).filter(f => /^F\d\d-.*\.md$/.test(f)).sort();
const seen = new Set();

const lines = [];
lines.push('# Estado de las tareas');
lines.push('');
lines.push('Estados: `TODO`, `DOING`, `DONE`, `BLOCKED` (di por qué en «Notas» de la fase). Una tarea `HUMANO` necesita');
lines.push('al usuario. «Depende de» repite lo que dice el archivo de fase; si no coinciden, manda el archivo de fase.');
lines.push('Una tarea sin dependencias explícitas depende de la anterior de su fase; la primera, de lo que pide la fase.');
lines.push('Al cerrar una tarea, pon `DONE` y los commits como `repo@hash` (`asm@…` CeresASM, `cc@…` Ceres-C, `lib@…` STDLIB).');
lines.push('Tras añadir o renombrar tareas en una fase: `node plan/v2/tools/gen_status.js` (conserva estados y commits).');
lines.push('');

for (const file of files) {
    const text = fs.readFileSync(path.join(phasesDir, file), 'utf8').replace(/\r\n/g, '\n');
    const title = text.split('\n')[0].replace(/^#\s*/, '');
    const hdr = text.match(depRe);
    const phaseDeps = hdr && hdr[1].trim() !== '—' ? 'fase: ' + hdr[1].trim() : '—';
    lines.push(`## ${title}`);
    lines.push('');
    lines.push(`Archivo: [phases/${file}](phases/${file})`);
    lines.push('');
    lines.push('| ID | Tarea | Depende de | Estado | Commits |');
    lines.push('| --- | --- | --- | --- | --- |');
    let prev = null;
    const blocks = text.split(/\n(?=### )/).filter(b => b.startsWith('### '));
    for (const block of blocks) {
        const head = block.split('\n')[0].replace(/^###\s*/, '');
        const m = head.match(/^(F\d+\.\d+)\s*·\s*(.*)$/);
        if (!m) continue;
        const id = m[1];
        const human = /`HUMANO`/.test(head);
        const tail = head.match(/·\s*depende de (.*)$/i);
        const name = m[2]
            .replace(/`HUMANO`/g, '')
            .replace(/·\s*requiere\s+P\d+/g, '')
            .replace(/·\s*depende de .*$/i, '')
            .replace(/[\s·]+$/, '')
            .trim();
        const dep = block.match(depRe);
        let deps = dep ? dep[1].trim() : (tail ? tail[1].trim() : '');
        if (!deps) deps = prev ? prev : phaseDeps;
        if (deps === '—') deps = phaseDeps;
        prev = id;
        const was = known.get(id) || { state: 'TODO', commits: '' };
        seen.add(id);
        lines.push(`| ${id} | ${name}${human ? ' · **HUMANO**' : ''} | ${deps} | ${was.state} | ${was.commits} |`);
    }
    lines.push('');
}

const gone = [...known.keys()].filter(id => !seen.has(id));
if (gone.length) {
    lines.push('## Tareas que ya no están en ninguna fase');
    lines.push('');
    lines.push('| ID | Estado | Commits |');
    lines.push('| --- | --- | --- |');
    for (const id of gone) lines.push(`| ${id} | ${known.get(id).state} | ${known.get(id).commits} |`);
    lines.push('');
}

fs.writeFileSync(statusPath, lines.join('\n'));
console.log(`STATUS.md: ${seen.size} tasks${gone.length ? `, ${gone.length} no longer in any phase` : ''}`);
