"""Generated 3D motion -> 2D joint tracks on the character image.

The motion model produces a 3D skeleton (SMPL-H body, metres). The character
is a 2D picture. This module projects the motion with an orthographic camera
whose yaw matches the way the character faces in the picture, then rebuilds
the character's own skeleton bone by bone (forward kinematics) so its bone
lengths and proportions stay those of the drawing:

* limb bones (upper/lower arm, thigh, shin) take the motion's absolute
  direction, blended in from the drawn pose over the first ``blend_in_s``;
* torso, neck/head, shoulder/hip and foot bones turn by the motion's rotation
  relative to its first frame, so the drawn pose of those parts is kept;
* bone lengths follow the motion's foreshortening (a forearm pointing at the
  camera gets shorter);
* the root follows the motion's pelvis, scaled to the character, and planted
  feet are kept from sliding ("foot lock").

Limits (inherent to warping one picture): no new views appear (turning
around shows a mirrored-looking squash, not the back), limbs crossing in front
of the body cannot change their overlap order on a single layer, and walking
toward the camera stays in place (orthographic camera).
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import Optional

import numpy as np

from .skeleton import ABSOLUTE_BONES, RIG_JOINTS, RIG_PARENT, Rig, rig_points_from_motion

SMPL_FRONT_WIDTH_RATIO = 0.57  # (shoulder width + hip width) / 2 / torso length of the SMPL template


@dataclass
class RetargetOptions:
    view_yaw_deg: Optional[float] = None  # None = estimate from the rig
    root_motion: bool = True  # False: keep the character in place horizontally
    foot_lock: bool = True
    blend_in_s: float = 0.25
    motion_fps: float = 30.0  # HY-Motion output_mesh_fps
    out_fps: float = 30.0


@dataclass
class Retargeted:
    fps: float
    tracks: dict[str, np.ndarray]  # joint -> (N, 2) stage pixels, y down
    view_yaw_deg: float
    view_reason: str
    px_per_m: float
    contacts: dict[str, np.ndarray] = field(default_factory=dict)
    warnings: list[str] = field(default_factory=list)

    @property
    def num_frames(self) -> int:
        return len(next(iter(self.tracks.values())))


def _wrap(a: np.ndarray | float) -> np.ndarray | float:
    return (np.asarray(a) + np.pi) % (2 * np.pi) - np.pi


def _smoothstep(x: np.ndarray) -> np.ndarray:
    x = np.clip(x, 0.0, 1.0)
    return x * x * (3 - 2 * x)


def estimate_view_yaw(rig: Rig) -> tuple[float, str]:
    """Camera yaw from the drawn pose: 0 = facing the viewer, +90 = facing right,
    -90 = facing left, 180 = back to the viewer."""
    j = rig.complete().joints
    need = ("neck", "pelvis", "l_shoulder", "r_shoulder", "l_hip", "r_hip")
    if any(n not in j for n in need):
        return 0.0, "eksik eklemler: önden kabul edildi"
    torso = math.dist(j["neck"], j["pelvis"])
    if torso < 1e-6:
        return 0.0, "gövde boyu sıfır: önden kabul edildi"
    signed_width = ((j["l_shoulder"][0] - j["r_shoulder"][0]) + (j["l_hip"][0] - j["r_hip"][0])) / 2 / torso
    c = float(np.clip(signed_width / SMPL_FRONT_WIDTH_RATIO, -1.0, 1.0))
    facing = 0.0
    if "head" in j:
        facing += (j["head"][0] - j["neck"][0]) / torso
    for s in ("l", "r"):
        if f"{s}_toe" in j and f"{s}_ankle" in j:
            facing += (j[f"{s}_toe"][0] - j[f"{s}_ankle"][0]) / torso
    s = math.sqrt(max(0.0, 1.0 - c * c)) * (1.0 if facing >= 0 else -1.0)
    yaw = math.degrees(math.atan2(s, c))
    if abs(yaw) < 20:
        reason = "önden"
    elif abs(yaw) > 160:
        reason = "arkadan"
    elif 70 <= abs(yaw) <= 110:
        reason = "yandan, " + ("sağa bakıyor" if yaw > 0 else "sola bakıyor")
    else:
        reason = "3/4, " + ("sağa dönük" if yaw > 0 else "sola dönük")
    return yaw, reason


def normalize_heading(k3d: np.ndarray) -> np.ndarray:
    """Rotates about the vertical so frame 0 faces +Z, and puts the frame-0 pelvis at x = z = 0."""
    pts = rig_points_from_motion(k3d)
    left = pts["l_hip"][0] - pts["r_hip"][0]
    left[1] = 0.0
    n = np.linalg.norm(left)
    if n < 1e-9:
        return k3d.copy()
    left /= n
    forward = np.cross(left, np.array([0.0, 1.0, 0.0]))
    yaw0 = math.atan2(forward[0], forward[2])
    c, s = math.cos(-yaw0), math.sin(-yaw0)
    out = k3d.astype(np.float64).copy()
    x, z = out[..., 0].copy(), out[..., 2].copy()
    out[..., 0] = x * c + z * s
    out[..., 2] = -x * s + z * c
    root = pts["pelvis"][0]
    rx, rz = root[0] * c + root[2] * s, -root[0] * s + root[2] * c
    out[..., 0] -= rx
    out[..., 2] -= rz
    return out


def resample(k3d: np.ndarray, src_fps: float, dst_fps: float) -> np.ndarray:
    if abs(src_fps - dst_fps) < 1e-9:
        return k3d.copy()
    t_src = np.arange(len(k3d)) / src_fps
    duration = t_src[-1]
    n = int(math.floor(duration * dst_fps + 1e-6)) + 1
    t_dst = np.arange(n) / dst_fps
    flat = k3d.reshape(len(k3d), -1)
    out = np.empty((n, flat.shape[1]))
    for i in range(flat.shape[1]):
        out[:, i] = np.interp(t_dst, t_src, flat[:, i])
    return out.reshape((n,) + k3d.shape[1:])


def project(points: np.ndarray, yaw_deg: float) -> np.ndarray:
    """Orthographic projection -> (..., 2) metres, x right, y up."""
    th = math.radians(yaw_deg)
    u = points[..., 0] * math.cos(th) + points[..., 2] * math.sin(th)
    return np.stack([u, points[..., 1]], axis=-1)


def foot_contacts(p3: dict[str, np.ndarray], fps: float, height_tol: float = 0.05,
                  speed_tol: float = 0.5) -> dict[str, np.ndarray]:
    out = {}
    for s in ("l", "r"):
        foot = p3[f"{s}_toe"]
        h = foot[:, 1]
        v = np.zeros(len(foot))
        if len(foot) > 1:
            v[1:] = np.linalg.norm(np.diff(foot[:, [0, 2]], axis=0), axis=1) * fps
            v[0] = v[1]
        out[s] = (h < h.min() + height_tol) & (v < speed_tol)
    return out


def retarget(k3d: np.ndarray, rig: Rig, opts: RetargetOptions = RetargetOptions()) -> Retargeted:
    rig = rig.complete()
    missing = rig.missing_required()
    if missing:
        raise ValueError("Karakterde eksik eklemler: " + ", ".join(missing))
    warnings: list[str] = []

    k3d = normalize_heading(np.asarray(k3d, dtype=np.float64)[:, :22])
    k3d = resample(k3d, opts.motion_fps, opts.out_fps)
    n = len(k3d)
    p3 = rig_points_from_motion(k3d)

    if opts.view_yaw_deg is None:
        yaw, reason = estimate_view_yaw(rig)
    else:
        yaw, reason = float(opts.view_yaw_deg), "kullanıcı seçti"
    m2 = {k: project(v, yaw) for k, v in p3.items()}

    H = rig.height
    img = {k: np.array([x, H - y], dtype=np.float64) for k, (x, y) in rig.joints.items()}  # y up

    # Character scale (pixels per metre) from bones that lie in the picture plane at frame 0.
    ratios_scale = []
    for c in RIG_JOINTS:
        p = RIG_PARENT[c]
        if p is None or c not in img or p not in img:
            continue
        d3 = p3[c][0] - p3[p][0]
        d2 = m2[c][0] - m2[p][0]
        l3, l2 = np.linalg.norm(d3), np.linalg.norm(d2)
        if l3 > 1e-6 and l2 / l3 > 0.75 and c not in ("l_toe", "r_toe", "head"):
            ratios_scale.append(np.linalg.norm(img[c] - img[p]) / l2)
    if ratios_scale:
        k = float(np.median(ratios_scale))
    else:
        top = min(y for _, y in rig.joints.values())
        bottom = max(y for _, y in rig.joints.values())
        k = (bottom - top) / max(1e-6, p3["head"][0][1] - min(p3["l_ankle"][0][1], p3["r_ankle"][0][1]))
        warnings.append("Ölçek kaba tahmin edildi (resim düzleminde kemik bulunamadı).")

    blend_frames = max(1.0, opts.blend_in_s * opts.out_fps)
    w = _smoothstep(np.arange(n) / blend_frames) if opts.blend_in_s > 0 else np.ones(n)

    pos: dict[str, np.ndarray] = {}
    root_delta = (m2["pelvis"] - m2["pelvis"][0]) * k
    if not opts.root_motion:
        root_delta[:, 0] = 0.0
    pos["pelvis"] = img["pelvis"] + root_delta

    for c in RIG_JOINTS:
        p = RIG_PARENT[c]
        if p is None or c not in img or p not in pos:
            continue
        B = img[c] - img[p]
        lb = float(np.linalg.norm(B))
        img_ang = math.atan2(B[1], B[0])
        d3 = p3[c] - p3[p]
        d2 = m2[c] - m2[p]
        l3 = np.linalg.norm(d3, axis=1)
        l2 = np.linalg.norm(d2, axis=1)
        ratio = l2 / np.maximum(l3, 1e-9)
        ang_m = np.arctan2(d2[:, 1], d2[:, 0])
        if c in ABSOLUTE_BONES:
            l_true = lb / max(ratio[0], 0.3)
            l_true = float(np.clip(l_true, 0.6 * k * l3.mean(), 1.6 * k * l3.mean()))
            ang = img_ang + _wrap(ang_m - img_ang) * w
            length = lb + (l_true * ratio - lb) * w
        elif ratio[0] < 0.35:
            # Bone points at the camera in the first frame: its 2D angle is noise. Keep the drawing.
            ang = np.full(n, img_ang)
            length = np.full(n, lb)
        else:
            ang = img_ang + _wrap(ang_m - ang_m[0])
            length = lb * np.clip(ratio / ratio[0], 0.3, 1.5)
        pos[c] = pos[p] + np.stack([np.cos(ang), np.sin(ang)], axis=1) * length[:, None]

    contacts = foot_contacts(p3, opts.out_fps)
    if opts.root_motion and opts.foot_lock:
        corr = np.zeros((n, 2))
        for t in range(1, n):
            deltas = []
            for s in ("l", "r"):
                foot = f"{s}_toe" if f"{s}_toe" in pos else f"{s}_ankle"
                if contacts[s][t] and contacts[s][t - 1]:
                    deltas.append(pos[foot][t] - pos[foot][t - 1])
            corr[t] = corr[t - 1] - (np.mean(deltas, axis=0) if deltas else 0.0)
        for name in pos:
            pos[name] = pos[name] + corr

    tracks = {name: np.stack([v[:, 0], H - v[:, 1]], axis=1) for name, v in pos.items()}
    return Retargeted(opts.out_fps, tracks, yaw, reason, k, contacts, warnings)
