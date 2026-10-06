// GraphEditor: DOM shell + wiring between store, viewport, renderer and
// interaction. Framework-agnostic; mount into any element.

import { GraphStore } from '../core/model.js';
import { INTERP, TANGENTS, EASING_PRESETS, segmentToEasing, matchEasingPreset, keysBounds, curveBounds, evalKeys } from '../core/curve.js';
import {
  formatTime,
  parseTime,
  parseNumber,
  formatValue,
  snapToFrame,
  framesToSeconds,
  chooseValueStep,
  visibleFrames,
} from '../core/time.js';
import { Viewport } from './viewport.js';
import { render, renderEasingInset, renderNavigator, readTheme, channelTransform, keyPlotValue, keySpeeds } from './renderer.js';
import { Interaction } from './interaction.js';
import { resolveStrings, fmt } from './strings.js';
import { ICONS } from './icons.js';
import { Emitter } from '../core/emitter.js';

const DEFAULT_OPTIONS = {
  mode: 'value', // 'value' | 'speed'
  normalize: false,
  timeFormat: 'timecode', // 'timecode' | 'frames' | 'seconds'
  snapFrames: true,
  snapKeys: true,
  showHandles: 'selected', // 'selected' | 'all' | 'none'
  collision: 'block',
  undo: 'internal',
  locale: 'en',
  theme: 'dark', // 'dark' | 'light' | 'auto'
  playhead: 0,
  showToolbar: true,
  showInspector: true,
  showNavigator: true,
  showEasingInset: true,
  showStatus: true,
  reducedMotion: null, // null = follow system
};

function el(tag, attrs = {}, children = []) {
  const node = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (v == null || v === false) continue;
    if (k === 'class') node.className = v;
    else if (k === 'html') node.innerHTML = v;
    else if (k === 'text') node.textContent = v;
    else if (k.startsWith('on') && typeof v === 'function') node.addEventListener(k.slice(2), v);
    else node.setAttribute(k, v === true ? '' : v);
  }
  for (const c of [].concat(children)) if (c) node.appendChild(typeof c === 'string' ? document.createTextNode(c) : c);
  return node;
}

export class GraphEditor {
  constructor(container, options = {}) {
    if (!container) throw new Error('GraphEditor needs a container element');
    this.container = container;
    this.options = { ...DEFAULT_OPTIONS, ...options };
    this.strings = resolveStrings(this.options.locale);
    this.events = new Emitter();
    this.store = new GraphStore(options.document || { fps: options.fps, range: options.range, channels: [] }, {
      undo: this.options.undo,
      collision: this.options.collision,
    });
    this.vp = new Viewport();
    this.mode = this.options.mode;
    this.normalize = !!this.options.normalize;
    this.playhead = this.options.playhead;
    this.state = {
      hover: null,
      boxSelect: null,
      ghost: null,
      snapLine: null,
      blockedAt: null,
      transformBox: null,
      transformBoxWorld: null,
      pulse: null,
    };
    this.clipboard = null;
    this._raf = 0;
    this._dirty = false;
    this._destroyed = false;
    this._menu = null;
    this._help = null;
    this._insetDrag = null;
    this._navDrag = null;

    this._buildDom();
    this.theme = readTheme(this.root);
    this.interaction = new Interaction(this);
    this._bindStore();
    this._observeSize();
    this._applyTheme();
    this.fitAll();
    this.requestRender();
  }

