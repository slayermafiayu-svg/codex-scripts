// Time model: a single source of truth for frame <-> seconds conversions.
// Frame rate is kept as a rational number (num/den) so that 29.97 is really
// 30000/1001 and never a rounded float. Every conversion in the editor goes
// through this module.

const KNOWN_RATES = [
  [23.976, 24000, 1001],
  [29.97, 30000, 1001],
  [47.952, 48000, 1001],
  [59.94, 60000, 1001],
  [119.88, 120000, 1001],
];

function gcd(a, b) {
  a = Math.abs(a);
  b = Math.abs(b);
  while (b) [a, b] = [b, a % b];
  return a || 1;
}

/** @typedef {{num:number, den:number}} Fps */

/**
 * Build a normalized rational frame rate.
 * @param {number} num
 * @param {number} [den]
 * @returns {Fps}
 */
export function makeFps(num, den = 1) {
  if (!Number.isFinite(num) || !Number.isFinite(den) || num <= 0 || den <= 0) {
    throw new RangeError(`Invalid frame rate ${num}/${den}`);
  }
  if (!Number.isInteger(num) || !Number.isInteger(den)) {
    return fpsFromNumber(num / den);
  }
  const g = gcd(num, den);
  return { num: num / g, den: den / g };
}

/**
 * Convert a possibly rounded float rate (23.976, 29.97, 25, 60) to a rational.
 * Known NTSC-style rates map to their exact 1001 denominators.
 * @param {number} value
 * @returns {Fps}
 */
export function fpsFromNumber(value) {
  if (!Number.isFinite(value) || value <= 0) throw new RangeError(`Invalid frame rate ${value}`);
  for (const [approx, num, den] of KNOWN_RATES) {
    if (Math.abs(value - approx) < 0.002 || Math.abs(value - num / den) < 1e-6) return { num, den };
  }
  if (Number.isInteger(value)) return { num: value, den: 1 };
  // Fall back to a small-denominator continued fraction approximation.
  let bestNum = Math.round(value);
  let bestDen = 1;
  let bestErr = Math.abs(value - bestNum);
  for (let den = 2; den <= 1001; den++) {
    const num = Math.round(value * den);
    const err = Math.abs(value - num / den);
    if (err < bestErr - 1e-12) {
      bestErr = err;
      bestNum = num;
      bestDen = den;
      if (err < 1e-9) break;
    }
  }
  return makeFps(bestNum, bestDen);
}

/** @param {Fps} fps */
export function fpsToNumber(fps) {
  return fps.num / fps.den;
}

/** Duration of one frame in seconds. @param {Fps} fps */
export function frameDuration(fps) {
  return fps.den / fps.num;
}

/** Exact (fractional) frame position for a time. @param {number} t @param {Fps} fps */
export function secondsToFrames(t, fps) {
  return (t * fps.num) / fps.den;
}

/** Time of frame n (n may be fractional). @param {number} n @param {Fps} fps */
export function framesToSeconds(n, fps) {
  return (n * fps.den) / fps.num;
}

/**
 * Tolerance used when comparing times: a small fraction of a frame.
 * Host time resolution is far finer than this, so true overflows are not hidden.
 * @param {Fps} fps
 */
export function timeEpsilon(fps) {
  return frameDuration(fps) * 1e-4;
}

/** Nearest frame index for a time. */
export function nearestFrame(t, fps) {
  return Math.round(secondsToFrames(t, fps));
}

/** Snap a time to the nearest frame boundary and return it in seconds. */
export function snapToFrame(t, fps) {
  return framesToSeconds(nearestFrame(t, fps), fps);
}

/** True when t sits on a frame boundary (within tolerance). */
export function isOnFrame(t, fps) {
  const f = secondsToFrames(t, fps);
  return Math.abs(f - Math.round(f)) < 1e-4;
}

/**
 * Index of the frame that is displayed at time t (floor with tolerance).
 * Used for ruler labels; never used to create keyframes.
 */
export function displayedFrame(t, fps) {
  return Math.floor(secondsToFrames(t, fps) + 1e-6);
}

/**
 * Visible frames of a range [start, end): the last displayable frame is the one
 * strictly before the end boundary. 6 s at 30 fps -> frames 0..179, boundary at 6.000.
 * @param {{start:number,end:number}} range
 * @param {Fps} fps
 */
