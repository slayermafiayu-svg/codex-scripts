"""VRAM profiles (8 / 16 / 24 GB) and what each one runs where.

Every number below says where it comes from:

  official  - published by the model authors (README tables)
  computed  - parameter count x bytes per parameter (weights only, no activations)
  measured  - written by ``python -m aimotion.selftest`` on your GPU into
              selftest.json; the UI shows measured values when they exist

Components and dtypes
---------------------
* HY-Motion-1.0 motion DiT (1.0B params): FP16 weights + FP16 autocast on the
  GPU in every profile. Official code runs it in FP32; FP16 is checked against
  FP32 by the self-test on your card.
* Qwen3-8B text encoder (HY-Motion's LLM encoder): bf16, as the official code
  loads it. It runs once per new prompt (results are cached on D:) and is
  unloaded before the DiT starts. Where it does not fit in VRAM it is split
  between GPU, RAM and (if RAM runs out) D: disk with accelerate - same bf16
  weights, same result, just slower. GPUs without bf16 (RTX 20xx) run it on
  the CPU.
* Wan2.2-TI2V-5B (frames mode): transformer FP16, VAE FP32, umT5 encoder bf16
  on the CPU (like the official ``--t5_cpu``). Official peak on an RTX 4090:
  22.8 GB at 720P with --offload_model --convert_model_dtype --t5_cpu.
"""

from __future__ import annotations

import json
from dataclasses import asdict, dataclass, field
from pathlib import Path

GIB = 1024**3


@dataclass(frozen=True)
class MemoryFigure:
    gb: float
    basis: str  # official / computed / measured
    note: str = ""


@dataclass(frozen=True)
class HYMotionPlan:
    dit_variant: str = "HY-Motion-1.0"  # or "HY-Motion-1.0-Lite"
    dit_dtype: str = "fp16"
    text_placement: str = "gpu"  # gpu | split | cpu
    text_gpu_budget_gib: float = 0.0  # for split
    candidates: int = 2  # seeds generated per request


@dataclass(frozen=True)
class WanPlan:
    enabled: bool = False
    experimental: bool = False
    offload: str = "model"  # model | sequential
    transformer_dtype: str = "fp16"
    width: int = 1280
    height: int = 704
    num_frames: int = 121  # 4k + 1; 121 frames = 5.04 s at 24 fps
    fps: int = 24
    steps: int = 50
    vae_tiling: bool = True


@dataclass(frozen=True)
class Profile:
    key: str
    label: str
    vram_gb: int
    hymotion: HYMotionPlan
    wan: WanPlan
    expected: dict = field(default_factory=dict)  # name -> MemoryFigure
    notes: tuple[str, ...] = ()


# Weights-only sizes from parameter counts (FP16/bf16 = 2 bytes per parameter).
DIT_FP16 = MemoryFigure(2.0, "computed", "HY-Motion-1.0: 1.0B parametre x 2 bayt")
DIT_LITE_FP16 = MemoryFigure(0.92, "computed", "HY-Motion-1.0-Lite: 0.46B x 2 bayt")
QWEN_BF16 = MemoryFigure(
    15.1, "computed",
    "Qwen3-8B gövdesi (lm_head yüklenmez), ~7.6B parametre x 2 bayt; tam model ~16.4 GB",
)
HYMOTION_OFFICIAL = MemoryFigure(
    26.0, "official", "HY-Motion README: HY-Motion-1.0 için en az 26 GB (Lite: 24 GB), "
    "Qwen3-8B bf16 ve DiT FP32 aynı anda GPU'da"
)
WAN5B_OFFICIAL = MemoryFigure(
    22.8, "official", "Wan2.2 verimlilik tablosu: TI2V-5B I2V 720P, tek RTX 4090, 524.8 s"
)

