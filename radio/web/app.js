import { QUALITIES, SOURCE_RATE, PEAK_BLOCK, capacity, decode, peaks, analyse, gainFor, shape, envelope, render, silenceBounds, renderedLength } from './dsp.js';
import { FORMAT_PCM10, addSample, sampleImage } from './uf2.js';
import { Player } from './player.js';
import { Knob } from './knob.js';
import * as flash from './flash.js';
import { load, save } from './store.js';

const $ = selector => document.querySelector(selector);
const $$ = selector => [...document.querySelectorAll(selector)];
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

const MIN_SELECTION = Math.round(0.02 * SOURCE_RATE);
const MIN_VIEW = 400;
const MAX_FILE = 400 * 1024 * 1024;
const LED = { forward: '#00ff50', reverse: '#ff6400', flash: '#ffffff', none: '#ff2a2a' };
const COLOURS = {
  melon: '#cec74c',
  flesh: '#e4566e',
  dim: '#4b4840',
  text: '#f5f3ea',
  faint: '#7d786c',
  line: '#2b2925',
};

const state = {
  source: null,
  sel: { a: 0, b: 0 },
  view: { start: 0, end: 1 },
  stats: { mean: 0, peak: 1 },
  settings: { normalize: true, gain: 0, fadeIn: 5, fadeOut: 20 },
  auto: true,
  qualityId: 'best',
  knobs: { speed: 0.5, direction: 0, position: 0 },
  loop: false,
  rendered: null,
  base: null,
  version: null,
};

// ── Stars ────────────────────────────────────────────────────────────────────

function random(seed) {
  return () => {
    seed |= 0;
    seed = seed + 0x6d2b79f5 | 0;
    let t = Math.imul(seed ^ seed >>> 15, 1 | seed);
    t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t;
    return ((t ^ t >>> 14) >>> 0) / 4294967296;
  };
}

function sparkle(ctx, x, y, r) {
  ctx.beginPath();
  for (let i = 0; i < 8; i++) {
    const a = i * Math.PI / 4;
    const d = i % 2 ? r * 0.32 : r;
    ctx.lineTo(x + d * Math.sin(a), y - d * Math.cos(a));
  }
  ctx.closePath();
  ctx.fill();
}

function drawStars() {
  const canvas = $('#stars');
  const dpr = devicePixelRatio || 1;
  const w = innerWidth, h = innerHeight;
  canvas.width = w * dpr;
  canvas.height = h * dpr;
  const ctx = canvas.getContext('2d');
  ctx.scale(dpr, dpr);
  const rand = random(77);
  for (let i = 0; i < w * h / 4200; i++) {
    ctx.globalAlpha = 0.15 + rand() * 0.45;
    ctx.fillStyle = rand() < 0.3 ? COLOURS.melon : '#fffdf0';
    ctx.beginPath();
    ctx.arc(rand() * w, rand() * h, 0.4 + rand() * 0.9, 0, Math.PI * 2);
    ctx.fill();
  }
  ctx.fillStyle = COLOURS.melon;
  for (let i = 0; i < w * h / 60000; i++) {
    ctx.globalAlpha = 0.25 + rand() * 0.35;
    sparkle(ctx, rand() * w, rand() * h, 3 + rand() * 4);
  }
}

function moduleStars() {
  const face = $('.module-face');
  const { width, height } = face.getBoundingClientRect();
  if (!width) return;
  const rand = random(12);
  let shapes = '';
  for (let i = 0; i < 18; i++) {
    const x = rand() * width, y = rand() * height, r = 2.5 + rand() * 3;
    const points = Array.from({ length: 8 }, (_, k) => {
      const a = k * Math.PI / 4, d = k % 2 ? r * 0.32 : r;
      return `${(x + d * Math.sin(a)).toFixed(1)},${(y - d * Math.cos(a)).toFixed(1)}`;
    }).join(' ');
    shapes += `<polygon points="${points}" fill="%23cec74c" opacity="${(0.55 + rand() * 0.4).toFixed(2)}"/>`;
  }
  face.style.setProperty('--module-stars', `url('data:image/svg+xml;utf8,<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${width.toFixed(0)} ${height.toFixed(0)}">${shapes}</svg>')`);
}

// ── Formatting ───────────────────────────────────────────────────────────────

function formatTime(seconds, digits = 3) {
  if (seconds < 60) return seconds.toFixed(digits);
  const m = Math.floor(seconds / 60);
  const s = seconds - m * 60;
  return `${m}:${s.toFixed(digits).padStart(digits ? digits + 3 : 2, '0')}`;
}

function parseTime(text) {
  const clean = text.trim().replace(',', '.').replace(/\s*s$/, '');
  const match = clean.match(/^(?:(\d+):)?(\d+(?:\.\d*)?|\.\d+)$/);
  if (!match) return null;
  return (match[1] ? Number(match[1]) * 60 : 0) + Number(match[2]);
}

function formatLength(seconds) {
  if (seconds >= 60) {
    const m = Math.floor(seconds / 60);
    const s = Math.round(seconds - m * 60);
    return s ? `${m} min ${s} s` : `${m} min`;
  }
  return `${seconds < 10 ? seconds.toFixed(1) : Math.round(seconds)} s`;
}

function formatBytes(bytes) {
  return bytes > 1e6 ? `${(bytes / 1e6).toFixed(1)} MB` : `${Math.round(bytes / 1e3)} KB`;
}

function fileSlug(name) {
  const slug = name.replace(/\.[^.]+$/, '').toLowerCase().normalize('NFKD').replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '').slice(0, 40);
  return slug ? `radio-${slug}.uf2` : 'radio.uf2';
}

// Fades use a squared scale so short fades are easy to set.
const fadeToMs = p => Math.round(3000 * (p / 1000) ** 2);
const msToFade = ms => Math.round(Math.sqrt(ms / 3000) * 1000);
const formatMs = ms => ms >= 1000 ? `${(ms / 1000).toFixed(2)} s` : `${ms} ms`;

// ── Selection helpers ────────────────────────────────────────────────────────

const selSeconds = () => (state.sel.b - state.sel.a) / SOURCE_RATE;

function quality() {
  return QUALITIES.find(q => q.id === state.qualityId);
}

