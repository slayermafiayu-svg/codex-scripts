// Keyframe curve math. Pure functions, no DOM.
//
// A keyframe owns the segment that leaves it:
//   interp   'bezier' | 'linear' | 'hold'   -> how to travel to the next key
//   tangents 'auto' | 'unified' | 'broken'  -> how its two handles behave
//   in  {dt<=0, dv}  handle for the incoming segment (offset from the key)
//   out {dt>=0, dv}  handle for the outgoing segment (offset from the key)
// Handles are weighted (both dt and dv matter) and stored in real units:
// seconds and channel units. Nothing here is rounded to frames; frame snapping
// is an editing policy applied by the editor, never by the evaluator.

export const INTERP = Object.freeze({ BEZIER: 'bezier', LINEAR: 'linear', HOLD: 'hold' });
export const TANGENTS = Object.freeze({ AUTO: 'auto', UNIFIED: 'unified', BROKEN: 'broken' });
export const EXTRAPOLATION = Object.freeze({ HOLD: 'hold', LINEAR: 'linear', CYCLE: 'cycle', PINGPONG: 'pingpong' });

let idCounter = 0;
/** Stable, name-independent id. */
export function createId(prefix = 'k') {
  idCounter = (idCounter + 1) % 0xffffff;
  const rnd = Math.floor(Math.random() * 0xffffff).toString(36);
  return `${prefix}_${Date.now().toString(36)}${idCounter.toString(36)}${rnd}`;
}

/**
 * @typedef {{dt:number, dv:number}} Handle
 * @typedef {{id:string, t:number, v:number, interp:string, tangents:string, in:Handle, out:Handle}} Keyframe
 */

/**
 * @param {number} t
 * @param {number} v
 * @param {Partial<Keyframe>} [overrides]
 * @returns {Keyframe}
 */
export function createKey(t, v, overrides = {}) {
  return {
    id: overrides.id || createId(),
    t,
    v,
    interp: overrides.interp || INTERP.BEZIER,
    tangents: overrides.tangents || TANGENTS.AUTO,
    in: overrides.in ? { dt: Math.min(0, overrides.in.dt), dv: overrides.in.dv } : { dt: 0, dv: 0 },
    out: overrides.out ? { dt: Math.max(0, overrides.out.dt), dv: overrides.out.dv } : { dt: 0, dv: 0 },
  };
}

export function cloneKey(k) {
  return { ...k, in: { ...k.in }, out: { ...k.out } };
}

/** Sort keys by time in place and return them. */
export function sortKeys(keys) {
  keys.sort((a, b) => a.t - b.t);
  return keys;
}

function isFiniteNumber(x) {
  return typeof x === 'number' && Number.isFinite(x);
}

/**
 * Recompute handles for keys whose tangents are 'auto'. Uses Fritsch–Carlson
 * monotone slopes so auto curves never overshoot their neighbouring values.
 * Hold/linear segments act as smoothness breaks.
 * @param {Keyframe[]} keys sorted
 */
export function updateAutoTangents(keys) {
  const n = keys.length;
  if (n === 0) return keys;
  const slopes = new Array(n).fill(0);
  const h = new Array(Math.max(0, n - 1));
  const d = new Array(Math.max(0, n - 1));
  for (let i = 0; i < n - 1; i++) {
    h[i] = keys[i + 1].t - keys[i].t;
    d[i] = h[i] > 0 ? (keys[i + 1].v - keys[i].v) / h[i] : 0;
  }
  const smoothLeft = (i) => i > 0 && keys[i - 1].interp === INTERP.BEZIER && h[i - 1] > 0;
  const smoothRight = (i) => i < n - 1 && keys[i].interp === INTERP.BEZIER && h[i] > 0;
  for (let i = 0; i < n; i++) {
    const L = smoothLeft(i);
    const R = smoothRight(i);
    if (L && R) {
      if (d[i - 1] * d[i] <= 0) slopes[i] = 0;
      else {
        const w1 = 2 * h[i] + h[i - 1];
        const w2 = h[i] + 2 * h[i - 1];
        slopes[i] = (w1 + w2) / (w1 / d[i - 1] + w2 / d[i]);
      }
    } else if (R) slopes[i] = d[i];
    else if (L) slopes[i] = d[i - 1];
    else slopes[i] = 0;
  }
  for (let i = 0; i < n; i++) {
    const k = keys[i];
    if (k.tangents !== TANGENTS.AUTO) continue;
    const m = slopes[i];
    const outDt = i < n - 1 && h[i] > 0 ? h[i] / 3 : i > 0 && h[i - 1] > 0 ? h[i - 1] / 3 : 1 / 3;
    const inDt = i > 0 && h[i - 1] > 0 ? h[i - 1] / 3 : outDt;
    k.out = { dt: outDt, dv: m * outDt };
    k.in = { dt: -inDt, dv: -m * inDt };
  }
  return keys;
}