  // ------------------------------------------------------------------ DOM
  _buildDom() {
    const s = this.strings;
    this.root = el('div', {
      class: 'ge-root',
      tabindex: '0',
      role: 'application',
      'aria-label': 'Graph editor',
      'data-theme': this.options.theme === 'light' ? 'light' : 'dark',
    });

    // Toolbar ---------------------------------------------------------------
    const btn = (action, icon, labelText, title, extra = {}) =>
      el(
        'button',
        {
          class: `ge-btn${labelText ? '' : ' ge-btn--icon'}`,
          type: 'button',
          'data-action': action,
          title: title || labelText,
          'aria-label': title || labelText,
          ...extra,
        },
        [el('span', { html: ICONS[icon] || '' }), labelText ? el('span', { class: 'ge-btn-label', text: labelText }) : null],
      );
    this.ui = {};
    const ui = this.ui;
    ui.modeValue = btn('mode-value', 'value', s.modeValue, s.modeValueHint, { 'aria-pressed': 'true' });
    ui.modeSpeed = btn('mode-speed', 'speed', s.modeSpeed, s.modeSpeedHint, { 'aria-pressed': 'false' });
    ui.channels = el('div', { class: 'ge-group ge-group--channels', role: 'group', 'aria-label': s.channels });
    ui.easingBtn = btn('easing', 'easing', s.easing, s.easingHint, { 'aria-haspopup': 'menu' });
    ui.interpBezier = btn('interp-bezier', 'bezier', '', `${s.interp}: ${s.interpBezier} (1)`);
    ui.interpLinear = btn('interp-linear', 'linear', '', `${s.interp}: ${s.interpLinear} (2)`);
    ui.interpHold = btn('interp-hold', 'hold', '', `${s.interp}: ${s.interpHold} (3)`);
    ui.fitAll = btn('fit-all', 'fitAll', '', `${s.fitAll} (F)`);
    ui.fitSel = btn('fit-selected', 'fitSelected', '', `${s.fitSelected} (Shift+F)`);
    ui.fitRange = btn('fit-range', 'fitRange', '', `${s.fitRange} (Home)`);
    ui.normalize = btn('normalize', 'normalize', '', s.normalizeHint, { 'aria-pressed': String(this.normalize) });
    ui.snapFrames = btn('snap-frames', 'snap', '', s.snapFrames, { 'aria-pressed': String(this.options.snapFrames) });
    ui.snapKeys = btn('snap-keys', 'magnet', '', s.snapKeys, { 'aria-pressed': String(this.options.snapKeys) });
    ui.handles = btn('handles', 'handles', '', `${s.handles}: ${this._handlesLabel()}`, {
      'aria-pressed': String(this.options.showHandles !== 'none'),
    });
    ui.zoom = el('input', { type: 'range', min: '0', max: '1000', value: '500', 'aria-label': s.zoom, title: s.zoom });
    ui.timeFormat = el('select', { class: 'ge-select', 'aria-label': s.timeFormat, title: s.timeFormat }, [
      el('option', { value: 'timecode', text: s.tfTimecode }),
      el('option', { value: 'frames', text: s.tfFrames }),
      el('option', { value: 'seconds', text: s.tfSeconds }),
    ]);
    ui.timeFormat.value = this.options.timeFormat;
    ui.undo = btn('undo', 'undo', '', `${s.undo} (Ctrl+Z)`);
    ui.redo = btn('redo', 'redo', '', `${s.redo} (Ctrl+Y)`);
    ui.help = btn('help', 'help', '', `${s.help} (?)`);

    this.toolbar = el('div', { class: 'ge-toolbar', role: 'toolbar' }, [
      el('div', { class: 'ge-group ge-group--mode' }, [ui.modeValue, ui.modeSpeed]),
      ui.channels,
      el('div', { class: 'ge-group ge-group--edit' }, [
        ui.easingBtn,
        el('span', { class: 'ge-sep' }),
        ui.interpBezier,
        ui.interpLinear,
        ui.interpHold,
      ]),
      el('div', { class: 'ge-group ge-group--view' }, [
        ui.fitAll,
        ui.fitSel,
        ui.fitRange,
        el('span', { class: 'ge-sep' }),
        ui.normalize,
        ui.snapFrames,
        ui.snapKeys,
        ui.handles,
      ]),
      el('div', { class: 'ge-group ge-zoom' }, [
        el('span', { html: ICONS.clock, style: 'display:inline-flex;width:14px;height:14px;color:var(--ge-text-dim)' }),
        ui.zoom,
      ]),
      el('div', { class: 'ge-group ge-group--format' }, [ui.timeFormat]),
      el('div', { class: 'ge-group ge-group--history' }, [ui.undo, ui.redo, ui.help]),
    ]);
    if (!this.options.showToolbar) this.toolbar.style.display = 'none';

    // Stage -----------------------------------------------------------------
    this.canvas = el('canvas', { class: 'ge-canvas', 'aria-hidden': 'true' });
    this.emptyHint = el('div', { class: 'ge-empty', text: '' });
    this.stage = el('div', { class: 'ge-stage' }, [this.canvas, this.emptyHint]);

    // Footer: inspector + easing inset ----------------------------------------
    const field = (key, labelText, cls = '', unit = '') => {
      const input = el('input', {
        class: `ge-input ${cls}`.trim(),
        type: 'text',
        inputmode: 'decimal',
        autocomplete: 'off',
        spellcheck: 'false',
        'aria-label': labelText,
        'data-field': key,
      });
      const row = el('div', { class: 'ge-field-row' }, [input, unit ? el('span', { class: 'ge-unit', text: unit }) : null]);
      return { wrap: el('div', { class: 'ge-field' }, [el('label', { text: labelText }), row]), input };
    };
    ui.fTime = field('time', s.time);
    ui.fValue = field('value', s.value);
    ui.fInSpeed = field('inSpeed', s.inSpeed, 'ge-input--sm', s.perSecond);
    ui.fInInfl = field('inInfl', `${s.influence}`, 'ge-input--sm', '%');
    ui.fOutSpeed = field('outSpeed', s.outSpeed, 'ge-input--sm', s.perSecond);
    ui.fOutInfl = field('outInfl', `${s.influence}`, 'ge-input--sm', '%');
    ui.interpSelect = el('select', { class: 'ge-select', 'aria-label': s.interp, 'data-field': 'interp' }, [
      el('option', { value: INTERP.BEZIER, text: s.interpBezier }),
      el('option', { value: INTERP.LINEAR, text: s.interpLinear }),
      el('option', { value: INTERP.HOLD, text: s.interpHold }),
      el('option', { value: '', text: s.mixed, disabled: true, hidden: true }),
    ]);
    ui.tangentSelect = el('select', { class: 'ge-select', 'aria-label': s.tangents, 'data-field': 'tangents' }, [
      el('option', { value: TANGENTS.AUTO, text: s.tangentsAuto }),
      el('option', { value: TANGENTS.UNIFIED, text: s.tangentsUnified }),
      el('option', { value: TANGENTS.BROKEN, text: s.tangentsBroken }),
      el('option', { value: '', text: s.mixed, disabled: true, hidden: true }),
    ]);
    this.inspector = el('div', { class: 'ge-inspector' }, [
      ui.fTime.wrap,
      ui.fValue.wrap,
      el('div', { class: 'ge-field' }, [el('label', { text: s.interp }), ui.interpSelect]),
      el('div', { class: 'ge-field' }, [el('label', { text: s.tangents }), ui.tangentSelect]),
      ui.fInSpeed.wrap,
      ui.fInInfl.wrap,
      ui.fOutSpeed.wrap,
      ui.fOutInfl.wrap,
    ]);
    ui.insetCanvas = el('canvas', { 'aria-label': s.easing, role: 'img' });
    ui.insetLabel = el('div', { class: 'ge-inset-label', text: '' });
    this.inset = el('div', { class: 'ge-inset' }, [ui.insetCanvas, ui.insetLabel]);
    if (!this.options.showEasingInset) this.inset.style.display = 'none';
    this.footer = el('div', { class: 'ge-footer' }, [this.inspector, this.inset]);
    if (!this.options.showInspector) this.footer.style.display = 'none';

    // Navigator ---------------------------------------------------------------
    ui.navCanvas = el('canvas', { 'aria-hidden': 'true' });
    this.navigator = el('div', { class: 'ge-navigator' }, [ui.navCanvas]);
    if (!this.options.showNavigator) this.navigator.style.display = 'none';

    // Status --------------------------------------------------------------------
    ui.statusMain = el('span', { class: 'ge-status-main', text: s.noSelection });
    ui.statusWarn = el('span', { class: 'ge-status-warn', text: '' });
    ui.statusRight = el('span', { class: 'ge-status-right', text: '' });
    this.status = el('div', { class: 'ge-status' }, [ui.statusMain, ui.statusWarn, ui.statusRight]);
    if (!this.options.showStatus) this.status.style.display = 'none';
    this.live = el('div', { class: 'ge-live', 'aria-live': 'polite' });

    this.root.append(this.toolbar, this.stage, this.footer, this.navigator, this.status, this.live);
    this.container.appendChild(this.root);

    this._bindToolbar();
    this._bindInspector();
    this._bindInset();
    this._bindNavigator();
    this._renderChannels();
  }

  _handlesLabel() {
    const s = this.strings;
    return this.options.showHandles === 'all' ? s.handlesAll : this.options.showHandles === 'none' ? s.handlesNone : s.handlesSelected;
  }

  _bindToolbar() {
    const ui = this.ui;
    this.toolbar.addEventListener('click', (e) => {
      const b = e.target.closest('[data-action]');
      if (!b) return;
      const action = b.dataset.action;
      const sel = [...this.store.selectedKeys];
      switch (action) {
        case 'mode-value':
          this.setMode('value');
          break;
        case 'mode-speed':
          this.setMode('speed');
          break;
        case 'easing':
          this.openEasingMenu(b);
          break;
        case 'interp-bezier':
        case 'interp-linear':
        case 'interp-hold': {
          const interp = action.split('-')[1];
          if (sel.length) this.store.setInterp(sel, interp);
          else if (this.store.selectedSegment) this.store.setInterp([this.store.selectedSegment.keyId], interp);
          break;
        }
        case 'fit-all':
          this.fitAll();
          break;
        case 'fit-selected':
          this.fitSelected();
          break;
        case 'fit-range':
          this.fitRange();
          break;
        case 'normalize':
          this.setNormalize(!this.normalize);
          break;
        case 'snap-frames':
          this.setOption('snapFrames', !this.options.snapFrames);
          break;
        case 'snap-keys':
          this.setOption('snapKeys', !this.options.snapKeys);
          break;
        case 'handles': {
          const order = ['selected', 'all', 'none'];
          this.setOption('showHandles', order[(order.indexOf(this.options.showHandles) + 1) % order.length]);
          break;
        }
        case 'undo':
          this.undo();
          break;
        case 'redo':
          this.redo();
          break;
        case 'help':
          this.toggleHelp();
          break;
        default:
          break;
      }
      this.root.focus({ preventScroll: true });
    });
    ui.zoom.addEventListener('input', () => {
      // Logarithmic slider: 0 -> 0.5 px/s, 1000 -> 60000 px/s. Anchor at the playhead if visible, else centre.
      const v = Number(ui.zoom.value) / 1000;
      const pxPerSec = Math.exp(Math.log(0.5) + v * (Math.log(60000) - Math.log(0.5)));
      const px = this.playhead != null ? this.vp.xOf(this.playhead) : this.vp.width / 2;
      const anchor = px >= 0 && px <= this.vp.width ? px : this.vp.width / 2;
      this.vp.setPxPerSec(pxPerSec, anchor);
      this.onViewChanged(true);
    });
    ui.timeFormat.addEventListener('change', () => this.setOption('timeFormat', ui.timeFormat.value));
    ui.channels.addEventListener('click', (e) => {
      const lock = e.target.closest('.ge-chip-lock');
      const chip = e.target.closest('.ge-chip');
      if (!chip) return;
      const id = chip.dataset.channel;
      const ch = this.store.channel(id);
      if (!ch) return;
      if (lock) {
        this.store.setChannelProps(id, { locked: !ch.locked });
      } else if (e.altKey) {
        const solo = this.store.channels.every((c) => (c.id === id ? c.visible : !c.visible));
        for (const c of this.store.channels) this.store.setChannelProps(c.id, { visible: solo ? true : c.id === id });
      } else {
        this.store.setChannelProps(id, { visible: !ch.visible });
      }
      this.store.activeChannelId = id;
      this._renderChannels();
      this.requestRender();
    });
    ui.channels.addEventListener('scroll', () => this._updateChannelOverflow(), { passive: true });
    ui.channels.addEventListener('dblclick', (e) => {
      const chip = e.target.closest('.ge-chip');
      if (!chip) return;
      const ch = this.store.channel(chip.dataset.channel);
      if (ch) this.fitChannels([ch]);
    });
  }

