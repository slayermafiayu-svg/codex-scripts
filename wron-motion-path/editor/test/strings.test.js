import { test } from 'node:test';
import assert from 'node:assert/strict';
import { STRINGS, LOCALES, translator, resolveLocale } from '../src/view/strings.js';
import { PARAMS } from '../src/core/params.js';
import { PRESET_KEYS } from '../src/core/presets.js';

test('all eight locales have exactly the reference keys and matching placeholders', () => {
  assert.deepEqual(LOCALES, ['en', 'tr', 'es', 'pt', 'fr', 'de', 'ru', 'zh-Hans']);
  const ref = STRINGS.en;
  const holders = (s) => (s.match(/\{[a-z]+\}/g) || []).sort().join(',');
  for (const loc of LOCALES) {
    const table = STRINGS[loc];
    assert.deepEqual(Object.keys(table).sort(), Object.keys(ref).sort(), loc);
    for (const [k, v] of Object.entries(table)) {
      assert.ok(typeof v === 'string' && v.trim().length > 0, `${loc}.${k} empty`);
      assert.equal(holders(v), holders(ref[k]), `${loc}.${k} placeholders`);
    }
  }
});

test('every parameter label, option and preset has a string', () => {
  for (const p of PARAMS) {
    assert.ok(STRINGS.en[p.label], p.label);
    for (const o of p.options || []) assert.ok(STRINGS.en[o], o);
  }
  for (const k of PRESET_KEYS) assert.ok(STRINGS.en[`preset.${k}`], k);
});

test('locale resolution and interpolation', () => {
  assert.equal(resolveLocale('tr-TR'), 'tr');
  assert.equal(resolveLocale('zh-CN'), 'zh-Hans');
  assert.equal(resolveLocale('pt_BR'), 'pt');
  assert.equal(resolveLocale('xx'), 'en');
  const t = translator('tr');
  assert.equal(t('sel.many', { n: 3 }), '3 nokta seçili');
});
