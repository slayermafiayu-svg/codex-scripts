"""Command line: the same pipeline as the UI, for scripted or first runs.

    python -m aimotion.cli doctor
    python -m aimotion.cli pins   --image char.png --rig rig.json --prompt "A person walks forward." \\
                                  [--duration 4] [--seeds 11,22] [--stage 1920x1080@29.97] [--view auto|0|90|-90]
                                  [--layers 00.png 01.png ...] [--profile auto] [--export]
    python -m aimotion.cli frames --image char.png --prompt "..." [--seeds 7] [--export]

``pins``/``frames`` create a job under <root>/jobs, run the worker (a separate
process, paused while VEGAS plays) and print where the results are, with the
provenance of each candidate. ``--export`` hands the first candidate to VEGAS
(outbox/latest.txt), ready for "AI Motion: Import Latest Result".
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

from .paths import apply_environment, resolve_layout

PKG_ROOT = Path(__file__).resolve().parent.parent


def _parse_stage(s: str) -> dict:
    size, _, fps = s.partition("@")
    w, h = size.lower().split("x")
    return {"width": int(w), "height": int(h), "fps": float(fps or 29.97)}


def doctor(layout) -> int:
    from .backends.hymotion import HYMotionBackend
    from .backends.wan import WanTI2VBackend
    from .guard import read_vegas_state
    from .jobs import resolve_profile
    from .preview import find_ffmpeg, find_puppet_render
    from .profiles import detect_gpu

    gpu = detect_gpu()
    prof = resolve_profile("auto")
    print(f"Kök: {layout.root}")
    print("GPU: " + (f"{gpu['name']}, {gpu['total_bytes'] / 1024**3:.1f} GB, bf16={gpu['bf16']}, torch {gpu['torch']}"
                     f" / CUDA {gpu['cuda']}" if gpu.get("available") else f"yok ({gpu.get('reason')})"))
    print(f"Profil: {prof.label}")
    vs = read_vegas_state(layout.vegas_state_file)
    print(f"VEGAS: {vs.state} - {vs.detail}" + (f" - proje {vs.project}" if vs.project else ""))
    print(f"puppet_render: {find_puppet_render() or 'YOK (önizleme yalnızca iskelet)'}")
    try:
        print(f"ffmpeg: {find_ffmpeg()}")
    except Exception as e:  # noqa: BLE001
        print(f"ffmpeg: YOK ({e})")
    ok = True
    for name, missing in (("HY-Motion (pin modu)", HYMotionBackend(layout, prof, None, None).missing()),
                          ("Wan2.2-TI2V-5B (kare modu)", WanTI2VBackend(layout, prof, None, None).missing())):
        print(f"{name}: " + ("hazır" if not missing else "eksik"))
        for m in missing:
            print(f"   - {m}")
        ok = ok and (bool(missing) is False or name.startswith("Wan"))
    return 0 if ok else 1


def run(layout, kind: str, a) -> int:
    from .jobs import JOB_FORMAT, export_to_vegas, new_job_id, read_json, write_json_atomic
    from .skeleton import load_rig_file
    from PIL import Image

    job_id = new_job_id()
    d = layout.job_dir(job_id)
    d.mkdir(parents=True)
    Image.open(a.image).convert("RGBA").save(d / "character.png")
    if a.mask:
        Image.open(a.mask).convert("L").save(d / "mask.png")
    if getattr(a, "layers", None):
        (d / "layers").mkdir()
        for i, p in enumerate(a.layers):
            shutil.copy2(p, d / "layers" / f"{i:02d}_{Path(p).stem}.png")
    seeds = [int(s) for s in a.seeds.split(",")] if a.seeds else [int(time.time()) % 1_000_000]
    spec = {"format": JOB_FORMAT, "kind": kind, "prompt": a.prompt, "duration_s": a.duration, "seeds": seeds,
            "cfg_scale": a.cfg, "steps": a.steps, "profile": a.profile,
            "stage": dict(_parse_stage(a.stage), center_x=a.center_x, char_height=a.height),
            "retarget": {"view_yaw_deg": None if a.view == "auto" else float(a.view)},
            "unknown_vegas_policy": "allow" if a.ignore_unknown_vegas else "block",
            "allow_experimental": a.experimental}
    if kind == "pins":
        w, h = Image.open(d / "character.png").size
        rig = load_rig_file(Path(a.rig), w, h)
        missing = rig.missing_required()
        if missing:
            print("Eksik eklemler: " + ", ".join(missing), file=sys.stderr)
            return 2
        write_json_atomic(d / "rig_src.json", rig.to_dict())
    write_json_atomic(d / "job.json", spec)
    print(f"İş: {d}")
    proc = subprocess.Popen([sys.executable, "-m", "aimotion.worker", str(d)], cwd=str(PKG_ROOT),
                            env=dict(os.environ, AIMOTION_ROOT=str(layout.root), PYTHONUTF8="1",
                                     PYTHONIOENCODING="utf-8"))
    last = None
    try:
        while proc.poll() is None:
            st = read_json(d / "status.json", {}) or {}
            line = f"[{st.get('state', '?')}] {st.get('stage', '')}" + (
                f" {st['progress'] * 100:.0f}%" if st.get("progress") is not None else "")
            if line != last:
                print(line, flush=True)
                last = line
            time.sleep(0.5)
    except KeyboardInterrupt:
        (d / "cancel").write_text("1")
        proc.wait()
    st = read_json(d / "status.json", {}) or {}
    if st.get("state") != "done":
        print(f"Bitmedi: {st.get('state')} {st.get('error') or ''}", file=sys.stderr)
        log = d / "error.log"
        if log.is_file():
            print(log.read_text(encoding="utf-8"), file=sys.stderr)
        return 1
    for r in st["results"]:
        prov = read_json(d / r["dir"] / "provenance.json")
        print(f"\nAday seed={r['seed']}: {d / r['dir'] / r['preview']}")
        print(f"  {prov['model']} | ckpt sha256 {prov['checkpoint_sha256'][:16]}… | {prov['license']}")
        print(f"  süreler: {json.dumps(prov['timings_s'])}")
        peaks = {k: round(v['allocated'] / 1024**3, 2) for k, v in (prov.get('peak_vram_bytes') or {}).items()}
        print(f"  tepe VRAM (GB, torch allocated): {peaks}")
    if a.export:
        m = export_to_vegas(layout, d, st["results"][0]["seed"])
        print(f"\nVEGAS'a hazır: {m}\nVEGAS: Tools > Extensions > AI Motion: Import Latest Result")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="AI Motion komut satırı")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("doctor", help="kurulumu ve GPU'yu kontrol et")
    for kind in ("pins", "frames"):
        p = sub.add_parser(kind)
        p.add_argument("--image", required=True)
        p.add_argument("--mask")
        p.add_argument("--prompt", required=True)
        p.add_argument("--seeds", default="")
        p.add_argument("--profile", default="auto")
        p.add_argument("--steps", type=int)
        p.add_argument("--cfg", type=float, default=5.0)
        p.add_argument("--duration", type=float, default=4.0)
        p.add_argument("--stage", default="1920x1080@29.97")
        p.add_argument("--center-x", type=float, default=0.5, help="karakterin başlangıçtaki yatay konumu (0-1)")
        p.add_argument("--height", type=float, default=0.72, help="karakter yüksekliği / kare yüksekliği")
        p.add_argument("--export", action="store_true")
        p.add_argument("--experimental", action="store_true", help="kare modunu profil kapalı olsa da dene")
        p.add_argument("--ignore-unknown-vegas", action="store_true",
                       help="VEGAS köprü eklentisi yokken de çalış (VEGAS açıksa önerilmez)")
        if kind == "pins":
            p.add_argument("--rig", required=True, help="rig.json / DWPose-OpenPose JSON")
            p.add_argument("--view", default="auto")
            p.add_argument("--layers", nargs="*")
    a = ap.parse_args(argv)
    layout = resolve_layout()
    apply_environment(layout)
    if a.cmd == "doctor":
        return doctor(layout)
    if a.cmd == "frames":
        a.view, a.rig = "auto", None
    return run(layout, a.cmd, a)


if __name__ == "__main__":
    sys.exit(main())
