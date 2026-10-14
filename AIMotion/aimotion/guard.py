"""Keeps heavy AI work off the GPU while VEGAS is playing or rendering.

The VEGAS side (vegas/AIMotionBridge.cs, an application extension) writes
``<root>/state/vegas_state.json`` about four times a second:

    {"version": 1, "pid": 1234, "playing": false, "rendering": false,
     "ts": 1760000000.25, "project": {"width": 1920, "height": 1080, "fps": 29.97}}

The worker calls ``PlaybackGuard.checkpoint()`` before loading models and
before every denoising step. While VEGAS plays or renders, the call blocks
(the job is paused, nothing new is computed); it returns once VEGAS is idle
again or raises ``Cancelled`` when the job is cancelled.

If VEGAS is running but the heartbeat is missing or stale (extension not
installed, VEGAS hung), the state is "unknown" and the default policy blocks
too, so a missing bridge can never let heavy work run during playback.
"""

from __future__ import annotations

import json
import os
import subprocess
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional

IDLE, PLAYING, RENDERING, UNKNOWN, NOT_RUNNING = "idle", "playing", "rendering", "unknown", "not_running"
BUSY_STATES = (PLAYING, RENDERING)


class Cancelled(Exception):
    pass


@dataclass
class VegasState:
    state: str
    detail: str = ""
    project: Optional[dict] = None
    age_s: Optional[float] = None


def vegas_process_running() -> Optional[bool]:
    """True/False on Windows, None when it cannot be told (other OS, no tasklist)."""
    if os.name != "nt":
        return None
    try:
        import psutil  # optional

        return any((p.info.get("name") or "").lower().startswith("vegas") for p in psutil.process_iter(["name"]))
    except ImportError:
        pass
    try:
        out = subprocess.run(
            ["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True, timeout=5,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        ).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    return any(line.strip('"').lower().startswith("vegas") for line in out.splitlines())


def read_vegas_state(
    state_file: Path,
    stale_after_s: float = 2.0,
    now: Optional[float] = None,
    process_running: Callable[[], Optional[bool]] = vegas_process_running,
) -> VegasState:
    now = time.time() if now is None else now
    try:
        data = json.loads(Path(state_file).read_text(encoding="utf-8-sig"))
        age = now - float(data.get("ts", 0))
    except FileNotFoundError:
        data, age = None, None
    except (OSError, ValueError, TypeError):
        # The bridge rewrites the file constantly; a half-written read is not an answer.
        return VegasState(UNKNOWN, "durum dosyası okunamadı")

    if data is not None and age is not None and -5.0 <= age <= stale_after_s:
        project = data.get("project") if isinstance(data.get("project"), dict) else None
        if data.get("closed"):
            return VegasState(NOT_RUNNING, "VEGAS kapatıldı")
        if data.get("rendering"):
            return VegasState(RENDERING, "VEGAS render alıyor", project, age)
        if data.get("playing") is None:
            # The bridge runs but this VEGAS version gives it no way to see playback.
            return VegasState(UNKNOWN, "köprü eklentisi oynatma durumunu okuyamıyor", project, age)
        if data.get("playing"):
            return VegasState(PLAYING, "VEGAS oynatıyor", project, age)
        return VegasState(IDLE, "VEGAS boşta", project, age)

    running = process_running()
    if running is False:
        return VegasState(NOT_RUNNING, "VEGAS açık değil")
    if data is not None and data.get("closed"):
        # The bridge writes closed=true when VEGAS exits normally.
        return VegasState(NOT_RUNNING, "VEGAS kapatıldı")
    detail = "VEGAS köprü eklentisinden haber yok" if data is None else f"VEGAS durumu {age:.0f} sn eski"
    if running is None:
        detail += " (VEGAS'ın açık olup olmadığı anlaşılamadı)"
    return VegasState(UNKNOWN, detail)


class PlaybackGuard:
    """Blocks heavy work while VEGAS is busy. Thread-safe; one per job."""

    def __init__(
        self,
        state_file: Path,
        unknown_policy: str = "block",  # block | allow
        poll_s: float = 0.25,
        stale_after_s: float = 2.0,
        on_wait: Optional[Callable[[VegasState], None]] = None,
        on_resume: Optional[Callable[[], None]] = None,
        reader: Callable[..., VegasState] = read_vegas_state,
        sleep: Callable[[float], None] = time.sleep,
    ):
        if unknown_policy not in ("block", "allow"):
            raise ValueError(unknown_policy)
        self.state_file = Path(state_file)
        self.unknown_policy = unknown_policy
        self.poll_s = poll_s
        self.stale_after_s = stale_after_s
        self.on_wait = on_wait
        self.on_resume = on_resume
        self._reader = reader
        self._sleep = sleep
        self._cancel = threading.Event()
        self.waited_s = 0.0
        self.pauses = 0

    def cancel(self) -> None:
        self._cancel.set()

    @property
    def cancelled(self) -> bool:
        return self._cancel.is_set()

    def must_wait(self, st: VegasState) -> bool:
        if st.state in BUSY_STATES:
            return True
        return st.state == UNKNOWN and self.unknown_policy == "block"

    def current(self) -> VegasState:
        return self._reader(self.state_file, self.stale_after_s)

    def checkpoint(self) -> None:
        """Returns when heavy work may run; blocks while VEGAS is busy."""
        if self._cancel.is_set():
            raise Cancelled()
        st = self.current()
        if not self.must_wait(st):
            return
        self.pauses += 1
        last_reported = None
        t0 = time.monotonic()
        while self.must_wait(st):
            if self._cancel.is_set():
                raise Cancelled()
            if self.on_wait and (last_reported is None or st.state != last_reported):
                self.on_wait(st)
                last_reported = st.state
            self._sleep(self.poll_s)
            st = self.current()
        self.waited_s += time.monotonic() - t0
        if self.on_resume:
            self.on_resume()
        if self._cancel.is_set():
            raise Cancelled()


def lower_process_priority() -> None:
    """Below-normal CPU priority so VEGAS keeps the UI and decoding threads."""
    try:
        if os.name == "nt":
            import ctypes

            BELOW_NORMAL_PRIORITY_CLASS = 0x00004000
            handle = ctypes.windll.kernel32.GetCurrentProcess()
            ctypes.windll.kernel32.SetPriorityClass(handle, BELOW_NORMAL_PRIORITY_CLASS)
        else:
            os.nice(5)
    except (OSError, AttributeError):
        pass
