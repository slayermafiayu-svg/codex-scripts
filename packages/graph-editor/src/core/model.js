// Editing model: document, selection, transactions and undo/redo.
// All persistent edits go through here so a pointer drag is one logical
// operation, values are re-applied from the transaction snapshot (no
// cumulative rounding drift), and the host can mirror every transaction
// into its own undo system.

import { Emitter } from './emitter.js';
import {
  INTERP,
  TANGENTS,
  EXTRAPOLATION,
  createId,
  createKey,
  cloneKey,
  sortKeys,
  updateAutoTangents,
  evalKeys,
  speedKeys,
  splitSegment,
  applyEasingToSegment,
  alignOppositeHandle,
  findSegmentIndex,
  sanitizeNumber,
} from './curve.js';
import { makeFps, fpsFromNumber, snapToFrame, timeEpsilon, nearestFrame, framesToSeconds } from './time.js';

export const COLLISION = Object.freeze({ BLOCK: 'block', REPLACE: 'replace' });

const DEFAULT_COLORS = ['#ff5f6d', '#5ddc7a', '#4fa3ff', '#ffb547', '#c38bff', '#3fd1c9', '#ff8ad1', '#d7dc5d'];

function cloneChannel(ch) {
  return { ...ch, extrapolation: { ...(ch.extrapolation || {}) }, keys: ch.keys.map(cloneKey) };
}

function normalizeFps(fps) {
  if (fps == null) return makeFps(30);
  if (typeof fps === 'number') return fpsFromNumber(fps);
  return makeFps(fps.num, fps.den);
}

/**
 * Normalize an incoming document without mutating the caller's object.
 * Unknown fields on channels/keys are preserved so host metadata survives.
 */
export function normalizeDocument(input) {
  const doc = {
    version: 1,
    fps: normalizeFps(input?.fps),
    range: {
      start: sanitizeNumber(input?.range?.start, 0),
      end: sanitizeNumber(input?.range?.end, 5),
    },
    channels: [],
    ...(input?.meta ? { meta: input.meta } : {}),
  };
  if (doc.range.end <= doc.range.start) doc.range.end = doc.range.start + 1;
  const seen = new Set();
  (input?.channels || []).forEach((ch, idx) => {
    const id = ch.id || createId('c');
    const keys = (ch.keys || [])
      .filter((k) => Number.isFinite(k.t) && Number.isFinite(k.v))
      .map((k) => {
        let kid = k.id || createId();
        while (seen.has(kid)) kid = createId();
        seen.add(kid);
        return createKey(k.t, k.v, {
          ...k,
          id: kid,
          interp: Object.values(INTERP).includes(k.interp) ? k.interp : INTERP.BEZIER,
          tangents: Object.values(TANGENTS).includes(k.tangents) ? k.tangents : k.in || k.out ? TANGENTS.UNIFIED : TANGENTS.AUTO,
          in: k.in && Number.isFinite(k.in.dt) && Number.isFinite(k.in.dv) ? k.in : undefined,
          out: k.out && Number.isFinite(k.out.dt) && Number.isFinite(k.out.dv) ? k.out : undefined,
        });
      });
    sortKeys(keys);
    updateAutoTangents(keys);
    doc.channels.push({
      ...ch,
      id,
      name: ch.name || `Channel ${idx + 1}`,
      group: ch.group || '',
      color: ch.color || DEFAULT_COLORS[idx % DEFAULT_COLORS.length],
      unit: ch.unit || '',
      visible: ch.visible !== false,
      locked: !!ch.locked,
      extrapolation: {
        pre: ch.extrapolation?.pre || EXTRAPOLATION.HOLD,
        post: ch.extrapolation?.post || EXTRAPOLATION.HOLD,
      },
      keys,
    });
  });
  return doc;
}

