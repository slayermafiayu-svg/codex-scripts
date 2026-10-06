import { test } from 'node:test';
import assert from 'node:assert/strict';
import { GraphStore, COLLISION } from '../src/core/model.js';
import { framesToSeconds } from '../src/core/time.js';
import { INTERP, TANGENTS } from '../src/core/curve.js';

const F = (n, fps = { num: 30, den: 1 }) => framesToSeconds(n, fps);

function sampleDoc() {
  return {
    fps: { num: 30, den: 1 },
    range: { start: 0, end: 6 },
    channels: [
      {
        id: 'px',
        name: 'Position X',
        keys: [
          { id: 'a', t: 0, v: 0 },
          { id: 'b', t: 3, v: 10 },
          { id: 'c', t: 6, v: 0 },
        ],
      },
      {
        id: 'py',
        name: 'Position Y',
        keys: [
          { id: 'd', t: 1, v: 5 },
          { id: 'e', t: 2, v: -5 },
        ],
      },
      { id: 'lk', name: 'Locked', locked: true, keys: [{ id: 'f', t: 1, v: 1 }] },
    ],
  };
}

test('load normalizes, keeps ids, sorts and does not add keys', () => {
  const s = new GraphStore({
    fps: 29.97,
    range: { start: 0, end: 6 },
    channels: [
      {
        id: 'x',
        keys: [
          { id: 'k2', t: 2, v: 1 },
          { id: 'k1', t: 1, v: 0 },
        ],
      },
    ],
  });
  assert.deepEqual(s.fps, { num: 30000, den: 1001 });
  assert.deepEqual(
    s.channel('x').keys.map((k) => k.id),
    ['k1', 'k2'],
  );
  assert.equal(s.channel('x').keys.length, 2);
  const exported = s.exportDoc();
  assert.equal(exported.channels[0].keys.length, 2);
  assert.equal(exported.channels[0].keys[0].id, 'k1');
});

test('group move preserves spacing, snaps the anchor to a frame, one undo entry', () => {
  const s = new GraphStore(sampleDoc());
  s.select(['a', 'b'], 'replace');
  s.begin('drag');
  for (const dt of [0.011, 0.02, 0.0333, 0.04]) {
    s.restoreTransactionStart();
    s.moveKeys(['a', 'b'], dt, 1, { anchorId: 'a' });
  }
  s.commit();
  const ch = s.channel('px');
  const a = ch.keys.find((k) => k.id === 'a');
  const b = ch.keys.find((k) => k.id === 'b');
  assert.ok(Math.abs(a.t - F(1)) < 1e-12, `anchor on frame 1, got ${a.t}`);
  assert.ok(Math.abs(b.t - a.t - 3) < 1e-12, 'spacing preserved');
  assert.equal(a.v, 1);
  assert.equal(b.v, 11);
  assert.equal(s._undo.length, 1);
  s.undo();
  assert.equal(s.channel('px').keys.find((k) => k.id === 'a').t, 0);
  assert.deepEqual([...s.selectedKeys].sort(), ['a', 'b']);
  s.redo();
  assert.ok(Math.abs(s.channel('px').keys.find((k) => k.id === 'a').t - F(1)) < 1e-12);
});

test('collision: block stops at the last free frame, replace removes the other key', () => {
  const s = new GraphStore(sampleDoc(), { collision: COLLISION.BLOCK });
  // Move key a (t=0) onto b (t=3): blocked one frame before.
  const r = s.moveKeys(['a'], 3, 0, { anchorId: 'a' });
  assert.equal(r.blocked, true);
  const a = s.channel('px').keys.find((k) => k.id === 'a');
  assert.ok(Math.abs(a.t - F(89)) < 1e-12, `expected frame 89, got ${a.t}`);
  assert.equal(s.channel('px').keys.length, 3);

  const s2 = new GraphStore(sampleDoc(), { collision: COLLISION.REPLACE });
  s2.moveKeys(['a'], 3, 0, { anchorId: 'a' });
  const keys = s2.channel('px').keys;
  assert.equal(keys.length, 2);
  assert.ok(keys.some((k) => k.id === 'a' && Math.abs(k.t - 3) < 1e-12));
  assert.ok(!keys.some((k) => k.id === 'b'));
});

test('locked channels are never edited or selected', () => {
  const s = new GraphStore(sampleDoc());
  s.select(['f'], 'replace');
  assert.equal(s.selectedKeys.size, 0);
  s.moveKeys(['f'], 1, 1);
  assert.equal(s.channel('lk').keys[0].t, 1);
  assert.equal(s.insertKey('lk', 2), null);
});

