"""Where a motion came from, recorded with every result.

The tool has no motion presets. A result is either produced by a model run
on this machine (``source = "ai_model"``, with checkpoint hash, seed, prompt,
dtype, device and timings) or it is test data (``source = "test_fixture"``),
which the UI, the preview and the VEGAS import all label "TEST - AI DEĞİL".
Exports refuse records without a source, and an ``ai_model`` record without
a checkpoint hash is rejected, so a canned motion can never be passed off
as a generated one.
"""

from __future__ import annotations

import datetime as _dt
import hashlib
import json
import os
import platform
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Optional

AI_MODEL = "ai_model"
TEST_FIXTURE = "test_fixture"
SOURCES = (AI_MODEL, TEST_FIXTURE)


class ProvenanceError(ValueError):
    pass


@dataclass
class Provenance:
    source: str
    model: str = ""  # e.g. "tencent/HY-Motion-1.0 (HY-Motion-1.0)"
    checkpoint_sha256: str = ""
    code_revision: str = ""  # git commit of the model code
    license: str = ""
    prompt: str = ""
    seed: Optional[int] = None
    params: dict = field(default_factory=dict)  # steps, cfg, dtype, profile, duration ...
    device: dict = field(default_factory=dict)  # gpu name, vram, torch/cuda versions
    timings_s: dict = field(default_factory=dict)
    peak_vram_bytes: dict = field(default_factory=dict)  # per phase, from torch.cuda
    edits: list = field(default_factory=list)  # user edits applied afterwards
    created_at: str = field(default_factory=lambda: _dt.datetime.now().astimezone().isoformat(timespec="seconds"))
    host: str = field(default_factory=platform.node)

    def validate(self) -> "Provenance":
        if self.source not in SOURCES:
            raise ProvenanceError(f"bilinmeyen kaynak: {self.source!r}")
        if self.source == AI_MODEL:
            missing = [k for k in ("model", "checkpoint_sha256", "prompt") if not getattr(self, k)]
            if self.seed is None:
                missing.append("seed")
            if missing:
                raise ProvenanceError("AI sonucu eksik kaynak bilgisiyle kaydedilemez: " + ", ".join(missing))
        return self

    @property
    def is_ai(self) -> bool:
        return self.source == AI_MODEL

    def label(self) -> str:
        if self.source == TEST_FIXTURE:
            return "TEST - AI DEĞİL"
        return f"AI: {self.model} seed={self.seed}"

    def to_dict(self) -> dict:
        return asdict(self)

    @classmethod
    def from_dict(cls, d: dict) -> "Provenance":
        known = {k: v for k, v in d.items() if k in cls.__dataclass_fields__}
        return cls(**known).validate()


def sha256_file(path: Path, cache_file: Optional[Path] = None, chunk: int = 8 << 20) -> str:
    """SHA-256 of a (large) file; cached by path, size and mtime."""
    path = Path(path)
    st = path.stat()
    key = f"{path.resolve()}|{st.st_size}|{int(st.st_mtime)}"
    cache = {}
    if cache_file:
        try:
            cache = json.loads(Path(cache_file).read_text(encoding="utf-8"))
        except (FileNotFoundError, ValueError):
            cache = {}
        if key in cache:
            return cache[key]
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(chunk)
            if not b:
                break
            h.update(b)
    digest = h.hexdigest()
    if cache_file:
        cache[key] = digest
        tmp = Path(str(cache_file) + ".tmp")
        tmp.write_text(json.dumps(cache, indent=1), encoding="utf-8")
        os.replace(tmp, cache_file)
    return digest


def git_revision(repo: Path) -> str:
    """Commit of a checkout without running git (HEAD -> ref -> sha)."""
    git = Path(repo) / ".git"
    try:
        head = (git / "HEAD").read_text(encoding="utf-8").strip()
        if head.startswith("ref: "):
            ref = head[5:]
            p = git / ref
            if p.exists():
                return p.read_text(encoding="utf-8").strip()
            packed = (git / "packed-refs").read_text(encoding="utf-8")
            for line in packed.splitlines():
                if line.endswith(" " + ref):
                    return line.split(" ", 1)[0]
            return ""
        return head
    except OSError:
        return ""
