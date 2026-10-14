"""Joint tracks -> Puppet Pin keyframes.

Puppet Pin (PuppetPinOFX) has 20 pins. Each pin has a hidden anchor (where
it is stuck to the picture) and a keyframable position, both in normalised
frame coordinates with (0, 0) at the bottom-left. Parameter names, as VEGAS
scripts see them: ``pinNN_on``, ``pinNN_pos``, ``pinNN_anchor`` (NN = 01..20),
``mode`` (0 = Mesh, 1 = Animate), ``meshArea``, ``alphaThreshold``,
``meshDensity``, ``expansion``, ``rigidity``.

The pin export is plain JSON so the VEGAS import script, the preview renderer
and anybody's own tools can read it.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from .provenance import Provenance
from .retarget import Retargeted
from .skeleton import RIG_JOINTS, RIG_LABELS_TR

MAX_PINS = 20
FORMAT = "aimotion.pins/1"


@dataclass
class MeshSettings:
    use_alpha: bool = True
    alpha_threshold: float = 0.05
    density: int = 48
    expansion: float = 3.0
    rigidity: int = 20


@dataclass
class PinTrack:
    number: int  # 1..20 -> pinNN
    joint: str
    label: str
    anchor: tuple[float, float]  # normalised, y up
    track: np.ndarray  # (N, 2) normalised, y up


@dataclass
class PinAnimation:
    width: int
    height: int
    fps: float
    pins: list[PinTrack]
    mesh: MeshSettings
    provenance: Provenance
    stage_image: str = ""  # file name of the stage PNG, relative to the export folder
    # Optional part layers, back to front: [{"image": "stage_layer_00.png", "pins": [pin numbers]}].
    # Empty = the whole stage image is one sheet with all pins.
    layers: list[dict] = field(default_factory=list)
    view: dict = field(default_factory=dict)

    @property
    def num_frames(self) -> int:
        return len(self.pins[0].track) if self.pins else 0

    @property
    def duration_s(self) -> float:
        return self.num_frames / self.fps if self.fps else 0.0

    def to_json(self) -> dict:
        self.provenance.validate()
        return {
            "format": FORMAT,
            "width": self.width,
            "height": self.height,
            "fps": self.fps,
            "frames": self.num_frames,
            "duration_s": round(self.duration_s, 6),
            "stage_image": self.stage_image,
            "layers": self.layers,
            "view": self.view,
            "mesh": self.mesh.__dict__,
            "provenance": self.provenance.to_dict(),
            "pins": [
                {
                    "number": p.number,
                    "param": f"pin{p.number:02d}",
                    "joint": p.joint,
                    "label": p.label,
                    "anchor": [round(p.anchor[0], 6), round(p.anchor[1], 6)],
                    "track": np.round(p.track, 6).tolist(),
                }
                for p in self.pins
            ],
        }

    def save(self, path: Path) -> None:
        Path(path).write_text(json.dumps(self.to_json(), ensure_ascii=False), encoding="utf-8")

    @classmethod
    def load(cls, path: Path) -> "PinAnimation":
        d = json.loads(Path(path).read_text(encoding="utf-8"))
        if d.get("format") != FORMAT:
            raise ValueError(f"{path}: {FORMAT} biçiminde değil")
        pins = [
            PinTrack(p["number"], p["joint"], p.get("label", ""), tuple(p["anchor"]), np.asarray(p["track"], float))
            for p in d["pins"]
        ]
        return cls(d["width"], d["height"], d["fps"], pins, MeshSettings(**d["mesh"]),
                   Provenance.from_dict(d["provenance"]), d.get("stage_image", ""), d.get("layers", []),
                   d.get("view", {}))


def to_normalised(xy_down: np.ndarray, width: int, height: int) -> np.ndarray:
    xy = np.asarray(xy_down, dtype=float)
    return np.stack([xy[..., 0] / width, 1.0 - xy[..., 1] / height], axis=-1)


def build_pin_animation(retargeted: Retargeted, anchors_px: dict[str, tuple[float, float]], width: int,
                        height: int, provenance: Provenance, mesh: MeshSettings | None = None,
                        joints: list[str] | None = None) -> PinAnimation:
    """One pin per rig joint that has both an anchor and a track (at most 20)."""
    provenance.validate()
    names = [j for j in (joints or RIG_JOINTS) if j in anchors_px and j in retargeted.tracks]
    if not names:
        raise ValueError("Pin oluşturulacak eklem yok")
    if len(names) > MAX_PINS:
        raise ValueError(f"Puppet Pin en fazla {MAX_PINS} pin destekler ({len(names)} istendi)")
    pins = []
    for i, name in enumerate(names):
        anchor = to_normalised(np.array(anchors_px[name]), width, height)
        track = to_normalised(retargeted.tracks[name], width, height)
        if not np.all(np.isfinite(track)):
            raise ValueError(f"{name} izinde geçersiz değer var")
        pins.append(PinTrack(i + 1, name, RIG_LABELS_TR.get(name, name), (float(anchor[0]), float(anchor[1])), track))
    view = {"yaw_deg": retargeted.view_yaw_deg, "reason": retargeted.view_reason,
            "px_per_m": retargeted.px_per_m, "warnings": retargeted.warnings}
    return PinAnimation(width, height, retargeted.fps, pins, mesh or MeshSettings(), provenance, view=view)
