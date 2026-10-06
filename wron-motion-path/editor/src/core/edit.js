// Path editing operations. Mirrors core/src/edit.cpp so the web editor and the
// OFX preview overlay edit paths by the same rules. Operations mutate `path`.
import { Cubic } from './geometry.js';
import { segmentCount } from './document.js';
import { add, cross, degToRad, len, normalizeOrZero, sub } from './vec.js';

export const HitKind = Object.freeze({ None: 'none', Anchor: 'anchor', InHandle: 'in', OutHandle: 'out', Segment: 'segment' });

const toD = (n, aspect) => ({ x: n.x * aspect, y: n.y });
const toN = (d, aspect) => ({ x: d.x / aspect, y: d.y });

export function segmentCubicN(path, i) {
  const n = path.points.length;
  const a = path.points[i], b = path.points[(i + 1) % n];
  return new Cubic(a.p, add(a.p, a.out), add(b.p, b.in), b.p);
}

function distToSegmentD(cN, aspect, posD) {
  const K = 64;
  const dist = (t) => len(sub(toD(cN.eval(t), aspect), posD));
  let bestT = 0, best = dist(0);
  for (let i = 1; i <= K; i += 1) {
    const t = i / K, d = dist(t);
    if (d < best) { best = d; bestT = t; }
  }
  let lo = Math.max(0, bestT - 1 / K), hi = Math.min(1, bestT + 1 / K);
  const g = 0.5 * (Math.sqrt(5) - 1);
  let x1 = hi - g * (hi - lo), x2 = lo + g * (hi - lo);
  let f1 = dist(x1), f2 = dist(x2);
  for (let k = 0; k < 40; k += 1) {
    if (f1 < f2) { hi = x2; x2 = x1; f2 = f1; x1 = hi - g * (hi - lo); f1 = dist(x1); }
    else { lo = x1; x1 = x2; f1 = f2; x2 = lo + g * (hi - lo); f2 = dist(x2); }
  }
  const tm = 0.5 * (lo + hi), dm = dist(tm);
  if (dm < best) { best = dm; bestT = tm; }
  return { d: best, t: bestT };
}

// selection: array of selected point indices (handles are tested for these).
export function hitTest(path, aspect, posN, tol, selection = []) {
  const posD = toD(posN, aspect);
  let best = tol;
  let hit = { kind: HitKind.None, index: -1, t: 0, distance: 0 };
  path.points.forEach((pt, i) => {
    const d = len(sub(toD(pt.p, aspect), posD));
    if (d <= best) { best = d; hit = { kind: HitKind.Anchor, index: i, t: 0, distance: d }; }
  });
  if (hit.kind !== HitKind.None) return hit;
  for (const i of selection) {
    const pt = path.points[i];
    if (!pt) continue;
    for (const [kind, h] of [[HitKind.InHandle, pt.in], [HitKind.OutHandle, pt.out]]) {
      if (h.x === 0 && h.y === 0) continue;
      const d = len(sub(toD(add(pt.p, h), aspect), posD));
      if (d <= best) { best = d; hit = { kind, index: i, t: 0, distance: d }; }
    }
  }
  if (hit.kind !== HitKind.None) return hit;
  const segs = segmentCount(path);
  for (let s = 0; s < segs; s += 1) {
    const r = distToSegmentD(segmentCubicN(path, s), aspect, posD);
    if (r.d <= best) { best = r.d; hit = { kind: HitKind.Segment, index: s, t: r.t, distance: r.d }; }
  }
  return hit;
}

export function nextPointId(path) {
  const used = new Set(path.points.map((p) => p.id));
  for (let k = path.points.length + 1; ; k += 1) {
    const id = `p${k}`;
    if (!used.has(id)) return id;
  }
}

export function insertPoint(path, segment, t) {
  const segs = segmentCount(path);
  if (segment < 0 || segment >= segs) return -1;
  t = Math.min(Math.max(t, 1e-6), 1 - 1e-6);
  const [left, right] = segmentCubicN(path, segment).split(t);
  const n = path.points.length;
  const a = path.points[segment], b = path.points[(segment + 1) % n];
  a.out = sub(left.p1, left.p0);
  b.in = sub(right.p2, right.p3);
  const m = {
    id: nextPointId(path),
    p: left.p3,
    in: sub(left.p2, left.p3),
    out: sub(right.p1, right.p0),
    mode: 'smooth',
    extra: {},
  };
  if (m.in.x === 0 && m.in.y === 0 && m.out.x === 0 && m.out.y === 0) m.mode = 'corner';
  path.points.splice(segment + 1, 0, m);
  return segment + 1;
}

