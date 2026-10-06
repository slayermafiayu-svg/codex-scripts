// 2D vector helpers ({x, y} objects). Mirrors core/include/wmp/vec2.h.
export const vec = (x = 0, y = 0) => ({ x, y });
export const add = (a, b) => ({ x: a.x + b.x, y: a.y + b.y });
export const sub = (a, b) => ({ x: a.x - b.x, y: a.y - b.y });
export const mul = (a, s) => ({ x: a.x * s, y: a.y * s });
export const neg = (a) => ({ x: -a.x, y: -a.y });
export const dot = (a, b) => a.x * b.x + a.y * b.y;
export const cross = (a, b) => a.x * b.y - a.y * b.x;
export const len = (a) => Math.hypot(a.x, a.y);
export const lerp = (a, b, t) => ({ x: a.x + (b.x - a.x) * t, y: a.y + (b.y - a.y) * t });
export const eq = (a, b) => a.x === b.x && a.y === b.y;
export const isZero = (a) => a.x === 0 && a.y === 0;
export const isFiniteVec = (a) => Number.isFinite(a.x) && Number.isFinite(a.y);
export function normalizeOrZero(a, eps = 0) {
  const l = len(a);
  return l > eps ? { x: a.x / l, y: a.y / l } : { x: 0, y: 0 };
}
export const PI = Math.PI;
export const degToRad = (d) => d * (Math.PI / 180);
export const radToDeg = (r) => r * (180 / Math.PI);
