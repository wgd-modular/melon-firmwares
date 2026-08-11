#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
REPO_ROOT="$(dirname "$PROJECT_DIR")"
MOD2_DIR="${MOD2_DIR:-$REPO_ROOT}"
BUILD_DIR="${BUILD_DIR:-/tmp/melon-build}"
OUTPUT_DIR="$PROJECT_DIR/output"

BOARD="rp2040:rp2040:seeed_xiao_rp2350"
ARDUINO_CORE_DIR="$(find "$HOME/.arduino15/packages/rp2040/hardware/rp2040" -maxdepth 1 -mindepth 1 -type d 2>/dev/null | sort -V | tail -1)"
if [[ -z "$ARDUINO_CORE_DIR" || ! -d "$ARDUINO_CORE_DIR" ]]; then
    echo "ERROR: Could not find rp2040 Arduino core. Install via arduino-cli."
    exit 1
fi
echo "Arduino core: $ARDUINO_CORE_DIR"
LINKER_TEMPLATE="$ARDUINO_CORE_DIR/lib/rp2350/memmap_default.ld"
LINKER_BACKUP="$LINKER_TEMPLATE.orig"

BOOTLOADER_SIZE=0x20000  # 128 KB
SLOT_SIZE=0x3C000        # 240 KB
EEPROM_SIZE=0x2000       # 8 KB per slot
FLASH_BASE=0x10000000

SLOT_NAMES=(braids kick clap hihat vco fm_drum claves mod303)
SLOT_DIRS=(braids kick clap hihat vco fm_drum claves mod303)
SLOT_COLORS=("Blue" "Red" "Orange" "Yellow" "Green" "Purple" "Cyan" "Magenta")
NUM_SLOTS=8

cleanup() {
    if [[ -f "$LINKER_BACKUP" ]]; then
        cp "$LINKER_BACKUP" "$LINKER_TEMPLATE"
        rm -f "$LINKER_BACKUP"
        echo "Restored original linker script."
    fi
}
trap cleanup EXIT

echo "=== Melon Multi-Firmware Build ==="
echo "MOD2 source: $MOD2_DIR"
echo "Build dir:   $BUILD_DIR"
echo "Output dir:  $OUTPUT_DIR"
echo ""

mkdir -p "$OUTPUT_DIR" "$BUILD_DIR"

for dir in "${SLOT_DIRS[@]}"; do
    ino="$MOD2_DIR/$dir/$dir.ino"
    if [[ ! -f "$ino" ]]; then
        echo "ERROR: Firmware not found: $ino"
        exit 1
    fi
done

echo "--- Building bootloader ---"
BOOTLOADER_BUILD="$BUILD_DIR/bootloader"
arduino-cli compile \
    -b "$BOARD" \
    --build-path "$BOOTLOADER_BUILD" \
    "$PROJECT_DIR/bootloader/bootloader.ino" 2>&1 | tail -3

cp "$BOOTLOADER_BUILD/bootloader.ino.uf2" "$OUTPUT_DIR/bootloader.uf2"
echo "Bootloader: OK"
echo ""

echo "--- Patching firmwares for Melon (WS2812B LED + button hold) ---"
bash "$SCRIPT_DIR/patch_led.sh"
echo ""

cp "$LINKER_TEMPLATE" "$LINKER_BACKUP"

for slot in $(seq 0 $((NUM_SLOTS - 1))); do
    name="${SLOT_NAMES[$slot]}"
    dir="${SLOT_DIRS[$slot]}"
    color="${SLOT_COLORS[$slot]}"

    slot_base=$((FLASH_BASE + BOOTLOADER_SIZE + slot * SLOT_SIZE))
    eeprom_start=$((slot_base + SLOT_SIZE - EEPROM_SIZE))
    flash_length=$((SLOT_SIZE - EEPROM_SIZE))

    slot_base_hex=$(printf "0x%08X" $slot_base)
    eeprom_start_dec=$eeprom_start

    echo "--- Slot $slot: $name ($color) @ $slot_base_hex ---"

    sed "s/ORIGIN = 0x10000000/ORIGIN = $slot_base_hex/" \
        "$LINKER_BACKUP" > "$LINKER_TEMPLATE"

    SLOT_BUILD="$BUILD_DIR/slot${slot}_${name}"
    ino="$MOD2_DIR/$dir/$dir.ino"

    arduino-cli compile \
        -b "$BOARD" \
        --build-path "$SLOT_BUILD" \
        --build-property "build.eeprom_start=$eeprom_start_dec" \
        --build-property "build.flash_length=$flash_length" \
        --build-property "build.fs_start=$eeprom_start_dec" \
        --build-property "build.fs_end=$eeprom_start_dec" \
        --build-property "compiler.c.elf.extra_flags=-Wl,--allow-multiple-definition" \
        "$ino" 2>&1 | tail -3
    # --allow-multiple-definition: patch_led.sh injects melon_led.h which brings
    # a NeoPixel instance that may duplicate symbols from original firmware LED code.

    cp "$SLOT_BUILD/$dir.ino.uf2" "$OUTPUT_DIR/slot${slot}_${name}.uf2"

    size=$(stat -c%s "$SLOT_BUILD/$dir.ino.bin" 2>/dev/null || echo "?")
    echo "Slot $slot ($name): OK — $size bytes"
    echo ""
done

cp "$LINKER_BACKUP" "$LINKER_TEMPLATE"
rm -f "$LINKER_BACKUP"

echo "--- Merging into combined UF2 ---"

UF2_LIST=("$OUTPUT_DIR/bootloader.uf2")
for slot in $(seq 0 $((NUM_SLOTS - 1))); do
    name="${SLOT_NAMES[$slot]}"
    UF2_LIST+=("$OUTPUT_DIR/slot${slot}_${name}.uf2")
done

python3 "$SCRIPT_DIR/merge_uf2.py" \
    -o "$OUTPUT_DIR/melon_combined.uf2" \
    "${UF2_LIST[@]}"

rm -rf "${BACKUP_DIR:-/tmp/melon-patch-backup}"

echo ""
echo "=== Build complete ==="
echo "Combined firmware: $OUTPUT_DIR/melon_combined.uf2"
echo ""
echo "Flash via BOOTSEL: hold BOOTSEL, connect USB, drop melon_combined.uf2"
echo ""
echo "Slot layout:"
for slot in $(seq 0 $((NUM_SLOTS - 1))); do
    slot_base=$((FLASH_BASE + BOOTLOADER_SIZE + slot * SLOT_SIZE))
    printf "  Slot %d: %-8s @ 0x%08X  [%s]\n" \
        "$slot" "${SLOT_NAMES[$slot]}" "$slot_base" "${SLOT_COLORS[$slot]}"
done
