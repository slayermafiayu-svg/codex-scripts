// Pointer, wheel and keyboard state machine for the main canvas.
// Direct manipulation follows the pointer immediately: no easing, no spring.
// Every drag is one store transaction; live updates are re-applied from the
// transaction start so nothing drifts or accumulates rounding.

import { INTERP, TANGENTS, findSegmentIndex, evalKeys } from '../core/curve.js';
import { snapToFrame, framesToSeconds, formatTime, formatValue, timeEpsilon } from '../core/time.js';
import {
  channelTransform,
  plotValueAt,
  keyPlotValue,
  handleScreenPositions,
  keySpeeds,
  HIT_KEY,
  HIT_HANDLE,
  HIT_SEGMENT,
  BOX_HANDLE,
} from './renderer.js';

const DRAG_THRESHOLD = 3;
const SNAP_PX = 7;

function isEditableTarget(t) {
  if (!t) return false;
  const tag = t.tagName;
  return tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT' || t.isContentEditable;
}

export class Interaction {
  /** @param {import('./editor.js').GraphEditor} ed */
  constructor(ed) {
    this.ed = ed;
    this.canvas = ed.canvas;
    this.press = null;
    this.drag = null;
    this.spaceHeld = false;
    this.lastPointer = { x: 0, y: 0 };
    this._bound = [];
    const on = (el, type, fn, opts) => {
      el.addEventListener(type, fn, opts);
      this._bound.push(() => el.removeEventListener(type, fn, opts));
    };
    on(this.canvas, 'pointerdown', (e) => this.onPointerDown(e));
    on(this.canvas, 'pointermove', (e) => this.onPointerMove(e));
    on(this.canvas, 'pointerup', (e) => this.onPointerUp(e));
    on(this.canvas, 'pointercancel', (e) => this.onPointerCancel(e));
    on(this.canvas, 'pointerleave', () => this.onPointerLeave());
    on(this.canvas, 'dblclick', (e) => this.onDoubleClick(e));
    on(this.canvas, 'wheel', (e) => this.onWheel(e), { passive: false });
    on(this.canvas, 'contextmenu', (e) => this.onContextMenu(e));
    on(ed.root, 'keydown', (e) => this.onKeyDown(e));
    on(ed.root, 'keyup', (e) => this.onKeyUp(e));
    on(window, 'blur', () => this.cancelDrag());
  }

  destroy() {
    for (const off of this._bound) off();
    this._bound.length = 0;
  }

  // ------------------------------------------------------------ geometry
  localPoint(e) {
    const r = this.canvas.getBoundingClientRect();
    return { x: e.clientX - r.left, y: e.clientY - r.top };
  }

  showHandlesFor(ch, k) {
    const { store, options } = this.ed;
    if (options.showHandles === 'none') return false;
    if (options.showHandles === 'all') return true;
    if (store.selectedKeys.has(k.id)) return true;
    const hv = this.ed.state.hover;
    if (hv && hv.keyId === k.id && (hv.type === 'key' || hv.type === 'handle')) return true;
    if (store.selectedSegment && store.selectedSegment.channelId === ch.id) {
      const ref = store.keyRef(store.selectedSegment.keyId);
      if (ref && (ref.key.id === k.id || ref.channel.keys[ref.index + 1]?.id === k.id)) return true;
    }
    return false;
  }

