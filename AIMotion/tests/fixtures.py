"""Synthetic test skeletons. These are TEST FIXTURES, not motion presets: they
exist only to check the geometry code and are never shown as AI output."""

import math

import numpy as np

from aimotion.skeleton import SMPL, Rig

# A rough humanoid in the SMPL joint layout (metres, Y up, facing +Z, left = +X), arms down.
REST = np.array([
    [0.00, 0.93, 0.00],   # Pelvis
    [0.09, 0.84, 0.00],   # L_Hip
    [-0.09, 0.84, 0.00],  # R_Hip
    [0.00, 1.03, 0.00],   # Spine1
    [0.10, 0.47, 0.01],   # L_Knee
    [-0.10, 0.47, 0.01],  # R_Knee
    [0.00, 1.15, 0.00],   # Spine2
    [0.10, 0.08, -0.03],  # L_Ankle
    [-0.10, 0.08, -0.03], # R_Ankle
    [0.00, 1.23, 0.00],   # Spine3
    [0.11, 0.02, 0.10],   # L_Foot
    [-0.11, 0.02, 0.10],  # R_Foot
    [0.00, 1.43, 0.00],   # Neck
    [0.06, 1.36, 0.00],   # L_Collar
    [-0.06, 1.36, 0.00],  # R_Collar
    [0.00, 1.55, 0.02],   # Head
    [0.17, 1.37, 0.00],   # L_Shoulder
    [-0.17, 1.37, 0.00],  # R_Shoulder
    [0.19, 1.10, 0.00],   # L_Elbow
    [-0.19, 1.10, 0.00],  # R_Elbow
    [0.20, 0.86, 0.02],   # L_Wrist
    [-0.20, 0.86, 0.02],  # R_Wrist
])


def rot_y(points, deg):
    a = math.radians(deg)
    c, s = math.cos(a), math.sin(a)
    out = points.copy()
    x, z = points[..., 0].copy(), points[..., 2].copy()
    out[..., 0] = x * c + z * s
    out[..., 2] = -x * s + z * c
    return out


def static_motion(frames=30, yaw_deg=0.0):
    return np.repeat(rot_y(REST, yaw_deg)[None], frames, axis=0)


def rig_from_pose(pose3d, yaw_deg, px_per_m=400.0, width=1920, height=1080, origin=(960, 1000),
                  with_toes=True):
    """Draws a 3D pose as the 'picture' would show it (orthographic, given yaw)."""
    th = math.radians(yaw_deg)
    u = pose3d[:, 0] * math.cos(th) + pose3d[:, 2] * math.sin(th)
    v = pose3d[:, 1]
    xs = origin[0] + u * px_per_m
    ys = origin[1] - v * px_per_m
    pick = {"head": "Head", "l_shoulder": "L_Shoulder", "r_shoulder": "R_Shoulder", "l_elbow": "L_Elbow",
            "r_elbow": "R_Elbow", "l_wrist": "L_Wrist", "r_wrist": "R_Wrist", "l_hip": "L_Hip",
            "r_hip": "R_Hip", "l_knee": "L_Knee", "r_knee": "R_Knee", "l_ankle": "L_Ankle",
            "r_ankle": "R_Ankle", "pelvis": "Pelvis", "chest": "Spine3"}
    if with_toes:
        pick.update({"l_toe": "L_Foot", "r_toe": "R_Foot"})
    joints = {k: (float(xs[SMPL[v]]), float(ys[SMPL[v]])) for k, v in pick.items()}
    joints["neck"] = ((joints["l_shoulder"][0] + joints["r_shoulder"][0]) / 2,
                      (joints["l_shoulder"][1] + joints["r_shoulder"][1]) / 2)
    return Rig(width, height, joints, source="test")


def raise_left_arm(frames=30):
    """Left arm swings from down to horizontal (in the body's frontal plane)."""
    out = static_motion(frames)
    sh = REST[SMPL["L_Shoulder"]]
    for t in range(frames):
        a = math.radians(80.0 * t / (frames - 1))  # rotation about +Z (frontal plane)
        c, s = math.cos(a), math.sin(a)
        for name in ("L_Elbow", "L_Wrist"):
            d = REST[SMPL[name]] - sh
            out[t, SMPL[name], 0] = sh[0] + d[0] * c - d[1] * s
            out[t, SMPL[name], 1] = sh[1] + d[0] * s + d[1] * c
    return out


def lean_forward_planted(frames=30, dz=0.15):
    """Pelvis and upper body move forward by dz while both feet stay planted."""
    out = static_motion(frames)
    lower = [SMPL[n] for n in ("L_Ankle", "R_Ankle", "L_Foot", "R_Foot")]
    knees = [SMPL["L_Knee"], SMPL["R_Knee"]]
    for t in range(frames):
        k = dz * t / (frames - 1)
        for j in range(22):
            if j in lower:
                continue
            out[t, j, 2] += k * (0.5 if j in knees else 1.0)
    return out


def _rot_x(v, a):
    """Rotate vectors about the X axis (sagittal swing; +a swings forward toward +Z)."""
    c, s = math.cos(a), math.sin(a)
    out = v.copy()
    out[..., 1] = v[..., 1] * c - v[..., 2] * s
    out[..., 2] = v[..., 1] * s + v[..., 2] * c
    return out


def synthetic_walk(frames=60, fps=30.0, speed=1.1, cadence_hz=0.9):
    """Crude kinematic walk (legs and arms swing, pelvis travels). TEST FIXTURE ONLY: used to
    exercise retargeting, layers and rendering; never presented as generated motion."""
    out = np.zeros((frames, 22, 3))
    chains = {"L": ["L_Hip", "L_Knee", "L_Ankle", "L_Foot"], "R": ["R_Hip", "R_Knee", "R_Ankle", "R_Foot"]}
    arms = {"L": ["L_Shoulder", "L_Elbow", "L_Wrist"], "R": ["R_Shoulder", "R_Elbow", "R_Wrist"]}
    for t in range(frames):
        ph = 2 * math.pi * cadence_hz * t / fps
        pose = REST.copy()
        for side, sign in (("L", 1.0), ("R", -1.0)):
            hip = 0.45 * sign * math.sin(ph)
            knee = max(0.0, -0.9 * sign * math.cos(ph))  # bend while swinging through
            c = [SMPL[n] for n in chains[side]]
            root = REST[c[0]]
            seg = _rot_x(REST[c[1:]] - root, hip)
            pose[c[1:]] = root + seg
            kpos = pose[c[1]]
            below = _rot_x(REST[c[2:]] - REST[c[1]], hip - knee)
            pose[c[2:]] = kpos + below
            a = [SMPL[n] for n in arms[side]]
            sh = REST[a[0]]
            pose[a[1:]] = sh + _rot_x(REST[a[1:]] - sh, -0.5 * sign * math.sin(ph))
        lift = min(pose[SMPL["L_Foot"]][1], pose[SMPL["R_Foot"]][1]) - REST[SMPL["L_Foot"]][1]
        pose[:, 1] -= lift
        pose[:, 2] += speed * t / fps
        out[t] = pose
    return out
