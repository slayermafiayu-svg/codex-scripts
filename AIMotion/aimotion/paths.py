"""Where everything lives on disk.

All models, caches, temporary files and job outputs go under one root, by
default ``D:\\AIMotion``. The root can be changed with the ``AIMOTION_ROOT``
environment variable or the ``root`` key of ``aimotion.json`` next to the
package. On Windows the root must be on the drive named by ``required_drive``
(``D:`` by default); nothing silently falls back to C:.

``apply_environment()`` points the Hugging Face, PyTorch, Triton, CUDA and
pip caches and the temp directory at the root. Call it before importing
torch, transformers or diffusers (the worker and server do).
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass
from pathlib import Path, PureWindowsPath

PACKAGE_DIR = Path(__file__).resolve().parent
CONFIG_FILE = PACKAGE_DIR.parent / "aimotion.json"
DEFAULT_ROOT = r"D:\AIMotion"
DEFAULT_DRIVE = "D:"


class StorageError(RuntimeError):
    pass


def load_user_config() -> dict:
    try:
        with open(CONFIG_FILE, "r", encoding="utf-8") as f:
            return json.load(f)
    except FileNotFoundError:
        return {}


@dataclass(frozen=True)
class Layout:
    root: Path

    # models (downloaded once, large)
    @property
    def models(self) -> Path:
        return self.root / "models"

    @property
    def hymotion_repo(self) -> Path:  # code: github.com/Tencent-Hunyuan/HY-Motion-1.0
        return self.root / "repos" / "HY-Motion-1.0"

    @property
    def hymotion_ckpts(self) -> Path:  # tencent/HY-Motion-1.0 (HY-Motion-1.0/, HY-Motion-1.0-Lite/)
        return self.models / "HY-Motion-1.0"

    @property
    def qwen3_8b(self) -> Path:  # Qwen/Qwen3-8B, HY-Motion's LLM text encoder
        return self.models / "Qwen3-8B"

    @property
    def clip_l(self) -> Path:  # openai/clip-vit-large-patch14, HY-Motion's sentence embedding
        return self.models / "clip-vit-large-patch14"

    @property
    def wan_ti2v_5b(self) -> Path:  # Wan-AI/Wan2.2-TI2V-5B-Diffusers
        return self.models / "Wan2.2-TI2V-5B-Diffusers"

    # caches and scratch
    @property
    def cache(self) -> Path:
        return self.root / "cache"

    @property
    def hf_home(self) -> Path:
        return self.cache / "huggingface"

    @property
    def offload(self) -> Path:  # accelerate disk offload for split text encoders
        return self.cache / "offload"

    @property
    def text_embeddings(self) -> Path:  # cached prompt encodings
        return self.cache / "text-embeddings"

    @property
    def tmp(self) -> Path:
        return self.root / "tmp"

    # work
    @property
    def jobs(self) -> Path:
        return self.root / "jobs"

    @property
    def outbox(self) -> Path:  # results handed to VEGAS
        return self.root / "outbox"

    @property
    def state(self) -> Path:  # VEGAS bridge heartbeat lives here
        return self.root / "state"

    @property
    def vegas_state_file(self) -> Path:
        return self.state / "vegas_state.json"

    @property
    def logs(self) -> Path:
        return self.root / "logs"

    @property
    def selftest_report(self) -> Path:
        return self.root / "selftest.json"

    def job_dir(self, job_id: str) -> Path:
        if not job_id or any(c in job_id for c in "/\\:.") or len(job_id) > 64:
            raise ValueError(f"bad job id: {job_id!r}")
        return self.jobs / job_id

    def all_dirs(self) -> list[Path]:
        return [
            self.models, self.hymotion_repo.parent, self.cache, self.hf_home, self.offload,
            self.text_embeddings, self.tmp, self.jobs, self.outbox, self.state, self.logs,
        ]


def drive_of(path: str | Path) -> str:
    return PureWindowsPath(str(path)).drive.upper()


def resolve_layout(root: str | Path | None = None, *, check_drive: bool | None = None) -> Layout:
    cfg = load_user_config()
    root = root or os.environ.get("AIMOTION_ROOT") or cfg.get("root")
    if not root:
        # D:\AIMotion on Windows; development machines without drive letters use ~/AIMotion.
        root = DEFAULT_ROOT if os.name == "nt" else str(Path.home() / "AIMotion")
    required = (cfg.get("required_drive") or DEFAULT_DRIVE).upper()
    if check_drive is None:
        check_drive = os.name == "nt" and os.environ.get("AIMOTION_ALLOW_ANY_DRIVE") != "1"
    if check_drive and drive_of(root) != required:
        raise StorageError(
            f"AI Motion kök klasörü {root} {required} sürücüsünde değil. Modeller ve önbellek "
            f"{required} sürücüsünde tutulmalı (aimotion.json 'root' veya AIMOTION_ROOT ile ayarlayın)."
        )
    return Layout(Path(root))


def apply_environment(layout: Layout, create: bool = True) -> dict[str, str]:
    """Points every cache and temp directory at the layout. Returns what was set."""
    if create:
        for d in layout.all_dirs():
            d.mkdir(parents=True, exist_ok=True)
    env = {
        "HF_HOME": layout.hf_home,
        "HF_HUB_CACHE": layout.hf_home / "hub",
        "HF_XET_CACHE": layout.hf_home / "xet",
        "HF_ASSETS_CACHE": layout.hf_home / "assets",
        "TORCH_HOME": layout.cache / "torch",
        "TORCHINDUCTOR_CACHE_DIR": layout.cache / "torchinductor",
        "TRITON_CACHE_DIR": layout.cache / "triton",
        "CUDA_CACHE_PATH": layout.cache / "cuda-jit",
        "PIP_CACHE_DIR": layout.cache / "pip",
        "XDG_CACHE_HOME": layout.cache,
        "MPLCONFIGDIR": layout.cache / "matplotlib",
        "TMP": layout.tmp,
        "TEMP": layout.tmp,
        "TMPDIR": layout.tmp,
        "GRADIO_TEMP_DIR": layout.tmp / "gradio",
    }
    out = {k: str(v) for k, v in env.items()}
    os.environ.update(out)
    # HY-Motion reads local text encoder folders instead of downloading at run time.
    os.environ["USE_HF_MODELS"] = "0"
    os.environ.setdefault("HF_HUB_DISABLE_TELEMETRY", "1")
    import tempfile

    tempfile.tempdir = None  # re-read TMP/TEMP on next use
    return out


def assert_under_root(layout: Layout, path: str | Path) -> Path:
    """Refuses paths outside the layout root (used for anything written by jobs)."""
    p = Path(path).resolve()
    root = layout.root.resolve()
    if p != root and root not in p.parents:
        raise StorageError(f"{p} AI Motion kök klasörünün ({root}) dışında")
    return p