  _bindInspector() {
    const ui = this.ui;
    const inputs = [ui.fTime.input, ui.fValue.input, ui.fInSpeed.input, ui.fInInfl.input, ui.fOutSpeed.input, ui.fOutInfl.input];
    for (const input of inputs) {
      input.addEventListener('keydown', (e) => {
        if (e.key === 'Enter') {
          e.preventDefault();
          this._applyField(input);
          input.select();
        } else if (e.key === 'Escape') {
          e.preventDefault();
          this._syncInspector();
          this.root.focus({ preventScroll: true });
        } else if (e.key === 'ArrowUp' || e.key === 'ArrowDown') {
          e.preventDefault();
          this._stepField(input, (e.key === 'ArrowUp' ? 1 : -1) * (e.shiftKey ? 10 : 1));
        }
        e.stopPropagation();
      });
      input.addEventListener('focus', () => input.select());
      input.addEventListener('blur', () => this._applyField(input, true));
      input.addEventListener('input', () => input.classList.remove('is-invalid'));
    }
    ui.interpSelect.addEventListener('change', () => {
      const ids = this._inspectorTargets();
      if (ids.length && ui.interpSelect.value) this.store.setInterp(ids, ui.interpSelect.value);
    });
    ui.tangentSelect.addEventListener('change', () => {
      const ids = this._inspectorTargets();
      if (ids.length && ui.tangentSelect.value) this.store.setTangents(ids, ui.tangentSelect.value);
    });
  }

  _inspectorTargets() {
    const ids = [...this.store.selectedKeys];
    if (ids.length) return ids;
    if (this.store.selectedSegment) return [this.store.selectedSegment.keyId];
    return [];
  }

  _stepField(input, dir) {
    const store = this.store;
    const ids = this._inspectorTargets();
    if (!ids.length) return;
    const f = input.dataset.field;
    if (f === 'time') store.moveKeys(ids, dir * framesToSeconds(1, store.fps), 0, { snap: false, anchorId: ids[0] });
    else if (f === 'value') store.moveKeys(ids, 0, dir * this.valueNudgeStep(), { snap: false, valueOnly: true });
    else {
      const cur = parseNumber(input.value);
      if (cur == null) return;
      input.value = String(cur + dir * (f.endsWith('Infl') ? 5 : this.valueNudgeStep()));
      this._applyField(input);
    }
    this._syncInspector();
    input.select();
  }

  _applyField(input, silent = false) {
    if (input.dataset.synced === input.value) return; // unchanged
    const store = this.store;
    const ids = this._inspectorTargets();
    if (!ids.length) return;
    const f = input.dataset.field;
    const refs = ids.map((id) => store.keyRef(id)).filter(Boolean);
    const invalid = () => {
      if (!silent) input.classList.add('is-invalid');
      else this._syncInspector();
    };
    if (f === 'time') {
      const first = refs.reduce((a, b) => (a.key.t <= b.key.t ? a : b));
      const t = parseTime(input.value, store.fps, {
        mode: this.options.timeFormat === 'seconds' ? 'seconds' : 'frames',
        base: first.key.t,
      });
      if (t == null) return invalid();
      store.moveKeys(ids, t - first.key.t, 0, { snap: this.options.snapFrames, anchorId: first.key.id });
    } else if (f === 'value') {
      const v = parseNumber(input.value);
      if (v == null) return invalid();
      store.begin('set value');
      for (const r of refs) store.setKeyValue(r.key.id, v);
      store.commit();
    } else {
      const n = parseNumber(input.value);
      if (n == null) return invalid();
      const side = f.startsWith('in') ? 'in' : 'out';
      const isInfl = f.endsWith('Infl');
      store.begin('tangent');
      for (const r of refs) {
        const keys = r.channel.keys;
        const i = keys.indexOf(r.key);
        const segDur = side === 'in' ? (i > 0 ? r.key.t - keys[i - 1].t : 0) : i < keys.length - 1 ? keys[i + 1].t - r.key.t : 0;
        if (segDur <= 0) continue;
        const h = r.key[side];
        const curSpeed = h.dt !== 0 ? h.dv / h.dt : 0;
        const curInfl = Math.abs(h.dt) / segDur;
        const infl = isInfl ? Math.min(1, Math.max(0.001, n / 100)) : Math.max(0.001, curInfl || 1 / 3);
        const speed = isInfl ? curSpeed : n;
        const dt = (side === 'in' ? -1 : 1) * infl * segDur;
        store.setHandle(r.key.id, side, { dt, dv: speed * dt }, { breakTangents: r.key.tangents === TANGENTS.BROKEN });
      }
      store.commit();
    }
    this._syncInspector();
  }

