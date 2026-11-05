// Puts a recording into the compiled radio firmware. Mirrors radio/postbuild.py
// and the flash layout in radio.ino.

export const SAMPLE_ADDR = 0x10020000;
export const SAMPLE_END = 0x101fe000;
export const HEADER_SIZE = 256;
export const DATA_BYTES = SAMPLE_END - SAMPLE_ADDR - HEADER_SIZE;

export const FORMAT_PCM10 = 1;
export const FORMAT_ULAW = 2;

const MAGIC = 0x4f494452;
const UF2_MAGIC_START0 = 0x0a324655;
const UF2_MAGIC_START1 = 0x9e5d5157;
const UF2_MAGIC_END = 0x0ab16f30;
const UF2_FLAG_FAMILY = 0x00002000;
const ABSOLUTE_FAMILY = 0xe48bff57;
const PAGE = 256;
const BLOCK = 512;

export function dataBytes(format, length) {
  return format === FORMAT_PCM10 ? Math.ceil(length * 10 / 8) : length;
}

function label(name) {
  const encoder = new TextEncoder();
  let text = name;
  while (encoder.encode(text).length > 47) text = text.slice(0, -1);
  return encoder.encode(text);
}

export function sampleImage({ format, rate, length, data, name }) {
  const image = new Uint8Array(HEADER_SIZE + data.length);
  const view = new DataView(image.buffer);
  view.setUint32(0, MAGIC, true);
  view.setUint16(4, 1, true);
  view.setUint8(6, format);
  view.setUint32(8, rate, true);
  view.setUint32(12, length, true);
  image.set(label(name), 16);
  image.set(data, HEADER_SIZE);
  return image;
}

export function readBlocks(uf2) {
  if (uf2.length === 0 || uf2.length % BLOCK) throw new Error('Not a UF2 file');
  const blocks = [];
  for (let offset = 0; offset < uf2.length; offset += BLOCK) {
    const view = new DataView(uf2.buffer, uf2.byteOffset + offset, BLOCK);
    const fields = Array.from({ length: 8 }, (_, i) => view.getUint32(i * 4, true));
    if (fields[0] !== UF2_MAGIC_START0 || fields[1] !== UF2_MAGIC_START1 || view.getUint32(508, true) !== UF2_MAGIC_END) {
      throw new Error(`Bad UF2 block at offset ${offset}`);
    }
    blocks.push({ fields, payload: uf2.subarray(offset + 32, offset + 32 + fields[4]) });
  }
  return blocks;
}

function program(blocks) {
  return blocks.filter(b => b.fields[7] !== ABSOLUTE_FAMILY && b.fields[3] < SAMPLE_ADDR);
}

export function firmwareEnd(uf2) {
  return Math.max(...program(readBlocks(uf2)).map(b => b.fields[3] + b.fields[4]));
}

export function addSample(uf2, image) {
  if (image.length > SAMPLE_END - SAMPLE_ADDR) throw new Error('Recording does not fit');

  const blocks = readBlocks(uf2);
  const firmware = program(blocks);
  if (!firmware.length) throw new Error('No firmware blocks found');
  const family = firmware[0].fields[7];
  const end = Math.max(...firmware.map(b => b.fields[3] + b.fields[4]));
  if (end > SAMPLE_ADDR) throw new Error('Firmware runs into the recording');

  const pages = [...firmware];
  for (let offset = 0; offset < image.length; offset += PAGE) {
    pages.push({
      fields: [UF2_MAGIC_START0, UF2_MAGIC_START1, UF2_FLAG_FAMILY, SAMPLE_ADDR + offset, PAGE, 0, 0, family],
      payload: image.subarray(offset, offset + PAGE),
    });
  }

  const extra = blocks.filter(b => b.fields[7] === ABSOLUTE_FAMILY);
  const out = new Uint8Array((extra.length + pages.length) * BLOCK);
  const view = new DataView(out.buffer);
  [...extra, ...pages].forEach((block, n) => {
    const fields = [...block.fields];
    if (n >= extra.length) {
      fields[5] = n - extra.length;
      fields[6] = pages.length;
    }
    const offset = n * BLOCK;
    fields.forEach((value, i) => view.setUint32(offset + i * 4, value, true));
    out.set(block.payload, offset + 32);
    view.setUint32(offset + 508, UF2_MAGIC_END, true);
  });
  return out;
}