export function deletePoint(path, index) {
  if (index < 0 || index >= path.points.length) return;
  path.points.splice(index, 1);
  if (path.points.length < 2) path.closed = false;
}

export function moveAnchor(path, index, posN) {
  const pt = path.points[index];
  if (pt) pt.p = { ...posN };
}

export function moveHandle(path, index, outHandle, handlePosN, aspect, breakTangent) {
  const pt = path.points[index];
  if (!pt) return;
  const key = outHandle ? 'out' : 'in';
  const other = outHandle ? 'in' : 'out';
  pt[key] = sub(handlePosN, pt.p);
  if (breakTangent || pt.mode === 'corner') {
    pt.mode = 'free';
    return;
  }
  if (pt.mode === 'smooth') {
    const hD = toD(pt[key], aspect);
    const oLen = len(toD(pt[other], aspect));
    const hLen = len(hD);
    if (hLen > 0 && oLen > 0) pt[other] = toN({ x: hD.x * (-oLen / hLen), y: hD.y * (-oLen / hLen) }, aspect);
  }
}

export function setMode(path, index, mode, aspect) {
  const n = path.points.length;
  const pt = path.points[index];
  if (!pt) return;
  pt.mode = mode;
  if (mode === 'corner') {
    pt.in = { x: 0, y: 0 };
    pt.out = { x: 0, y: 0 };
    return;
  }
  if (mode !== 'smooth') return;
  const inD = toD(pt.in, aspect), outD = toD(pt.out, aspect);
  let inLen = len(inD), outLen = len(outD);
  let dir = normalizeOrZero(sub(outD, inD));
  if (inLen === 0 && outLen === 0) {
    const hasPrev = path.closed || index > 0;
    const hasNext = path.closed || index < n - 1;
    const p = toD(pt.p, aspect);
    const prev = hasPrev ? toD(path.points[(index - 1 + n) % n].p, aspect) : p;
    const next = hasNext ? toD(path.points[(index + 1) % n].p, aspect) : p;
    dir = normalizeOrZero(sub(next, prev));
    inLen = len(sub(p, prev)) / 3;
    outLen = len(sub(next, p)) / 3;
  }
  if (dir.x === 0 && dir.y === 0) return;
  pt.in = toN({ x: -dir.x * inLen, y: -dir.y * inLen }, aspect);
  pt.out = toN({ x: dir.x * outLen, y: dir.y * outLen }, aspect);
}

// Uniform scale + rotation (+ translation) of points around pivotN, in
// display space. `indices` limits the edit to some points (default: all).
export function transformPoints(path, aspect, pivotN, scale, rotationDeg, translateN, indices = null) {
  const r = degToRad(rotationDeg);
  const c = Math.cos(r) * scale, s = Math.sin(r) * scale;
  const pv = toD(pivotN, aspect);
  const lin = (v) => ({ x: c * v.x - s * v.y, y: s * v.x + c * v.y });
  const list = indices || path.points.map((_, i) => i);
  for (const i of list) {
    const pt = path.points[i];
    const pD = toD(pt.p, aspect);
    const q = toN(add(pv, lin(sub(pD, pv))), aspect);
    pt.p = { x: q.x + translateN.x, y: q.y + translateN.y };
    pt.in = toN(lin(toD(pt.in, aspect)), aspect);
    pt.out = toN(lin(toD(pt.out, aspect)), aspect);
  }
}

export const transformPath = (path, aspect, pivotN, scale, rotationDeg, translateN) =>
  transformPoints(path, aspect, pivotN, scale, rotationDeg, translateN, null);

// True when the in/out handles of a smooth point are collinear (display space).
export function isSmoothConsistent(pt, aspect) {
  const a = toD(pt.in, aspect), b = toD(pt.out, aspect);
  return Math.abs(cross(a, b)) <= 1e-9 * Math.max(1, len(a) * len(b));
}
