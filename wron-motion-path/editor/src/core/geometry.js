// Cubic Bézier, arc length and path geometry. Mirrors core/include/wmp/bezier.h,
// core/src/arclength.cpp and core/src/geometry.cpp step for step (same
// quadrature, same subdivision, same Newton iteration) so the editor preview
// and the renderer agree; test/golden.test.js checks this against vectors
// written by the C++ core.
import { add, len, lerp, mul, normalizeOrZero, sub } from './vec.js';

export const SpeedMode = Object.freeze({ ConstantSpeed: 0, EqualTimePerSegment: 1 });

export class Cubic {
  constructor(p0, p1, p2, p3) {
    this.p0 = p0;
    this.p1 = p1;
    this.p2 = p2;
    this.p3 = p3;
  }
  eval(t) {
    const u = 1 - t;
    const b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
    const { p0, p1, p2, p3 } = this;
    return { x: b0 * p0.x + b1 * p1.x + b2 * p2.x + b3 * p3.x, y: b0 * p0.y + b1 * p1.y + b2 * p2.y + b3 * p3.y };
  }
  d1(t) {
    const u = 1 - t;
    const a = sub(this.p1, this.p0), b = sub(this.p2, this.p1), c = sub(this.p3, this.p2);
    return mul(add(add(mul(a, u * u), mul(b, 2 * u * t)), mul(c, t * t)), 3);
  }
  d2(t) {
    const a = add(sub(this.p2, mul(this.p1, 2)), this.p0);
    const b = add(sub(this.p3, mul(this.p2, 2)), this.p1);
    return mul(add(mul(a, 1 - t), mul(b, t)), 6);
  }
  d3() {
    return mul(sub(add(sub(this.p3, mul(this.p2, 3)), mul(this.p1, 3)), this.p0), 6);
  }
  split(t) {
    const { p0, p1, p2, p3 } = this;
    const p01 = lerp(p0, p1, t), p12 = lerp(p1, p2, t), p23 = lerp(p2, p3, t);
    const p012 = lerp(p01, p12, t), p123 = lerp(p12, p23, t);
    const m = lerp(p012, p123, t);
    return [new Cubic(p0, p01, p012, m), new Cubic(m, p123, p23, p3)];
  }
  extent() {
    const xs = [this.p0.x, this.p1.x, this.p2.x, this.p3.x];
    const ys = [this.p0.y, this.p1.y, this.p2.y, this.p3.y];
    return Math.max(Math.max(...xs) - Math.min(...xs), Math.max(...ys) - Math.min(...ys));
  }
  isPoint() {
    const e = (a, b) => a.x === b.x && a.y === b.y;
    return e(this.p0, this.p1) && e(this.p1, this.p2) && e(this.p2, this.p3);
  }
}

// Unit forward tangent at t, robust to vanishing derivatives (see bezier.h).
export function robustTangent(c, t, side) {
  const scale = Math.max(c.extent(), 1e-300);
  const eps = scale * 1e-9;
  const v1 = c.d1(t);
  if (len(v1) > eps) return { ok: true, dir: normalizeOrZero(v1) };
  let s = side >= 0 ? 1 : -1;
  if (t <= 1e-9) s = 1;
  if (t >= 1 - 1e-9) s = -1;
  const v2 = c.d2(t);
  if (len(v2) > eps) return { ok: true, dir: normalizeOrZero(mul(v2, s)) };
  const v3 = c.d3();
  if (len(v3) > eps) return { ok: true, dir: normalizeOrZero(v3) };
  const chord = sub(c.p3, c.p0);
  if (len(chord) > eps) return { ok: true, dir: normalizeOrZero(chord) };
  return { ok: false, dir: { x: 0, y: 0 } };
}

const GX = [-0.9061798459386639928, -0.53846931010568309104, 0, 0.53846931010568309104, 0.9061798459386639928];
const GW = [0.23692688505618908751, 0.47862867049936646804, 0.56888888888888888889, 0.47862867049936646804,
  0.23692688505618908751];
const INITIAL_INTERVALS = 8;
const MAX_DEPTH = 24;