export class GraphStore {
  /**
   * @param {object} doc
   * @param {{undo?:'internal'|'external', maxUndo?:number, collision?:'block'|'replace'}} [options]
   */
  constructor(doc, options = {}) {
    this.options = { undo: 'internal', maxUndo: 200, collision: COLLISION.BLOCK, ...options };
    this.events = new Emitter();
    this.selectedKeys = new Set();
    this.selectedSegment = null; // {channelId, keyId}
    this.activeChannelId = null;
    this._undo = [];
    this._redo = [];
    this._tx = null;
    this.setDoc(doc || {});
  }

  on(type, fn) {
    return this.events.on(type, fn);
  }

  // ---------------------------------------------------------------- document
  get doc() {
    return this._doc;
  }

  setDoc(doc, { keepHistory = false } = {}) {
    this._doc = normalizeDocument(doc);
    if (!keepHistory) {
      this._undo.length = 0;
      this._redo.length = 0;
    }
    this.selectedKeys.clear();
    this.selectedSegment = null;
    this.activeChannelId = this._doc.channels.find((c) => c.visible)?.id || null;
    this.events.emit('change', { label: 'load', kind: 'load' });
    this.events.emit('selection', this.selectionInfo());
  }

  /** Deep copy of the document for saving. Only persistent data, no UI state. */
  exportDoc() {
    const d = this._doc;
    return {
      version: d.version,
      fps: { ...d.fps },
      range: { ...d.range },
      ...(d.meta ? { meta: d.meta } : {}),
      channels: d.channels.map((ch) => ({
        ...ch,
        extrapolation: { ...ch.extrapolation },
        keys: ch.keys.map((k) => ({
          id: k.id,
          t: k.t,
          v: k.v,
          interp: k.interp,
          tangents: k.tangents,
          in: { ...k.in },
          out: { ...k.out },
        })),
      })),
    };
  }

  get fps() {
    return this._doc.fps;
  }

  get range() {
    return this._doc.range;
  }

  setFps(fps) {
    this._mutate('fps', () => {
      this._doc.fps = normalizeFps(fps);
    });
  }

  setRange(range) {
    this._mutate('range', () => {
      const start = sanitizeNumber(range.start, this._doc.range.start);
      const end = sanitizeNumber(range.end, this._doc.range.end);
      this._doc.range = { start, end: end > start ? end : start + framesToSeconds(1, this._doc.fps) };
    });
  }

  channel(id) {
    return this._doc.channels.find((c) => c.id === id) || null;
  }

  get channels() {
    return this._doc.channels;
  }

  /** Channels that can currently be edited. */
  get editableChannels() {
    return this._doc.channels.filter((c) => c.visible && !c.locked);
  }

  keyRef(id) {
    for (const ch of this._doc.channels) {
      const idx = ch.keys.findIndex((k) => k.id === id);
      if (idx >= 0) return { channel: ch, key: ch.keys[idx], index: idx };
    }
    return null;
  }

  setChannelProps(id, patch) {
    const ch = this.channel(id);
    if (!ch) return;
    this._mutate('channel', () => {
      Object.assign(ch, patch);
      if (patch.extrapolation) ch.extrapolation = { ...ch.extrapolation, ...patch.extrapolation };
      if (ch.locked || !ch.visible) {
        for (const k of ch.keys) this.selectedKeys.delete(k.id);
        if (this.selectedSegment?.channelId === id) this.selectedSegment = null;
      }
    });
    this.events.emit('selection', this.selectionInfo());
  }

  evalChannel(channelId, t) {
    const ch = this.channel(channelId);
    return ch ? evalKeys(ch.keys, t, ch.extrapolation) : 0;
  }

  speedChannel(channelId, t) {
    const ch = this.channel(channelId);
    return ch ? speedKeys(ch.keys, t) : 0;
  }

  // --------------------------------------------------------------- selection
  selectionInfo() {
    const keys = [];
    for (const id of this.selectedKeys) {
      const ref = this.keyRef(id);
      if (ref) keys.push(ref);
    }
    return { keys, segment: this.selectedSegment, activeChannelId: this.activeChannelId };
  }

