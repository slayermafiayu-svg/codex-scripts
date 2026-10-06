// The inspector's parameter schema must match the real plug-in (dumped from
// the built .ofx by tools/ofx_describe.cpp into testdata/ofx-params.json).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { PARAMS, sanitize, parseNumber, isEnabled, defaults } from '../src/core/params.js';

const here = dirname(fileURLToPath(import.meta.url));
const ofx = JSON.parse(readFileSync(join(here, '../../testdata/ofx-params.json'), 'utf8'));
const byId = Object.fromEntries(ofx.params.map((p) => [p.id, p]));
const TYPE = { double: 'OfxParamTypeDouble', double2: 'OfxParamTypeDouble2D', int: 'OfxParamTypeInteger', bool: 'OfxParamTypeBoolean', choice: 'OfxParamTypeChoice' };
const GROUP = { timing: 'wmpGrpTiming', transform: 'wmpGrpTransform', orientation: 'wmpGrpOrientation', blur: 'wmpGrpBlur' };

test('every editor parameter exists in the plug-in with the same type, group, default and range', () => {
  for (const p of PARAMS) {
    const o = byId[p.id];
    assert.ok(o, `${p.id} missing in plug-in`);
    assert.equal(o.type, TYPE[p.type], p.id);
    assert.equal(o.parent, GROUP[p.group], `${p.id} group`);
    const def = Array.isArray(p.def) ? p.def : [typeof p.def === 'boolean' ? (p.def ? 1 : 0) : p.def];
    assert.deepEqual(o.default, def, `${p.id} default`);
    if (p.type === 'choice') assert.equal(o.options.length, p.options.length, `${p.id} options`);
    if (p.min !== undefined) assert.deepEqual(o.min, Array.isArray(p.min) ? p.min : [p.min], `${p.id} min`);
    if (p.max !== undefined) assert.deepEqual(o.max, Array.isArray(p.max) ? p.max : [p.max], `${p.id} max`);
    assert.equal(o.animates, !!p.animates, `${p.id} animates`);
  }
});

test('sanitize never coerces invalid input', () => {
  const rot = PARAMS.find((p) => p.id === 'wmpRotation');
  assert.equal(sanitize(rot, NaN), null);
  assert.equal(sanitize(rot, 45), 45);
  const op = PARAMS.find((p) => p.id === 'wmpOpacity');
  assert.equal(sanitize(op, 150), 100);
  const ch = PARAMS.find((p) => p.id === 'wmpEndBehavior');
  assert.equal(sanitize(ch, 9), null);
  assert.equal(parseNumber('0,25'), 0.25);
  assert.equal(parseNumber(' -1.5e2 '), -150);
  assert.equal(parseNumber('abc'), null);
  assert.equal(parseNumber(''), null);
  const v = defaults();
  assert.equal(isEnabled(PARAMS.find((p) => p.id === 'wmpBlurSamples'), v), false);
  v.wmpBlur = true;
  v.wmpBlurAdaptive = false;
  assert.equal(isEnabled(PARAMS.find((p) => p.id === 'wmpBlurSamples'), v), true);
});
