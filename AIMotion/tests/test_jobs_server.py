"""Job folder flow and the local HTTP API, without any model (test fixtures only)."""

import base64
import io
import json
import threading
import time
import urllib.request
from http.server import ThreadingHTTPServer
from pathlib import Path

import numpy as np
import pytest
from PIL import Image, ImageDraw

from aimotion import jobs, paths
from aimotion.pins import PinAnimation
from aimotion.provenance import Provenance
from aimotion.server import App, make_handler

from fixtures import REST, raise_left_arm, rig_from_pose


def _character_png(w=300, h=500) -> bytes:
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.ellipse([120, 10, 180, 70], fill=(230, 190, 160, 255))  # head
    d.rectangle([110, 70, 190, 260], fill=(40, 90, 200, 255))  # body
    d.rectangle([70, 80, 105, 250], fill=(40, 90, 200, 255))  # arms
    d.rectangle([195, 80, 230, 250], fill=(40, 90, 200, 255))
    d.rectangle([115, 260, 145, 480], fill=(60, 60, 60, 255))  # legs
    d.rectangle([155, 260, 185, 480], fill=(60, 60, 60, 255))
    buf = io.BytesIO()
    im.save(buf, "PNG")
    return buf.getvalue()


@pytest.fixture
def layout(tmp_path):
    lay = paths.resolve_layout(tmp_path / "AIMotion", check_drive=False)
    paths.apply_environment(lay)
    return lay


def _rig_src(w=300, h=500):
    # front view joints on the drawn character (source pixels)
    return {"width": w, "height": h, "joints": {
        "head": [150, 40], "neck": [150, 80], "pelvis": [150, 255], "l_shoulder": [195, 85], "r_shoulder": [105, 85],
        "l_elbow": [212, 165], "r_elbow": [88, 165], "l_wrist": [212, 245], "r_wrist": [88, 245],
        "l_hip": [170, 262], "r_hip": [130, 262], "l_knee": [170, 370], "r_knee": [130, 370],
        "l_ankle": [170, 470], "r_ankle": [130, 470]}}


def test_fixture_job_to_vegas_manifest(layout, monkeypatch, tmp_path):
    d = layout.job_dir("test-job-1")
    d.mkdir(parents=True)
    (d / "character.png").write_bytes(_character_png())
    (d / "rig_src.json").write_text(json.dumps(_rig_src()), encoding="utf-8")
    spec = {"format": jobs.JOB_FORMAT, "kind": "pins", "stage": {"width": 640, "height": 360, "fps": 25.0},
            "retarget": {"view_yaw_deg": 0.0}, "mesh": {"density": 30}}
    (d / "job.json").write_text(json.dumps(spec), encoding="utf-8")
    rig, placement = jobs.prepare_stage(d, spec)
    assert Image.open(d / "stage.png").size == (640, 360)
    assert 0 < placement.scale < 1  # 500 px tall character fitted into a 360 px stage

    cand = d / "cand_7"
    cand.mkdir()
    prov = Provenance(source="test_fixture")
    info = jobs.make_pins(d, cand, spec, raise_left_arm(31), 30.0, prov)
    assert info["frames"] == 26 and (cand / "preview.mp4").stat().st_size > 0
    (cand / "provenance.json").write_text(json.dumps(prov.to_dict()), encoding="utf-8")

    manifest = json.loads(jobs.export_to_vegas(layout, d, 7).read_text(encoding="utf-8"))
    assert manifest["kind"] == "pins" and manifest["label"] == "TEST - AI DEĞİL"
    out = layout.outbox / "test-job-1_7"
    anim = PinAnimation.load(out / "pins.json")
    assert anim.width == 640 and anim.fps == 25.0 and len(anim.pins) == 16  # no toes in this rig
    latest = json.loads((layout.outbox / "latest.json").read_text(encoding="utf-8"))
    assert Path(latest["manifest"]) == out / "manifest.json"

    # pins anchored on the character's pixels (alpha > 0) in the stage image
    stage = np.asarray(Image.open(out / "stage.png"))
    for p in anim.pins:
        x, y = p.anchor[0] * 640, (1 - p.anchor[1]) * 360
        assert stage[int(y), int(x), 3] > 0, p.joint


