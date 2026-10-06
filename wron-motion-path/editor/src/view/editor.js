// Wron Motion Path — path editor component.
//
// Framework-agnostic: mount into any element, talk to the host only through
// the adapter contract in ../host/adapter.js. The editor never assumes VEGAS
// behaviour: what the host cannot do (parameters, keyframes, preset storage,
// frame image) is shown as unavailable instead of pretending to work.
//
// Data flow: the path document lives in the effect instance (OFX string
// parameter "wmpPathData"). One logical edit = one undo entry = one
// writePathData() call. Drags render from a working copy and write once on
// release; nothing is written while the pointer moves.
import { clonePath, loadDocument, makeDocument, reversed, saveDocument, segmentCount, Status, usable } from '../core/document.js';
import { PathGeometry, SpeedMode } from '../core/geometry.js';
import { easingFor } from '../core/timing.js';
import { previewPose } from '../core/motion.js';
import * as E from '../core/edit.js';
import { makePreset, PRESET_KEYS } from '../core/presets.js';
import { History } from '../core/history.js';
import { PARAMS, defaults, isEnabled, paramById, parseNumber, sanitize } from '../core/params.js';
import { buildPreset, parsePreset, planApply } from '../core/presets-file.js';
import { validateAdapter } from '../host/adapter.js';
import { translator, resolveLocale } from './strings.js';
import { ICONS } from './icons.js';

const HIT_PX = 8;
const GRID_STEP = 0.05; // frame heights
const SNAP_PX = 8;

function el(tag, attrs = {}, children = []) {
  const n = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (v === undefined || v === null || v === false) continue;
    if (k === 'class') n.className = v;
    else if (k === 'text') n.textContent = v;
    else if (k === 'html') n.innerHTML = v;
    else if (k.startsWith('on')) n.addEventListener(k.slice(2), v);
    else n.setAttribute(k, v === true ? '' : v);
  }
  for (const c of [].concat(children)) if (c) n.append(c);
  return n;
}

const samePath = (a, b) => JSON.stringify(a) === JSON.stringify(b);
const fmt = (v, d) => (Number.isFinite(v) ? Number(v.toFixed(d)).toString() : '');

export function createPathEditor(container, options = {}) {
  return new PathEditor(container, options);
}

export class PathEditor {
  constructor(container, { host, locale = 'en', theme = 'dark', dev = false } = {}) {
    this.container = container;
    this.host = host;
    this.caps = validateAdapter(host);
    this.locale = resolveLocale(locale);
    this.t = translator(this.locale);
    this.dev = dev;
    this.listeners = new Map();
    this.history = new History();
    this.doc = makeDocument({ closed: false, points: [] });
    this.status = Status.Ok;
    this.statusMessage = '';
    this.readOnly = false;
    this.selection = new Set();
    this.tool = 'select';
    this.settings = { grid: true, snap: false, units: 'norm' };
    this.preview = { progress: 0.35 };
    this.values = defaults();
    this.values.wmpProgress = 0;
    this.animated = new Set();
    this.keyframes = {};
    this.frame = { width: 1920, height: 1080, par: 1 };
    this.view = { s: 300, ox: 0, oy: 0, fitted: 'frame' };
    this.geometry = null;
    this.geometryKey = '';
    this.drag = null;
    this.hover = null;
    this.snapGuides = [];
    this.writeChain = Promise.resolve();
    this.lastWritten = null;
    this.drawPending = false;
    this.destroyed = false;
    this.buildDom(theme);
    this.bindEvents();
    this.ready = this.load();
  }

  // ------------------------------------------------------------------ API
  on(name, fn) {
    if (!this.listeners.has(name)) this.listeners.set(name, new Set());
    this.listeners.get(name).add(fn);
    return () => this.listeners.get(name)?.delete(fn);
  }
  emit(name, payload) {
    for (const fn of this.listeners.get(name) || []) fn(payload);
  }
  getDocument() {
    return JSON.parse(saveDocument(this.doc));
  }
  getSelection() {
    return [...this.selection].sort((a, b) => a - b);
  }
  selectPoints(indices) {
    this.selection = new Set(indices.filter((i) => i >= 0 && i < this.doc.path.points.length));
    this.selectionChanged();
  }
  setTool(tool) {
    this.tool = tool;
    this.updateToolbar();
    this.updateStatus();
  }
  setLocale(locale) {
    this.locale = resolveLocale(locale);
    this.t = translator(this.locale);
    this.root.setAttribute('lang', this.locale);
    this.rebuildChrome();
  }
  setTheme(theme) {
    this.root.dataset.theme = theme === 'light' ? 'light' : 'dark';
    this.colors = null;
    this.requestDraw();
  }
  async undo() {
    const e = this.history.undo();
    if (!e) return;
    this.doc.path = clonePath(e.before);
    this.afterEdit(this.t('undo'));
  }
  async redo() {
    const e = this.history.redo();
    if (!e) return;
    this.doc.path = clonePath(e.after);
    this.afterEdit(this.t('redo'));
  }
  async flush() {
    await this.writeChain;
  }
  destroy() {
    this.destroyed = true;
    this.unsubscribe?.();
    this.resizeObserver?.disconnect();
    window.removeEventListener('keyup', this.onWindowKeyUp);
    this.root.remove();
  }

  // ----------------------------------------------------------- loading
  async load() {
    try {
      const ctx = await this.host.getContext();
      this.applyContext(ctx);
      this.applyLoadedJson(await this.host.readPathData());
      await this.loadParams();
      if (this.caps.frameImage && this.host.getFrameImage) this.frameImage = await this.host.getFrameImage();
      this.unsubscribe = this.host.subscribe((evt) => this.onHostChange(evt));
      if (this.caps.presets) await this.refreshPresetList();
    } catch (e) {
      this.toast(this.t('msg.writeFailed', { msg: e.message }), 'error');
    }
    this.rebuildChrome();
    this.fitFrame();
  }

  applyContext(ctx) {
    this.context = ctx;
    if (ctx?.frame?.width > 0 && ctx?.frame?.height > 0) this.frame = { par: 1, ...ctx.frame };
    this.geometryKey = '';
  }

  get aspect() {
    return (this.frame.width * (this.frame.par || 1)) / this.frame.height;
  }

  applyLoadedJson(json) {
    const r = loadDocument(json);
    this.lastWritten = json;
    this.statusMessage = r.message;
    if (r.status === Status.Empty) {
      this.doc = makeDocument({ closed: false, points: [] });
      this.status = Status.Ok;
      this.readOnly = false;
    } else if (usable(r)) {
      this.doc = r.doc;
      this.status = r.status;
      this.readOnly = false;
    } else {
      this.doc = makeDocument({ closed: false, points: [] });
      this.status = r.status;
      this.readOnly = true;
    }
    const ed = this.doc.root.editor;
    if (ed && typeof ed === 'object') {
      if (typeof ed.grid === 'boolean') this.settings.grid = ed.grid;
      if (typeof ed.snap === 'boolean') this.settings.snap = ed.snap;
      if (ed.units === 'px' || ed.units === 'norm') this.settings.units = ed.units;
    }
    this.selection = new Set([...this.selection].filter((i) => i < this.doc.path.points.length));
    this.geometryKey = '';
  }

  async loadParams() {
    if (!this.caps.params || !this.host.readParams) return;
    const ids = [...PARAMS.map((p) => p.id), 'wmpProgress'];
    const got = await this.host.readParams(ids);
    for (const [id, v] of Object.entries(got)) {
      const p = paramById(id);
      const clean = p ? sanitize(p, v) : Number.isFinite(v) ? v : null;
      if (clean !== null) this.values[id] = clean;
    }
    if (this.caps.keyframes && this.host.readKeyframes) {
      this.keyframes = await this.host.readKeyframes(ids);
      this.animated = new Set(Object.keys(this.keyframes).filter((k) => this.keyframes[k]?.length));
    }
  }

  async onHostChange(evt) {
    if (this.destroyed) return;
    if (evt?.type === 'pathData') {
      const json = await this.host.readPathData();
      if (json === this.lastWritten) return; // our own write echoed back
      this.applyLoadedJson(json);
      this.history.clear(); // the host changed the data under us (e.g. VEGAS undo)
    } else if (evt?.type === 'params') {
      await this.loadParams();
    } else if (evt?.type === 'context') {
      this.applyContext(await this.host.getContext());
    }
    this.selectionChanged();
  }

  // ------------------------------------------------------------- writing
  persist(label) {
    const json = saveDocument(this.doc);
    this.lastWritten = json;
    this.writeChain = this.writeChain
      .then(() => this.host.writePathData(json, label))
      .then(() => this.emit('change', { document: JSON.parse(json), label }))
      .catch((e) => this.toast(this.t('msg.writeFailed', { msg: e?.message || String(e) }), 'error'));
    return this.writeChain;
  }

  commit(label, mutate, mergeKey = null) {
    if (this.readOnly) return false;
    const before = clonePath(this.doc.path);
    mutate(this.doc.path);
    const after = clonePath(this.doc.path);
    if (samePath(before, after)) return false;
    this.history.push(label, before, after, mergeKey);
    this.afterEdit(label);
    return true;
  }