export class ArcLengthTable {
  constructor(c, absTolerance) {
    this.c = c;
    this.tol = Math.max(absTolerance, 1e-15);
    this.t = [0];
    this.s = [0];
    if (c.isPoint()) {
      this.t.push(1);
      this.s.push(0);
      this.total = 0;
      return;
    }
    for (let i = 0; i < INITIAL_INTERVALS; i += 1) {
      const a = i / INITIAL_INTERVALS, b = (i + 1) / INITIAL_INTERVALS;
      this.refine(a, b, this.gauss(a, b), this.tol * (b - a), 0);
    }
    this.t[this.t.length - 1] = 1;
    this.total = this.s[this.s.length - 1];
  }
  speed(t) {
    return len(this.c.d1(t));
  }
  gauss(a, b) {
    const half = 0.5 * (b - a), mid = 0.5 * (a + b);
    let sum = 0;
    for (let i = 0; i < 5; i += 1) sum += GW[i] * this.speed(mid + half * GX[i]);
    return sum * half;
  }
  refine(a, b, whole, tol, depth) {
    const m = 0.5 * (a + b);
    const l = this.gauss(a, m), r = this.gauss(m, b);
    if (depth >= MAX_DEPTH || Math.abs(l + r - whole) <= tol) {
      this.s.push(this.s[this.s.length - 1] + l);
      this.t.push(m);
      this.s.push(this.s[this.s.length - 1] + r);
      this.t.push(b);
      return;
    }
    this.refine(a, m, l, 0.5 * tol, depth + 1);
    this.refine(m, b, r, 0.5 * tol, depth + 1);
  }
  get length() {
    return this.total;
  }
  tAtLength(s) {
    const { t: T, s: S } = this;
    if (T.length < 2 || !(this.total > 0)) return 0;
    if (!(s > 0)) return 0;
    if (s >= this.total) return 1;
    let lo = 0, hi = S.length;  // upper_bound
    while (lo < hi) {
      const mid = (lo + hi) >> 1;
      if (S[mid] <= s) lo = mid + 1;
      else hi = mid;
    }
    let k = Math.max(0, lo - 1);
    k = Math.min(k, S.length - 2);
    const a = T[k], b = T[k + 1];
    const span = S[k + 1] - S[k];
    const target = s - S[k];
    if (!(span > 0)) return a;
    let L = a, H = b;
    let t = a + (b - a) * (target / span);
    const ftol = this.tol * 1e-3;
    for (let iter = 0; iter < 40; iter += 1) {
      const f = this.gauss(a, t) - target;
      if (Math.abs(f) <= ftol) break;
      if (f > 0) H = t;
      else L = t;
      if (H - L <= 1e-15) break;
      const d = this.speed(t);
      const next = d > 0 ? t - f / d : L - 1;
      t = next > L && next < H ? next : 0.5 * (L + H);
    }
    return t;
  }
}