/**
 * Bezier control points for the segment k0 -> k1 with handle times clamped
 * into the segment so time never runs backwards.
 */
export function segmentControlPoints(k0, k1) {
  const t0 = k0.t;
  const t1 = k1.t;
  const dur = Math.max(0, t1 - t0);
  const outDt = Math.min(dur, Math.max(0, k0.out.dt));
  const inDt = Math.max(-dur, Math.min(0, k1.in.dt));
  return {
    x0: t0,
    y0: k0.v,
    x1: t0 + outDt,
    y1: k0.v + (k0.out.dt > 0 ? k0.out.dv * (outDt / k0.out.dt) : k0.out.dv),
    x2: t1 + inDt,
    y2: k1.v + (k1.in.dt < 0 ? k1.in.dv * (inDt / k1.in.dt) : k1.in.dv),
    x3: t1,
    y3: k1.v,
  };
}

function bez(u, a, b, c, d) {
  const mu = 1 - u;
  return mu * mu * mu * a + 3 * mu * mu * u * b + 3 * mu * u * u * c + u * u * u * d;
}
function bezDeriv(u, a, b, c, d) {
  const mu = 1 - u;
  return 3 * mu * mu * (b - a) + 6 * mu * u * (c - b) + 3 * u * u * (d - c);
}

/**
 * Solve the monotone cubic x(u) = x for u in [0,1]. Newton with bisection
 * safeguard; converges in a handful of iterations and is stable at the ends.
 */
export function solveBezierX(x, x0, x1, x2, x3) {
  if (x <= x0) return 0;
  if (x >= x3) return 1;
  const span = x3 - x0;
  if (span <= 0) return 1;
  let lo = 0;
  let hi = 1;
  let u = (x - x0) / span;
  for (let i = 0; i < 24; i++) {
    const fx = bez(u, x0, x1, x2, x3) - x;
    if (Math.abs(fx) < span * 1e-9) return u;
    if (fx > 0) hi = u;
    else lo = u;
    const dfx = bezDeriv(u, x0, x1, x2, x3);
    let next = dfx > 1e-12 ? u - fx / dfx : (lo + hi) / 2;
    if (!(next > lo && next < hi)) next = (lo + hi) / 2;
    u = next;
  }
  return u;
}

/** Value at time t within the segment k0 -> k1 (t clamped into the segment). */
export function evalSegment(k0, k1, t) {
  if (t <= k0.t) return k0.v;
  if (t >= k1.t) return k0.interp === INTERP.HOLD ? k0.v : k1.v;
  switch (k0.interp) {
    case INTERP.HOLD:
      return k0.v;
    case INTERP.LINEAR: {
      const u = (t - k0.t) / (k1.t - k0.t);
      return k0.v + (k1.v - k0.v) * u;
    }
    default: {
      const c = segmentControlPoints(k0, k1);
      const u = solveBezierX(t, c.x0, c.x1, c.x2, c.x3);
      return bez(u, c.y0, c.y1, c.y2, c.y3);
    }
  }
}