  afterEdit(label) {
    this.selection = new Set([...this.selection].filter((i) => i < this.doc.path.points.length));
    this.geometryKey = '';
    this.persist(label);
    this.selectionChanged();
  }

  writeEditorSettings() {
    if (this.readOnly) return;
    this.doc.root = { ...this.doc.root, editor: { ...(this.doc.root.editor || {}), ...this.settings } };
    this.persist('Editor settings');
  }

  async setParam(id, value) {
    const p = paramById(id);
    const clean = sanitize(p, value);
    if (clean === null || !this.caps.params || this.animated.has(id)) return false;
    this.values[id] = clean;
    if (id === 'wmpUniformScale' && clean) this.values.wmpScale = [this.values.wmpScale[0], this.values.wmpScale[0]];
    try {
      await this.host.writeParam(id, clean, this.t(p.label));
    } catch (e) {
      this.toast(this.t('msg.writeFailed', { msg: e.message }), 'error');
    }
    this.updateInspector();
    this.requestDraw();
    return true;
  }

  // ---------------------------------------------------------------- DOM
  buildDom(theme) {
    this.root = el('div', { class: 'wmp-root', tabindex: '0', 'data-theme': theme === 'light' ? 'light' : 'dark', lang: this.locale, role: 'application' });
    this.banner = this.dev ? el('div', { class: 'wmp-banner' }) : null;
    this.toolbar = el('div', { class: 'wmp-toolbar', role: 'toolbar' });
    this.canvas = el('canvas', { class: 'wmp-canvas' });
    this.statusEl = el('div', { class: 'wmp-status' });
    this.toastEl = el('div', { class: 'wmp-toast', role: 'status', 'aria-live': 'polite' });
    this.stage = el('div', { class: 'wmp-stage' }, [this.canvas, this.statusEl, this.toastEl]);
    this.inspector = el('div', { class: 'wmp-inspector' });
    this.root.append(...[this.banner, this.toolbar, el('div', { class: 'wmp-main' }, [this.stage, this.inspector])].filter(Boolean));
    this.container.append(this.root);
  }

  rebuildChrome() {
    if (this.banner) this.banner.textContent = this.t('dev.banner');
    this.buildToolbar();
    this.buildInspector();
    this.updateStatus();
    this.requestDraw();
  }

  buildToolbar() {
    const t = this.t;
    const b = (key, icon, label, onclick, { pressed, labelVisible } = {}) => {
      const btn = el('button', { class: 'wmp-btn', type: 'button', title: label, 'aria-label': label, 'data-action': key, onclick }, []);
      btn.innerHTML = ICONS[icon] || '';
      if (labelVisible) btn.append(el('span', { class: 'wmp-label', text: label }));
      if (pressed !== undefined) btn.setAttribute('aria-pressed', String(pressed));
      return btn;
    };
    this.tb = {};
    const bar = this.toolbar;
    bar.replaceChildren();
    this.tb.select = b('tool-select', 'select', t('tool.select'), () => this.setTool('select'), { pressed: false });
    this.tb.add = b('tool-add', 'add', t('tool.add'), () => this.setTool('add'), { pressed: false });
    this.tb.remove = b('tool-delete', 'remove', t('tool.delete'), () => this.setTool('delete'), { pressed: false });
    this.tb.undo = b('undo', 'undo', t('undo'), () => this.undo());
    this.tb.redo = b('redo', 'redo', t('redo'), () => this.redo());
    this.tb.corner = b('mode-corner', 'corner', t('mode.corner'), () => this.setModeSelected('corner'));
    this.tb.smooth = b('mode-smooth', 'smooth', t('mode.smooth'), () => this.setModeSelected('smooth'));
    this.tb.free = b('mode-free', 'free', t('mode.free'), () => this.setModeSelected('free'));
    this.tb.closed = b('closed', 'closed', t('path.close'), () => this.toggleClosed(), { pressed: false });
    this.tb.reverse = b('reverse', 'reverse', t('path.reverse'), () => this.reversePath());
    this.tb.grid = b('grid', 'grid', t('view.grid'), () => { this.settings.grid = !this.settings.grid; this.writeEditorSettings(); this.updateToolbar(); this.requestDraw(); }, { pressed: this.settings.grid });
    this.tb.snap = b('snap', 'snap', t('view.snap'), () => { this.settings.snap = !this.settings.snap; this.writeEditorSettings(); this.updateToolbar(); }, { pressed: this.settings.snap });
    this.tb.fitPath = b('fit-path', 'fitPath', t('view.fitPath'), () => this.fitPath());
    this.tb.fitFrame = b('fit-frame', 'fitFrame', t('view.fitFrame'), () => this.fitFrame());
    this.tb.zoomOut = b('zoom-out', 'zoomOut', t('view.zoomOut'), () => this.zoomBy(1 / 1.25));
    this.tb.zoom = el('span', { class: 'wmp-zoom', 'aria-live': 'off' });
    this.tb.zoomIn = b('zoom-in', 'zoomIn', t('view.zoomIn'), () => this.zoomBy(1.25));
    this.tb.inspector = b('inspector', 'settings', t('view.inspector'), () => this.toggleInspector(), { pressed: false });
    this.tb.inspector.classList.add('wmp-btn-inspector');
    const sep = (tier) => el('span', { class: 'wmp-sep', 'data-tier': tier });
    // Secondary controls are hidden in short/tiny docks (they stay available
    // in the settings panel), so the toolbar never takes more than one row.
    for (const k of ['corner', 'smooth', 'free', 'closed', 'reverse', 'grid', 'snap', 'zoomOut', 'zoom', 'zoomIn']) this.tb[k].dataset.tier = 'secondary';
    bar.append(this.tb.select, this.tb.add, this.tb.remove, sep(), this.tb.undo, this.tb.redo, sep('secondary'), this.tb.corner,
      this.tb.smooth, this.tb.free, this.tb.closed, this.tb.reverse, sep('secondary'), this.tb.grid, this.tb.snap, sep(),
      this.tb.fitPath, this.tb.fitFrame, this.tb.zoomOut, this.tb.zoom, this.tb.zoomIn, el('span', { class: 'wmp-spacer' }), this.tb.inspector);
    this.updateToolbar();
  }

  updateToolbar() {
    if (!this.tb) return;
    const ro = this.readOnly;
    this.tb.select.setAttribute('aria-pressed', String(this.tool === 'select'));
    this.tb.add.setAttribute('aria-pressed', String(this.tool === 'add'));
    this.tb.remove.setAttribute('aria-pressed', String(this.tool === 'delete'));
    for (const k of ['add', 'remove', 'reverse', 'closed']) this.tb[k].disabled = ro;
    this.tb.undo.disabled = ro || !this.history.canUndo();
    this.tb.redo.disabled = ro || !this.history.canRedo();
    const hasSel = this.selection.size > 0 && !ro;
    for (const k of ['corner', 'smooth', 'free']) this.tb[k].disabled = !hasSel;
    this.tb.closed.setAttribute('aria-pressed', String(!!this.doc.path.closed));
    this.tb.closed.disabled = ro || this.doc.path.points.length < 2;
    this.tb.reverse.disabled = ro || this.doc.path.points.length < 2;
    this.tb.grid.setAttribute('aria-pressed', String(this.settings.grid));
    this.tb.snap.setAttribute('aria-pressed', String(this.settings.snap));
    this.tb.zoom.textContent = `${Math.round((this.view.s / Math.max(1, this.frame.height)) * 100)}%`;
    this.tb.inspector.setAttribute('aria-pressed', String(this.root.dataset.inspector === 'open'));
  }

  toggleInspector() {
    this.root.dataset.inspector = this.root.dataset.inspector === 'open' ? 'closed' : 'open';
    this.updateToolbar();
  }

  section(key, open) {
    const body = el('div', { class: 'wmp-section-body' });
    const d = el('details', { class: 'wmp-section', 'data-section': key, open: open || undefined }, [el('summary', { text: this.t(`sec.${key}`) }), body]);
    this.inspector.append(d);
    return body;
  }

