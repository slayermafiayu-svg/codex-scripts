// Canvas renderer. Pure drawing: reads editor state, writes pixels. Nothing
// here mutates the document. Everything is drawn in CSS pixels on a canvas
// scaled by devicePixelRatio.

import { INTERP, TANGENTS, evalKeys, speedKeys, segmentControlPoints, curveBounds } from '../core/curve.js';
import { chooseTimeStep, chooseValueStep, formatTime, formatValue, framesToSeconds, visibleFrames, secondsToFrames } from '../core/time.js';

export const KEY_RADIUS = 5;
export const HANDLE_RADIUS = 4;
export const HIT_KEY = 9;
export const HIT_HANDLE = 8;
export const HIT_SEGMENT = 6;
export const BOX_HANDLE = 6;

const DEFAULT_THEME = {
  bg: '#14161b',
  plotBg: '#171a20',
  outside: 'rgba(0,0,0,0.28)',
  gridMinor: 'rgba(255,255,255,0.045)',
  gridMajor: 'rgba(255,255,255,0.10)',
  gridZero: 'rgba(255,255,255,0.22)',
  rulerBg: '#1c2027',
  rulerText: '#aab2bf',
  text: '#e6e9ef',
  textDim: '#8b94a3',
  accent: '#58b4ff',
  accentSoft: 'rgba(88,180,255,0.22)',
  selection: '#ffffff',
  playhead: '#ff5a5a',
  range: '#f2c14e',
  danger: '#ff6b6b',
  ghost: 'rgba(255,255,255,0.28)',
  handle: '#dfe6f1',
  handleLine: 'rgba(223,230,241,0.55)',
  boxFill: 'rgba(88,180,255,0.10)',
  boxStroke: 'rgba(88,180,255,0.8)',
  tooltipBg: 'rgba(20,22,27,0.92)',
  font: '11px ui-sans-serif, system-ui, -apple-system, "Segoe UI", Roboto, sans-serif',
  mono: '11px ui-monospace, "SF Mono", Menlo, Consolas, monospace',
};

/** Read theme tokens from CSS custom properties, falling back to defaults. */
export function readTheme(el) {
  const theme = { ...DEFAULT_THEME };
  if (!el || typeof getComputedStyle !== 'function') return theme;
  const cs = getComputedStyle(el);
  const map = {
    bg: '--ge-canvas-bg',
    plotBg: '--ge-plot-bg',
    outside: '--ge-outside',
    gridMinor: '--ge-grid-minor',
    gridMajor: '--ge-grid-major',
    gridZero: '--ge-grid-zero',
    rulerBg: '--ge-ruler-bg',
    rulerText: '--ge-ruler-text',
    text: '--ge-text',
    textDim: '--ge-text-dim',
    accent: '--ge-accent',
    accentSoft: '--ge-accent-soft',
    selection: '--ge-selection',
    playhead: '--ge-playhead',
    range: '--ge-range',
    danger: '--ge-danger',
    ghost: '--ge-ghost',
    handle: '--ge-handle',
    handleLine: '--ge-handle-line',
    boxFill: '--ge-box-fill',
    boxStroke: '--ge-box-stroke',
    tooltipBg: '--ge-tooltip-bg',
  };
  for (const [k, v] of Object.entries(map)) {
    const val = cs.getPropertyValue(v).trim();
    if (val) theme[k] = val;
  }
  const font = cs.getPropertyValue('--ge-font').trim();
  if (font) theme.font = `11px ${font}`;
  const mono = cs.getPropertyValue('--ge-font-mono').trim();
  if (mono) theme.mono = `11px ${mono}`;
  return theme;
}

/**
 * Per-channel transform from channel value to plot value.
 * value mode: identity; normalize: (v - min) / (max - min); speed: identity on speed.
 */
export function channelTransform(channel, mode, normalize) {
  if (mode === 'value' && normalize) {
    const b = curveBounds(channel.keys, 8);
    if (!b) return { offset: 0, scale: 1 };
    const span = b.vMax - b.vMin;
    if (span < 1e-9) return { offset: b.vMin - 0.5, scale: 1 };
    return { offset: b.vMin, scale: 1 / span };
  }
  return { offset: 0, scale: 1 };
}

/** Plot value of a channel at time t for the current mode. */
export function plotValueAt(channel, t, mode, tf) {
  if (mode === 'speed') return speedKeys(channel.keys, t);
  return (evalKeys(channel.keys, t, channel.extrapolation) - tf.offset) * tf.scale;
}

/** Plot value of a key itself. In speed mode a key sits at its outgoing speed (or incoming for the last key). */
export function keyPlotValue(channel, index, mode, tf) {
  const k = channel.keys[index];
  if (mode === 'speed') return keySpeeds(channel, index).out;
  return (k.v - tf.offset) * tf.scale;
}

