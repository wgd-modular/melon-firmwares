import { DATA_BYTES, FORMAT_PCM10, FORMAT_ULAW } from './uf2.js';

// The firmware's audio interrupt runs at 150 MHz / 4096.
export const ISR_RATE = 150e6 / 4096;
export const SOURCE_RATE = Math.round(ISR_RATE);

export const QUALITIES = [
  { id: 'best', name: 'Best', format: FORMAT_PCM10, rate: SOURCE_RATE, spec: '10-bit · 36.6 kHz', hint: 'Exactly what Melon can play' },
  { id: 'long', name: 'Long', format: FORMAT_ULAW, rate: SOURCE_RATE, spec: 'µ-law · 36.6 kHz', hint: 'A little grain on loud parts' },
  { id: 'longer', name: 'Longer', format: FORMAT_ULAW, rate: Math.round(ISR_RATE / 1.5), spec: 'µ-law · 24.4 kHz', hint: 'Fine for speech' },
  { id: 'longest', name: 'Longest', format: FORMAT_ULAW, rate: Math.round(ISR_RATE / 2), spec: 'µ-law · 18.3 kHz', hint: 'Duller top end' },
  { id: 'lofi', name: 'Lo-fi', format: FORMAT_ULAW, rate: Math.round(ISR_RATE / 3), spec: 'µ-law · 12.2 kHz', hint: 'AM radio' },
];

export function capacity(quality) {
  const samples = quality.format === FORMAT_PCM10 ? Math.floor(DATA_BYTES * 8 / 10) : DATA_BYTES;
  return samples / quality.rate;
}

export function renderedLength(quality, sourceSamples) {
  return Math.floor(sourceSamples * quality.rate / SOURCE_RATE);
}

export async function decode(arrayBuffer) {
  const context = new OfflineAudioContext(1, 1, SOURCE_RATE);
  const audio = await context.decodeAudioData(arrayBuffer);
  const mono = new Float32Array(audio.length);
  for (let c = 0; c < audio.numberOfChannels; c++) {
    const channel = audio.getChannelData(c);
    for (let i = 0; i < mono.length; i++) mono[i] += channel[i];
  }
  if (audio.numberOfChannels > 1) {
    const scale = 1 / audio.numberOfChannels;
    for (let i = 0; i < mono.length; i++) mono[i] *= scale;
  }
  return { samples: mono, channels: audio.numberOfChannels };
}

// Min/max per block, so long recordings draw quickly at any zoom.
export const PEAK_BLOCK = 64;

export function peaks(samples) {
  const count = Math.ceil(samples.length / PEAK_BLOCK);
  const min = new Float32Array(count);
  const max = new Float32Array(count);
  for (let b = 0; b < count; b++) {
    let lo = 1, hi = -1;
    const end = Math.min(samples.length, (b + 1) * PEAK_BLOCK);
    for (let i = b * PEAK_BLOCK; i < end; i++) {
      const s = samples[i];
      if (s < lo) lo = s;
      if (s > hi) hi = s;
    }
    min[b] = lo;
    max[b] = hi;
  }
  return { min, max };
}

export function analyse(samples, start, end) {
  let sum = 0;
  for (let i = start; i < end; i++) sum += samples[i];
  const mean = end > start ? sum / (end - start) : 0;
  let peak = 0;
  for (let i = start; i < end; i++) {
    const s = Math.abs(samples[i] - mean);
    if (s > peak) peak = s;
  }
  return { mean, peak };
}

// Normalising targets the knee, so only extra gain saturates.
const KNEE = 0.9;

export function gainFor(settings, peak) {
  const base = settings.normalize && peak > 1e-6 ? KNEE / peak : 1;
  return base * 10 ** (settings.gain / 20);
}

export function shape(x) {
  const a = Math.abs(x);
  if (a <= KNEE) return x;
  const y = KNEE + (1 - KNEE) * Math.tanh((a - KNEE) / (1 - KNEE));
  return x < 0 ? -y : y;
}

export function envelope(i, length, fadeIn, fadeOut) {
  let g = 1;
  if (i < fadeIn) g *= 0.5 - 0.5 * Math.cos(Math.PI * i / fadeIn);
  const tail = length - 1 - i;
  if (tail < fadeOut) g *= 0.5 - 0.5 * Math.cos(Math.PI * tail / fadeOut);
  return g;
}

export function process(samples, start, end, settings) {
  const { mean, peak } = analyse(samples, start, end);
  const gain = gainFor(settings, peak);
  const length = end - start;
  const fadeIn = Math.min(length, Math.round(settings.fadeIn / 1000 * SOURCE_RATE));
  const fadeOut = Math.min(length, Math.round(settings.fadeOut / 1000 * SOURCE_RATE));
  const out = new Float32Array(length);
  for (let i = 0; i < length; i++) {
    out[i] = shape((samples[start + i] - mean) * gain) * envelope(i, length, fadeIn, fadeOut);
  }
  return out;
}

