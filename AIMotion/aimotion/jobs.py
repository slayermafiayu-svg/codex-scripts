"""Jobs: one folder per generation under <root>/jobs/<id>.

    job.json        what to do (written by the UI/server)
    status.json     progress, written by the worker (atomic replace)
    cancel          created by the server to cancel
    character.png   the uploaded picture (+ mask.png)
    rig_src.json    joints in the picture's pixels
    stage.png       the character on a project-sized transparent canvas
    cand_<seed>/    one folder per generated candidate:
        motion.npz      HY-Motion skeleton (pins jobs)
        pins.json       Puppet Pin keyframes          (pins jobs)
        frames/         PNG frames                    (frames jobs)
        preview.mp4
        provenance.json

Heavy work (the models) only runs in the worker process; retargeting and
previews are cheap and can be redone from the server without the models.
"""

from __future__ import annotations

import json
import os
import secrets
import shutil
import time
from dataclasses import asdict
from pathlib import Path
from typing import Callable, Optional

import numpy as np
from PIL import Image

from .paths import Layout, assert_under_root
from .pins import MeshSettings, PinAnimation, build_pin_animation
from .preview import bake_png_sequence, find_ffmpeg, render_preview
from .profiles import PROFILES, Profile, detect_gpu, pick_profile
from .provenance import Provenance
from .retarget import RetargetOptions, retarget
from .skeleton import Rig
from . import layers
from .stage import Placement, make_layer_stages, make_stage, rig_to_stage

JOB_FORMAT = "aimotion.job/1"
VEGAS_FORMAT = "aimotion.vegas/1"


def new_job_id() -> str:
    return time.strftime("%Y%m%d-%H%M%S") + "-" + secrets.token_hex(3)


def write_json_atomic(path: Path, data) -> None:
    tmp = Path(str(path) + f".{os.getpid()}.tmp")
    tmp.write_text(json.dumps(data, ensure_ascii=False, indent=1), encoding="utf-8")
    os.replace(tmp, path)


def read_json(path: Path, default=None):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except (FileNotFoundError, ValueError):
        return default


class Status:
    def __init__(self, job_dir: Path):
        self.path = job_dir / "status.json"
        self.data = read_json(self.path, {}) or {}

    def update(self, **kw) -> None:
        self.data.update(kw)
        self.data["updated"] = time.time()
        write_json_atomic(self.path, self.data)


def resolve_profile(key: str) -> Profile:
    if key and key != "auto":
        return PROFILES[key]
    gpu = detect_gpu()
    return pick_profile(gpu["total_bytes"]) if gpu.get("available") else PROFILES["8gb"]


# ------------------------------------------------------------------ stage/rig

def prepare_stage(job_dir: Path, spec: dict) -> tuple[Rig, Placement]:
    st = spec["stage"]
    size = (int(st["width"]), int(st["height"]))
    placement = Placement(**st["placement"]) if st.get("placement") else None
    mask = job_dir / "mask.png"
    stage = make_stage(job_dir / "character.png", size, mask if mask.is_file() else None, placement,
                       center_x=float(st.get("center_x", 0.5)), height_fraction=float(st.get("char_height", 0.72)))
    stage.image.save(job_dir / "stage.png")
    layer_files = []
    layer_srcs = sorted((job_dir / "layers").glob("*.png")) if (job_dir / "layers").is_dir() else []
    if layer_srcs:
        char_size = Image.open(job_dir / "character.png").size
        for i, (src, im) in enumerate(zip(layer_srcs, make_layer_stages(layer_srcs, char_size, size,
                                                                         stage.placement))):
            stem = "".join(c for c in src.stem if c.isalnum() or c in "_-")[:40]
            name = f"stage_layer_{i:02d}_{stem}.png"
            im.save(job_dir / name)
            layer_files.append(name)
    rig_src = Rig.from_dict(read_json(job_dir / "rig_src.json"))
    rig = rig_to_stage(rig_src, stage.placement, size).complete()
    write_json_atomic(job_dir / "rig_stage.json", rig.to_dict())
    write_json_atomic(job_dir / "stage.json", {"placement": asdict(stage.placement), "has_alpha": stage.has_alpha,
                                               "warnings": stage.warnings, "layers": layer_files})
    return rig, stage.placement