  _syncInspector() {
    const ui = this.ui;
    const store = this.store;
    const s = this.strings;
    const ids = this._inspectorTargets();
    const refs = ids.map((id) => store.keyRef(id)).filter(Boolean);
    const set = (input, text, enabled = true) => {
      input.value = text;
      input.dataset.synced = text;
      input.disabled = !enabled;
      input.classList.remove('is-invalid');
    };
    if (!refs.length) {
      for (const f of [ui.fTime, ui.fValue, ui.fInSpeed, ui.fInInfl, ui.fOutSpeed, ui.fOutInfl]) set(f.input, '', false);
      ui.interpSelect.disabled = true;
      ui.tangentSelect.disabled = true;
      ui.interpSelect.value = '';
      ui.tangentSelect.value = '';
      return;
    }
    const same = (fn) => {
      const first = fn(refs[0]);
      return refs.every((r) => Math.abs(fn(r) - first) < 1e-9) ? first : null;
    };
    const fps = store.fps;
    const t = same((r) => r.key.t);
    set(
      ui.fTime.input,
      t == null
        ? s.mixed
        : formatTime(t, fps, this.options.timeFormat === 'timecode' ? 'timecode' : this.options.timeFormat, { showSubframe: true }),
    );
    const v = same((r) => r.key.v);
    set(ui.fValue.input, v == null ? s.mixed : formatValue(v, 4), this.mode !== 'speed');
    const speedOf = (r, side) => {
      const h = r.key[side];
      return h.dt !== 0 ? h.dv / h.dt : 0;
    };
    const inflOf = (r, side) => {
      const keys = r.channel.keys;
      const i = keys.indexOf(r.key);
      const seg = side === 'in' ? (i > 0 ? r.key.t - keys[i - 1].t : 0) : i < keys.length - 1 ? keys[i + 1].t - r.key.t : 0;
      return seg > 0 ? (Math.abs(r.key[side].dt) / seg) * 100 : 0;
    };
    const hasIn = refs.some((r) => r.channel.keys.indexOf(r.key) > 0);
    const hasOut = refs.some((r) => r.channel.keys.indexOf(r.key) < r.channel.keys.length - 1);
    const si = same((r) => speedOf(r, 'in'));
    const ii = same((r) => inflOf(r, 'in'));
    const so = same((r) => speedOf(r, 'out'));
    const io = same((r) => inflOf(r, 'out'));
    set(ui.fInSpeed.input, si == null ? s.mixed : formatValue(si, 3), hasIn);
    set(ui.fInInfl.input, ii == null ? s.mixed : formatValue(ii, 1), hasIn);
    set(ui.fOutSpeed.input, so == null ? s.mixed : formatValue(so, 3), hasOut);
    set(ui.fOutInfl.input, io == null ? s.mixed : formatValue(io, 1), hasOut);
    const interp = refs.every((r) => r.key.interp === refs[0].key.interp) ? refs[0].key.interp : '';
    const tang = refs.every((r) => r.key.tangents === refs[0].key.tangents) ? refs[0].key.tangents : '';
    ui.interpSelect.disabled = false;
    ui.tangentSelect.disabled = false;
    ui.interpSelect.value = interp;
    ui.tangentSelect.value = tang;
  }

  focusInspector(field) {
    const input = field === 'value' ? this.ui.fValue.input : this.ui.fTime.input;
    if (!input.disabled) {
      input.focus();
      input.select();
    }
  }

  // ----------------------------------------------------------- easing inset
  _bindInset() {
    const c = this.ui.insetCanvas;
    const pos = (e) => {
      const r = c.getBoundingClientRect();
      return { x: e.clientX - r.left, y: e.clientY - r.top, w: r.width, h: r.height };
    };
    const toNorm = (p) => {
      const pad = 10;
      return { u: (p.x - pad) / (p.w - pad * 2), p: 1 - (p.y - pad) / (p.h - pad * 2) };
    };
    c.addEventListener('pointerdown', (e) => {
      const segs = this.store.targetSegments();
      if (!segs.length) return;
      const e0 = segmentToEasing(segs[0].k0, segs[0].k1);
      if (!e0 || e0.interp !== INTERP.BEZIER) return;
      const p = pos(e);
      const n = toNorm(p);
      const d0 = Math.hypot(n.u - e0.cp[0], n.p - e0.cp[1]);
      const d1 = Math.hypot(n.u - e0.cp[2], n.p - e0.cp[3]);
      const which = d0 <= d1 ? 0 : 1;
      if (Math.min(d0, d1) > 0.18) return;
      this.store.begin('easing');
      this._insetDrag = { which, cp: [...e0.cp], segs };
      c.setPointerCapture(e.pointerId);
      e.preventDefault();
    });
    c.addEventListener('pointermove', (e) => {
      if (!this._insetDrag) {
        const segs = this.store.targetSegments();
        if (!segs.length) return;
        const e0 = segmentToEasing(segs[0].k0, segs[0].k1);
        if (!e0) return;
        const n = toNorm(pos(e));
        const d0 = Math.hypot(n.u - e0.cp[0], n.p - e0.cp[1]);
        const d1 = Math.hypot(n.u - e0.cp[2], n.p - e0.cp[3]);
        const hot = Math.min(d0, d1) <= 0.18 ? (d0 <= d1 ? 0 : 1) : null;
        if (hot !== this.state.insetHot) {
          this.state.insetHot = hot;
          this.requestRender();
        }
        return;
      }
      const n = toNorm(pos(e));
      const cp = this._insetDrag.cp;
      const u = Math.min(1, Math.max(0, n.u));
      const pv = e.shiftKey ? (this._insetDrag.which === 0 ? 0 : 1) : Math.min(2.5, Math.max(-1.5, n.p));
      if (this._insetDrag.which === 0) {
        cp[0] = u;
        cp[1] = pv;
      } else {
        cp[2] = u;
        cp[3] = pv;
      }
      this.store.restoreTransactionStart();
      this.store.applyEasing(
        cp,
        this._insetDrag.segs
          .map((sg) => ({
            channel: this.store.channel(sg.channel.id),
            k0: this.store.keyRef(sg.k0.id)?.key,
            k1: this.store.keyRef(sg.k1.id)?.key,
          }))
          .filter((x) => x.k0 && x.k1),
      );
      this.requestRender();
    });
    const end = () => {
      if (!this._insetDrag) return;
      this.store.commit();
      this._insetDrag = null;
      this.requestRender();
    };
    c.addEventListener('pointerup', end);
    c.addEventListener('pointercancel', end);
    c.addEventListener('pointerleave', () => {
      if (this.state.insetHot != null && !this._insetDrag) {
        this.state.insetHot = null;
        this.requestRender();
      }
    });
    c.addEventListener('dblclick', () => this.openEasingMenu(c));
  }

  // -------------------------------------------------------------- navigator
  _bindNavigator() {
    const c = this.ui.navCanvas;
    const extent = () => this.navExtent();
    const tAt = (clientX) => {
      const r = c.getBoundingClientRect();
      const ex = extent();
      return ex.tMin + ((clientX - r.left) / r.width) * (ex.tMax - ex.tMin);
    };
    c.addEventListener('pointerdown', (e) => {
      const t = tAt(e.clientX);
      const span = this.vp.t1 - this.vp.t0;
      if (t < this.vp.t0 || t > this.vp.t1) {
        this.vp.t0 = t - span / 2;
        this.onViewChanged();
      }
      this._navDrag = { startT: t, startT0: this.vp.t0 };
      c.setPointerCapture(e.pointerId);
      e.preventDefault();
    });
    c.addEventListener('pointermove', (e) => {
      if (!this._navDrag) return;
      const t = tAt(e.clientX);
      this.vp.t0 = this._navDrag.startT0 + (t - this._navDrag.startT);
      this.onViewChanged();
    });
    const end = () => (this._navDrag = null);
    c.addEventListener('pointerup', end);
    c.addEventListener('pointercancel', end);
    c.addEventListener(
      'wheel',
      (e) => {
        e.preventDefault();
        const factor = Math.exp(-e.deltaY * 0.0022);
        this.vp.zoomTime(factor, this.vp.width / 2);
        this.onViewChanged(true);
      },
      { passive: false },
    );
  }

  navExtent() {
    const doc = this.store.doc;
    let tMin = doc.range.start;
    let tMax = doc.range.end;
    for (const ch of doc.channels) {
      const b = keysBounds(ch.keys);
      if (!b) continue;
      tMin = Math.min(tMin, b.tMin);
      tMax = Math.max(tMax, b.tMax);
    }
    const pad = Math.max(0.25, (tMax - tMin) * 0.05);
    return { tMin: tMin - pad, tMax: tMax + pad };
  }