  /** Find what is under the pointer. Priority: handle > key > box edge > segment > empty. */
  hitTest(x, y) {
    const { store, vp, mode, normalize } = this.ed;
    if (y < vp.plotTop) return { type: 'ruler' };
    const channels = store.doc.channels.filter((c) => c.visible && !c.locked);
    const tfs = new Map(channels.map((c) => [c.id, channelTransform(c, mode, normalize)]));
    let best = null;
    // handles
    for (const ch of channels) {
      const tf = tfs.get(ch.id);
      for (let i = 0; i < ch.keys.length; i++) {
        const k = ch.keys[i];
        if (!this.showHandlesFor(ch, k)) continue;
        const pos = handleScreenPositions(vp, ch, i, mode, tf);
        for (const side of ['in', 'out']) {
          const p = pos[side];
          if (!p) continue;
          const d = Math.hypot(p.x - x, p.y - y);
          if (d <= HIT_HANDLE && (!best || d < best.d)) best = { type: 'handle', keyId: k.id, channelId: ch.id, side, d };
        }
      }
    }
    if (best) return best;
    // keys (prefer selected keys and the active channel on ties)
    for (const ch of channels) {
      const tf = tfs.get(ch.id);
      for (let i = 0; i < ch.keys.length; i++) {
        const k = ch.keys[i];
        const kx = vp.xOf(k.t);
        if (Math.abs(kx - x) > HIT_KEY + 2) continue;
        const ky = vp.yOf(keyPlotValue(ch, i, mode, tf));
        let d = Math.hypot(kx - x, ky - y);
        if (mode === 'speed') {
          const yIn = vp.yOf(keySpeeds(ch, i).in);
          d = Math.min(d, Math.hypot(kx - x, yIn - y));
        }
        if (d > HIT_KEY) continue;
        const score = d - (store.selectedKeys.has(k.id) ? 2 : 0) - (ch.id === store.activeChannelId ? 1 : 0);
        if (!best || score < best.score) best = { type: 'key', keyId: k.id, channelId: ch.id, d, score };
      }
    }
    if (best) return best;
    // transform box edges
    const box = this.ed.state.transformBox;
    if (box) {
      const hs = BOX_HANDLE + 3;
      const midX = (box.x0 + box.x1) / 2;
      const midY = (box.y0 + box.y1) / 2;
      const edges = [
        ['left', box.x0, midY],
        ['right', box.x1, midY],
      ];
      if (mode !== 'speed' && !normalize) edges.push(['top', midX, box.y0], ['bottom', midX, box.y1]);
      for (const [edge, ex, ey] of edges) {
        if (Math.abs(ex - x) <= hs && Math.abs(ey - y) <= hs) return { type: 'boxEdge', edge };
      }
    }
    // segments
    const t = vp.tOf(x);
    for (const ch of channels) {
      if (ch.keys.length < 2) continue;
      const first = ch.keys[0];
      const last = ch.keys[ch.keys.length - 1];
      if (t < first.t || t > last.t) continue;
      const tf = tfs.get(ch.id);
      let d = Infinity;
      for (const dx of [0, -2, 2, -4, 4]) {
        const tt = vp.tOf(x + dx);
        if (tt < first.t || tt > last.t) continue;
        const sy = vp.yOf(plotValueAt(ch, tt, mode, tf));
        d = Math.min(d, Math.hypot(dx, sy - y));
      }
      if (d <= HIT_SEGMENT && (!best || d < best.d)) {
        const idx = Math.min(ch.keys.length - 2, Math.max(0, findSegmentIndex(ch.keys, t)));
        best = { type: 'segment', channelId: ch.id, keyId: ch.keys[idx].id, t, d };
      }
    }
    if (best) return best;
    return { type: 'empty', t, v: vp.vOf(y) };
  }

  // ------------------------------------------------------------ pointer
  onPointerDown(e) {
    const ed = this.ed;
    ed.closeMenus();
    if (e.button === 2) return; // handled by contextmenu
    ed.root.focus({ preventScroll: true });
    const p = this.localPoint(e);
    this.lastPointer = p;
    if (e.button === 1 || this.spaceHeld) {
      e.preventDefault();
      this.canvas.setPointerCapture(e.pointerId);
      this.drag = { type: 'pan', pointerId: e.pointerId };
      ed.setCursor('grabbing');
      return;
    }
    if (e.button !== 0) return;
    const hit = this.hitTest(p.x, p.y);
    this.press = {
      x: p.x,
      y: p.y,
      hit,
      shift: e.shiftKey,
      ctrl: e.ctrlKey || e.metaKey,
      alt: e.altKey,
      pointerId: e.pointerId,
      moved: false,
    };
    this.canvas.setPointerCapture(e.pointerId);
    const store = ed.store;
    switch (hit.type) {
      case 'ruler':
        this.drag = { type: 'scrub' };
        this.scrubTo(p.x, e);
        break;
      case 'key': {
        const selected = store.selectedKeys.has(hit.keyId);
        if (this.press.shift) store.select([hit.keyId], 'add');
        else if (this.press.ctrl) store.select([hit.keyId], 'toggle');
        else if (!selected) store.select([hit.keyId], 'replace');
        else store.activeChannelId = hit.channelId;
        break;
      }
      case 'segment':
        if (!this.press.shift && !this.press.ctrl) store.selectSegment(hit.channelId, hit.keyId);
        break;
      default:
        break;
    }
    ed.requestRender();
  }

