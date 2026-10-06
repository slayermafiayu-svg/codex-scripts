// Cross-checks the JavaScript core against vectors written by the C++ core
// (tools/golden.cpp -> testdata/golden.json). The editor preview must show the
// same positions the OFX renderer produces.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

import { loadDocument, saveDocument, makeDocument, reversed, Status } from '../src/core/document.js';
import { PathGeometry } from '../src/core/geometry.js';
import { evalEasing, easingFor, EasingPreset, mapProgress, EndBehavior, LINEAR } from '../src/core/timing.js';
import { insertPoint, moveHandle, setMode, transformPath } from '../src/core/edit.js';
import { makePreset } from '../src/core/presets.js';

const here = dirname(fileURLToPath(import.meta.url));
const golden = JSON.parse(readFileSync(join(here, '../../testdata/golden.json'), 'utf8'));

const near = (a, b, tol, msg) => {
  if (!(Math.abs(a - b) <= tol)) assert.fail(`${msg}: ${a} vs ${b} (tol ${tol})`);
};
const nearVec = (a, b, tol, msg) => {
  near(a.x, b[0], tol, `${msg}.x`);
  near(a.y, b[1], tol, `${msg}.y`);
};

test('geometry matches the C++ core (lengths, positions, tangents, segments)', () => {
  let checked = 0;
  for (const c of golden.geometry) {
    const r = loadDocument(c.doc);
    assert.equal(r.status, Status.Ok, c.name);
    const g = new PathGeometry(r.doc.path, { aspect: c.aspect, tolerance: 1e-9 });
    near(g.length, c.length, 1e-12, `${c.name} length`);
    c.segmentLengths.forEach((l, i) => near(g.tables[i].length, l, 1e-12, `${c.name} seg ${i}`));
    for (const s of c.samples) {
      const d = g.distanceForU(s.u, s.mode);
      near(d, s.d, 1e-12, `${c.name} d(u=${s.u})`);
      const js = g.sampleAtDistance(d, s.bias, s.extend);
      nearVec(js.pos, s.pos, 1e-10, `${c.name} pos(u=${s.u},mode=${s.mode},bias=${s.bias})`);
      assert.equal(js.tangentValid, s.tanValid, `${c.name} tanValid u=${s.u}`);
      nearVec(js.tangent, s.tan, 1e-8, `${c.name} tan(u=${s.u},bias=${s.bias})`);
      assert.equal(js.segment, s.seg, `${c.name} segment u=${s.u} bias=${s.bias}`);
      // t is only defined up to arc-length tolerance / local speed; positions
      // above are compared tightly.
      near(js.t, s.t, 1e-7, `${c.name} t(u=${s.u})`);
      checked += 1;
    }
    for (const s of c.smoothed) {
      const r2 = g.smoothedTangent(s.d, s.sigma, 1);
      assert.equal(r2.ok, s.ok);
      nearVec(r2.dir, s.tan, 1e-9, `${c.name} smoothed(d=${s.d})`);
    }
  }
  assert.ok(checked > 1500);
});

test('presets build the same editable points as the C++ core', () => {
  for (const p of golden.presets) {
    const cpp = loadDocument(p.doc).doc.path;
    const js = makePreset(p.key, p.aspect);
    assert.equal(js.closed, cpp.closed, p.key);
    assert.equal(js.points.length, cpp.points.length, p.key);
    js.points.forEach((pt, i) => {
      const q = cpp.points[i];
      assert.equal(pt.id, q.id);
      assert.equal(pt.mode, q.mode);
      for (const k of ['p', 'in', 'out']) {
        near(pt[k].x, q[k].x, 1e-12, `${p.key}[${i}].${k}.x`);
        near(pt[k].y, q[k].y, 1e-12, `${p.key}[${i}].${k}.y`);
      }
    });
  }
});

test('easing curves match', () => {
  for (const e of golden.easing) {
    const [x1, y1, x2, y2] = e.curve;
    near(evalEasing({ x1, y1, x2, y2 }, e.x), e.y, 1e-12, `easing ${e.curve} @${e.x}`);
  }
});

test('progress mapping matches (end behaviours, reverse, offset, easing, closed)', () => {
  const ends = { clamp: EndBehavior.Clamp, extend: EndBehavior.Extend, loop: EndBehavior.Loop, pingpong: EndBehavior.PingPong };
  for (const g of golden.progress) {
    const st = mapProgress(g.p, g.closed, {
      startOffset: g.offset,
      reverse: g.reverse,
      endBehavior: ends[g.end],
      easing: g.ease ? easingFor(EasingPreset.EaseInOut) : LINEAR,
    });
    near(st.u, g.u, 1e-12, `u p=${g.p} ${g.end} closed=${g.closed} rev=${g.reverse} ease=${g.ease}`);
    assert.equal(st.direction, g.direction);
    assert.equal(st.lap, g.lap);
  }
});

test('editing operations produce the same documents', () => {
  const samePath = (a, b, msg) => {
    assert.equal(a.closed, b.closed, msg);
    assert.equal(a.points.length, b.points.length, msg);
    a.points.forEach((pt, i) => {
      assert.equal(pt.mode, b.points[i].mode, `${msg} mode ${i}`);
      assert.equal(pt.id, b.points[i].id, `${msg} id ${i}`);
      for (const k of ['p', 'in', 'out']) {
        near(pt[k].x, b.points[i][k].x, 1e-12, `${msg} ${k}.x ${i}`);
        near(pt[k].y, b.points[i][k].y, 1e-12, `${msg} ${k}.y ${i}`);
      }
    });
  };
  for (const e of golden.edits) {
    const path = loadDocument(e.before).doc.path;
    const expected = loadDocument(e.after).doc.path;
    switch (e.op) {
      case 'insert':
        assert.equal(insertPoint(path, e.segment, e.t), e.index);
        samePath(path, expected, 'insert');
        break;
      case 'moveHandle':
        moveHandle(path, e.index, e.out, { x: e.to[0], y: e.to[1] }, e.aspect, false);
        samePath(path, expected, 'moveHandle');
        break;
      case 'setModeSmooth':
        setMode(path, e.index, 'smooth', e.aspect);
        samePath(path, expected, 'setMode');
        break;
      case 'transform':
        transformPath(path, e.aspect, { x: 0.4, y: 0.6 }, 0.7, 33, { x: 0.05, y: -0.02 });
        samePath(path, expected, 'transform');
        break;
      case 'reverse':
        samePath(reversed(path), expected, 'reverse');
        break;
      default:
        assert.fail(`unknown op ${e.op}`);
    }
  }
});

test('documents written by JS load in the same shape (round trip through C++ format)', () => {
  for (const p of golden.presets) {
    const js = saveDocument(makeDocument(makePreset(p.key, p.aspect)));
    const back = loadDocument(js);
    assert.equal(back.status, Status.Ok);
    assert.equal(JSON.stringify(JSON.parse(js)), JSON.stringify(JSON.parse(p.doc)));
  }
});
