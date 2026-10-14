"""Wan2.2-TI2V-5B (Alibaba Wan, 5B dense, Apache-2.0): image + text -> video frames.

Used for the "frames" output: motions a single warped picture cannot show
(turning around, new body sides, cloth, hair). Runs the Diffusers version
(Wan-AI/Wan2.2-TI2V-5B-Diffusers) with WanImageToVideoPipeline:

* umT5 text encoder: bf16 on the CPU (like the official --t5_cpu), encoded
  once, then removed from the pipeline so it never takes VRAM;
* transformer: FP16 (official: bf16; the self-test compares them on your GPU)
  with model CPU offload (16/24 GB) or sequential offload (8 GB, experimental);
* VAE: FP32 with tiling.

The character keeps its look only as far as the model keeps it: identity,
clothes and proportions can drift over 5 s. The result is pixels, not pins:
it cannot be edited with the Puppet Pin tools afterwards.
"""

from __future__ import annotations

import hashlib
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import numpy as np
from PIL import Image

from ..guard import PlaybackGuard
from ..paths import Layout
from ..profiles import Profile, detect_gpu
from ..provenance import AI_MODEL, Provenance, sha256_file
from . import MissingModel, Report, free_cuda, measure

MODEL_ID = "Wan-AI/Wan2.2-TI2V-5B-Diffusers"
LICENSE = "Apache-2.0"
DEFAULT_NEGATIVE = (
    "camera movement, zoom, background change, scene change, different person, extra limbs, "
    "deformed hands, blurry, low quality, flicker, text, watermark"
)


@dataclass
class FramesResult:
    frames: np.ndarray  # (N, H, W, 3) uint8
    fps: int
    provenance: Provenance


