import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  makeFps,
  fpsFromNumber,
  secondsToFrames,
  framesToSeconds,
  snapToFrame,
  visibleFrames,
  fitsInRange,
  formatTime,
  parseTime,
  parseNumber,
  chooseTimeStep,
  chooseValueStep,
  formatValue,
} from '../src/core/time.js';

test('rational fps is never re-derived from a rounded label', () => {
  assert.deepEqual(fpsFromNumber(29.97), { num: 30000, den: 1001 });
  assert.deepEqual(fpsFromNumber(23.976), { num: 24000, den: 1001 });
  assert.deepEqual(fpsFromNumber(59.94), { num: 60000, den: 1001 });
  assert.deepEqual(fpsFromNumber(25), { num: 25, den: 1 });
  assert.deepEqual(makeFps(60000, 2002), { num: 30000, den: 1001 });
});

test('frame <-> seconds round trip is exact for NTSC rates', () => {
  for (const fps of [makeFps(24000, 1001), makeFps(30000, 1001), makeFps(60000, 1001), makeFps(30)]) {
    for (const n of [0, 1, 17, 179, 180, 1799, 86400]) {
      const t = framesToSeconds(n, fps);
      assert.ok(Math.abs(secondsToFrames(t, fps) - n) < 1e-9, `${fps.num}/${fps.den} frame ${n}`);
      assert.ok(Math.abs(snapToFrame(t + 1e-7, fps) - t) < 1e-12);
    }
  }
});

test('end boundary vs last displayable frame (6 s at 30 fps)', () => {
  const fps = makeFps(30);
  const vf = visibleFrames({ start: 0, end: 6 }, fps);
  assert.equal(vf.count, 180);
  assert.equal(vf.lastFrame, 179);
  assert.equal(vf.boundary, 6);
  // A key exactly on the boundary fits; 6.007 does not.
  assert.equal(fitsInRange(6.0, { start: 0, end: 6 }, fps), true);
  assert.equal(fitsInRange(framesToSeconds(180, fps), { start: 0, end: 6 }, fps), true);
  assert.equal(fitsInRange(6.007, { start: 0, end: 6 }, fps), false);
  // 29.97: 6 s holds 179.82 frames -> 180 displayable frames (last partially).
  const ntsc = visibleFrames({ start: 0, end: 6 }, makeFps(30000, 1001));
  assert.equal(ntsc.count, 180);
});

test('timecode and frame formatting', () => {
  const fps = makeFps(30);
  assert.equal(formatTime(6, fps, 'timecode'), '00:06:00');
  assert.equal(formatTime(framesToSeconds(179, fps), fps, 'timecode'), '00:05:29');
  assert.equal(formatTime(3661.5, fps, 'timecode'), '01:01:01:15');
  assert.equal(formatTime(framesToSeconds(179, fps), fps, 'frames'), '179');
  assert.equal(formatTime(2.5, fps, 'seconds'), '2.500s');
  assert.equal(formatTime(-1, fps, 'frames'), '-30');
  const ntsc = makeFps(30000, 1001);
  assert.equal(formatTime(framesToSeconds(30, ntsc), ntsc, 'timecode'), '00:01:00');
});

test('parseTime accepts frames, seconds with comma, timecode; rejects junk', () => {
  const fps = makeFps(30);
  assert.equal(parseTime('90', fps), 3);
  assert.equal(parseTime('2,5s', fps), 2.5);
  assert.equal(parseTime('2.5s', fps), 2.5);
  assert.equal(parseTime('00:02:15', fps), 2.5);
  assert.equal(parseTime('02:15', fps), 2.5);
  assert.equal(parseTime('+15', fps, { base: 1 }), 1.5);
  assert.equal(parseTime('abc', fps), null);
  assert.equal(parseTime('', fps), null);
  assert.equal(parseTime('00:02:30', fps), null); // ff >= fps
  assert.equal(parseTime('12', fps, { mode: 'seconds' }), 12);
});

test('parseNumber handles comma decimals and never returns 0 for invalid', () => {
  assert.equal(parseNumber('3,5'), 3.5);
  assert.equal(parseNumber('-12.25'), -12.25);
  assert.equal(parseNumber('1.234,5'), 1234.5);
  assert.equal(parseNumber('1,234.5'), 1234.5);
  assert.equal(parseNumber('45°'), 45);
  assert.equal(parseNumber(''), null);
  assert.equal(parseNumber('x'), null);
  assert.equal(parseNumber('1..2'), null);
});

test('ruler steps stay readable across zoom levels', () => {
  const fps = makeFps(30);
  const zoomedIn = chooseTimeStep(900, fps); // 30 px per frame
  assert.equal(zoomedIn.unit, 'frame');
  assert.ok(zoomedIn.majorFrames >= 1 && zoomedIn.majorFrames <= 5);
  const medium = chooseTimeStep(120, fps); // 4 px per frame
  assert.ok(medium.major * 120 >= 72);
  const zoomedOut = chooseTimeStep(2, fps);
  assert.equal(zoomedOut.unit, 'second');
  assert.ok(zoomedOut.major * 2 >= 72);
  // Labels never closer than the minimum.
  for (const px of [1, 3, 10, 40, 100, 400, 2000]) {
    const s = chooseTimeStep(px, fps);
    assert.ok(s.major * px >= 72 - 1e-9, `px ${px}`);
    assert.ok(s.minor > 0 && s.minor <= s.major);
  }
});

test('value steps are 1/2/5 ladders with matching decimals', () => {
  const a = chooseValueStep(100);
  assert.equal(a.major, 0.5);
  assert.equal(a.decimals, 1);
  const b = chooseValueStep(2);
  assert.equal(b.major, 20);
  assert.equal(b.decimals, 0);
  assert.equal(formatValue(12.3456, 2), '12.35');
  assert.equal(formatValue(12, 2), '12');
  assert.equal(formatValue(-0.001, 2), '0');
});