// Windowed-sinc resampler with the cutoff just below the new Nyquist.
export function resample(input, from, to) {
  if (from === to) return input;
  const ratio = from / to;
  const cutoff = 0.45 / Math.max(ratio, 1);
  const width = Math.ceil(6 / cutoff);
  const steps = 512;
  const table = new Float32Array(width * steps + 2);
  for (let i = 0; i < table.length; i++) {
    const t = i / steps;
    const u = t / width;
    if (u >= 1) continue;
    const window = 0.42 + 0.5 * Math.cos(Math.PI * u) + 0.08 * Math.cos(2 * Math.PI * u);
    const x = 2 * cutoff * t;
    table[i] = 2 * cutoff * (x === 0 ? 1 : Math.sin(Math.PI * x) / (Math.PI * x)) * window;
  }

  const length = Math.floor(input.length / ratio);
  const out = new Float32Array(length);
  const last = input.length - 1;
  for (let j = 0; j < length; j++) {
    const centre = j * ratio;
    const first = Math.max(0, Math.ceil(centre - width));
    const stop = Math.min(last, Math.floor(centre + width));
    let sum = 0;
    for (let k = first; k <= stop; k++) {
      sum += input[k] * table[Math.round(Math.abs(centre - k) * steps)];
    }
    out[j] = sum;
  }
  return out;
}

function toInt16(x) {
  return Math.max(-32768, Math.min(32767, Math.round(x * 32768)));
}

export function encodePcm10(samples) {
  const out = new Uint8Array(Math.ceil(samples.length * 10 / 8));
  for (let n = 0; n < samples.length; n++) {
    const bit = n * 10;
    const word = Math.min(1023, (toInt16(samples[n]) + 32768 + 32) >> 6) << (bit & 7);
    out[bit >> 3] |= word & 0xff;
    if (word >> 8) out[(bit >> 3) + 1] |= word >> 8;
  }
  return out;
}

function ulaw(s) {
  let sign = 0;
  if (s < 0) {
    s = -s;
    sign = 0x80;
  }
  s = Math.min(s, 32635) + 0x84;
  let exponent = 7;
  for (let mask = 0x4000; (s & mask) === 0 && exponent > 0; mask >>= 1) exponent--;
  const mantissa = (s >> (exponent + 3)) & 0x0f;
  return ~(sign | (exponent << 4) | mantissa) & 0xff;
}

export const ULAW_TABLE = Int16Array.from({ length: 256 }, (_, i) => {
  const u = ~i & 0xff;
  const t = (((u & 0x0f) << 3) + 0x84) << ((u & 0x70) >> 4);
  return u & 0x80 ? 0x84 - t : t - 0x84;
});

export function encodeUlaw(samples) {
  return Uint8Array.from(samples, x => ulaw(toInt16(x)));
}

// What the firmware sends to its 10-bit PWM output.
export function devicePreview(data, format, length) {
  const out = new Float32Array(length);
  for (let n = 0; n < length; n++) {
    let level;
    if (format === FORMAT_ULAW) {
      level = (ULAW_TABLE[data[n]] + 32768) >> 6;
    } else {
      const bit = n * 10;
      const p = bit >> 3;
      level = ((data[p] | ((data[p + 1] ?? 0) << 8)) >> (bit & 7)) & 0x3ff;
    }
    out[n] = (level * 64 + 32 - 32768) / 32768;
  }
  return out;
}

export function render(samples, start, end, settings, quality) {
  const shaped = process(samples, start, end, settings);
  const audio = resample(shaped, SOURCE_RATE, quality.rate);
  const data = quality.format === FORMAT_ULAW ? encodeUlaw(audio) : encodePcm10(audio);
  return {
    format: quality.format,
    rate: quality.rate,
    length: audio.length,
    data,
    preview: devicePreview(data, quality.format, audio.length),
  };
}

export function silenceBounds(samples, start, end) {
  const { mean, peak } = analyse(samples, start, end);
  const threshold = peak * 10 ** (-40 / 20);
  let a = start;
  while (a < end && Math.abs(samples[a] - mean) < threshold) a++;
  let b = end;
  while (b > a && Math.abs(samples[b - 1] - mean) < threshold) b--;
  const padIn = Math.round(0.01 * SOURCE_RATE);
  const padOut = Math.round(0.05 * SOURCE_RATE);
  return [Math.max(start, a - padIn), Math.min(end, b + padOut)];
}
