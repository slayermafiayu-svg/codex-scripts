"""Draws the two demo characters used by first_generation.bat and the docs.

    python examples/make_demo_characters.py

demo_side/   facing right, with part layers (far arm, far leg, body, near leg,
             near arm) so walking does not drag the torso along with the limbs
demo_front/  facing the viewer, a single picture

Each folder gets character.png (transparent), rig.json (joint positions in
the picture's pixels, known exactly because the figure is drawn from them)
and, for the side view, layers/*.png. The pictures are plain vector-style
drawings made by this script; they are inputs, not results.
"""

from __future__ import annotations

import json
from pathlib import Path

from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
SS = 3  # supersampling
W, H = 600, 1000

SKIN, HAIR, SHIRT, PANTS, SHOE = (236, 196, 160), (60, 40, 30), (52, 120, 200), (50, 58, 80), (30, 30, 34)


def shade(c, k):
    return tuple(int(v * k) for v in c)


class Canvas:
    def __init__(self):
        self.im = Image.new("RGBA", (W * SS, H * SS), (0, 0, 0, 0))
        self.d = ImageDraw.Draw(self.im)

    def limb(self, pts, width, color, outline=(20, 20, 24)):
        p = [(x * SS, y * SS) for x, y in pts]
        w = width * SS
        o = 3 * SS
        self.d.line(p, fill=outline, width=w + 2 * o, joint="curve")
        for x, y in p:
            self.d.ellipse([x - w / 2 - o, y - w / 2 - o, x + w / 2 + o, y + w / 2 + o], fill=outline)
        self.d.line(p, fill=color, width=w, joint="curve")
        for x, y in p:
            self.d.ellipse([x - w / 2, y - w / 2, x + w / 2, y + w / 2], fill=color)

    def poly(self, pts, color, outline=(20, 20, 24)):
        self.d.polygon([(x * SS, y * SS) for x, y in pts], fill=color, outline=outline, width=3 * SS)

    def ellipse(self, box, color, outline=(20, 20, 24)):
        self.d.ellipse([v * SS for v in box], fill=color, outline=outline, width=3 * SS)

    def image(self):
        return self.im.resize((W, H), Image.LANCZOS)


def side():
    """Facing right. Near side = the character's right (r_*), far side = left (l_*)."""
    j = {
        "head": (318, 118), "neck": (300, 238), "chest": (300, 320), "pelvis": (300, 520),
        "r_shoulder": (300, 252), "r_elbow": (304, 395), "r_wrist": (318, 520),
        "l_shoulder": (296, 250), "l_elbow": (286, 392), "l_wrist": (292, 515),
        "r_hip": (304, 525), "r_knee": (318, 720), "r_ankle": (306, 912), "r_toe": (372, 950),
        "l_hip": (296, 523), "l_knee": (282, 718), "l_ankle": (276, 910), "l_toe": (340, 948),
    }
    parts = {}

    c = Canvas()  # far arm
    c.limb([j["l_shoulder"], j["l_elbow"], j["l_wrist"]], 44, shade(SHIRT, 0.72))
    c.ellipse((j["l_wrist"][0] - 22, j["l_wrist"][1] - 10, j["l_wrist"][0] + 22, j["l_wrist"][1] + 34), shade(SKIN, 0.8))
    parts["00_uzak_kol"] = c.image()

    c = Canvas()  # far leg
    c.limb([j["l_hip"], j["l_knee"], j["l_ankle"]], 58, shade(PANTS, 0.72))
    c.poly([(250, 890), (300, 890), (350, 930), (352, 962), (248, 962)], shade(SHOE, 0.8))
    parts["01_uzak_bacak"] = c.image()

    c = Canvas()  # body: torso, neck, head
    c.poly([(250, 240), (350, 240), (362, 300), (352, 540), (248, 540), (238, 300)], SHIRT)
    c.limb([(300, 200), j["neck"]], 40, SKIN)
    c.ellipse((242, 40, 392, 198), SKIN)
    c.d.chord([242 * SS, 34 * SS, 392 * SS, 190 * SS], 180, 360, fill=HAIR)
    c.d.pieslice([236 * SS, 40 * SS, 330 * SS, 170 * SS], 90, 270, fill=HAIR)
    c.ellipse((350, 100, 364, 114), (30, 30, 30), outline=None)  # eye
    c.d.line([(352 * SS, 150 * SS), (370 * SS, 146 * SS)], fill=(150, 60, 60), width=3 * SS)  # mouth
    parts["02_govde"] = c.image()

    c = Canvas()  # near leg
    c.limb([j["r_hip"], j["r_knee"], j["r_ankle"]], 60, PANTS)
    c.poly([(280, 892), (332, 892), (384, 932), (386, 964), (278, 964)], SHOE)
    parts["03_yakin_bacak"] = c.image()

    c = Canvas()  # near arm
    c.limb([j["r_shoulder"], j["r_elbow"], j["r_wrist"]], 46, shade(SHIRT, 1.08))
    c.ellipse((j["r_wrist"][0] - 22, j["r_wrist"][1] - 10, j["r_wrist"][0] + 22, j["r_wrist"][1] + 36), SKIN)
    parts["04_yakin_kol"] = c.image()
    return j, parts