def _frame_warnings(anim: PinAnimation) -> list[str]:
    """Warns when the generated motion carries the character out of the frame."""
    pts = np.stack([p.track for p in anim.pins], axis=1)  # (N, pins, 2)
    outside = ((pts < -0.02) | (pts > 1.02)).any(axis=(1, 2))
    if not outside.any():
        return []
    t = int(np.argmax(outside)) / anim.fps
    return [f"Karakter {t:.1f}. saniyede kareden çıkıyor: başlangıç konumunu veya karakter yüksekliğini "
            f"değiştirin ya da 'Sahnede yer değiştir'i kapatın."]


def assign_layers(job_dir: Path, anim: PinAnimation) -> list[str]:
    """Pin subsets for the part layers (if any), by body part. Returns warnings."""
    files = (read_json(job_dir / "stage.json", {}) or {}).get("layers") or []
    warnings = []
    anim.layers = []
    if not files:
        return warnings
    joints_px = {p.joint: (p.anchor[0] * anim.width, (1 - p.anchor[1]) * anim.height) for p in anim.pins}
    pin_of_joint = {p.joint: p.number for p in anim.pins}
    anim.layers = layers.assign([job_dir / f for f in files], joints_px, pin_of_joint,
                                float(anim.view.get("yaw_deg") or 0.0))
    for layer in anim.layers:
        if not layer["pins"]:
            warnings.append(f"{layer['image']}: parçası anlaşılamadı/pin yok, olduğu gibi kalacak")
        elif not layer["role"]:
            warnings.append(f"{layer['image']}: parça adı/rolü anlaşılamadı, üzerindeki tüm pinler kullanıldı")
    return warnings


def make_pins(job_dir: Path, cand_dir: Path, spec: dict, k3d: np.ndarray, motion_fps: float,
              prov: Provenance, retarget_opts: Optional[dict] = None,
              progress: Optional[Callable[[float], None]] = None) -> dict:
    """Retarget + pins.json + preview.mp4 for one candidate (cheap; no model)."""
    rig = Rig.from_dict(read_json(job_dir / "rig_stage.json"))
    ro = dict(spec.get("retarget") or {})
    ro.update(retarget_opts or {})
    opts = RetargetOptions(
        view_yaw_deg=ro.get("view_yaw_deg"),
        root_motion=bool(ro.get("root_motion", True)),
        foot_lock=bool(ro.get("foot_lock", True)),
        blend_in_s=float(ro.get("blend_in_s", 0.25)),
        motion_fps=motion_fps,
        out_fps=float(spec["stage"].get("fps") or 30.0),
    )
    res = retarget(k3d, rig, opts)
    mesh = MeshSettings(**(spec.get("mesh") or {}))
    anim = build_pin_animation(res, rig.joints, rig.width, rig.height, prov, mesh)
    anim.stage_image = "stage.png"
    anim.view["warnings"] = list(anim.view.get("warnings", [])) + assign_layers(job_dir, anim) + _frame_warnings(anim)
    anim.save(cand_dir / "pins.json")
    preview = render_preview(anim, job_dir, cand_dir / "preview.mp4", cand_dir / "_work", progress=progress)
    return {"pins": "pins.json", "preview": "preview.mp4", "preview_kind": preview["kind"],
            "frames": anim.num_frames, "fps": anim.fps, "view": anim.view, "retarget": ro}


# ------------------------------------------------------------------ running