/** Incoming and outgoing speeds of a key (units/second). */
export function keySpeeds(channel, index) {
  const keys = channel.keys;
  const k = keys[index];
  let inS = 0;
  let outS = 0;
  if (index > 0) {
    const p = keys[index - 1];
    if (p.interp === INTERP.HOLD) inS = 0;
    else if (p.interp === INTERP.LINEAR) inS = (k.v - p.v) / Math.max(1e-12, k.t - p.t);
    else inS = k.in.dt < 0 ? k.in.dv / k.in.dt : speedKeys(keys, k.t - 1e-9);
  }
  if (index < keys.length - 1) {
    const n = keys[index + 1];
    if (k.interp === INTERP.HOLD) outS = 0;
    else if (k.interp === INTERP.LINEAR) outS = (n.v - k.v) / Math.max(1e-12, n.t - k.t);
    else outS = k.out.dt > 0 ? k.out.dv / k.out.dt : speedKeys(keys, k.t + 1e-9);
  } else outS = inS;
  if (index === 0 && keys.length > 1) inS = outS;
  return { in: inS, out: outS };
}

/**
 * Screen positions of a key's handles for hit testing and drawing.
 * Returns {in:{x,y}|null, out:{x,y}|null}.
 */
export function handleScreenPositions(vp, channel, index, mode, tf) {
  const keys = channel.keys;
  const k = keys[index];
  const res = { in: null, out: null };
  if (mode === 'speed') {
    const sp = keySpeeds(channel, index);
    const seg = index > 0 ? keys[index - 1] : null;
    if (seg && seg.interp === INTERP.BEZIER) res.in = { x: vp.xOf(k.t + k.in.dt), y: vp.yOf(sp.in) };
    if (index < keys.length - 1 && k.interp === INTERP.BEZIER) res.out = { x: vp.xOf(k.t + k.out.dt), y: vp.yOf(sp.out) };
    return res;
  }
  const prev = index > 0 ? keys[index - 1] : null;
  if (prev && prev.interp === INTERP.BEZIER) res.in = { x: vp.xOf(k.t + k.in.dt), y: vp.yOf((k.v + k.in.dv - tf.offset) * tf.scale) };
  if (index < keys.length - 1 && k.interp === INTERP.BEZIER)
    res.out = { x: vp.xOf(k.t + k.out.dt), y: vp.yOf((k.v + k.out.dv - tf.offset) * tf.scale) };
  return res;
}

function drawKeyShape(ctx, x, y, r, key) {
  ctx.beginPath();
  if (key.interp === INTERP.HOLD) {
    ctx.rect(x - r, y - r, r * 2, r * 2);
  } else if (key.interp === INTERP.LINEAR) {
    ctx.moveTo(x - r, y - r);
    ctx.lineTo(x + r, y);
    ctx.lineTo(x - r, y + r);
    ctx.closePath();
  } else if (key.tangents === TANGENTS.AUTO) {
    ctx.arc(x, y, r, 0, Math.PI * 2);
  } else {
    ctx.moveTo(x, y - r);
    ctx.lineTo(x + r, y);
    ctx.lineTo(x, y + r);
    ctx.lineTo(x - r, y);
    ctx.closePath();
  }
}

function roundRect(ctx, x, y, w, h, r) {
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.arcTo(x + w, y, x + w, y + h, r);
  ctx.arcTo(x + w, y + h, x, y + h, r);
  ctx.arcTo(x, y + h, x, y, r);
  ctx.arcTo(x, y, x + w, y, r);
  ctx.closePath();
}

function label(ctx, text, x, y, theme, { align = 'left', bg = true, color = theme.text, font = theme.mono } = {}) {
  ctx.font = font;
  ctx.textBaseline = 'middle';
  const w = ctx.measureText(text).width;
  let lx = x;
  if (align === 'right') lx = x - w;
  else if (align === 'center') lx = x - w / 2;
  if (bg) {
    ctx.fillStyle = theme.tooltipBg;
    roundRect(ctx, lx - 4, y - 8, w + 8, 16, 3);
    ctx.fill();
  }
  ctx.fillStyle = color;
  ctx.textAlign = 'left';
  ctx.fillText(text, lx, y);
  return { x: lx - 4, y: y - 8, w: w + 8, h: 16 };
}

/**
 * Draw one channel curve across the visible width.
 * @returns {void}
 */