  // --------------------------------------------------------------- channels
  _renderChannels() {
    const ui = this.ui;
    const s = this.strings;
    ui.channels.textContent = '';
    for (const ch of this.store.channels) {
      const chip = el(
        'button',
        {
          class: `ge-chip${ch.id === this.store.activeChannelId ? ' is-active' : ''}`,
          type: 'button',
          'data-channel': ch.id,
          'aria-pressed': String(ch.visible),
          title: `${fmt(s.toggleChannel, { name: ch.name })} · ${s.soloHint}`,
          style: `--c:${ch.color}`,
        },
        [
          el('span', { class: 'ge-chip-dot' }),
          el('span', { class: 'ge-chip-name', text: this._tiny && ch.shortName ? ch.shortName : ch.name }),
          el('span', {
            class: 'ge-chip-lock',
            role: 'button',
            tabindex: '0',
            'aria-pressed': String(ch.locked),
            'aria-label': fmt(ch.locked ? s.unlockChannel : s.lockChannel, { name: ch.name }),
            title: fmt(ch.locked ? s.unlockChannel : s.lockChannel, { name: ch.name }),
            html: ch.locked ? ICONS.lock : ICONS.unlock,
          }),
        ],
      );
      ui.channels.appendChild(chip);
    }
    this._updateChannelOverflow();
  }

  _updateChannelOverflow() {
    const c = this.ui.channels;
    const over = c.scrollWidth > c.clientWidth + 1;
    c.classList.toggle('is-overflowing', over);
    c.classList.toggle('is-scrolled', over && c.scrollLeft > 4);
  }

  // ------------------------------------------------------------------ store
  _bindStore() {
    this._offs = [
      this.store.on('change', (e) => {
        this._syncInspector();
        this._updateHistoryButtons();
        if (e.kind === 'load') {
          this._renderChannels();
          this.fitAll();
        }
        this.emptyHint.textContent = this.store.channels.some((c) => c.keys.length) ? '' : this.strings.noSelection;
        this.requestRender();
        if (e.kind !== 'cancel') this.events.emit('change', { label: e.label, kind: e.kind, document: this.store.exportDoc() });
      }),
      this.store.on('transaction', (e) => this.events.emit('transaction', e)),
      this.store.on('selection', (info) => {
        this._syncInspector();
        this._renderChannels();
        this._announceSelection(info);
        this.requestRender();
        this.events.emit('selection', {
          keys: info.keys.map((r) => r.key.id),
          segment: info.segment,
          activeChannelId: info.activeChannelId,
        });
      }),
    ];
    this._syncInspector();
    this._updateHistoryButtons();
  }

  _announceSelection(info) {
    const s = this.strings;
    let text = '';
    if (info.segment) {
      const ref = this.store.keyRef(info.segment.keyId);
      if (ref) {
        const k1 = ref.channel.keys[ref.index + 1];
        text = `${ref.channel.name}: ${fmt(s.segmentSelected, { a: formatTime(ref.key.t, this.store.fps, this.options.timeFormat), b: k1 ? formatTime(k1.t, this.store.fps, this.options.timeFormat) : '' })}`;
      }
    } else if (info.keys.length === 1) {
      const r = info.keys[0];
      text = `${r.channel.name} · ${formatTime(r.key.t, this.store.fps, this.options.timeFormat, { showSubframe: true })} · ${formatValue(r.key.v, 3)}${r.channel.unit ? ` ${r.channel.unit}` : ''}`;
    } else if (info.keys.length > 1) {
      const chans = new Set(info.keys.map((r) => r.channel.name));
      text = `${fmt(s.selectionCount, { n: info.keys.length })} · ${[...chans].join(', ')}`;
    } else text = s.noSelection;
    this.setStatusMain(text);
    this.live.textContent = text;
  }

  _updateHistoryButtons() {
    this.ui.undo.disabled = !this.store.canUndo && this.options.undo === 'internal';
    this.ui.redo.disabled = !this.store.canRedo && this.options.undo === 'internal';
  }

  // ------------------------------------------------------------------ sizing
  _observeSize() {
    this._ro = new ResizeObserver(() => this._onResize());
    this._ro.observe(this.root);
    this._ro.observe(this.stage);
    this._onResize();
  }

  _onResize() {
    const rect = this.stage.getBoundingClientRect();
    const w = Math.max(1, Math.round(rect.width));
    const h = Math.max(1, Math.round(rect.height));
    const dpr = Math.max(1, Math.min(3, window.devicePixelRatio || 1));
    this.dpr = dpr;
    if (this.canvas.width !== Math.round(w * dpr) || this.canvas.height !== Math.round(h * dpr)) {
      this.canvas.width = Math.round(w * dpr);
      this.canvas.height = Math.round(h * dpr);
    }
    // Panel resize keeps the same visible time/value window instead of
    // revealing or hiding content; the first real size triggers a fit.
    const prevW = this.vp.width;
    const prevPlotH = this.vp.plotHeight;
    this.vp.resize(w, h);
    if (this._sized) {
      if (prevW > 0 && w !== prevW) this.vp.pxPerSec *= w / prevW;
      if (prevPlotH > 0 && this.vp.plotHeight !== prevPlotH) this.vp.pxPerUnit *= this.vp.plotHeight / prevPlotH;
    }
    const rootRect = this.root.getBoundingClientRect();
    const sizes = [];
    if (rootRect.width < 760) sizes.push('narrow');
    if (rootRect.width < 520) sizes.push('tiny');
    if (rootRect.height < 330) sizes.push('short');
    if (rootRect.height < 190) sizes.push('micro');
    this.root.setAttribute('data-size', sizes.join(' '));
    const tiny = sizes.includes('tiny');
    if (tiny !== this._tiny) {
      this._tiny = tiny;
      this._renderChannels();
    } else this._updateChannelOverflow();
    for (const c of [this.ui.insetCanvas, this.ui.navCanvas]) {
      const r = c.getBoundingClientRect();
      if (r.width > 0) {
        c.width = Math.round(r.width * dpr);
        c.height = Math.round(r.height * dpr);
      }
    }
    if (!this._sized && rect.width > 1 && rect.height > 1) {
      this._sized = true;
      this.fitAll();
    }
    this.requestRender();
  }

  // -------------------------------------------------------------- rendering
  requestRender() {
    if (this._destroyed) return;
    this._dirty = true;
    if (this._raf) return;
    this._raf = requestAnimationFrame(() => {
      this._raf = 0;
      if (this._dirty) this._render();
    });
  }

  _computeTransformBox() {
    const store = this.store;
    if (store.selectedKeys.size < 2) {
      this.state.transformBox = null;
      this.state.transformBoxWorld = null;
      return;
    }
    let tMin = Infinity;
    let tMax = -Infinity;
    let vMin = Infinity;
    let vMax = -Infinity;
    for (const ch of store.channels) {
      if (!ch.visible) continue;
      const tf = channelTransform(ch, this.mode, this.normalize);
      for (let i = 0; i < ch.keys.length; i++) {
        const k = ch.keys[i];
        if (!store.selectedKeys.has(k.id)) continue;
        const pv = keyPlotValue(ch, i, this.mode, tf);
        tMin = Math.min(tMin, k.t);
        tMax = Math.max(tMax, k.t);
        vMin = Math.min(vMin, pv);
        vMax = Math.max(vMax, pv);
      }
    }
    if (!Number.isFinite(tMin)) {
      this.state.transformBox = null;
      this.state.transformBoxWorld = null;
      return;
    }
    const pad = 12;
    const vp = this.vp;
    this.state.transformBoxWorld = { tMin, tMax, vMin, vMax };
    this.state.transformBox = { x0: vp.xOf(tMin) - pad, x1: vp.xOf(tMax) + pad, y0: vp.yOf(vMax) - pad, y1: vp.yOf(vMin) + pad };
  }

