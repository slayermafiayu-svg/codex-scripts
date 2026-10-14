"""Model runners. Each one loads real pre-trained weights from the layout and
refuses to run (with a message saying what is missing) instead of falling
back to anything else."""

from __future__ import annotations

import gc
import time
from contextlib import contextmanager
from typing import Callable, Optional

Report = Callable[[str, Optional[float]], None]


class MissingModel(RuntimeError):
    pass


def free_cuda() -> None:
    gc.collect()
    try:
        import torch

        if torch.cuda.is_available():
            torch.cuda.empty_cache()
    except ImportError:
        pass


@contextmanager
def measure(phase: str, timings: dict, peaks: dict):
    """Wall time and peak CUDA memory (allocated and reserved) of one phase."""
    import torch

    cuda = torch.cuda.is_available()
    if cuda:
        torch.cuda.synchronize()
        torch.cuda.reset_peak_memory_stats()
    t0 = time.perf_counter()
    try:
        yield
    finally:
        if cuda:
            torch.cuda.synchronize()
            peaks[phase] = {
                "allocated": int(torch.cuda.max_memory_allocated()),
                "reserved": int(torch.cuda.max_memory_reserved()),
            }
        timings[phase] = round(time.perf_counter() - t0, 3)


def device_info() -> dict:
    from ..profiles import detect_gpu

    return detect_gpu()