function bestFit(seconds) {
  return QUALITIES.find(q => capacity(q) >= seconds) ?? QUALITIES[QUALITIES.length - 1];
}

function fits() {
  return selSeconds() <= capacity(quality()) + 1e-9;
}

function setSelection(a, b) {
  const n = state.source.samples.length;
  a = Math.round(Math.max(0, Math.min(a, n - MIN_SELECTION)));
  b = Math.round(Math.max(a + MIN_SELECTION, Math.min(b, n)));
  state.sel = { a, b };
  if (state.auto) state.qualityId = bestFit(selSeconds()).id;
}

function quickStats() {
  const { min, max } = state.source.peaks;
  const first = Math.floor(state.sel.a / PEAK_BLOCK);
  const last = Math.ceil(state.sel.b / PEAK_BLOCK);
  let peak = 0;
  for (let i = first; i < last; i++) peak = Math.max(peak, Math.abs(min[i] - state.stats.mean), Math.abs(max[i] - state.stats.mean));
  state.stats = { mean: state.stats.mean, peak };
}

function exactStats() {
  state.stats = analyse(state.source.samples, state.sel.a, state.sel.b);
}

// ── Waveform ─────────────────────────────────────────────────────────────────

const wave = { canvas: $('#wave'), layer: document.createElement('canvas'), dpr: 1, w: 0, h: 0 };
const overview = { canvas: $('#overview'), layer: document.createElement('canvas'), dpr: 1, w: 0, h: 0 };

function fitCanvas(target) {
  const dpr = devicePixelRatio || 1;
  const w = Math.round(target.canvas.clientWidth * dpr);
  const h = Math.round(target.canvas.clientHeight * dpr);
  if (w !== target.w || h !== target.h) {
    target.canvas.width = target.layer.width = w;
    target.canvas.height = target.layer.height = h;
    target.w = w;
    target.h = h;
  }
  target.dpr = dpr;
}

function columnRange(i0, i1) {
  const { samples, peaks: { min, max } } = state.source;
  let lo = Infinity, hi = -Infinity;
  if (i1 - i0 >= PEAK_BLOCK) {
    const b1 = Math.min(min.length, Math.ceil(i1 / PEAK_BLOCK));
    for (let b = Math.floor(i0 / PEAK_BLOCK); b < b1; b++) {
      if (min[b] < lo) lo = min[b];
      if (max[b] > hi) hi = max[b];
    }
  } else {
    const end = Math.min(samples.length, Math.ceil(i1));
    for (let i = Math.max(0, Math.floor(i0)); i < end; i++) {
      const s = samples[i];
      if (s < lo) lo = s;
      if (s > hi) hi = s;
    }
  }
  return lo === Infinity ? null : [lo, hi];
}

function fadeLengths() {
  const length = state.sel.b - state.sel.a;
  return [
    Math.min(length, Math.round(state.settings.fadeIn / 1000 * SOURCE_RATE)),
    Math.min(length, Math.round(state.settings.fadeOut / 1000 * SOURCE_RATE)),
  ];
}

function niceStep(secondsPerPixel, minPixels) {
  const steps = [0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1200];
  return steps.find(s => s / secondsPerPixel >= minPixels) ?? 1800;
}

function waveGeometry() {
  const { dpr, w, h } = wave;
  const top = 20 * dpr;
  const bottom = 22 * dpr;
  return { top, bottom, mid: top + (h - top - bottom) / 2, amp: (h - top - bottom) / 2 * 0.92, spp: (state.view.end - state.view.start) / w };
}

const toX = i => (i - state.view.start) / (state.view.end - state.view.start) * wave.w;
const toSample = x => state.view.start + x / wave.w * (state.view.end - state.view.start);

