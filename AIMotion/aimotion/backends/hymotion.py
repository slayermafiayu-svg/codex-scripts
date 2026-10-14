"""HY-Motion 1.0 (Tencent Hunyuan, 1.0B-parameter text-to-motion flow-matching DiT).

Runs the official code from github.com/Tencent-Hunyuan/HY-Motion-1.0 (cloned
to <root>/repos/HY-Motion-1.0) with the official weights, in two phases so it
fits the 8/16/24 GB profiles:

1. Text: CLIP-L + Qwen3-8B encode the prompt exactly as HY-Motion's
   HYTextModel does (same template, same layers, bf16). Placement follows the
   profile (all on the GPU, split GPU/RAM/disk with accelerate, or CPU). The
   result is cached per prompt on D:, the encoder is unloaded.
2. Motion: the DiT runs in FP16 on the GPU (weights converted to FP16, matmuls
   under FP16 autocast, output cast back to FP32 for the ODE solver). The
   guard is checked before every denoising step, so VEGAS playback pauses it.

What comes out is HY-Motion's own decoded skeleton: 22 SMPL-H body joints
(of 52) per frame at 30 fps, in metres, plus the per-seed provenance.

Nothing here falls back to anything: missing weights, a checkpoint that
leaves DiT parameters unset or missing normalisation statistics stop the job.
"""

from __future__ import annotations

import hashlib
import json
import os
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import numpy as np

from ..guard import PlaybackGuard
from ..paths import Layout
from ..profiles import Profile, detect_gpu
from ..provenance import AI_MODEL, Provenance, git_revision, sha256_file
from . import MissingModel, Report, free_cuda, measure

MODEL_ID = "tencent/HY-Motion-1.0"
LICENSE = "Tencent HY-Motion 1.0 Community License (AB, Birleşik Krallık ve Güney Kore hariç)"
OUTPUT_FPS = 30  # HY-Motion output_mesh_fps
MIN_FRAMES, MAX_FRAMES = 20, 360  # generate() clamps to these (train_frames = 360)


@dataclass
class MotionResult:
    keypoints3d: np.ndarray  # (T, 22, 3) metres, Y up
    fps: float
    provenance: Provenance


def _is_lfs_pointer(p: Path) -> bool:
    try:
        with open(p, "rb") as f:
            return f.read(40).startswith(b"version https://git-lfs")
    except OSError:
        return True