export class PathGeometry {
  constructor(path, { aspect = 16 / 9, tolerance = 1e-7 } = {}) {
    this.aspect = Number.isFinite(aspect) && aspect > 0 ? aspect : 1;
    const tol = Number.isFinite(tolerance) && tolerance > 0 ? tolerance : 1e-7;
    this.pointCount = path.points.length;
    this.closed = !!path.closed;
    this.cum = [0];
    this.segs = [];
    this.tables = [];
    if (this.pointCount === 0) return;
    this.firstPoint = this.toDisplay(path.points[0].p);
    const n = this.pointCount;
    const nseg = this.closed ? n : n - 1;
    for (let i = 0; i < nseg; i += 1) {
      const a = path.points[i], b = path.points[(i + 1) % n];
      const c = new Cubic(this.toDisplay(a.p), this.toDisplay(add(a.p, a.out)), this.toDisplay(add(b.p, b.in)),
        this.toDisplay(b.p));
      this.segs.push(c);
      const tab = new ArcLengthTable(c, tol);
      this.tables.push(tab);
      this.cum.push(this.cum[this.cum.length - 1] + tab.length);
    }
  }
  get empty() {
    return this.pointCount === 0;
  }
  get length() {
    return this.cum[this.cum.length - 1];
  }
  get segmentCount() {
    return this.segs.length;
  }
  toDisplay(n) {
    return { x: n.x * this.aspect, y: n.y };
  }
  toNormalized(d) {
    return { x: d.x / this.aspect, y: d.y };
  }
  wrap(d) {
    const L = this.length;
    let w = d - L * Math.floor(d / L);
    if (w < 0 || w > L) w = 0;
    return w;
  }
  neighbourTangent(seg, bias) {
    const n = this.segs.length;
    for (let pass = 0; pass < 2; pass += 1) {
      const dir = pass === 0 ? (bias >= 0 ? 1 : -1) : bias >= 0 ? -1 : 1;
      let k = seg;
      for (let step = 0; step < n; step += 1) {
        k += dir;
        if (this.closed) k = ((k % n) + n) % n;
        else if (k < 0 || k >= n) break;
        if (this.tables[k].length > 0) return robustTangent(this.segs[k], dir > 0 ? 0 : 1, dir);
      }
    }
    return { ok: false, dir: { x: 0, y: 0 } };
  }
  tangentAt(seg, t, bias) {
    if (this.tables[seg].length > 0) {
      const r = robustTangent(this.segs[seg], t, bias);
      if (r.ok) return r;
    }
    return this.neighbourTangent(seg, bias);
  }
  sampleAtDistance(d, bias, extend) {
    const L = this.length;
    if (this.segs.length === 0 || !(L > 0)) {
      return { pos: this.segs.length === 0 ? this.firstPoint || { x: 0, y: 0 } : this.segs[0].p0, tangent: { x: 0, y: 0 },
        tangentValid: false, segment: this.segs.length === 0 ? -1 : 0, t: 0 };
    }
    if (!Number.isFinite(d)) d = 0;
    if (this.closed) {
      d = this.wrap(d);
      if (bias < 0 && d === 0) d = L;
      if (bias >= 0 && d === L) d = 0;
    } else if (d < 0 || d > L) {
      if (extend) {
        const before = d < 0;
        const e = this.sampleAtDistance(before ? 0 : L, before ? 1 : -1, false);
        if (e.tangentValid) e.pos = add(e.pos, mul(e.tangent, before ? d : d - L));
        return e;
      }
      d = Math.min(Math.max(d, 0), L);
    }
    const n = this.segs.length;
    const cum = this.cum;
    const jointEps = 1e-12 * Math.max(1, L);
    let k = 0, local = 0;
    if (bias >= 0) {
      // upper_bound over cum[1..n] for d + eps
      const x = d + jointEps;
      let lo = 1, hi = cum.length;
      while (lo < hi) {
        const mid = (lo + hi) >> 1;
        if (cum[mid] <= x) lo = mid + 1;
        else hi = mid;
      }
      if (lo === cum.length) {
        k = n - 1;
        while (k > 0 && !(this.tables[k].length > 0)) k -= 1;
        local = this.tables[k].length;
      } else {
        k = lo - 1;
        local = d - cum[k];
      }
    } else {
      // lower_bound over cum[0..n-1] for d - eps
      const x = d - jointEps;
      let lo = 0, hi = cum.length - 1;
      while (lo < hi) {
        const mid = (lo + hi) >> 1;
        if (cum[mid] < x) lo = mid + 1;
        else hi = mid;
      }
      if (lo === 0) {
        k = 0;
        while (k < n - 1 && !(this.tables[k].length > 0)) k += 1;
        local = 0;
      } else {
        k = lo - 1;
        local = d - cum[k];
      }
    }
    const tab = this.tables[k];
    local = Math.min(Math.max(local, 0), tab.length);
    const t = tab.tAtLength(local);
    const tan = this.tangentAt(k, t, bias);
    return { pos: this.segs[k].eval(t), tangent: tan.dir, tangentValid: tan.ok, segment: k, t };
  }
  distanceForU(u, mode) {
    if (!Number.isFinite(u)) u = 0;
    const L = this.length;
    if (mode === SpeedMode.ConstantSpeed || this.segs.length === 0) return u * L;
    const n = this.segs.length;
    const x = u * n;
    if (x <= 0) return x * this.tables[0].length;
    if (x >= n) return L + (x - n) * this.tables[n - 1].length;
    const k = Math.min(n - 1, Math.floor(x));
    return this.cum[k] + (x - k) * this.tables[k].length;
  }
  smoothedTangent(d, sigma, bias) {
    const raw = this.sampleAtDistance(d, bias, !this.closed);
    const L = this.length;
    if (!(sigma > 0) || !(L > 0)) return { ok: raw.tangentValid, dir: raw.tangent };
    sigma = Math.min(sigma, this.closed ? 0.5 * L : L);
    const gx = [-0.86113631159405257522, -0.3399810435848562648, 0.3399810435848562648, 0.86113631159405257522];
    const gw = [0.34785484513745385737, 0.65214515486254614263, 0.65214515486254614263, 0.34785484513745385737];
    const SUB = 4;
    const meanPos = (from, to) => {
      let ax = 0, ay = 0;
      const h = (to - from) / SUB;
      for (let i = 0; i < SUB; i += 1) {
        const mid = from + (i + 0.5) * h;
        for (let j = 0; j < 4; j += 1) {
          const p = this.sampleAtDistance(mid + 0.5 * h * gx[j], bias, !this.closed).pos;
          ax += p.x * (gw[j] * 0.5);
          ay += p.y * (gw[j] * 0.5);
        }
      }
      return { x: ax / SUB, y: ay / SUB };
    };
    const dir = sub(meanPos(d, d + sigma), meanPos(d - sigma, d));
    if (len(dir) > 1e-9 * sigma) return { ok: true, dir: normalizeOrZero(dir) };
    return { ok: raw.tangentValid, dir: raw.tangent };
  }
}