def prepare_input_image(path: Path, width: int, height: int, background=(0, 177, 64)) -> Image.Image:
    """Fits the character into width x height on a flat background (green by default for keying)."""
    im = Image.open(path).convert("RGBA")
    s = min(width / im.width, height / im.height) * 0.9
    w, h = max(1, round(im.width * s)), max(1, round(im.height * s))
    canvas = Image.new("RGBA", (width, height), tuple(background) + (255,))
    canvas.alpha_composite(im.resize((w, h), Image.LANCZOS), ((width - w) // 2, height - h - height // 20))
    return canvas.convert("RGB")


def chroma_key(frames: np.ndarray, key=(0, 177, 64), tol: float = 60.0, soft: float = 40.0) -> np.ndarray:
    """Simple distance key -> RGBA frames (alpha 0 near the key colour)."""
    f = frames.astype(np.float32)
    d = np.linalg.norm(f - np.array(key, np.float32), axis=-1)
    alpha = np.clip((d - tol) / soft, 0.0, 1.0)
    # remove green spill on semi-transparent edges
    g = f[..., 1]
    limit = np.maximum(f[..., 0], f[..., 2])
    f[..., 1] = np.where(alpha < 1.0, np.minimum(g, limit), g)
    out = np.concatenate([f, (alpha * 255.0)[..., None]], axis=-1)
    return np.clip(out + 0.5, 0, 255).astype(np.uint8)


class WanTI2VBackend:
    def __init__(self, layout: Layout, profile: Profile, guard: PlaybackGuard, report: Report):
        self.layout = layout
        self.profile = profile
        self.guard = guard
        self.report = report
        self.path = layout.wan_ti2v_5b

    def missing(self) -> list[str]:
        out = []
        for sub in ("model_index.json", "transformer", "vae", "text_encoder", "tokenizer", "scheduler"):
            if not (self.path / sub).exists():
                out.append(f"{self.path / sub} yok ({MODEL_ID})")
        return out

    def generate(self, image_path: Path, prompt: str, seed: int, negative_prompt: str = DEFAULT_NEGATIVE,
                 steps: Optional[int] = None, guidance: float = 5.0, allow_experimental: bool = False,
                 transformer_dtype: Optional[str] = None) -> FramesResult:
        plan = self.profile.wan
        if not plan.enabled and not allow_experimental:
            raise MissingModel(f"Kare modu {self.profile.label} profilinde kapalı (resmi destek 24 GB)")
        missing = self.missing()
        if missing:
            raise MissingModel("Wan2.2-TI2V-5B çalıştırılamıyor:\n- " + "\n- ".join(missing))
        import torch
        from diffusers import AutoencoderKLWan, WanImageToVideoPipeline

        gpu = detect_gpu()
        if not gpu.get("available"):
            raise MissingModel("CUDA GPU bulunamadı")
        prompt = " ".join(prompt.split())
        if not prompt:
            raise ValueError("Hareket tarifi boş")
        tdtype = {"fp16": torch.float16, "bf16": torch.bfloat16}[transformer_dtype or plan.transformer_dtype]
        steps = int(steps or plan.steps)
        timings: dict = {}
        peaks: dict = {}
        t0 = time.perf_counter()

        self.guard.checkpoint()
        self.report("Wan2.2-TI2V-5B yükleniyor", None)
        with measure("load", timings, peaks):
            vae = AutoencoderKLWan.from_pretrained(self.path, subfolder="vae", torch_dtype=torch.float32)
            pipe = WanImageToVideoPipeline.from_pretrained(self.path, vae=vae, torch_dtype=tdtype)
            if not pipe.config.get("expand_timesteps", False):
                raise MissingModel(f"{self.path} TI2V-5B değil (expand_timesteps yok)")
            pipe.text_encoder.to("cpu", dtype=torch.bfloat16)

        self.guard.checkpoint()
        self.report("Metin kodlanıyor (umT5, CPU)", None)
        with measure("text_encoder", timings, peaks), torch.no_grad():
            pe, ne = pipe.encode_prompt(prompt=prompt, negative_prompt=negative_prompt,
                                        do_classifier_free_guidance=guidance > 1.0, max_sequence_length=512,
                                        device=torch.device("cpu"), dtype=torch.bfloat16)
        pipe.register_modules(text_encoder=None)
        free_cuda()

        if plan.offload == "sequential":
            pipe.enable_sequential_cpu_offload()
        else:
            pipe.enable_model_cpu_offload()
        if plan.vae_tiling:
            pipe.vae.enable_tiling()

        image = prepare_input_image(image_path, plan.width, plan.height)
        image.save(self.layout.tmp / f"wan_input_{seed}.png")

        def on_step(p, i, t, kwargs):
            self.report("Kareler üretiliyor", (i + 1) / steps)
            self.guard.checkpoint()
            return kwargs

        self.guard.checkpoint()
        generator = torch.Generator(device="cpu").manual_seed(int(seed))
        with measure("generate", timings, peaks), torch.no_grad():
            out = pipe(
                image=image,
                prompt_embeds=pe.to(tdtype),
                negative_prompt_embeds=None if ne is None else ne.to(tdtype),
                height=plan.height,
                width=plan.width,
                num_frames=plan.num_frames,
                num_inference_steps=steps,
                guidance_scale=guidance,
                generator=generator,
                output_type="np",
                callback_on_step_end=on_step,
            )
        frames = np.asarray(out.frames[0])
        del pipe, vae, out
        free_cuda()
        if not np.isfinite(frames).all() or float(frames.max() - frames.min()) < 1e-3:
            raise FloatingPointError("Kareler boş/NaN çıktı (FP16 taşması olabilir; profilde bf16 seçin)")
        frames = (np.clip(frames, 0, 1) * 255 + 0.5).astype(np.uint8)
        timings["total"] = round(time.perf_counter() - t0, 3)

        # Identity of the transformer weights: hash of the per-shard SHA-256s (cached after the first run).
        shards = sorted((self.path / "transformer").glob("*.safetensors"))
        cache = self.layout.cache / "hashes.json"
        ckpt_hash = hashlib.sha256("|".join(sha256_file(s, cache) for s in shards).encode()).hexdigest()
        prov = Provenance(
            source=AI_MODEL, model=MODEL_ID, checkpoint_sha256=ckpt_hash, license=LICENSE, prompt=prompt,
            seed=int(seed),
            params={"negative_prompt": negative_prompt, "steps": steps, "guidance": guidance,
                    "width": plan.width, "height": plan.height, "num_frames": plan.num_frames, "fps": plan.fps,
                    "transformer_dtype": str(tdtype).replace("torch.", ""), "offload": plan.offload,
                    "profile": self.profile.key, "transformer_shards": [s.name for s in shards]},
            device=gpu, timings_s=timings, peak_vram_bytes=peaks,
        ).validate()
        return FramesResult(frames, plan.fps, prov)
