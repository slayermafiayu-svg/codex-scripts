"""Runs one job in its own process: python -m aimotion.worker <job folder>

A separate process per job keeps the UI responsive, lets the job run at
below-normal CPU priority and gives all GPU memory back to VEGAS the moment
the job ends (the process exits).
"""

from __future__ import annotations

import sys
import threading
import time
import traceback
from pathlib import Path

from .paths import apply_environment, assert_under_root, resolve_layout


def main(argv=None) -> int:
    argv = argv if argv is not None else sys.argv[1:]
    if len(argv) != 1:
        print("kullanım: python -m aimotion.worker <iş klasörü>", file=sys.stderr)
        return 2
    layout = resolve_layout()
    apply_environment(layout)  # before torch/transformers/diffusers are imported

    from .guard import Cancelled, PlaybackGuard, lower_process_priority
    from .jobs import Status, read_json, run_job

    job_dir = assert_under_root(layout, Path(argv[0]))
    status = Status(job_dir)
    spec = read_json(job_dir / "job.json", {}) or {}
    lower_process_priority()

    def on_wait(st):
        status.update(state="paused", stage=f"Duraklatıldı: {st.detail}")

    def on_resume():
        status.update(state="running", stage="Devam ediyor")

    guard = PlaybackGuard(layout.vegas_state_file, unknown_policy=spec.get("unknown_vegas_policy", "block"),
                          on_wait=on_wait, on_resume=on_resume)
    done = threading.Event()

    def watch_cancel():
        while not done.is_set():
            if (job_dir / "cancel").exists():
                guard.cancel()
                return
            time.sleep(0.3)

    threading.Thread(target=watch_cancel, daemon=True).start()
    status.update(state="running", stage="Başlıyor", progress=None, error=None, started=time.time())
    try:
        run_job(layout, job_dir, guard, status)
        return 0
    except Cancelled:
        status.update(state="cancelled", stage="İptal edildi")
        return 3
    except Exception as e:  # noqa: BLE001 - every failure is reported to the UI
        tb = traceback.format_exc()
        (job_dir / "error.log").write_text(tb, encoding="utf-8")
        status.update(state="error", stage="Hata", error=f"{type(e).__name__}: {e}")
        return 1
    finally:
        done.set()


if __name__ == "__main__":
    sys.exit(main())
