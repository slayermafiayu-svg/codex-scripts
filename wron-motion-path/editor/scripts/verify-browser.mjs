// Drives the path editor in headless Chromium with real pointer/keyboard
// input against the MemoryHostAdapter (mock host — NOT VEGAS).
// Usage: node scripts/verify-browser.mjs [--screenshots <dir>] [--port 8765]
import { createRequire } from 'node:module';
import { execSync, spawn } from 'node:child_process';
import { mkdirSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

let playwright;
try {
  playwright = await import('playwright');
} catch {
  const req = createRequire(`${execSync('npm root -g').toString().trim()}/`);
  playwright = req('playwright');
}
const { chromium } = playwright;

const args = process.argv.slice(2);
const shotDir = args.includes('--screenshots') ? resolve(args[args.indexOf('--screenshots') + 1]) : null;
const port = args.includes('--port') ? Number(args[args.indexOf('--port') + 1]) : 8765;
const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
if (shotDir) mkdirSync(shotDir, { recursive: true });

const server = spawn('python3', ['-m', 'http.server', String(port), '--bind', '127.0.0.1', '--directory', root], { stdio: 'ignore' });
await new Promise((r) => setTimeout(r, 800));

const results = [];
const check = (name, ok, detail = '') => {
  results.push({ name, ok: !!ok, detail });
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? `  (${detail})` : ''}`);
};

const browser = await chromium.launch();
const base = `http://127.0.0.1:${port}/editor/demo/index.html`;

async function open(viewport = { width: 1200, height: 800 }, query = '') {
  const page = await browser.newPage({ viewport });
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()); });
  await page.goto(base + query);
  await page.waitForFunction(() => window.wmpEditor?.ready);
  await page.evaluate(() => window.wmpEditor.ready);
  await page.waitForTimeout(150);
  page.errors = errors;
  return page;
}

const screen = (page, n) =>
  page.evaluate(({ x, y }) => {
    const ed = window.wmpEditor;
    const r = ed.canvas.getBoundingClientRect();
    const s = ed.toScreen({ x, y });
    return { x: r.left + s.x, y: r.top + s.y };
  }, n);
const doc = (page) => page.evaluate(() => window.wmpEditor.getDocument());
const writes = (page) => page.evaluate(() => window.wmpHost.log.filter((l) => l.kind === 'path').length);
const pathLength = (page) =>
  page.evaluate(async () => {
    const { geometry } = await import('/editor/src/index.js');
    const ed = window.wmpEditor;
    return new geometry.PathGeometry(ed.doc.path, { aspect: ed.aspect, tolerance: 1e-10 }).length;
  });
const shot = async (page, name) => { if (shotDir) await page.screenshot({ path: join(shotDir, name) }); };