function drawCurve(ctx, vp, channel, mode, tf, style, step = 1) {
  const keys = channel.keys;
  if (!keys.length) return;
  const { color, width, alpha, dashOutside } = style;
  const firstX = vp.xOf(keys[0].t);
  const lastX = vp.xOf(keys[keys.length - 1].t);
  const xStart = Math.max(0, Math.floor(firstX));
  const xEnd = Math.min(vp.width, Math.ceil(lastX));
  ctx.lineWidth = width;
  ctx.strokeStyle = color;
  ctx.globalAlpha = alpha;
  ctx.lineJoin = 'round';
  ctx.lineCap = 'round';

  const path = (x0, x1, dashed) => {
    if (x1 < x0) return;
    ctx.setLineDash(dashed ? [4, 4] : []);
    ctx.beginPath();
    let first = true;
    for (let x = x0; x <= x1; x += step) {
      const t = vp.tOf(x);
      const y = vp.yOf(plotValueAt(channel, t, mode, tf));
      if (!Number.isFinite(y)) continue;
      const cy = Math.max(-1e5, Math.min(1e5, y));
      if (first) {
        ctx.moveTo(x, cy);
        first = false;
      } else ctx.lineTo(x, cy);
    }
    // Finish exactly at x1 so the curve ends on the key.
    const yEnd = vp.yOf(plotValueAt(channel, vp.tOf(x1), mode, tf));
    if (!first && Number.isFinite(yEnd)) ctx.lineTo(x1, Math.max(-1e5, Math.min(1e5, yEnd)));
    ctx.stroke();
  };

  if (keys.length === 1) {
    const y = vp.yOf(mode === 'speed' ? 0 : (keys[0].v - tf.offset) * tf.scale);
    ctx.setLineDash([4, 4]);
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(vp.width, y);
    ctx.stroke();
  } else {
    if (dashOutside && firstX > 0) path(0, Math.min(vp.width, firstX), true);
    path(xStart, xEnd, false);
    if (dashOutside && lastX < vp.width) path(Math.max(0, lastX), vp.width, true);
  }
  // Hold segments: draw the vertical jump as a dotted connector for clarity.
  ctx.setLineDash([2, 3]);
  for (let i = 0; i < keys.length - 1; i++) {
    if (keys[i].interp !== INTERP.HOLD || mode === 'speed') continue;
    const x = vp.xOf(keys[i + 1].t);
    if (x < -2 || x > vp.width + 2) continue;
    ctx.beginPath();
    ctx.moveTo(x, vp.yOf((keys[i].v - tf.offset) * tf.scale));
    ctx.lineTo(x, vp.yOf((keys[i + 1].v - tf.offset) * tf.scale));
    ctx.stroke();
  }
  ctx.setLineDash([]);
  ctx.globalAlpha = 1;
}

