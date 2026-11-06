#!/usr/bin/env python3
"""Put a recording into a compiled radio.uf2.

The firmware reads its recording from a fixed flash region, so it can be
swapped without rebuilding. CI runs this to ship sample.wav with the release
build, and the web tool does the same in the browser (web/uf2.js).

    python3 radio/postbuild.py radio.uf2 [recording.wav]

The WAV has to be 16-bit PCM. Stereo is mixed down to mono, the sample rate
is kept as it is.
"""

import argparse
import struct
import sys
import wave
from array import array
from pathlib import Path

SAMPLE_ADDR = 0x10020000
SAMPLE_END = 0x101FE000
HEADER_SIZE = 256
MAGIC = 0x4F494452
FORMAT_PCM10 = 1

UF2_MAGIC_START = (0x0A324655, 0x9E5D5157)
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY = 0x00002000
ABSOLUTE_FAMILY = 0xE48BFF57
PAGE = 256


def read_wav(path):
    with wave.open(str(path)) as w:
        if w.getsampwidth() != 2:
            sys.exit(f"{path}: only 16-bit WAV files are supported")
        rate, channels = w.getframerate(), w.getnchannels()
        samples = array("h", w.readframes(w.getnframes()))
    if sys.byteorder == "big":
        samples.byteswap()
    if channels > 1:
        samples = array("h", (
            sum(samples[i:i + channels]) // channels
            for i in range(0, len(samples), channels)
        ))
    return rate, samples


def pack10(samples):
    """10 bits per sample, least significant bits first, like the PWM output."""
    out = bytearray((len(samples) * 10 + 7) // 8)
    for n, s in enumerate(samples):
        bit = n * 10
        word = min(1023, (s + 32768 + 32) >> 6) << (bit & 7)
        out[bit >> 3] |= word & 0xFF
        if word >> 8:
            out[(bit >> 3) + 1] |= word >> 8
    return bytes(out)


def sample_image(rate, samples, name):
    data = pack10(samples)
    label = name.encode()[:47]
    header = struct.pack("<IHBBII48s", MAGIC, 1, FORMAT_PCM10, 0, rate, len(samples), label)
    return header.ljust(HEADER_SIZE, b"\0") + data


def read_blocks(uf2):
    if len(uf2) % 512:
        sys.exit("not a UF2 file")
    blocks = []
    for offset in range(0, len(uf2), 512):
        block = uf2[offset:offset + 512]
        fields = list(struct.unpack_from("<8I", block))
        if tuple(fields[:2]) != UF2_MAGIC_START or struct.unpack_from("<I", block, 508)[0] != UF2_MAGIC_END:
            sys.exit(f"bad UF2 block at offset {offset}")
        blocks.append((fields, block[32:32 + fields[4]]))
    return blocks


def write_block(fields, payload):
    return struct.pack("<8I", *fields) + payload.ljust(476, b"\0") + struct.pack("<I", UF2_MAGIC_END)


def add_sample(uf2, image):
    if len(image) > SAMPLE_END - SAMPLE_ADDR:
        sys.exit(f"recording is {len(image) - (SAMPLE_END - SAMPLE_ADDR)} bytes too long")

    blocks = read_blocks(uf2)
    program = [b for b in blocks if b[0][7] != ABSOLUTE_FAMILY and b[0][3] < SAMPLE_ADDR]
    if not program:
        sys.exit("no firmware blocks found")
    family = program[0][0][7]
    end = max(fields[3] + fields[4] for fields, _ in program)
    if end > SAMPLE_ADDR:
        sys.exit(f"firmware ends at {end:#x} and runs into the recording at {SAMPLE_ADDR:#x}")

    for offset in range(0, len(image), PAGE):
        fields = [*UF2_MAGIC_START, UF2_FLAG_FAMILY, SAMPLE_ADDR + offset, PAGE, 0, 0, family]
        program.append((fields, image[offset:offset + PAGE].ljust(PAGE, b"\0")))

    for n, (fields, _) in enumerate(program):
        fields[5], fields[6] = n, len(program)

    extra = [b for b in blocks if b[0][7] == ABSOLUTE_FAMILY]
    return b"".join(write_block(*b) for b in extra + program)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("uf2", type=Path, help="compiled radio.uf2, rewritten in place")
    parser.add_argument("wav", type=Path, nargs="?", default=Path(__file__).with_name("sample.wav"))
    args = parser.parse_args()

    rate, samples = read_wav(args.wav)
    image = sample_image(rate, samples, args.wav.name)
    args.uf2.write_bytes(add_sample(args.uf2.read_bytes(), image))
    print(f"{args.uf2}: added {args.wav.name}, {len(samples) / rate:.1f} s at {rate} Hz")


if __name__ == "__main__":
    main()