function drawWaveLayer() {
  fitCanvas(wave);
  const ctx = wave.layer.getContext('2d');
  const { dpr, w, h } = wave;
  const { top, bottom, mid, amp, spp } = waveGeometry();
  const { a, b } = state.sel;
  const { mean, peak } = state.stats;
  const gain = gainFor(state.settings, peak);
  const [fadeIn, fadeOut] = fadeLengths();
  const length = b - a;

  ctx.fillStyle = '#000';
  ctx.fillRect(0, 0, w, h);

  const xa = toX(a), xb = toX(b);
  ctx.fillStyle = 'rgba(206, 199, 76, 0.07)';
  ctx.fillRect(xa, 0, xb - xa, h);

  ctx.fillStyle = COLOURS.line;
  ctx.fillRect(0, Math.round(mid), w, Math.max(1, dpr));

  const shaped = (s, i) => shape((s - mean) * gain) * envelope(i - a, length, fadeIn, fadeOut);

  if (spp >= 1.5) {
    const paths = { dim: new Path2D(), melon: new Path2D(), flesh: new Path2D() };
    for (let x = 0; x < w; x++) {
      const i0 = state.view.start + x * spp;
      const range = columnRange(i0, i0 + spp);
      if (!range) continue;
      let [lo, hi] = range;
      const centre = i0 + spp / 2;
      let kind = 'dim';
      if (centre >= a && centre < b) {
        const raw = Math.max(Math.abs(lo - mean), Math.abs(hi - mean)) * gain;
        lo = shaped(lo, centre);
        hi = shaped(hi, centre);
        kind = raw > 0.9 ? 'flesh' : 'melon';
      }
      const y0 = mid - hi * amp;
      const y1 = mid - lo * amp;
      paths[kind].rect(x, y0, 1, Math.max(dpr, y1 - y0));
    }
    for (const [kind, path] of Object.entries(paths)) {
      ctx.fillStyle = COLOURS[kind];
      ctx.fill(path);
    }
  } else {
    const { samples } = state.source;
    const first = Math.max(0, Math.floor(state.view.start) - 1);
    const last = Math.min(samples.length - 1, Math.ceil(state.view.end) + 1);
    for (const inside of [false, true]) {
      ctx.beginPath();
      for (let i = first; i <= last; i++) {
        const within = i >= a && i < b;
        const s = within ? shaped(samples[i], i) : samples[i];
        const x = toX(i + 0.5), y = mid - s * amp;
        if (within === inside) ctx.lineTo(x, y);
        else ctx.moveTo(x, y);
      }
      ctx.strokeStyle = inside ? COLOURS.melon : COLOURS.dim;
      ctx.lineWidth = 1.5 * dpr;
      ctx.stroke();
      if (spp < 0.12) {
        ctx.fillStyle = ctx.strokeStyle;
        for (let i = first; i <= last; i++) {
          if ((i >= a && i < b) !== inside) continue;
          const s = inside ? shaped(samples[i], i) : samples[i];
          ctx.fillRect(toX(i + 0.5) - 1.5 * dpr, mid - s * amp - 1.5 * dpr, 3 * dpr, 3 * dpr);
        }
      }
    }
  }

  // Fade curves
  ctx.strokeStyle = 'rgba(245, 243, 234, 0.55)';
  ctx.lineWidth = 1.2 * dpr;
  for (const [from, n, rising] of [[a, fadeIn, true], [b - fadeOut, fadeOut, false]]) {
    if (n < 2 || toX(from + n) < 0 || toX(from) > w) continue;
    ctx.beginPath();
    const steps = 48;
    for (let k = 0; k <= steps; k++) {
      const t = k / steps;
      const g = 0.5 - 0.5 * Math.cos(Math.PI * (rising ? t : 1 - t));
      ctx.lineTo(toX(from + t * n), mid - g * amp);
    }
    ctx.stroke();
  }

  // Time ruler
  ctx.fillStyle = 'rgba(0, 0, 0, 0.85)';
  ctx.fillRect(0, 0, w, top);
  const secondsPerPixel = spp / SOURCE_RATE;
  const step = niceStep(secondsPerPixel, 90 * dpr);
  const digits = step < 0.01 ? 3 : step < 0.1 ? 2 : step < 1 ? 1 : 0;
  ctx.font = `${11 * dpr}px "Nunito Sans", system-ui, sans-serif`;
  ctx.textBaseline = 'middle';
  ctx.fillStyle = COLOURS.faint;
  for (let t = Math.ceil(state.view.start / SOURCE_RATE / step) * step; t * SOURCE_RATE <= state.view.end; t += step) {
    const x = Math.round(toX(t * SOURCE_RATE));
    ctx.fillRect(x, top - 5 * dpr, Math.max(1, dpr), 5 * dpr);
    ctx.fillText(formatTime(t, digits), x + 4 * dpr, top / 2);
  }

  // POSITION scale, the 101 start points of the knob
  ctx.fillStyle = 'rgba(0, 0, 0, 0.85)';
  ctx.fillRect(0, h - bottom, w, bottom);
  const span = xb - xa;
  ctx.textAlign = 'center';
  for (let p = 0; p <= 100; p++) {
    const major = p % 10 === 0;
    if (!major && span / 100 < 5 * dpr) continue;
    const x = Math.round(xa + span * p / 100);
    if (x < -20 || x > w + 20) continue;
    ctx.fillStyle = major ? COLOURS.melon : COLOURS.faint;
    ctx.fillRect(x, h - bottom, Math.max(1, dpr), (major ? 6 : 3) * dpr);
    if (major && (span / 10 > 26 * dpr || p === 0 || p === 100)) {
      ctx.fillText(String(p / 10), x, h - bottom / 2 + 3 * dpr);
    }
  }
  ctx.textAlign = 'start';
}

function drawWave() {
  if (!state.source) return;
  const ctx = wave.canvas.getContext('2d');
  const { dpr, w, h } = wave;
  const { top, bottom } = waveGeometry();
  ctx.drawImage(wave.layer, 0, 0);

  const xa = toX(state.sel.a), xb = toX(state.sel.b);

  // POSITION marker: where the next trigger starts
  const startX = xa + (xb - xa) * positionPercent() / 100;
  ctx.strokeStyle = COLOURS.flesh;
  ctx.lineWidth = 1.5 * dpr;
  ctx.setLineDash([4 * dpr, 4 * dpr]);
  ctx.beginPath();
  ctx.moveTo(Math.round(startX) + 0.5, top);
  ctx.lineTo(Math.round(startX) + 0.5, h - bottom);
  ctx.stroke();
  ctx.setLineDash([]);
  ctx.fillStyle = COLOURS.flesh;
  ctx.beginPath();
  ctx.moveTo(startX, h - bottom - 1 * dpr);
  ctx.lineTo(startX - 6 * dpr, h - bottom + 8 * dpr);
  ctx.lineTo(startX + 6 * dpr, h - bottom + 8 * dpr);
  ctx.fill();

  // Selection edges and grab tabs
  ctx.fillStyle = COLOURS.melon;
  for (const x of [xa, xb]) {
    if (x < -10 || x > w + 10) continue;
    ctx.fillRect(Math.round(x - dpr), 0, 2 * dpr, h - bottom);
    const tab = new Path2D();
    tab.roundRect(x - 6 * dpr, 2 * dpr, 12 * dpr, top - 4 * dpr, 3 * dpr);
    ctx.fill(tab);
    ctx.fillStyle = '#000';
    ctx.fillRect(Math.round(x - 2 * dpr), 6 * dpr, dpr, top - 12 * dpr);
    ctx.fillRect(Math.round(x + dpr), 6 * dpr, dpr, top - 12 * dpr);
    ctx.fillStyle = COLOURS.melon;
  }

  // Playhead
  const position = player.position();
  if (position !== null && state.rendered) {
    const i = state.sel.a + position / state.rendered.length * (state.sel.b - state.sel.a);
    const x = Math.round(toX(i));
    ctx.fillStyle = COLOURS.text;
    ctx.fillRect(x - dpr, top, 2 * dpr, h - top - bottom);
  }
}

function drawOverviewLayer() {
  fitCanvas(overview);
  const ctx = overview.layer.getContext('2d');
  const { w, h } = overview;
  const n = state.source.samples.length;
  const spp = n / w;
  ctx.fillStyle = '#000';
  ctx.fillRect(0, 0, w, h);
  const xa = state.sel.a / spp, xb = state.sel.b / spp;
  ctx.fillStyle = 'rgba(206, 199, 76, 0.14)';
  ctx.fillRect(xa, 0, Math.max(1, xb - xa), h);
  const paths = { dim: new Path2D(), melon: new Path2D() };
  const mid = h / 2, amp = h / 2 * 0.86;
  for (let x = 0; x < w; x++) {
    const range = columnRange(x * spp, (x + 1) * spp);
    if (!range) continue;
    const centre = (x + 0.5) * spp;
    const kind = centre >= state.sel.a && centre < state.sel.b ? 'melon' : 'dim';
    paths[kind].rect(x, mid - range[1] * amp, 1, Math.max(1, (range[1] - range[0]) * amp));
  }
  for (const [kind, path] of Object.entries(paths)) {
    ctx.fillStyle = COLOURS[kind];
    ctx.fill(path);
  }
}

