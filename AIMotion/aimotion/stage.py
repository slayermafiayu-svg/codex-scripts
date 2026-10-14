"""The stage: the character placed on a transparent canvas the size of the VEGAS project.

Puppet Pin's pin coordinates are normalised to the project frame. Placing the
character on a canvas of exactly the project size (and importing that PNG as
the event) makes the pin coordinates computed here land on the same pixels in
VEGAS, whatever the size of the original picture. It also leaves room for the
character to walk across the frame.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import numpy as np
from PIL import Image

from .skeleton import Rig


@dataclass
class Placement:
    scale: float  # stage pixels per source pixel
    offset_x: float  # stage position of the source's top-left corner
    offset_y: float

    def to_stage(self, x: float, y: float) -> tuple[float, float]:
        return x * self.scale + self.offset_x, y * self.scale + self.offset_y


@dataclass
class Stage:
    image: Image.Image  # RGBA, stage size
    placement: Placement
    has_alpha: bool
    warnings: list[str]


def load_character(path: Path, mask_path: Optional[Path] = None) -> tuple[Image.Image, bool, list[str]]:
    """Loads the character as RGBA. Alpha comes from the file or from a mask (white = character)."""
    warnings: list[str] = []
    im = Image.open(path)
    im.load()
    rgba = im.convert("RGBA")
    if mask_path:
        m = Image.open(mask_path).convert("L").resize(rgba.size, Image.BILINEAR)
        rgba.putalpha(m)
        return rgba, True, warnings
    alpha = np.asarray(rgba.getchannel("A"))
    has_alpha = bool(alpha.min() < 250)
    if not has_alpha:
        warnings.append(
            "Resimde saydamlık yok: tüm kare ağ (mesh) ile kaplanır ve arka plan da bükülür. "
            "En iyi sonuç için karakteri dekupe edin (mevcut SAM aracınızla) veya maske verin."
        )
    return rgba, has_alpha, warnings


def auto_placement(src_size: tuple[int, int], stage_size: tuple[int, int], alpha: Optional[np.ndarray],
                   height_fraction: float = 0.72, foot_line: float = 0.92, center_x: float = 0.5) -> Placement:
    """Scales the visible character to height_fraction of the stage, feet on foot_line."""
    sw, sh = src_size
    W, H = stage_size
    if alpha is not None and alpha.max() > 0:
        ys, xs = np.nonzero(alpha > 12)
        x0, x1, y0, y1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
    else:
        x0, x1, y0, y1 = 0, sw, 0, sh
    s = height_fraction * H / max(1, (y1 - y0))
    s = min(s, 0.95 * W / max(1, (x1 - x0)))
    ox = center_x * W - (x0 + x1) / 2 * s
    oy = foot_line * H - y1 * s
    return Placement(s, float(round(ox)), float(round(oy)))  # whole pixels, as pasted


def build_stage(character: Image.Image, stage_size: tuple[int, int], placement: Placement) -> Image.Image:
    W, H = stage_size
    stage = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    w = max(1, round(character.width * placement.scale))
    h = max(1, round(character.height * placement.scale))
    scaled = character.resize((w, h), Image.LANCZOS)
    # Plain paste copies RGBA as is (the canvas is empty) and clips negative offsets.
    stage.paste(scaled, (round(placement.offset_x), round(placement.offset_y)))
    return stage


def make_stage(character_path: Path, stage_size: tuple[int, int], mask_path: Optional[Path] = None,
               placement: Optional[Placement] = None, center_x: float = 0.5) -> Stage:
    rgba, has_alpha, warnings = load_character(character_path, mask_path)
    alpha = np.asarray(rgba.getchannel("A")) if has_alpha else None
    placement = placement or auto_placement(rgba.size, stage_size, alpha, center_x=center_x)
    return Stage(build_stage(rgba, stage_size, placement), placement, has_alpha, warnings)


def rig_to_stage(rig_src: Rig, placement: Placement, stage_size: tuple[int, int]) -> Rig:
    W, H = stage_size
    joints = {k: placement.to_stage(x, y) for k, (x, y) in rig_src.joints.items()}
    return Rig(W, H, joints, dict(rig_src.confidence), rig_src.source)


def make_layer_stages(layer_paths: list[Path], character_size: tuple[int, int], stage_size: tuple[int, int],
                      placement: Placement) -> list[Image.Image]:
    """Optional body-part layers (e.g. from SAM: far arm, far leg, body, near leg, near arm),
    each the size of the character picture, placed exactly like the character.

    One picture warps as one sheet, so a limb drawn over the body drags the body
    with it. With layers, every part gets its own Puppet Pin mesh (its own VEGAS
    track) and only the pins on that part bend it.
    """
    out = []
    for p in layer_paths:
        im = Image.open(p).convert("RGBA")
        if im.size != tuple(character_size):
            raise ValueError(f"{Path(p).name}: katman {im.size}, karakter {tuple(character_size)} boyutunda olmalı")
        out.append(build_stage(im, stage_size, placement))
    return out