test('insertKey preserves curve shape and never creates duplicates on the same frame', () => {
  const s = new GraphStore(sampleDoc());
  const ch = s.channel('px');
  const before = [];
  for (let i = 0; i <= 180; i++) before.push(s.evalChannel('px', F(i)));
  const k = s.insertKey('px', 1.5);
  assert.equal(ch.keys.length, 4);
  for (let i = 0; i <= 180; i++) assert.ok(Math.abs(s.evalChannel('px', F(i)) - before[i]) < 1e-7, `frame ${i}`);
  const again = s.insertKey('px', 1.5 + 1e-6);
  assert.equal(again.id, k.id);
  assert.equal(ch.keys.length, 4);
  // Insert outside keyed range uses the extrapolated value.
  const far = s.insertKey('px', 8);
  assert.equal(far.v, 0);
  assert.equal(ch.keys.length, 5);
});

test('easing on a target segment only touches that segment', () => {
  const s = new GraphStore(sampleDoc());
  s.selectSegment('px', 'b'); // segment b -> c (last incoming segment)
  const segs = s.targetSegments();
  assert.equal(segs.length, 1);
  assert.equal(segs[0].k0.id, 'b');
  const inBefore = { ...s.channel('px').keys[1].in };
  s.applyEasing([0.42, 0, 0.58, 1]);
  const b = s.channel('px').keys[1];
  assert.deepEqual(b.in, inBefore, 'incoming handle of middle key untouched');
  assert.equal(b.tangents, TANGENTS.BROKEN);
  assert.ok(Math.abs(b.out.dt - 0.42 * 3) < 1e-9);
  // Single selected middle key targets both neighbouring segments.
  s.select(['b'], 'replace');
  assert.equal(s.targetSegments().length, 2);
  // Last key alone targets its incoming segment.
  s.select(['c'], 'replace');
  const last = s.targetSegments();
  assert.equal(last.length, 1);
  assert.equal(last[0].k1.id, 'c');
});

test('setHandle keeps handles inside the segment and unified alignment', () => {
  const s = new GraphStore(sampleDoc());
  s.setHandle('b', 'out', { dt: 10, dv: 4 });
  const b = s.channel('px').keys[1];
  assert.ok(b.out.dt <= 3 + 1e-12);
  assert.equal(b.tangents, TANGENTS.UNIFIED);
  assert.ok(b.in.dt < 0 && Math.abs(b.in.dv / b.in.dt - b.out.dv / b.out.dt) < 1e-9);
  s.setHandle('b', 'in', { dt: -0.5, dv: 0 }, { breakTangents: true });
  assert.equal(s.channel('px').keys[1].tangents, TANGENTS.BROKEN);
  assert.ok(Math.abs(s.channel('px').keys[1].out.dv - b.out.dv) < 1e-12, 'broken: other side untouched');
});

test('delete, copy/paste and duplicate keep ids stable and counts exact', () => {
  const s = new GraphStore(sampleDoc());
  const clip = s.copyKeys(['d', 'e']);
  assert.equal(clip.items.length, 2);
  const ids = s.pasteKeys(clip, 4);
  assert.equal(ids.length, 2);
  const py = s.channel('py');
  assert.equal(py.keys.length, 4);
  assert.ok(Math.abs(py.keys[2].t - 4) < 1e-12 && Math.abs(py.keys[3].t - 5) < 1e-12);
  s.deleteKeys(ids);
  assert.equal(py.keys.length, 2);
  assert.deepEqual(
    py.keys.map((k) => k.id),
    ['d', 'e'],
  );
  s.duplicateKeys(['d']);
  assert.equal(py.keys.length, 3);
  assert.ok(Math.abs(py.keys[1].t - F(31)) < 1e-12);
});

test('cancel restores everything, external undo mode records no history', () => {
  const s = new GraphStore(sampleDoc(), { undo: 'external' });
  let tx = null;
  s.on('transaction', (e) => (tx = e));
  s.begin('drag');
  s.moveKeys(['a'], 1, 0);
  s.cancel();
  assert.equal(s.channel('px').keys[0].t, 0);
  assert.equal(tx, null);
  s.moveKeys(['a'], 1, 0);
  assert.ok(tx && tx.before && tx.after);
  assert.equal(s.canUndo, false);
  s.applySnapshot(tx.before);
  assert.equal(s.channel('px').keys[0].t, 0);
});

test('scaleKeys around a pivot keeps keys on frames and folds duplicates', () => {
  const s = new GraphStore(sampleDoc());
  s.scaleKeys(['a', 'b', 'c'], { pivotT: 0, kT: 0.5 });
  const ks = s.channel('px').keys;
  assert.deepEqual(
    ks.map((k) => k.t),
    [0, 1.5, 3],
  );
  s.setInterp(['a'], INTERP.HOLD);
  assert.equal(s.channel('px').keys[0].interp, INTERP.HOLD);
  assert.equal(s.evalChannel('px', 1), 0);
});