function drawOverview() {
  if (!state.source) return;
  const ctx = overview.canvas.getContext('2d');
  const { w, h, dpr } = overview;
  ctx.drawImage(overview.layer, 0, 0);
  const n = state.source.samples.length;
  const x0 = state.view.start / n * w;
  const x1 = state.view.end / n * w;
  if (x1 - x0 < w - 1) {
    ctx.fillStyle = 'rgba(245, 243, 234, 0.08)';
    ctx.fillRect(x0, 0, x1 - x0, h);
    ctx.strokeStyle = COLOURS.text;
    ctx.lineWidth = 1.5 * dpr;
    ctx.strokeRect(x0 + dpr / 2, dpr / 2, Math.max(2, x1 - x0 - dpr), h - dpr);
  }
}

let frame = 0;
function redraw({ layer = true, overviewLayer = false } = {}) {
  if (!state.source) return;
  if (layer) drawWaveLayer();
  if (overviewLayer) drawOverviewLayer();
  cancelAnimationFrame(frame);
  frame = requestAnimationFrame(() => {
    drawWave();
    drawOverview();
  });
}

function setView(start, end) {
  const n = state.source.samples.length;
  let width = Math.min(n, Math.max(MIN_VIEW, end - start));
  start = Math.max(0, Math.min(start, n - width));
  state.view = { start, end: start + width };
}

function zoom(factor, centre) {
  const { start, end } = state.view;
  centre ??= (start + end) / 2;
  const width = Math.min(state.source.samples.length, Math.max(MIN_VIEW, (end - start) * factor));
  const ratio = (centre - start) / (end - start);
  setView(centre - ratio * width, centre - ratio * width + width);
  redraw();
}

function fitSelection() {
  const pad = (state.sel.b - state.sel.a) * 0.04;
  setView(state.sel.a - pad, state.sel.b + pad);
  redraw();
}

function showAll() {
  setView(0, state.source.samples.length);
  redraw();
}

// Pointer handling on the waveform
function waveInteractions() {
  const canvas = wave.canvas;
  let drag = null;

  const hit = event => {
    const rect = canvas.getBoundingClientRect();
    const x = (event.clientX - rect.left) * wave.dpr;
    const xa = toX(state.sel.a), xb = toX(state.sel.b);
    const grab = 9 * wave.dpr;
    const da = Math.abs(x - xa), db = Math.abs(x - xb);
    if (Math.min(da, db) <= grab) return { x, kind: da <= db ? 'a' : 'b' };
    if (x > xa && x < xb) return { x, kind: 'inside' };
    return { x, kind: 'outside' };
  };

  canvas.addEventListener('pointermove', event => {
    if (!state.source) return;
    if (!drag) {
      const { kind } = hit(event);
      canvas.style.cursor = kind === 'a' || kind === 'b' ? 'ew-resize' : kind === 'inside' ? 'pointer' : 'crosshair';
      return;
    }
    const x = (event.clientX - canvas.getBoundingClientRect().left) * wave.dpr;
    if (!drag.moved && Math.abs(x - drag.x) < 4 * wave.dpr) return;
    drag.moved = true;
    const at = toSample(x);
    if (drag.kind === 'a') setSelection(Math.min(at, state.sel.b - MIN_SELECTION), state.sel.b);
    else if (drag.kind === 'b') setSelection(state.sel.a, Math.max(at, state.sel.a + MIN_SELECTION));
    else if (drag.kind === 'inside') {
      const length = drag.sel.b - drag.sel.a;
      const n = state.source.samples.length;
      const a = Math.max(0, Math.min(n - length, drag.sel.a + at - drag.at));
      setSelection(a, a + length);
      canvas.style.cursor = 'grabbing';
    } else {
      setSelection(Math.min(drag.at, at), Math.max(drag.at, at));
    }
    quickStats();
    selectionChanged({ commit: false });
  });

  canvas.addEventListener('pointerdown', event => {
    if (!state.source || event.button !== 0) return;
    const { x, kind } = hit(event);
    canvas.setPointerCapture(event.pointerId);
    drag = { x, kind, at: toSample(x), sel: { ...state.sel }, moved: false };
  });

  const finish = event => {
    if (!drag) return;
    const done = drag;
    drag = null;
    canvas.style.cursor = '';
    if (done.moved) {
      selectionChanged({ commit: true });
    } else if (done.kind === 'inside' && event.type === 'pointerup') {
      const percent = Math.floor((done.at - state.sel.a) / (state.sel.b - state.sel.a) * 100);
      knobs.position.set(Math.min(100, Math.max(0, percent)) / 100 + 0.001);
      trigger();
    }
  };
  canvas.addEventListener('pointerup', finish);
  canvas.addEventListener('pointercancel', finish);

  canvas.addEventListener('wheel', event => {
    if (!state.source) return;
    const rect = canvas.getBoundingClientRect();
    if (event.ctrlKey || event.metaKey) {
      event.preventDefault();
      zoom(Math.exp(event.deltaY * 0.01), toSample((event.clientX - rect.left) * wave.dpr));
    } else if (Math.abs(event.deltaX) > Math.abs(event.deltaY)) {
      event.preventDefault();
      const spp = (state.view.end - state.view.start) / rect.width;
      setView(state.view.start + event.deltaX * spp, state.view.end + event.deltaX * spp);
      redraw();
    }
  }, { passive: false });

  const ov = overview.canvas;
  let pan = null;
  const sampleAt = event => {
    const rect = ov.getBoundingClientRect();
    return (event.clientX - rect.left) / rect.width * state.source.samples.length;
  };
  ov.addEventListener('pointerdown', event => {
    if (!state.source) return;
    ov.setPointerCapture(event.pointerId);
    const at = sampleAt(event);
    const width = state.view.end - state.view.start;
    if (at < state.view.start || at > state.view.end) setView(at - width / 2, at + width / 2);
    pan = { at, start: state.view.start };
    redraw();
  });
  ov.addEventListener('pointermove', event => {
    if (!pan) return;
    const width = state.view.end - state.view.start;
    const start = pan.start + sampleAt(event) - pan.at;
    setView(start, start + width);
    redraw();
  });
  const stopPan = () => { pan = null; };
  ov.addEventListener('pointerup', stopPan);
  ov.addEventListener('pointercancel', stopPan);
  ov.addEventListener('dblclick', showAll);
}