  _render() {
    this._dirty = false;
    const ctx = this.canvas.getContext('2d');
    if (!ctx) return;
    this._computeTransformBox();
    render(ctx, {
      vp: this.vp,
      store: this.store,
      mode: this.mode,
      normalize: this.normalize,
      theme: this.theme,
      hover: this.state.hover,
      drag: this.interaction?.drag,
      playhead: this.playhead,
      timeFormat: this.options.timeFormat,
      showHandles: this.options.showHandles,
      dpr: this.dpr || 1,
      boxSelect: this.state.boxSelect,
      ghost: this.state.ghost,
      snapLine: this.state.snapLine,
      blockedAt: this.state.blockedAt,
      transformBox: this.state.transformBox,
      pulse: this.state.pulse,
    });
    // inset
    if (this.options.showEasingInset && this.ui.insetCanvas.width) {
      const segs = this.store.targetSegments();
      const seg = segs[0];
      const easing = seg ? segmentToEasing(seg.k0, seg.k1) : null;
      const ictx = this.ui.insetCanvas.getContext('2d');
      renderEasingInset(ictx, {
        width: this.ui.insetCanvas.width / (this.dpr || 1),
        height: this.ui.insetCanvas.height / (this.dpr || 1),
        dpr: this.dpr || 1,
        theme: this.theme,
        easing,
        color: seg ? seg.channel.color : this.theme.accent,
        hot: this.state.insetHot,
      });
      let labelText = '';
      if (easing) {
        const s = this.strings;
        if (easing.interp === INTERP.HOLD) labelText = s.presets.hold;
        else if (easing.interp === INTERP.LINEAR) labelText = s.presets.linear;
        else {
          const id = matchEasingPreset(easing.cp);
          labelText = id ? s.presets[id] || id : s.presets.custom;
        }
        if (segs.length > 1) labelText += ` ×${segs.length}`;
      }
      this.ui.insetLabel.textContent = labelText;
    }
    // navigator
    if (this.options.showNavigator && this.ui.navCanvas.width) {
      const nctx = this.ui.navCanvas.getContext('2d');
      renderNavigator(nctx, {
        width: this.ui.navCanvas.width / (this.dpr || 1),
        height: this.ui.navCanvas.height / (this.dpr || 1),
        dpr: this.dpr || 1,
        theme: this.theme,
        store: this.store,
        vp: this.vp,
        mode: this.mode,
        normalize: this.normalize,
        extent: this.navExtent(),
      });
    }
    // zoom slider reflects the viewport
    const v = (Math.log(this.vp.pxPerSec) - Math.log(0.5)) / (Math.log(60000) - Math.log(0.5));
    const sliderVal = String(Math.round(Math.min(1, Math.max(0, v)) * 1000));
    if (this.ui.zoom.value !== sliderVal && document.activeElement !== this.ui.zoom) this.ui.zoom.value = sliderVal;
    // right status: visible frames / fps
    const vf = visibleFrames(this.store.range, this.store.fps);
    const fpsLabel = this.store.fps.den === 1 ? `${this.store.fps.num}` : `${this.store.fps.num}/${this.store.fps.den}`;
    this.ui.statusRight.textContent = `${fpsLabel} fps · ${vf.count} ${this.strings.frame.toLowerCase()}${vf.count === 1 ? '' : 's'} · ${formatTime(this.store.range.end, this.store.fps, this.options.timeFormat)}`;
  }

  onViewChanged(fromSlider = false) {
    void fromSlider;
    this.requestRender();
    this.events.emit('viewchange', { t0: this.vp.t0, t1: this.vp.t1, pxPerSec: this.vp.pxPerSec });
  }

  pulseKey(keyId) {
    const reduced = this.options.reducedMotion ?? (window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches);
    if (reduced) return;
    const start = performance.now();
    const step = (now) => {
      const p = Math.min(1, (now - start) / 180);
      this.state.pulse = { keyId, amount: Math.sin(p * Math.PI) };
      this.requestRender();
      if (p < 1 && !this._destroyed) requestAnimationFrame(step);
      else {
        this.state.pulse = null;
        this.requestRender();
      }
    };
    requestAnimationFrame(step);
  }

  // ------------------------------------------------------------ status/cursor
  setCursor(name) {
    if (name) this.stage.setAttribute('data-cursor', name);
    else this.stage.removeAttribute('data-cursor');
  }

  setStatusMain(text) {
    this.ui.statusMain.textContent = text;
  }

  setStatusWarn(text) {
    this.ui.statusWarn.textContent = text;
  }

  setStatusHover(hit) {
    if (this.interaction?.drag) return;
    if (hit.type === 'empty' || hit.type === 'ruler') {
      const info = this.store.selectionInfo();
      if (!info.keys.length && !info.segment) {
        const t = this.vp.tOf(this.interaction.lastPointer.x);
        this.setStatusMain(`${formatTime(t, this.store.fps, this.options.timeFormat)}`);
      }
    }
  }

  // ----------------------------------------------------------------- menus
  closeMenus() {
    if (this._menu) {
      this._menu.remove();
      this._menu = null;
      document.removeEventListener('pointerdown', this._menuOutside, true);
    }
  }

  _showMenu(x, y, build) {
    this.closeMenus();
    const menu = el('div', { class: 'ge-menu', role: 'menu' });
    build(menu);
    document.body.appendChild(menu);
    const vw = window.innerWidth;
    const vh = window.innerHeight;
    const r = menu.getBoundingClientRect();
    menu.style.left = `${Math.max(4, Math.min(x, vw - r.width - 4))}px`;
    menu.style.top = `${Math.max(4, Math.min(y, vh - r.height - 4))}px`;
    this._menu = menu;
    this._menuOutside = (e) => {
      if (!menu.contains(e.target)) this.closeMenus();
    };
    document.addEventListener('pointerdown', this._menuOutside, true);
    menu.addEventListener('keydown', (e) => {
      if (e.key === 'Escape') {
        this.closeMenus();
        this.root.focus();
      }
      e.stopPropagation();
    });
    const first = menu.querySelector('button:not(:disabled)');
    if (first) first.focus();
    return menu;
  }

  _menuItem(label, onClick, { checked = null, kbd = '', disabled = false } = {}) {
    const item = el(
      'button',
      {
        class: 'ge-menu-item',
        type: 'button',
        role: checked == null ? 'menuitem' : 'menuitemcheckbox',
        'aria-checked': checked == null ? null : String(checked),
        disabled,
      },
      [el('span', { text: label }), kbd ? el('kbd', { text: kbd }) : null],
    );
    item.addEventListener('click', () => {
      this.closeMenus();
      onClick();
      this.root.focus({ preventScroll: true });
      this.requestRender();
    });
    return item;
  }