try {
  // ---------------------------------------------------------------- basics
  let page = await open();
  check('loads without console errors', page.errors.length === 0, page.errors.join('; '));
  let d = await doc(page);
  check('initial document has the S-curve (3 points)', d.path.points.length === 3);
  await shot(page, '01-editor-wide.png');

  // Select + drag an anchor: nothing written during the drag, exactly one write after.
  const p1 = d.path.points[1].p;
  let s = await screen(page, { x: p1[0], y: p1[1] });
  const w0 = await writes(page);
  await page.mouse.move(s.x, s.y);
  await page.mouse.down();
  for (let i = 1; i <= 8; i += 1) await page.mouse.move(s.x + i * 6, s.y - i * 3);
  const wDuring = await writes(page);
  await page.mouse.up();
  await page.waitForTimeout(50);
  const wAfter = await writes(page);
  d = await doc(page);
  check('anchor drag writes nothing while moving', wDuring === w0, `${wDuring - w0} writes during drag`);
  check('anchor drag commits exactly one write', wAfter === w0 + 1, `${wAfter - w0}`);
  const moved = d.path.points[1].p;
  const expect = await page.evaluate(({ x, y }) => { const ed = window.wmpEditor; return { dx: 48 / (ed.aspect * ed.view.s), dy: -24 / ed.view.s }; }, {});
  check('anchor moved by the pointer delta', Math.abs(moved[0] - p1[0] - expect.dx) < 1e-3 && Math.abs(moved[1] - p1[1] - expect.dy) < 1e-3);
  check('selection shows the dragged point', JSON.stringify(await page.evaluate(() => window.wmpEditor.getSelection())) === '[1]');

  // Undo / redo by keyboard (editor focused after the click).
  await page.keyboard.press('Control+z');
  d = await doc(page);
  check('Ctrl+Z restores the point', Math.abs(d.path.points[1].p[0] - p1[0]) < 1e-12);
  await page.keyboard.press('Control+y');
  d = await doc(page);
  check('Ctrl+Y re-applies the move', Math.abs(d.path.points[1].p[0] - moved[0]) < 1e-12);

  // Numeric X with a decimal comma; invalid text is rejected, not coerced.
  await page.fill('[data-field="point-x"]', '0,3');
  await page.press('[data-field="point-x"]', 'Enter');
  d = await doc(page);
  check('numeric X accepts "0,3"', Math.abs(d.path.points[1].p[0] - 0.3) < 1e-12);
  await page.fill('[data-field="point-x"]', 'abc');
  await page.press('[data-field="point-x"]', 'Enter');
  const invalid = await page.getAttribute('[data-field="point-x"]', 'aria-invalid');
  d = await doc(page);
  check('invalid number is flagged and ignored', invalid === 'true' && Math.abs(d.path.points[1].p[0] - 0.3) < 1e-12);

  // Double-click on the curve: one more point, identical shape.
  const lenBefore = await pathLength(page);
  const mid = await page.evaluate(async () => {
    const { geometry } = await import('/editor/src/index.js');
    const ed = window.wmpEditor;
    const g = new geometry.PathGeometry(ed.doc.path, { aspect: ed.aspect });
    return g.toNormalized(g.sampleAtDistance(g.length * 0.25, 1, false).pos);
  });
  s = await screen(page, mid);
  await page.mouse.dblclick(s.x, s.y);
  d = await doc(page);
  const lenAfter = await pathLength(page);
  check('double-click on curve inserts a point', d.path.points.length === 4);
  check('inserted point keeps the curve shape (length equal to 1e-9)', Math.abs(lenAfter - lenBefore) < 1e-9, `${lenBefore} vs ${lenAfter}`);

  // Add tool: click empty space appends; delete tool removes.
  await page.click('[data-action="tool-add"]');
  s = await screen(page, { x: 0.9, y: 0.85 });
  await page.mouse.click(s.x, s.y);
  d = await doc(page);
  check('add tool appends a point on empty space', d.path.points.length === 5 && Math.abs(d.path.points[4].p[0] - 0.9) < 1e-3);
  await page.click('[data-action="tool-delete"]');
  s = await screen(page, { x: d.path.points[4].p[0], y: d.path.points[4].p[1] });
  await page.mouse.click(s.x, s.y);
  d = await doc(page);
  check('delete tool removes the clicked point', d.path.points.length === 4);
  await page.click('[data-action="tool-select"]');

  // Point type shortcuts and handle rules.
  s = await screen(page, { x: d.path.points[2].p[0], y: d.path.points[2].p[1] });
  await page.mouse.click(s.x, s.y);
  await page.keyboard.press('1');
  d = await doc(page);
  check('"1" makes the selected point a corner (handles retracted)', d.path.points[2].mode === 'corner' && d.path.points[2].in.every((v) => v === 0));
  await page.keyboard.press('2');
  d = await doc(page);
  const pin = d.path.points[2].in, pout = d.path.points[2].out;
  const asp = await page.evaluate(() => window.wmpEditor.aspect);
  check('"2" makes it smooth with collinear handles', d.path.points[2].mode === 'smooth' && Math.abs(pin[0] * asp * pout[1] - pin[1] * pout[0] * asp) < 1e-9);
  // Alt-drag an out handle -> independent handles.
  const hpos = await screen(page, { x: d.path.points[2].p[0] + pout[0], y: d.path.points[2].p[1] + pout[1] });
  await page.keyboard.down('Alt');
  await page.mouse.move(hpos.x, hpos.y);
  await page.mouse.down();
  await page.mouse.move(hpos.x + 20, hpos.y + 25, { steps: 4 });
  await page.mouse.up();
  await page.keyboard.up('Alt');
  d = await doc(page);
  check('Alt-dragging a handle breaks it (free) and keeps the other handle', d.path.points[2].mode === 'free' && d.path.points[2].in[0] === pin[0]);

  // Keyboard isolation: keys typed elsewhere never reach the editor; Space is never swallowed.
  const countBefore = (await doc(page)).path.points.length;
  await page.evaluate(() => { const i = document.createElement('input'); i.id = 'outside'; document.body.append(i); });
  await page.focus('#outside');
  await page.keyboard.press('Delete');
  await page.keyboard.press('Control+z');
  check('Delete / Ctrl+Z outside the editor do not edit the path', (await doc(page)).path.points.length === countBefore);
  const spacePrevented = await page.evaluate(() => {
    const ed = window.wmpEditor;
    ed.root.focus();
    const ev = new KeyboardEvent('keydown', { key: ' ', bubbles: true, cancelable: true });
    ed.root.dispatchEvent(ev);
    return ev.defaultPrevented;
  });
  check('Space is left to the host (not prevented)', spacePrevented === false);

  // Box select + rotate/scale via the transform box.
  await page.evaluate(() => window.wmpEditor.selectPoints([]));
  const r = await page.evaluate(() => { const b = window.wmpEditor.canvas.getBoundingClientRect(); return { x: b.left, y: b.top, w: b.width, h: b.height }; });
  await page.mouse.move(r.x + 4, r.y + 4);
  await page.mouse.down();
  await page.mouse.move(r.x + r.w - 4, r.y + r.h - 20, { steps: 5 });
  await page.mouse.up();
  const selCount = (await page.evaluate(() => window.wmpEditor.getSelection())).length;
  check('box selection selects all points inside', selCount === 4, `${selCount}`);
  const lenB = await pathLength(page);
  const box = await page.evaluate(() => window.wmpEditor.transformBox());
  const cr = box.corners[2];
  const pv = box.corners[0];
  await page.mouse.move(r.x + cr.x, r.y + cr.y);
  await page.mouse.down();
  await page.mouse.move(r.x + pv.x + (cr.x - pv.x) * 0.5, r.y + pv.y + (cr.y - pv.y) * 0.5, { steps: 5 });
  await page.mouse.up();
  const lenS = await pathLength(page);
  check('corner drag scales the selection uniformly (~0.5x length)', Math.abs(lenS / lenB - 0.5) < 0.02, `${(lenS / lenB).toFixed(4)}`);
  const box2 = await page.evaluate(() => window.wmpEditor.transformBox());
  const before = (await doc(page)).path.points.map((p) => p.p);
  await page.mouse.move(r.x + box2.knob.x, r.y + box2.knob.y);
  await page.mouse.down();
  await page.mouse.move(r.x + box2.cx + (box2.cy - box2.knob.y), r.y + box2.cy, { steps: 6 });
  await page.mouse.up();
  const lenR = await pathLength(page);
  const after = (await doc(page)).path.points.map((p) => p.p);
  check('rotate knob turns the selection without changing its length', Math.abs(lenR - lenS) < 1e-6 && JSON.stringify(before) !== JSON.stringify(after));
  await shot(page, '02-transform-box.png');

  // Closed toggle, reverse, grid/snap persisted in the document.
  await page.keyboard.press('Escape');
  await page.click('[data-action="closed"]');
  d = await doc(page);
  check('closed toggle closes the path', d.path.closed === true);
  const firstId = d.path.points[0].id, secondId = d.path.points[1].id, lastId = d.path.points[3].id;
  await page.click('[data-action="reverse"]');
  d = await doc(page);
  check('reverse keeps a closed loop start and flips the order', d.path.points[0].id === firstId && d.path.points[1].id === lastId && d.path.points[3].id === secondId);
  await page.click('[data-action="grid"]');
  await page.click('[data-action="snap"]');
  d = await doc(page);
  check('grid/snap settings are stored in the project document', d.editor && d.editor.grid === false && d.editor.snap === true);
  const sp0 = d.path.points[0].p;
  s = await screen(page, { x: sp0[0], y: sp0[1] });
  // Target: a grid intersection away from other anchors, approached 3 px off.
  const gx = (Math.round((sp0[0] * asp) / 0.05) + 3) * 0.05, gy = (Math.round(sp0[1] / 0.05) + 2) * 0.05;
  const gs = await screen(page, { x: gx / asp, y: gy });
  await page.mouse.move(s.x, s.y);
  await page.mouse.down();
  await page.mouse.move(gs.x + 3, gs.y - 2, { steps: 6 });
  await page.mouse.up();
  d = await doc(page);
  const snapped = d.path.points[0].p;
  check('snap lands a dragged point exactly on a nearby grid intersection',
    Math.abs(snapped[0] * asp - gx) < 1e-9 && Math.abs(snapped[1] - gy) < 1e-9, JSON.stringify(snapped));
  // Ctrl disables snapping for the drag.
  s = await screen(page, { x: snapped[0], y: snapped[1] });
  await page.keyboard.down('Control');
  await page.mouse.move(s.x, s.y);
  await page.mouse.down();
  await page.mouse.move(s.x + 3, s.y - 2, { steps: 3 });
  await page.mouse.up();
  await page.keyboard.up('Control');
  d = await doc(page);
  check('Ctrl drag moves freely (no snapping)', Math.abs(d.path.points[0].p[0] * asp - gx) > 1e-6);

  // Zoom does not touch the document.
  const wz = await writes(page);
  await page.mouse.move(r.x + r.w / 2, r.y + r.h / 2);
  await page.mouse.wheel(0, -400);
  const zoom = await page.evaluate(() => window.wmpEditor.view.s);
  check('wheel zooms the view only (no writes)', (await writes(page)) === wz && zoom > 0);

  // Preview marker equals the shared core evaluation.
  await page.fill('[data-field="preview"]', '0.8').catch(() => {});
  await page.evaluate(() => { const r2 = document.querySelector('[data-field="preview"]'); r2.value = '0.8'; r2.dispatchEvent(new Event('input')); });
  const markerOk = await page.evaluate(async () => {
    const { previewPose } = await import('/editor/src/index.js');
    const ed = window.wmpEditor;
    const pose = previewPose(ed.getGeometry(), ed.poseSettings());
    return ed.preview.progress === 0.8 && Number.isFinite(pose.positionN.x);
  });
  check('preview slider drives the shared pose evaluation', markerOk);

  // Parameters: write through the adapter; animated ones are not edited statically.
  await page.click('details[data-section="transform"] > summary');
  await page.fill('[data-param-input="wmpRotation"]', '45');
  await page.press('[data-param-input="wmpRotation"]', 'Enter');
  check('rotation parameter written to the host', await page.evaluate(() => window.wmpHost.params.wmpRotation === 45));
  await page.evaluate(async () => {
    window.wmpHost.keyframes.wmpRotation = [{ t: 0, v: 0, interp: 'linear' }, { t: 2, v: 90, interp: 'linear' }];
    window.wmpHost.listeners.forEach((l) => l({ type: 'params' }));
    await new Promise((res) => setTimeout(res, 50));
  });
  check('animated parameter is shown read-only with a badge', await page.evaluate(() => document.querySelector('[data-param-input="wmpRotation"]').disabled && !document.querySelector('[data-param="wmpRotation"] .wmp-badge').hidden));

  // Built-in preset and user presets (timing fit/preserve).
  await page.click('[data-preset="circle"]');
  d = await doc(page);
  check('built-in preset loads editable points (circle: 4, closed)', d.path.points.length === 4 && d.path.closed);
  await page.fill('[data-field="preset-name"]', 'loop');
  await page.selectOption('[data-field="preset-level"]', 'timing');
  await page.click('[data-action="preset-save"]');
  check('timing preset saved', await page.evaluate(() => window.wmpHost.presets.has('loop')));
  const presetJson = await page.evaluate(() => window.wmpHost.presets.get('loop'));
  await page.close();

  // Load it into a second "event" that is twice as long.
  page = await open();
  await page.evaluate(async (json) => {
    window.wmpHost.presets.set('loop', json);
    window.wmpHost.context.eventDuration = 10;
    window.wmpEditor.applyContext(await window.wmpHost.getContext());
    await window.wmpEditor.refreshPresetList();
  }, presetJson);
  await page.selectOption('[data-field="preset-list"]', 'loop');
  await page.selectOption('[data-field="preset-timing"]', 'fit');
  await page.click('[data-action="preset-load"]');
  await page.waitForTimeout(50);
  const fitKeys = await page.evaluate(() => window.wmpHost.keyframes.wmpProgress.map((k) => k.t));
  check('fit to event retimes keyframes from the source (5 s -> 10 s)', JSON.stringify(fitKeys) === JSON.stringify([0, 10]), JSON.stringify(fitKeys));
  await page.selectOption('[data-field="preset-timing"]', 'preserve');
  await page.click('[data-action="preset-load"]');
  await page.waitForTimeout(50);
  const keepKeys = await page.evaluate(() => window.wmpHost.keyframes.wmpProgress.map((k) => k.t));
  check('preserve timing keeps the original key times', JSON.stringify(keepKeys) === JSON.stringify([0, 5]), JSON.stringify(keepKeys));
  d = await doc(page);
  check('preset geometry arrives as editable points', d.path.points.length === 4 && d.path.closed);

  // Host-side change (e.g. VEGAS undo) reloads without echo writes.
  const wh = await writes(page);
  await page.click('#hostUndo');
  await page.waitForTimeout(80);
  d = await doc(page);
  check('host-side change reloads the path without writing back', d.path.points.length === 6 && (await writes(page)) === wh);

  // Newer-format data: read-only, never overwritten.
  await page.evaluate(async () => {
    window.wmpHost.emitExternalPathChange('{"format":"wron.motionpath","version":9,"path":{}}');
    await new Promise((res) => setTimeout(res, 50));
  });
  const ro = await page.evaluate(() => ({ ro: window.wmpEditor.readOnly, addDisabled: document.querySelector('[data-action="tool-add"]').disabled, text: document.querySelector('.wmp-error').textContent }));
  const wro = await writes(page);
  const rr = await page.evaluate(() => { const b = window.wmpEditor.canvas.getBoundingClientRect(); return { x: b.left, y: b.top, w: b.width, h: b.height }; });
  await page.mouse.click(rr.x + rr.w / 2, rr.y + rr.h / 2);
  await page.keyboard.press('Delete');
  check('future-version data is read-only and never written', ro.ro && ro.addDisabled && ro.text.length > 0 && (await writes(page)) === wro && (await page.evaluate(() => window.wmpHost.pathData)).includes('"version":9'));
  await shot(page, '03-readonly-future.png');
  await page.close();

  // Locales.
  for (const loc of ['en', 'tr', 'es', 'pt', 'fr', 'de', 'ru', 'zh-Hans']) {
    page = await open({ width: 1100, height: 700 }, `?lang=${loc}`);
    const info = await page.evaluate(() => ({
      lang: window.wmpEditor.root.getAttribute('lang'),
      labels: [...document.querySelectorAll('.wmp-toolbar [aria-label]')].map((b) => b.getAttribute('aria-label')),
      overflow: window.wmpEditor.inspector.scrollWidth > window.wmpEditor.inspector.clientWidth + 1,
    }));
    check(`locale ${loc}: strings applied, no raw keys, inspector does not overflow horizontally`,
      info.lang === loc && info.labels.every((l) => !/^[a-z]+\.[a-zA-Z]+$/.test(l)) && !info.overflow);
    if (loc === 'tr' || loc === 'zh-Hans' || loc === 'ru') await shot(page, `04-locale-${loc}.png`);
    await page.close();
  }

  // Small docks.
  for (const [w, h] of [[760, 480], [420, 300], [360, 240], [320, 180], [900, 180]]) {
    page = await open({ width: 1200, height: 800 });
    await page.evaluate(({ w: ww, h: hh }) => { const dk = document.getElementById('dock'); dk.style.width = `${ww}px`; dk.style.height = `${hh}px`; }, { w, h });
    await page.waitForTimeout(150);
    const info = await page.evaluate(() => {
      const ed = window.wmpEditor;
      const c = ed.canvas.getBoundingClientRect();
      return { size: ed.root.dataset.size, overflow: ed.root.scrollWidth > ed.root.clientWidth + 1, cw: c.width, ch: c.height,
        toolbarOverflow: ed.toolbar.scrollWidth > ed.toolbar.clientWidth + 1, inspectorBtn: getComputedStyle(ed.tb.inspector).display !== 'none' };
    });
    const needsDrawer = /short|tiny/.test(info.size);
    check(`dock ${w}x${h}: usable canvas (${Math.round(info.cw)}x${Math.round(info.ch)}), no horizontal overflow, settings reachable`,
      !info.overflow && !info.toolbarOverflow && info.cw >= 150 && info.ch >= Math.min(140, h - 40) && (!needsDrawer || info.inspectorBtn), info.size);
    await shot(page, `05-dock-${w}x${h}.png`);
    if (needsDrawer) {
      await page.click('[data-action="inspector"]');
      await page.waitForTimeout(80);
      const visible = await page.evaluate(() => getComputedStyle(window.wmpEditor.inspector).display !== 'none');
      check(`dock ${w}x${h}: settings drawer opens`, visible);
      await shot(page, `05-dock-${w}x${h}-settings.png`);
    }
    await page.close();
  }

  page = await open({ width: 1100, height: 700 }, '?theme=light&lang=de');
  await shot(page, '06-light-de.png');
  check('light theme renders without errors', page.errors.length === 0);
  await page.close();
} catch (e) {
  check('verification script ran to completion', false, e.stack || String(e));
} finally {
  await browser.close();
  server.kill();
}

const failed = results.filter((r) => !r.ok);
console.log(`\n${results.length - failed.length}/${results.length} checks passed`);
process.exit(failed.length ? 1 : 0);
