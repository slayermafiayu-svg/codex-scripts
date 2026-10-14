import numpy as np
from PIL import Image

from aimotion.backends.wan import chroma_key, prepare_input_image


def test_prepare_input_image_fits_character_on_key_colour(tmp_path):
    im = Image.new("RGBA", (300, 600), (0, 0, 0, 0))
    im.paste((200, 50, 50, 255), (100, 100, 200, 500))
    p = tmp_path / "c.png"
    im.save(p)
    out = np.asarray(prepare_input_image(p, 960, 544))
    assert out.shape == (544, 960, 3)
    assert tuple(out[5, 5]) == (0, 177, 64)  # key colour background
    assert (np.abs(out.astype(int) - [200, 50, 50]).sum(-1) < 10).any()  # the character is there


def test_chroma_key_makes_background_transparent():
    f = np.zeros((2, 4, 4, 3), np.uint8)
    f[:] = (0, 177, 64)
    f[:, 1:3, 1:3] = (200, 50, 50)
    out = chroma_key(f)
    assert out.shape == (2, 4, 4, 4)
    assert out[0, 0, 0, 3] == 0 and out[0, 1, 1, 3] == 255
    assert tuple(out[0, 1, 1, :3]) == (200, 50, 50)