  openEasingMenu(anchorEl) {
    const r = anchorEl.getBoundingClientRect();
    const segs = this.store.targetSegments();
    const s = this.strings;
    this._showMenu(r.left, r.bottom + 4, (menu) => {
      menu.appendChild(el('div', { class: 'ge-menu-title', text: `${s.easing}${segs.length ? ` · ${segs.length}` : ''}` }));
      const grid = el('div', { class: 'ge-menu-grid' });
      for (const p of EASING_PRESETS)
        grid.appendChild(this._menuItem(s.presets[p.id] || p.id, () => this.applyEasing(p.id), { disabled: !segs.length }));
      menu.appendChild(grid);
      menu.appendChild(el('div', { class: 'ge-menu-sep' }));
      const ids = this._inspectorTargets();
      menu.appendChild(this._menuItem(s.interpHold, () => this.store.setInterp(ids, INTERP.HOLD), { disabled: !ids.length, kbd: '3' }));
      menu.appendChild(this._menuItem(s.interpLinear, () => this.store.setInterp(ids, INTERP.LINEAR), { disabled: !ids.length, kbd: '2' }));
    });
  }

  openContextMenu(x, y, hit) {
    const s = this.strings;
    const store = this.store;
    const sel = [...store.selectedKeys];
    const segs = store.targetSegments();
    this._showMenu(x, y, (menu) => {
      if (hit.type === 'segment' || hit.type === 'empty') {
        const chId = hit.type === 'segment' ? hit.channelId : store.activeChannelId;
        const ch = store.channel(chId);
        menu.appendChild(
          this._menuItem(
            `${s.insertKey}${ch ? ` · ${ch.name}` : ''}`,
            () => {
              if (!ch) return;
              const tf = channelTransform(ch, this.mode, this.normalize);
              const value = hit.type === 'empty' && this.mode !== 'speed' ? hit.v / tf.scale + tf.offset : undefined;
              const k = store.insertKey(chId, hit.t, { snap: this.options.snapFrames, value });
              if (k) {
                store.select([k.id], 'replace');
                this.pulseKey(k.id);
              }
            },
            { disabled: !ch || ch.locked },
          ),
        );
        menu.appendChild(el('div', { class: 'ge-menu-sep' }));
      }
      if (segs.length) {
        menu.appendChild(el('div', { class: 'ge-menu-title', text: `${s.easing} · ${segs.length}` }));
        const grid = el('div', { class: 'ge-menu-grid' });
        for (const p of EASING_PRESETS) grid.appendChild(this._menuItem(s.presets[p.id] || p.id, () => this.applyEasing(p.id)));
        menu.appendChild(grid);
        menu.appendChild(el('div', { class: 'ge-menu-sep' }));
      }
      const ids = this._inspectorTargets();
      if (ids.length) {
        const refs = ids.map((id) => store.keyRef(id)).filter(Boolean);
        const interp = refs.every((r) => r.key.interp === refs[0].key.interp) ? refs[0].key.interp : null;
        const tang = refs.every((r) => r.key.tangents === refs[0].key.tangents) ? refs[0].key.tangents : null;
        menu.appendChild(el('div', { class: 'ge-menu-title', text: s.interp }));
        menu.appendChild(
          this._menuItem(s.interpBezier, () => store.setInterp(ids, INTERP.BEZIER), { checked: interp === INTERP.BEZIER, kbd: '1' }),
        );
        menu.appendChild(
          this._menuItem(s.interpLinear, () => store.setInterp(ids, INTERP.LINEAR), { checked: interp === INTERP.LINEAR, kbd: '2' }),
        );
        menu.appendChild(
          this._menuItem(s.interpHold, () => store.setInterp(ids, INTERP.HOLD), { checked: interp === INTERP.HOLD, kbd: '3' }),
        );
        menu.appendChild(el('div', { class: 'ge-menu-title', text: s.tangents }));
        menu.appendChild(this._menuItem(s.tangentsAuto, () => store.setTangents(ids, TANGENTS.AUTO), { checked: tang === TANGENTS.AUTO }));
        menu.appendChild(
          this._menuItem(s.tangentsUnified, () => store.setTangents(ids, TANGENTS.UNIFIED), { checked: tang === TANGENTS.UNIFIED }),
        );
        menu.appendChild(
          this._menuItem(s.tangentsBroken, () => store.setTangents(ids, TANGENTS.BROKEN), { checked: tang === TANGENTS.BROKEN }),
        );
        menu.appendChild(el('div', { class: 'ge-menu-sep' }));
      }
      menu.appendChild(this._menuItem(s.copy, () => this.copySelection(), { disabled: !sel.length, kbd: 'Ctrl+C' }));
      menu.appendChild(this._menuItem(s.paste, () => this.pasteAtPlayhead(), { disabled: !this.clipboard, kbd: 'Ctrl+V' }));
      menu.appendChild(this._menuItem(s.duplicate, () => store.duplicateKeys(sel), { disabled: !sel.length, kbd: 'Ctrl+D' }));
      menu.appendChild(this._menuItem(s.deleteKeys, () => store.deleteKeys(sel), { disabled: !sel.length, kbd: 'Del' }));
      menu.appendChild(el('div', { class: 'ge-menu-sep' }));
      menu.appendChild(this._menuItem(s.selectAll, () => store.selectAll(), { kbd: 'Ctrl+A' }));
      menu.appendChild(this._menuItem(s.fitAll, () => this.fitAll(), { kbd: 'F' }));
      menu.appendChild(this._menuItem(s.fitSelected, () => this.fitSelected(), { disabled: !sel.length, kbd: 'Shift+F' }));
    });
  }

  toggleHelp() {
    if (this._help) {
      this._help.remove();
      this._help = null;
      return;
    }
    const s = this.strings;
    const table = el(
      'table',
      {},
      s.shortcuts.map(([k, d]) => el('tr', {}, [el('td', { text: k }), el('td', { text: d })])),
    );
    const close = el('button', { class: 'ge-btn ge-btn--icon', type: 'button', 'aria-label': 'Close', html: ICONS.close });
    close.addEventListener('click', () => this.toggleHelp());
    this._help = el('div', { class: 'ge-help', role: 'dialog', 'aria-label': s.help }, [
      el('div', { class: 'ge-help-head' }, [el('span', { text: s.help }), close]),
      table,
    ]);
    this.root.appendChild(this._help);
  }

  // ------------------------------------------------------------- commands
  applyEasing(idOrCp) {
    const cp = Array.isArray(idOrCp) ? idOrCp : EASING_PRESETS.find((p) => p.id === idOrCp)?.cp;
    if (!cp) return 0;
    return this.store.applyEasing(cp);
  }

  copySelection() {
    const clip = this.store.copyKeys();
    if (clip) this.clipboard = clip;
    return clip;
  }

  pasteAtPlayhead() {
    if (!this.clipboard) return [];
    return this.store.pasteKeys(this.clipboard, this.playhead ?? this.store.range.start, { snap: this.options.snapFrames });
  }

  undo() {
    if (this.options.undo === 'internal') this.store.undo();
    else this.events.emit('request-undo', {});
  }

  redo() {
    if (this.options.undo === 'internal') this.store.redo();
    else this.events.emit('request-redo', {});
  }

  valueNudgeStep() {
    // One minor grid step at the current vertical zoom, in plot units.
    return chooseValueStep(this.vp.pxPerUnit, 44).minor;
  }

