"""Local UI server: python -m aimotion.server [--port 7860]

Listens on 127.0.0.1 only. Serves the single-page UI (aimotion/web) and a
small JSON API. Model work is never done in this process: each generation
starts ``python -m aimotion.worker`` and only one runs at a time.
"""

from __future__ import annotations

import argparse
import base64
import io
import json
import mimetypes
import os
import re
import subprocess
import sys
import threading
import time
import webbrowser
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlparse

from .paths import Layout, apply_environment, assert_under_root, resolve_layout

WEB_DIR = Path(__file__).resolve().parent / "web"
PKG_ROOT = Path(__file__).resolve().parent.parent
MAX_UPLOAD = 64 * 1024 * 1024
JOB_ID = re.compile(r"^[0-9A-Za-z-]{6,64}$")


class ApiError(Exception):
    def __init__(self, status: int, message: str):
        super().__init__(message)
        self.status = status


class App:
    def __init__(self, layout: Layout):
        self.layout = layout
        self.lock = threading.Lock()
        self.proc: subprocess.Popen | None = None
        self.proc_job: str | None = None

    # ---------------------------------------------------------------- helpers

    def job_dir(self, job_id: str) -> Path:
        if not JOB_ID.match(job_id or ""):
            raise ApiError(400, "geçersiz iş kimliği")
        d = self.layout.job_dir(job_id)
        if not d.is_dir():
            raise ApiError(404, "iş bulunamadı")
        return d

    def running(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    # ---------------------------------------------------------------- API

    def state(self) -> dict:
        from .backends.hymotion import HYMotionBackend
        from .backends.wan import WanTI2VBackend
        from .guard import read_vegas_state
        from .jobs import resolve_profile
        from .preview import find_puppet_render
        from .profiles import PROFILES, describe, detect_gpu, load_measured

        gpu = detect_gpu()
        auto = resolve_profile("auto")
        vs = read_vegas_state(self.layout.vegas_state_file)
        hy = HYMotionBackend(self.layout, auto, None, lambda *a: None)
        wan = WanTI2VBackend(self.layout, auto, None, lambda *a: None)
        try:
            from .pose import detector_available

            detector = detector_available()
        except Exception:  # noqa: BLE001 - optional component
            detector = False
        return {
            "root": str(self.layout.root),
            "gpu": gpu,
            "auto_profile": auto.key,
            "profiles": {k: describe(p) for k, p in PROFILES.items()},
            "measured": load_measured(self.layout.selftest_report),
            "vegas": {"state": vs.state, "detail": vs.detail, "project": vs.project},
            "models": {"hymotion": hy.missing(), "wan": wan.missing()},
            "puppet_render": bool(find_puppet_render()),
            "detector": detector,
            "busy": self.running(),
            "busy_job": self.proc_job if self.running() else None,
        }

    def upload(self, body: dict) -> dict:
        from PIL import Image

        from .jobs import new_job_id
        import numpy as np

        data = _decode_data_url(body.get("image", ""))
        try:
            im = Image.open(io.BytesIO(data))
            im.load()
        except Exception as e:  # noqa: BLE001
            raise ApiError(400, f"resim okunamadı: {e}") from e
        job_id = new_job_id()
        d = self.layout.job_dir(job_id)
        d.mkdir(parents=True)
        rgba = im.convert("RGBA")
        rgba.save(d / "character.png")
        if body.get("mask"):
            Image.open(io.BytesIO(_decode_data_url(body["mask"]))).convert("L").save(d / "mask.png")
        layers = body.get("layers") or []
        if layers:
            (d / "layers").mkdir()
            names = body.get("layer_names") or []
            for i, data_url in enumerate(layers[:12]):
                layer = Image.open(io.BytesIO(_decode_data_url(data_url))).convert("RGBA")
                if layer.size != rgba.size:
                    raise ApiError(400, f"katman {i + 1}: {layer.size}, karakter {rgba.size} boyutunda olmalı")
                stem = Path(names[i]).stem if i < len(names) else ""
                stem = "".join(c for c in stem if c.isalnum() or c in "_-")[:40]
                layer.save(d / "layers" / f"{i:02d}_{stem}.png")
        a = np.asarray(rgba.getchannel("A"))
        has_alpha = bool(a.min() < 250)
        if (d / "mask.png").is_file():
            a = np.asarray(Image.open(d / "mask.png").convert("L").resize(rgba.size))
            has_alpha = True
        ys, xs = np.nonzero(a > 12) if has_alpha else (np.array([0, rgba.height - 1]), np.array([0, rgba.width - 1]))
        bbox = [int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1] if len(xs) else [0, 0, *rgba.size]
        return {"job_id": job_id, "width": rgba.width, "height": rgba.height, "has_alpha": has_alpha, "bbox": bbox,
                "layers": len(layers)}

    def import_rig(self, body: dict) -> dict:
        from .skeleton import load_rig_file

        d = self.job_dir(body.get("job_id", ""))
        from PIL import Image

        w, h = Image.open(d / "character.png").size
        tmp = d / "rig_import.json"
        tmp.write_text(body.get("json", ""), encoding="utf-8")
        try:
            rig = load_rig_file(tmp, w, h)
        except Exception as e:  # noqa: BLE001
            raise ApiError(400, f"eklem dosyası okunamadı: {e}") from e
        return rig.to_dict()

    def detect_rig(self, body: dict) -> dict:
        from .pose import detect

        d = self.job_dir(body.get("job_id", ""))
        try:
            return detect(d / "character.png").to_dict()
        except Exception as e:  # noqa: BLE001
            raise ApiError(500, f"otomatik algılama başarısız: {e}") from e

    def generate(self, body: dict) -> dict:
        from .jobs import JOB_FORMAT, write_json_atomic
        from .skeleton import Rig

        d = self.job_dir(body.get("job_id", ""))
        kind = body.get("kind", "pins")
        if kind not in ("pins", "frames"):
            raise ApiError(400, "çıktı türü pins veya frames olmalı")
        prompt = " ".join(str(body.get("prompt", "")).split())
        if not prompt:
            raise ApiError(400, "hareket tarifini yazın")
        n = max(1, min(4, int(body.get("candidates", 1))))
        seeds = body.get("seeds") or [int.from_bytes(os.urandom(3), "little") for _ in range(n)]
        st = body.get("stage") or {}
        spec = {
            "format": JOB_FORMAT,
            "kind": kind,
            "prompt": prompt,
            "duration_s": float(body.get("duration_s", 4.0)),
            "seeds": [int(s) for s in seeds],
            "cfg_scale": float(body.get("cfg_scale", 5.0)),
            "steps": body.get("steps"),
            "profile": body.get("profile", "auto"),
            "dit_dtype": body.get("dit_dtype"),
            "allow_experimental": bool(body.get("allow_experimental", False)),
            "key_background": bool(body.get("key_background", True)),
            "unknown_vegas_policy": "allow" if body.get("vegas_unknown_allow") else "block",
            "stage": {"width": int(st.get("width", 1920)), "height": int(st.get("height", 1080)),
                      "fps": float(st.get("fps", 29.97)), "center_x": float(st.get("center_x", 0.5)),
                      "char_height": float(st.get("char_height", 0.72))},
            "retarget": body.get("retarget") or {},
            "mesh": body.get("mesh") or {},
        }
        if kind == "pins":
            rig = Rig.from_dict(body.get("rig") or {})
            missing = rig.complete().missing_required()
            if missing:
                raise ApiError(400, "eksik eklemler: " + ", ".join(missing))
            write_json_atomic(d / "rig_src.json", rig.to_dict())
        with self.lock:
            if self.running():
                raise ApiError(409, "başka bir üretim sürüyor")
            for f in ("status.json", "cancel", "error.log"):
                (d / f).unlink(missing_ok=True)
            write_json_atomic(d / "job.json", spec)
            write_json_atomic(d / "status.json", {"state": "queued", "stage": "Sırada", "progress": None})
            flags = 0
            if os.name == "nt":
                flags = subprocess.CREATE_NO_WINDOW | 0x00004000  # BELOW_NORMAL_PRIORITY_CLASS
            log = open(self.layout.logs / f"{d.name}.log", "ab")
            env = dict(os.environ, AIMOTION_ROOT=str(self.layout.root), PYTHONUTF8="1", PYTHONIOENCODING="utf-8")
            self.proc = subprocess.Popen([sys.executable, "-m", "aimotion.worker", str(d)], cwd=str(PKG_ROOT),
                                         stdout=log, stderr=subprocess.STDOUT, creationflags=flags, env=env)
            log.close()
            self.proc_job = d.name
        return {"ok": True, "job_id": d.name, "seeds": spec["seeds"]}

    def job(self, job_id: str) -> dict:
        from .jobs import read_json

        d = self.job_dir(job_id)
        st = read_json(d / "status.json", {}) or {}
        if st.get("state") in ("queued", "running", "paused") and self.proc_job == job_id and not self.running():
            st = dict(st, state="error", error=st.get("error") or "işçi süreci beklenmedik biçimde kapandı")
        st["job"] = read_json(d / "job.json", {})
        st["stage_info"] = read_json(d / "stage.json", {})
        return st

    def cancel(self, job_id: str) -> dict:
        (self.job_dir(job_id) / "cancel").write_text("1")
        return {"ok": True}

    def retarget(self, job_id: str, body: dict) -> dict:
        from .jobs import redo_pins

        d = self.job_dir(job_id)
        try:
            return redo_pins(self.layout, d, int(body["seed"]), body.get("options") or {})
        except (KeyError, ValueError) as e:
            raise ApiError(400, str(e)) from e

    def export(self, job_id: str, body: dict) -> dict:
        from .jobs import export_to_vegas

        d = self.job_dir(job_id)
        m = export_to_vegas(self.layout, d, int(body["seed"]), bake_frames=bool(body.get("bake")))
        return {"ok": True, "manifest": str(m)}


def _decode_data_url(s: str) -> bytes:
    if "," in s and s.startswith("data:"):
        s = s.split(",", 1)[1]
    try:
        return base64.b64decode(s, validate=False)
    except ValueError as e:
        raise ApiError(400, "base64 çözülemedi") from e


def make_handler(app: App):
    class Handler(BaseHTTPRequestHandler):
        server_version = "AIMotion/0.1"

        def log_message(self, fmt, *args):  # quiet
            pass

        def _send(self, status: int, body: bytes, ctype: str, extra: dict | None = None):
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)

        def _json(self, status: int, data):
            self._send(status, json.dumps(data, ensure_ascii=False).encode("utf-8"), "application/json; charset=utf-8")

        def _body(self) -> dict:
            n = int(self.headers.get("Content-Length") or 0)
            if n > MAX_UPLOAD:
                raise ApiError(413, "dosya çok büyük")
            raw = self.rfile.read(n) if n else b"{}"
            try:
                return json.loads(raw.decode("utf-8"))
            except ValueError as e:
                raise ApiError(400, "geçersiz JSON") from e

        def _file(self, path: Path):
            if not path.is_file():
                return self._json(404, {"error": "dosya yok"})
            size = path.stat().st_size
            ctype = mimetypes.guess_type(path.name)[0] or "application/octet-stream"
            rng = self.headers.get("Range")
            m = re.match(r"bytes=(\d*)-(\d*)$", rng or "")
            with open(path, "rb") as f:
                if m and (m.group(1) or m.group(2)):
                    start = int(m.group(1)) if m.group(1) else max(0, size - int(m.group(2)))
                    end = int(m.group(2)) if m.group(1) and m.group(2) else size - 1
                    end = min(end, size - 1)
                    if start > end:
                        return self._send(416, b"", ctype, {"Content-Range": f"bytes */{size}"})
                    f.seek(start)
                    data = f.read(end - start + 1)
                    return self._send(206, data, ctype, {"Content-Range": f"bytes {start}-{end}/{size}",
                                                         "Accept-Ranges": "bytes"})
                return self._send(200, f.read(), ctype, {"Accept-Ranges": "bytes"})

        def _dispatch(self):
            url = urlparse(self.path)
            parts = [unquote(p) for p in url.path.split("/") if p]
            try:
                if self.command in ("GET", "HEAD"):
                    if not parts:
                        return self._file(WEB_DIR / "index.html")
                    if parts == ["favicon.ico"]:
                        return self._send(204, b"", "image/x-icon")
                    if parts[0] == "static" and len(parts) == 2 and re.match(r"^[\w.-]+$", parts[1]):
                        return self._file(WEB_DIR / parts[1])
                    if parts[0] == "files" and len(parts) >= 3:
                        d = app.job_dir(parts[1])
                        p = assert_under_root(app.layout, d.joinpath(*parts[2:]))
                        if d.resolve() not in p.parents:
                            raise ApiError(403, "izin yok")
                        return self._file(p)
                    if parts == ["api", "state"]:
                        return self._json(200, app.state())
                    if len(parts) == 3 and parts[:2] == ["api", "job"]:
                        return self._json(200, app.job(parts[2]))
                    raise ApiError(404, "bulunamadı")
                if self.command == "POST":
                    if self.headers.get("Origin") not in (None, f"http://127.0.0.1:{self.server.server_port}",
                                                          f"http://localhost:{self.server.server_port}"):
                        raise ApiError(403, "yalnızca yerel arayüz")
                    body = self._body()
                    routes = {
                        ("api", "upload"): lambda: app.upload(body),
                        ("api", "rig-import"): lambda: app.import_rig(body),
                        ("api", "rig-detect"): lambda: app.detect_rig(body),
                        ("api", "generate"): lambda: app.generate(body),
                    }
                    if tuple(parts) in routes:
                        return self._json(200, routes[tuple(parts)]())
                    if len(parts) == 4 and parts[:2] == ["api", "job"]:
                        action = {"cancel": lambda: app.cancel(parts[2]),
                                  "retarget": lambda: app.retarget(parts[2], body),
                                  "export": lambda: app.export(parts[2], body)}.get(parts[3])
                        if action:
                            return self._json(200, action())
                    raise ApiError(404, "bulunamadı")
                raise ApiError(405, "yöntem desteklenmiyor")
            except ApiError as e:
                return self._json(e.status, {"error": str(e)})
            except Exception as e:  # noqa: BLE001 - report instead of dropping the connection
                return self._json(500, {"error": f"{type(e).__name__}: {e}"})

        do_GET = do_POST = do_HEAD = _dispatch

    return Handler


def serve(port: int = 7860, open_browser: bool = True, layout: Layout | None = None) -> None:
    layout = layout or resolve_layout()
    apply_environment(layout)
    app = App(layout)
    httpd = ThreadingHTTPServer(("127.0.0.1", port), make_handler(app))
    url = f"http://127.0.0.1:{httpd.server_port}/"
    print(f"AI Motion arayüzü: {url}  (kök: {layout.root})", flush=True)
    if open_browser:
        threading.Timer(0.5, lambda: webbrowser.open(url)).start()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="AI Motion yerel arayüzü")
    ap.add_argument("--port", type=int, default=7860)
    ap.add_argument("--no-browser", action="store_true")
    a = ap.parse_args(argv)
    serve(a.port, not a.no_browser)
    return 0


if __name__ == "__main__":
    sys.exit(main())
