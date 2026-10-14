import numpy as np
from PIL import Image

from aimotion.layers import assign, role_from_name


def test_roles_from_turkish_and_english_names():
    assert role_from_name("02_Gövde.png", "l") == "body"
    assert role_from_name("00_uzak_kol.png", "l") == "l_arm"  # facing right: the far side is the left
    assert role_from_name("04_yakın_kol.png", "l") == "r_arm"
    assert role_from_name("near-leg.png", "r") == "l_leg"
    assert role_from_name("sağ bacak.png", "l") == "r_leg"
    assert role_from_name("left_arm.png", "r") == "l_arm"
    assert role_from_name("parca3.png", "l") is None


def test_side_view_layers_from_drawing(tmp_path):
    """Side view facing right: both arms overlap the torso; depth decides far/near."""
    W, H = 200, 300
    j = {"head": (100, 30), "neck": (100, 60), "chest": (100, 90), "pelvis": (100, 150), "l_shoulder": (98, 62),
         "r_shoulder": (102, 62), "l_elbow": (96, 110), "r_elbow": (104, 110), "l_wrist": (95, 150),
         "r_wrist": (105, 150), "l_hip": (98, 150), "r_hip": (102, 150), "l_knee": (97, 220), "r_knee": (103, 220),
         "l_ankle": (96, 290), "r_ankle": (104, 290)}
    pins = {name: i + 1 for i, name in enumerate(j)}

    def layer(name, boxes):
        a = np.zeros((H, W, 4), np.uint8)
        for x0, y0, x1, y1 in boxes:
            a[y0:y1, x0:x1] = 255
        p = tmp_path / name
        Image.fromarray(a, "RGBA").save(p)
        return p

    files = [layer("a.png", [(90, 55, 100, 160)]),  # far arm (unnamed)
             layer("b.png", [(80, 0, 120, 160)]),  # head + torso
             layer("c.png", [(100, 55, 112, 160)])]  # near arm
    out = assign(files, j, pins, yaw_deg=90.0)
    assert [o["role"] for o in out] == ["l_arm", "body", "r_arm"]
    by = {o["image"]: {k for k, v in pins.items() if v in o["pins"]} for o in out}
    assert by["b.png"] == {"pelvis", "chest", "neck", "head", "l_shoulder", "r_shoulder", "l_hip", "r_hip"}
    assert "r_elbow" not in by["b.png"]  # the torso does not follow the elbow drawn over it
