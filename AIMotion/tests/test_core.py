import json
import math
import os
from pathlib import Path

import numpy as np
import pytest

from aimotion import guard, paths, profiles
from aimotion.pins import MAX_PINS, MeshSettings, PinAnimation, build_pin_animation, to_normalised
from aimotion.provenance import Provenance, ProvenanceError, git_revision, sha256_file
from aimotion.retarget import RetargetOptions, estimate_view_yaw, normalize_heading, project, retarget
from aimotion.skeleton import (COCO_WHOLEBODY, OPENPOSE18, SMPL, Rig, load_rig_file, rig_from_keypoints,
                               rig_points_from_motion)

from fixtures import REST, lean_forward_planted, raise_left_arm, rig_from_pose, rot_y, static_motion

TEST_PROV = Provenance(source="test_fixture")


# --- skeleton / view ---------------------------------------------------------

def test_normalize_heading_turns_motion_to_face_plus_z():
    m = static_motion(5, yaw_deg=73.0)
    m[..., 0] += 2.0
    out = normalize_heading(m)
    p = rig_points_from_motion(out)
    left = p["l_hip"][0] - p["r_hip"][0]
    assert abs(left[2]) < 1e-9 and left[0] > 0  # left side is +X again -> facing +Z
    assert abs(p["pelvis"][0][0]) < 1e-9 and abs(p["pelvis"][0][2]) < 1e-9


@pytest.mark.parametrize("yaw", [0.0, 45.0, 90.0, -90.0, -45.0])
def test_view_yaw_estimate_matches_drawn_view(yaw):
    rig = rig_from_pose(REST, yaw)
    est, _ = estimate_view_yaw(rig)
    assert abs(est - yaw) < 12.0


def test_projection_side_view_maps_forward_to_image_right():
    pts = np.array([[0.0, 0.0, 1.0]])
    assert project(pts, 90.0)[0, 0] == pytest.approx(1.0)
    assert project(pts, -90.0)[0, 0] == pytest.approx(-1.0)
    assert project(np.array([[1.0, 0, 0]]), 0.0)[0, 0] == pytest.approx(1.0)


def test_openpose_and_wholebody_import(tmp_path):
    kp = np.zeros((18, 3))
    for i, name in OPENPOSE18.items():
        kp[i] = [100 + i * 10, 200 + i * 5, 0.9]
    doc = {"canvas_width": 1000, "canvas_height": 800, "people": [{"pose_keypoints_2d": kp.ravel().tolist()}]}
    f = tmp_path / "pose.json"
    f.write_text(json.dumps([doc]))
    rig = load_rig_file(f, 2000, 1600)  # scaled to the stage
    assert rig.joints["l_shoulder"] == pytest.approx((2 * (100 + 50), 2 * (200 + 25)))
    assert "pelvis" in rig.joints and "chest" in rig.joints

    wb = np.zeros((133, 3))
    for i in COCO_WHOLEBODY:
        wb[i] = [0.1 + i / 400, 0.2 + i / 500, 0.8]
    rig2 = rig_from_keypoints(wb, COCO_WHOLEBODY, 1000, 1000, "coco-wholebody")  # normalised input
    assert rig2.joints["l_toe"][0] == pytest.approx((0.1 + 17 / 400) * 1000)
    assert not rig2.missing_required()


# --- retargeting -------------------------------------------------------------

def _tracks_at(res, name, i):
    return res.tracks[name][i]


@pytest.mark.parametrize("yaw", [0.0, 90.0, -90.0])
def test_static_motion_keeps_the_drawing_still(yaw):
    rig = rig_from_pose(REST, yaw)
    res = retarget(static_motion(20), rig, RetargetOptions(view_yaw_deg=yaw))
    for name, (x, y) in rig.joints.items():
        assert np.abs(res.tracks[name] - np.array([x, y])).max() < 1.0, name


def test_arm_raise_follows_motion_and_keeps_character_bone_lengths():
    # The character has 1.3x longer arms than the motion's body.
    rig = rig_from_pose(REST, 0.0)
    j = dict(rig.joints)
    for a, b in (("l_shoulder", "l_elbow"), ("l_elbow", "l_wrist")):
        pa, pb = np.array(j[a]), np.array(j[b])
        j[b] = tuple(pa + (pb - pa) * 1.3)
        if b == "l_elbow":  # move the wrist along
            j["l_wrist"] = tuple(np.array(j["l_wrist"]) + (np.array(j[b]) - pb))
    rig = Rig(rig.width, rig.height, j)
    res = retarget(raise_left_arm(31), rig, RetargetOptions(view_yaw_deg=0.0, blend_in_s=0.0))
    sh, el, wr = (res.tracks[n][-1] for n in ("l_shoulder", "l_elbow", "l_wrist"))
    upper = el - sh
    # 80 degrees up from hanging down: arm points mostly to the image right (character's left), slightly down
    ang = math.degrees(math.atan2(-upper[1], upper[0]))
    assert -20 < ang < 0
    drawn = np.linalg.norm(np.array(j["l_elbow"]) - np.array(j["l_shoulder"]))
    assert np.linalg.norm(upper) == pytest.approx(drawn, rel=0.05)
    # right arm untouched
    assert np.abs(res.tracks["r_wrist"][-1] - np.array(j["r_wrist"])).max() < 1.0


