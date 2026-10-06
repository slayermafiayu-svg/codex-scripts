// Versioned path document. Mirrors core/src/document.cpp (same validation,
// same defaults, unknown members preserved). Stored by the OFX plug-in in the
// "wmpPathData" string parameter, one document per effect instance.

export const FORMAT = 'wron.motionpath';
export const VERSION = 1;
export const MAX_POINTS = 4096;
export const MAX_COORD = 1e4;
export const MAX_BYTES = 8 * 1024 * 1024;

export const Status = Object.freeze({
  Ok: 'ok',
  Migrated: 'migrated',
  Empty: 'empty',
  Invalid: 'invalid',
  Future: 'unsupported-future-version',
});

export const MODES = ['corner', 'smooth', 'free'];

// v1 is the first released format: no migration steps yet.
export const defaultMigrations = Object.freeze({ currentVersion: VERSION, steps: [] });

const isObj = (v) => v !== null && typeof v === 'object' && !Array.isArray(v);

function readVec(v) {
  if (!Array.isArray(v) || v.length !== 2) return null;
  const [x, y] = v;
  if (typeof x !== 'number' || typeof y !== 'number') return null;
  if (!Number.isFinite(x) || !Number.isFinite(y) || Math.abs(x) > MAX_COORD || Math.abs(y) > MAX_COORD) return null;
  return { x, y };
}

function parsePath(pv, warnings) {
  if (!isObj(pv)) throw new Error('"path" must be an object');
  const path = { closed: false, points: [] };
  if ('closed' in pv) {
    if (typeof pv.closed !== 'boolean') throw new Error('"path.closed" must be a boolean');
    path.closed = pv.closed;
  }
  if (!('points' in pv)) return path;
  if (!Array.isArray(pv.points)) throw new Error('"path.points" must be an array');
  if (pv.points.length > MAX_POINTS) throw new Error('too many path points');
  pv.points.forEach((item, index) => {
    const where = `point ${index}`;
    if (!isObj(item)) throw new Error(`${where} is not an object`);
    const pt = { id: '', p: { x: 0, y: 0 }, in: { x: 0, y: 0 }, out: { x: 0, y: 0 }, mode: 'corner', extra: {} };
    let hasMode = false;
    for (const [key, val] of Object.entries(item)) {
      if (key === 'p' || key === 'in' || key === 'out') {
        const v = readVec(val);
        if (!v) throw new Error(`${where}: invalid "${key}"`);
        pt[key] = v;
      } else if (key === 'mode') {
        if (typeof val !== 'string') throw new Error(`${where}: "mode" must be a string`);
        if (MODES.includes(val)) pt.mode = val;
        else {
          pt.mode = 'free';
          warnings.push(`${where}: unknown mode "${val}", using free`);
        }
        hasMode = true;
      } else if (key === 'id') {
        if (typeof val === 'string') pt.id = val;
      } else {
        pt.extra[key] = val;
      }
    }
    if (!('p' in item)) throw new Error(`${where}: missing "p"`);
    if (!hasMode) {
      const zero = pt.in.x === 0 && pt.in.y === 0 && pt.out.x === 0 && pt.out.y === 0;
      pt.mode = zero ? 'corner' : 'free';
    }
    path.points.push(pt);
  });
  return path;
}

export function loadDocument(text, table = defaultMigrations) {
  const r = { status: Status.Invalid, sourceVersion: 0, message: '', warnings: [], doc: null };
  if (typeof text !== 'string' || /^[ \t\r\n]*$/.test(text)) {
    r.status = Status.Empty;
    return r;
  }
  if (text.length > MAX_BYTES) {
    r.message = 'input exceeds size limit';
    return r;
  }
  let root;
  try {
    root = JSON.parse(text);
  } catch (e) {
    r.message = `JSON error: ${e.message}`;
    return r;
  }
  if (!isObj(root)) {
    r.message = 'document is not a JSON object';
    return r;
  }
  if (root.format !== FORMAT) {
    r.message = 'not a Wron motion path document';
    return r;
  }
  const ver = root.version;
  if (typeof ver !== 'number' || !Number.isInteger(ver) || ver < 0 || ver > 1e6) {
    r.message = 'missing or invalid "version"';
    return r;
  }
  r.sourceVersion = ver;
  if (ver > table.currentVersion) {
    r.status = Status.Future;
    r.message = `document version ${ver} is newer than supported version ${table.currentVersion}`;
    return r;
  }
  let version = ver;
  while (version < table.currentVersion) {
    const step = table.steps.find((s) => s.from === version);
    if (!step) {
      r.message = `no migration from version ${version}`;
      return r;
    }
    try {
      step.apply(root);
    } catch (e) {
      r.message = `migration from version ${version} failed: ${e.message}`;
      return r;
    }
    version += 1;
    root.version = version;
  }
  if (!('path' in root)) {
    r.message = 'missing "path"';
    return r;
  }
  try {
    const path = parsePath(root.path, r.warnings);
    r.doc = { path, root };
  } catch (e) {
    r.message = e.message;
    return r;
  }
  r.status = r.sourceVersion === table.currentVersion ? Status.Ok : Status.Migrated;
  if (r.status === Status.Migrated) r.message = `migrated from version ${r.sourceVersion}`;
  return r;
}

export const usable = (r) => r.status === Status.Ok || r.status === Status.Migrated;

function writePath(path) {
  return {
    closed: !!path.closed,
    points: path.points.map((pt) => {
      const o = {};
      if (pt.id) o.id = pt.id;
      o.p = [pt.p.x, pt.p.y];
      o.in = [pt.in.x, pt.in.y];
      o.out = [pt.out.x, pt.out.y];
      o.mode = pt.mode;
      for (const [k, v] of Object.entries(pt.extra || {})) if (!(k in o)) o[k] = v;
      return o;
    }),
  };
}

export function saveDocument(doc) {
  const root = isObj(doc.root) ? { ...doc.root } : {};
  root.format = FORMAT;
  root.version = VERSION;
  root.path = writePath(doc.path);
  return JSON.stringify(root, (k, v) => (typeof v === 'number' && !Number.isFinite(v) ? null : v));
}

export function makeDocument(path) {
  return { path, root: { format: FORMAT, version: VERSION } };
}

export function clonePath(path) {
  return {
    closed: !!path.closed,
    points: path.points.map((pt) => ({
      id: pt.id,
      p: { ...pt.p },
      in: { ...pt.in },
      out: { ...pt.out },
      mode: pt.mode,
      extra: structuredClone(pt.extra || {}),
    })),
  };
}

export const segmentCount = (path) => {
  const n = path.points.length;
  if (n === 0) return 0;
  return path.closed ? n : n - 1;
};

// Mirrors wmp::reversed: swaps handles; closed loops keep their start point.
export function reversed(path) {
  const flip = (q) => ({ ...q, in: { ...q.out }, out: { ...q.in }, p: { ...q.p }, extra: structuredClone(q.extra || {}) });
  const n = path.points.length;
  if (n === 0) return { closed: path.closed, points: [] };
  if (path.closed) {
    const pts = [flip(path.points[0])];
    for (let i = n - 1; i >= 1; i -= 1) pts.push(flip(path.points[i]));
    return { closed: true, points: pts };
  }
  return { closed: false, points: path.points.slice().reverse().map(flip) };
}
