// Host adapter contract between the path editor and its host.
//
// In production the host is the Wron Main Panel (WebView inside VEGAS Pro),
// which must implement this contract over its existing bridge to the VEGAS
// extension (that bridge's sources are not in this repository). Until then,
// MemoryHostAdapter is a complete in-memory implementation used by the demo
// harness and the tests. It is NOT connected to VEGAS.
//
// All methods are async. Times are seconds relative to the event start.
// Keyframe objects: { t, v, interp, slopes? } with interp one of
// "linear" | "fast" | "slow" | "smooth" | "sharp" | "hold" | "manual" | "split"
// (VEGAS curve types) and v a number or [x, y].
//
// capabilities (booleans):
//   params        readParams / writeParam work
//   keyframes     readKeyframes / writeKeyframes work (needed for timing presets)
//   presets       listPresets / savePreset / loadPreset / deletePreset work
//   frameImage    getFrameImage returns a still of the event (editor background)
//
// Required methods: getContext, readPathData, writePathData, subscribe.

export const REQUIRED_METHODS = ['getContext', 'readPathData', 'writePathData', 'subscribe'];

export function validateAdapter(adapter) {
  const missing = REQUIRED_METHODS.filter((m) => typeof adapter?.[m] !== 'function');
  if (missing.length) throw new Error(`host adapter is missing: ${missing.join(', ')}`);
  return { params: false, keyframes: false, presets: false, frameImage: false, ...(adapter.capabilities || {}) };
}

export class MemoryHostAdapter {
  constructor({
    width = 1920,
    height = 1080,
    par = 1,
    fps = { num: 30000, den: 1001 },
    eventDuration = 5,
    pathData = '',
    params = {},
    keyframes = {},
    frameImage = null,
  } = {}) {
    this.context = { frame: { width, height, par }, fps, eventDuration, time: 0 };
    this.pathData = pathData;
    this.params = { ...params };
    this.keyframes = structuredClone(keyframes);
    this.presets = new Map();
    this.frameImage = frameImage;
    this.listeners = new Set();
    this.log = []; // every host write, for tests: { kind, id?, label }
    this.capabilities = { params: true, keyframes: true, presets: true, frameImage: !!frameImage };
  }
  async getContext() { return structuredClone(this.context); }
  async readPathData() { return this.pathData; }
  async writePathData(json, label) {
    this.pathData = json;
    this.log.push({ kind: 'path', label });
  }
  async readParams(ids) {
    const o = {};
    for (const id of ids) if (id in this.params) o[id] = structuredClone(this.params[id]);
    return o;
  }
  async writeParam(id, value, label) {
    this.params[id] = structuredClone(value);
    this.log.push({ kind: 'param', id, label });
  }
  async readKeyframes(ids) {
    const o = {};
    for (const id of ids) if (this.keyframes[id]?.length) o[id] = structuredClone(this.keyframes[id]);
    return o;
  }
  async writeKeyframes(id, keys, label) {
    this.keyframes[id] = structuredClone(keys);
    this.log.push({ kind: 'keys', id, label });
  }
  async listPresets() { return [...this.presets.keys()].sort(); }
  async savePreset(name, preset) {
    this.presets.set(name, JSON.stringify(preset));
    this.log.push({ kind: 'preset-save', id: name });
  }
  async loadPreset(name) {
    const s = this.presets.get(name);
    return s ? JSON.parse(s) : null;
  }
  async deletePreset(name) { this.presets.delete(name); }
  async getFrameImage() { return this.frameImage; }
  subscribe(listener) {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }
  // Test/demo helper: simulate a change made by the host (e.g. VEGAS undo).
  emitExternalPathChange(json) {
    this.pathData = json;
    for (const l of this.listeners) l({ type: 'pathData' });
  }
}