class HYMotionBackend:
    def __init__(self, layout: Layout, profile: Profile, guard: PlaybackGuard, report: Report,
                 variant: Optional[str] = None):
        self.layout = layout
        self.profile = profile
        self.guard = guard
        self.report = report
        self.variant = variant or profile.hymotion.dit_variant
        self.ckpt_dir = layout.hymotion_ckpts / self.variant

    # ------------------------------------------------------------------ checks

    def missing(self) -> list[str]:
        repo = self.layout.hymotion_repo
        out = []
        if not (repo / "hymotion" / "pipeline" / "motion_diffusion.py").is_file():
            out.append(f"HY-Motion kodu yok: {repo} (install.ps1 klonlar)")
        else:
            for rel in ("stats/Mean.npy", "stats/Std.npy", "scripts/gradio/static/assets/dump_wooden/j_template.bin",
                        "scripts/gradio/static/assets/dump_wooden/v_template.bin"):
                if _is_lfs_pointer(repo / rel):
                    out.append(f"{repo / rel} bir Git LFS işaretçisi; 'git lfs pull' çalıştırın")
        for f in ("config.yml", "latest.ckpt"):
            if not (self.ckpt_dir / f).is_file():
                out.append(f"{self.ckpt_dir / f} yok ({MODEL_ID} ağırlıkları)")
        if not (self.layout.qwen3_8b / "config.json").is_file():
            out.append(f"Qwen3-8B metin kodlayıcı yok: {self.layout.qwen3_8b}")
        if not (self.layout.clip_l / "config.json").is_file():
            out.append(f"CLIP ViT-L/14 metin kodlayıcı yok: {self.layout.clip_l}")
        return out

    def _require(self):
        missing = self.missing()
        if missing:
            raise MissingModel("HY-Motion çalıştırılamıyor:\n- " + "\n- ".join(missing))

    def _import(self):
        repo = str(self.layout.hymotion_repo)
        if repo not in sys.path:
            sys.path.insert(0, repo)
        # HY-Motion resolves its body model and statistics relative to the working directory.
        os.chdir(repo)
        from hymotion.network.text_encoders import model_constants, text_encoder
        from hymotion.utils.loaders import load_object

        return load_object, text_encoder, model_constants

    # -------------------------------------------------------------- phase 1

    def _text_placement(self, gpu: dict) -> tuple[str, dict, str]:
        plan = self.profile.hymotion
        placement = plan.text_placement
        if not gpu.get("available"):
            placement = "cpu"
        elif not gpu.get("bf16") and placement != "cpu":
            # Official weights are bf16; GPUs without bf16 (Turing) run the encoder on the CPU.
            placement = "cpu"
        ram_gib = _available_ram_gib()
        cpu_budget = max(4, int(ram_gib - 6))  # leave room for VEGAS and the OS
        if placement == "gpu":
            return placement, {"device_map": {"": 0}}, "cuda:0"
        if placement == "split":
            return placement, {
                "device_map": "auto",
                "max_memory": {0: f"{plan.text_gpu_budget_gib}GiB", "cpu": f"{cpu_budget}GiB"},
                "offload_folder": str(self.layout.offload),
            }, "cuda:0"
        if ram_gib - 6 < 17:  # bf16 Qwen3-8B body is ~15 GB; spill the rest to D:
            return "cpu+disk", {"device_map": "auto", "max_memory": {"cpu": f"{cpu_budget}GiB"},
                                "offload_folder": str(self.layout.offload)}, "cpu"
        return placement, {"device_map": {"": "cpu"}}, "cpu"

    def encode_prompt(self, prompt: str, gpu: dict, timings: dict, peaks: dict, info: dict):
        import torch

        load_object, te, mc = self._import()
        cfg = _read_config(self.ckpt_dir / "config.yml")
        args = cfg.get("train_pipeline_args") or {}
        te_module, te_cfg = args.get("text_encoder_module"), args.get("text_encoder_cfg")
        if not te_module or te_cfg is None:
            raise MissingModel(f"{self.ckpt_dir / 'config.yml'} metin kodlayıcı ayarlarını içermiyor")
        te_cfg = dict(te_cfg)
        key_src = json.dumps({
            "prompt": prompt, "template": mc.PROMPT_TEMPLATE_ENCODE_HUMAN_MOTION, "cfg": te_cfg,
            "qwen": _dir_fingerprint(self.layout.qwen3_8b), "clip": _dir_fingerprint(self.layout.clip_l),
        }, sort_keys=True)
        key = hashlib.sha256(key_src.encode("utf-8")).hexdigest()[:32]
        cache = self.layout.text_embeddings / f"{key}.pt"
        if cache.is_file():
            info["text_cache"] = "hit"
            self.report("Metin kodlaması önbellekten alındı", None)
            return torch.load(cache, map_location="cpu", weights_only=True)

        self.guard.checkpoint()
        placement, llm_kwargs, clip_device = self._text_placement(gpu)
        info["text_placement"] = placement
        self.report(f"Metin kodlayıcı yükleniyor (Qwen3-8B bf16, {placement})", None)

        class _LLM:  # loads Qwen3-8B where the profile says; no LM head (only hidden states are used)
            @classmethod
            def from_pretrained(cls, path, **kw):
                from transformers import AutoModel

                kw.update(llm_kwargs)
                return AutoModel.from_pretrained(path, **kw)

        class _CLIP:
            @classmethod
            def from_pretrained(cls, path, **kw):
                from transformers import CLIPTextModel

                return CLIPTextModel.from_pretrained(path, **kw).to(clip_device)

        saved = (dict(te.LLM_ENCODER_LAYOUT["qwen3"]), dict(te.SENTENCE_EMB_LAYOUT["clipl"]))
        te.LLM_ENCODER_LAYOUT["qwen3"].update(module_path=str(self.layout.qwen3_8b), text_encoder_class=_LLM)
        te.SENTENCE_EMB_LAYOUT["clipl"].update(module_path=str(self.layout.clip_l), text_encoder_class=_CLIP)
        try:
            with measure("text_encoder", timings, peaks):
                model = load_object(te_module, te_cfg)
                self.guard.checkpoint()
                self.report("Metin kodlanıyor", None)
                with torch.no_grad():
                    vtxt, ctxt, length = model.encode([prompt])
                out = {
                    "text_vec_raw": vtxt.detach().float().cpu(),
                    "text_ctxt_raw": ctxt.detach().float().cpu(),
                    "text_ctxt_raw_length": length.detach().cpu(),
                }
                del model
        finally:
            te.LLM_ENCODER_LAYOUT["qwen3"].clear()
            te.LLM_ENCODER_LAYOUT["qwen3"].update(saved[0])
            te.SENTENCE_EMB_LAYOUT["clipl"].clear()
            te.SENTENCE_EMB_LAYOUT["clipl"].update(saved[1])
            free_cuda()
        for k, v in out.items():
            if v.is_floating_point() and not torch.isfinite(v).all():
                raise RuntimeError(f"Metin kodlaması geçersiz değer üretti ({k})")
        torch.save(out, cache)
        info["text_cache"] = "miss"
        return out

    # -------------------------------------------------------------- phase 2

    def load_dit(self, device: str, dtype: str, info: dict):
        import torch

        load_object, _, _ = self._import()
        cfg = _read_config(self.ckpt_dir / "config.yml")
        pipeline = load_object(cfg["train_pipeline"], cfg["train_pipeline_args"],
                               network_module=cfg["network_module"],
                               network_module_args=cfg["network_module_args"])
        if tuple(pipeline.mean.shape) != (201,):
            # HY-Motion silently falls back to a blank mean/std when stats/ is missing.
            raise RuntimeError("HY-Motion normalizasyon istatistikleri (stats/Mean.npy, Std.npy) yüklenmedi")
        state = torch.load(self.ckpt_dir / "latest.ckpt", map_location="cpu", weights_only=False)
        result = pipeline.load_state_dict(state["model_state_dict"], strict=False)
        del state
        unset = [k for k in result.missing_keys if not k.startswith("text_encoder.")]
        if unset:
            raise RuntimeError(f"Kontrol noktası {len(unset)} DiT parametresini doldurmadı (ör. {unset[:3]})")
        if tuple(pipeline.mean.shape) != (201,) or float(pipeline.std.abs().sum()) == 0.0:
            raise RuntimeError("HY-Motion normalizasyon istatistikleri (stats/Mean.npy, Std.npy) yüklenmedi")
        pipeline.eval().to(device)
        mt = pipeline.motion_transformer
        if dtype == "fp16" and device.startswith("cuda"):
            mt.half()
        info["dit_params"] = int(sum(p.numel() for p in mt.parameters()))
        info["dit_dtype"] = str(next(mt.parameters()).dtype).replace("torch.", "")
        return pipeline

    def _instrument(self, pipeline, steps: int, fp16: bool):
        """Guard + progress before every DiT call; FP16 autocast inside, FP32 out."""
        import torch

        mt = pipeline.motion_transformer
        orig = mt.forward
        state = {"calls": 0}

        def forward(*args, **kwargs):
            self.guard.checkpoint()
            state["calls"] += 1
            self.report("Hareket üretiliyor", min(1.0, state["calls"] / steps))
            if fp16:
                with torch.autocast("cuda", dtype=torch.float16):
                    out = orig(*args, **kwargs)
            else:
                out = orig(*args, **kwargs)
            if not torch.isfinite(out).all():
                raise FloatingPointError("DiT çıktısında NaN/Inf (FP16 taşması olabilir; profilde fp32 deneyin)")
            return out.float()

        mt.forward = forward
        return state

    # ------------------------------------------------------------------ run

    def generate(self, prompt: str, duration_s: float, seeds: list[int], cfg_scale: float = 5.0,
                 steps: Optional[int] = None, dit_dtype: Optional[str] = None) -> list[MotionResult]:
        prompt = " ".join(prompt.split())
        if not prompt:
            raise ValueError("Hareket tarifi boş")
        if not seeds:
            raise ValueError("En az bir seed gerekli")
        self._require()
        import torch

        gpu = detect_gpu()
        if not gpu.get("available"):
            raise MissingModel("CUDA GPU bulunamadı: HY-Motion DiT bu araçta yalnızca NVIDIA GPU'da çalıştırılır")
        timings: dict = {}
        peaks: dict = {}
        info: dict = {}
        t_start = time.perf_counter()

        frames = int(round(duration_s * OUTPUT_FPS))
        if not MIN_FRAMES <= frames <= MAX_FRAMES:
            raise ValueError(f"Süre {MIN_FRAMES / OUTPUT_FPS:.2f}-{MAX_FRAMES / OUTPUT_FPS:.0f} sn aralığında olmalı")

        hidden = self.encode_prompt(prompt, gpu, timings, peaks, info)

        self.guard.checkpoint()
        dtype = dit_dtype or self.profile.hymotion.dit_dtype
        self.report(f"DiT yükleniyor ({self.variant}, {dtype})", None)
        with measure("dit_load", timings, peaks):
            pipeline = self.load_dit("cuda", dtype, info)
        if steps:
            pipeline.validation_steps = int(steps)
        n_steps = int(pipeline.validation_steps)
        self._instrument(pipeline, n_steps, fp16=(dtype == "fp16"))

        n = len(seeds)
        hsd = {
            "text_vec_raw": hidden["text_vec_raw"].repeat(n, 1, 1).cuda(),
            "text_ctxt_raw": hidden["text_ctxt_raw"].repeat(n, 1, 1).cuda(),
            "text_ctxt_raw_length": hidden["text_ctxt_raw_length"].repeat(n).cuda(),
        }
        try:
            with measure("dit_generate", timings, peaks), torch.no_grad():
                out = pipeline.generate(prompt, list(seeds), duration_s, cfg_scale=cfg_scale,
                                        hidden_state_dict=hsd, length=frames)
        finally:
            del pipeline
            free_cuda()
        k3d = out["keypoints3d"].numpy().astype(np.float32)  # (B, L, 52, 3)
        if not np.isfinite(k3d).all():
            raise FloatingPointError("Üretilen iskelette NaN/Inf var")
        timings["total"] = round(time.perf_counter() - t_start, 3)

        ckpt_hash = sha256_file(self.ckpt_dir / "latest.ckpt", self.layout.cache / "hashes.json")
        cfg_hash = sha256_file(self.ckpt_dir / "config.yml")
        results = []
        for i, seed in enumerate(seeds):
            prov = Provenance(
                source=AI_MODEL,
                model=f"{MODEL_ID} ({self.variant})",
                checkpoint_sha256=ckpt_hash,
                code_revision=git_revision(self.layout.hymotion_repo),
                license=LICENSE,
                prompt=prompt,
                seed=int(seed),
                params={
                    "duration_s": duration_s, "frames": int(k3d.shape[1]), "fps": OUTPUT_FPS,
                    "cfg_scale": cfg_scale, "steps": n_steps, "candidates": n, "config_sha256": cfg_hash,
                    "profile": self.profile.key, **info,
                },
                device=gpu,
                timings_s=dict(timings),
                peak_vram_bytes=dict(peaks),
            ).validate()
            results.append(MotionResult(k3d[i, :, :22, :].copy(), OUTPUT_FPS, prov))
        return results