  onPointerMove(e) {
    const p = this.localPoint(e);
    const ed = this.ed;
    if (this.drag) {
      this.updateDrag(p, e);
      this.lastPointer = p;
      return;
    }
    if (this.press) {
      const dist = Math.hypot(p.x - this.press.x, p.y - this.press.y);
      if (dist >= DRAG_THRESHOLD) {
        this.press.moved = true;
        this.startDrag(p, e);
        if (this.drag) this.updateDrag(p, e);
      }
      this.lastPointer = p;
      return;
    }
    this.lastPointer = p;
    this.updateHover(p, e);
  }

  onPointerUp(e) {
    const ed = this.ed;
    const p = this.localPoint(e);
    if (this.drag) {
      this.finishDrag(p, e);
    } else if (this.press) {
      const { hit, shift, ctrl, alt } = this.press;
      if (hit.type === 'empty' && !shift && !ctrl && !alt) ed.store.clearSelection();
      else if (hit.type === 'key' && !shift && !ctrl && ed.store.selectedKeys.size > 1) ed.store.select([hit.keyId], 'replace');
    }
    this.press = null;
    this.drag = null;
    try {
      this.canvas.releasePointerCapture(e.pointerId);
    } catch {
      /* already released */
    }
    this.updateHover(p, e);
    ed.requestRender();
  }

  onPointerCancel() {
    this.cancelDrag();
  }

  onPointerLeave() {
    if (!this.press && !this.drag && this.ed.state.hover) {
      this.ed.state.hover = null;
      this.ed.setCursor('');
      this.ed.requestRender();
    }
  }

  cancelDrag() {
    const ed = this.ed;
    if (this.drag) {
      if (this.drag.tx) ed.store.cancel();
      if (this.drag.type === 'pan' && this.drag.vpStart) ed.vp.restore(this.drag.vpStart);
      this.drag = null;
      ed.state.boxSelect = null;
      ed.state.ghost = null;
      ed.state.snapLine = null;
      ed.state.blockedAt = null;
      ed.setStatusWarn('');
    }
    this.press = null;
    ed.setCursor('');
    ed.requestRender();
  }

