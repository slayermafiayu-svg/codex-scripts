import { test } from 'node:test';
import assert from 'node:assert/strict';
import { buildPreset, parsePreset, planApply, retimeKeys } from '../src/core/presets-file.js';
import { makePreset } from '../src/core/presets.js';

const path = makePreset('arc', 16 / 9);
const keys = [
  { t: -1, v: 0, interp: 'linear' },
  { t: 0, v: 0, interp: 'smooth' },
  { t: 2, v: 0.5, interp: 'manual', slopes: [0.2, 0.3] },
  { t: 4, v: 1, interp: 'linear' },
  { t: 6, v: 1.5, interp: 'hold' },
];

test('three content levels', () => {
  const values = { wmpRotation: 15, wmpOrient: true, wmpEndBehavior: 2, wmpBlur: true };
  const g = buildPreset({ name: 'a', level: 'geometry', path, aspect: 16 / 9, values });
  assert.equal(g.look, undefined);
  assert.equal(g.timing, undefined);
  const l = buildPreset({ name: 'b', level: 'look', path, aspect: 16 / 9, values });
  assert.deepEqual(l.look, { wmpRotation: 15, wmpOrient: true, wmpBlur: true });
  assert.equal(l.timing, undefined);
  const t = buildPreset({ name: 'c', level: 'timing', path, aspect: 16 / 9, values, keyframes: { wmpProgress: keys }, eventDuration: 4 });
  assert.equal(t.timing.sourceDuration, 4);
  assert.equal(t.timing.values.wmpEndBehavior, 2);
  assert.equal(t.timing.keyframes.wmpProgress.length, 5);
  assert.throws(() => buildPreset({ name: 'd', level: 'timing', path, aspect: 1, values }), /duration/);
  const round = parsePreset(JSON.parse(JSON.stringify(t)));
  assert.equal(round.path.points.length, path.points.length);
  assert.throws(() => parsePreset({ format: 'x' }), /not a/);
  assert.throws(() => parsePreset({ ...t, version: 99 }), /newer/);
});

test('fit to event scales all keys (outside ones keep relative position), preserve keeps them', () => {
  const fit = retimeKeys(keys, 4, 8, 'fit');
  assert.deepEqual(fit.map((k) => k.t), [-2, 0, 4, 8, 12]);
  assert.deepEqual(fit[2].slopes, [0.1, 0.15]);
  assert.equal(fit[4].interp, 'hold');
  assert.deepEqual(retimeKeys(keys, 4, 8, 'preserve').map((k) => k.t), keys.map((k) => k.t));
  assert.equal(retimeKeys(keys, 4, null, 'fit'), null);
});

test('applying a timing preset repeatedly never drifts (always from the source)', () => {
  const preset = buildPreset({ name: 'c', level: 'timing', path, aspect: 1, values: {}, keyframes: { wmpProgress: keys }, eventDuration: 4 });
  let last;
  for (const d of [7.1, 1.3, 23.976 / 7, 100, 0.01, 4]) last = planApply(preset, { targetDuration: d, timingMode: 'fit' });
  assert.deepEqual(last.keyframeWrites.wmpProgress.map((k) => k.t), keys.map((k) => k.t));
});

test('look values are not forced onto animated parameters', () => {
  const preset = buildPreset({ name: 'b', level: 'look', path, aspect: 1, values: { wmpRotation: 15, wmpScale: [50, 50] } });
  const plan = planApply(preset, { animatedIds: ['wmpRotation'] });
  assert.deepEqual(plan.skipped, ['wmpRotation']);
  assert.deepEqual(plan.paramWrites, { wmpScale: [50, 50] });
  const tp = buildPreset({ name: 'c', level: 'timing', path, aspect: 1, values: {}, keyframes: { wmpProgress: keys }, eventDuration: 4 });
  assert.equal(planApply(tp, { targetDuration: null, timingMode: 'fit' }).error, 'event-duration-unknown');
});