def _read_config(path: Path) -> dict:
    import yaml

    with open(path, "r", encoding="utf-8") as f:
        return yaml.load(f, Loader=yaml.FullLoader)  # same loader as HY-Motion's own runtime


def _dir_fingerprint(d: Path) -> str:
    """Identifies a model folder by its config and weight file sizes (cheap, no hashing of 16 GB)."""
    parts = []
    for p in sorted(Path(d).glob("*")):
        if p.suffix in (".json", ".safetensors", ".bin", ".txt", ".model"):
            parts.append(f"{p.name}:{p.stat().st_size}")
    return hashlib.sha256("|".join(parts).encode()).hexdigest()[:16]


def _available_ram_gib() -> float:
    try:
        import psutil

        return psutil.virtual_memory().available / 1024**3
    except ImportError:
        pass
    if os.name == "nt":
        import ctypes

        class MEMORYSTATUSEX(ctypes.Structure):
            _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong),
                        ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
                        ("ullTotalPageFile", ctypes.c_ulonglong), ("ullAvailPageFile", ctypes.c_ulonglong),
                        ("ullTotalVirtual", ctypes.c_ulonglong), ("ullAvailVirtual", ctypes.c_ulonglong),
                        ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]

        st = MEMORYSTATUSEX()
        st.dwLength = ctypes.sizeof(MEMORYSTATUSEX)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(st))
        return st.ullAvailPhys / 1024**3
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemAvailable:"):
                    return int(line.split()[1]) / 1024**2
    except OSError:
        pass
    return 16.0