  // ------------------------------------------------------------- hover
  updateHover(p, e) {
    const ed = this.ed;
    const hit = this.hitTest(p.x, p.y);
    const { store, vp, mode, normalize, options } = ed;
    const fps = store.fps;
    let tooltip = null;
    let cursor = '';
    if (hit.type === 'key') {
      const ref = store.keyRef(hit.keyId);
      if (ref) {
        const unit = ref.channel.unit ? ` ${ref.channel.unit}` : '';
        const lines = [
          { text: ref.channel.name, color: ref.channel.color },
          { text: `${formatTime(ref.key.t, fps, options.timeFormat, { showSubframe: true })}  ·  ${formatValue(ref.key.v, 3)}${unit}` },
        ];
        if (mode === 'speed') {
          const sp = keySpeeds(ref.channel, ref.index);
          lines.push({ text: `in ${formatValue(sp.in, 2)}${ed.strings.perSecond}   out ${formatValue(sp.out, 2)}${ed.strings.perSecond}` });
        }
        const outside = ref.key.t < store.range.start - 1e-9 || ref.key.t > store.range.end + 1e-9;
        if (outside) lines.push({ text: ed.strings.outsideRange, color: ed.theme.range });
        tooltip = { x: p.x, y: p.y, lines };
      }
      cursor = 'grab';
    } else if (hit.type === 'handle') {
      const ref = store.keyRef(hit.keyId);
      if (ref) {
        const h = ref.key[hit.side];
        const speed = h.dt !== 0 ? h.dv / h.dt : 0;
        tooltip = {
          x: p.x,
          y: p.y,
          lines: [
            { text: `${ref.channel.name} · ${hit.side}`, color: ref.channel.color },
            {
              text: `${formatValue(speed, 2)}${ed.strings.perSecond}  ·  ${ed.strings.influence} ${Math.round(this.influenceOf(ref, hit.side) * 100)}%  ·  ${ed.strings[`tangents${ref.key.tangents[0].toUpperCase()}${ref.key.tangents.slice(1)}`] || ref.key.tangents}`,
            },
          ],
        };
      }
      cursor = 'move';
    } else if (hit.type === 'boxEdge') {
      cursor = hit.edge === 'left' || hit.edge === 'right' ? 'ew' : 'ns';
    } else if (hit.type === 'segment') {
      const ch = store.channel(hit.channelId);
      const t = options.snapFrames && !(e && (e.ctrlKey || e.metaKey)) ? snapToFrame(hit.t, fps) : hit.t;
      const v = evalKeys(ch.keys, t, ch.extrapolation);
      const tf = channelTransform(ch, mode, normalize);
      const lines = [
        { text: ch.name, color: ch.color },
        {
          text: `${formatTime(t, fps, options.timeFormat)}  ·  ${mode === 'speed' ? `${formatValue(plotValueAt(ch, t, 'speed', tf), 2)}${ed.strings.perSecond}` : formatValue(v, 3) + (ch.unit ? ` ${ch.unit}` : '')}`,
        },
      ];
      tooltip = { x: p.x, y: p.y, lines };
      cursor = 'pointer';
    } else if (hit.type === 'ruler') {
      cursor = 'col';
    } else if (hit.type === 'empty' && e && (e.ctrlKey || e.metaKey)) {
      cursor = 'copy';
    }
    ed.state.hover = { ...hit, tooltip };
    ed.setCursor(cursor);
    ed.setStatusHover(hit);
    ed.requestRender();
  }

  // ------------------------------------------------------------- drags
  startDrag(p, e) {
    const ed = this.ed;
    const { store, vp } = ed;
    const { hit, shift, alt } = this.press;
    switch (hit.type) {
      case 'empty':
      case 'segment':
        if (hit.type === 'segment' && !shift && !alt) {
          // Dragging from a curve selects nothing else; treat as box select start too.
        }
        this.drag = { type: 'box', mode: shift ? 'add' : alt ? 'remove' : this.press.ctrl ? 'toggle' : 'replace' };
        ed.state.boxSelect = { x0: this.press.x, y0: this.press.y, x1: p.x, y1: p.y };
        break;
      case 'key': {
        const ids = [...store.selectedKeys];
        if (!ids.length) return;
        const refs = ids.map((id) => store.keyRef(id)).filter(Boolean);
        store.begin('move keys');
        const anchor = refs.find((r) => r.key.id === hit.keyId) || refs[0];
        this.drag = {
          type: 'move',
          tx: true,
          ids,
          anchorId: anchor.key.id,
          anchorT: anchor.key.t,
          anchorV: anchor.key.v,
          startTfs: new Map(refs.map((r) => [r.channel.id, channelTransform(r.channel, ed.mode, ed.normalize)])),
          others: this.collectSnapTimes(new Set(ids)),
        };
        ed.state.ghost = store.transactionStart();
        ed.setCursor('grabbing');
        break;
      }
      case 'handle': {
        const ref = store.keyRef(hit.keyId);
        if (!ref) return;
        store.begin('edit tangent');
        this.drag = {
          type: 'handle',
          tx: true,
          keyId: hit.keyId,
          side: hit.side,
          channelId: ref.channel.id,
          tf: channelTransform(ref.channel, ed.mode, ed.normalize),
        };
        ed.state.ghost = store.transactionStart();
        break;
      }
      case 'boxEdge': {
        const ids = [...store.selectedKeys];
        const box = ed.state.transformBoxWorld;
        if (!ids.length || !box) return;
        store.begin('scale keys');
        this.drag = { type: 'scale', tx: true, ids, edge: hit.edge, box: { ...box } };
        ed.state.ghost = store.transactionStart();
        break;
      }
      default:
        break;
    }
    if (this.drag && this.drag.type !== 'pan') this.drag.vpStart = null;
  }