def test_foot_lock_keeps_planted_feet_still_for_other_proportions():
    rig = rig_from_pose(REST, 90.0)
    # character with much shorter legs than the motion body
    j = dict(rig.joints)
    for side in ("l", "r"):
        hip = np.array(j[f"{side}_hip"])
        for n in ("knee", "ankle", "toe"):
            j[f"{side}_{n}"] = tuple(hip + (np.array(j[f"{side}_{n}"]) - hip) * 0.6)
    rig = Rig(rig.width, rig.height, j)
    motion = lean_forward_planted(30, dz=0.2)
    locked = retarget(motion, rig, RetargetOptions(view_yaw_deg=90.0, foot_lock=True))
    free = retarget(motion, rig, RetargetOptions(view_yaw_deg=90.0, foot_lock=False))
    drift_locked = np.abs(locked.tracks["l_toe"][-1] - locked.tracks["l_toe"][0]).max()
    drift_free = np.abs(free.tracks["l_toe"][-1] - free.tracks["l_toe"][0]).max()
    assert drift_free > 10.0  # without the lock the short legs slide
    assert drift_locked < 2.0


def test_in_place_mode_drops_horizontal_travel():
    m = static_motion(30)
    m[:, :, 2] += np.linspace(0, 1.0, 30)[:, None]  # walk 1 m forward
    rig = rig_from_pose(REST, 90.0)
    moving = retarget(m, rig, RetargetOptions(view_yaw_deg=90.0, foot_lock=False))
    still = retarget(m, rig, RetargetOptions(view_yaw_deg=90.0, root_motion=False))
    assert moving.tracks["pelvis"][-1][0] - moving.tracks["pelvis"][0][0] == pytest.approx(400.0, rel=0.05)
    assert abs(still.tracks["pelvis"][-1][0] - still.tracks["pelvis"][0][0]) < 1e-6


def test_resampling_to_project_fps():
    res = retarget(static_motion(31), rig_from_pose(REST, 0.0), RetargetOptions(view_yaw_deg=0.0, out_fps=25.0))
    assert res.num_frames == 26  # 1.0 s at 25 fps, both ends included


def test_missing_joints_are_reported():
    rig = rig_from_pose(REST, 0.0)
    del rig.joints["l_knee"]
    with pytest.raises(ValueError, match="l_knee"):
        retarget(static_motion(5), rig)


# --- pins / provenance -------------------------------------------------------

def test_pin_animation_round_trip(tmp_path):
    rig = rig_from_pose(REST, 0.0)
    res = retarget(raise_left_arm(10), rig, RetargetOptions(view_yaw_deg=0.0))
    anim = build_pin_animation(res, rig.joints, rig.width, rig.height, TEST_PROV, MeshSettings(density=40))
    assert len(anim.pins) == 18 <= MAX_PINS
    p = tmp_path / "pins.json"
    anim.save(p)
    back = PinAnimation.load(p)
    assert back.num_frames == 10 and back.mesh.density == 40
    assert np.allclose(back.pins[0].anchor, to_normalised(np.array(rig.joints["pelvis"]), 1920, 1080), atol=1e-5)
    d = json.loads(p.read_text())
    assert d["pins"][4]["param"] == "pin05"
    assert d["provenance"]["source"] == "test_fixture"


def test_normalised_coordinates_are_bottom_left_origin():
    assert to_normalised(np.array([0.0, 1080.0]), 1920, 1080) == pytest.approx([0.0, 0.0])
    assert to_normalised(np.array([1920.0, 0.0]), 1920, 1080) == pytest.approx([1.0, 1.0])


def test_ai_provenance_requires_model_hash_seed_prompt():
    with pytest.raises(ProvenanceError):
        Provenance(source="ai_model", model="x", prompt="walk").validate()
    with pytest.raises(ProvenanceError):
        Provenance(source="preset").validate()
    ok = Provenance(source="ai_model", model="m", checkpoint_sha256="ab", prompt="walk", seed=1).validate()
    assert ok.is_ai and "seed=1" in ok.label()
    assert Provenance(source="test_fixture").label() == "TEST - AI DEĞİL"