  buildInspector() {
    const t = this.t;
    const openState = {};
    for (const d of this.inspector.querySelectorAll('details[data-section]')) openState[d.dataset.section] = d.open;
    const isOpen = (k, def) => (k in openState ? openState[k] : def);
    this.inspector.replaceChildren();
    this.ins = {};

    // Path
    const path = this.section('path', isOpen('path', true));
    this.ins.summary = el('div', { class: 'wmp-note' });
    this.ins.problem = el('div', { class: 'wmp-error' });
    this.ins.selection = el('div', { class: 'wmp-section-body', style: 'padding:0' });
    const unitSel = el('select', { class: 'wmp-select', 'aria-label': t('view.units'), onchange: (e) => { this.settings.units = e.target.value; this.writeEditorSettings(); this.renderSelection(); } }, [
      el('option', { value: 'norm', text: t('units.norm') }), el('option', { value: 'px', text: t('units.px') })]);
    unitSel.value = this.settings.units;
    this.ins.units = unitSel;
    const check = (key, label, get, onchange) => {
      const cb = el('input', { type: 'checkbox', 'data-field': key, onchange: (e) => onchange(e.target.checked) });
      cb.checked = get();
      return { cb, row: el('label', { class: 'wmp-check' }, [cb, el('span', { text: label })]) };
    };
    const closed = check('closed', t('path.close'), () => !!this.doc.path.closed, () => this.toggleClosed());
    const gridCb = check('grid', t('view.grid'), () => this.settings.grid, (v) => { this.settings.grid = v; this.writeEditorSettings(); this.updateToolbar(); this.requestDraw(); });
    const snapCb = check('snap', t('view.snap'), () => this.settings.snap, (v) => { this.settings.snap = v; this.writeEditorSettings(); this.updateToolbar(); });
    this.ins.closed = closed.cb;
    this.ins.grid = gridCb.cb;
    this.ins.snap = snapCb.cb;
    this.ins.reverse = el('button', { class: 'wmp-btn', type: 'button', 'data-action': 'reverse-inspector', onclick: () => this.reversePath() }, [el('span', { html: ICONS.reverse }), el('span', { text: t('path.reverse') })]);
    path.append(this.ins.problem, this.ins.summary,
      el('div', { class: 'wmp-actions' }, [closed.row, gridCb.row, snapCb.row]),
      el('div', { class: 'wmp-actions' }, [this.ins.reverse]),
      el('div', { class: 'wmp-row' }, [el('span', { class: 'wmp-row-label', text: t('view.units') }), unitSel]), this.ins.selection);

    // Preview
    const prev = this.section('preview', isOpen('preview', true));
    const range = el('input', { class: 'wmp-range', type: 'range', min: '0', max: '1', step: '0.001', 'aria-label': t('preview.progress'), 'data-field': 'preview' });
    range.value = String(this.preview.progress);
    range.addEventListener('input', () => { this.preview.progress = Number(range.value); this.ins.previewNum.value = fmt(this.preview.progress, 3); this.requestDraw(); });
    const num = this.numberInput((v) => { this.preview.progress = v; range.value = String(v); this.requestDraw(); return true; }, t('preview.progress'));
    num.value = fmt(this.preview.progress, 3);
    this.ins.previewRange = range;
    this.ins.previewNum = num;
    prev.append(el('div', { class: 'wmp-row' }, [el('label', { text: t('preview.progress') }), num]), range, el('div', { class: 'wmp-note', text: t('preview.note') }));

    // Parameter groups
    this.ins.params = {};
    for (const group of ['timing', 'transform', 'orientation', 'blur']) {
      const body = this.section(group, isOpen(group, group === 'timing'));
      if (!this.caps.params) {
        body.append(el('div', { class: 'wmp-note', text: t('params.unavailable') }));
        continue;
      }
      for (const p of PARAMS.filter((q) => q.group === group)) body.append(this.paramRow(p));
    }

    // Presets
    const pre = this.section('presets', isOpen('presets', true));
    pre.append(el('div', { class: 'wmp-row-label', text: t('presets.builtin') }));
    const grid = el('div', { class: 'wmp-presets' });
    this.ins.presetButtons = [];
    for (const key of PRESET_KEYS) {
      const btn = el('button', { class: 'wmp-preset', type: 'button', 'data-preset': key, title: t(`preset.${key}`), onclick: () => this.loadBuiltinPreset(key) });
      btn.innerHTML = this.presetThumb(key);
      btn.append(el('span', { text: t(`preset.${key}`) }));
      this.ins.presetButtons.push(btn);
      grid.append(btn);
    }
    pre.append(grid, el('div', { class: 'wmp-row-label', text: t('presets.user'), style: 'margin-top:4px' }));
    if (!this.caps.presets) {
      pre.append(el('div', { class: 'wmp-note', text: t('presets.unavailable') }));
    } else {
      this.ins.presetList = el('select', { class: 'wmp-select', 'aria-label': t('presets.user'), 'data-field': 'preset-list' });
      this.ins.presetTiming = el('select', { class: 'wmp-select', 'aria-label': t('presets.timingMode'), 'data-field': 'preset-timing' }, [
        el('option', { value: 'fit', text: t('timing.fit') }), el('option', { value: 'preserve', text: t('timing.preserve') })]);
      this.ins.presetName = el('input', { class: 'wmp-input', type: 'text', placeholder: t('presets.name'), 'aria-label': t('presets.name'), 'data-field': 'preset-name' });
      this.ins.presetLevel = el('select', { class: 'wmp-select', 'aria-label': t('presets.level'), 'data-field': 'preset-level' }, [
        el('option', { value: 'geometry', text: t('level.geometry') }), el('option', { value: 'look', text: t('level.look') }), el('option', { value: 'timing', text: t('level.timing') })]);
      this.ins.presetNote = el('div', { class: 'wmp-note' });
      pre.append(
        el('div', { class: 'wmp-row' }, [el('label', { text: t('presets.user') }), this.ins.presetList]),
        el('div', { class: 'wmp-row' }, [el('label', { text: t('presets.timingMode') }), this.ins.presetTiming]),
        el('div', { class: 'wmp-actions' }, [
          el('button', { class: 'wmp-btn', type: 'button', 'data-action': 'preset-load', text: t('presets.load'), onclick: () => this.loadUserPreset() }),
          el('button', { class: 'wmp-btn', type: 'button', 'data-action': 'preset-delete', text: t('presets.delete'), onclick: () => this.deleteUserPreset() }),
        ]),
        el('div', { class: 'wmp-row' }, [el('label', { text: t('presets.name') }), this.ins.presetName]),
        el('div', { class: 'wmp-row' }, [el('label', { text: t('presets.level') }), this.ins.presetLevel]),
        el('div', { class: 'wmp-actions' }, [el('button', { class: 'wmp-btn', type: 'button', 'data-action': 'preset-save', text: t('presets.save'), onclick: () => this.saveUserPreset() })]),
        this.ins.presetNote,
      );
      this.fillPresetList();
    }
    this.renderSelection();
    this.updateInspector();
  }

