"""Which pins drive which part layer.

Each part layer (far arm, body, near leg, ...) becomes its own Puppet Pin
mesh. A layer must only get the pins of its own body part: in a side view
the near arm is drawn over the torso, so "every pin on the layer's pixels"
would make the torso bend along with the elbow.

The role of a layer comes from its file name when it says so (Turkish or
English: govde/body/torso, kol/arm, bacak/leg + sol/left, sag/right,
uzak/far, yakin/near), otherwise from the drawing: the layer with the head
and the pelvis is the body, a layer with a wrist and no ankle is an arm, one
with an ankle or knee is a leg. Left/right comes from which joint lies on
the layer, or, when both do (side views), from depth: layers behind the
body are the far side.
"""

from __future__ import annotations

import re
from pathlib import Path
from typing import Optional

import numpy as np
from PIL import Image

PART_JOINTS = {
    "body": ["pelvis", "chest", "neck", "head", "l_shoulder", "r_shoulder", "l_hip", "r_hip"],
    "l_arm": ["l_shoulder", "l_elbow", "l_wrist"],
    "r_arm": ["r_shoulder", "r_elbow", "r_wrist"],
    "l_leg": ["l_hip", "l_knee", "l_ankle", "l_toe"],
    "r_leg": ["r_hip", "r_knee", "r_ankle", "r_toe"],
}


def _norm(name: str) -> str:
    tr = str.maketrans("çğıöşüÇĞİÖŞÜ", "cgiosuCGIOSU")
    return Path(name).stem.translate(tr).lower()


def role_from_name(name: str, far_side: str) -> Optional[str]:
    n = _norm(name)
    words = set(re.split(r"[^a-z]+", n))
    if words & {"govde", "body", "torso", "bas", "head", "gogus"}:
        return "body"
    part = "arm" if words & {"kol", "arm"} else "leg" if words & {"bacak", "leg"} else None
    if not part:
        return None
    if words & {"sol", "left", "l"}:
        side = "l"
    elif words & {"sag", "right", "r"}:
        side = "r"
    elif words & {"uzak", "far", "arka", "back"}:
        side = far_side
    elif words & {"yakin", "near", "on", "front"}:
        side = "r" if far_side == "l" else "l"
    else:
        return None
    return f"{side}_{part}"


def _on(alpha: np.ndarray, xy: tuple[float, float], r: int = 6, thr: int = 13) -> bool:
    h, w = alpha.shape
    x, y = int(round(xy[0])), int(round(xy[1]))
    x0, x1, y0, y1 = max(0, x - r), min(w, x + r + 1), max(0, y - r), min(h, y + r + 1)
    return x0 < x1 and y0 < y1 and bool((alpha[y0:y1, x0:x1] >= thr).any())


def role_from_drawing(alpha: np.ndarray, joints_px: dict[str, tuple[float, float]], behind_body: bool,
                      far_side: str) -> Optional[str]:
    on = {j: _on(alpha, p) for j, p in joints_px.items()}
    if on.get("head") and on.get("pelvis"):
        return "body"
    for part, keys in (("arm", ("wrist", "elbow")), ("leg", ("ankle", "knee", "toe"))):
        sides = [s for s in ("l", "r") if any(on.get(f"{s}_{k}") for k in keys)]
        if part == "arm" and any(on.get(f"{s}_{k}") for s in "lr" for k in ("ankle", "knee")):
            continue  # an "arm" layer reaching the knees is a leg (or a full body) layer
        if len(sides) == 1:
            return f"{sides[0]}_{part}"
        if len(sides) == 2:
            near = "r" if far_side == "l" else "l"
            return f"{far_side if behind_body else near}_{part}"
    return None


def assign(layer_files: list[Path], joints_px: dict[str, tuple[float, float]], pin_of_joint: dict[str, int],
           yaw_deg: float) -> list[dict]:
    """[{"image": name, "role": role|None, "pins": [...]}] back to front."""
    far_side = "l" if yaw_deg >= 0 else "r"  # facing right shows the right side; the left is behind
    alphas = [np.asarray(Image.open(f).convert("RGBA").getchannel("A")) for f in layer_files]
    roles = [role_from_name(Path(f).name, far_side) for f in layer_files]
    body_index = next((i for i, r in enumerate(roles) if r == "body"), None)
    if body_index is None:
        for i, a in enumerate(alphas):
            if _on(a, joints_px.get("head", (-99, -99))) and _on(a, joints_px.get("pelvis", (-99, -99))):
                body_index = i
                break
    out = []
    for i, f in enumerate(layer_files):
        role = roles[i] or role_from_drawing(alphas[i], joints_px, body_index is not None and i < body_index, far_side)
        if role:
            joints = PART_JOINTS[role]
        else:  # unknown part: every pin that lies on it
            joints = [j for j in pin_of_joint if _on(alphas[i], joints_px[j])]
        pins = sorted(pin_of_joint[j] for j in joints if j in pin_of_joint)
        out.append({"image": Path(f).name, "role": role, "pins": pins})
    return out
