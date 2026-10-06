import { test } from 'node:test';
import assert from 'node:assert/strict';
import { loadDocument, saveDocument, makeDocument, Status, reversed, MAX_POINTS } from '../src/core/document.js';
import { History } from '../src/core/history.js';
import { makePreset } from '../src/core/presets.js';

test('empty, corrupt, future and unknown data', () => {
  assert.equal(loadDocument('').status, Status.Empty);
  assert.equal(loadDocument('{').status, Status.Invalid);
  assert.equal(loadDocument('{"format":"x","version":1,"path":{}}').status, Status.Invalid);
  assert.equal(loadDocument('{"format":"wron.motionpath","version":1,"path":{"points":[{"p":[1e999,0]}]}}').status, Status.Invalid);
  assert.equal(loadDocument('{"format":"wron.motionpath","version":3,"path":{}}').status, Status.Future);
  const pts = Array.from({ length: MAX_POINTS + 1 }, () => ({ p: [0, 0] }));
  assert.equal(loadDocument(JSON.stringify({ format: 'wron.motionpath', version: 1, path: { points: pts } })).status, Status.Invalid);
  const doc = loadDocument('{"format":"wron.motionpath","version":1,"path":{"points":[{"p":[0,0],"x":1}]},"editor":{"grid":true},"z":[1]}');
  assert.equal(doc.status, Status.Ok);
  const saved = JSON.parse(saveDocument(doc.doc));
  assert.deepEqual(saved.editor, { grid: true });
  assert.deepEqual(saved.z, [1]);
  assert.equal(saved.path.points[0].x, 1);
});

test('reverse keeps shape and closed start; history merges nudges', () => {
  const c = makePreset('circle', 1);
  const r = reversed(c);
  assert.equal(r.points[0].id, c.points[0].id);
  assert.equal(r.points[1].id, c.points[3].id);
  assert.deepEqual(r.points[1].in, c.points[3].out);
  const h = new History();
  h.push('a', 1, 2, 'nudge');
  h.push('b', 2, 3, 'nudge');
  assert.equal(h.undoStack.length, 1);
  assert.equal(h.undo().after, 3);
  assert.ok(h.canRedo());
  h.push('c', 1, 5);
  assert.ok(!h.canRedo());
  assert.equal(saveDocument(makeDocument(c)).length > 0, true);
});
