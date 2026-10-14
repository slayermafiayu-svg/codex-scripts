"""Joint sets: HY-Motion's SMPL-H body, the 2D character rig, pose-detector formats.

Coordinates
-----------
* Motion (HY-Motion ``keypoints3d``): metres, Y up, ground at y = 0. In the
  rest pose the body faces +Z and its left side is +X (SMPL convention,
  checked against HY-Motion's j_template.bin).
* Rig (character image): stage pixels, x right, y DOWN (image convention).
* Pins (Puppet Pin): normalised frame coordinates, (0, 0) bottom-left.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import numpy as np

# First 22 joints of HY-Motion's body model (hymotion/.../dump_wooden/joint_names.json).
SMPL22_NAMES = [
    "Pelvis", "L_Hip", "R_Hip", "Spine1", "L_Knee", "R_Knee", "Spine2", "L_Ankle", "R_Ankle",
    "Spine3", "L_Foot", "R_Foot", "Neck", "L_Collar", "R_Collar", "Head", "L_Shoulder",
    "R_Shoulder", "L_Elbow", "R_Elbow", "L_Wrist", "R_Wrist",
]
SMPL22_PARENTS = [-1, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 9, 9, 12, 13, 14, 16, 17, 18, 19]
SMPL = {n: i for i, n in enumerate(SMPL22_NAMES)}

# 2D rig joints, in forward-kinematics order (parents first).
RIG_PARENT: dict[str, Optional[str]] = {
    "pelvis": None,
    "chest": "pelvis",
    "neck": "chest",
    "head": "neck",
    "l_shoulder": "neck",
    "l_elbow": "l_shoulder",
    "l_wrist": "l_elbow",
    "r_shoulder": "neck",
    "r_elbow": "r_shoulder",
    "r_wrist": "r_elbow",
    "l_hip": "pelvis",
    "l_knee": "l_hip",
    "l_ankle": "l_knee",
    "l_toe": "l_ankle",
    "r_hip": "pelvis",
    "r_knee": "r_hip",
    "r_ankle": "r_knee",
    "r_toe": "r_ankle",
}
RIG_JOINTS = list(RIG_PARENT)

RIG_LABELS_TR = {
    "pelvis": "Kalça merkezi", "chest": "Göğüs", "neck": "Boyun", "head": "Baş",
    "l_shoulder": "Sol omuz", "l_elbow": "Sol dirsek", "l_wrist": "Sol bilek",
    "r_shoulder": "Sağ omuz", "r_elbow": "Sağ dirsek", "r_wrist": "Sağ bilek",
    "l_hip": "Sol kalça", "l_knee": "Sol diz", "l_ankle": "Sol ayak bileği", "l_toe": "Sol ayak ucu",
    "r_hip": "Sağ kalça", "r_knee": "Sağ diz", "r_ankle": "Sağ ayak bileği", "r_toe": "Sağ ayak ucu",
}

# Which point of the SMPL body each rig joint follows. A tuple means the
# midpoint (detectors put "neck" between the shoulders, SMPL's Neck joint sits
# higher, at the base of the skull).
RIG_TO_SMPL: dict[str, tuple[int, ...]] = {
    "pelvis": (SMPL["Pelvis"],),
    "chest": (SMPL["Spine3"],),
    "neck": (SMPL["L_Shoulder"], SMPL["R_Shoulder"]),
    "head": (SMPL["Head"],),
    "l_shoulder": (SMPL["L_Shoulder"],),
    "l_elbow": (SMPL["L_Elbow"],),
    "l_wrist": (SMPL["L_Wrist"],),
    "r_shoulder": (SMPL["R_Shoulder"],),
    "r_elbow": (SMPL["R_Elbow"],),
    "r_wrist": (SMPL["R_Wrist"],),
    "l_hip": (SMPL["L_Hip"],),
    "l_knee": (SMPL["L_Knee"],),
    "l_ankle": (SMPL["L_Ankle"],),
    "l_toe": (SMPL["L_Foot"],),
    "r_hip": (SMPL["R_Hip"],),
    "r_knee": (SMPL["R_Knee"],),
    "r_ankle": (SMPL["R_Ankle"],),
    "r_toe": (SMPL["R_Foot"],),
}

# Limb bones follow the generated motion's absolute direction (shoulder, elbow,
# hip, knee mean the same thing for a pose detector and for SMPL). Torso, head
# and feet follow it relative to the first frame, so small differences between
# detector keypoints and SMPL joints never tilt the head or the body.
ABSOLUTE_BONES = {"l_elbow", "l_wrist", "r_elbow", "r_wrist", "l_knee", "l_ankle", "r_knee", "r_ankle"}


def rig_points_from_motion(k3d: np.ndarray) -> dict[str, np.ndarray]:
    """(T, >=22, 3) SMPL joints -> {rig joint: (T, 3)}."""
    if k3d.ndim != 3 or k3d.shape[1] < 22 or k3d.shape[2] != 3:
        raise ValueError(f"keypoints3d şekli (T, 22+, 3) olmalı, {k3d.shape} geldi")
    return {name: k3d[:, list(idx), :].mean(axis=1) for name, idx in RIG_TO_SMPL.items()}


@dataclass
class Rig:
    """Character joints on the stage image (pixels, y down)."""

    width: int
    height: int
    joints: dict[str, tuple[float, float]] = field(default_factory=dict)
    confidence: dict[str, float] = field(default_factory=dict)
    source: str = "manual"  # manual | dwpose | openpose-json | coco-wholebody

    def has(self, name: str) -> bool:
        return name in self.joints

    def to_dict(self) -> dict:
        return {
            "width": self.width, "height": self.height, "source": self.source,
            "joints": {k: [float(v[0]), float(v[1])] for k, v in self.joints.items()},
            "confidence": {k: float(v) for k, v in self.confidence.items()},
        }

    @classmethod
    def from_dict(cls, d: dict) -> "Rig":
        joints = {k: (float(v[0]), float(v[1])) for k, v in d.get("joints", {}).items() if k in RIG_PARENT}
        return cls(int(d["width"]), int(d["height"]), joints, dict(d.get("confidence", {})), d.get("source", "manual"))

    def complete(self) -> "Rig":
        """Fills derivable joints (chest, neck, pelvis) when a detector does not give them."""
        j = dict(self.joints)

        def mid(a, b):
            return ((j[a][0] + j[b][0]) / 2, (j[a][1] + j[b][1]) / 2)

        if "neck" not in j and "l_shoulder" in j and "r_shoulder" in j:
            j["neck"] = mid("l_shoulder", "r_shoulder")
        if "pelvis" not in j and "l_hip" in j and "r_hip" in j:
            j["pelvis"] = mid("l_hip", "r_hip")
        if "chest" not in j and "neck" in j and "pelvis" in j:
            n, p = j["neck"], j["pelvis"]
            j["chest"] = (p[0] + (n[0] - p[0]) * 0.62, p[1] + (n[1] - p[1]) * 0.62)
        return Rig(self.width, self.height, j, dict(self.confidence), self.source)

    def missing_required(self) -> list[str]:
        required = ["pelvis", "neck", "head", "l_shoulder", "r_shoulder", "l_hip", "r_hip",
                    "l_knee", "r_knee", "l_ankle", "r_ankle"]
        return [n for n in required if n not in self.joints]


# OpenPose COCO-18 (DWPose's ControlNet-style JSON, pose_keypoints_2d).
OPENPOSE18 = {
    0: "head", 1: "neck", 2: "r_shoulder", 3: "r_elbow", 4: "r_wrist", 5: "l_shoulder", 6: "l_elbow",
    7: "l_wrist", 8: "r_hip", 9: "r_knee", 10: "r_ankle", 11: "l_hip", 12: "l_knee", 13: "l_ankle",
}
# COCO-WholeBody 133 (DWPose / RTMW raw output): body 0..16, feet 17..22.
COCO_WHOLEBODY = {
    0: "head", 5: "l_shoulder", 6: "r_shoulder", 7: "l_elbow", 8: "r_elbow", 9: "l_wrist", 10: "r_wrist",
    11: "l_hip", 12: "r_hip", 13: "l_knee", 14: "r_knee", 15: "l_ankle", 16: "r_ankle",
    17: "l_toe", 20: "r_toe",  # big toes
}


def rig_from_keypoints(points: np.ndarray, mapping: dict[int, str], width: int, height: int,
                       source: str, min_conf: float = 0.3) -> Rig:
    pts = np.asarray(points, dtype=float)
    if pts.ndim == 1:
        if pts.size % 3:
            raise ValueError("anahtar noktalar x, y, güven üçlüleri olmalı")
        pts = pts.reshape(-1, 3)
    if len(pts) and np.nanmax(pts[:, :2]) <= 1.0:  # normalised coordinates
        pts = pts.copy()
        pts[:, 0] *= width
        pts[:, 1] *= height
    joints, conf = {}, {}
    for i, name in mapping.items():
        if i < len(pts):
            x, y, c = pts[i]
            if c >= min_conf and np.isfinite(x) and np.isfinite(y) and (x > 0 or y > 0):
                joints[name] = (float(x), float(y))
                conf[name] = float(c)
    return Rig(width, height, joints, conf, source).complete()


def load_rig_file(path: Path, width: int, height: int) -> Rig:
    """Reads our rig.json, an OpenPose/DWPose JSON (first person) or a 133-point list."""
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    if isinstance(data, dict) and "joints" in data:
        return Rig.from_dict(data).complete()
    if isinstance(data, list) and data and isinstance(data[0], dict) and "people" in data[0]:
        data = data[0]  # ComfyUI POSE_KEYPOINT is a list of frames
    if isinstance(data, dict) and "people" in data:
        if not data["people"]:
            raise ValueError("JSON içinde kişi yok")
        w = int(data.get("canvas_width") or width)
        h = int(data.get("canvas_height") or height)
        rig = rig_from_keypoints(data["people"][0]["pose_keypoints_2d"], OPENPOSE18, w, h, "openpose-json")
        return scale_rig(rig, width, height)
    arr = np.asarray(data, dtype=float)
    if arr.ndim == 2 and arr.shape[0] >= 23 and arr.shape[1] in (2, 3):
        if arr.shape[1] == 2:
            arr = np.concatenate([arr, np.ones((len(arr), 1))], axis=1)
        return rig_from_keypoints(arr, COCO_WHOLEBODY, width, height, "coco-wholebody")
    raise ValueError("Tanınmayan eklem dosyası biçimi")


def scale_rig(rig: Rig, width: int, height: int) -> Rig:
    if rig.width == width and rig.height == height:
        return rig
    sx, sy = width / rig.width, height / rig.height
    return Rig(width, height, {k: (x * sx, y * sy) for k, (x, y) in rig.joints.items()},
               dict(rig.confidence), rig.source)