def front():
    """Facing the viewer: the character's left side is on the picture's right."""
    j = {
        "head": (300, 120), "neck": (300, 245), "chest": (300, 320), "pelvis": (300, 525),
        "l_shoulder": (380, 252), "l_elbow": (408, 395), "l_wrist": (420, 525),
        "r_shoulder": (220, 252), "r_elbow": (192, 395), "r_wrist": (180, 525),
        "l_hip": (340, 530), "l_knee": (346, 725), "l_ankle": (348, 915), "l_toe": (356, 958),
        "r_hip": (260, 530), "r_knee": (254, 725), "r_ankle": (252, 915), "r_toe": (244, 958),
    }
    c = Canvas()
    for s in ("l", "r"):
        c.limb([j[f"{s}_hip"], j[f"{s}_knee"], j[f"{s}_ankle"]], 64, PANTS)
        x = j[f"{s}_ankle"][0]
        c.ellipse((x - 42, 900, x + 42, 968), SHOE)
    c.poly([(212, 238), (388, 238), (396, 300), (372, 545), (228, 545), (204, 300)], SHIRT)
    for s in ("l", "r"):
        c.limb([j[f"{s}_shoulder"], j[f"{s}_elbow"], j[f"{s}_wrist"]], 46, shade(SHIRT, 0.95))
        x, y = j[f"{s}_wrist"]
        c.ellipse((x - 22, y - 8, x + 22, y + 38), SKIN)
    c.limb([(300, 205), j["neck"]], 44, SKIN)
    c.ellipse((225, 42, 375, 200), SKIN)
    c.d.chord([225 * SS, 36 * SS, 375 * SS, 150 * SS], 180, 360, fill=HAIR)
    for ex in (268, 332):
        c.ellipse((ex - 8, 108, ex + 8, 124), (30, 30, 30), outline=None)
    c.d.arc([276 * SS, 135 * SS, 324 * SS, 170 * SS], 20, 160, fill=(150, 60, 60), width=3 * SS)
    return j, c.image()


def write(folder: Path, joints: dict, character: Image.Image, layers: dict | None = None):
    folder.mkdir(parents=True, exist_ok=True)
    character.save(folder / "character.png", optimize=True)
    rig = {"width": W, "height": H, "source": "drawn", "joints": {k: list(v) for k, v in joints.items()}}
    (folder / "rig.json").write_text(json.dumps(rig, indent=1), encoding="utf-8")
    if layers:
        (folder / "layers").mkdir(exist_ok=True)
        for name, im in layers.items():
            im.save(folder / "layers" / f"{name}.png", optimize=True)


def main():
    j, parts = side()
    full = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    for im in parts.values():
        full.alpha_composite(im)
    write(HERE / "demo_side", j, full, parts)
    j, im = front()
    write(HERE / "demo_front", j, im)
    print("examples/demo_side, examples/demo_front yazıldı")


if __name__ == "__main__":
    main()