def run_job(layout: Layout, job_dir: Path, guard, status: Status) -> None:
    spec = read_json(job_dir / "job.json")
    if not spec or spec.get("format") != JOB_FORMAT:
        raise ValueError("job.json geçersiz")
    profile = resolve_profile(spec.get("profile", "auto"))
    status.update(profile=profile.key)

    def report(stage: str, frac: Optional[float]) -> None:
        status.update(state="running", stage=stage, progress=frac)

    seeds = [int(s) for s in spec.get("seeds") or [secrets.randbelow(1_000_000)]]
    results = []
    if spec["kind"] == "pins":
        from .backends.hymotion import HYMotionBackend

        report("Sahne hazırlanıyor", None)
        prepare_stage(job_dir, spec)
        backend = HYMotionBackend(layout, profile, guard, report, spec.get("variant"))
        motions = backend.generate(spec["prompt"], float(spec.get("duration_s", 4.0)), seeds,
                                   cfg_scale=float(spec.get("cfg_scale", 5.0)), steps=spec.get("steps"),
                                   dit_dtype=spec.get("dit_dtype"))
        for m in motions:
            cand = job_dir / f"cand_{m.provenance.seed}"
            cand.mkdir(exist_ok=True)
            np.savez_compressed(cand / "motion.npz", keypoints3d=m.keypoints3d, fps=m.fps)
            write_json_atomic(cand / "provenance.json", m.provenance.to_dict())
            report(f"Önizleme (seed {m.provenance.seed})", None)
            info = make_pins(job_dir, cand, spec, m.keypoints3d, m.fps, m.provenance,
                             progress=lambda f: report("Önizleme", f))
            results.append({"seed": m.provenance.seed, "dir": cand.name, **info,
                            "label": m.provenance.label()})
    elif spec["kind"] == "frames":
        from PIL import Image

        from .backends.wan import WanTI2VBackend, chroma_key

        backend = WanTI2VBackend(layout, profile, guard, report)
        for seed in seeds:
            r = backend.generate(job_dir / "character.png", spec["prompt"], seed, steps=spec.get("steps"),
                                 allow_experimental=bool(spec.get("allow_experimental")),
                                 transformer_dtype=spec.get("dit_dtype"))
            cand = job_dir / f"cand_{seed}"
            frames_dir = cand / "frames"
            frames_dir.mkdir(parents=True, exist_ok=True)
            frames = chroma_key(r.frames) if spec.get("key_background", True) else r.frames
            mode = "RGBA" if frames.shape[-1] == 4 else "RGB"
            for i, f in enumerate(frames):
                Image.fromarray(f, mode).save(frames_dir / f"frame_{i:05d}.png", compress_level=1)
            write_json_atomic(cand / "provenance.json", r.provenance.to_dict())
            _frames_preview(r.frames, r.fps, cand / "preview.mp4")
            results.append({"seed": seed, "dir": cand.name, "frames": len(frames), "fps": r.fps,
                            "frames_dir": "frames", "preview": "preview.mp4", "preview_kind": "video",
                            "label": r.provenance.label()})
    else:
        raise ValueError(f"bilinmeyen iş türü: {spec['kind']}")
    status.update(state="done", stage="Bitti", progress=1.0, results=results,
                  guard={"pauses": guard.pauses, "waited_s": round(guard.waited_s, 1)})


