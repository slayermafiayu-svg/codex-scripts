// Progress -> position along the path. Mirrors core/src/timing.cpp.
export const EndBehavior = Object.freeze({ Clamp: 0, Extend: 1, Loop: 2, PingPong: 3 });
export const EasingPreset = Object.freeze({ Linear: 0, EaseIn: 1, EaseOut: 2, EaseInOut: 3, BackOut: 4, Custom: 5 });

export const LINEAR = Object.freeze({ x1: 0, y1: 0, x2: 1, y2: 1 });

export const isLinear = (e) => e.x1 === e.y1 && e.x2 === e.y2;

export function evalEasing(e, x) {
  if (!Number.isFinite(x)) return 0;
  x = Math.min(Math.max(x, 0), 1);
  if (isLinear(e)) return x;
  if (x <= 0) return 0;
  if (x >= 1) return 1;
  const ax1 = Math.min(Math.max(e.x1, 0), 1), ax2 = Math.min(Math.max(e.x2, 0), 1);
  const bx = (s) => { const u = 1 - s; return 3 * u * u * s * ax1 + 3 * u * s * s * ax2 + s * s * s; };
  const dbx = (s) => { const u = 1 - s; return 3 * u * u * ax1 + 6 * u * s * (ax2 - ax1) + 3 * s * s * (1 - ax2); };
  const by = (s) => { const u = 1 - s; return 3 * u * u * s * e.y1 + 3 * u * s * s * e.y2 + s * s * s; };
  let lo = 0, hi = 1, s = x;
  for (let i = 0; i < 64; i += 1) {
    const f = bx(s) - x;
    if (Math.abs(f) < 1e-14) break;
    if (f > 0) hi = s;
    else lo = s;
    const d = dbx(s);
    const next = d > 1e-12 ? s - f / d : -1;
    s = next > lo && next < hi ? next : 0.5 * (lo + hi);
    if (hi - lo < 1e-15) break;
  }
  return by(s);
}

export function easingFor(preset, custom = LINEAR) {
  switch (preset) {
    case EasingPreset.EaseIn: return { x1: 0.42, y1: 0, x2: 1, y2: 1 };
    case EasingPreset.EaseOut: return { x1: 0, y1: 0, x2: 0.58, y2: 1 };
    case EasingPreset.EaseInOut: return { x1: 0.42, y1: 0, x2: 0.58, y2: 1 };
    case EasingPreset.BackOut: return { x1: 0.34, y1: 1.56, x2: 0.64, y2: 1 };
    case EasingPreset.Custom: return { ...custom };
    default: return { ...LINEAR };
  }
}

export function lapEase(p, e) {
  if (!Number.isFinite(p)) return 0;
  if (isLinear(e)) return p;
  const base = Math.floor(p);
  return base + evalEasing(e, p - base);
}

// settings: { startOffset, reverse, endBehavior, easing }
export function mapProgress(progress, closed, s) {
  const st = { u: 0, direction: 1, lap: 0, clamped: false };
  if (!Number.isFinite(progress)) progress = 0;
  const offset = Number.isFinite(s.startOffset) ? s.startOffset : 0;
  let q = lapEase(progress, s.easing || LINEAR) + offset;
  q = Math.min(Math.max(q, -1e12), 1e12);
  let sign = 1;
  if (s.reverse) {
    q = 1 - q;
    sign = -1;
  }
  if (closed) {
    st.u = q - Math.floor(q);
    st.direction = sign;
    return st;
  }
  switch (s.endBehavior ?? EndBehavior.Clamp) {
    case EndBehavior.Extend:
      st.u = q;
      st.direction = sign;
      break;
    case EndBehavior.Loop: {
      const lap = q > 0 ? Math.ceil(q) - 1 : Math.floor(q);
      st.u = q - lap;
      st.lap = lap;
      st.direction = sign;
      break;
    }
    case EndBehavior.PingPong: {
      const m = q - 2 * Math.floor(q * 0.5);
      if (m <= 1) {
        st.u = m;
        st.direction = sign;
      } else {
        st.u = 2 - m;
        st.direction = -sign;
      }
      break;
    }
    default:
      st.clamped = q < 0 || q > 1;
      st.u = Math.min(Math.max(q, 0), 1);
      st.direction = sign;
  }
  return st;
}