export function visibleFrames(range, fps) {
  const total = secondsToFrames(range.end - range.start, fps);
  const count = Math.max(0, Math.ceil(total - 1e-6));
  return { count, firstFrame: 0, lastFrame: count - 1, boundary: range.end };
}

/**
 * Whether a keyframe time fits inside the range. The end boundary itself is a
 * legal keyframe time (it holds the mathematical end value) even though no
 * frame is rendered there.
 */
export function fitsInRange(t, range, fps) {
  const eps = timeEpsilon(fps);
  return t >= range.start - eps && t <= range.end + eps;
}

function pad2(n) {
  return n < 10 ? `0${n}` : String(n);
}

/**
 * Format a time for display.
 * @param {number} t seconds
 * @param {Fps} fps
 * @param {'frames'|'seconds'|'timecode'} mode
 * @param {{showSubframe?:boolean}} [opts]
 */
export function formatTime(t, fps, mode = 'timecode', opts = {}) {
  const neg = t < 0;
  const abs = Math.abs(t);
  const exactFrames = secondsToFrames(abs, fps);
  const sub = exactFrames - Math.round(exactFrames);
  const frames = Math.round(exactFrames);
  const subMark = opts.showSubframe && Math.abs(sub) > 1e-4 ? '*' : '';
  const sign = neg ? '-' : '';
  if (mode === 'frames') return `${sign}${frames}${subMark}`;
  if (mode === 'seconds') {
    return `${sign}${abs.toFixed(3)}s`;
  }
  // Non-drop-frame timecode. Frames per second label uses the ceiling (29.97 -> 30).
  const fpsInt = Math.ceil(fps.num / fps.den - 1e-9);
  const totalSeconds = Math.floor(frames / fpsInt);
  const ff = frames - totalSeconds * fpsInt;
  const hh = Math.floor(totalSeconds / 3600);
  const mm = Math.floor((totalSeconds % 3600) / 60);
  const ss = totalSeconds % 60;
  const head = hh > 0 ? `${pad2(hh)}:${pad2(mm)}:${pad2(ss)}` : `${pad2(mm)}:${pad2(ss)}`;
  return `${sign}${head}:${pad2(ff)}${subMark}`;
}

/**
 * Parse user input into seconds. Accepts "120" (frames), "2.5s"/"2,5s" (seconds),
 * "00:02:12" / "02:12" (timecode mm:ss:ff), "+12" (relative frames, when base given).
 * Comma decimals are accepted. Returns null for invalid input (never silently 0).
 * @param {string} text
 * @param {Fps} fps
 * @param {{mode?:'frames'|'seconds'|'timecode', base?:number}} [opts]
 * @returns {number|null}
 */
export function parseTime(text, fps, opts = {}) {
  if (text == null) return null;
  let s = String(text).trim().replace(/\s+/g, '');
  if (!s) return null;
  let relative = 0;
  if (s[0] === '+' || (s[0] === '-' && opts.base != null && /^[+-]\d/.test(s) && opts.relative)) {
    relative = s[0] === '-' ? -1 : 1;
    s = s.slice(1);
  }
  const unitSeconds = /s$/i.test(s) && !/:/.test(s);
  const unitFrames = /f$/i.test(s);
  s = s.replace(/[sf]$/i, '');
  let seconds;
  if (s.includes(':')) {
    const parts = s.split(':');
    if (parts.length < 2 || parts.length > 4 || parts.some((p) => !/^\d+$/.test(p))) return null;
    const nums = parts.map(Number);
    const fpsInt = Math.ceil(fps.num / fps.den - 1e-9);
    let frames = 0;
    // hh:mm:ss:ff, mm:ss:ff, ss:ff
    const ff = nums.pop();
    if (ff >= fpsInt) return null;
    let mult = 1;
    let totalSec = 0;
    while (nums.length) {
      totalSec += nums.pop() * mult;
      mult *= 60;
    }
    frames = totalSec * fpsInt + ff;
    seconds = framesToSeconds(frames, fps);
  } else {
    const normalized = s.replace(',', '.');
    if (!/^-?\d*\.?\d+$/.test(normalized)) return null;
    const value = Number(normalized);
    if (!Number.isFinite(value)) return null;
    const mode = unitSeconds ? 'seconds' : unitFrames ? 'frames' : opts.mode || 'frames';
    seconds = mode === 'seconds' ? value : framesToSeconds(value, fps);
  }
  if (relative) {
    const base = opts.base || 0;
    return base + relative * seconds;
  }
  return seconds;
}