  /**
   * @param {string[]} ids
   * @param {'replace'|'add'|'toggle'|'remove'} mode
   */
  select(ids, mode = 'replace') {
    const before = new Set(this.selectedKeys);
    if (mode === 'replace') this.selectedKeys.clear();
    for (const id of ids) {
      const ref = this.keyRef(id);
      if (!ref || ref.channel.locked || !ref.channel.visible) continue;
      if (mode === 'toggle') {
        if (this.selectedKeys.has(id)) this.selectedKeys.delete(id);
        else this.selectedKeys.add(id);
      } else if (mode === 'remove') this.selectedKeys.delete(id);
      else this.selectedKeys.add(id);
      if (mode !== 'remove') this.activeChannelId = ref.channel.id;
    }
    if (ids.length && mode !== 'remove') this.selectedSegment = null;
    if (!setsEqual(before, this.selectedKeys)) this.events.emit('selection', this.selectionInfo());
  }

  selectSegment(channelId, keyId) {
    const ch = this.channel(channelId);
    if (!ch || ch.locked) return;
    this.selectedKeys.clear();
    this.selectedSegment = keyId ? { channelId, keyId } : null;
    this.activeChannelId = channelId;
    this.events.emit('selection', this.selectionInfo());
  }

  clearSelection() {
    if (!this.selectedKeys.size && !this.selectedSegment) return;
    this.selectedKeys.clear();
    this.selectedSegment = null;
    this.events.emit('selection', this.selectionInfo());
  }

  selectAll(channelIds) {
    const ids = [];
    for (const ch of this.editableChannels) {
      if (channelIds && !channelIds.includes(ch.id)) continue;
      for (const k of ch.keys) ids.push(k.id);
    }
    this.select(ids, 'replace');
  }

  /** Keys that are selected, grouped per channel, in time order. */
  selectedRefs() {
    const refs = [];
    for (const ch of this._doc.channels) for (const k of ch.keys) if (this.selectedKeys.has(k.id)) refs.push({ channel: ch, key: k });
    return refs;
  }

  /**
   * Segments the current selection targets:
   * - an explicitly selected segment, or
   * - every segment whose both ends are selected, or
   * - for a single selected key: its incoming and outgoing segments.
   * Returns [{channel, k0, k1}].
   */
  targetSegments() {
    const out = [];
    if (this.selectedSegment) {
      const ref = this.keyRef(this.selectedSegment.keyId);
      if (ref && ref.index < ref.channel.keys.length - 1)
        out.push({ channel: ref.channel, k0: ref.key, k1: ref.channel.keys[ref.index + 1] });
      return out;
    }
    const refs = this.selectedRefs();
    if (refs.length === 1) {
      const { channel, key } = refs[0];
      const i = channel.keys.indexOf(key);
      if (i > 0) out.push({ channel, k0: channel.keys[i - 1], k1: key });
      if (i < channel.keys.length - 1) out.push({ channel, k0: key, k1: channel.keys[i + 1] });
      return out;
    }
    for (const ch of this._doc.channels) {
      for (let i = 0; i < ch.keys.length - 1; i++) {
        if (this.selectedKeys.has(ch.keys[i].id) && this.selectedKeys.has(ch.keys[i + 1].id))
          out.push({ channel: ch, k0: ch.keys[i], k1: ch.keys[i + 1] });
      }
    }
    return out;
  }

  // ------------------------------------------------------------ transactions
  get isTransacting() {
    return !!this._tx;
  }

  /** Begin a logical edit. Nested begins are merged into the outer one. */
  begin(label = 'edit') {
    if (this._tx) {
      this._tx.depth++;
      return;
    }
    this._tx = {
      label,
      depth: 1,
      dirty: false,
      before: this._snapshot(),
      selectionBefore: [...this.selectedKeys],
    };
  }

  /** Snapshot taken when the open transaction began (for ghost drawing). */
  transactionStart() {
    return this._tx ? this._tx.before : null;
  }

  /** Re-apply the pre-transaction state so a drag can set absolute deltas. */
  restoreTransactionStart() {
    if (!this._tx) return;
    this._restore(this._tx.before);
    this._tx.dirty = false;
  }

