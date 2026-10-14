"""puppet_render (PuppetCore.h) and the preview pipeline."""

import os
import shutil
import subprocess
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

from aimotion.pins import MeshSettings, PinAnimation, PinTrack
from aimotion.preview import render_frames, render_preview
from aimotion.provenance import Provenance

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="session")
def renderer(tmp_path_factory):
    exe = os.environ.get("AIMOTION_PUPPET_RENDER")
    if exe and Path(exe).is_file():
        return exe
    if not (shutil.which("cmake") and (shutil.which("c++") or shutil.which("g++") or shutil.which("cl"))):
        pytest.skip("C++ derleyici yok")
    build = tmp_path_factory.mktemp("native")
    subprocess.run(["cmake", "-S", str(ROOT / "native"), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release"],
                   check=True, capture_output=True)
    subprocess.run(["cmake", "--build", str(build), "--config", "Release"], check=True, capture_output=True)
    for c in (build / "puppet_render", build / "Release" / "puppet_render.exe", build / "puppet_render.exe"):
        if c.is_file():
            return str(c)
    pytest.fail("puppet_render derlenemedi")


def _stage(tmp_path, w=200, h=120):
    rng = np.random.default_rng(0)
    img = np.zeros((h, w, 4), np.uint8)
    img[30:90, 60:140, :3] = rng.integers(0, 255, (60, 80, 3))
    img[30:90, 60:140, 3] = 255
    p = tmp_path / "stage.png"
    Image.fromarray(img, "RGBA").save(p)
    return p, img


def _anim(w, h, anchors_px, offsets_px, frames=3, source="test_fixture"):
    pins = []
    for i, ((x, y), (dx, dy)) in enumerate(zip(anchors_px, offsets_px)):
        a = (x / w, 1 - y / h)
        track = np.array([[(x + dx * f / max(1, frames - 1)) / w, 1 - (y + dy * f / max(1, frames - 1)) / h]
                          for f in range(frames)])
        pins.append(PinTrack(i + 1, f"p{i}", "", a, track))
    return PinAnimation(w, h, 30.0, pins, MeshSettings(density=20, expansion=2.0), Provenance(source=source))


def test_pins_at_anchors_leave_the_picture_unchanged(tmp_path, renderer):
    stage, img = _stage(tmp_path)
    anim = _anim(200, 120, [(70, 40), (130, 80), (70, 80)], [(0, 0)] * 3)
    frames = list(render_frames(anim, stage, tmp_path / "w", background=None, renderer=renderer))
    assert len(frames) == 3
    out = frames[-1][1]
    inner = (slice(34, 86), slice(64, 136))
    assert np.abs(out[inner].astype(int) - img[inner].astype(int)).max() <= 1


def test_moving_every_pin_translates_the_picture(tmp_path, renderer):
    stage, img = _stage(tmp_path)
    anim = _anim(200, 120, [(70, 40), (130, 80), (70, 80), (130, 40)], [(12, -6)] * 4)
    out = list(render_frames(anim, stage, tmp_path / "w", background=None, renderer=renderer))[-1][1]
    inner_src = img[36:84, 66:134]
    inner_dst = out[30:78, 78:146]  # 12 px right, 6 px up (image y down)
    assert np.abs(inner_dst.astype(int) - inner_src.astype(int)).max() <= 2


def test_preview_mp4_and_test_label(tmp_path, renderer, monkeypatch):
    if not shutil.which("ffmpeg"):
        pytest.skip("ffmpeg yok")
    monkeypatch.setenv("AIMOTION_PUPPET_RENDER", renderer)
    stage, _ = _stage(tmp_path)
    anim = _anim(200, 120, [(70, 40), (130, 80)], [(20, 0), (0, 0)], frames=12)
    out = tmp_path / "p.mp4"
    res = render_preview(anim, tmp_path, out, tmp_path / "w")
    assert res["kind"] == "warp" and out.stat().st_size > 1000


def test_part_layers_move_independently(tmp_path, renderer, monkeypatch):
    """Two parts: only the right part has pins that move; the left part must stay put."""
    from aimotion.preview import composed_frames

    monkeypatch.setenv("AIMOTION_PUPPET_RENDER", renderer)
    stage, img = _stage(tmp_path)
    left, right = img.copy(), img.copy()
    left[:, 100:] = 0
    right[:, :100] = 0
    Image.fromarray(left, "RGBA").save(tmp_path / "stage_layer_00.png")
    Image.fromarray(right, "RGBA").save(tmp_path / "stage_layer_01.png")
    anim = _anim(200, 120, [(70, 40), (70, 80), (120, 40), (120, 80)], [(0, 0), (0, 0), (0, 15), (0, 15)])
    anim.layers = [{"image": "stage_layer_00.png", "pins": [1, 2]}, {"image": "stage_layer_01.png", "pins": [3, 4]}]
    out = list(composed_frames(anim, tmp_path, tmp_path / "w", renderer))[-1][1]
    assert np.abs(out[36:84, 64:96].astype(int) - img[36:84, 64:96].astype(int)).max() <= 1  # left part unchanged
    moved = out[51:84, 104:136]  # right part 15 px lower (image y down)
    assert np.abs(moved.astype(int) - img[36:69, 104:136].astype(int)).max() <= 2