  collectSnapTimes(excluded) {
    const { store } = this.ed;
    const times = [];
    for (const ch of store.doc.channels) {
      if (!ch.visible) continue;
      for (const k of ch.keys) if (!excluded.has(k.id)) times.push(k.t);
    }
    if (this.ed.playhead != null) times.push(this.ed.playhead);
    times.push(store.range.start, store.range.end);
    return times;
  }

  updateDrag(p, e) {
    const ed = this.ed;
    const { store, vp } = ed;
    const d = this.drag;
    const fps = store.fps;
    switch (d.type) {
      case 'pan': {
        if (!d.vpStart) d.vpStart = vp.snapshot();
        vp.pan(p.x - this.lastPointer.x, p.y - this.lastPointer.y);
        ed.onViewChanged();
        break;
      }
      case 'scrub':
        this.scrubTo(p.x, e);
        break;
      case 'box':
        ed.state.boxSelect.x1 = p.x;
        ed.state.boxSelect.y1 = p.y;
        break;
      case 'move': {
        const ctrl = e.ctrlKey || e.metaKey;
        const dx = p.x - this.press.x;
        const dy = p.y - this.press.y;
        let timeOnly = ed.mode === 'speed';
        let valueOnly = false;
        if (e.shiftKey && ed.mode !== 'speed') {
          if (Math.abs(dx) >= Math.abs(dy)) timeOnly = true;
          else valueOnly = true;
        }
        let dt = dx / vp.pxPerSec;
        const dvPlot = -dy / vp.pxPerUnit;
        let snap = ed.options.snapFrames && !ctrl;
        let snapLine = null;
        if (!timeOnly || true) {
          if (ed.options.snapKeys && !ctrl && !valueOnly) {
            const target = d.anchorT + dt;
            let bestT = null;
            let bestD = SNAP_PX + 1;
            for (const t of d.others) {
              const dist = Math.abs(vp.xOf(t) - vp.xOf(target));
              if (dist < bestD) {
                bestD = dist;
                bestT = t;
              }
            }
            if (bestT != null) {
              dt = bestT - d.anchorT;
              snap = false;
              snapLine = bestT;
            }
          }
        }
        store.restoreTransactionStart();
        const anchorTf = d.startTfs.get(store.keyRef(d.anchorId)?.channel.id) || { offset: 0, scale: 1 };
        const dvScale = new Map();
        for (const [chId, tf] of d.startTfs) dvScale.set(chId, (1 / tf.scale) * anchorTf.scale);
        const dv = dvPlot / anchorTf.scale;
        const res = store.moveKeys(d.ids, dt, dv, { snap, anchorId: d.anchorId, timeOnly, valueOnly, dvScale });
        ed.state.snapLine = snapLine;
        ed.state.blockedAt = res.blocked ? d.anchorT + dt : null;
        ed.setStatusWarn(res.blocked ? ed.strings.blocked : '');
        const anchor = store.keyRef(d.anchorId);
        if (anchor)
          ed.setStatusMain(
            `${formatTime(anchor.key.t, fps, ed.options.timeFormat, { showSubframe: true })}  ·  ${formatValue(anchor.key.v, 3)}  (Δ ${formatTime(res.dt, fps, 'frames')} f, ${formatValue(res.dv, 3)})`,
          );
        break;
      }
      case 'handle': {
        store.restoreTransactionStart();
        const ref = store.keyRef(d.keyId);
        if (!ref) break;
        const k = ref.key;
        const t = vp.tOf(p.x);
        let dt = t - k.t;
        let dv;
        if (ed.mode === 'speed') {
          const speed = vp.vOf(p.y);
          if (d.side === 'in') dt = Math.min(0, dt);
          else dt = Math.max(0, dt);
          dv = speed * dt;
          if (e.shiftKey) dv = 0;
        } else {
          const plotV = vp.vOf(p.y);
          const realV = plotV / d.tf.scale + d.tf.offset;
          dv = realV - k.v;
          if (e.shiftKey) dv = 0;
        }
        store.setHandle(d.keyId, d.side, { dt, dv }, { breakTangents: e.altKey });
        const k2 = store.keyRef(d.keyId)?.key;
        if (k2) {
          const h = k2[d.side];
          const speed = h.dt !== 0 ? h.dv / h.dt : 0;
          ed.setStatusMain(
            `${d.side}  ·  ${formatValue(speed, 3)}${ed.strings.perSecond}  ·  ${ed.strings.influence} ${Math.round(this.influenceOf(ref, d.side) * 100)}%`,
          );
        }
        break;
      }
      case 'scale': {
        store.restoreTransactionStart();
        const b = d.box; // world (time, plot value) at drag start
        let kT = 1;
        let kV = 1;
        let pivotT = b.tMin;
        let pivotV = b.vMin;
        const t = vp.tOf(p.x);
        const v = vp.vOf(p.y);
        if (d.edge === 'left') {
          pivotT = b.tMax;
          kT = (pivotT - t) / Math.max(1e-9, pivotT - b.tMin);
        } else if (d.edge === 'right') {
          pivotT = b.tMin;
          kT = (t - pivotT) / Math.max(1e-9, b.tMax - pivotT);
        } else if (d.edge === 'top') {
          pivotV = b.vMin;
          kV = (v - pivotV) / Math.max(1e-9, b.vMax - pivotV);
        } else if (d.edge === 'bottom') {
          pivotV = b.vMax;
          kV = (pivotV - v) / Math.max(1e-9, pivotV - b.vMin);
        }
        kT = Math.max(0.001, kT);
        store.scaleKeys(d.ids, { pivotT, kT, pivotV, kV, snap: ed.options.snapFrames && !(e.ctrlKey || e.metaKey) });
        ed.setStatusMain(`×${formatValue(kT, 3)} t   ×${formatValue(kV, 3)} v`);
        break;
      }
      default:
        break;
    }
    ed.requestRender();
  }