/** Instantaneous speed (value units per second) inside a segment. */
export function speedSegment(k0, k1, t) {
  if (k1.t <= k0.t) return 0;
  switch (k0.interp) {
    case INTERP.HOLD:
      return 0;
    case INTERP.LINEAR:
      return (k1.v - k0.v) / (k1.t - k0.t);
    default: {
      const c = segmentControlPoints(k0, k1);
      const u = solveBezierX(Math.min(k1.t, Math.max(k0.t, t)), c.x0, c.x1, c.x2, c.x3);
      const dx = bezDeriv(u, c.x0, c.x1, c.x2, c.x3);
      const dy = bezDeriv(u, c.y0, c.y1, c.y2, c.y3);
      if (Math.abs(dx) < 1e-12) {
        // Degenerate handle: take a tiny finite difference instead of blowing up.
        const h = (k1.t - k0.t) * 1e-4;
        return (evalSegment(k0, k1, t + h) - evalSegment(k0, k1, t - h)) / (2 * h);
      }
      return dy / dx;
    }
  }
}

/** Index of the last key with key.t <= t, or -1. */
export function findSegmentIndex(keys, t) {
  let lo = 0;
  let hi = keys.length - 1;
  let ans = -1;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    if (keys[mid].t <= t) {
      ans = mid;
      lo = mid + 1;
    } else hi = mid - 1;
  }
  return ans;
}

function mapExtrapolatedTime(keys, t, mode) {
  const first = keys[0];
  const last = keys[keys.length - 1];
  const span = last.t - first.t;
  if (span <= 0) return { t: first.t, offset: 0 };
  const rel = t - first.t;
  if (mode === EXTRAPOLATION.CYCLE) {
    const m = ((rel % span) + span) % span;
    return { t: first.t + m, offset: 0 };
  }
  if (mode === EXTRAPOLATION.PINGPONG) {
    const period = span * 2;
    let m = ((rel % period) + period) % period;
    if (m > span) m = period - m;
    return { t: first.t + m, offset: 0 };
  }
  return null;
}

/**
 * Evaluate a channel at time t.
 * @param {Keyframe[]} keys sorted
 * @param {number} t
 * @param {{pre?:string, post?:string}} [extrapolation]
 */
export function evalKeys(keys, t, extrapolation = {}) {
  const n = keys.length;
  if (n === 0) return 0;
  if (n === 1) return keys[0].v;
  const first = keys[0];
  const last = keys[n - 1];
  if (t < first.t) {
    const mode = extrapolation.pre || EXTRAPOLATION.HOLD;
    if (mode === EXTRAPOLATION.LINEAR) return first.v + speedKeys(keys, first.t + 1e-9) * (t - first.t);
    const mapped = mapExtrapolatedTime(keys, t, mode);
    return mapped ? evalKeys(keys, mapped.t) : first.v;
  }
  if (t > last.t) {
    const mode = extrapolation.post || EXTRAPOLATION.HOLD;
    if (mode === EXTRAPOLATION.LINEAR) return last.v + speedKeys(keys, last.t - 1e-9) * (t - last.t);
    const mapped = mapExtrapolatedTime(keys, t, mode);
    return mapped ? evalKeys(keys, mapped.t) : last.v;
  }
  const i = findSegmentIndex(keys, t);
  if (i >= n - 1) return last.v;
  return evalSegment(keys[i], keys[i + 1], t);
}

/** Speed (units/second) at time t; 0 outside the keyed range for hold extrapolation. */
export function speedKeys(keys, t) {
  const n = keys.length;
  if (n < 2) return 0;
  if (t < keys[0].t || t > keys[n - 1].t) return 0;
  let i = findSegmentIndex(keys, t);
  if (i >= n - 1) i = n - 2;
  return speedSegment(keys[i], keys[i + 1], t);
}

/**
 * Insert a key at time t inside the segment k0 -> k1 without changing the
 * curve (de Casteljau split for bezier). Returns the new key and the
 * updated handles for its neighbours. Does not mutate inputs.
 * @returns {{key:Keyframe, k0:Keyframe, k1:Keyframe}}
 */
