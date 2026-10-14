"""Downloads the pre-trained weights from their official Hugging Face repos to D:.

    python -m aimotion.download                 # HY-Motion-1.0 + Qwen3-8B + CLIP-L
    python -m aimotion.download --lite          # also HY-Motion-1.0-Lite
    python -m aimotion.download --wan           # also Wan2.2-TI2V-5B (frames mode)
    python -m aimotion.download --dry-run       # only show what would be fetched

Every model keeps its own license (see docs/ARASTIRMA.md); nothing is
redistributed by this project. Downloads resume if interrupted.
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

from .paths import Layout, apply_environment, resolve_layout


@dataclass
class Item:
    key: str
    repo: str
    dest: Path
    allow: Optional[list[str]]
    license: str
    approx_gb: float


def plan(layout: Layout, lite: bool = False, wan: bool = False) -> list[Item]:
    hy = ["HY-Motion-1.0/*"] + (["HY-Motion-1.0-Lite/*"] if lite else [])
    items = [
        Item("hymotion", "tencent/HY-Motion-1.0", layout.hymotion_ckpts, hy,
             "Tencent HY-Motion 1.0 Community License", 5.9 if lite else 4.0),
        Item("qwen3-8b", "Qwen/Qwen3-8B", layout.qwen3_8b,
             ["*.json", "*.safetensors", "merges.txt", "vocab.json", "LICENSE"], "Apache-2.0", 16.4),
        Item("clip-l", "openai/clip-vit-large-patch14", layout.clip_l,
             ["*.json", "merges.txt", "vocab.json", "model.safetensors"], "MIT (OpenAI CLIP)", 1.7),
    ]
    if wan:
        items.append(Item("wan-ti2v-5b", "Wan-AI/Wan2.2-TI2V-5B-Diffusers", layout.wan_ti2v_5b, None, "Apache-2.0",
                          34.0))
    return items


def fetch(item: Item) -> None:
    from huggingface_hub import list_repo_files, snapshot_download
    from huggingface_hub.utils import GatedRepoError, RepositoryNotFoundError

    allow = item.allow
    try:
        if allow and "model.safetensors" in allow:
            files = list_repo_files(item.repo)
            if "model.safetensors" not in files:  # older repos ship only PyTorch .bin weights
                allow = [a for a in allow if a != "model.safetensors"] + ["pytorch_model.bin"]
        item.dest.mkdir(parents=True, exist_ok=True)
        snapshot_download(item.repo, local_dir=str(item.dest), allow_patterns=allow, max_workers=4)
    except GatedRepoError as e:
        raise SystemExit(
            f"{item.repo} lisans onayı istiyor: https://huggingface.co/{item.repo} sayfasında koşulları kabul edin, "
            f"sonra 'hf auth login' ile giriş yapıp tekrar çalıştırın."
        ) from e
    except RepositoryNotFoundError as e:
        raise SystemExit(f"{item.repo} bulunamadı veya erişim yok") from e


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="AI Motion model indirici")
    ap.add_argument("--lite", action="store_true", help="HY-Motion-1.0-Lite'ı da indir")
    ap.add_argument("--wan", action="store_true", help="Wan2.2-TI2V-5B'yi (kare modu, ~34 GB) da indir")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args(argv)
    layout = resolve_layout()
    apply_environment(layout)
    items = plan(layout, a.lite, a.wan)
    total = sum(i.approx_gb for i in items)
    print(f"Hedef: {layout.models}  (kabaca {total:.0f} GB; boyutlar parametre sayısından tahmin)")
    for i in items:
        print(f"  {i.repo:38s} -> {i.dest}   [{i.license}, ~{i.approx_gb} GB]")
    if a.dry_run:
        return 0
    for i in items:
        print(f"İndiriliyor: {i.repo}", flush=True)
        fetch(i)
    print("Tamam.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
