// Where the source pivot sits on the path for given settings (editor preview).
// Mirrors the position/orientation part of core/src/motion.cpp.
import { SpeedMode } from './geometry.js';
import { mapProgress, EndBehavior } from './timing.js';
import { radToDeg } from './vec.js';

export const OrientMode = Object.freeze({ TravelDirection: 0, PathDirection: 1 });

// settings: { progress, startOffset, reverse, endBehavior, easing, speedMode,
//             orient, orientMode, orientSmoothing (fraction), orientOffset, rotation }
export function previewPose(geometry, settings) {
  if (!geometry || geometry.empty) return null;
  const st = mapProgress(settings.progress ?? 0, geometry.closed, settings);
  const extend = !geometry.closed && settings.endBehavior === EndBehavior.Extend;
  const d = geometry.distanceForU(st.u, settings.speedMode ?? SpeedMode.ConstantSpeed);
  const s = geometry.sampleAtDistance(d, st.direction, extend);
  let angle = 0;
  let angleValid = false;
  let dir = s.tangent;
  let ok = s.tangentValid;
  if ((settings.orientSmoothing ?? 0) > 0) {
    const r = geometry.smoothedTangent(d, settings.orientSmoothing * geometry.length, st.direction);
    ok = r.ok;
    dir = r.dir;
  }
  if (ok) {
    if ((settings.orientMode ?? OrientMode.TravelDirection) === OrientMode.TravelDirection && st.direction < 0) {
      dir = { x: -dir.x, y: -dir.y };
    }
    angle = radToDeg(Math.atan2(dir.y, dir.x));
    angleValid = true;
  }
  return { positionN: geometry.toNormalized(s.pos), distance: d, state: st, angle, angleValid };
}
