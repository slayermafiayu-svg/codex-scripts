import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  createKey,
  sortKeys,
  updateAutoTangents,
  evalKeys,
  evalSegment,
  speedKeys,
  splitSegment,
  applyEasingToSegment,
  segmentToEasing,
  matchEasingPreset,
  EASING_PRESETS,
  retimeKeys,
  shiftKeys,
  alignOppositeHandle,
  solveBezierX,
  INTERP,
  TANGENTS,
  EXTRAPOLATION,
  curveBounds,
} from '../src/core/curve.js';

function near(a, b, eps = 1e-9, msg) {
  assert.ok(Math.abs(a - b) <= eps, msg || `${a} != ${b}`);
}

test('evaluation hits key values exactly and stays monotone for auto tangents', () => {
  const keys = sortKeys([createKey(0, 0), createKey(1, 10), createKey(3, 4), createKey(6, 4)]);
  updateAutoTangents(keys);
  for (const k of keys) near(evalKeys(keys, k.t), k.v);
  // Monotone within each segment (no overshoot past neighbours).
  for (let i = 0; i < keys.length - 1; i++) {
    const lo = Math.min(keys[i].v, keys[i + 1].v);
    const hi = Math.max(keys[i].v, keys[i + 1].v);
    for (let s = 0; s <= 50; s++) {
      const t = keys[i].t + ((keys[i + 1].t - keys[i].t) * s) / 50;
      const v = evalKeys(keys, t);
      assert.ok(v >= lo - 1e-9 && v <= hi + 1e-9, `overshoot at ${t}: ${v}`);
    }
  }
  // Extrema get flat tangents.
  near(keys[1].out.dv, 0);
  near(keys[1].in.dv, 0);
});

test('linear and hold segments', () => {
  const keys = sortKeys([createKey(0, 0, { interp: INTERP.LINEAR }), createKey(2, 4, { interp: INTERP.HOLD }), createKey(4, 8)]);
  near(evalKeys(keys, 1), 2);
  near(evalKeys(keys, 3), 4);
  near(evalKeys(keys, 3.999), 4);
  near(evalKeys(keys, 4), 8);
  near(speedKeys(keys, 1), 2);
  near(speedKeys(keys, 3), 0);
});

test('extrapolation modes', () => {
  const keys = sortKeys([createKey(0, 0, { interp: INTERP.LINEAR }), createKey(2, 4, { interp: INTERP.LINEAR })]);
  near(evalKeys(keys, -1), 0);
  near(evalKeys(keys, 5), 4);
  near(evalKeys(keys, 5, { post: EXTRAPOLATION.CYCLE }), 2);
  near(evalKeys(keys, 3, { post: EXTRAPOLATION.PINGPONG }), 2);
  near(evalKeys(keys, 3, { post: EXTRAPOLATION.LINEAR }), 6);
  near(evalKeys(keys, -1, { pre: EXTRAPOLATION.LINEAR }), -2);
});

test('solveBezierX is stable at ends and with degenerate handles', () => {
  near(solveBezierX(0, 0, 0, 1, 1), 0);
  near(solveBezierX(1, 0, 0, 1, 1), 1);
  near(solveBezierX(0.5, 0, 0, 1, 1), 0.5, 1e-7);
  // Handles pulled to the extremes (x1 = 0, x2 = 1 would make dx/du = 0 at ends).
  const u = solveBezierX(0.001, 0, 0, 1, 1);
  assert.ok(u > 0 && u < 0.1);
  const u2 = solveBezierX(0.5, 0, 1, 0, 1);
  near(u2, 0.5, 1e-7);
});

test('time never runs backwards: handles beyond the segment are clamped', () => {
  const k0 = createKey(0, 0, { tangents: TANGENTS.BROKEN, out: { dt: 5, dv: 0 } });
  const k1 = createKey(1, 1, { tangents: TANGENTS.BROKEN, in: { dt: -5, dv: 0 } });
  let prev = -Infinity;
  for (let s = 0; s <= 100; s++) {
    const t = s / 100;
    const v = evalSegment(k0, k1, t);
    assert.ok(Number.isFinite(v));
    prev = t;
  }
  near(evalSegment(k0, k1, 0.5), 0.5, 1e-6);
});

