"""Measures what the models really need on this PC and checks FP16.

    python -m aimotion.selftest [--profile auto|8gb|16gb|24gb] [--wan] [--wan-compare]

* HY-Motion: encodes a test prompt with the profile's text-encoder placement,
  then runs the DiT twice with the same seed, in FP32 and in FP16, and
  compares the two skeletons joint by joint (centimetres). Peak VRAM
  (torch.cuda.max_memory_allocated / reserved) and time of every phase are
  recorded.
* --wan: one short Wan2.2-TI2V-5B generation (25 frames, 10 steps) with the
  profile's settings for peak VRAM and a NaN check; --wan-compare also runs
  the transformer in bf16 and reports the pixel difference.

Results go to <root>/selftest.json; the UI shows them next to the official
and computed figures. Nothing is estimated here: if a step fails (out of
memory, missing weights), the failure is what gets written down.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
import traceback
from pathlib import Path

from .paths import apply_environment, resolve_layout

TEST_PROMPT = "A person walks forward at a normal pace, arms swinging naturally."


def _gib(b) -> float:
    return round(b / 1024**3, 2)


def run(profile_key: str, wan: bool, wan_compare: bool) -> dict:
    layout = resolve_layout()
    apply_environment(layout)
    import numpy as np

    from .backends.hymotion import HYMotionBackend
    from .guard import PlaybackGuard
    from .jobs import resolve_profile
    from .profiles import describe, detect_gpu

    gpu = detect_gpu()
    profile = resolve_profile(profile_key)
    report = {"date": dt.datetime.now().astimezone().isoformat(timespec="seconds"), "gpu": gpu,
              "profile": profile.key, "profile_plan": describe(profile), "hymotion": {}, "wan": {}}
    guard = PlaybackGuard(layout.vegas_state_file,
                          on_wait=lambda st: print(f"  VEGAS meşgul, bekleniyor: {st.detail}", flush=True))

    def say(stage, frac):
        if frac is None or frac >= 1.0:
            print(f"  {stage}", flush=True)

    hy = HYMotionBackend(layout, profile, guard, say)
    missing = hy.missing()
    if missing:
        report["hymotion"] = {"ok": False, "error": "eksik: " + "; ".join(missing)}
    else:
        runs = {}
        for dtype in ("fp32", "fp16"):
            print(f"HY-Motion DiT {dtype} ...", flush=True)
            try:
                res = hy.generate(TEST_PROMPT, 3.0, [1234], dit_dtype=dtype)[0]
                p = res.provenance
                runs[dtype] = res.keypoints3d
                report["hymotion"][dtype] = {
                    "ok": True, "timings_s": p.timings_s,
                    "peak_vram_gib": {k: {kk: _gib(vv) for kk, vv in v.items()} for k, v in p.peak_vram_bytes.items()},
                    "text_placement": p.params.get("text_placement"), "text_cache": p.params.get("text_cache"),
                    "dit_params": p.params.get("dit_params"), "dit_dtype": p.params.get("dit_dtype"),
                }
            except Exception as e:  # noqa: BLE001 - record any failure (OOM included)
                report["hymotion"][dtype] = {"ok": False, "error": f"{type(e).__name__}: {e}",
                                             "trace": traceback.format_exc(limit=3)}
        if "fp32" in runs and "fp16" in runs:
            d = np.linalg.norm(runs["fp16"] - runs["fp32"], axis=-1) * 100.0
            report["hymotion"]["fp16_vs_fp32_cm"] = {"max": round(float(d.max()), 3),
                                                     "mean": round(float(d.mean()), 3)}
            report["hymotion"]["fp16_ok"] = bool(d.max() < 2.0)

    if wan or wan_compare:
        from dataclasses import replace

        from .backends.wan import WanTI2VBackend

        short = replace(profile, wan=replace(profile.wan, num_frames=25, steps=10))
        img = Path(__file__).resolve().parent.parent / "examples" / "demo_front" / "character.png"
        frames = {}
        for dtype in (["fp16", "bf16"] if wan_compare else [profile.wan.transformer_dtype]):
            print(f"Wan2.2-TI2V-5B {dtype} ...", flush=True)
            try:
                r = WanTI2VBackend(layout, short, guard, say).generate(
                    img, TEST_PROMPT, 1234, allow_experimental=True, transformer_dtype=dtype)
                frames[dtype] = r.frames.astype(np.float32)
                report["wan"][dtype] = {
                    "ok": True, "timings_s": r.provenance.timings_s,
                    "peak_vram_gib": {k: {kk: _gib(vv) for kk, vv in v.items()}
                                      for k, v in r.provenance.peak_vram_bytes.items()},
                    "settings": {k: r.provenance.params[k] for k in ("width", "height", "num_frames", "steps",
                                                                       "offload")},
                }
            except Exception as e:  # noqa: BLE001
                report["wan"][dtype] = {"ok": False, "error": f"{type(e).__name__}: {e}"}
        if len(frames) == 2:
            report["wan"]["fp16_vs_bf16_mean_abs_8bit"] = round(float(np.abs(frames["fp16"] - frames["bf16"]).mean()), 2)

    layout.selftest_report.write_text(json.dumps(report, ensure_ascii=False, indent=1), encoding="utf-8")
    return report


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--profile", default="auto")
    ap.add_argument("--wan", action="store_true")
    ap.add_argument("--wan-compare", action="store_true")
    a = ap.parse_args(argv)
    rep = run(a.profile, a.wan, a.wan_compare)
    print(json.dumps(rep, ensure_ascii=False, indent=1))
    ok = rep["hymotion"].get("fp16", {}).get("ok", False)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
