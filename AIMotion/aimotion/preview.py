"""Preview video (and optional baked PNG frames) for a pin animation.

The pictures come from ``puppet_render``, built from the Puppet Pin plug-in's
own PuppetCore.h, so the preview shows the same mesh warp VEGAS will render.
If the renderer is missing, a plainly labelled skeleton-only preview is made
instead (no warp).
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Callable, Optional

import numpy as np
from PIL import Image, ImageDraw

from .pins import PinAnimation
from .skeleton import RIG_PARENT

PACKAGE_DIR = Path(__file__).resolve().parent
_NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)


class PreviewError(RuntimeError):
    pass


def find_ffmpeg() -> str:
    exe = shutil.which("ffmpeg")
    if exe:
        return exe
    try:
        import imageio_ffmpeg

        return imageio_ffmpeg.get_ffmpeg_exe()
    except Exception as e:  # noqa: BLE001 - any failure means "not available"
        raise PreviewError("ffmpeg bulunamadı (PATH'e ekleyin veya 'pip install imageio-ffmpeg')") from e


def find_puppet_render() -> Optional[str]:
    name = "puppet_render.exe" if os.name == "nt" else "puppet_render"
    env = os.environ.get("AIMOTION_PUPPET_RENDER")
    candidates = [env] if env else []
    candidates += [
        str(PACKAGE_DIR.parent / "bin" / name),
        str(PACKAGE_DIR.parent / "native" / "build" / "Release" / name),
        str(PACKAGE_DIR.parent / "native" / "build" / name),
    ]
    for c in candidates:
        if c and Path(c).is_file():
            return c
    return shutil.which(name)


def _watermark(frame: np.ndarray, text: str) -> np.ndarray:
    im = Image.fromarray(frame, "RGBA")
    d = ImageDraw.Draw(im)
    scale = max(1, im.height // 360)
    x, y = 12 * scale, 12 * scale
    box = d.textbbox((0, 0), text)
    w, h = (box[2] - box[0]) * scale, (box[3] - box[1]) * scale
    d.rectangle([x - 6, y - 6, x + w + 6, y + h + 6], fill=(200, 30, 30, 230))
    small = Image.new("RGBA", (box[2] - box[0] + 2, box[3] - box[1] + 4), (0, 0, 0, 0))
    ImageDraw.Draw(small).text((-box[0], -box[1]), text, fill=(255, 255, 255, 255))
    small = small.resize((small.width * scale, small.height * scale), Image.NEAREST)
    im.alpha_composite(small, (x, y))
    return np.asarray(im)


def _ffmpeg_mp4(ffmpeg: str, w: int, h: int, fps: float, out_mp4: Path) -> subprocess.Popen:
    return subprocess.Popen(
        [ffmpeg, "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgba", "-s", f"{w}x{h}",
         "-r", f"{fps:.6f}", "-i", "-", "-c:v", "libx264", "-preset", "veryfast", "-crf", "20",
         "-pix_fmt", "yuv420p", "-movflags", "+faststart", str(out_mp4)],
        stdin=subprocess.PIPE, creationflags=_NO_WINDOW,
    )


def render_frames(anim: PinAnimation, stage_png: Path, work_dir: Path, background: Optional[tuple] = (36, 36, 40),
                  draw_pins: bool = False, renderer: Optional[str] = None):
    """Yields (frame_index, RGBA ndarray) from puppet_render."""
    renderer = renderer or find_puppet_render()
    if not renderer:
        raise PreviewError("puppet_render bulunamadı")
    stage = Image.open(stage_png).convert("RGBA")
    if stage.size != (anim.width, anim.height):
        raise PreviewError(f"sahne {stage.size} ama pin verisi {anim.width}x{anim.height} için")
    work_dir.mkdir(parents=True, exist_ok=True)
    raw = work_dir / "stage.rgba"
    raw.write_bytes(np.asarray(stage).tobytes())
    job = {
        "input": str(raw),
        "width": anim.width,
        "height": anim.height,
        "frames": anim.num_frames,
        "mesh": {"use_alpha": anim.mesh.use_alpha, "alpha_threshold": anim.mesh.alpha_threshold,
                 "density": anim.mesh.density, "expansion": anim.mesh.expansion},
        "rigidity": anim.mesh.rigidity,
        "draw_pins": draw_pins,
        "pins": [{"anchor": list(p.anchor), "track": p.track.tolist()} for p in anim.pins],
    }
    if background is not None:
        job["background"] = list(background)
    job_path = work_dir / "render_job.json"
    job_path.write_text(json.dumps(job), encoding="utf-8")
    frame_bytes = anim.width * anim.height * 4
    proc = subprocess.Popen([renderer, str(job_path)], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            creationflags=_NO_WINDOW)
    try:
        for i in range(anim.num_frames):
            buf = proc.stdout.read(frame_bytes)
            if len(buf) != frame_bytes:
                break
            yield i, np.frombuffer(buf, np.uint8).reshape(anim.height, anim.width, 4)
    finally:
        proc.stdout.close()
        err = proc.stderr.read().decode("utf-8", "replace")
        proc.stderr.close()
        code = proc.wait()
    if code != 0:
        raise PreviewError(f"puppet_render başarısız ({code}): {err.strip()}")


def _sub_animation(anim: PinAnimation, numbers: list[int]) -> PinAnimation:
    pins = [p for p in anim.pins if p.number in set(numbers)]
    return PinAnimation(anim.width, anim.height, anim.fps, pins, anim.mesh, anim.provenance)


def composed_frames(anim: PinAnimation, stage_dir: Path, work_dir: Path, renderer: Optional[str] = None):
    """Yields (i, RGBA frame): the warped stage, or every part layer warped by its own pins
    and composited back to front (what the stacked VEGAS tracks show)."""
    if not anim.layers:
        yield from render_frames(anim, stage_dir / (anim.stage_image or "stage.png"), work_dir, background=None,
                                 renderer=renderer)
        return
    streams = []
    for k, layer in enumerate(anim.layers):
        if layer["pins"]:
            sub = _sub_animation(anim, layer["pins"])
            streams.append(render_frames(sub, stage_dir / layer["image"], work_dir / f"layer{k:02d}", background=None,
                                         renderer=renderer))
        else:
            still = np.asarray(Image.open(stage_dir / layer["image"]).convert("RGBA"))
            streams.append(((i, still) for i in range(anim.num_frames)))
    for parts in zip(*streams):
        acc = np.zeros((anim.height, anim.width, 4), np.float32)
        for _, f in parts:  # straight-alpha "over", back to front
            fa = f[..., 3:4].astype(np.float32) / 255.0
            aa = acc[..., 3:4]
            out_a = fa + aa * (1 - fa)
            rgb = (f[..., :3] * fa + acc[..., :3] * aa * (1 - fa)) / np.maximum(out_a, 1e-6)
            acc = np.concatenate([rgb, out_a], axis=-1)
        acc[..., 3] *= 255.0
        yield parts[0][0], np.clip(acc + 0.5, 0, 255).astype(np.uint8)


def _over_background(frame: np.ndarray, bg=(36, 36, 40)) -> np.ndarray:
    a = frame[..., 3:4].astype(np.float32) / 255.0
    rgb = frame[..., :3] * a + np.array(bg, np.float32) * (1 - a)
    out = np.empty_like(frame)
    out[..., :3] = np.clip(rgb + 0.5, 0, 255)
    out[..., 3] = 255
    return out


def _draw_pins(frame: np.ndarray, anim: PinAnimation, i: int) -> np.ndarray:
    im = Image.fromarray(frame, "RGBA")
    d = ImageDraw.Draw(im)
    r = max(3, anim.height // 160)
    for p in anim.pins:
        x, y = p.track[i][0] * anim.width, (1 - p.track[i][1]) * anim.height
        d.ellipse([x - r - 1, y - r - 1, x + r + 1, y + r + 1], fill=(0, 0, 0, 255))
        d.ellipse([x - r, y - r, x + r, y + r], fill=(255, 217, 26, 255))
    return np.asarray(im)


def render_preview(anim: PinAnimation, stage_dir: Path, out_mp4: Path, work_dir: Path,
                   draw_pins: bool = True, progress: Optional[Callable[[float], None]] = None) -> dict:
    """MP4 preview. Returns {"kind": "warp"|"skeleton", "path": ...}."""
    ffmpeg = find_ffmpeg()
    label = "TEST - AI DEGIL" if not anim.provenance.is_ai else None
    renderer = find_puppet_render()
    enc = _ffmpeg_mp4(ffmpeg, anim.width, anim.height, anim.fps, out_mp4)
    kind = "warp" if renderer else "skeleton"
    try:
        frames = composed_frames(anim, stage_dir, work_dir, renderer) if renderer else skeleton_frames(anim, stage_dir)
        for i, frame in frames:
            frame = _over_background(frame)
            if draw_pins and renderer:
                frame = _draw_pins(frame, anim, i)
            if label:
                frame = _watermark(frame, label)
            elif kind == "skeleton":
                frame = _watermark(frame, "ISKELET ONIZLEME - puppet_render yok, bukme gosterilmiyor")
            enc.stdin.write(frame.tobytes())
            if progress:
                progress((i + 1) / anim.num_frames)
    finally:
        enc.stdin.close()
        code = enc.wait()
    if code != 0:
        raise PreviewError(f"ffmpeg MP4 yazamadı ({code})")
    return {"kind": kind, "path": str(out_mp4)}


def bake_png_sequence(anim: PinAnimation, stage_dir: Path, out_dir: Path, work_dir: Path,
                      progress: Optional[Callable[[float], None]] = None) -> list[Path]:
    """Transparent PNG frames (for VEGAS without the Puppet Pin plug-in)."""
    out_dir.mkdir(parents=True, exist_ok=True)
    paths = []
    for i, frame in composed_frames(anim, stage_dir, work_dir):
        p = out_dir / f"frame_{i:05d}.png"
        Image.fromarray(frame, "RGBA").save(p, compress_level=1)
        paths.append(p)
        if progress:
            progress((i + 1) / anim.num_frames)
    if len(paths) != anim.num_frames:
        raise PreviewError("PNG karelerin hepsi yazılamadı")
    return paths


def skeleton_frames(anim: PinAnimation, stage_dir: Path):
    """Static picture with the moving skeleton drawn over it (no warp)."""
    base = Image.new("RGBA", (anim.width, anim.height), (36, 36, 40, 255))
    stage = Image.open(stage_dir / (anim.stage_image or "stage.png")).convert("RGBA")
    base.alpha_composite(stage)
    by_joint = {p.joint: p for p in anim.pins}
    for i in range(anim.num_frames):
        im = base.copy()
        d = ImageDraw.Draw(im)

        def px(j):
            u, v = by_joint[j].track[i]
            return u * anim.width, (1 - v) * anim.height

        for c, p in RIG_PARENT.items():
            if p and c in by_joint and p in by_joint:
                d.line([px(p), px(c)], fill=(255, 210, 40, 255), width=max(2, anim.height // 240))
        for j in by_joint:
            x, y = px(j)
            r = max(3, anim.height // 180)
            d.ellipse([x - r, y - r, x + r, y + r], fill=(255, 80, 60, 255))
        yield i, np.asarray(im)


def main(argv=None) -> int:
    """python -m aimotion.preview <folder with pins.json and the stage PNGs> out.mp4"""
    argv = argv or sys.argv[1:]
    if len(argv) != 2:
        print(main.__doc__)
        return 2
    folder = Path(argv[0])
    anim = PinAnimation.load(folder / "pins.json")
    out = Path(argv[1])
    print(render_preview(anim, folder, out, out.parent / "_preview_work"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