function toolbarInteractions() {
  const commit = (a, b) => {
    setSelection(a, b);
    exactStats();
    selectionChanged({ commit: true });
  };
  $('#trim').addEventListener('click', () => commit(...silenceBounds(state.source.samples, state.sel.a, state.sel.b)));
  $('#all').addEventListener('click', () => {
    commit(0, state.source.samples.length);
    showAll();
  });
  $('#zoom-in').addEventListener('click', () => zoom(0.5));
  $('#zoom-out').addEventListener('click', () => zoom(2));
  $('#zoom-fit').addEventListener('click', fitSelection);
  $('#zoom-all').addEventListener('click', showAll);
}

// ── Fields, sound and quality ────────────────────────────────────────────────

const fields = { start: $('#sel-start'), end: $('#sel-end'), length: $('#sel-length') };

function updateFields() {
  const { a, b } = state.sel;
  if (document.activeElement !== fields.start) fields.start.value = formatTime(a / SOURCE_RATE);
  if (document.activeElement !== fields.end) fields.end.value = formatTime(b / SOURCE_RATE);
  if (document.activeElement !== fields.length) fields.length.value = formatTime((b - a) / SOURCE_RATE);
}

function fieldInteractions() {
  for (const [key, input] of Object.entries(fields)) {
    const apply = () => {
      const seconds = parseTime(input.value);
      input.classList.toggle('invalid', seconds === null);
      if (seconds === null) return;
      const at = seconds * SOURCE_RATE;
      if (key === 'start') setSelection(at, Math.max(state.sel.b, at + MIN_SELECTION));
      if (key === 'end') setSelection(Math.min(state.sel.a, at - MIN_SELECTION), at);
      if (key === 'length') setSelection(state.sel.a, state.sel.a + at);
      exactStats();
      selectionChanged({ commit: true });
      input.value = formatTime((key === 'start' ? state.sel.a : key === 'end' ? state.sel.b : state.sel.b - state.sel.a) / SOURCE_RATE);
    };
    input.addEventListener('change', apply);
    input.addEventListener('keydown', event => {
      if (event.key === 'Enter') {
        apply();
        input.select();
      }
    });
    input.addEventListener('blur', () => {
      input.classList.remove('invalid');
      updateFields();
    });
  }
}

function paintRange(input) {
  const fill = (input.value - input.min) / (input.max - input.min) * 100;
  input.style.setProperty('--fill', `${fill}%`);
}

function updateSoundControls() {
  const { normalize, gain, fadeIn, fadeOut } = state.settings;
  $('#normalize').checked = normalize;
  $('#gain').value = gain;
  $('#gain-value').textContent = `${gain > 0 ? '+' : gain < 0 ? '−' : ''}${Math.abs(gain).toFixed(1)} dB`;
  $('#fade-in').value = msToFade(fadeIn);
  $('#fade-in-value').textContent = formatMs(fadeIn);
  $('#fade-out').value = msToFade(fadeOut);
  $('#fade-out-value').textContent = formatMs(fadeOut);
  for (const id of ['#gain', '#fade-in', '#fade-out']) paintRange($(id));
}

function soundInteractions() {
  const changed = () => {
    updateSoundControls();
    redraw();
    requestRender();
    saveSession();
  };
  $('#normalize').addEventListener('change', event => { state.settings.normalize = event.target.checked; changed(); });
  $('#gain').addEventListener('input', event => { state.settings.gain = Number(event.target.value); changed(); });
  $('#gain').addEventListener('dblclick', () => { state.settings.gain = 0; changed(); });
  $('#fade-in').addEventListener('input', event => { state.settings.fadeIn = fadeToMs(Number(event.target.value)); changed(); });
  $('#fade-out').addEventListener('input', event => { state.settings.fadeOut = fadeToMs(Number(event.target.value)); changed(); });
}

function buildQualities() {
  const group = $('#qualities');
  for (const q of QUALITIES) {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'quality';
    button.dataset.id = q.id;
    button.setAttribute('role', 'radio');
    button.title = q.hint;
    button.innerHTML = `<span class="q-name">${q.name}</span><span class="q-length">${formatLength(capacity(q))}</span><span class="q-spec">${q.spec}</span>`;
    button.addEventListener('click', () => {
      state.auto = false;
      state.qualityId = q.id;
      $('#auto').checked = false;
      qualityChanged();
    });
    group.append(button);
  }
  $('#auto').addEventListener('change', event => {
    state.auto = event.target.checked;
    if (state.auto) state.qualityId = bestFit(selSeconds()).id;
    qualityChanged();
  });
}

function updateQualities() {
  const seconds = selSeconds();
  for (const button of $$('.quality')) {
    const q = QUALITIES.find(item => item.id === button.dataset.id);
    button.setAttribute('aria-checked', String(q.id === state.qualityId));
    button.classList.toggle('too-short', capacity(q) < seconds);
  }
  const q = quality();
  const cap = capacity(q);
  const over = seconds > cap + 1e-9;
  $('#meter').classList.toggle('over', over);
  $('#meter-fill').style.width = `${Math.min(100, seconds / cap * 100)}%`;
  $('#meter-text').textContent = over
    ? `${(seconds - cap).toFixed(1)} s too long for ${q.name}. Shorten the selection${q === QUALITIES[QUALITIES.length - 1] ? '' : ' or pick a longer quality'}.`
    : `${seconds.toFixed(1)} s of ${cap.toFixed(1)} s · ${Math.round(seconds / cap * 100)} % of Melon's memory`;
  const step = seconds / 100;
  $('#position-step').textContent = `POSITION has 101 start points, ${step < 1 ? `${Math.round(step * 1000)} ms` : `${step.toFixed(2)} s`} apart.`;
}

function qualityChanged() {
  updateQualities();
  updateSend();
  requestRender();
  saveSession();
}

