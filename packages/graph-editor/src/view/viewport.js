// Screen <-> world mapping for the plot. Time maps to x, plot value maps to y.
// "Plot value" is the channel value after an optional per-channel normalize
// transform, so the same viewport serves value, normalized and speed modes.

const MIN_PX_PER_SEC = 0.5;
const MAX_PX_PER_SEC = 60000;
const MIN_PX_PER_UNIT = 1e-6;
const MAX_PX_PER_UNIT = 1e7;

function clamp(x, lo, hi) {
  return Math.min(hi, Math.max(lo, x));
}

export class Viewport {
  constructor() {
    this.width = 800;
    this.height = 400;
    this.rulerHeight = 24;
    this.t0 = -0.5; // time at x = 0
    this.pxPerSec = 120;
    this.vTop = 12; // plot value at plot top
    this.pxPerUnit = 20;
  }

  get plotTop() {
    return this.rulerHeight;
  }

  get plotHeight() {
    return Math.max(1, this.height - this.rulerHeight);
  }

  get t1() {
    return this.t0 + this.width / this.pxPerSec;
  }

  get vBottom() {
    return this.vTop - this.plotHeight / this.pxPerUnit;
  }

  resize(width, height) {
    // Keep the left time and the top value anchored; the visible span grows.
    this.width = Math.max(1, width);
    this.height = Math.max(1, height);
  }

  xOf(t) {
    return (t - this.t0) * this.pxPerSec;
  }

  tOf(x) {
    return this.t0 + x / this.pxPerSec;
  }

  yOf(v) {
    return this.plotTop + (this.vTop - v) * this.pxPerUnit;
  }

  vOf(y) {
    return this.vTop - (y - this.plotTop) / this.pxPerUnit;
  }

  zoomTime(factor, anchorX) {
    const t = this.tOf(anchorX);
    this.pxPerSec = clamp(this.pxPerSec * factor, MIN_PX_PER_SEC, MAX_PX_PER_SEC);
    this.t0 = t - anchorX / this.pxPerSec;
  }

  setPxPerSec(pxPerSec, anchorX) {
    const t = this.tOf(anchorX);
    this.pxPerSec = clamp(pxPerSec, MIN_PX_PER_SEC, MAX_PX_PER_SEC);
    this.t0 = t - anchorX / this.pxPerSec;
  }

  zoomValue(factor, anchorY) {
    const v = this.vOf(anchorY);
    this.pxPerUnit = clamp(this.pxPerUnit * factor, MIN_PX_PER_UNIT, MAX_PX_PER_UNIT);
    this.vTop = v + (anchorY - this.plotTop) / this.pxPerUnit;
  }

  pan(dxPx, dyPx) {
    this.t0 -= dxPx / this.pxPerSec;
    this.vTop += dyPx / this.pxPerUnit;
  }

  /** Fit a time span with pixel padding on both sides. */
  fitTime(tMin, tMax, padPx = 48) {
    if (!Number.isFinite(tMin) || !Number.isFinite(tMax)) return;
    if (tMax - tMin < 1e-9) {
      tMin -= 0.5;
      tMax += 0.5;
    }
    const usable = Math.max(10, this.width - padPx * 2);
    this.pxPerSec = clamp(usable / (tMax - tMin), MIN_PX_PER_SEC, MAX_PX_PER_SEC);
    this.t0 = tMin - padPx / this.pxPerSec;
  }

  /** Fit a value span with pixel padding top and bottom. */
  fitValue(vMin, vMax, padPx = 32) {
    if (!Number.isFinite(vMin) || !Number.isFinite(vMax)) return;
    if (vMax - vMin < 1e-9) {
      const m = Math.max(1, Math.abs(vMax) * 0.1);
      vMin -= m;
      vMax += m;
    }
    const usable = Math.max(10, this.plotHeight - padPx * 2);
    this.pxPerUnit = clamp(usable / (vMax - vMin), MIN_PX_PER_UNIT, MAX_PX_PER_UNIT);
    this.vTop = vMax + padPx / this.pxPerUnit;
  }

  /** Scroll so that time t is visible (used by "go to key"). */
  ensureTimeVisible(t, marginPx = 24) {
    const x = this.xOf(t);
    if (x < marginPx) this.t0 = t - marginPx / this.pxPerSec;
    else if (x > this.width - marginPx) this.t0 = t - (this.width - marginPx) / this.pxPerSec;
  }

  snapshot() {
    return { t0: this.t0, pxPerSec: this.pxPerSec, vTop: this.vTop, pxPerUnit: this.pxPerUnit };
  }

  restore(s) {
    Object.assign(this, s);
  }
}