def test_sha256_cache_and_git_revision(tmp_path):
    f = tmp_path / "w.bin"
    f.write_bytes(b"abc")
    cache = tmp_path / "h.json"
    h = sha256_file(f, cache)
    assert h == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    assert json.loads(cache.read_text())
    g = tmp_path / "repo" / ".git"
    (g / "refs" / "heads").mkdir(parents=True)
    (g / "HEAD").write_text("ref: refs/heads/master\n")
    (g / "refs" / "heads" / "master").write_text("4e426f5\n")
    assert git_revision(tmp_path / "repo") == "4e426f5"


# --- guard -------------------------------------------------------------------

def _write_state(p: Path, **kw):
    p.write_text(json.dumps({"version": 1, "ts": kw.pop("ts"), **kw}))


def test_guard_states(tmp_path):
    f = tmp_path / "vegas_state.json"
    now = 1000.0
    _write_state(f, ts=now, playing=True)
    assert guard.read_vegas_state(f, now=now).state == guard.PLAYING
    _write_state(f, ts=now, playing=False, rendering=True)
    assert guard.read_vegas_state(f, now=now).state == guard.RENDERING
    _write_state(f, ts=now, playing=None)
    assert guard.read_vegas_state(f, now=now).state == guard.UNKNOWN
    _write_state(f, ts=now, playing=False, closed=True)
    assert guard.read_vegas_state(f, now=now).state == guard.NOT_RUNNING
    _write_state(f, ts=now, playing=False, project={"width": 1280, "height": 720, "fps": 25})
    st = guard.read_vegas_state(f, now=now)
    assert st.state == guard.IDLE and st.project["width"] == 1280
    # stale heartbeat while VEGAS still runs -> unknown; VEGAS closed -> not running
    assert guard.read_vegas_state(f, now=now + 10, process_running=lambda: True).state == guard.UNKNOWN
    assert guard.read_vegas_state(f, now=now + 10, process_running=lambda: False).state == guard.NOT_RUNNING
    assert guard.read_vegas_state(tmp_path / "none.json", process_running=lambda: True).state == guard.UNKNOWN


def test_guard_blocks_while_playing_and_resumes():
    seq = iter([guard.PLAYING, guard.PLAYING, guard.RENDERING, guard.IDLE])
    seen, slept = [], []
    g = guard.PlaybackGuard(Path("x"), reader=lambda f, s: guard.VegasState(next(seq)),
                            sleep=slept.append, on_wait=lambda st: seen.append(st.state))
    g.checkpoint()
    assert seen == [guard.PLAYING, guard.RENDERING] and len(slept) == 3 and g.pauses == 1


def test_guard_unknown_policy_and_cancel():
    g = guard.PlaybackGuard(Path("x"), unknown_policy="allow", reader=lambda f, s: guard.VegasState(guard.UNKNOWN))
    g.checkpoint()  # allowed
    blocking = guard.PlaybackGuard(Path("x"), reader=lambda f, s: guard.VegasState(guard.UNKNOWN),
                                   sleep=lambda s: blocking.cancel())
    with pytest.raises(guard.Cancelled):
        blocking.checkpoint()


# --- paths / profiles --------------------------------------------------------

def test_layout_refuses_other_drive_on_windows():
    with pytest.raises(paths.StorageError):
        paths.resolve_layout(r"C:\AIMotion", check_drive=True)
    assert paths.resolve_layout(r"D:\AIMotion", check_drive=True).root == Path(r"D:\AIMotion")


def test_apply_environment_points_caches_at_root(tmp_path, monkeypatch):
    for k in ("HF_HOME", "TORCH_HOME", "TMP", "USE_HF_MODELS"):
        monkeypatch.delenv(k, raising=False)
    lay = paths.resolve_layout(tmp_path / "root", check_drive=False)
    env = paths.apply_environment(lay)
    for k in ("HF_HOME", "HF_HUB_CACHE", "TORCH_HOME", "TRITON_CACHE_DIR", "CUDA_CACHE_PATH", "TMP", "TEMP"):
        assert env[k].startswith(str(tmp_path / "root")), k
        assert os.environ[k] == env[k]
    assert os.environ["USE_HF_MODELS"] == "0"
    assert (tmp_path / "root" / "state").is_dir()
    with pytest.raises(ValueError):
        lay.job_dir("..\\evil")
    with pytest.raises(paths.StorageError):
        paths.assert_under_root(lay, tmp_path / "elsewhere")


@pytest.mark.parametrize("gib,key", [(8, "8gb"), (11.9, "8gb"), (15.6, "16gb"), (16, "16gb"), (23.5, "24gb"),
                                     (48, "24gb"), (6, "8gb")])
def test_profile_pick(gib, key):
    assert profiles.pick_profile(int(gib * profiles.GIB)).key == key


def test_profiles_label_every_memory_figure():
    for p in profiles.PROFILES.values():
        assert p.hymotion.dit_dtype == "fp16"
        for fig in p.expected.values():
            assert fig.basis in ("official", "computed", "measured") and fig.note