function selectionChanged({ commit }) {
  updateFields();
  updateQualities();
  redraw({ overviewLayer: true });
  if (commit) {
    exactStats();
    redraw({ overviewLayer: true });
    requestRender();
    saveSession();
  }
  updateSend();
}

// ── Rendering ────────────────────────────────────────────────────────────────

let worker = null;
try {
  worker = new Worker(new URL('render-worker.js', import.meta.url), { type: 'module' });
  worker.onerror = () => { worker = null; };
} catch {
  worker = null;
}

let renderTimer = 0;
let renderSeq = 0;
let latest = null;
const waiting = new Map();

function requestRender() {
  state.rendered = null;
  clearTimeout(renderTimer);
  renderTimer = setTimeout(startRender, 140);
}

function finishRender(result) {
  const resolve = waiting.get(result.id);
  waiting.delete(result.id);
  if (result.id === renderSeq) {
    state.rendered = result;
    player.setAudio(result.preview, result.rate);
    updateSend();
  }
  resolve?.(result);
}

function startRender() {
  clearTimeout(renderTimer);
  renderTimer = 0;
  const id = ++renderSeq;
  const samples = state.source.samples.slice(state.sel.a, state.sel.b);
  const job = { id, samples, settings: { ...state.settings }, quality: { ...quality() } };
  latest = new Promise(resolve => waiting.set(id, resolve));
  if (worker) {
    worker.postMessage(job, [samples.buffer]);
  } else {
    setTimeout(() => finishRender({ id, ...render(samples, 0, samples.length, job.settings, job.quality) }));
  }
  return latest;
}

worker?.addEventListener('message', ({ data }) => finishRender(data));

async function ensureRendered() {
  while (!state.rendered) {
    if (renderTimer || !latest) startRender();
    await latest;
  }
  return state.rendered;
}

// ── Module preview ───────────────────────────────────────────────────────────

const player = new Player(playerChanged);
const led = $('#led');
let flashUntil = 0;

const positionPercent = () => Math.floor(state.knobs.position * 100);
const reversed = () => state.knobs.direction >= 0.5;

const knobs = {
  speed: new Knob($('[data-knob="speed"]'), {
    label: 'SPEED', value: state.knobs.speed, defaultValue: 0.5, origin: 0.5,
    format: v => `${(0.5 + v).toFixed(2)}×`,
    onInput: v => { state.knobs.speed = v; player.setSpeed(0.5 + v); saveSession(); },
  }),
  direction: new Knob($('[data-knob="direction"]'), {
    label: 'DIRECTION', value: state.knobs.direction, defaultValue: 0,
    format: v => v >= 0.5 ? 'Reverse' : 'Forward',
    onInput: v => { state.knobs.direction = v; player.setReversed(v >= 0.5); updateLed(); saveSession(); },
  }),
  position: new Knob($('[data-knob="position"]'), {
    label: 'POSITION', value: state.knobs.position, defaultValue: 0,
    format: v => `${Math.floor(v * 100)} %`,
    onInput: v => { state.knobs.position = v; redraw({ layer: false }); saveSession(); },
  }),
};

function updateLed() {
  let colour = LED.none;
  if (state.source) colour = performance.now() < flashUntil ? LED.flash : reversed() ? LED.reverse : LED.forward;
  led.style.setProperty('--led', colour);
  led.classList.toggle('idle', !state.source);
  knobs.direction.setColour(state.source ? (reversed() ? LED.reverse : LED.forward) : 'var(--melon)');
}

async function trigger() {
  if (!state.source) return;
  player.ensureContext();
  const button = $('#trigger');
  button.classList.add('pressed');
  setTimeout(() => button.classList.remove('pressed'), 120);
  flashUntil = performance.now() + 80;
  updateLed();
  setTimeout(updateLed, 90);
  await ensureRendered();
  player.trigger(positionPercent());
}

let animating = false;
function animate() {
  if (!player.playing) {
    animating = false;
    $('#out').classList.remove('live');
    redraw({ layer: false });
    return;
  }
  animating = true;
  drawWave();
  const out = $('#out');
  out.classList.add('live');
  out.style.setProperty('--level', player.level().toFixed(2));
  requestAnimationFrame(animate);
}

function playerChanged() {
  const play = $('#play');
  play.querySelector('.icon').className = `icon ${player.playing ? 'stop' : 'play'}`;
  play.querySelector('.label').textContent = player.playing ? 'Stop' : 'Play';
  if (player.playing && !animating) animate();
}

function moduleInteractions() {
  $('#trigger').addEventListener('click', trigger);
  $('#in1').addEventListener('click', trigger);
  $('#in2').addEventListener('click', () => {
    state.loop = !state.loop;
    $('#in2').setAttribute('aria-pressed', String(state.loop));
    player.setLoop(state.loop);
    saveSession();
  });
  $('#play').addEventListener('click', () => {
    if (player.playing) player.stop();
    else trigger();
  });
  document.addEventListener('keydown', event => {
    if (!state.source || event.defaultPrevented) return;
    const target = event.target;
    const typing = target.closest('input, textarea, select, button, summary, a, dialog');
    if (event.code === 'Space' && !typing) {
      event.preventDefault();
      trigger();
    } else if (event.key === 'Escape' && !$('#flash').open) {
      player.stop();
    }
  });
}

// ── Loading ──────────────────────────────────────────────────────────────────

const status = $('#load-status');

function setStatus(text, error = false) {
  status.textContent = text;
  status.classList.toggle('error', error);
}