export function splitSegment(k0, k1, t, id) {
  const a = cloneKey(k0);
  const b = cloneKey(k1);
  const dur = k1.t - k0.t;
  if (k0.interp === INTERP.HOLD) {
    return { key: createKey(t, k0.v, { id, interp: INTERP.HOLD, tangents: TANGENTS.AUTO }), k0: a, k1: b };
  }
  if (k0.interp === INTERP.LINEAR || dur <= 0) {
    const v = dur > 0 ? k0.v + ((k1.v - k0.v) * (t - k0.t)) / dur : k0.v;
    return { key: createKey(t, v, { id, interp: INTERP.LINEAR, tangents: TANGENTS.AUTO }), k0: a, k1: b };
  }
  const c = segmentControlPoints(k0, k1);
  const u = solveBezierX(t, c.x0, c.x1, c.x2, c.x3);
  const lerp = (p, q) => ({ x: p.x + (q.x - p.x) * u, y: p.y + (q.y - p.y) * u });
  const P0 = { x: c.x0, y: c.y0 };
  const P1 = { x: c.x1, y: c.y1 };
  const P2 = { x: c.x2, y: c.y2 };
  const P3 = { x: c.x3, y: c.y3 };
  const Q0 = lerp(P0, P1);
  const Q1 = lerp(P1, P2);
  const Q2 = lerp(P2, P3);
  const R0 = lerp(Q0, Q1);
  const R1 = lerp(Q1, Q2);
  const M = lerp(R0, R1);
  // Neighbours keep their shape, so they become explicit if they were auto.
  if (a.tangents === TANGENTS.AUTO) a.tangents = TANGENTS.UNIFIED;
  if (b.tangents === TANGENTS.AUTO) b.tangents = TANGENTS.UNIFIED;
  a.out = { dt: Q0.x - P0.x, dv: Q0.y - P0.y };
  b.in = { dt: Q2.x - P3.x, dv: Q2.y - P3.y };
  const key = createKey(M.x, M.y, {
    id,
    interp: INTERP.BEZIER,
    tangents: TANGENTS.UNIFIED,
    in: { dt: R0.x - M.x, dv: R0.y - M.y },
    out: { dt: R1.x - M.x, dv: R1.y - M.y },
  });
  return { key, k0: a, k1: b };
}

/**
 * Easing presets expressed as normalized cubic-bezier control points
 * (x1, y1, x2, y2) over a segment. x is time, y is value progress.
 */
export const EASING_PRESETS = Object.freeze([
  { id: 'linear', cp: [1 / 3, 1 / 3, 2 / 3, 2 / 3] },
  { id: 'smooth', cp: [0.33, 0, 0.67, 1] },
  { id: 'easeIn', cp: [0.42, 0, 1, 1] },
  { id: 'easeOut', cp: [0, 0, 0.58, 1] },
  { id: 'easeInOut', cp: [0.42, 0, 0.58, 1] },
  { id: 'fast', cp: [0.05, 0.6, 0.25, 1] },
  { id: 'slow', cp: [0.75, 0, 0.95, 0.4] },
  { id: 'overshoot', cp: [0.34, 1.4, 0.64, 1] },
  { id: 'anticipate', cp: [0.36, -0.4, 0.66, 1] },
]);

/**
 * Convert normalized control points to real handles for the segment k0 -> k1.
 * Only the two handles that face each other change; the far handles of each
 * key are untouched, so the keys become 'broken' when they had a partner side.
 */
export function applyEasingToSegment(k0, k1, cp) {
  const [x1, y1, x2, y2] = cp;
  const dt = k1.t - k0.t;
  const dv = k1.v - k0.v;
  k0.interp = INTERP.BEZIER;
  k0.out = { dt: Math.max(0, Math.min(1, x1)) * dt, dv: y1 * dv };
  k1.in = { dt: (Math.max(0, Math.min(1, x2)) - 1) * dt, dv: (y2 - 1) * dv };
  if (k0.tangents !== TANGENTS.BROKEN) k0.tangents = TANGENTS.BROKEN;
  if (k1.tangents !== TANGENTS.BROKEN) k1.tangents = TANGENTS.BROKEN;
}

/**
 * Normalized control points of the segment k0 -> k1, for the easing inset.
 * y is normalized by the value change; when the change is ~0 the y of handles
 * is reported in raw units with `flat: true` so the caller can label it.
 */
export function segmentToEasing(k0, k1) {
  const c = segmentControlPoints(k0, k1);
  const dt = c.x3 - c.x0;
  const dv = c.y3 - c.y0;
  if (dt <= 0) return null;
  const flat = Math.abs(dv) < 1e-12;
  const ny = (y) => (flat ? y - c.y0 : (y - c.y0) / dv);
  return {
    cp: [(c.x1 - c.x0) / dt, ny(c.y1), (c.x2 - c.x0) / dt, ny(c.y2)],
    flat,
    interp: k0.interp,
  };
}