  influenceOf(ref, side) {
    const keys = ref.channel.keys;
    const i = ref.index;
    const k = ref.key;
    if (side === 'out' && i < keys.length - 1) return Math.abs(k.out.dt) / Math.max(1e-9, keys[i + 1].t - k.t);
    if (side === 'in' && i > 0) return Math.abs(k.in.dt) / Math.max(1e-9, k.t - keys[i - 1].t);
    return 0;
  }

  finishDrag(p, e) {
    const ed = this.ed;
    const { store, vp } = ed;
    const d = this.drag;
    switch (d.type) {
      case 'box': {
        const b = ed.state.boxSelect;
        const x0 = Math.min(b.x0, b.x1);
        const x1 = Math.max(b.x0, b.x1);
        const y0 = Math.min(b.y0, b.y1);
        const y1 = Math.max(b.y0, b.y1);
        const ids = [];
        for (const ch of store.doc.channels) {
          if (!ch.visible || ch.locked) continue;
          const tf = channelTransform(ch, ed.mode, ed.normalize);
          for (let i = 0; i < ch.keys.length; i++) {
            const k = ch.keys[i];
            const kx = vp.xOf(k.t);
            const ky = vp.yOf(keyPlotValue(ch, i, ed.mode, tf));
            if (kx >= x0 && kx <= x1 && ky >= y0 && ky <= y1) ids.push(k.id);
          }
        }
        if (d.mode === 'replace') store.select(ids, 'replace');
        else store.select(ids, d.mode);
        ed.state.boxSelect = null;
        break;
      }
      case 'move':
      case 'handle':
      case 'scale':
        store.commit();
        break;
      default:
        break;
    }
    ed.state.ghost = null;
    ed.state.snapLine = null;
    ed.state.blockedAt = null;
    ed.setStatusWarn('');
    ed.setCursor('');
    this.drag = null;
  }

