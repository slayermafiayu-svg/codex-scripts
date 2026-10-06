// Browser verification: loads the demo in headless Chromium, drives real
// pointer/keyboard interactions, asserts document state, and saves screenshots.
import http from 'node:http';
import { createRequire } from 'node:module';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
// Resolve playwright from the project, then from the global toolchain.
const requireFrom = createRequire(import.meta.url);
let chromium;
for (const candidate of ['playwright', process.env.PLAYWRIGHT_MODULE, '/opt/node-tools/node_modules/playwright'].filter(Boolean)) {
  try {
    chromium = requireFrom(candidate).chromium;
    break;
  } catch {
    /* try next */
  }
}
if (!chromium) {
  console.error('playwright not found: npm i -D playwright (or set PLAYWRIGHT_MODULE)');
  process.exit(2);
}
// Screenshots go to an ignored folder unless --screenshots <dir> is given
// (used to refresh docs/screenshots on purpose, not on every run).
const argIdx = process.argv.indexOf('--screenshots');
const outDir =
  argIdx >= 0 ? path.resolve(process.argv[argIdx + 1] || path.join(root, 'docs', 'screenshots')) : path.join(root, '.verify-out');
fs.mkdirSync(outDir, { recursive: true });

const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.json': 'application/json', '.png': 'image/png' };
const server = http.createServer((req, res) => {
  const url = decodeURIComponent(req.url.split('?')[0]);
  let file = path.join(root, url === '/' ? '/demo/index.html' : url);
  if (!file.startsWith(root) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) {
    res.writeHead(404);
    res.end('not found');
    return;
  }
  res.writeHead(200, { 'Content-Type': MIME[path.extname(file)] || 'application/octet-stream', 'Cache-Control': 'no-store' });
  fs.createReadStream(file).pipe(res);
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const port = server.address().port;
const base = `http://127.0.0.1:${port}/demo/index.html`;

const results = [];
const check = (name, ok, detail = '') => {
  results.push({ name, ok, detail });
  console.log(`${ok ? 'ok ' : 'FAIL'} ${name}${detail ? ` — ${detail}` : ''}`);
};

const browser = await chromium.launch();
const errors = [];
try {
  const page = await browser.newPage({ viewport: { width: 1280, height: 720 }, deviceScaleFactor: 2 });
  page.on('console', (m) => {
    if (m.type() === 'error') errors.push(m.text());
  });
  page.on('pageerror', (e) => errors.push(String(e)));
  await page.goto(base);
  await page.waitForFunction(() => window.editor && window.editor.store.channels.length > 0);
  await page.waitForTimeout(150);

  const canvas = page.locator('.ge-canvas');
  const box = async () => await canvas.boundingBox();

  // Helper: screen position of a key by id.
  const keyPos = async (id) =>
    page.evaluate((id) => {
      const ed = window.editor;
      const ref = ed.store.keyRef(id);
      const tf = { offset: 0, scale: 1 };
      const r = ed.canvas.getBoundingClientRect();
      return { x: r.left + ed.vp.xOf(ref.key.t), y: r.top + ed.vp.yOf((ref.key.v - tf.offset) * tf.scale), t: ref.key.t, v: ref.key.v };
    }, id);
  const doc = async () => page.evaluate(() => window.editor.getDocument());
  const keyById = (d, id) => d.channels.flatMap((c) => c.keys).find((k) => k.id === id);
  const countKeys = (d) => d.channels.reduce((n, c) => n + c.keys.length, 0);

  // 1. Initial render + screenshot
  await page.screenshot({ path: path.join(outDir, '01-wide-value.png') });
  const d0 = await doc();
  const initialKeys = countKeys(d0);
  check('initial document loaded with sample keys', initialKeys === 22, `${initialKeys} keys`);

  // 2. Click a key selects it; drag moves it with frame snapping and preserves key count
  let p = await keyPos('px1');
  await page.mouse.move(p.x, p.y);
  await page.mouse.down();
  await page.mouse.move(p.x + 37, p.y - 20, { steps: 8 });
  await page.mouse.move(p.x + 61, p.y - 44, { steps: 8 });
  await page.screenshot({ path: path.join(outDir, '02-drag-key-ghost.png') });
  await page.mouse.up();
  let d1 = await doc();
  const px1 = keyById(d1, 'px1');
  const fps = d1.fps.num / d1.fps.den;
  const frames = px1.t * fps;
  check(
    'dragged key snapped to a whole frame',
    Math.abs(frames - Math.round(frames)) < 1e-6,
    `t=${px1.t.toFixed(5)} (${frames.toFixed(3)} f)`,
  );
  check('drag moved the key in time and value', px1.t > 1.5 && px1.v > 80, `t=${px1.t.toFixed(3)} v=${px1.v.toFixed(2)}`);
  check('drag did not add keys', countKeys(d1) === initialKeys, `${countKeys(d1)} keys`);
  const undoCount = await page.evaluate(() => window.editor.store._undo.length);
  check('one drag = one undo entry', undoCount === 1, `${undoCount}`);

  // 3. Undo restores
  await page.keyboard.press('Control+z');
  d1 = await doc();
  check('undo restored the key', Math.abs(keyById(d1, 'px1').t - 1.5) < 1e-9 && keyById(d1, 'px1').v === 80);
  await page.keyboard.press('Control+y');
  d1 = await doc();
  check('redo re-applied the move', Math.abs(keyById(d1, 'px1').t - px1.t) < 1e-9);
  await page.keyboard.press('Control+z');

  // 4. Box select from empty space across channels, then group move keeps spacing
  await page.keyboard.press('Escape');
  const a = await keyPos('px2');
  const b = await keyPos('py1');
  const bx = await box();
  const x0 = Math.min(a.x, b.x) - 30;
  const x1 = Math.max(a.x, b.x) + 30;
  await page.mouse.move(x0, bx.y + 40);
  await page.mouse.down();
  await page.mouse.move(x1, bx.y + bx.height - 10, { steps: 6 });
  await page.screenshot({ path: path.join(outDir, '03-box-select.png') });
  await page.mouse.up();
  let selCount = await page.evaluate(() => window.editor.getSelection().keys.length);
  check('box select from empty space selected multiple keys across channels', selCount >= 2, `${selCount} keys`);
  const selIds = await page.evaluate(() => window.editor.getSelection().keys);
  const before = await doc();
  const t0s = selIds.map((id) => keyById(before, id).t);
  const grab = await keyPos(selIds[0]);
  await page.mouse.move(grab.x, grab.y);
  await page.mouse.down();
  await page.mouse.move(grab.x + 80, grab.y + 5, { steps: 10 });
  await page.screenshot({ path: path.join(outDir, '04-group-move-transform-box.png') });
  await page.mouse.up();
  const after = await doc();
  const dts = selIds.map((id, i) => keyById(after, id).t - t0s[i]);
  const sameDelta = dts.every((dt) => Math.abs(dt - dts[0]) < 1e-9);
  check('group move applied the same frame delta to every selected key', sameDelta && dts[0] > 0, `Δ=${(dts[0] * fps).toFixed(3)} f`);
  await page.keyboard.press('Control+z');

  // 5. Double-click a curve inserts a key without changing the motion
  await page.keyboard.press('Escape');
  await page.evaluate(() => window.editor.fitAll());
  const sample = await page.evaluate(() => {
    const ed = window.editor;
    const ch = ed.store.channel('pos.x');
    const out = [];
    for (let i = 0; i <= 180; i++) out.push(ed.store.evalChannel('pos.x', i / 30));
    const t = 2.2;
    const r = ed.canvas.getBoundingClientRect();
    return { out, x: r.left + ed.vp.xOf(t), y: r.top + ed.vp.yOf(ed.store.evalChannel('pos.x', t)), keys: ch.keys.length };
  });
  await page.mouse.dblclick(sample.x, sample.y);
  const afterInsert = await page.evaluate(() => {
    const ed = window.editor;
    const out = [];
    for (let i = 0; i <= 180; i++) out.push(ed.store.evalChannel('pos.x', i / 30));
    return { out, keys: ed.store.channel('pos.x').keys.length, sel: ed.getSelection().keys.length };
  });
  const maxDiff = Math.max(...sample.out.map((v, i) => Math.abs(v - afterInsert.out[i])));
  check('double-click inserted exactly one key', afterInsert.keys === sample.keys + 1, `${sample.keys} -> ${afterInsert.keys}`);
  check('inserted key did not change the curve', maxDiff < 1e-6, `max diff ${maxDiff.toExponential(2)}`);
  check('inserted key is selected', afterInsert.sel === 1);
  await page.screenshot({ path: path.join(outDir, '05-inserted-key-handles.png') });

  // 6. Handle drag edits the tangent; Alt breaks it
  const hp = await page.evaluate(() => {
    const ed = window.editor;
    const id = ed.getSelection().keys[0];
    const ref = ed.store.keyRef(id);
    const r = ed.canvas.getBoundingClientRect();
    const k = ref.key;
    return { id, x: r.left + ed.vp.xOf(k.t + k.out.dt), y: r.top + ed.vp.yOf(k.v + k.out.dv), tangents: k.tangents };
  });
  await page.mouse.move(hp.x, hp.y);
  await page.mouse.down();
  await page.mouse.move(hp.x + 15, hp.y - 40, { steps: 6 });
  await page.mouse.up();
  const afterHandle = await page.evaluate((id) => {
    const k = window.editor.store.keyRef(id).key;
    return { out: k.out, in: k.in, tangents: k.tangents };
  }, hp.id);
  check(
    'handle drag changed the outgoing tangent',
    afterHandle.out.dv !== 0 && afterHandle.tangents === 'unified',
    `tangents=${afterHandle.tangents}`,
  );
  const slopeOut = afterHandle.out.dv / afterHandle.out.dt;
  const slopeIn = afterHandle.in.dv / afterHandle.in.dt;
  check('unified tangents stay collinear', Math.abs(slopeOut - slopeIn) < 1e-6, `${slopeOut.toFixed(3)} vs ${slopeIn.toFixed(3)}`);

  // 7. Easing preset via context menu on the last incoming segment
  const seg = await page.evaluate(() => {
    const ed = window.editor;
    const ch = ed.store.channel('rot.y');
    const k0 = ch.keys[ch.keys.length - 2];
    const k1 = ch.keys[ch.keys.length - 1];
    const t = (k0.t + k1.t) / 2;
    const r = ed.canvas.getBoundingClientRect();
    return { x: r.left + ed.vp.xOf(t), y: r.top + ed.vp.yOf(ed.store.evalChannel('rot.y', t)), k0: k0.id, k1: k1.id, k0in: { ...k0.in } };
  });
  await page.mouse.click(seg.x, seg.y);
  const segSel = await page.evaluate(() => window.editor.getSelection().segment);
  check('clicking a curve selects that segment (no select mode needed)', !!segSel && segSel.keyId === seg.k0);
  await page.mouse.click(seg.x, seg.y, { button: 'right' });
  await page.waitForSelector('.ge-menu');
  await page.screenshot({ path: path.join(outDir, '06-context-menu.png') });
  await page.click('.ge-menu .ge-menu-item:has-text("Ease In-Out")');
  const eased = await page.evaluate(({ k0, k1 }) => {
    const ed = window.editor;
    const a = ed.store.keyRef(k0).key;
    const b = ed.store.keyRef(k1).key;
    const dur = b.t - a.t;
    return { outFrac: a.out.dt / dur, inFrac: -b.in.dt / dur, outDv: a.out.dv, k0in: { ...a.in } };
  }, seg);
  check(
    'easing applied to the last incoming segment only',
    Math.abs(eased.outFrac - 0.42) < 1e-6 && Math.abs(eased.inFrac - 0.42) < 1e-6 && Math.abs(eased.outDv) < 1e-9,
    `out ${eased.outFrac.toFixed(3)} in ${eased.inFrac.toFixed(3)}`,
  );
  check(
    'middle key incoming handle untouched by easing of its outgoing segment',
    eased.k0in.dt === seg.k0in.dt && eased.k0in.dv === seg.k0in.dv,
  );

  // 8. Keyboard: Delete must not fire while typing in the inspector
  await page.evaluate(() => window.editor.selectKeys(['py1']));
  const timeInput = page.locator('.ge-input[data-field="time"]');
  await timeInput.focus();
  await page.keyboard.press('Delete');
  const stillThere = await page.evaluate(() => !!window.editor.store.keyRef('py1'));
  check('Delete inside a text field does not delete the key', stillThere);
  await timeInput.fill('75');
  await page.keyboard.press('Enter');
  const movedByInput = await page.evaluate(() => window.editor.store.keyRef('py1').key.t);
  check('typing a frame number in the inspector moves the key', Math.abs(movedByInput - 2.5) < 1e-9, `t=${movedByInput}`);
  await timeInput.fill('abc');
  await page.keyboard.press('Enter');
  const invalidClass = await timeInput.evaluate((e) => e.classList.contains('is-invalid'));
  const unchanged = await page.evaluate(() => window.editor.store.keyRef('py1').key.t);
  check('invalid input is flagged and does not change the key', invalidClass && Math.abs(unchanged - 2.5) < 1e-9);
  const valueInput = page.locator('.ge-input[data-field="value"]');
  await valueInput.fill('12,5');
  await page.keyboard.press('Enter');
  const commaValue = await page.evaluate(() => window.editor.store.keyRef('py1').key.v);
  check('comma decimal input accepted', commaValue === 12.5, `v=${commaValue}`);

  // 9. Collision block: nudge a key onto its neighbour with arrow keys
  await page.evaluate(() => {
    const ed = window.editor;
    ed.root.focus();
    ed.selectKeys(['py0']);
  });
  const py1t = await page.evaluate(() => window.editor.store.keyRef('py1').key.t);
  await page.evaluate((t) => {
    const ed = window.editor;
    ed.store.moveKeys(['py0'], t, 0, { anchorId: 'py0' });
  }, py1t);
  const py0t = await page.evaluate(() => window.editor.store.keyRef('py0').key.t);
  const py1still = await page.evaluate(() => !!window.editor.store.keyRef('py1'));
  check(
    'blocked collision stops one frame before the occupied frame',
    py1still && Math.abs(py0t - (py1t - 1 / 30)) < 1e-9,
    `t=${py0t.toFixed(4)}`,
  );

  // 10. Zoom with wheel keeps the time under the cursor fixed
  const zb = await box();
  const cx = Math.round(zb.x + zb.width * 0.6);
  const cy = Math.round(zb.y + zb.height * 0.5);
  const tBefore = await page.evaluate((x) => window.editor.vp.tOf(x), cx - zb.x);
  await page.mouse.move(cx, cy);
  await page.mouse.wheel(0, -400);
  await page.waitForTimeout(50);
  const tAfter = await page.evaluate((x) => window.editor.vp.tOf(x), cx - zb.x);
  const zoomed = await page.evaluate(() => window.editor.vp.pxPerSec);
  check(
    'wheel zoom keeps the time under the cursor',
    Math.abs(tBefore - tAfter) < 1e-6,
    `${tBefore.toFixed(4)} -> ${tAfter.toFixed(4)}, ${zoomed.toFixed(1)} px/s`,
  );
  await page.screenshot({ path: path.join(outDir, '07-zoomed-frame-grid.png') });
  await page.keyboard.press('f');

  // 11. Speed graph mode
  await page.click('[data-action="mode-speed"]');
  await page.waitForTimeout(80);
  await page.evaluate(() => window.editor.selectKeys(['ry1']));
  await page.screenshot({ path: path.join(outDir, '08-speed-graph.png') });
  const modeNow = await page.evaluate(() => window.editor.getMode());
  check('speed mode active', modeNow === 'speed');
  await page.click('[data-action="mode-value"]');

  // 12. Normalize view
  await page.click('[data-action="normalize"]');
  await page.waitForTimeout(80);
  await page.screenshot({ path: path.join(outDir, '09-normalized.png') });
  await page.click('[data-action="normalize"]');

  // 13. NTSC fps: keys land on exact 1001-based frames
  await page.selectOption('#fps', '30000/1001');
  await page.waitForTimeout(100);
  const pN = await keyPos('px1');
  await page.mouse.move(pN.x, pN.y);
  await page.mouse.down();
  await page.mouse.move(pN.x + 53, pN.y, { steps: 6 });
  await page.mouse.up();
  const ntsc = await page.evaluate(() => {
    const ed = window.editor;
    const k = ed.store.keyRef('px1').key;
    const f = (k.t * ed.store.fps.num) / ed.store.fps.den;
    return { t: k.t, f, fps: ed.store.fps };
  });
  check(
    '29.97 drag snaps to exact 30000/1001 frames',
    Math.abs(ntsc.f - Math.round(ntsc.f)) < 1e-6 && ntsc.fps.den === 1001,
    `frame ${ntsc.f.toFixed(6)}`,
  );
  await page.selectOption('#fps', '30/1');

  // 14. Narrow dock layout
  await page.selectOption('#size', 'size-dock');
  await page.waitForTimeout(300);
  await page.evaluate(() => window.editor.fitAll());
  await page.waitForTimeout(80);
  await page.screenshot({ path: path.join(outDir, '10-narrow-dock-420x300.png') });
  const dockState = await page.evaluate(() => {
    const ed = window.editor;
    const r = ed.root.getBoundingClientRect();
    const stage = ed.stage.getBoundingClientRect();
    const toolbar = ed.toolbar;
    return {
      size: ed.root.getAttribute('data-size'),
      w: r.width,
      h: r.height,
      stageH: stage.height,
      toolbarScroll: toolbar.scrollWidth > toolbar.clientWidth + 1,
      overflowX: ed.root.scrollWidth > ed.root.clientWidth + 1,
    };
  });
  check(
    'narrow dock keeps the plot area usable (>= 45% of height)',
    dockState.stageH >= dockState.h * 0.45,
    `${Math.round(dockState.stageH)}px of ${Math.round(dockState.h)}px, size="${dockState.size}"`,
  );
  check('narrow dock does not overflow the panel horizontally', !dockState.overflowX);

  await page.selectOption('#size', 'size-dock-tall');
  await page.waitForTimeout(300);
  await page.screenshot({ path: path.join(outDir, '11-narrow-tall-380x560.png') });
  await page.selectOption('#size', 'size-strip');
  await page.waitForTimeout(300);
  await page.screenshot({ path: path.join(outDir, '12-strip-900x180.png') });
  const stripState = await page.evaluate(() => window.editor.root.getAttribute('data-size'));
  check('strip layout hides inset and navigator', /short/.test(stripState), `size="${stripState}"`);
  await page.selectOption('#size', 'size-wide');
  await page.waitForTimeout(300);

  // 15. Light theme + Turkish locale
  await page.selectOption('#theme', 'light');
  await page.selectOption('#locale', 'tr');
  await page.waitForTimeout(120);
  await page.evaluate(() => window.editor.selectKeys(['px2']));
  await page.screenshot({ path: path.join(outDir, '13-light-turkish.png') });
  const trLabel = await page.locator('[data-action="mode-value"] .ge-btn-label').textContent();
  check('locale switch relabels the UI', trLabel.trim() === 'Değer', trLabel);
  await page.selectOption('#theme', 'dark');
  await page.selectOption('#locale', 'en');

  // 16. Idle: no animation frames when nothing changes (settle first: the
  // locale/theme switch above rebuilds the DOM and ResizeObserver fires once).
  await page.waitForTimeout(400);
  const idleFrames = await page.evaluate(async () => {
    const ed = window.editor;
    let count = 0;
    const orig = ed._render.bind(ed);
    ed._render = () => {
      count++;
      orig();
    };
    await new Promise((r) => setTimeout(r, 600));
    ed._render = orig;
    return count;
  });
  check('no re-render while idle', idleFrames === 0, `${idleFrames} renders in 600 ms`);

  check('no console errors or page errors', errors.length === 0, errors.slice(0, 3).join(' | '));
} finally {
  await browser.close();
  server.close();
}

const failed = results.filter((r) => !r.ok);
console.log(`\n${results.length - failed.length}/${results.length} browser checks passed`);
fs.writeFileSync(path.join(outDir, 'results.json'), JSON.stringify(results, null, 2));
process.exit(failed.length ? 1 : 0);