async function loadAudio(blob, name, { example = false, restore = null } = {}) {
  if (blob.size > MAX_FILE) {
    setStatus('This file is very large. Cut it down to the part you want first, then try again.', true);
    return;
  }
  setStatus(`Reading ${name}…`);
  player.stop();
  let decoded;
  try {
    decoded = await decode(await blob.arrayBuffer());
  } catch {
    setStatus(`${name} could not be read. Try a WAV, MP3, AIFF, FLAC, M4A or OGG file.`, true);
    return;
  }
  if (decoded.samples.length < MIN_SELECTION * 2) {
    setStatus(`${name} is too short.`, true);
    return;
  }

  setStatus('');
  const { samples, channels } = decoded;
  state.source = { samples, peaks: peaks(samples), name, channels };
  const seconds = samples.length / SOURCE_RATE;

  $('#file-name').textContent = example ? 'Example recording' : name.replace(/\.[^.]+$/, '');
  $('#file-meta').textContent = `${formatTime(seconds, 1)}${seconds < 60 ? ' s' : ''} · ${channels === 1 ? 'mono' : channels === 2 ? 'stereo, mixed to mono' : `${channels} channels, mixed to mono`}`;
  $('#start').hidden = true;
  $('#editor').hidden = false;
  moduleStars();

  const longest = capacity(QUALITIES[QUALITIES.length - 1]);
  if (restore) {
    state.settings = { ...state.settings, ...restore.settings };
    state.auto = restore.auto ?? true;
    state.qualityId = QUALITIES.some(q => q.id === restore.qualityId) ? restore.qualityId : 'best';
    state.loop = Boolean(restore.loop);
    Object.assign(state.knobs, restore.knobs);
    for (const [key, knob] of Object.entries(knobs)) knob.set(state.knobs[key], false);
    player.setSpeed(0.5 + state.knobs.speed);
    player.setReversed(reversed());
    player.setLoop(state.loop);
    $('#in2').setAttribute('aria-pressed', String(state.loop));
    setSelection(restore.start * SOURCE_RATE, restore.end * SOURCE_RATE);
  } else if (seconds <= longest) {
    setSelection(0, samples.length);
  } else {
    setSelection(0, Math.floor(capacity(QUALITIES[0]) * SOURCE_RATE));
  }
  $('#auto').checked = state.auto;
  exactStats();
  updateSoundControls();
  updateLed();
  if (state.sel.b - state.sel.a < samples.length) fitSelection();
  else setView(0, samples.length);
  selectionChanged({ commit: true });

  if (example) save('file', { example: true, name });
  else if (!restore) save('file', { blob, name });

  $('#editor').scrollIntoView({ behavior: matchMedia('(prefers-reduced-motion: reduce)').matches ? 'auto' : 'smooth', block: 'start' });
}

async function loadExample(restore = null) {
  setStatus('Loading the example…');
  try {
    const response = await fetch('example.wav');
    if (!response.ok) throw new Error();
    await loadAudio(await response.blob(), 'example.wav', { example: true, restore });
  } catch {
    setStatus('The example could not be loaded.', true);
  }
}

function pickFile() {
  $('#file').value = '';
  $('#file').click();
}

function loadingInteractions() {
  $('#browse').addEventListener('click', pickFile);
  $('#replace').addEventListener('click', pickFile);
  $('#example').addEventListener('click', () => loadExample());
  $('#file').addEventListener('change', event => {
    const file = event.target.files[0];
    if (file) loadAudio(file, file.name);
  });

  const overlay = $('#drop-overlay');
  let depth = 0;
  const hasFiles = event => [...(event.dataTransfer?.types ?? [])].includes('Files');
  addEventListener('dragenter', event => {
    if (!hasFiles(event)) return;
    event.preventDefault();
    depth++;
    overlay.hidden = false;
    $('#start').classList.add('over');
  });
  addEventListener('dragover', event => {
    if (hasFiles(event)) event.preventDefault();
  });
  addEventListener('dragleave', () => {
    depth = Math.max(0, depth - 1);
    if (!depth) {
      overlay.hidden = true;
      $('#start').classList.remove('over');
    }
  });
  addEventListener('drop', event => {
    if (!hasFiles(event)) return;
    event.preventDefault();
    depth = 0;
    overlay.hidden = true;
    $('#start').classList.remove('over');
    const file = [...event.dataTransfer.files].find(f => f.type.startsWith('audio/') || /\.(wav|aiff?|mp3|m4a|aac|flac|ogg|opus|webm|caf)$/i.test(f.name)) ?? event.dataTransfer.files[0];
    if (file) loadAudio(file, file.name);
  });
}

// ── Remembering the last session ─────────────────────────────────────────────

let saveTimer = 0;
function saveSession() {
  if (!state.source) return;
  clearTimeout(saveTimer);
  saveTimer = setTimeout(() => save('session', {
    start: state.sel.a / SOURCE_RATE,
    end: state.sel.b / SOURCE_RATE,
    settings: state.settings,
    auto: state.auto,
    qualityId: state.qualityId,
    knobs: state.knobs,
    loop: state.loop,
  }), 400);
}

async function offerResume() {
  const file = await load('file');
  if (!file) return;
  const button = $('#resume');
  button.querySelector('b').textContent = file.example ? 'the example' : file.name;
  button.hidden = false;
  button.addEventListener('click', async () => {
    const session = await load('session');
    if (file.example) loadExample(session);
    else loadAudio(file.blob, file.name, { restore: session });
  });
}

// ── Building and sending ─────────────────────────────────────────────────────

async function buildUf2() {
  const rendered = await ensureRendered();
  const image = sampleImage({ ...rendered, name: state.source.name });
  return addSample(state.base, image);
}

function estimatedSize() {
  const q = quality();
  const length = renderedLength(q, state.sel.b - state.sel.a);
  const bytes = 256 + (q.format === FORMAT_PCM10 ? Math.ceil(length * 10 / 8) : length);
  return state.base.length + Math.ceil(bytes / 256) * 512;
}

function updateSend() {
  const send = $('#send');
  const download = $('#download');
  const info = $('#send-info');
  if (!state.source) return;
  if (!state.base) {
    send.disabled = download.disabled = true;
    info.textContent = state.baseError ?? 'Loading the Radio firmware…';
    return;
  }
  const ok = fits();
  send.disabled = download.disabled = !ok;
  const version = state.version?.version ? ` ${state.version.version}` : '';
  info.textContent = ok
    ? `${fileSlug(state.source.name)} · about ${formatBytes(estimatedSize())} · Radio firmware${version} with your recording`
    : 'The selection is too long for the chosen quality.';
}

async function download() {
  const bytes = await buildUf2();
  const url = URL.createObjectURL(new Blob([bytes], { type: 'application/octet-stream' }));
  const link = document.createElement('a');
  link.href = url;
  link.download = fileSlug(state.source.name);
  document.body.append(link);
  link.click();
  link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 10000);
}

// The flash dialog walks through: power off, restart, choose drive, write.
const dialog = $('#flash');
const STEPS = ['power', 'restart', 'drive', 'write'];
let flashBytes = null;
let remembered = null;