function drawSegmentHighlight(ctx, vp, channel, k0, k1, mode, tf, color, width) {
  const x0 = Math.max(0, vp.xOf(k0.t));
  const x1 = Math.min(vp.width, vp.xOf(k1.t));
  if (x1 <= x0) return;
  ctx.save();
  ctx.lineWidth = width;
  ctx.strokeStyle = color;
  ctx.lineCap = 'round';
  ctx.beginPath();
  for (let x = x0; x <= x1; x += 1) {
    const y = vp.yOf(plotValueAt(channel, vp.tOf(x), mode, tf));
    if (x === x0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  }
  ctx.stroke();
  ctx.restore();
}

/**
 * Main render entry.
 * @param {CanvasRenderingContext2D} ctx
 * @param {object} s render state
 */
export function render(ctx, s) {
  const {
    vp,
    store,
    mode,
    normalize,
    theme,
    hover,
    drag,
    playhead,
    timeFormat,
    showHandles,
    dpr,
    boxSelect,
    ghost,
    snapLine,
    blockedAt,
    pulse,
  } = s;
  const doc = store.doc;
  const fps = doc.fps;
  const W = vp.width;
  const H = vp.height;
  const plotTop = vp.plotTop;

  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, W, H);
  ctx.fillStyle = theme.plotBg;
  ctx.fillRect(0, plotTop, W, H - plotTop);

  // ---- outside-range shading
  const xs = vp.xOf(doc.range.start);
  const xe = vp.xOf(doc.range.end);
  ctx.fillStyle = theme.outside;
  if (xs > 0) ctx.fillRect(0, plotTop, Math.min(W, xs), H - plotTop);
  if (xe < W) ctx.fillRect(Math.max(0, xe), plotTop, W - Math.max(0, xe), H - plotTop);

  // ---- value grid
  const vStep = chooseValueStep(vp.pxPerUnit, 44);
  const vMin = vp.vBottom;
  const vMax = vp.vTop;
  ctx.lineWidth = 1;
  const vFirst = Math.floor(vMin / vStep.minor) * vStep.minor;
  const majorEvery = Math.round(vStep.major / vStep.minor);
  let vi = 0;
  const valueLabels = [];
  for (let v = vFirst; v <= vMax + 1e-9; v += vStep.minor, vi++) {
    const y = Math.round(vp.yOf(v)) + 0.5;
    if (y < plotTop || y > H) continue;
    const isMajor = Math.abs(v / vStep.major - Math.round(v / vStep.major)) < 1e-6;
    const isZero = Math.abs(v) < vStep.minor * 1e-6;
    ctx.strokeStyle = isZero ? theme.gridZero : isMajor ? theme.gridMajor : theme.gridMinor;
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(W, y);
    ctx.stroke();
    if (isMajor) valueLabels.push({ v, y });
  }
  void majorEvery;

  // ---- time grid
  const tStep = chooseTimeStep(vp.pxPerSec, fps, 72);
  const tFirst = Math.floor(vp.t0 / tStep.minor) * tStep.minor;
  const majorRatio = Math.round(tStep.major / tStep.minor);
  const frameDur = framesToSeconds(1, fps);
  const pxPerFrame = vp.pxPerSec * frameDur;
  const drawFrameLines = pxPerFrame >= 9 && tStep.minor > frameDur * 1.5;
  if (drawFrameLines) {
    ctx.strokeStyle = theme.gridMinor;
    ctx.globalAlpha = 0.6;
    const f0 = Math.floor(secondsToFrames(vp.t0, fps));
    const f1 = Math.ceil(secondsToFrames(vp.t1, fps));
    for (let f = f0; f <= f1; f++) {
      const x = Math.round(vp.xOf(framesToSeconds(f, fps))) + 0.5;
      ctx.beginPath();
      ctx.moveTo(x, plotTop);
      ctx.lineTo(x, H);
      ctx.stroke();
    }
    ctx.globalAlpha = 1;
  }
  const timeTicks = [];
  for (let i = 0, t = tFirst; t <= vp.t1 + tStep.minor; i++, t = tFirst + i * tStep.minor) {
    const x = Math.round(vp.xOf(t)) + 0.5;
    if (x < -1 || x > W + 1) continue;
    const isMajor = Math.abs(t / tStep.major - Math.round(t / tStep.major)) < 1e-6;
    ctx.strokeStyle = isMajor ? theme.gridMajor : theme.gridMinor;
    ctx.beginPath();
    ctx.moveTo(x, plotTop);
    ctx.lineTo(x, H);
    ctx.stroke();
    timeTicks.push({ t, x, isMajor });
  }
  void majorRatio;

  // ---- range boundaries
  ctx.save();
  ctx.strokeStyle = theme.range;
  ctx.lineWidth = 1;
  const drawBoundary = (x, dashed) => {
    if (x < -1 || x > W + 1) return;
    ctx.setLineDash(dashed ? [6, 4] : []);
    ctx.beginPath();
    ctx.moveTo(Math.round(x) + 0.5, plotTop);
    ctx.lineTo(Math.round(x) + 0.5, H);
    ctx.stroke();
  };
  drawBoundary(xs, false);
  drawBoundary(xe, true);
  // last displayable frame tick
  const vf = visibleFrames(doc.range, fps);
  if (vf.count > 0) {
    const xl = vp.xOf(doc.range.start + framesToSeconds(vf.lastFrame, fps));
    if (pxPerFrame >= 6 && xl > 0 && xl < W) {
      ctx.setLineDash([]);
      ctx.globalAlpha = 0.6;
      ctx.beginPath();
      ctx.moveTo(Math.round(xl) + 0.5, plotTop);
      ctx.lineTo(Math.round(xl) + 0.5, plotTop + 10);
      ctx.stroke();
      ctx.globalAlpha = 1;
    }
  }
  ctx.restore();

  // ---- ghost curves (pre-drag state)
  if (ghost) {
    for (const ch of ghost.channels) {
      if (!ch.visible) continue;
      const tf = channelTransform(ch, mode, normalize);
      drawCurve(ctx, vp, ch, mode, tf, { color: theme.ghost, width: 1, alpha: 0.7, dashOutside: false }, 2);
    }
  }

  // ---- curves
  const channels = doc.channels.filter((c) => c.visible);
  const tfs = new Map();
  for (const ch of channels) tfs.set(ch.id, channelTransform(ch, mode, normalize));
  const active = store.activeChannelId;
  const anySelectedIn = new Set();
  for (const id of store.selectedKeys) {
    const ref = store.keyRef(id);
    if (ref) anySelectedIn.add(ref.channel.id);
  }
  // Draw inactive first so the active channel sits on top.
  const ordered = [...channels].sort((a, b) => (a.id === active) - (b.id === active));
  for (const ch of ordered) {
    const isActive = ch.id === active || anySelectedIn.has(ch.id);
    drawCurve(ctx, vp, ch, mode, tfs.get(ch.id), {
      color: ch.color,
      width: isActive ? 2 : 1.5,
      alpha: ch.locked ? 0.35 : isActive ? 1 : 0.6,
      dashOutside: true,
    });
  }

  // ---- hovered / selected segment highlight
  if (hover?.type === 'segment') {
    const ref = store.keyRef(hover.keyId);
    if (ref && ref.index < ref.channel.keys.length - 1) {
      drawSegmentHighlight(
        ctx,
        vp,
        ref.channel,
        ref.key,
        ref.channel.keys[ref.index + 1],
        mode,
        tfs.get(ref.channel.id) || channelTransform(ref.channel, mode, normalize),
        'rgba(255,255,255,0.35)',
        5,
      );
    }
  }
  if (store.selectedSegment) {
    const ref = store.keyRef(store.selectedSegment.keyId);
    if (ref && ref.channel.visible && ref.index < ref.channel.keys.length - 1) {
      drawSegmentHighlight(
        ctx,
        vp,
        ref.channel,
        ref.key,
        ref.channel.keys[ref.index + 1],
        mode,
        tfs.get(ref.channel.id),
        theme.accentSoft,
        9,
      );
      drawSegmentHighlight(
        ctx,
        vp,
        ref.channel,
        ref.key,
        ref.channel.keys[ref.index + 1],
        mode,
        tfs.get(ref.channel.id),
        theme.selection,
        2.5,
      );
    }
  }

  // ---- transform box
  if (s.transformBox) {
    const b = s.transformBox;
    ctx.save();
    ctx.strokeStyle = theme.boxStroke;
    ctx.setLineDash([5, 4]);
    ctx.lineWidth = 1;
    ctx.strokeRect(Math.round(b.x0) + 0.5, Math.round(b.y0) + 0.5, Math.round(b.x1 - b.x0), Math.round(b.y1 - b.y0));
    ctx.setLineDash([]);
    ctx.fillStyle = theme.bg;
    const mid = (a, c) => (a + c) / 2;
    const hs = BOX_HANDLE;
    const handles = [
      [b.x0, mid(b.y0, b.y1)],
      [b.x1, mid(b.y0, b.y1)],
    ];
    if (mode !== 'speed') handles.push([mid(b.x0, b.x1), b.y0], [mid(b.x0, b.x1), b.y1]);
    for (const [hx, hy] of handles) {
      ctx.fillStyle = theme.bg;
      ctx.fillRect(hx - hs / 2, hy - hs / 2, hs, hs);
      ctx.strokeStyle = theme.boxStroke;
      ctx.strokeRect(hx - hs / 2 + 0.5, hy - hs / 2 + 0.5, hs - 1, hs - 1);
    }
    ctx.restore();
  }

  // ---- handles (selected keys, hovered key, or all)
  const showFor = (ch, k) => {
    if (showHandles === 'none') return false;
    if (showHandles === 'all') return true;
    if (store.selectedKeys.has(k.id)) return true;
    if (hover && hover.keyId === k.id && (hover.type === 'key' || hover.type === 'handle')) return true;
    if (store.selectedSegment && store.selectedSegment.channelId === ch.id) {
      const ref = store.keyRef(store.selectedSegment.keyId);
      if (ref && (ref.key.id === k.id || ref.channel.keys[ref.index + 1]?.id === k.id)) return true;
    }
    return false;
  };
  for (const ch of channels) {
    if (ch.locked) continue;
    const tf = tfs.get(ch.id);
    for (let i = 0; i < ch.keys.length; i++) {
      const k = ch.keys[i];
      if (!showFor(ch, k)) continue;
      const pos = handleScreenPositions(vp, ch, i, mode, tf);
      const kx = vp.xOf(k.t);
      const ky = vp.yOf(keyPlotValue(ch, i, mode, tf));
      const kyIn = mode === 'speed' ? vp.yOf(keySpeeds(ch, i).in) : ky;
      for (const side of ['in', 'out']) {
        const p = pos[side];
        if (!p) continue;
        const isHot = hover?.type === 'handle' && hover.keyId === k.id && hover.side === side;
        const fromY = side === 'in' ? kyIn : ky;
        ctx.strokeStyle = theme.handleLine;
        ctx.lineWidth = 1;
        ctx.setLineDash(k.tangents === TANGENTS.AUTO ? [2, 3] : []);
        ctx.beginPath();
        ctx.moveTo(kx, fromY);
        ctx.lineTo(p.x, p.y);
        ctx.stroke();
        ctx.setLineDash([]);
        ctx.beginPath();
        ctx.arc(p.x, p.y, isHot ? HANDLE_RADIUS + 1.5 : HANDLE_RADIUS, 0, Math.PI * 2);
        ctx.fillStyle = k.tangents === TANGENTS.BROKEN ? theme.plotBg : theme.handle;
        ctx.fill();
        ctx.strokeStyle = isHot ? theme.accent : theme.handle;
        ctx.lineWidth = isHot ? 2 : 1.25;
        ctx.stroke();
      }
    }
  }

  // ---- keys
  for (const ch of channels) {
    const tf = tfs.get(ch.id);
    for (let i = 0; i < ch.keys.length; i++) {
      const k = ch.keys[i];
      const x = vp.xOf(k.t);
      if (x < -12 || x > W + 12) continue;
      const y = vp.yOf(keyPlotValue(ch, i, mode, tf));
      if (y < plotTop - 12 || y > H + 12) continue;
      const selected = store.selectedKeys.has(k.id);
      const hot = hover?.type === 'key' && hover.keyId === k.id;
      const outside = k.t < doc.range.start - 1e-9 || k.t > doc.range.end + 1e-9;
      // Speed mode: show incoming speed as a second dot when it differs.
      if (mode === 'speed') {
        const sp = keySpeeds(ch, i);
        const yIn = vp.yOf(sp.in);
        if (Math.abs(yIn - y) > 1.5) {
          ctx.strokeStyle = ch.color;
          ctx.globalAlpha = 0.6;
          ctx.setLineDash([2, 2]);
          ctx.beginPath();
          ctx.moveTo(x, yIn);
          ctx.lineTo(x, y);
          ctx.stroke();
          ctx.setLineDash([]);
          ctx.globalAlpha = 1;
          ctx.beginPath();
          ctx.arc(x, yIn, 3, 0, Math.PI * 2);
          ctx.fillStyle = selected ? theme.selection : ch.color;
          ctx.fill();
        }
      }
      let r = KEY_RADIUS + (hot ? 1.5 : 0);
      if (pulse && pulse.keyId === k.id) r += pulse.amount * 4;
      if (selected) {
        ctx.beginPath();
        ctx.arc(x, y, r + 4, 0, Math.PI * 2);
        ctx.fillStyle = theme.accentSoft;
        ctx.fill();
      }
      drawKeyShape(ctx, x, y, r, k);
      ctx.fillStyle = selected ? theme.selection : outside ? theme.plotBg : ch.color;
      ctx.fill();
      ctx.lineWidth = selected ? 2 : 1.5;
      ctx.strokeStyle = selected ? ch.color : outside ? ch.color : theme.bg;
      ctx.setLineDash(outside && !selected ? [2, 2] : []);
      ctx.stroke();
      ctx.setLineDash([]);
      if (ch.locked) {
        ctx.fillStyle = theme.textDim;
        ctx.fillRect(x - 1.5, y - 1.5, 3, 3);
      }
    }
  }

  // ---- blocked move indicator
  if (blockedAt != null) {
    const x = Math.round(vp.xOf(blockedAt)) + 0.5;
    ctx.save();
    ctx.strokeStyle = theme.danger;
    ctx.setLineDash([3, 3]);
    ctx.beginPath();
    ctx.moveTo(x, plotTop);
    ctx.lineTo(x, H);
    ctx.stroke();
    ctx.restore();
  }

  // ---- snap line
  if (snapLine != null) {
    const x = Math.round(vp.xOf(snapLine)) + 0.5;
    ctx.save();
    ctx.strokeStyle = theme.accent;
    ctx.globalAlpha = 0.8;
    ctx.setLineDash([2, 3]);
    ctx.beginPath();
    ctx.moveTo(x, plotTop);
    ctx.lineTo(x, H);
    ctx.stroke();
    ctx.restore();
  }

  // ---- playhead
  if (playhead != null) {
    const x = Math.round(vp.xOf(playhead)) + 0.5;
    if (x >= -1 && x <= W + 1) {
      ctx.save();
      ctx.strokeStyle = theme.playhead;
      ctx.lineWidth = 1.25;
      ctx.beginPath();
      ctx.moveTo(x, 0);
      ctx.lineTo(x, H);
      ctx.stroke();
      // value dots + readouts
      let labelY = plotTop + 14;
      for (const ch of channels) {
        const tf = tfs.get(ch.id);
        const pv = plotValueAt(ch, playhead, mode, tf);
        const y = vp.yOf(pv);
        if (y < plotTop || y > H) continue;
        ctx.beginPath();
        ctx.arc(x, y, 3.5, 0, Math.PI * 2);
        ctx.fillStyle = ch.color;
        ctx.fill();
        ctx.strokeStyle = theme.bg;
        ctx.lineWidth = 1;
        ctx.stroke();
        if (s.showReadouts !== false) {
          const real = mode === 'speed' ? pv : evalKeys(ch.keys, playhead, ch.extrapolation);
          const txt = `${formatValue(real, mode === 'speed' ? 1 : vStep.decimals + 1)}${mode === 'speed' ? '/s' : ch.unit ? ` ${ch.unit}` : ''}`;
          const ly = Math.max(labelY, Math.min(H - 10, y));
          label(ctx, txt, x + 10, ly, theme, { color: ch.color });
          labelY = ly + 18;
        }
      }
      ctx.restore();
    }
  }

  // ---- box selection
  if (boxSelect) {
    ctx.save();
    ctx.fillStyle = theme.boxFill;
    ctx.strokeStyle = theme.boxStroke;
    ctx.lineWidth = 1;
    const x = Math.min(boxSelect.x0, boxSelect.x1);
    const y = Math.min(boxSelect.y0, boxSelect.y1);
    const w = Math.abs(boxSelect.x1 - boxSelect.x0);
    const h = Math.abs(boxSelect.y1 - boxSelect.y0);
    ctx.fillRect(x, y, w, h);
    ctx.strokeRect(Math.round(x) + 0.5, Math.round(y) + 0.5, Math.round(w), Math.round(h));
    ctx.restore();
  }

  // ---- value labels (drawn last so they sit above curves)
  for (const { v, y } of valueLabels) {
    if (y < plotTop + 8 || y > H - 8) continue;
    const text = normalize && mode === 'value' ? `${Math.round(v * 100)}%` : formatValue(v, vStep.decimals);
    label(ctx, text, 6, y, theme, { color: theme.textDim, bg: true });
  }

  // ---- ruler
  ctx.fillStyle = theme.rulerBg;
  ctx.fillRect(0, 0, W, plotTop);
  ctx.strokeStyle = theme.gridMajor;
  ctx.beginPath();
  ctx.moveTo(0, plotTop - 0.5);
  ctx.lineTo(W, plotTop - 0.5);
  ctx.stroke();
  ctx.font = theme.mono;
  ctx.textBaseline = 'middle';
  ctx.textAlign = 'left';
  let lastLabelRight = -Infinity;
  for (const tick of timeTicks) {
    ctx.strokeStyle = tick.isMajor ? theme.rulerText : theme.gridMajor;
    ctx.beginPath();
    ctx.moveTo(tick.x, plotTop - (tick.isMajor ? 9 : 5));
    ctx.lineTo(tick.x, plotTop - 1);
    ctx.stroke();
    if (tick.isMajor) {
      const txt = formatTime(tick.t, fps, timeFormat);
      const w = ctx.measureText(txt).width;
      if (tick.x + 3 > lastLabelRight + 6) {
        ctx.fillStyle = theme.rulerText;
        ctx.fillText(txt, tick.x + 3, plotTop / 2 - 2);
        lastLabelRight = tick.x + 3 + w;
      }
    }
  }
  // range markers in ruler
  ctx.fillStyle = theme.range;
  if (xs >= 0 && xs <= W) ctx.fillRect(xs - 1, 0, 2, plotTop);
  if (xe >= 0 && xe <= W) ctx.fillRect(xe - 1, 0, 2, plotTop);
  // playhead head
  if (playhead != null) {
    const x = vp.xOf(playhead);
    if (x >= -8 && x <= W + 8) {
      ctx.fillStyle = theme.playhead;
      ctx.beginPath();
      ctx.moveTo(x - 6, 0);
      ctx.lineTo(x + 6, 0);
      ctx.lineTo(x + 6, plotTop - 8);
      ctx.lineTo(x, plotTop - 1);
      ctx.lineTo(x - 6, plotTop - 8);
      ctx.closePath();
      ctx.fill();
      const txt = formatTime(playhead, fps, timeFormat);
      ctx.font = theme.mono;
      const w = ctx.measureText(txt).width;
      const lx = x + 10 + w > W ? x - 10 - w : x + 10;
      ctx.fillStyle = theme.playhead;
      roundRect(ctx, lx - 4, 3, w + 8, plotTop - 6, 3);
      ctx.fill();
      ctx.fillStyle = '#fff';
      ctx.fillText(txt, lx, plotTop / 2);
    }
  }

  // ---- hover tooltip
  if (hover && hover.tooltip && !drag) {
    const { x, y, lines } = hover.tooltip;
    ctx.font = theme.mono;
    let w = 0;
    for (const l of lines) w = Math.max(w, ctx.measureText(l.text).width);
    const h = lines.length * 16 + 8;
    let tx = x + 14;
    let ty = y - h - 8;
    if (tx + w + 12 > W) tx = x - w - 22;
    if (ty < plotTop) ty = y + 14;
    ctx.fillStyle = theme.tooltipBg;
    roundRect(ctx, tx, ty, w + 12, h, 4);
    ctx.fill();
    ctx.strokeStyle = theme.gridMajor;
    ctx.stroke();
    lines.forEach((l, i) => {
      ctx.fillStyle = l.color || theme.text;
      ctx.fillText(l.text, tx + 6, ty + 12 + i * 16);
    });
  }
}

