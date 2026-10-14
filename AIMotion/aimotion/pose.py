"""Optional built-in joint detection (DWPose-family whole-body model via rtmlib).

Your existing DWPose/SAM tool stays the main source of joints: export its
OpenPose-style JSON and import it in the UI (``skeleton.load_rig_file`` reads
OpenPose-18, ComfyUI POSE_KEYPOINT lists and raw 133-point COCO-WholeBody).
This module only adds a one-click detector when ``pip install rtmlib
onnxruntime-gpu`` is present. It was written against rtmlib's documented
``Wholebody`` API but could not be run in the development environment (model
downloads were blocked there), so treat it as untested until it works on
your PC; manual dragging of joints in the UI always works.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
from PIL import Image

from .skeleton import COCO_WHOLEBODY, Rig, rig_from_keypoints


def detector_available() -> bool:
    try:
        import rtmlib  # noqa: F401

        return True
    except ImportError:
        return False


def detect(image_path: Path, device: str = "cuda") -> Rig:
    from rtmlib import Wholebody

    rgba = Image.open(image_path).convert("RGBA")
    # Flatten transparent pixels onto grey: detectors are trained on photos, not alpha.
    bg = Image.new("RGBA", rgba.size, (128, 128, 128, 255))
    bg.alpha_composite(rgba)
    bgr = np.asarray(bg.convert("RGB"))[:, :, ::-1].copy()
    try:
        model = Wholebody(mode="performance", backend="onnxruntime", device=device)
    except Exception:  # noqa: BLE001 - no CUDA execution provider
        model = Wholebody(mode="performance", backend="onnxruntime", device="cpu")
    keypoints, scores = model(bgr)
    if len(keypoints) == 0:
        raise RuntimeError("resimde kişi/karakter bulunamadı")
    best = int(np.argmax(np.asarray(scores).mean(axis=1)))
    pts = np.concatenate([np.asarray(keypoints[best], float), np.asarray(scores[best], float)[:, None]], axis=1)
    rig = rig_from_keypoints(pts, COCO_WHOLEBODY, rgba.width, rgba.height, "rtmlib-wholebody")
    return rig