/**
 * Parse a numeric value typed by the user. Accepts comma or dot decimals,
 * optional unit suffix. Returns null when invalid.
 * @param {string} text
 * @returns {number|null}
 */
export function parseNumber(text) {
  if (text == null) return null;
  let s = String(text).trim().replace(/\s+/g, '');
  if (!s) return null;
  s = s.replace(/[°%a-zA-Z]+$/u, '');
  // Accept "1.234,5" (tr) and "1,234.5" (en) thousand separators conservatively:
  if (/^-?\d{1,3}(\.\d{3})+,\d+$/.test(s)) s = s.replace(/\./g, '').replace(',', '.');
  else if (/^-?\d{1,3}(,\d{3})+\.\d+$/.test(s)) s = s.replace(/,/g, '');
  else s = s.replace(',', '.');
  if (!/^-?(\d+\.?\d*|\.\d+)(e-?\d+)?$/i.test(s)) return null;
  const v = Number(s);
  return Number.isFinite(v) ? v : null;
}

const STEP_LADDER = [1, 2, 5];

/**
 * Pick a readable ruler step for the current zoom.
 * Returns the major step in seconds and the minor subdivision, preferring
 * whole frames while zoomed in and whole seconds/minutes when zoomed out.
 * @param {number} pxPerSecond
 * @param {Fps} fps
 * @param {number} [minMajorPx] minimum pixel distance between labelled ticks
 */
export function chooseTimeStep(pxPerSecond, fps, minMajorPx = 72) {
  const fd = frameDuration(fps);
  const pxPerFrame = pxPerSecond * fd;
  // Frame-level steps: 1,2,5,10,... frames while a labelled frame step still
  // represents less than a second.
  const frameSteps = [];
  for (let p = 1; p < fps.num / fps.den; p *= 10) {
    for (const m of STEP_LADDER) {
      const n = p * m;
      if (n < fps.num / fps.den) frameSteps.push(n);
    }
  }
  for (const n of frameSteps) {
    if (pxPerFrame * n >= minMajorPx) {
      const minor = n >= 10 ? n / 5 : n >= 5 ? 1 : 1;
      return { major: framesToSeconds(n, fps), minor: framesToSeconds(minor, fps), unit: 'frame', majorFrames: n };
    }
  }
  // Second-level steps.
  const secondSteps = [1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600];
  for (const s of secondSteps) {
    if (pxPerSecond * s >= minMajorPx) {
      let minor;
      if (s === 1) minor = pxPerFrame >= 4 ? fd : 0.5;
      else if (s === 2) minor = 0.5;
      else if (s === 5) minor = 1;
      else if (s === 10) minor = 2;
      else if (s === 15) minor = 5;
      else if (s === 30) minor = 10;
      else if (s === 60) minor = 15;
      else minor = s / 4;
      return { major: s, minor, unit: 'second', majorFrames: secondsToFrames(s, fps) };
    }
  }
  let s = 3600;
  while (pxPerSecond * s < minMajorPx) s *= 2;
  return { major: s, minor: s / 4, unit: 'second', majorFrames: secondsToFrames(s, fps) };
}

/**
 * Pick a readable value grid step (1, 2, 5 x 10^k) for the current vertical zoom.
 * @param {number} pxPerUnit
 * @param {number} [minMajorPx]
 */
export function chooseValueStep(pxPerUnit, minMajorPx = 40) {
  if (!(pxPerUnit > 0) || !Number.isFinite(pxPerUnit)) return { major: 1, minor: 0.5, decimals: 0 };
  const minUnits = minMajorPx / pxPerUnit;
  const exp = Math.floor(Math.log10(minUnits));
  const base = Math.pow(10, exp);
  let major = base * 10;
  for (const m of STEP_LADDER) {
    if (base * m >= minUnits) {
      major = base * m;
      break;
    }
  }
  const minor = major / (major / base === 2 ? 4 : 5);
  const decimals = Math.max(0, -Math.floor(Math.log10(major) + 1e-9));
  return { major, minor, decimals };
}

/**
 * Format a value with the precision implied by a grid step.
 * @param {number} v
 * @param {number} [decimals]
 */
export function formatValue(v, decimals = 2) {
  if (!Number.isFinite(v)) return '—';
  const fixed = v.toFixed(Math.min(6, Math.max(0, decimals)));
  // Trim trailing zeros but keep at least the integer part.
  return fixed.replace(/\.?0+$/, (m) => (m.startsWith('.') ? '' : m)).replace(/^-0$/, '0');
}