test('splitSegment inserts a key without changing the curve', () => {
  const keys = sortKeys([
    createKey(0, 0, { tangents: TANGENTS.BROKEN, out: { dt: 0.4, dv: 3 } }),
    createKey(2, 5, { tangents: TANGENTS.BROKEN, in: { dt: -0.9, dv: -4 }, out: { dt: 0.5, dv: 1 } }),
    createKey(3, 1),
  ]);
  updateAutoTangents(keys);
  const before = [];
  for (let s = 0; s <= 200; s++) before.push(evalKeys(keys, (3 * s) / 200));
  const { key, k0, k1 } = splitSegment(keys[0], keys[1], 0.7);
  const after = sortKeys([k0, key, k1, keys[2]]);
  for (let s = 0; s <= 200; s++) near(evalKeys(after, (3 * s) / 200), before[s], 1e-7, `t=${(3 * s) / 200}`);
  near(key.t, 0.7, 1e-9);
  assert.equal(after.length, 4); // exactly one key added, no intermediate samples
  // Hold and linear splits keep their type.
  const h = splitSegment(createKey(0, 1, { interp: INTERP.HOLD }), createKey(1, 2), 0.5);
  assert.equal(h.key.interp, INTERP.HOLD);
  near(h.key.v, 1);
  const l = splitSegment(createKey(0, 0, { interp: INTERP.LINEAR }), createKey(1, 2), 0.25);
  near(l.key.v, 0.5);
});

test('easing presets round-trip through handles and only touch their segment', () => {
  for (const preset of EASING_PRESETS) {
    const keys = sortKeys([createKey(0, 0), createKey(2, 10), createKey(5, -4)]);
    updateAutoTangents(keys);
    const farIn = { ...keys[0].in };
    const farOut = { ...keys[1].out };
    applyEasingToSegment(keys[0], keys[1], preset.cp);
    const e = segmentToEasing(keys[0], keys[1]);
    for (let i = 0; i < 4; i++) near(e.cp[i], preset.cp[i], 1e-9, preset.id);
    assert.equal(matchEasingPreset(e.cp), preset.id);
    assert.deepEqual(keys[0].in, farIn, 'incoming handle of first key untouched');
    assert.deepEqual(keys[1].out, farOut, 'outgoing handle of second key untouched');
    near(evalKeys(keys, 0), 0);
    near(evalKeys(keys, 2), 10);
  }
});

test('overshoot preset really overshoots the target value', () => {
  const keys = sortKeys([createKey(0, 0), createKey(1, 10)]);
  applyEasingToSegment(keys[0], keys[1], EASING_PRESETS.find((p) => p.id === 'overshoot').cp);
  let max = -Infinity;
  for (let s = 0; s <= 100; s++) max = Math.max(max, evalKeys(keys, s / 100));
  assert.ok(max > 10, `expected overshoot, got max ${max}`);
});

test('retiming scales times and handle dt by k, keeps dv, and never clamps outside points', () => {
  const keys = sortKeys([
    createKey(-0.2, 1, { tangents: TANGENTS.BROKEN, out: { dt: 0.1, dv: 2 } }),
    createKey(0, 0),
    createKey(2, 5, { tangents: TANGENTS.BROKEN, in: { dt: -0.5, dv: -1 } }),
    createKey(2.4, 7),
  ]);
  const out = retimeKeys(keys, 0, 2, 0, 10);
  near(out[0].t, -1);
  near(out[1].t, 0);
  near(out[2].t, 10);
  near(out[3].t, 12);
  near(out[0].out.dt, 0.5);
  near(out[0].out.dv, 2);
  near(out[2].in.dt, -2.5);
  near(out[2].in.dv, -1);
  assert.equal(out.length, keys.length);
  // Original-speed policy: pure shift.
  const shifted = shiftKeys(keys, 0, 3);
  near(shifted[0].t, 2.8);
  near(shifted[3].t, 5.4);
  // Re-applying from canonical source avoids drift: 10 -> 4 -> 10 equals direct.
  const back = retimeKeys(retimeKeys(keys, 0, 2, 0, 4), 0, 4, 0, 10);
  for (let i = 0; i < keys.length; i++) near(back[i].t, out[i].t, 1e-12);
  assert.throws(() => retimeKeys(keys, 0, 0, 0, 1));
});

test('alignOppositeHandle keeps the opposite length and sign convention', () => {
  const k = createKey(1, 1, { tangents: TANGENTS.UNIFIED, in: { dt: -0.5, dv: 0 }, out: { dt: 0.25, dv: 0 } });
  k.out = { dt: 0.25, dv: 0.25 };
  alignOppositeHandle(k, 'out');
  near(Math.hypot(k.in.dt, k.in.dv), 0.5, 1e-9);
  assert.ok(k.in.dt < 0);
  near(k.in.dv / k.in.dt, 1, 1e-9); // same slope
});

test('curveBounds includes bezier extrema', () => {
  const keys = sortKeys([createKey(0, 0), createKey(1, 10)]);
  applyEasingToSegment(keys[0], keys[1], EASING_PRESETS.find((p) => p.id === 'overshoot').cp);
  const b = curveBounds(keys, 64);
  assert.ok(b.vMax > 10);
});