PROFILES: dict[str, Profile] = {
    "8gb": Profile(
        key="8gb",
        label="8 GB (ör. RTX 3060 Ti / 4060)",
        vram_gb=8,
        hymotion=HYMotionPlan(text_placement="split", text_gpu_budget_gib=5.0, candidates=1),
        wan=WanPlan(enabled=False, experimental=True, offload="sequential", width=832, height=480, num_frames=49),
        expected={"dit": DIT_FP16, "text_encoder": QWEN_BF16, "official_full": HYMOTION_OFFICIAL},
        notes=(
            "Metin kodlayıcı (Qwen3-8B) 5 GB GPU + RAM (+ gerekirse D: diski) arasında bölünür; "
            "her yeni komutta bir kez çalışır ve önbelleğe alınır.",
            "Kare (video) modu bu profilde kapalı: Wan2.2-TI2V-5B için resmi destek 24 GB. "
            "Ayarlardan 'deneysel' olarak açılabilir (katman katman offload, çok yavaş).",
            "32 GB sistem RAM önerilir.",
        ),
    ),
    "16gb": Profile(
        key="16gb",
        label="16 GB (ör. RTX 4060 Ti 16G / 4080 / 5070 Ti)",
        vram_gb=16,
        hymotion=HYMotionPlan(text_placement="split", text_gpu_budget_gib=12.0, candidates=2),
        wan=WanPlan(enabled=True, experimental=True, offload="model", width=960, height=544, num_frames=81),
        expected={"dit": DIT_FP16, "text_encoder": QWEN_BF16, "wan_official_4090": WAN5B_OFFICIAL},
        notes=(
            "Metin kodlayıcı 12 GB GPU + RAM arasında bölünür.",
            "Kare modu: resmi ölçüm (720P, 121 kare) 22.8 GB; bu profil 960x544, 81 kare (3.4 sn) ve "
            "model offload + VAE tiling kullanır. Sığdığı tahmin; kartınızda self-test ile doğrulayın.",
        ),
    ),
    "24gb": Profile(
        key="24gb",
        label="24 GB (ör. RTX 3090 / 4090)",
        vram_gb=24,
        hymotion=HYMotionPlan(text_placement="gpu", candidates=4),
        wan=WanPlan(enabled=True, offload="model", width=1280, height=704),
        expected={"dit": DIT_FP16, "text_encoder": QWEN_BF16, "wan_official_4090": WAN5B_OFFICIAL},
        notes=(
            "Qwen3-8B tamamen GPU'da (bf16) çalışır, sonra boşaltılır; ardından DiT FP16 çalışır.",
            "Kare modu resmi olarak test edilmiş yapılandırmaya en yakın profil (RTX 4090, 22.8 GB).",
        ),
    ),
}


def pick_profile(total_vram_bytes: int) -> Profile:
    """Largest profile for the card. Cards report a little under their nominal size
    (a 16 GB card shows 15.6-16.0 GiB), hence the 1 GiB tolerance; the desktop and
    VEGAS reserve is taken out of the per-component budgets, not here."""
    gib = total_vram_bytes / GIB
    best = PROFILES["8gb"]
    for p in sorted(PROFILES.values(), key=lambda p: p.vram_gb):
        if p.vram_gb <= gib + 1.0:
            best = p
    return best


def detect_gpu() -> dict:
    """GPU facts without allocating anything (torch is imported lazily)."""
    try:
        import torch
    except ImportError:
        return {"available": False, "reason": "torch kurulu değil"}
    if not torch.cuda.is_available():
        return {"available": False, "reason": "CUDA GPU bulunamadı"}
    props = torch.cuda.get_device_properties(0)
    return {
        "available": True,
        "name": props.name,
        "total_bytes": int(props.total_memory),
        "capability": f"{props.major}.{props.minor}",
        "bf16": bool(torch.cuda.is_bf16_supported()),
        "torch": torch.__version__,
        "cuda": torch.version.cuda,
    }


def load_measured(report_path: Path) -> dict:
    try:
        return json.loads(Path(report_path).read_text(encoding="utf-8"))
    except (FileNotFoundError, ValueError):
        return {}


def describe(profile: Profile) -> dict:
    d = asdict(profile)
    d["expected"] = {k: asdict(v) for k, v in profile.expected.items()}
    return d
