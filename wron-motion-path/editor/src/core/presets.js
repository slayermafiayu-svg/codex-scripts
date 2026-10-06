// Built-in path presets. Mirrors core/src/presets.cpp: every preset is a set
// of editable points, built in display space for the frame aspect.
import { PI } from './vec.js';

export const PRESET_KEYS = ['straight', 'arc', 'circle', 'ellipse', 's-curve', 'zigzag', 'spiral'];

const arcHandle = (angle) => (4 / 3) * Math.tan(angle / 4);

export function makePreset(key, aspect) {
  if (!(aspect > 0) || !Number.isFinite(aspect)) aspect = 16 / 9;
  const A = aspect;
  const cx = 0.5 * A, cy = 0.5;
  const path = { closed: false, points: [] };
  const addPt = (p, inv, out, mode) => {
    path.points.push({
      id: `p${path.points.length + 1}`,
      p: { x: p.x / A, y: p.y },
      in: { x: inv.x / A, y: inv.y },
      out: { x: out.x / A, y: out.y },
      mode,
      extra: {},
    });
  };
  const Z = { x: 0, y: 0 };
  switch (key) {
    case 'arc': {
      const halfChord = 0.3 * A, half = PI / 3;
      const R = halfChord / Math.sin(half);
      const chordY = 0.68;
      const c = { x: cx, y: chordY + R * Math.cos(half) };
      const k = arcHandle(half) * R;
      [-half, 0, half].forEach((f, i) => {
        const p = { x: c.x + Math.sin(f) * R, y: c.y - Math.cos(f) * R };
        const tan = { x: Math.cos(f), y: Math.sin(f) };
        addPt(p, i === 0 ? Z : { x: -tan.x * k, y: -tan.y * k }, i === 2 ? Z : { x: tan.x * k, y: tan.y * k }, 'smooth');
      });
      break;
    }
    case 'circle':
    case 'ellipse': {
      const rx = key === 'circle' ? 0.3 : 0.36 * A, ry = 0.3;
      const k = arcHandle(PI / 2);
      addPt({ x: cx, y: cy - ry }, { x: -k * rx, y: 0 }, { x: k * rx, y: 0 }, 'smooth');
      addPt({ x: cx + rx, y: cy }, { x: 0, y: -k * ry }, { x: 0, y: k * ry }, 'smooth');
      addPt({ x: cx, y: cy + ry }, { x: k * rx, y: 0 }, { x: -k * rx, y: 0 }, 'smooth');
      addPt({ x: cx - rx, y: cy }, { x: 0, y: k * ry }, { x: 0, y: -k * ry }, 'smooth');
      path.closed = true;
      break;
    }
    case 's-curve':
      addPt({ x: 0.15 * A, y: 0.75 }, Z, { x: 0.22 * A, y: 0 }, 'smooth');
      addPt({ x: cx, y: cy }, { x: -0.07 * A, y: 0.17 }, { x: 0.07 * A, y: -0.17 }, 'smooth');
      addPt({ x: 0.85 * A, y: 0.25 }, { x: -0.22 * A, y: 0 }, Z, 'smooth');
      break;
    case 'zigzag':
      for (let i = 0; i < 6; i += 1) addPt({ x: (0.15 + 0.14 * i) * A, y: i % 2 === 0 ? 0.65 : 0.35 }, Z, Z, 'corner');
      break;
    case 'spiral': {
      const r0 = 0.03, r1 = 0.4, turns = 2.5;
      const thetaEnd = turns * 2 * PI;
      const b = (r1 - r0) / thetaEnd;
      const steps = Math.trunc(turns * 4);
      const dTheta = thetaEnd / steps;
      const k = arcHandle(dTheta);
      for (let i = 0; i <= steps; i += 1) {
        const th = dTheta * i;
        const r = r0 + b * th;
        const dir = { x: Math.cos(th), y: Math.sin(th) };
        const p = { x: cx + dir.x * r, y: cy + dir.y * r };
        const deriv = { x: dir.x * b - dir.y * r, y: dir.y * b + dir.x * r };
        const h = { x: deriv.x * k, y: deriv.y * k };
        addPt(p, i === 0 ? Z : { x: -h.x, y: -h.y }, i === steps ? Z : h, 'smooth');
      }
      break;
    }
    default:
      addPt({ x: 0.2 * A, y: cy }, Z, Z, 'corner');
      addPt({ x: 0.8 * A, y: cy }, Z, Z, 'corner');
  }
  return path;
}