def test_retarget_redo_records_edit(layout):
    d = layout.job_dir("test-job-2")
    d.mkdir(parents=True)
    (d / "character.png").write_bytes(_character_png())
    (d / "rig_src.json").write_text(json.dumps(_rig_src()), encoding="utf-8")
    spec = {"format": jobs.JOB_FORMAT, "kind": "pins", "stage": {"width": 640, "height": 360, "fps": 30.0}}
    (d / "job.json").write_text(json.dumps(spec), encoding="utf-8")
    jobs.prepare_stage(d, spec)
    cand = d / "cand_3"
    cand.mkdir()
    np.savez_compressed(cand / "motion.npz", keypoints3d=raise_left_arm(20), fps=30.0)
    (cand / "provenance.json").write_text(json.dumps(Provenance(source="test_fixture").to_dict()), encoding="utf-8")
    (d / "status.json").write_text(json.dumps({"state": "done", "results": [{"seed": 3, "dir": "cand_3"}]}), encoding="utf-8")
    info = jobs.redo_pins(layout, d, 3, {"view_yaw_deg": 0.0, "root_motion": False})
    assert info["view"]["yaw_deg"] == 0.0
    prov = json.loads((cand / "provenance.json").read_text(encoding="utf-8"))
    assert prov["edits"][0]["type"] == "retarget"
    assert json.loads((d / "status.json").read_text(encoding="utf-8"))["results"][0]["frames"] == 20


@pytest.fixture
def server(layout):
    httpd = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(App(layout)))
    t = threading.Thread(target=httpd.serve_forever, daemon=True)
    t.start()
    yield f"http://127.0.0.1:{httpd.server_port}"
    httpd.shutdown()


def _call(base, path, body=None, origin=True):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(base + path, data=data, method="POST" if body is not None else "GET")
    req.add_header("Content-Type", "application/json")
    if origin and body is not None:
        req.add_header("Origin", base)
    try:
        with urllib.request.urlopen(req) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read())


def test_http_flow_without_models(server, layout):
    st, state = _call(server, "/api/state")
    assert st == 200 and state["models"]["hymotion"]  # nothing installed in the test root
    img = "data:image/png;base64," + base64.b64encode(_character_png()).decode()
    st, up = _call(server, "/api/upload", {"image": img})
    assert st == 200 and up["has_alpha"] and up["width"] == 300
    job = up["job_id"]

    pose = {"canvas_width": 300, "canvas_height": 500, "people": [{"pose_keypoints_2d": [0] * 54}]}
    st, err = _call(server, "/api/rig-import", {"job_id": job, "json": json.dumps(pose)})
    assert st == 200 and err["joints"] == {}  # all-zero keypoints are ignored

    st, err = _call(server, "/api/generate", {"job_id": job, "prompt": "walk", "rig": {"width": 300, "height": 500,
                                                                                       "joints": {"head": [1, 1]}}})
    assert st == 400 and "eksik" in err["error"]

    # a cross-site page cannot drive the API
    req = urllib.request.Request(server + "/api/upload", data=b"{}", method="POST")
    req.add_header("Origin", "http://evil.example")
    with pytest.raises(urllib.error.HTTPError) as e:
        urllib.request.urlopen(req)
    assert e.value.code == 403

    # path traversal through /files is refused
    st, _ = _call(server, f"/files/{job}/..%2F..%2Fselftest.json")
    assert st in (403, 404)

    # generation without the models: the worker reports what is missing, no fallback
    st, res = _call(server, "/api/generate", {"job_id": job, "prompt": "a person walks", "rig": _rig_src()})
    assert st == 200
    for _ in range(100):
        st, s = _call(server, f"/api/job/{job}")
        if s.get("state") in ("done", "error", "cancelled"):
            break
        time.sleep(0.2)
    assert s["state"] == "error", s
    assert "HY-Motion" in s["error"] or "GPU" in s["error"]
    assert not s.get("results")


def test_out_of_frame_warning():
    from aimotion.pins import MeshSettings, PinTrack

    track = np.stack([np.linspace(0.5, 1.3, 30), np.full(30, 0.4)], axis=1)
    anim = PinAnimation(640, 360, 30.0, [PinTrack(1, "pelvis", "", (0.5, 0.4), track)], MeshSettings(),
                        Provenance(source="test_fixture"))
    w = jobs._frame_warnings(anim)
    assert len(w) == 1 and "kareden çıkıyor" in w[0]
    anim.pins[0].track = np.full((30, 2), 0.5)
    assert jobs._frame_warnings(anim) == []
