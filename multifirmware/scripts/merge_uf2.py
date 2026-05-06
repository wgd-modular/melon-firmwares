#!/usr/bin/env python3
"""
merge_uf2.py — Merge multiple UF2 files into a single combined UF2.

UF2 format: each block is 512 bytes with a target address.
This tool concatenates blocks from multiple UF2 files, verifying
that address ranges don't overlap.

Usage:
    python3 merge_uf2.py -o combined.uf2 bootloader.uf2 slot0.uf2 slot1.uf2 ...
"""

import argparse
import struct
import sys

# UF2 constants
UF2_MAGIC_START0 = 0x0A324655  # "UF2\n"
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_BLOCK_SIZE = 512
UF2_DATA_SIZE = 256  # payload per block
UF2_FLAG_FAMILYID = 0x00002000
RP2350_FAMILY_ID = 0xE48BFF59


def parse_uf2(data, filename=""):
    """Parse a UF2 file and return list of (address, payload) tuples."""
    blocks = []
    if len(data) % UF2_BLOCK_SIZE != 0:
        print(f"Warning: {filename} size not multiple of 512", file=sys.stderr)

    num_blocks = len(data) // UF2_BLOCK_SIZE
    for i in range(num_blocks):
        block = data[i * UF2_BLOCK_SIZE : (i + 1) * UF2_BLOCK_SIZE]
        magic0, magic1, flags, addr, size, block_no, num_blks, family = (
            struct.unpack_from("<IIIIIIII", block, 0)
        )
        (magic_end,) = struct.unpack_from("<I", block, 508)

        if magic0 != UF2_MAGIC_START0 or magic1 != UF2_MAGIC_START1:
            print(f"Warning: {filename} block {i} bad start magic", file=sys.stderr)
            continue
        if magic_end != UF2_MAGIC_END:
            print(f"Warning: {filename} block {i} bad end magic", file=sys.stderr)
            continue

        payload = block[32 : 32 + size]
        blocks.append((addr, payload, flags, family))

    return blocks


def make_uf2_block(addr, payload, block_no, num_blocks, family_id=RP2350_FAMILY_ID):
    """Create a single 512-byte UF2 block."""
    flags = UF2_FLAG_FAMILYID
    data_size = len(payload)

    # Pad payload to 476 bytes (512 - 32 header - 4 end magic)
    padded = payload + b"\x00" * (476 - data_size)

    block = struct.pack(
        "<IIIIIIII",
        UF2_MAGIC_START0,
        UF2_MAGIC_START1,
        flags,
        addr,
        data_size,
        block_no,
        num_blocks,
        family_id,
    )
    block += padded
    block += struct.pack("<I", UF2_MAGIC_END)

    assert len(block) == 512
    return block


def offset_uf2(input_path, offset):
    """Read a UF2 file and offset all target addresses by `offset` bytes."""
    with open(input_path, "rb") as f:
        data = f.read()

    blocks = parse_uf2(data, input_path)
    result = []
    for addr, payload, flags, family in blocks:
        result.append((addr + offset, payload, flags, family))
    return result


def merge_uf2_files(file_list, output_path):
    """Merge multiple UF2 files, checking for overlaps."""
    FLASH_START = 0x10000000
    FLASH_END = 0x10200000  # 2 MB
    all_blocks = []
    seen_addrs = set()
    skipped = 0

    for path in file_list:
        with open(path, "rb") as f:
            data = f.read()
        blocks = parse_uf2(data, path)
        added = 0
        for block in blocks:
            addr = block[0]
            if addr < FLASH_START or addr >= FLASH_END:
                skipped += 1
                continue
            if addr in seen_addrs:
                print(
                    f"  Warning: {path} duplicate address 0x{addr:08X} (skipped)",
                    file=sys.stderr,
                )
            else:
                seen_addrs.add(addr)
                all_blocks.append(block)
                added += 1
        print(f"  {path}: {len(blocks)} blocks, {added} in range", file=sys.stderr)

    if skipped:
        print(
            f"  (skipped {skipped} blocks outside 0x{FLASH_START:08X}-0x{FLASH_END:08X})",
            file=sys.stderr,
        )

    all_blocks.sort(key=lambda b: b[0])

    for i in range(1, len(all_blocks)):
        prev_addr, prev_data, _, _ = all_blocks[i - 1]
        curr_addr = all_blocks[i][0]
        prev_end = prev_addr + len(prev_data)
        if curr_addr < prev_end:
            print(
                f"ERROR: Address overlap at 0x{curr_addr:08X} "
                f"(previous block ends at 0x{prev_end:08X})",
                file=sys.stderr,
            )
            sys.exit(1)

    # Determine family ID from first block
    family_id = all_blocks[0][3] if all_blocks else RP2350_FAMILY_ID

    # Write combined UF2
    num_blocks = len(all_blocks)
    with open(output_path, "wb") as f:
        for i, (addr, payload, flags, family) in enumerate(all_blocks):
            block = make_uf2_block(addr, payload, i, num_blocks, family_id)
            f.write(block)

    print(
        f"  -> {output_path}: {num_blocks} blocks, {num_blocks * 256 / 1024:.1f} KB",
        file=sys.stderr,
    )


def main():
    parser = argparse.ArgumentParser(description="Merge multiple UF2 files")
    parser.add_argument("-o", "--output", required=True, help="Output UF2 file")
    parser.add_argument("inputs", nargs="+", help="Input UF2 files")
    args = parser.parse_args()

    print("Merging UF2 files:", file=sys.stderr)
    merge_uf2_files(args.inputs, args.output)
    print("Done.", file=sys.stderr)


if __name__ == "__main__":
    main()