/**
 * Draw the normalized easing inset for one segment.
 * @param {CanvasRenderingContext2D} ctx
 */
export function renderEasingInset(ctx, { width, height, dpr, theme, easing, color, hot, hidden }) {
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, width, height);
  if (hidden) return;
  const pad = 10;
  const w = width - pad * 2;
  const h = height - pad * 2;
  const X = (u) => pad + u * w;
  const Y = (p) => pad + h - p * h;
  ctx.strokeStyle = theme.gridMinor;
  ctx.lineWidth = 1;
  for (let i = 0; i <= 4; i++) {
    const u = i / 4;
    ctx.beginPath();
    ctx.moveTo(X(u), pad);
    ctx.lineTo(X(u), pad + h);
    ctx.stroke();
    ctx.beginPath();
    ctx.moveTo(pad, Y(u));
    ctx.lineTo(pad + w, Y(u));
    ctx.stroke();
  }
  ctx.strokeStyle = theme.gridMajor;
  ctx.strokeRect(pad + 0.5, pad + 0.5, w - 1, h - 1);
  if (!easing) {
    ctx.fillStyle = theme.textDim;
    ctx.font = theme.font;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.fillText('—', width / 2, height / 2);
    return;
  }
  const [x1, y1, x2, y2] = easing.cp;
  // reference diagonal
  ctx.setLineDash([3, 3]);
  ctx.strokeStyle = theme.gridZero;
  ctx.beginPath();
  ctx.moveTo(X(0), Y(0));
  ctx.lineTo(X(1), Y(1));
  ctx.stroke();
  ctx.setLineDash([]);
  if (easing.interp === INTERP.HOLD) {
    ctx.strokeStyle = color;
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.moveTo(X(0), Y(0));
    ctx.lineTo(X(1), Y(0));
    ctx.lineTo(X(1), Y(1));
    ctx.stroke();
    return;
  }
  if (easing.interp === INTERP.LINEAR) {
    ctx.strokeStyle = color;
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.moveTo(X(0), Y(0));
    ctx.lineTo(X(1), Y(1));
    ctx.stroke();
    return;
  }
  // handle lines
  ctx.strokeStyle = theme.handleLine;
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(X(0), Y(0));
  ctx.lineTo(X(x1), Y(y1));
  ctx.moveTo(X(1), Y(1));
  ctx.lineTo(X(x2), Y(y2));
  ctx.stroke();
  // curve
  ctx.strokeStyle = color;
  ctx.lineWidth = 2;
  ctx.beginPath();
  ctx.moveTo(X(0), Y(0));
  ctx.bezierCurveTo(X(x1), Y(y1), X(x2), Y(y2), X(1), Y(1));
  ctx.stroke();
  for (const [i, [hx, hy]] of [
    [0, [x1, y1]],
    [1, [x2, y2]],
  ]) {
    ctx.beginPath();
    ctx.arc(X(hx), Y(hy), hot === i ? 5.5 : 4, 0, Math.PI * 2);
    ctx.fillStyle = theme.handle;
    ctx.fill();
    ctx.strokeStyle = hot === i ? theme.accent : theme.handle;
    ctx.stroke();
  }
  // end points
  for (const [px, py] of [
    [0, 0],
    [1, 1],
  ]) {
    ctx.beginPath();
    ctx.arc(X(px), Y(py), 3, 0, Math.PI * 2);
    ctx.fillStyle = color;
    ctx.fill();
  }
}

