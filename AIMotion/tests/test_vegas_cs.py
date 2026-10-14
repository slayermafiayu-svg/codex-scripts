"""Compiles the VEGAS C# files as C# 5 against a stub API and runs the importer
on a fake timeline. This checks syntax, C# 5 compatibility and our own logic;
it cannot prove the real ScriptPortal.Vegas API matches the stub (see
tests/vegas_stub/ScriptPortal.Vegas.Stub.cs)."""

import json
import os
import shutil
import subprocess
from pathlib import Path

import numpy as np
import pytest

from aimotion import jobs, paths
from aimotion.provenance import Provenance

from fixtures import raise_left_arm
from test_jobs_server import _character_png, _rig_src

ROOT = Path(__file__).resolve().parents[1]
STUB = ROOT / "tests" / "vegas_stub"

pytestmark = pytest.mark.skipif(not (shutil.which("mcs") and shutil.which("mono")), reason="Mono yok")


@pytest.fixture(scope="module")
def driver(tmp_path_factory):
    out = tmp_path_factory.mktemp("cs")
    stub = out / "ScriptPortal.Vegas.dll"
    subprocess.run(["mcs", "-langversion:5", "-target:library", f"-out:{stub}",
                    str(STUB / "ScriptPortal.Vegas.Stub.cs")], check=True, capture_output=True, text=True)
    # The extension and the script exactly as shipped, compiled together as build_extension.bat does.
    lib = out / "AIMotionBridge.dll"
    r = subprocess.run(["mcs", "-langversion:5", "-target:library", "-warnaserror+", f"-out:{lib}", f"-r:{stub}",
                        "-r:System.Windows.Forms.dll", str(ROOT / "vegas" / "AIMotionBridge.cs"),
                        str(ROOT / "vegas" / "AIMotion_Import.cs")], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    exe = out / "driver.exe"
    subprocess.run(["mcs", "-langversion:5", f"-out:{exe}", f"-r:{stub}", f"-r:{lib}", "-r:System.Windows.Forms.dll",
                    str(STUB / "Driver.cs")], check=True, capture_output=True, text=True)
    return exe


def _export_fixture(tmp_path, kind="pins", layers=False):
    lay = paths.resolve_layout(tmp_path / "AIMotion", check_drive=False)
    paths.apply_environment(lay)
    d = lay.job_dir("vegas-test")
    d.mkdir(parents=True)
    (d / "character.png").write_bytes(_character_png())
    if layers:  # two parts: the character's right arm (image left) behind, the rest in front
        import io

        from PIL import Image

        full = np.asarray(Image.open(io.BytesIO(_character_png())))
        arm = np.zeros_like(full)
        arm[:, :108] = full[:, :108]
        rest = full.copy()
        rest[:, :108] = 0
        (d / "layers").mkdir()
        Image.fromarray(arm, "RGBA").save(d / "layers" / "00_sag_kol.png")
        Image.fromarray(rest, "RGBA").save(d / "layers" / "01_govde.png")
    (d / "rig_src.json").write_text(json.dumps(_rig_src()))
    spec = {"format": jobs.JOB_FORMAT, "kind": kind, "stage": {"width": 640, "height": 360, "fps": 25.0},
            "retarget": {"view_yaw_deg": 0.0}, "mesh": {"density": 30}}
    (d / "job.json").write_text(json.dumps(spec))
    jobs.prepare_stage(d, spec)
    cand = d / "cand_5"
    cand.mkdir()
    prov = Provenance(source="test_fixture")
    if kind == "pins":
        jobs.make_pins(d, cand, spec, raise_left_arm(31), 30.0, prov)
    else:
        from PIL import Image

        (cand / "frames").mkdir()
        for i in range(12):
            Image.new("RGBA", (64, 36), (i * 20, 0, 0, 255)).save(cand / "frames" / f"frame_{i:05d}.png")
        (cand / "preview.mp4").write_bytes(b"x")
        prov.params = {"fps": 24}
    (cand / "provenance.json").write_text(json.dumps(prov.to_dict()))
    jobs.export_to_vegas(lay, d, 5)
    return lay


def _run(driver, lay, size=(640, 360, 25.0)):
    env = dict(os.environ, AIMOTION_ROOT=str(lay.root))
    r = subprocess.run(["mono", str(driver), str(size[0]), str(size[1]), str(size[2])], capture_output=True,
                       text=True, env=env)
    assert r.returncode == 0, r.stdout + r.stderr
    return {line.split(" ", 1)[0]: line.split(" ", 1)[1] for line in r.stdout.splitlines() if " " in line} | {
        "_pins": [line.split()[1:] for line in r.stdout.splitlines() if line.startswith("PIN ")]}


def test_pins_import_on_fake_timeline(driver, tmp_path):
    lay = _export_fixture(tmp_path)
    out = _run(driver, lay)
    start, length, rest = out["EVENT"].split(" ", 2)
    assert float(start) == 1000.0  # at the cursor
    assert float(length) == pytest.approx(26 / 25 * 1000, abs=0.01)
    assert rest.startswith("TEST - AI DEĞİL|") and rest.endswith("stage.png")
    assert out["MODE"] == "1" and out["DENSITY"] == "30"
    pins = out["_pins"]
    assert len(pins) == 16
    lines = (lay.outbox / "vegas-test_5" / "vegas_import.txt").read_text().splitlines()
    anchors = {int(l.split()[1]): (float(l.split()[2]), float(l.split()[3])) for l in lines if l.startswith("pin ")}
    last = {}
    for l in lines:
        if l.startswith("key "):
            n, i, u, v = l.split()[1:]
            last[int(n)] = (float(u), float(v))
    for k, ax, ay, animated, count, t0, t1, x1, y1 in pins:
        k = int(k)
        assert (float(ax), float(ay)) == pytest.approx(anchors[k], abs=1e-6)
        assert animated == "True" and int(count) == 26
        assert float(t0) == 0.0 and float(t1) == pytest.approx(25 / 25 * 1000)
        assert (float(x1), float(y1)) == pytest.approx(last[k], abs=1e-6)
    assert "PLAYING" not in out["MSG"]
    hooks = out["HOOKS"].split()
    assert hooks[:2] == ["True", "True"] and hooks[2] == "False"  # the stub has no RenderStarted event
    playing = json.loads(out["STATE_PLAYING"])
    stopped = json.loads(out["STATE_STOPPED"])
    assert playing["playing"] is True and stopped["playing"] is False
    assert playing["project"] == {"width": 640, "height": 360, "fps": 25}


def test_aspect_warning(driver, tmp_path):
    lay = _export_fixture(tmp_path)
    out = _run(driver, lay, size=(1440, 1080, 25.0))
    assert "en-boy oranı farklı" in out["MSG"]


def test_frames_import_uses_image_sequence(driver, tmp_path):
    lay = _export_fixture(tmp_path, kind="frames")
    out = _run(driver, lay)
    assert out["SEQ"].endswith("frame_00000.png")
    assert float(out["EVENT"].split()[1]) == pytest.approx(12 / 24 * 1000)


def test_layered_import_stacks_parts_front_on_top(driver, tmp_path):
    lay = _export_fixture(tmp_path, layers=True)
    out = _run(driver, lay)
    top, bottom = out["TRACK0"].split("|"), out["TRACK1"].split("|")
    assert top[1].endswith("stage_layer_01_01_govde.png") and bottom[1].endswith("stage_layer_00_00_sag_kol.png")
    lines = (lay.outbox / "vegas-test_5" / "vegas_import.txt").read_text().splitlines()
    joint = {int(l.split()[1]): l.split()[4] for l in lines if l.startswith("pin ")}
    layer_lines = [l for l in lines if l.startswith("layer ")]
    arm_pins = {joint[int(n)] for n in layer_lines[0].split()[1].split(",")}
    body_pins = {joint[int(n)] for n in layer_lines[1].split()[1].split(",")}
    assert arm_pins == {"r_shoulder", "r_elbow", "r_wrist"}
    assert body_pins == {"pelvis", "chest", "neck", "head", "l_shoulder", "r_shoulder", "l_hip", "r_hip"}
    assert int(bottom[2]) == 3 and int(top[2]) == 8