/** Find the preset whose control points match (within tolerance), or null. */
export function matchEasingPreset(cp, tol = 0.02) {
  if (!cp) return null;
  for (const p of EASING_PRESETS) {
    let ok = true;
    for (let i = 0; i < 4; i++) if (Math.abs(p.cp[i] - cp[i]) > tol) ok = false;
    if (ok) return p.id;
  }
  return null;
}

/**
 * Make the opposite handle of a key collinear with the side that just changed,
 * keeping the opposite handle's own length (weighted "aligned" behaviour).
 * @param {Keyframe} key
 * @param {'in'|'out'} changed
 */
export function alignOppositeHandle(key, changed) {
  const src = key[changed];
  const dstName = changed === 'in' ? 'out' : 'in';
  const dst = key[dstName];
  const srcLen = Math.hypot(src.dt, src.dv);
  if (srcLen < 1e-12) return;
  const dstLen = Math.hypot(dst.dt, dst.dv) || srcLen;
  const dirT = -src.dt / srcLen;
  const dirV = -src.dv / srcLen;
  let ndt = dirT * dstLen;
  let ndv = dirV * dstLen;
  // Keep the sign convention (in <= 0, out >= 0). A vertical source handle
  // keeps the destination vertical too.
  if (dstName === 'in' && ndt > 0) ndt = 0;
  if (dstName === 'out' && ndt < 0) ndt = 0;
  key[dstName] = { dt: ndt, dv: ndv };
}

/**
 * Retime keys: canonical mapping t' = a + (t - A) * Dt/Ds. Points outside
 * [A, A+Ds] are mapped too; nothing is clamped. Handle dt scales with k,
 * handle dv is unchanged (value/second slopes divide by k). Returns new keys.
 * @param {Keyframe[]} keys
 * @param {number} A source start
 * @param {number} Ds source length (>0)
 * @param {number} a target start
 * @param {number} Dt target length (>0)
 */
export function retimeKeys(keys, A, Ds, a, Dt) {
  if (!(Ds > 0) || !(Dt > 0)) throw new RangeError('retimeKeys needs positive source and target lengths');
  const k = Dt / Ds;
  return keys.map((key) => {
    const c = cloneKey(key);
    c.t = a + (key.t - A) * k;
    c.in = { dt: key.in.dt * k, dv: key.in.dv };
    c.out = { dt: key.out.dt * k, dv: key.out.dv };
    return c;
  });
}

/** Shift keys by a constant offset (original speed policy): t' = a + (t - A). */
export function shiftKeys(keys, A, a) {
  return keys.map((key) => {
    const c = cloneKey(key);
    c.t = a + (key.t - A);
    return c;
  });
}

/** Bounding box of key positions (not handles). */
export function keysBounds(keys) {
  if (!keys.length) return null;
  let tMin = Infinity;
  let tMax = -Infinity;
  let vMin = Infinity;
  let vMax = -Infinity;
  for (const k of keys) {
    if (k.t < tMin) tMin = k.t;
    if (k.t > tMax) tMax = k.t;
    if (k.v < vMin) vMin = k.v;
    if (k.v > vMax) vMax = k.v;
  }
  return { tMin, tMax, vMin, vMax };
}

/** Bounding box of the curve including bezier extrema (sampled). */
export function curveBounds(keys, samples = 16) {
  const b = keysBounds(keys);
  if (!b) return null;
  for (let i = 0; i < keys.length - 1; i++) {
    const k0 = keys[i];
    const k1 = keys[i + 1];
    if (k0.interp !== INTERP.BEZIER) continue;
    for (let s = 1; s < samples; s++) {
      const t = k0.t + ((k1.t - k0.t) * s) / samples;
      const v = evalSegment(k0, k1, t);
      if (v < b.vMin) b.vMin = v;
      if (v > b.vMax) b.vMax = v;
    }
  }
  return b;
}

/** Guard: replace NaN/Infinity with a fallback so bad input never propagates. */
export function sanitizeNumber(x, fallback = 0) {
  return isFiniteNumber(x) ? x : fallback;
}