  scrubTo(x, e) {
    const ed = this.ed;
    let t = ed.vp.tOf(x);
    if (ed.options.snapFrames && !(e && (e.ctrlKey || e.metaKey))) t = snapToFrame(t, ed.store.fps);
    ed.setPlayhead(t, 'user');
  }

  // ------------------------------------------------------- double click
  onDoubleClick(e) {
    const ed = this.ed;
    const p = this.localPoint(e);
    const hit = this.hitTest(p.x, p.y);
    const { store } = ed;
    if (hit.type === 'segment') {
      const key = store.insertKey(hit.channelId, hit.t, { snap: ed.options.snapFrames && !(e.ctrlKey || e.metaKey) });
      if (key) {
        store.select([key.id], 'replace');
        ed.pulseKey(key.id);
      }
    } else if (hit.type === 'empty' && (e.ctrlKey || e.metaKey)) {
      const chId = store.activeChannelId;
      const ch = store.channel(chId);
      if (ch && ch.visible && !ch.locked) {
        const tf = channelTransform(ch, ed.mode, ed.normalize);
        const value = ed.mode === 'speed' ? undefined : hit.v / tf.scale + tf.offset;
        const key = store.insertKey(chId, hit.t, { snap: ed.options.snapFrames, value });
        if (key) {
          store.select([key.id], 'replace');
          ed.pulseKey(key.id);
        }
      }
    } else if (hit.type === 'key') {
      ed.focusInspector('time');
    } else if (hit.type === 'ruler') {
      ed.fitAll();
    }
    ed.requestRender();
  }

  // --------------------------------------------------------------- wheel
  onWheel(e) {
    e.preventDefault();
    const ed = this.ed;
    const p = this.localPoint(e);
    const { vp } = ed;
    const dy = e.deltaMode === 1 ? e.deltaY * 16 : e.deltaY;
    const dx = e.deltaMode === 1 ? e.deltaX * 16 : e.deltaX;
    const factor = Math.exp(-dy * 0.0022);
    if (e.ctrlKey || e.metaKey) {
      // Ctrl+wheel (and trackpad pinch): value zoom.
      vp.zoomValue(factor, p.y);
    } else if (e.altKey) {
      vp.zoomTime(factor, p.x);
      vp.zoomValue(factor, p.y);
    } else if (e.shiftKey) {
      vp.pan(-dy, 0);
    } else if (Math.abs(dx) > Math.abs(dy)) {
      vp.pan(-dx, 0);
    } else {
      vp.zoomTime(factor, p.x);
    }
    ed.onViewChanged();
    this.updateHover(p, e);
    ed.requestRender();
  }

  // --------------------------------------------------------- context menu
  onContextMenu(e) {
    e.preventDefault();
    const ed = this.ed;
    const p = this.localPoint(e);
    const hit = this.hitTest(p.x, p.y);
    if (hit.type === 'key' && !ed.store.selectedKeys.has(hit.keyId)) ed.store.select([hit.keyId], 'replace');
    if (hit.type === 'segment') ed.store.selectSegment(hit.channelId, hit.keyId);
    ed.openContextMenu(e.clientX, e.clientY, hit);
  }

  // ------------------------------------------------------------ keyboard
  onKeyUp(e) {
    if (e.code === 'Space') this.spaceHeld = false;
  }

