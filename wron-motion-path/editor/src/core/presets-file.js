// User preset files and timing adaptation. Mirrors core/src/retime.cpp.
//
// A preset always contains the path geometry. Optionally:
//   level "look"   + static values of transform / orientation / motion-blur parameters
//   level "timing" + the above + keyframes of animated parameters and the
//                    timing settings, with the SOURCE event duration
//
// Applying timing to another event:
//   "fit"      t' = t * targetDuration / sourceDuration (keys outside the event
//              keep their relative position, never clamped into it; manual
//              slopes in value/second are divided by the same factor)
//   "preserve" t' = t (original duration and speed)
// Always computed from the stored source timing, so applying a preset again
// and again cannot drift.
import { clonePath } from './document.js';
import { LOOK_PARAMS, TIMING_PARAMS, paramById } from './params.js';

export const PRESET_FORMAT = 'wron.motionpath.preset';
export const PRESET_VERSION = 1;
export const LEVELS = ['geometry', 'look', 'timing'];

export function retimeKeys(keys, sourceDuration, targetDuration, mode) {
  if (mode === 'preserve') return keys.map((k) => ({ ...k }));
  if (!(sourceDuration > 0) || !(targetDuration > 0)) return null;
  const k = targetDuration / sourceDuration;
  return keys.map((key) => {
    const o = { ...key, t: key.t * k };
    if (Array.isArray(key.slopes)) o.slopes = key.slopes.map((s) => s / k);
    return o;
  });
}

export function buildPreset({ name, level, path, aspect, values = {}, keyframes = {}, eventDuration = null }) {
  if (!LEVELS.includes(level)) throw new Error(`unknown preset level ${level}`);
  const preset = {
    format: PRESET_FORMAT,
    version: PRESET_VERSION,
    name,
    level,
    aspect,
    path: {
      closed: !!path.closed,
      points: clonePath(path).points.map((p) => ({ id: p.id, p: [p.p.x, p.p.y], in: [p.in.x, p.in.y], out: [p.out.x, p.out.y], mode: p.mode })),
    },
  };
  if (level === 'look' || level === 'timing') {
    preset.look = {};
    for (const id of LOOK_PARAMS) if (id in values) preset.look[id] = structuredClone(values[id]);
  }
  if (level === 'timing') {
    if (!(eventDuration > 0)) throw new Error('timing presets need the source event duration');
    preset.timing = { sourceDuration: eventDuration, values: {}, keyframes: {} };
    for (const id of TIMING_PARAMS) {
      if (id in values) preset.timing.values[id] = structuredClone(values[id]);
    }
    for (const [id, keys] of Object.entries(keyframes)) {
      if (Array.isArray(keys) && keys.length) preset.timing.keyframes[id] = structuredClone(keys);
    }
  }
  return preset;
}

export function parsePreset(obj) {
  if (!obj || obj.format !== PRESET_FORMAT) throw new Error('not a Wron motion path preset');
  if (!Number.isInteger(obj.version) || obj.version > PRESET_VERSION) throw new Error('preset was saved by a newer version');
  if (!LEVELS.includes(obj.level)) throw new Error('invalid preset level');
  const pts = obj.path?.points;
  if (!Array.isArray(pts)) throw new Error('preset has no path');
  const vec = (v) => {
    if (!Array.isArray(v) || v.length !== 2 || !v.every(Number.isFinite)) throw new Error('invalid point');
    return { x: v[0], y: v[1] };
  };
  return {
    ...obj,
    path: {
      closed: !!obj.path.closed,
      points: pts.map((p, i) => ({ id: typeof p.id === 'string' ? p.id : `p${i + 1}`, p: vec(p.p), in: vec(p.in ?? [0, 0]), out: vec(p.out ?? [0, 0]), mode: ['corner', 'smooth', 'free'].includes(p.mode) ? p.mode : 'free', extra: {} })),
    },
  };
}

// Plans what applying a preset does, without touching the host: returns the
// new path, the static parameter writes, the keyframe writes and parameters
// that were skipped (animated in the target, so a static look value would
// destroy the user's animation).
export function planApply(preset, { targetDuration = null, timingMode = 'fit', animatedIds = [] } = {}) {
  const plan = { path: clonePath(preset.path), paramWrites: {}, keyframeWrites: {}, skipped: [], error: null };
  if (preset.look) {
    for (const [id, v] of Object.entries(preset.look)) {
      if (!paramById(id)) continue;
      if (animatedIds.includes(id)) plan.skipped.push(id);
      else plan.paramWrites[id] = v;
    }
  }
  if (preset.timing) {
    const keyed = preset.timing.keyframes || {};
    for (const [id, v] of Object.entries(preset.timing.values || {})) {
      if (id === 'wmpProgress' || !paramById(id) || id in keyed) continue;
      if (animatedIds.includes(id)) plan.skipped.push(id);
      else plan.paramWrites[id] = v;
    }
    const src = preset.timing.sourceDuration;
    for (const [id, keys] of Object.entries(preset.timing.keyframes || {})) {
      const out = retimeKeys(keys, src, targetDuration, timingMode);
      if (!out) {
        plan.error = 'event-duration-unknown';
        return plan;
      }
      plan.keyframeWrites[id] = out;
    }
  }
  return plan;
}