  commit() {
    const tx = this._tx;
    if (!tx) return;
    if (--tx.depth > 0) return;
    this._tx = null;
    if (!tx.dirty) return;
    const entry = {
      label: tx.label,
      before: tx.before,
      after: this._snapshot(),
      selectionBefore: tx.selectionBefore,
      selectionAfter: [...this.selectedKeys],
    };
    if (this.options.undo === 'internal') {
      this._undo.push(entry);
      if (this._undo.length > this.options.maxUndo) this._undo.shift();
      this._redo.length = 0;
    }
    this.events.emit('transaction', { label: tx.label, before: entry.before, after: entry.after });
    this.events.emit('change', { label: tx.label, kind: 'edit' });
  }

  cancel() {
    const tx = this._tx;
    if (!tx) return;
    this._tx = null;
    this._restore(tx.before);
    this.selectedKeys = new Set(tx.selectionBefore);
    this.events.emit('change', { label: tx.label, kind: 'cancel' });
    this.events.emit('selection', this.selectionInfo());
  }

  get canUndo() {
    return this._undo.length > 0;
  }

  get canRedo() {
    return this._redo.length > 0;
  }

  undo() {
    if (this._tx) this.cancel();
    const e = this._undo.pop();
    if (!e) return false;
    this._restore(e.before);
    this._redo.push(e);
    this.selectedKeys = new Set(e.selectionBefore.filter((id) => this.keyRef(id)));
    this.selectedSegment = null;
    this.events.emit('change', { label: e.label, kind: 'undo' });
    this.events.emit('selection', this.selectionInfo());
    return true;
  }

  redo() {
    const e = this._redo.pop();
    if (!e) return false;
    this._restore(e.after);
    this._undo.push(e);
    this.selectedKeys = new Set(e.selectionAfter.filter((id) => this.keyRef(id)));
    this.selectedSegment = null;
    this.events.emit('change', { label: e.label, kind: 'redo' });
    this.events.emit('selection', this.selectionInfo());
    return true;
  }

  /** Apply a snapshot received from the host's undo system (external mode). */
  applySnapshot(snapshot) {
    this._restore(snapshot);
    this.selectedKeys = new Set([...this.selectedKeys].filter((id) => this.keyRef(id)));
    this.selectedSegment = null;
    this.events.emit('change', { label: 'external', kind: 'external' });
    this.events.emit('selection', this.selectionInfo());
  }

  _snapshot() {
    return {
      fps: { ...this._doc.fps },
      range: { ...this._doc.range },
      channels: this._doc.channels.map(cloneChannel),
    };
  }

  _restore(snap) {
    this._doc.fps = { ...snap.fps };
    this._doc.range = { ...snap.range };
    this._doc.channels = snap.channels.map(cloneChannel);
  }

  /** Run fn inside a transaction (auto-created when none is open). */
  _mutate(label, fn) {
    const own = !this._tx;
    if (own) this.begin(label);
    try {
      const r = fn();
      this._tx.dirty = true;
      return r;
    } finally {
      if (own) this.commit();
    }
  }

  _finalizeChannel(ch) {
    sortKeys(ch.keys);
    updateAutoTangents(ch.keys);
  }