def _frames_preview(frames: np.ndarray, fps: int, out: Path) -> None:
    import subprocess

    h, w = frames.shape[1:3]
    p = subprocess.Popen([find_ffmpeg(), "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24",
                          "-s", f"{w}x{h}", "-r", str(fps), "-i", "-", "-c:v", "libx264", "-crf", "18",
                          "-pix_fmt", "yuv420p", str(out)], stdin=subprocess.PIPE,
                         creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    p.stdin.write(np.ascontiguousarray(frames[..., :3]).tobytes())
    p.stdin.close()
    if p.wait() != 0:
        raise RuntimeError("önizleme MP4 yazılamadı")


def redo_pins(layout: Layout, job_dir: Path, seed: int, retarget_opts: dict) -> dict:
    """User changed view/root/foot-lock settings: new pins and preview from the same AI motion."""
    spec = read_json(job_dir / "job.json")
    cand = job_dir / f"cand_{int(seed)}"
    data = np.load(cand / "motion.npz")
    prov = Provenance.from_dict(read_json(cand / "provenance.json"))
    prov.edits.append({"type": "retarget", "options": retarget_opts, "at": time.time()})
    info = make_pins(job_dir, cand, spec, data["keypoints3d"], float(data["fps"]), prov, retarget_opts)
    write_json_atomic(cand / "provenance.json", prov.to_dict())
    st = Status(job_dir)
    results = [dict(r, **info) if r.get("seed") == int(seed) else r for r in st.data.get("results", [])]
    st.update(results=results)
    return info


# ------------------------------------------------------------------ VEGAS

def export_to_vegas(layout: Layout, job_dir: Path, seed: int, bake_frames: bool = False) -> Path:
    """Copies one candidate to <root>/outbox/<job>_<seed> and points outbox/latest.json at it."""
    spec = read_json(job_dir / "job.json")
    cand = job_dir / f"cand_{int(seed)}"
    prov = Provenance.from_dict(read_json(cand / "provenance.json"))
    dest = layout.outbox / f"{job_dir.name}_{int(seed)}"
    assert_under_root(layout, dest)
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    manifest = {"format": VEGAS_FORMAT, "kind": spec["kind"], "label": prov.label(), "provenance": prov.to_dict(),
                "job": job_dir.name, "seed": int(seed), "created": time.time()}
    shutil.copy2(cand / "preview.mp4", dest / "preview.mp4")
    manifest["preview"] = "preview.mp4"
    if spec["kind"] == "pins":
        anim = PinAnimation.load(cand / "pins.json")
        shutil.copy2(job_dir / "stage.png", dest / "stage.png")
        for layer in anim.layers:
            shutil.copy2(job_dir / layer["image"], dest / layer["image"])
        shutil.copy2(cand / "pins.json", dest / "pins.json")
        manifest.update(pins="pins.json", stage_image="stage.png", fps=anim.fps, frames=anim.num_frames,
                        width=anim.width, height=anim.height, duration_s=anim.duration_s)
        if bake_frames:
            bake_png_sequence(anim, job_dir, dest / "frames", cand / "_work")
            manifest.update(frames_dir="frames", frame_pattern="frame_%05d.png")
    else:
        shutil.copytree(cand / "frames", dest / "frames")
        n = len(list((dest / "frames").glob("frame_*.png")))
        fps = float(read_json(cand / "provenance.json")["params"]["fps"])
        manifest.update(frames_dir="frames", frame_pattern="frame_%05d.png", frames=n, fps=fps,
                        duration_s=n / fps)
    write_json_atomic(dest / "manifest.json", manifest)
    write_vegas_import(dest, manifest, PinAnimation.load(dest / "pins.json") if spec["kind"] == "pins" else None)
    write_json_atomic(layout.outbox / "latest.json", {"manifest": str(dest / "manifest.json"), "created": time.time()})
    tmp = layout.outbox / "latest.txt.tmp"
    tmp.write_text(str(dest) + "\n", encoding="utf-8")
    os.replace(tmp, layout.outbox / "latest.txt")
    return dest / "manifest.json"


def write_vegas_import(dest: Path, manifest: dict, anim: Optional[PinAnimation]) -> Path:
    """Line-based twin of manifest.json for the VEGAS script (no JSON parser needed in C# 5).

    One record per line, fields separated by single spaces, numbers with '.' as the
    decimal point; ``label`` keeps the rest of its line. See vegas/AIMotion_Import.cs.
    """
    prov = manifest["provenance"]
    lines = [
        "AIMOTION_VEGAS 1",
        f"kind {manifest['kind']}",
        f"source {prov['source']}",
        "label " + " ".join(manifest["label"].split()),
        f"fps {manifest['fps']:.6f}",
        f"frames {int(manifest['frames'])}",
        f"duration {manifest['duration_s']:.6f}",
    ]
    if anim is not None:
        m = anim.mesh
        lines += [
            f"size {anim.width} {anim.height}",
            f"stage {manifest['stage_image']}",
            f"mesh {int(m.use_alpha)} {m.alpha_threshold:.6f} {int(m.density)} {m.expansion:.6f} {int(m.rigidity)}",
        ]
        for p in anim.pins:
            lines.append(f"pin {p.number} {p.anchor[0]:.6f} {p.anchor[1]:.6f} {p.joint}")
        for layer in anim.layers:  # back to front
            lines.append(f"layer {','.join(str(n) for n in layer['pins']) or '-'} {layer['image']}")
        for p in anim.pins:
            lines += [f"key {p.number} {i} {u:.6f} {v:.6f}" for i, (u, v) in enumerate(p.track)]
    if manifest.get("frames_dir"):
        lines += [f"frames_dir {manifest['frames_dir']}", f"first_frame {manifest['frame_pattern'] % 0}"]
    lines.append("end")
    out = dest / "vegas_import.txt"
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return out