  onKeyDown(e) {
    const ed = this.ed;
    if (isEditableTarget(e.target)) return; // never steal keys from inputs
    const { store } = ed;
    const fps = store.fps;
    const ctrl = e.ctrlKey || e.metaKey;
    const sel = [...store.selectedKeys];
    const frame = framesToSeconds(1, fps);
    const mult = e.shiftKey ? 10 : 1;
    const handled = () => {
      e.preventDefault();
      e.stopPropagation();
      ed.requestRender();
    };
    if (e.code === 'Space') {
      this.spaceHeld = true;
      if (this.drag || this.press) return;
      // Space alone is left to the host (play/pause); only used as a pan modifier here.
      return;
    }
    if (e.key === 'Escape') {
      ed.closeMenus();
      if (this.drag) this.cancelDrag();
      else store.clearSelection();
      return handled();
    }
    if (ctrl && (e.key === 'z' || e.key === 'Z')) {
      if (e.shiftKey) ed.redo();
      else ed.undo();
      return handled();
    }
    if (ctrl && (e.key === 'y' || e.key === 'Y')) {
      ed.redo();
      return handled();
    }
    if (ctrl && (e.key === 'a' || e.key === 'A')) {
      store.selectAll();
      return handled();
    }
    if (ctrl && (e.key === 'c' || e.key === 'C')) {
      ed.copySelection();
      return handled();
    }
    if (ctrl && (e.key === 'v' || e.key === 'V')) {
      ed.pasteAtPlayhead();
      return handled();
    }
    if (ctrl && (e.key === 'd' || e.key === 'D')) {
      if (sel.length) store.duplicateKeys(sel);
      return handled();
    }
    if (e.key === 'Delete' || e.key === 'Backspace') {
      if (sel.length) store.deleteKeys(sel);
      return handled();
    }
    if (e.key === 'f' || e.key === 'F') {
      if (e.shiftKey) ed.fitSelected();
      else ed.fitAll();
      return handled();
    }
    if (e.key === 'Home') {
      ed.fitRange();
      return handled();
    }
    if (e.key === 'ArrowLeft' || e.key === 'ArrowRight') {
      if (!sel.length) {
        ed.setPlayhead((ed.playhead || 0) + (e.key === 'ArrowLeft' ? -1 : 1) * frame * mult, 'user');
        return handled();
      }
      store.moveKeys(sel, (e.key === 'ArrowLeft' ? -1 : 1) * frame * mult, 0, { snap: false, anchorId: sel[0] });
      return handled();
    }
    if (e.key === 'ArrowUp' || e.key === 'ArrowDown') {
      if (!sel.length || ed.mode === 'speed') return;
      const step = ed.valueNudgeStep() * mult;
      store.moveKeys(sel, 0, (e.key === 'ArrowUp' ? 1 : -1) * step, { snap: false, valueOnly: true });
      return handled();
    }
    if (e.key === '[' || e.key === ']') {
      ed.jumpToKey(e.key === '[' ? -1 : 1);
      return handled();
    }
    if (e.key === '1' || e.key === '2' || e.key === '3') {
      if (!sel.length) return;
      store.setInterp(sel, e.key === '1' ? INTERP.BEZIER : e.key === '2' ? INTERP.LINEAR : INTERP.HOLD);
      return handled();
    }
    if (e.key === ',' || e.key === '.') {
      // Walk through keys of the active channel (Tab is left to the browser).
      const chId = store.activeChannelId;
      const ch = store.channel(chId);
      if (!ch || !ch.keys.length) return;
      const current = sel.length === 1 ? ch.keys.findIndex((k) => k.id === sel[0]) : -1;
      const next = (current + (e.key === ',' ? -1 : 1) + ch.keys.length) % ch.keys.length;
      store.select([ch.keys[next].id], 'replace');
      ed.vp.ensureTimeVisible(ch.keys[next].t);
      ed.onViewChanged();
      return handled();
    }
    if (e.key === '?') {
      ed.toggleHelp();
      return handled();
    }
  }
}

export { TANGENTS, timeEpsilon };