  // ------------------------------------------------------------------- edits
  /**
   * Move keys by a time/value delta. Deltas are absolute relative to the
   * transaction start, so call restoreTransactionStart() before each live
   * update. The anchor key (grabbed key) is snapped to a frame and every key
   * moves by the same delta so relative spacing is preserved.
   * @returns {{dt:number, dv:number, blocked:boolean}} the applied deltas
   */
  moveKeys(ids, dt, dv, opts = {}) {
    const { snap = true, anchorId = null, timeOnly = false, valueOnly = false, collision = this.options.collision, dvScale = null } = opts;
    let appliedDt = valueOnly ? 0 : sanitizeNumber(dt, 0);
    let appliedDv = timeOnly ? 0 : sanitizeNumber(dv, 0);
    let blocked = false;
    return this._mutate('move', () => {
      const refs = ids.map((id) => this.keyRef(id)).filter((r) => r && !r.channel.locked);
      if (!refs.length) return { dt: 0, dv: 0, blocked: false };
      if (snap && appliedDt !== 0) {
        const anchor = refs.find((r) => r.key.id === anchorId) || refs[0];
        appliedDt = snapToFrame(anchor.key.t + appliedDt, this._doc.fps) - anchor.key.t;
      }
      const eps = timeEpsilon(this._doc.fps);
      const selected = new Set(refs.map((r) => r.key.id));
      const collides = (delta) =>
        refs.some((r) => r.channel.keys.some((k) => !selected.has(k.id) && Math.abs(k.t - (r.key.t + delta)) < eps));
      if (appliedDt !== 0 && collides(appliedDt)) {
        if (collision === COLLISION.REPLACE) {
          for (const r of refs) {
            const t = r.key.t + appliedDt;
            r.channel.keys = r.channel.keys.filter((k) => selected.has(k.id) || Math.abs(k.t - t) >= eps);
          }
        } else {
          // Block: back off one frame at a time toward zero until free.
          const frame = framesToSeconds(1, this._doc.fps);
          const sign = Math.sign(appliedDt);
          let steps = Math.min(100000, Math.floor(Math.abs(appliedDt) / frame + 0.5));
          let candidate = appliedDt;
          while (steps > 0 && collides(candidate)) {
            steps--;
            candidate = sign * steps * frame;
            if (snap) {
              const anchor = refs.find((r) => r.key.id === anchorId) || refs[0];
              candidate = snapToFrame(anchor.key.t + candidate, this._doc.fps) - anchor.key.t;
            }
          }
          if (collides(candidate)) candidate = 0;
          blocked = candidate !== appliedDt;
          appliedDt = candidate;
        }
      }
      const touched = new Set();
      for (const r of refs) {
        const scale = dvScale && dvScale.has(r.channel.id) ? dvScale.get(r.channel.id) : 1;
        r.key.t = sanitizeNumber(r.key.t + appliedDt, r.key.t);
        r.key.v = sanitizeNumber(r.key.v + appliedDv * scale, r.key.v);
        touched.add(r.channel);
      }
      for (const ch of touched) this._finalizeChannel(ch);
      return { dt: appliedDt, dv: appliedDv, blocked };
    });
  }

  setKeyTime(id, t, { snap = true, collision = this.options.collision } = {}) {
    const ref = this.keyRef(id);
    if (!ref || ref.channel.locked) return false;
    const target = snap ? snapToFrame(t, this._doc.fps) : t;
    return this.moveKeys([id], target - ref.key.t, 0, { snap: false, anchorId: id, collision }).dt !== 0 || target === ref.key.t;
  }

  setKeyValue(id, v) {
    const ref = this.keyRef(id);
    if (!ref || ref.channel.locked || !Number.isFinite(v)) return false;
    this._mutate('value', () => {
      ref.key.v = v;
      this._finalizeChannel(ref.channel);
    });
    return true;
  }

  /** Set several key values relative (dv) or absolute. */
  nudgeKeys(ids, dt, dv, opts) {
    return this.moveKeys(ids, dt, dv, { ...opts, snap: true });
  }

  /**
   * Set a handle of a key in real units. Dragging a handle on an auto key
   * converts it to unified; alt/break makes it broken.
   * @param {'in'|'out'} side
   */
  setHandle(id, side, handle, { breakTangents = false } = {}) {
    const ref = this.keyRef(id);
    if (!ref || ref.channel.locked) return;
    this._mutate('tangent', () => {
      const k = ref.key;
      let dt = sanitizeNumber(handle.dt, 0);
      const dv = sanitizeNumber(handle.dv, 0);
      if (side === 'in') dt = Math.min(0, dt);
      else dt = Math.max(0, dt);
      // Keep the handle inside its segment so time stays monotone.
      const i = ref.index;
      if (side === 'out' && i < ref.channel.keys.length - 1) dt = Math.min(dt, ref.channel.keys[i + 1].t - k.t);
      if (side === 'in' && i > 0) dt = Math.max(dt, ref.channel.keys[i - 1].t - k.t);
      k[side] = { dt, dv };
      if (breakTangents) k.tangents = TANGENTS.BROKEN;
      else if (k.tangents === TANGENTS.AUTO) k.tangents = TANGENTS.UNIFIED;
      if (k.tangents === TANGENTS.UNIFIED) alignOppositeHandle(k, side);
      if (k.interp !== INTERP.BEZIER && side === 'out') k.interp = INTERP.BEZIER;
      if (side === 'in' && i > 0 && ref.channel.keys[i - 1].interp !== INTERP.BEZIER) ref.channel.keys[i - 1].interp = INTERP.BEZIER;
      this._finalizeChannel(ref.channel);
    });
  }