function showStep(name) {
  for (const section of dialog.querySelectorAll('[data-step]')) section.hidden = section.dataset.step !== name;
  const index = name === 'done' ? STEPS.length : STEPS.indexOf(name);
  dialog.querySelectorAll('.flash-steps li').forEach((li, i) => li.classList.toggle('done', i <= index));
  dialog.querySelector('.flash-steps').hidden = name === 'error';
  const focus = dialog.querySelector(`[data-step="${name}"] .button.primary`);
  focus?.focus();
}

async function prepareDriveStep() {
  remembered = await flash.rememberedDrive();
  const known = remembered && !remembered.granted;
  $('#pick').textContent = known ? 'Allow access to RP2350' : 'Choose RP2350';
  $('#pick-other').hidden = !known;
  $('#drive-text').innerHTML = known
    ? 'Melon is ready. Let the site save to the <b>RP2350</b> drive again.'
    : 'A drive called <b>RP2350</b> has appeared. Choose it in the next window, then let the site save to it.';
  $('#drive-status').textContent = '';
  showStep('drive');
}

function fail(message) {
  $('#error-text').textContent = message;
  showStep('error');
}

async function write(dir) {
  showStep('write');
  const bar = $('#write-progress');
  const progress = bar.parentElement;
  progress.classList.remove('busy');
  bar.style.width = '0%';
  $('#write-status').textContent = '';
  try {
    const result = await flash.writeUf2(dir, fileSlug(state.source.name), flashBytes, p => {
      bar.style.width = `${Math.round(p * 100)}%`;
      if (p >= 1) {
        progress.classList.add('busy');
        $('#write-status').textContent = 'Melon is saving the recording…';
      }
    });
    progress.classList.remove('busy');
    $('#done-text').textContent = result.closed
      ? 'Melon restarted with your recording. Unplug the USB cable, switch your case back on and press the button.'
      : 'Melon restarted a moment before the browser finished, which is normal. If the LED lights up green, unplug the USB cable, switch your case back on and press the button. If it pulses red, try again.';
    showStep('done');
  } catch (error) {
    progress.classList.remove('busy');
    fail(`Writing to the drive failed: ${error.message || error.name}. Check that Melon is still connected and try again.`);
  }
}

function flashInteractions() {
  if (!flash.canSerial || !flash.canWrite) {
    $('#send').hidden = true;
    $('#download').classList.add('primary');
    $('#send-support').hidden = false;
  }

  $('#download').addEventListener('click', download);
  dialog.querySelector('[data-download]').addEventListener('click', () => { dialog.close(); download(); });

  $('#send').addEventListener('click', async () => {
    flashBytes = await buildUf2();
    $('#restart-status').textContent = '';
    showStep('power');
    dialog.showModal();
  });

  for (const button of dialog.querySelectorAll('[data-go]')) {
    button.addEventListener('click', () => {
      if (button.dataset.go === 'drive') prepareDriveStep();
      else showStep(button.dataset.go);
    });
  }
  dialog.querySelector('[data-close]').addEventListener('click', () => dialog.close());

  $('#restart').addEventListener('click', async () => {
    const statusLine = $('#restart-status');
    const button = $('#restart');
    statusLine.classList.remove('error');
    statusLine.textContent = 'Pick Melon in the window your browser shows.';
    try {
      await flash.restartIntoBootloader();
    } catch (error) {
      statusLine.classList.add('error');
      statusLine.textContent = error.name === 'NotFoundError' ? 'No device was chosen.' : `Melon could not be restarted: ${error.message}`;
      return;
    }
    button.disabled = true;
    statusLine.textContent = 'Melon is restarting…';
    const known = await flash.rememberedDrive();
    if (known?.granted) {
      statusLine.textContent = 'Waiting for the RP2350 drive…';
      if (await flash.waitForDrive(known.dir, 12000)) {
        button.disabled = false;
        write(known.dir);
        return;
      }
    } else {
      await sleep(2500);
    }
    button.disabled = false;
    prepareDriveStep();
  });

  $('#pick').addEventListener('click', async () => {
    const statusLine = $('#drive-status');
    statusLine.textContent = '';
    if (remembered && !remembered.granted) {
      try {
        if (await flash.allow(remembered.dir)) {
          if (await flash.waitForDrive(remembered.dir, 8000)) {
            write(remembered.dir);
            return;
          }
          statusLine.textContent = 'The RP2350 drive is not there yet. Wait a moment and try again, or choose it yourself.';
          return;
        }
      } catch {
        // Fall through to the picker.
      }
    }
    try {
      const picked = await flash.pickDrive();
      if (!picked.ok) {
        statusLine.textContent = picked.reason;
        return;
      }
      write(picked.dir);
    } catch (error) {
      if (error.name !== 'AbortError') statusLine.textContent = error.message;
    }
  });

  $('#pick-other').addEventListener('click', async () => {
    remembered = null;
    $('#pick').textContent = 'Choose RP2350';
    $('#pick').click();
  });
}

// ── Start ────────────────────────────────────────────────────────────────────

async function loadFirmware() {
  try {
    const [uf2, version] = await Promise.all([
      fetch('radio.uf2').then(r => { if (!r.ok) throw new Error(); return r.arrayBuffer(); }),
      fetch('version.json').then(r => r.ok ? r.json() : null).catch(() => null),
    ]);
    state.base = new Uint8Array(uf2);
    state.version = version;
    $('#firmware-version').textContent = version
      ? `Radio firmware ${version.version}, built from ${version.commit.slice(0, 7)}.`
      : 'Radio firmware, development build.';
  } catch {
    state.baseError = 'The Radio firmware could not be loaded. Reload the page to try again.';
  }
  updateSend();
}

function start() {
  drawStars();
  buildQualities();
  waveInteractions();
  toolbarInteractions();
  fieldInteractions();
  soundInteractions();
  moduleInteractions();
  loadingInteractions();
  flashInteractions();
  updateSoundControls();
  updateLed();
  loadFirmware();
  offerResume();

  let resizeTimer = 0;
  addEventListener('resize', () => {
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(() => {
      drawStars();
      moduleStars();
      redraw({ overviewLayer: true });
    }, 120);
  });
}

start();
