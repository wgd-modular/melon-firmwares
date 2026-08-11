# Multi-Firmware Switcher

Store all 8 firmwares on one Melon module and switch between them without reflashing.

## Slot Layout

| Slot | Firmware | LED Color | Flash Address |
|------|----------|-----------|---------------|
| 0 | Braids | Blue | 0x10020000 |
| 1 | Kick | Red | 0x1005C000 |
| 2 | Clap | Orange | 0x10098000 |
| 3 | Hi-Hat | Yellow | 0x100D4000 |
| 4 | VCO | Green | 0x10110000 |
| 5 | FM Drum | Purple | 0x1014C000 |
| 6 | Claves | Cyan | 0x10188000 |
| 7 | MOD303 | Magenta | 0x101C4000 |

Flash layout: 128 KB bootloader + 8 x 240 KB slots = 2 MB (full flash).

## Flashing

1. Hold **BOOTSEL** on the XIAO RP2350
2. Connect USB
3. Drag `output/melon_combined.uf2` onto the `RPI-RP2` drive
4. Module reboots into the last selected firmware

## Switching Firmwares

**From any running firmware:** hold the button for **5 seconds**. The module reboots into the selection menu.

**At power-on:** hold the button immediately for 5 seconds (fallback method).

### Menu

1. LED blinks 3x in the current slot's color
2. **Short press** cycles to the next slot -- LED changes to that slot's color
3. **Pot 1 (A0)** jumps directly to a slot by knob position
4. **Long press** (>800 ms) confirms your selection
5. **No input for 5 seconds** auto-confirms the current selection
6. LED flashes **white 3x** to confirm, then boots the selected firmware

The selected slot is stored in flash and persists across power cycles.

## LED Behavior

Each firmware shows its slot color on the WS2812B LED:

- **Bright** = active mode / triggered
- **Dim** = alternate mode / idle (the LED never turns fully off, so you always know which firmware is loaded)

## Building from Source

### Prerequisites

- `arduino-cli` with the [Earle Philhower RP2040/RP2350 core](https://github.com/earlephilhower/arduino-pico) installed
- Libraries: `Adafruit NeoPixel`, `Bounce2`, `RPI_PICO_TimerInterrupt`
- BRAIDS and STMLIB libraries from [poetaster/arduinoMI](https://github.com/poetaster/arduinoMI)

### Build

```bash
cd multifirmware
bash scripts/build_all.sh
```

This will:
1. Compile the bootloader at `0x10000000`
2. Patch all firmwares for WS2812B LED and 5-second hold detection
3. Compile each firmware at its slot offset (modified linker script)
4. Merge everything into `output/melon_combined.uf2`

Individual slot UF2 files are also available in `output/` for debugging.

## How It Works

The bootloader lives in the first 128 KB of flash. Each firmware is compiled with a different `FLASH ORIGIN` in the linker script so all code addresses are correct for its slot position. On boot, the bootloader reads the active slot index from flash, then jumps directly to that firmware's vector table (at slot base + 0x3000, after the OTA section).

When a firmware detects a 5-second button hold, it writes a magic value (`0xCAFEAFFE`) to the RP2350's watchdog scratch register and triggers a watchdog reset. The bootloader checks this register on startup and enters the menu directly.