  setInterp(ids, interp) {
    if (!Object.values(INTERP).includes(interp)) return;
    this._mutate('interpolation', () => {
      const touched = new Set();
      for (const id of ids) {
        const ref = this.keyRef(id);
        if (!ref || ref.channel.locked) continue;
        ref.key.interp = interp;
        touched.add(ref.channel);
      }
      for (const ch of touched) this._finalizeChannel(ch);
    });
  }

  setTangents(ids, mode) {
    if (!Object.values(TANGENTS).includes(mode)) return;
    this._mutate('tangents', () => {
      const touched = new Set();
      for (const id of ids) {
        const ref = this.keyRef(id);
        if (!ref || ref.channel.locked) continue;
        ref.key.tangents = mode;
        if (mode === TANGENTS.UNIFIED) alignOppositeHandle(ref.key, 'out');
        touched.add(ref.channel);
      }
      for (const ch of touched) this._finalizeChannel(ch);
    });
  }

  /** Apply a normalized easing to segments (defaults to targetSegments()). */
  applyEasing(cp, segments = this.targetSegments()) {
    if (!segments.length) return 0;
    this._mutate('easing', () => {
      const touched = new Set();
      for (const s of segments) {
        if (s.channel.locked) continue;
        applyEasingToSegment(s.k0, s.k1, cp);
        touched.add(s.channel);
      }
      for (const ch of touched) this._finalizeChannel(ch);
    });
    return segments.length;
  }

  /**
   * Insert a key on a channel at time t. Inside a segment the curve shape is
   * preserved; outside the keyed range the current (extrapolated) value is used.
   */
  insertKey(channelId, t, { snap = true, value } = {}) {
    const ch = this.channel(channelId);
    if (!ch || ch.locked) return null;
    const time = snap ? snapToFrame(t, this._doc.fps) : t;
    const eps = timeEpsilon(this._doc.fps);
    const existing = ch.keys.find((k) => Math.abs(k.t - time) < eps);
    if (existing) {
      if (value != null && value !== existing.v) this.setKeyValue(existing.id, value);
      return existing;
    }
    return this._mutate('insert key', () => {
      const i = findSegmentIndex(ch.keys, time);
      let key;
      if (i >= 0 && i < ch.keys.length - 1) {
        const res = splitSegment(ch.keys[i], ch.keys[i + 1], time);
        ch.keys[i] = res.k0;
        ch.keys[i + 1] = res.k1;
        key = res.key;
        if (value != null) key.v = value;
      } else {
        const v = value != null ? value : evalKeys(ch.keys, time, ch.extrapolation);
        const neighbour = i >= 0 ? ch.keys[i] : ch.keys[0];
        key = createKey(time, v, { interp: neighbour ? neighbour.interp : INTERP.BEZIER, tangents: TANGENTS.AUTO });
      }
      ch.keys.push(key);
      this._finalizeChannel(ch);
      return key;
    });
  }

  deleteKeys(ids) {
    const set = new Set(ids);
    this._mutate('delete keys', () => {
      for (const ch of this._doc.channels) {
        if (ch.locked) continue;
        const before = ch.keys.length;
        ch.keys = ch.keys.filter((k) => !set.has(k.id));
        if (ch.keys.length !== before) this._finalizeChannel(ch);
      }
      for (const id of set) this.selectedKeys.delete(id);
      if (this.selectedSegment && set.has(this.selectedSegment.keyId)) this.selectedSegment = null;
    });
    this.events.emit('selection', this.selectionInfo());
  }

