// OFX parameters edited from the path editor's inspector. IDs, types,
// defaults and ranges match the plug-in exactly (test/params.test.js checks
// them against testdata/ofx-params.json, dumped from the real .ofx).
//
// Not listed on purpose: wmpProgress (animated in the host timeline; the editor
// only previews it), wmpPathData (the document itself), labels, the OFX
// overlay tool and the OFX preset button (the editor has its own preset UI).

export const GROUPS = ['path', 'timing', 'transform', 'orientation', 'blur', 'presets'];

export const PARAMS = [
  // Timing
  { id: 'wmpStartOffset', type: 'double', group: 'timing', label: 'p.startOffset', def: 0, min: -1e6, max: 1e6, step: 0.01, digits: 4, animates: true },
  { id: 'wmpReverse', type: 'bool', group: 'timing', label: 'p.reverseTravel', def: false },
  { id: 'wmpEndBehavior', type: 'choice', group: 'timing', label: 'p.endBehavior', def: 0, options: ['o.clamp', 'o.extend', 'o.loop', 'o.pingPong'] },
  { id: 'wmpSpeedMode', type: 'choice', group: 'timing', label: 'p.speedMode', def: 0, options: ['o.constantSpeed', 'o.equalTime'] },
  { id: 'wmpEasing', type: 'choice', group: 'timing', label: 'p.easing', def: 0, options: ['o.linear', 'o.easeIn', 'o.easeOut', 'o.easeInOut', 'o.backOut', 'o.custom'] },
  { id: 'wmpEaseP1', type: 'double2', group: 'timing', label: 'p.easeP1', def: [0.42, 0], min: [0, -5], max: [1, 5], step: 0.01, digits: 3, enabledWhen: { wmpEasing: 5 } },
  { id: 'wmpEaseP2', type: 'double2', group: 'timing', label: 'p.easeP2', def: [0.58, 1], min: [0, -5], max: [1, 5], step: 0.01, digits: 3, enabledWhen: { wmpEasing: 5 } },
  // Transform
  { id: 'wmpPivot', type: 'double2', group: 'transform', label: 'p.pivot', def: [0.5, 0.5], min: [-1e4, -1e4], max: [1e4, 1e4], step: 0.01, digits: 4, animates: true },
  { id: 'wmpOffset', type: 'double2', group: 'transform', label: 'p.offset', def: [0, 0], min: [-1e4, -1e4], max: [1e4, 1e4], step: 0.01, digits: 4, animates: true },
  { id: 'wmpUniformScale', type: 'bool', group: 'transform', label: 'p.uniformScale', def: true },
  { id: 'wmpScale', type: 'double2', group: 'transform', label: 'p.scale', def: [100, 100], min: [-1e5, -1e5], max: [1e5, 1e5], step: 1, digits: 2, animates: true },
  { id: 'wmpRotation', type: 'double', group: 'transform', label: 'p.rotation', def: 0, min: -1e6, max: 1e6, step: 1, digits: 2, animates: true },
  { id: 'wmpOpacity', type: 'double', group: 'transform', label: 'p.opacity', def: 100, min: 0, max: 100, step: 1, digits: 1, animates: true },
  // Orientation
  { id: 'wmpOrient', type: 'bool', group: 'orientation', label: 'p.orient', def: false },
  { id: 'wmpOrientOffset', type: 'double', group: 'orientation', label: 'p.orientOffset', def: 0, min: -1e6, max: 1e6, step: 1, digits: 2, animates: true, enabledWhen: { wmpOrient: true } },
  { id: 'wmpOrientMode', type: 'choice', group: 'orientation', label: 'p.orientMode', def: 0, options: ['o.travel', 'o.pathDir'], enabledWhen: { wmpOrient: true } },
  { id: 'wmpOrientSmooth', type: 'double', group: 'orientation', label: 'p.orientSmooth', def: 0, min: 0, max: 50, step: 0.5, digits: 2, animates: true, enabledWhen: { wmpOrient: true } },
  // Motion blur
  { id: 'wmpBlur', type: 'bool', group: 'blur', label: 'p.blur', def: false },
  { id: 'wmpShutterAngle', type: 'double', group: 'blur', label: 'p.shutterAngle', def: 180, min: 0, max: 720, step: 1, digits: 1, animates: true, enabledWhen: { wmpBlur: true } },
  { id: 'wmpShutterPhase', type: 'double', group: 'blur', label: 'p.shutterPhase', def: -90, min: -360, max: 360, step: 1, digits: 1, animates: true, enabledWhen: { wmpBlur: true } },
  { id: 'wmpBlurAdaptive', type: 'bool', group: 'blur', label: 'p.blurAdaptive', def: true, enabledWhen: { wmpBlur: true } },
  { id: 'wmpBlurSamples', type: 'int', group: 'blur', label: 'p.blurSamples', def: 16, min: 1, max: 256, enabledWhen: { wmpBlur: true, wmpBlurAdaptive: false } },
  { id: 'wmpBlurMaxSamples', type: 'int', group: 'blur', label: 'p.blurMaxSamples', def: 64, min: 2, max: 256, enabledWhen: { wmpBlur: true, wmpBlurAdaptive: true } },
  { id: 'wmpBlurPreview', type: 'choice', group: 'blur', label: 'p.blurPreview', def: 1, options: ['o.full', 'o.reduced', 'o.off'], enabledWhen: { wmpBlur: true } },
];

// Preset content levels.
export const LOOK_PARAMS = PARAMS.filter((p) => ['transform', 'orientation', 'blur'].includes(p.group)).map((p) => p.id);
export const TIMING_PARAMS = ['wmpProgress', ...PARAMS.filter((p) => p.group === 'timing').map((p) => p.id)];

export const paramById = (id) => PARAMS.find((p) => p.id === id);

export function defaults() {
  const o = {};
  for (const p of PARAMS) o[p.id] = Array.isArray(p.def) ? [...p.def] : p.def;
  return o;
}

export function isEnabled(param, values) {
  if (!param.enabledWhen) return true;
  return Object.entries(param.enabledWhen).every(([k, v]) => values[k] === v);
}

// Clamp/validate a value for a parameter. Returns null if invalid (never
// coerces bad input to 0).
export function sanitize(param, value) {
  const clamp = (v, lo, hi) => Math.min(Math.max(v, lo), hi);
  switch (param.type) {
    case 'bool': return typeof value === 'boolean' ? value : null;
    case 'choice': return Number.isInteger(value) && value >= 0 && value < param.options.length ? value : null;
    case 'int': return Number.isFinite(value) ? clamp(Math.round(value), param.min, param.max) : null;
    case 'double': return Number.isFinite(value) ? clamp(value, param.min, param.max) : null;
    case 'double2':
      return Array.isArray(value) && value.length === 2 && value.every(Number.isFinite)
        ? [clamp(value[0], param.min[0], param.max[0]), clamp(value[1], param.min[1], param.max[1])]
        : null;
    default: return null;
  }
}

// Parses a user-typed number: accepts "," or "." as decimal separator.
export function parseNumber(text) {
  if (typeof text !== 'string') return null;
  const t = text.trim().replace(',', '.');
  if (!/^[-+]?(\d+\.?\d*|\.\d+)(e[-+]?\d+)?$/i.test(t)) return null;
  const v = Number(t);
  return Number.isFinite(v) ? v : null;
}