/**
 * Draw the range navigator strip (whole animation with the visible window).
 */
export function renderNavigator(ctx, { width, height, dpr, theme, store, vp, mode, normalize, extent }) {
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, width, height);
  ctx.fillStyle = theme.rulerBg;
  ctx.fillRect(0, 0, width, height);
  const { tMin, tMax } = extent;
  const span = Math.max(1e-6, tMax - tMin);
  const X = (t) => ((t - tMin) / span) * width;
  const doc = store.doc;
  // range
  ctx.fillStyle = 'rgba(242,193,78,0.10)';
  ctx.fillRect(X(doc.range.start), 0, Math.max(1, X(doc.range.end) - X(doc.range.start)), height);
  // curves (coarse)
  const channels = doc.channels.filter((c) => c.visible && c.keys.length);
  let gMin = Infinity;
  let gMax = -Infinity;
  const tfs = new Map();
  for (const ch of channels) {
    const tf = channelTransform(ch, mode, normalize);
    tfs.set(ch.id, tf);
    const b = curveBounds(ch.keys, 6);
    if (!b) continue;
    const lo = mode === 'speed' ? -1 : (b.vMin - tf.offset) * tf.scale;
    const hi = mode === 'speed' ? 1 : (b.vMax - tf.offset) * tf.scale;
    gMin = Math.min(gMin, lo);
    gMax = Math.max(gMax, hi);
  }
  if (!Number.isFinite(gMin)) {
    gMin = -1;
    gMax = 1;
  }
  if (gMax - gMin < 1e-9) {
    gMin -= 1;
    gMax += 1;
  }
  const Y = (v) => 3 + (1 - (v - gMin) / (gMax - gMin)) * (height - 6);
  for (const ch of channels) {
    ctx.strokeStyle = ch.color;
    ctx.globalAlpha = 0.8;
    ctx.lineWidth = 1;
    ctx.beginPath();
    const tf = tfs.get(ch.id);
    for (let x = 0; x <= width; x += 2) {
      const t = tMin + (x / width) * span;
      const v =
        mode === 'speed' ? Math.max(-1, Math.min(1, plotValueAt(ch, t, mode, tf) / (Math.abs(gMax) || 1))) : plotValueAt(ch, t, mode, tf);
      const y = Y(v);
      if (x === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    }
    ctx.stroke();
    ctx.globalAlpha = 1;
  }
  // visible window
  const wx0 = X(vp.t0);
  const wx1 = X(vp.t1);
  ctx.fillStyle = 'rgba(0,0,0,0.35)';
  ctx.fillRect(0, 0, Math.max(0, wx0), height);
  ctx.fillRect(Math.min(width, wx1), 0, width - Math.min(width, wx1), height);
  ctx.strokeStyle = theme.accent;
  ctx.lineWidth = 1;
  ctx.strokeRect(Math.round(wx0) + 0.5, 0.5, Math.max(4, Math.round(wx1 - wx0)) - 1, height - 1);
}

export { segmentControlPoints };