  /** Copy selected keys to a clipboard object (relative times). */
  copyKeys(ids = [...this.selectedKeys]) {
    const refs = ids.map((id) => this.keyRef(id)).filter(Boolean);
    if (!refs.length) return null;
    const t0 = Math.min(...refs.map((r) => r.key.t));
    return {
      fps: { ...this._doc.fps },
      items: refs.map((r) => ({ channelId: r.channel.id, channelName: r.channel.name, key: { ...cloneKey(r.key), t: r.key.t - t0 } })),
    };
  }

  /**
   * Paste keys at time `at`. Keys go back to their source channel when it
   * exists, else to the active channel. Existing keys at the same frame are
   * replaced (an explicit paste is a replace intent).
   */
  pasteKeys(clip, at, { snap = true } = {}) {
    if (!clip || !clip.items?.length) return [];
    const base = snap ? snapToFrame(at, this._doc.fps) : at;
    const eps = timeEpsilon(this._doc.fps);
    const newIds = [];
    this._mutate('paste keys', () => {
      const touched = new Set();
      for (const item of clip.items) {
        const ch = this.channel(item.channelId) || this.channel(this.activeChannelId);
        if (!ch || ch.locked) continue;
        const t = base + item.key.t;
        ch.keys = ch.keys.filter((k) => Math.abs(k.t - t) >= eps);
        const k = createKey(t, item.key.v, { ...item.key, id: createId() });
        ch.keys.push(k);
        newIds.push(k.id);
        touched.add(ch);
      }
      for (const ch of touched) this._finalizeChannel(ch);
    });
    this.select(newIds, 'replace');
    return newIds;
  }

  /**
   * Scale keys around pivots (transform box). Factors are absolute relative
   * to the transaction start. Times are snapped to frames afterwards only
   * when they were on frames before (keeps explicit subframe data intact).
   */
  scaleKeys(ids, { pivotT = 0, kT = 1, pivotV = 0, kV = 1, snap = true } = {}) {
    this._mutate('scale keys', () => {
      const touched = new Set();
      const fps = this._doc.fps;
      for (const id of ids) {
        const ref = this.keyRef(id);
        if (!ref || ref.channel.locked) continue;
        const k = ref.key;
        const wasOnFrame = Math.abs(nearestFrame(k.t, fps) - (k.t * fps.num) / fps.den) < 1e-4;
        let t = pivotT + (k.t - pivotT) * kT;
        if (snap && wasOnFrame) t = snapToFrame(t, fps);
        k.t = sanitizeNumber(t, k.t);
        k.v = sanitizeNumber(pivotV + (k.v - pivotV) * kV, k.v);
        if (k.tangents !== TANGENTS.AUTO) {
          k.in = { dt: k.in.dt * Math.abs(kT), dv: k.in.dv * kV };
          k.out = { dt: k.out.dt * Math.abs(kT), dv: k.out.dv * kV };
        }
        touched.add(ref.channel);
      }
      for (const ch of touched) {
        // Scaling can fold keys onto the same frame; keep the first, drop later duplicates.
        const eps = timeEpsilon(fps);
        sortKeys(ch.keys);
        ch.keys = ch.keys.filter((k, i, arr) => i === 0 || Math.abs(k.t - arr[i - 1].t) >= eps || !ids.includes(k.id));
        this._finalizeChannel(ch);
      }
    });
  }

  /** Duplicate keys shifted by dt (defaults to one frame to the right). */
  duplicateKeys(ids, dt) {
    const clip = this.copyKeys(ids);
    if (!clip) return [];
    const refs = ids.map((id) => this.keyRef(id)).filter(Boolean);
    const t0 = Math.min(...refs.map((r) => r.key.t));
    const shift = dt != null ? dt : framesToSeconds(1, this._doc.fps);
    return this.pasteKeys(clip, t0 + shift);
  }
}

function setsEqual(a, b) {
  if (a.size !== b.size) return false;
  for (const x of a) if (!b.has(x)) return false;
  return true;
}