  numberInput(onCommit, label, attrs = {}) {
    const input = el('input', { class: 'wmp-input', type: 'text', inputmode: 'decimal', 'aria-label': label, ...attrs });
    const commit = () => {
      const v = parseNumber(input.value);
      if (v === null) {
        input.setAttribute('aria-invalid', 'true');
        return;
      }
      input.removeAttribute('aria-invalid');
      onCommit(v);
    };
    input.addEventListener('change', commit);
    input.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') {
        commit();
        input.select();
      } else if (e.key === 'Escape') {
        input.removeAttribute('aria-invalid');
        input.blur();
      }
    });
    return input;
  }

  paramRow(p) {
    const t = this.t;
    const label = t(p.label);
    const row = el('div', { class: 'wmp-row', 'data-param': p.id });
    const refs = { row };
    if (p.type === 'bool') {
      const cb = el('input', { type: 'checkbox', 'data-param-input': p.id, onchange: (e) => this.setParam(p.id, e.target.checked) });
      refs.inputs = [cb];
      row.append(el('span', { class: 'wmp-row-label', text: label }), el('label', { class: 'wmp-check' }, [cb]));
    } else if (p.type === 'choice') {
      const sel = el('select', { class: 'wmp-select', 'aria-label': label, 'data-param-input': p.id, onchange: (e) => this.setParam(p.id, Number(e.target.value)) },
        p.options.map((o, i) => el('option', { value: String(i), text: t(o) })));
      refs.inputs = [sel];
      row.append(el('label', { text: label }), sel);
    } else if (p.type === 'double2') {
      const a = this.numberInput((v) => this.setParam(p.id, [v, this.values[p.id][1]]), `${label} X`, { 'data-param-input': `${p.id}.0` });
      const b = this.numberInput((v) => this.setParam(p.id, [this.values[p.id][0], v]), `${label} Y`, { 'data-param-input': `${p.id}.1` });
      refs.inputs = [a, b];
      row.append(el('label', { text: label }), el('div', { class: 'wmp-pair' }, [a, b]));
    } else {
      const inp = this.numberInput((v) => this.setParam(p.id, v), label, { 'data-param-input': p.id });
      refs.inputs = [inp];
      row.append(el('label', { text: label }), inp);
    }
    refs.badge = el('div', { class: 'wmp-badge', text: t('params.animated') });
    row.append(refs.badge);
    refs.badge.style.gridColumn = '1 / -1';
    this.ins.params[p.id] = refs;
    return row;
  }

  updateInspector() {
    if (!this.ins) return;
    const t = this.t;
    // Path summary / problems
    if (this.readOnly) {
      this.ins.problem.textContent = this.status === Status.Future ? t('path.future') : t('path.invalid', { msg: this.statusMessage });
      this.ins.summary.textContent = '';
    } else {
      this.ins.problem.textContent = '';
      const n = this.doc.path.points.length;
      const g = this.getGeometry();
      this.ins.summary.textContent = n === 0 ? t('path.empty')
        : t('path.summary', { n, kind: this.doc.path.closed ? t('path.kindClosed') : t('path.kindOpen'), len: fmt(g ? g.length : 0, 3) });
    }
    for (const btn of this.ins.presetButtons || []) btn.disabled = this.readOnly;
    if (this.ins.closed) {
      this.ins.closed.checked = !!this.doc.path.closed;
      this.ins.closed.disabled = this.readOnly || this.doc.path.points.length < 2;
      this.ins.reverse.disabled = this.readOnly || this.doc.path.points.length < 2;
      this.ins.grid.checked = this.settings.grid;
      this.ins.snap.checked = this.settings.snap;
    }
    // Parameters
    for (const [id, refs] of Object.entries(this.ins.params || {})) {
      const p = paramById(id);
      const animated = this.animated.has(id);
      let enabled = isEnabled(p, this.values) && !animated;
      if (id === 'wmpScale' && this.values.wmpUniformScale) refs.inputs[1].disabled = true;
      refs.badge.hidden = !animated;
      const v = this.values[id];
      refs.inputs.forEach((inp, i) => {
        if (document.activeElement !== inp) {
          if (p.type === 'bool') inp.checked = !!v;
          else if (p.type === 'choice') inp.value = String(v);
          else if (p.type === 'double2') inp.value = fmt(v[i], p.digits ?? 4);
          else inp.value = fmt(v, p.digits ?? 0);
        }
        inp.disabled = !enabled || (id === 'wmpScale' && i === 1 && this.values.wmpUniformScale);
      });
    }
    if (this.ins.presetLevel) {
      const timingOk = this.caps.keyframes && this.context?.eventDuration > 0;
      this.ins.presetLevel.querySelector('option[value="timing"]').disabled = !timingOk;
      this.ins.presetLevel.querySelector('option[value="look"]').disabled = !this.caps.params;
    }
  }

  renderSelection() {
    if (!this.ins) return;
    const t = this.t;
    const box = this.ins.selection;
    box.replaceChildren();
    const sel = this.getSelection();
    if (this.readOnly) return;
    if (sel.length === 0) {
      box.append(el('div', { class: 'wmp-note', text: t('sel.none') }));
      return;
    }
    if (sel.length > 1) {
      box.append(el('div', { class: 'wmp-note', text: t('sel.many', { n: sel.length }) }));
      box.append(el('div', { class: 'wmp-actions' }, [el('button', { class: 'wmp-btn', type: 'button', 'data-action': 'delete-selected', onclick: () => this.deleteSelected() }, [el('span', { html: ICONS.trash }), el('span', { text: t('point.delete') })])]));
      return;
    }
    const i = sel[0];
    const pt = this.doc.path.points[i];
    const px = this.settings.units === 'px';
    const sx = px ? this.frame.width : 1, sy = px ? this.frame.height : 1;
    const dg = px ? 2 : 4;
    const fx = this.numberInput((v) => this.commit('Move point', (path) => { path.points[i].p.x = v / sx; }), `${t('point.x')}`, { 'data-field': 'point-x' });
    const fy = this.numberInput((v) => this.commit('Move point', (path) => { path.points[i].p.y = v / sy; }), `${t('point.y')}`, { 'data-field': 'point-y' });
    fx.value = fmt(pt.p.x * sx, dg);
    fy.value = fmt(pt.p.y * sy, dg);
    const handle = (key) => {
      const hx = this.numberInput((v) => this.commit('Edit handle', (path) => {
        const q = path.points[i];
        E.moveHandle(path, i, key === 'out', { x: q.p.x + v / sx, y: q.p.y + q[key].y }, this.aspect, false);
      }), `${t(`point.${key}`)} X`, { 'data-field': `handle-${key}-x` });
      const hy = this.numberInput((v) => this.commit('Edit handle', (path) => {
        const q = path.points[i];
        E.moveHandle(path, i, key === 'out', { x: q.p.x + q[key].x, y: q.p.y + v / sy }, this.aspect, false);
      }), `${t(`point.${key}`)} Y`, { 'data-field': `handle-${key}-y` });
      hx.value = fmt(pt[key].x * sx, dg);
      hy.value = fmt(pt[key].y * sy, dg);
      return el('div', { class: 'wmp-row' }, [el('label', { text: t(`point.${key}`) }), el('div', { class: 'wmp-pair' }, [hx, hy])]);
    };
    const modes = el('div', { class: 'wmp-seg', role: 'group', 'aria-label': t('point.mode') }, ['corner', 'smooth', 'free'].map((m) => {
      const b = el('button', { class: 'wmp-btn', type: 'button', title: t(`mode.${m}`), 'aria-label': t(`mode.${m}`), 'aria-pressed': String(pt.mode === m), 'data-mode': m, onclick: () => this.setModeSelected(m) });
      b.innerHTML = ICONS[m];
      return b;
    }));
    box.append(
      el('div', { class: 'wmp-row' }, [el('label', { text: `${t('point.x')} / ${t('point.y')}` }), el('div', { class: 'wmp-pair' }, [fx, fy])]),
      el('div', { class: 'wmp-row' }, [el('span', { class: 'wmp-row-label', text: t('point.mode') }), modes]),
      handle('in'),
      handle('out'),
      el('div', { class: 'wmp-actions' }, [el('button', { class: 'wmp-btn', type: 'button', 'data-action': 'delete-selected', onclick: () => this.deleteSelected() }, [el('span', { html: ICONS.trash }), el('span', { text: t('point.delete') })])]),
    );
  }

  selectionChanged() {
    this.renderSelection();
    this.updateInspector();
    this.updateToolbar();
    this.updateStatus();
    this.requestDraw();
    this.emit('selection', this.getSelection());
  }

  updateStatus() {
    if (!this.statusEl) return;
    const t = this.t;
    if (this.readOnly) this.statusEl.textContent = t('hint.readonly');
    else this.statusEl.textContent = t(this.tool === 'add' ? 'hint.add' : this.tool === 'delete' ? 'hint.delete' : 'hint.select');
  }

  toast(text, kind = 'info') {
    if (!this.toastEl) return;
    this.toastEl.textContent = text;
    this.toastEl.dataset.kind = kind;
    this.toastEl.dataset.show = 'true';
    clearTimeout(this.toastTimer);
    this.toastTimer = setTimeout(() => { this.toastEl.dataset.show = 'false'; }, kind === 'error' ? 6000 : 2200);
    if (kind === 'error') this.emit('error', text);
  }

  presetThumb(key) {
    const p = makePreset(key, 16 / 9);
    const W = 52, H = 30;
    const tx = (n) => `${(n.x * W).toFixed(2)} ${(n.y * H).toFixed(2)}`;
    let d = '';
    const n = p.points.length;
    const segs = p.closed ? n : n - 1;
    for (let i = 0; i < segs; i += 1) {
      const a = p.points[i], b = p.points[(i + 1) % n];
      if (i === 0) d += `M${tx(a.p)}`;
      d += ` C${tx({ x: a.p.x + a.out.x, y: a.p.y + a.out.y })} ${tx({ x: b.p.x + b.in.x, y: b.p.y + b.in.y })} ${tx(b.p)}`;
    }
    return `<svg viewBox="0 0 ${W} ${H}" aria-hidden="true"><rect x="0.5" y="0.5" width="${W - 1}" height="${H - 1}" rx="2" fill="none" stroke="currentColor" stroke-opacity=".25"/><path d="${d}" fill="none" stroke="var(--wmp-accent)" stroke-width="1.6"/></svg>`;
  }

  // ------------------------------------------------------------ path ops
  setModeSelected(mode) {
    const sel = this.getSelection();
    if (!sel.length) return;
    this.commit(this.t(`mode.${mode}`), (path) => { for (const i of sel) E.setMode(path, i, mode, this.aspect); });
  }

  toggleClosed() {
    if (this.doc.path.points.length < 2) return;
    this.commit(this.t('path.close'), (path) => { path.closed = !path.closed; });
  }

  reversePath() {
    const n = this.doc.path.points.length;
    if (n < 2) return;
    const closed = this.doc.path.closed;
    this.commit(this.t('path.reverse'), (path) => { const r = reversed(path); path.points = r.points; });
    // Keep the same points selected after reordering.
    this.selection = new Set([...this.selection].map((i) => (closed ? (i === 0 ? 0 : n - i) : n - 1 - i)));
    this.selectionChanged();
  }

  deleteSelected() {
    const sel = this.getSelection();
    if (!sel.length) return;
    this.commit(this.t('point.delete'), (path) => { for (const i of sel.slice().reverse()) E.deletePoint(path, i); });
    this.selection.clear();
    this.selectionChanged();
  }

  loadBuiltinPreset(key) {
    if (this.readOnly) return;
    this.commit(this.t(`preset.${key}`), (path) => { const p = makePreset(key, this.aspect); path.points = p.points; path.closed = p.closed; });
    this.selection.clear();
    this.selectionChanged();
    this.fitFrame();
  }

  async refreshPresetList() {
    this.presetNames = this.caps.presets ? await this.host.listPresets() : [];
    this.fillPresetList();
  }

  fillPresetList() {
    if (!this.ins?.presetList) return;
    const names = this.presetNames || [];
    this.ins.presetList.replaceChildren(...(names.length ? names.map((n) => el('option', { value: n, text: n })) : [el('option', { value: '', text: this.t('presets.none') })]));
  }

  async saveUserPreset() {
    const name = this.ins.presetName.value.trim();
    if (!name) {
      this.ins.presetNote.textContent = this.t('presets.nameRequired');
      return;
    }
    const level = this.ins.presetLevel.value;
    const preset = buildPreset({ name, level, path: this.doc.path, aspect: this.aspect, values: this.values, keyframes: this.keyframes, eventDuration: this.context?.eventDuration });
    await this.host.savePreset(name, preset);
    await this.refreshPresetList();
    this.ins.presetList.value = name;
    this.ins.presetNote.textContent = this.t('presets.saved', { name });
  }

  async loadUserPreset() {
    const name = this.ins.presetList.value;
    if (!name || this.readOnly) return;
    let preset;
    try {
      preset = parsePreset(await this.host.loadPreset(name));
    } catch (e) {
      this.ins.presetNote.textContent = e.message;
      return;
    }
    const plan = planApply(preset, { targetDuration: this.context?.eventDuration ?? null, timingMode: this.ins.presetTiming.value, animatedIds: [...this.animated] });
    if (plan.error) {
      this.ins.presetNote.textContent = this.t('presets.timingUnavailable');
      return;
    }
    if (Object.keys(plan.keyframeWrites).length && !this.caps.keyframes) {
      this.ins.presetNote.textContent = this.t('presets.timingUnavailable');
      return;
    }
    this.commit(name, (path) => { path.points = plan.path.points; path.closed = plan.path.closed; });
    for (const [id, v] of Object.entries(plan.paramWrites)) await this.setParam(id, v);
    for (const [id, keys] of Object.entries(plan.keyframeWrites)) {
      await this.host.writeKeyframes(id, keys, name);
      this.keyframes[id] = keys;
      this.animated.add(id);
    }
    const msgs = [this.t('presets.loaded', { name })];
    if (plan.skipped.length) msgs.push(this.t('presets.skipped', { list: plan.skipped.map((id) => this.t(paramById(id).label)).join(', ') }));
    this.ins.presetNote.textContent = msgs.join(' ');
    this.updateInspector();
    this.fitFrame();
  }

  async deleteUserPreset() {
    const name = this.ins.presetList.value;
    if (!name) return;
    await this.host.deletePreset(name);
    await this.refreshPresetList();
  }

  // ------------------------------------------------------------- geometry
  getGeometry() {
    const key = `${this.aspect}|${JSON.stringify(this.doc.path)}`;
    if (key !== this.geometryKey) {
      this.geometry = this.doc.path.points.length ? new PathGeometry(this.doc.path, { aspect: this.aspect, tolerance: 1e-7 }) : null;
      this.geometryKey = key;
    }
    return this.geometry;
  }

  poseSettings() {
    const v = this.values;
    return {
      progress: this.preview.progress,
      startOffset: v.wmpStartOffset,
      reverse: v.wmpReverse,
      endBehavior: v.wmpEndBehavior,
      speedMode: v.wmpSpeedMode === 1 ? SpeedMode.EqualTimePerSegment : SpeedMode.ConstantSpeed,
      easing: easingFor(v.wmpEasing, { x1: v.wmpEaseP1[0], y1: v.wmpEaseP1[1], x2: v.wmpEaseP2[0], y2: v.wmpEaseP2[1] }),
      orientMode: v.wmpOrientMode,
      orientSmoothing: v.wmpOrient ? v.wmpOrientSmooth / 100 : 0,
    };
  }

  // ------------------------------------------------------------- viewport
  toScreen(n) {
    return { x: this.view.ox + n.x * this.aspect * this.view.s, y: this.view.oy + n.y * this.view.s };
  }
  toNorm(s) {
    return { x: (s.x - this.view.ox) / (this.aspect * this.view.s), y: (s.y - this.view.oy) / this.view.s };
  }
  stageSize() {
    const r = this.stage.getBoundingClientRect();
    return { w: Math.max(1, r.width), h: Math.max(1, r.height) };
  }
  fitRect(x1, y1, x2, y2, margin = 24) {
    const { w, h } = this.stageSize();
    const A = this.aspect;
    const dw = Math.max((x2 - x1) * A, 1e-3), dh = Math.max(y2 - y1, 1e-3);
    const m = Math.min(margin, w / 6, h / 6);
    const s = Math.max(1e-3, Math.min((w - 2 * m) / dw, (h - 2 * m) / dh));
    this.view.s = s;
    this.view.ox = w / 2 - ((x1 + x2) / 2) * A * s;
    this.view.oy = h / 2 - ((y1 + y2) / 2) * s;
    this.updateToolbar();
    this.requestDraw();
  }
  fitFrame() {
    this.view.fitted = 'frame';
    this.fitRect(0, 0, 1, 1);
  }
  fitPath() {
    const pts = this.doc.path.points;
    if (!pts.length) return this.fitFrame();
    let x1 = Infinity, y1 = Infinity, x2 = -Infinity, y2 = -Infinity;
    for (const p of pts) for (const q of [p.p, { x: p.p.x + p.in.x, y: p.p.y + p.in.y }, { x: p.p.x + p.out.x, y: p.p.y + p.out.y }]) {
      x1 = Math.min(x1, q.x); y1 = Math.min(y1, q.y); x2 = Math.max(x2, q.x); y2 = Math.max(y2, q.y);
    }
    this.view.fitted = 'path';
    this.fitRect(x1, y1, x2, y2, 40);
  }
  zoomBy(f, at) {
    const { w, h } = this.stageSize();
    const c = at || { x: w / 2, y: h / 2 };
    const n = this.toNorm(c);
    this.view.s = Math.min(Math.max(this.view.s * f, 5), 1e5);
    this.view.ox = c.x - n.x * this.aspect * this.view.s;
    this.view.oy = c.y - n.y * this.view.s;
    this.view.fitted = null;
    this.updateToolbar();
    this.requestDraw();
  }

  // --------------------------------------------------------------- events
  bindEvents() {
    this.resizeObserver = new ResizeObserver(() => this.onResize());
    this.resizeObserver.observe(this.root);
    this.canvas.addEventListener('pointerdown', (e) => this.onPointerDown(e));
    this.canvas.addEventListener('pointermove', (e) => this.onPointerMove(e));
    this.canvas.addEventListener('pointerup', (e) => this.onPointerUp(e));
    this.canvas.addEventListener('pointercancel', () => this.cancelDrag());
    this.canvas.addEventListener('dblclick', (e) => this.onDoubleClick(e));
    this.canvas.addEventListener('contextmenu', (e) => e.preventDefault());
    this.canvas.addEventListener('wheel', (e) => this.onWheel(e), { passive: false });
    this.root.addEventListener('keydown', (e) => this.onKeyDown(e));
    this.onWindowKeyUp = () => {};
    window.addEventListener('keyup', this.onWindowKeyUp);
  }

  onResize() {
    const r = this.root.getBoundingClientRect();
    const size = [];
    if (r.width < 600) size.push('narrow');
    if (r.width < 420) size.push('tiny');
    if (r.height < 340) size.push('short');
    if (r.height < 220) size.push('micro');
    this.root.dataset.size = size.join(' ') || 'wide';
    if (this.view.fitted === 'frame') this.fitFrame();
    else if (this.view.fitted === 'path') this.fitPath();
    this.requestDraw();
  }

  localPoint(e) {
    const r = this.canvas.getBoundingClientRect();
    return { x: e.clientX - r.left, y: e.clientY - r.top };
  }

  hitAt(sp) {
    const n = this.toNorm(sp);
    const tolD = HIT_PX / this.view.s;
    return E.hitTest(this.doc.path, this.aspect, n, tolD, this.getSelection());
  }

  // Transform box of a multi-selection, in screen space.
  transformBox() {
    const sel = this.getSelection();
    if (sel.length < 2 || this.readOnly) return null;
    let x1 = Infinity, y1 = Infinity, x2 = -Infinity, y2 = -Infinity;
    for (const i of sel) {
      const s = this.toScreen(this.doc.path.points[i].p);
      x1 = Math.min(x1, s.x); y1 = Math.min(y1, s.y); x2 = Math.max(x2, s.x); y2 = Math.max(y2, s.y);
    }
    const pad = 10;
    x1 -= pad; y1 -= pad; x2 += pad; y2 += pad;
    return { x1, y1, x2, y2, cx: (x1 + x2) / 2, cy: (y1 + y2) / 2, knob: { x: (x1 + x2) / 2, y: y1 - 22 },
      corners: [{ x: x1, y: y1 }, { x: x2, y: y1 }, { x: x2, y: y2 }, { x: x1, y: y2 }] };
  }

  snap(n, e) {
    this.snapGuides = [];
    if (!this.settings.snap || e.ctrlKey || e.metaKey) return n;
    const A = this.aspect, s = this.view.s;
    const th = SNAP_PX / s; // frame heights
    let xD = n.x * A, y = n.y;
    const candX = [Math.round(xD / GRID_STEP) * GRID_STEP, 0, A / 2, A];
    const candY = [Math.round(y / GRID_STEP) * GRID_STEP, 0, 0.5, 1];
    for (const p of this.doc.path.points) {
      if (this.drag?.indices?.includes(this.doc.path.points.indexOf(p))) continue;
      candX.push(p.p.x * A);
      candY.push(p.p.y);
    }
    let bx = null, by = null;
    for (const c of candX) if (Math.abs(c - xD) <= th && (bx === null || Math.abs(c - xD) < Math.abs(bx - xD))) bx = c;
    for (const c of candY) if (Math.abs(c - y) <= th && (by === null || Math.abs(c - y) < Math.abs(by - y))) by = c;
    if (bx !== null) { xD = bx; this.snapGuides.push({ x: bx / A }); }
    if (by !== null) { y = by; this.snapGuides.push({ y: by }); }
    return { x: xD / A, y };
  }

  onPointerDown(e) {
    this.root.focus({ preventScroll: true });
    const sp = this.localPoint(e);
    if (e.button === 1 || e.button === 2) {
      this.drag = { kind: 'pan', start: sp, ox: this.view.ox, oy: this.view.oy };
      this.canvas.setPointerCapture(e.pointerId);
      return;
    }
    if (e.button !== 0 || this.readOnly) return;
    this.canvas.setPointerCapture(e.pointerId);
    const n = this.toNorm(sp);
    const path = this.doc.path;
    if (this.tool === 'delete') {
      const hit = this.hitAt(sp);
      if (hit.kind === E.HitKind.Anchor) {
        this.commit(this.t('tool.delete'), (p) => E.deletePoint(p, hit.index));
        this.selection.clear();
        this.selectionChanged();
      }
      return;
    }
    if (this.tool === 'add') {
      const hit = this.hitAt(sp);
      if (hit.kind === E.HitKind.Segment) {
        let idx = -1;
        this.commit(this.t('tool.add'), (p) => { idx = E.insertPoint(p, hit.index, hit.t); });
        this.selection = new Set([idx]);
        this.selectionChanged();
        return;
      }
      if (hit.kind === E.HitKind.Anchor) {
        const sel = this.getSelection();
        const last = path.points.length - 1;
        if (!path.closed && hit.index === 0 && sel.length === 1 && sel[0] === last && last >= 1) {
          this.commit(this.t('path.close'), (p) => { p.closed = true; });
        } else {
          this.selection = new Set([hit.index]);
          this.selectionChanged();
        }
        return;
      }
      // Pen: append a point (after the selected open end, else at the end); drag pulls handles.
      const start = clonePath(path);
      const sel = this.getSelection();
      const atStart = !path.closed && sel.length === 1 && sel[0] === 0 && path.points.length > 1;
      const p0 = this.snap(n, e);
      const pt = { id: E.nextPointId(path), p: p0, in: { x: 0, y: 0 }, out: { x: 0, y: 0 }, mode: 'corner', extra: {} };
      let idx;
      if (atStart) { path.points.unshift(pt); idx = 0; }
      else { path.points.push(pt); idx = path.points.length - 1; }
      this.selection = new Set([idx]);
      this.drag = { kind: 'pen', start, index: idx, anchor: p0, atStart, moved: false };
      this.geometryKey = '';
      this.requestDraw();
      return;
    }
    // Select tool
    const box = this.transformBox();
    if (box) {
      const near = (a, b) => Math.hypot(a.x - b.x, a.y - b.y) <= HIT_PX + 2;
      if (near(sp, box.knob)) return this.beginTransform('rotate', sp, box, e);
      const ci = box.corners.findIndex((c) => near(sp, c));
      if (ci >= 0) return this.beginTransform('scale', sp, box, e, box.corners[(ci + 2) % 4]);
    }
    const hit = this.hitAt(sp);
    if (hit.kind === E.HitKind.Anchor) {
      if (e.shiftKey) {
        if (this.selection.has(hit.index)) this.selection.delete(hit.index);
        else this.selection.add(hit.index);
      } else if (!this.selection.has(hit.index)) {
        this.selection = new Set([hit.index]);
      }
      this.selectionChanged();
      if (!this.selection.has(hit.index)) return;
      const indices = this.getSelection();
      this.drag = { kind: 'move', start: clonePath(path), startN: n, grab: hit.index, indices, moved: false };
      return;
    }
    if (hit.kind === E.HitKind.InHandle || hit.kind === E.HitKind.OutHandle) {
      this.drag = { kind: 'handle', start: clonePath(path), index: hit.index, out: hit.kind === E.HitKind.OutHandle, moved: false };
      return;
    }
    if (box && sp.x >= box.x1 && sp.x <= box.x2 && sp.y >= box.y1 && sp.y <= box.y2) {
      this.drag = { kind: 'move', start: clonePath(path), startN: n, grab: -1, indices: this.getSelection(), moved: false };
      return;
    }
    this.drag = { kind: 'box', start: sp, now: sp, add: e.shiftKey, base: e.shiftKey ? new Set(this.selection) : new Set() };
  }

  beginTransform(mode, sp, box, e, pivotScreen) {
    const pivot = mode === 'rotate' || e.altKey ? { x: box.cx, y: box.cy } : pivotScreen;
    this.drag = { kind: mode, start: clonePath(this.doc.path), startSp: sp, pivotSp: pivot, pivotN: this.toNorm(pivot), indices: this.getSelection(), moved: false };
  }

  onPointerMove(e) {
    const sp = this.localPoint(e);
    const d = this.drag;
    if (!d) {
      const prev = this.hover;
      this.hover = this.readOnly ? null : this.hitAt(sp);
      const box = this.transformBox();
      let cursor = 'default';
      if (this.tool === 'add') cursor = 'crosshair';
      else if (this.hover && this.hover.kind !== E.HitKind.None && this.hover.kind !== E.HitKind.Segment) cursor = this.tool === 'delete' ? 'not-allowed' : 'move';
      if (box && Math.hypot(sp.x - box.knob.x, sp.y - box.knob.y) <= HIT_PX + 2) cursor = 'grab';
      this.canvas.style.cursor = cursor;
      if (JSON.stringify(prev) !== JSON.stringify(this.hover)) this.requestDraw();
      return;
    }
    if (d.kind === 'pan') {
      this.view.ox = d.ox + (sp.x - d.start.x);
      this.view.oy = d.oy + (sp.y - d.start.y);
      this.view.fitted = null;
      this.requestDraw();
      return;
    }
    if (d.kind === 'box') {
      d.now = sp;
      const x1 = Math.min(d.start.x, sp.x), x2 = Math.max(d.start.x, sp.x), y1 = Math.min(d.start.y, sp.y), y2 = Math.max(d.start.y, sp.y);
      const sel = new Set(d.base);
      this.doc.path.points.forEach((p, i) => {
        const s = this.toScreen(p.p);
        if (s.x >= x1 && s.x <= x2 && s.y >= y1 && s.y <= y2) sel.add(i);
      });
      this.selection = sel;
      this.requestDraw();
      return;
    }
    const n = this.toNorm(sp);
    const path = clonePath(d.start);
    if (d.kind === 'move') {
      let delta = { x: n.x - d.startN.x, y: n.y - d.startN.y };
      if (e.shiftKey) {
        if (Math.abs(delta.x * this.aspect) > Math.abs(delta.y)) delta.y = 0;
        else delta.x = 0;
      }
      if (d.grab >= 0) {
        const target = this.snap({ x: d.start.points[d.grab].p.x + delta.x, y: d.start.points[d.grab].p.y + delta.y }, e);
        delta = { x: target.x - d.start.points[d.grab].p.x, y: target.y - d.start.points[d.grab].p.y };
      }
      for (const i of d.indices) E.moveAnchor(path, i, { x: d.start.points[i].p.x + delta.x, y: d.start.points[i].p.y + delta.y });
    } else if (d.kind === 'handle') {
      E.moveHandle(path, d.index, d.out, n, this.aspect, e.altKey);
    } else if (d.kind === 'pen') {
      const pt = path.points[d.index];
      const dx = n.x - d.anchor.x, dy = n.y - d.anchor.y;
      const far = Math.hypot(dx * this.aspect, dy) * this.view.s > 3;
      if (far) {
        // The drag direction is the travel direction at this point.
        const dir = d.atStart ? -1 : 1;
        pt.out = { x: dx * dir, y: dy * dir };
        pt.in = { x: -dx * dir, y: -dy * dir };
        pt.mode = 'smooth';
      }
      d.moved = d.moved || far;
    } else if (d.kind === 'scale' || d.kind === 'rotate') {
      const a = { x: d.startSp.x - d.pivotSp.x, y: d.startSp.y - d.pivotSp.y };
      const b = { x: sp.x - d.pivotSp.x, y: sp.y - d.pivotSp.y };
      if (d.kind === 'scale') {
        const f = Math.hypot(b.x, b.y) / Math.max(1e-6, Math.hypot(a.x, a.y));
        E.transformPoints(path, this.aspect, d.pivotN, f, 0, { x: 0, y: 0 }, d.indices);
      } else {
        let ang = ((Math.atan2(b.y, b.x) - Math.atan2(a.y, a.x)) * 180) / Math.PI;
        if (e.shiftKey) ang = Math.round(ang / 15) * 15;
        E.transformPoints(path, this.aspect, d.pivotN, 1, ang, { x: 0, y: 0 }, d.indices);
      }
    }
    if (d.kind !== 'pen') d.moved = true;
    this.doc.path = path;
    this.geometryKey = '';
    this.requestDraw();
  }

  onPointerUp(e) {
    const d = this.drag;
    this.drag = null;
    this.snapGuides = [];
    if (this.canvas.hasPointerCapture?.(e.pointerId)) this.canvas.releasePointerCapture(e.pointerId);
    if (!d) return;
    if (d.kind === 'pan') return;
    if (d.kind === 'box') {
      this.selectionChanged();
      return;
    }
    const labels = { move: 'Move points', handle: 'Edit handle', pen: this.t('tool.add'), scale: 'Scale points', rotate: 'Rotate points' };
    const changed = d.kind === 'pen' || (d.moved && !samePath(d.start, this.doc.path));
    if (changed) {
      this.history.push(labels[d.kind], d.start, clonePath(this.doc.path));
      this.afterEdit(labels[d.kind]);
    } else {
      this.doc.path = d.start;
      this.selectionChanged();
    }
  }

  cancelDrag() {
    if (this.drag && this.drag.start && this.drag.kind !== 'pan' && this.drag.kind !== 'box') this.doc.path = this.drag.start;
    this.drag = null;
    this.snapGuides = [];
    this.geometryKey = '';
    this.selectionChanged();
  }

  onDoubleClick(e) {
    if (this.readOnly || this.tool !== 'select') return;
    const hit = this.hitAt(this.localPoint(e));
    if (hit.kind === E.HitKind.Segment) {
      let idx = -1;
      this.commit(this.t('tool.add'), (p) => { idx = E.insertPoint(p, hit.index, hit.t); });
      this.selection = new Set([idx]);
      this.selectionChanged();
    } else if (hit.kind === E.HitKind.Anchor) {
      const m = this.doc.path.points[hit.index].mode === 'corner' ? 'smooth' : 'corner';
      this.selection = new Set([hit.index]);
      this.setModeSelected(m);
    }
  }

  onWheel(e) {
    e.preventDefault();
    const sp = this.localPoint(e);
    if (e.shiftKey && !e.ctrlKey) {
      this.view.ox -= e.deltaY || e.deltaX;
      this.view.fitted = null;
      this.requestDraw();
      return;
    }
    if (Math.abs(e.deltaX) > Math.abs(e.deltaY) && !e.ctrlKey) {
      this.view.ox -= e.deltaX;
      this.view.fitted = null;
      this.requestDraw();
      return;
    }
    const unit = e.deltaMode === 1 ? 16 : e.deltaMode === 2 ? 400 : 1;
    this.zoomBy(Math.exp(-e.deltaY * unit * 0.0015), sp);
  }

  onKeyDown(e) {
    const tag = e.target?.tagName;
    if (tag === 'INPUT' || tag === 'SELECT' || tag === 'TEXTAREA' || e.target?.isContentEditable) return;
    const mod = e.ctrlKey || e.metaKey;
    const k = e.key;
    let handled = true;
    if (mod && (k === 'z' || k === 'Z') && !e.shiftKey) this.undo();
    else if (mod && ((k === 'z' || k === 'Z') && e.shiftKey || k === 'y' || k === 'Y')) this.redo();
    else if (mod && (k === 'a' || k === 'A')) this.selectPoints(this.doc.path.points.map((_, i) => i));
    else if (mod) handled = false;
    else if (k === 'Delete' || k === 'Backspace') {
      if (this.selection.size) this.deleteSelected();
      else handled = false;
    } else if (k === 'Escape') {
      if (this.drag) this.cancelDrag();
      else if (this.selection.size) this.selectPoints([]);
      else handled = false;
    } else if (k.startsWith('Arrow') && this.selection.size && !this.readOnly) {
      const step = (e.shiftKey ? 10 : 1) / this.view.s;
      const dx = k === 'ArrowLeft' ? -step / this.aspect : k === 'ArrowRight' ? step / this.aspect : 0;
      const dy = k === 'ArrowUp' ? -step : k === 'ArrowDown' ? step : 0;
      const sel = this.getSelection();
      this.commit('Nudge', (p) => { for (const i of sel) p.points[i].p = { x: p.points[i].p.x + dx, y: p.points[i].p.y + dy }; }, 'nudge');
    } else if (k === '1') this.setModeSelected('corner');
    else if (k === '2') this.setModeSelected('smooth');
    else if (k === '3') this.setModeSelected('free');
    else if (k === 'v' || k === 'V') this.setTool('select');
    else if (k === 'p' || k === 'P') this.setTool('add');
    else if (k === 'x' || k === 'X') this.setTool('delete');
    else if (k === 'f' || k === 'F') (e.shiftKey ? this.fitFrame() : this.fitPath());
    else if (k === 'Home') this.fitFrame();
    else if (k === 'g' || k === 'G') this.tb.grid.click();
    else if (k === 'c' || k === 'C') this.toggleClosed();
    else if (k === '+' || k === '=') this.zoomBy(1.25);
    else if (k === '-' || k === '_') this.zoomBy(1 / 1.25);
    else handled = false; // leave everything else (Space, J/K/L, ...) to the host
    if (handled) {
      e.preventDefault();
      e.stopPropagation();
    }
  }

  // ---------------------------------------------------------------- draw
  requestDraw() {
    if (this.drawPending || this.destroyed) return;
    this.drawPending = true;
    requestAnimationFrame(() => {
      this.drawPending = false;
      this.draw();
    });
  }

  readColors() {
    const cs = getComputedStyle(this.root);
    const g = (n) => cs.getPropertyValue(n).trim();
    this.colors = {
      stage: g('--wmp-stage'), frame: g('--wmp-frame'), frameEdge: g('--wmp-frame-edge'), path: g('--wmp-path'),
      outline: g('--wmp-path-outline'), anchor: g('--wmp-anchor'), selected: g('--wmp-selected'), marker: g('--wmp-marker'),
      grid: g('--wmp-grid'), gridMajor: g('--wmp-grid-major'), accent: g('--wmp-accent'), text: g('--wmp-text-dim'),
    };
  }

  draw() {
    if (this.destroyed) return;
    if (!this.colors) this.readColors();
    const C = this.colors;
    const { w, h } = this.stageSize();
    const dpr = window.devicePixelRatio || 1;
    if (this.canvas.width !== Math.round(w * dpr) || this.canvas.height !== Math.round(h * dpr)) {
      this.canvas.width = Math.round(w * dpr);
      this.canvas.height = Math.round(h * dpr);
    }
    const g = this.canvas.getContext('2d');
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.fillStyle = C.stage;
    g.fillRect(0, 0, w, h);
    // Frame
    const f0 = this.toScreen({ x: 0, y: 0 }), f1 = this.toScreen({ x: 1, y: 1 });
    g.fillStyle = C.frame;
    g.fillRect(f0.x, f0.y, f1.x - f0.x, f1.y - f0.y);
    if (this.frameImage) {
      try { g.drawImage(this.frameImage, f0.x, f0.y, f1.x - f0.x, f1.y - f0.y); } catch { /* image not ready */ }
    }
    if (this.settings.grid) this.drawGrid(g, f0, f1, C);
    g.strokeStyle = C.frameEdge;
    g.lineWidth = 1;
    g.strokeRect(Math.round(f0.x) + 0.5, Math.round(f0.y) + 0.5, Math.round(f1.x - f0.x), Math.round(f1.y - f0.y));

    const path = this.doc.path;
    const geom = this.getGeometry();
    this.drawGhost(g, geom, C);
    this.drawPath(g, path, geom, C);
    this.drawMarker(g, geom, C);
    // Snap guides
    if (this.snapGuides.length) {
      g.save();
      g.strokeStyle = C.marker;
      g.setLineDash([4, 3]);
      for (const sg of this.snapGuides) {
        g.beginPath();
        if ('x' in sg) { const x = this.toScreen({ x: sg.x, y: 0 }).x; g.moveTo(x, 0); g.lineTo(x, h); }
        else { const y = this.toScreen({ x: 0, y: sg.y }).y; g.moveTo(0, y); g.lineTo(w, y); }
        g.stroke();
      }
      g.restore();
    }
    // Box selection
    if (this.drag?.kind === 'box') {
      const d = this.drag;
      g.save();
      g.fillStyle = 'rgba(61,184,255,0.12)';
      g.strokeStyle = C.accent;
      g.setLineDash([4, 3]);
      const x = Math.min(d.start.x, d.now.x), y = Math.min(d.start.y, d.now.y);
      g.fillRect(x, y, Math.abs(d.now.x - d.start.x), Math.abs(d.now.y - d.start.y));
      g.strokeRect(x + 0.5, y + 0.5, Math.abs(d.now.x - d.start.x), Math.abs(d.now.y - d.start.y));
      g.restore();
    }
    const box = !this.drag || this.drag.kind === 'scale' || this.drag.kind === 'rotate' ? this.transformBox() : null;
    if (box && this.tool === 'select') this.drawTransformBox(g, box, C);
    this.updateToolbar();
  }

  drawGrid(g, f0, f1, C) {
    const A = this.aspect, s = this.view.s;
    let step = GRID_STEP;
    while (step * s < 8) step *= 2;
    g.save();
    g.beginPath();
    g.rect(f0.x, f0.y, f1.x - f0.x, f1.y - f0.y);
    g.clip();
    g.lineWidth = 1;
    for (let pass = 0; pass < 2; pass += 1) {
      g.beginPath();
      g.strokeStyle = pass === 0 ? C.grid : C.gridMajor;
      for (let xD = 0; xD <= A + 1e-9; xD += step) {
        const major = Math.abs(xD - A / 2) < 1e-9;
        if ((pass === 1) !== major) continue;
        const x = Math.round(this.view.ox + xD * s) + 0.5;
        g.moveTo(x, f0.y);
        g.lineTo(x, f1.y);
      }
      for (let y = 0; y <= 1 + 1e-9; y += step) {
        const major = Math.abs(y - 0.5) < 1e-9;
        if ((pass === 1) !== major) continue;
        const sy = Math.round(this.view.oy + y * s) + 0.5;
        g.moveTo(f0.x, sy);
        g.lineTo(f1.x, sy);
      }
      // Centre lines are always major.
      if (pass === 1) {
        const cx = Math.round(this.view.ox + (A / 2) * s) + 0.5, cy = Math.round(this.view.oy + 0.5 * s) + 0.5;
        g.moveTo(cx, f0.y); g.lineTo(cx, f1.y); g.moveTo(f0.x, cy); g.lineTo(f1.x, cy);
      }
      g.stroke();
    }
    g.restore();
  }

  segmentScreen(path, i) {
    const n = path.points.length;
    const a = path.points[i], b = path.points[(i + 1) % n];
    return [this.toScreen(a.p), this.toScreen({ x: a.p.x + a.out.x, y: a.p.y + a.out.y }), this.toScreen({ x: b.p.x + b.in.x, y: b.p.y + b.in.y }), this.toScreen(b.p)];
  }

  drawPath(g, path, geom, C) {
    const n = path.points.length;
    const segs = segmentCount(path);
    if (segs > 0) {
      const trace = () => {
        g.beginPath();
        for (let i = 0; i < segs; i += 1) {
          const [p0, p1, p2, p3] = this.segmentScreen(path, i);
          if (i === 0) g.moveTo(p0.x, p0.y);
          g.bezierCurveTo(p1.x, p1.y, p2.x, p2.y, p3.x, p3.y);
        }
        if (path.closed) g.closePath();
      };
      g.save();
      g.lineJoin = 'round';
      g.lineCap = 'round';
      trace();
      g.strokeStyle = C.outline;
      g.lineWidth = 5;
      g.stroke();
      trace();
      g.strokeStyle = C.path;
      g.lineWidth = 2;
      g.stroke();
      // Hovered segment
      if (this.hover?.kind === E.HitKind.Segment && !this.drag) {
        const [p0, p1, p2, p3] = this.segmentScreen(path, this.hover.index);
        g.beginPath();
        g.moveTo(p0.x, p0.y);
        g.bezierCurveTo(p1.x, p1.y, p2.x, p2.y, p3.x, p3.y);
        g.lineWidth = 4;
        g.strokeStyle = C.path;
        g.globalAlpha = 0.45;
        g.stroke();
        g.globalAlpha = 1;
      }
      // Direction chevrons every ~90 px along the path.
      if (geom && geom.length > 0) {
        const pxLen = geom.length * this.view.s;
        const count = Math.min(60, Math.floor(pxLen / 90));
        g.fillStyle = C.path;
        for (let k = 1; k <= count; k += 1) {
          const sm = geom.sampleAtDistance((geom.length * k) / (count + 1), 1, false);
          if (!sm.tangentValid) continue;
          const p = this.toScreen(geom.toNormalized(sm.pos));
          const tx = sm.tangent.x, ty = sm.tangent.y;
          g.beginPath();
          g.moveTo(p.x + tx * 5, p.y + ty * 5);
          g.lineTo(p.x - tx * 4 - ty * 4, p.y - ty * 4 + tx * 4);
          g.lineTo(p.x - tx * 4 + ty * 4, p.y - ty * 4 - tx * 4);
          g.closePath();
          g.fill();
        }
      }
      g.restore();
    }
    // Handles of selected points
    g.save();
    for (const i of this.getSelection()) {
      const pt = path.points[i];
      if (!pt) continue;
      const a = this.toScreen(pt.p);
      for (const key of ['in', 'out']) {
        const hnd = pt[key];
        if (hnd.x === 0 && hnd.y === 0) continue;
        const b = this.toScreen({ x: pt.p.x + hnd.x, y: pt.p.y + hnd.y });
        g.strokeStyle = C.outline;
        g.lineWidth = 3;
        g.beginPath(); g.moveTo(a.x, a.y); g.lineTo(b.x, b.y); g.stroke();
        g.strokeStyle = C.selected;
        g.lineWidth = 1.2;
        g.beginPath(); g.moveTo(a.x, a.y); g.lineTo(b.x, b.y); g.stroke();
        const hovered = this.hover && this.hover.index === i && this.hover.kind === key;
        g.fillStyle = C.selected;
        g.strokeStyle = C.outline;
        g.lineWidth = 1.5;
        g.beginPath(); g.arc(b.x, b.y, hovered ? 5 : 4, 0, Math.PI * 2); g.fill(); g.stroke();
      }
    }
    // Anchors: square = corner, circle = smooth, diamond = independent handles.
    path.points.forEach((pt, i) => {
      const p = this.toScreen(pt.p);
      const sel = this.selection.has(i);
      const hovered = this.hover?.kind === E.HitKind.Anchor && this.hover.index === i;
      const r = hovered ? 6 : 5;
      g.beginPath();
      if (pt.mode === 'corner') g.rect(p.x - r, p.y - r, 2 * r, 2 * r);
      else if (pt.mode === 'free') { g.moveTo(p.x, p.y - r * 1.3); g.lineTo(p.x + r * 1.3, p.y); g.lineTo(p.x, p.y + r * 1.3); g.lineTo(p.x - r * 1.3, p.y); g.closePath(); }
      else g.arc(p.x, p.y, r, 0, Math.PI * 2);
      g.fillStyle = sel ? C.selected : C.anchor;
      g.fill();
      g.lineWidth = 1.5;
      g.strokeStyle = C.outline;
      g.stroke();
      if (i === 0 && n > 1) {
        // Start marker ring
        g.beginPath();
        g.arc(p.x, p.y, r + 4, 0, Math.PI * 2);
        g.strokeStyle = C.path;
        g.lineWidth = 1.5;
        g.stroke();
      }
    });
    g.restore();
  }

  // Outline of the source frame placed by the current preview pose.
  drawGhost(g, geom, C) {
    if (!geom || geom.empty) return;
    const pose = previewPose(geom, this.poseSettings());
    if (!pose) return;
    const v = this.values;
    const A = this.aspect;
    const scale = [v.wmpScale[0] / 100, (v.wmpUniformScale ? v.wmpScale[0] : v.wmpScale[1]) / 100];
    const rot = ((v.wmpRotation + (v.wmpOrient ? pose.angle + v.wmpOrientOffset : 0)) * Math.PI) / 180;
    const P = { x: (pose.positionN.x + v.wmpOffset[0]) * A, y: pose.positionN.y + v.wmpOffset[1] };
    const Q = { x: v.wmpPivot[0] * A, y: v.wmpPivot[1] };
    const cs = Math.cos(rot), sn = Math.sin(rot);
    const map = (n) => {
      const sx = (n.x * A - Q.x) * scale[0], sy = (n.y - Q.y) * scale[1];
      return this.toScreen({ x: (P.x + cs * sx - sn * sy) / A, y: P.y + sn * sx + cs * sy });
    };
    const corners = [map({ x: 0, y: 0 }), map({ x: 1, y: 0 }), map({ x: 1, y: 1 }), map({ x: 0, y: 1 })];
    g.save();
    g.setLineDash([3, 5]);
    g.strokeStyle = C.marker;
    g.globalAlpha = 0.38;
    g.lineWidth = 1;
    g.beginPath();
    corners.forEach((c, i) => (i ? g.lineTo(c.x, c.y) : g.moveTo(c.x, c.y)));
    g.closePath();
    g.stroke();
    g.restore();
  }

  drawMarker(g, geom, C) {
    if (!geom || geom.empty) return;
    const pose = previewPose(geom, this.poseSettings());
    if (!pose) return;
    const p = this.toScreen(pose.positionN);
    g.save();
    g.fillStyle = C.marker;
    g.strokeStyle = C.outline;
    g.lineWidth = 2;
    g.beginPath();
    g.arc(p.x, p.y, 6, 0, Math.PI * 2);
    g.fill();
    g.stroke();
    if (pose.angleValid) {
      const a = (pose.angle * Math.PI) / 180;
      g.strokeStyle = C.marker;
      g.lineWidth = 2;
      g.beginPath();
      g.moveTo(p.x, p.y);
      g.lineTo(p.x + Math.cos(a) * 22, p.y + Math.sin(a) * 22);
      g.stroke();
    }
    g.restore();
  }

  drawTransformBox(g, box, C) {
    g.save();
    g.strokeStyle = C.accent;
    g.setLineDash([4, 3]);
    g.lineWidth = 1;
    g.strokeRect(box.x1 + 0.5, box.y1 + 0.5, box.x2 - box.x1, box.y2 - box.y1);
    g.setLineDash([]);
    g.beginPath();
    g.moveTo(box.cx, box.y1);
    g.lineTo(box.knob.x, box.knob.y);
    g.stroke();
    g.fillStyle = C.anchor;
    for (const c of box.corners) {
      g.fillRect(c.x - 4, c.y - 4, 8, 8);
      g.strokeRect(c.x - 4 + 0.5, c.y - 4 + 0.5, 8, 8);
    }
    g.beginPath();
    g.arc(box.knob.x, box.knob.y, 5, 0, Math.PI * 2);
    g.fill();
    g.stroke();
    g.restore();
  }
}