  jumpToKey(dir) {
    const times = [];
    for (const ch of this.store.channels) if (ch.visible) for (const k of ch.keys) times.push(k.t);
    if (!times.length) return;
    times.sort((a, b) => a - b);
    const cur = this.playhead ?? 0;
    const eps = 1e-6;
    let target = null;
    if (dir > 0) target = times.find((t) => t > cur + eps);
    else for (const t of times) if (t < cur - eps) target = t;
    if (target == null) return;
    this.setPlayhead(target, 'user');
    this.vp.ensureTimeVisible(target);
    this.onViewChanged();
  }

  // ------------------------------------------------------------- view ops
  _visibleBounds(keysFilter) {
    let tMin = Infinity;
    let tMax = -Infinity;
    let vMin = Infinity;
    let vMax = -Infinity;
    for (const ch of this.store.channels) {
      if (!ch.visible) continue;
      const keys = keysFilter ? ch.keys.filter((k) => keysFilter(ch, k)) : ch.keys;
      if (!keys.length) continue;
      const tf = channelTransform(ch, this.mode, this.normalize);
      if (this.mode === 'speed') {
        for (let i = 0; i < ch.keys.length; i++) {
          if (keysFilter && !keysFilter(ch, ch.keys[i])) continue;
          const sp = keySpeeds(ch, i);
          vMin = Math.min(vMin, sp.in, sp.out);
          vMax = Math.max(vMax, sp.in, sp.out);
        }
        // Sample speed extrema between keys.
        for (let i = 0; i < ch.keys.length - 1; i++) {
          for (let sIdx = 1; sIdx < 12; sIdx++) {
            const t = ch.keys[i].t + ((ch.keys[i + 1].t - ch.keys[i].t) * sIdx) / 12;
            const sp = this.store.speedChannel(ch.id, t);
            vMin = Math.min(vMin, sp);
            vMax = Math.max(vMax, sp);
          }
        }
      } else {
        const b = keysFilter ? keysBounds(keys) : curveBounds(ch.keys, 12);
        vMin = Math.min(vMin, (b.vMin - tf.offset) * tf.scale);
        vMax = Math.max(vMax, (b.vMax - tf.offset) * tf.scale);
      }
      const kb = keysBounds(keys);
      tMin = Math.min(tMin, kb.tMin);
      tMax = Math.max(tMax, kb.tMax);
    }
    if (!Number.isFinite(tMin)) return null;
    return { tMin, tMax, vMin, vMax };
  }

  fitAll() {
    const b = this._visibleBounds(null);
    const r = this.store.range;
    const tMin = Math.min(b ? b.tMin : r.start, r.start);
    const tMax = Math.max(b ? b.tMax : r.end, r.end);
    this.vp.fitTime(tMin, tMax, 40);
    if (b) this.vp.fitValue(b.vMin, b.vMax, 36);
    else this.vp.fitValue(-1, 1, 36);
    this.onViewChanged();
  }

  fitSelected() {
    const sel = this.store.selectedKeys;
    if (!sel.size) return this.fitAll();
    const b = this._visibleBounds((ch, k) => sel.has(k.id));
    if (!b) return;
    this.vp.fitTime(b.tMin, b.tMax, 80);
    this.vp.fitValue(b.vMin, b.vMax, 60);
    this.onViewChanged();
  }

  fitRange() {
    const r = this.store.range;
    this.vp.fitTime(r.start, r.end, 24);
    const b = this._visibleBounds(null);
    if (b) this.vp.fitValue(b.vMin, b.vMax, 36);
    this.onViewChanged();
  }

  fitChannels(channels) {
    const ids = new Set(channels.map((c) => c.id));
    const b = this._visibleBounds((ch) => ids.has(ch.id));
    if (!b) return;
    this.vp.fitTime(b.tMin, b.tMax, 48);
    this.vp.fitValue(b.vMin, b.vMax, 36);
    this.onViewChanged();
  }

  // --------------------------------------------------------------- public
  on(type, fn) {
    return this.events.on(type, fn);
  }

  setDocument(doc) {
    this.store.setDoc(doc);
  }

  getDocument() {
    return this.store.exportDoc();
  }

  setPlayhead(t, source = 'host') {
    if (!Number.isFinite(t)) return;
    if (this.playhead === t) return;
    this.playhead = t;
    this.requestRender();
    this.events.emit('playhead', { time: t, source });
  }

  getPlayhead() {
    return this.playhead;
  }

  setMode(mode) {
    if (mode !== 'value' && mode !== 'speed') return;
    if (this.mode === mode) return;
    this.mode = mode;
    this.ui.modeValue.setAttribute('aria-pressed', String(mode === 'value'));
    this.ui.modeSpeed.setAttribute('aria-pressed', String(mode === 'speed'));
    this.ui.normalize.disabled = mode === 'speed';
    this.fitAll();
    this._syncInspector();
    this.events.emit('modechange', { mode });
  }

  getMode() {
    return this.mode;
  }

  setNormalize(on) {
    this.normalize = !!on;
    this.ui.normalize.setAttribute('aria-pressed', String(this.normalize));
    this.fitAll();
  }

  setOption(name, value) {
    this.options[name] = value;
    const ui = this.ui;
    if (name === 'snapFrames') ui.snapFrames.setAttribute('aria-pressed', String(!!value));
    if (name === 'snapKeys') ui.snapKeys.setAttribute('aria-pressed', String(!!value));
    if (name === 'showHandles') {
      ui.handles.setAttribute('aria-pressed', String(value !== 'none'));
      ui.handles.title = `${this.strings.handles}: ${this._handlesLabel()}`;
      ui.handles.setAttribute('aria-label', ui.handles.title);
    }
    if (name === 'timeFormat') {
      ui.timeFormat.value = value;
      this._syncInspector();
    }
    if (name === 'collision') this.store.options.collision = value;
    if (name === 'theme') this._applyTheme();
    this.requestRender();
  }

  setFps(fps) {
    this.store.setFps(fps);
  }

  setRange(range) {
    this.store.setRange(range);
  }

  selectKeys(ids, mode = 'replace') {
    this.store.select(ids, mode);
  }

  getSelection() {
    return { keys: [...this.store.selectedKeys], segment: this.store.selectedSegment };
  }

  setLocale(locale) {
    this.options.locale = locale;
    this.strings = resolveStrings(locale);
    // Rebuild the shell to refresh labels; document and view are untouched.
    const vpState = this.vp.snapshot();
    this.interaction.destroy();
    this.root.remove();
    this._buildDom();
    this.theme = readTheme(this.root);
    this.interaction = new Interaction(this);
    this._applyTheme();
    this._ro.disconnect();
    this._ro.observe(this.root);
    this._ro.observe(this.stage);
    this._onResize();
    this.vp.restore(vpState);
    this._syncInspector();
    this._updateHistoryButtons();
    this.requestRender();
  }

  setTheme(theme) {
    this.options.theme = theme;
    this._applyTheme();
  }

  _applyTheme() {
    let t = this.options.theme;
    if (t === 'auto') t = window.matchMedia && window.matchMedia('(prefers-color-scheme: light)').matches ? 'light' : 'dark';
    this.root.setAttribute('data-theme', t === 'light' ? 'light' : 'dark');
    this.theme = readTheme(this.root);
    this.requestRender();
  }

  resize() {
    this._onResize();
  }

  destroy() {
    this._destroyed = true;
    if (this._raf) cancelAnimationFrame(this._raf);
    this.closeMenus();
    this.interaction?.destroy();
    this._ro?.disconnect();
    for (const off of this._offs || []) off();
    this.events.clear();
    this.root.remove();
  }
}

export { evalKeys, snapToFrame, formatTime };
