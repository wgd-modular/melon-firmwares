# FX LoPerformer

A simple, audio effects processor firmware for the **WGD Modular Melon** Eurorack module (based on the Raspberry Pi RP2350 microcontroller) and it predecessor Hagiwo MOD2 (untested).

**FX LoPerformer** is an adaptation of the FX part of the [*Generative Drum Synthesizer*](https://github.com/wgd-modular/melon-firmwares/tree/main/drums) firmware by Vincent Maurer. Combining a tape delay, a reverb, a flanger, a bitcrusher, and a DJ filter, it offers a comprehensive suite of effects for Eurorack systems in a compact package with performance in mind. Don't expect hifi, expect fun!

---

## Key Features

- **5 Color-Coded FX Pages**:
  - **Bitcrusher**: Combinable bit-depth and sample-rate reduction.
  - **Flanger**: High-resonance, LFO-modulated delay line.
  - **Tape Delay**: Smooth delay with continuous time adjustment and clock synchronization.
  - **Reverb**: Room reverb algorithm based on Freeverb.
  - **Master page with DJ Filter and Dry/Wet Mix**: Single-knob bi-directional filter (Lowpass $\leftarrow$ Clean $\rightarrow$ Highpass).
- **Master Dynamics & Signal Path**:
  - **SSL-style Master Bus Compressor**: Always-on "glue" compressor for volume leveling and saturation.
  - **DC Blocker**: Active 1-pole high-pass filtering on both input and output paths.
  - **Noise Gate & Input Filtering**: 4-point moving average filter and dynamic noise gate to eliminate ADC switching whine and background hiss.
  - **Parameter Auto-Save**: Saves all knob values to internal flash (EEPROM) after 10 seconds of inactivity or instantly upon changing pages.
  - **Potentiometer Pickup Lock**: Prevents sudden audio jumps when switching pages. A parameter is locked until the physical pot matches the stored value.
- **Pseudo-CV Gate Control**: Trigger Input 2 (`IN 2` / GPIO 0) acts as a gate control. Whenever a HIGH signal is received, the Dry/Wet mix is instantly maxed to 100% Wet (with a smooth 0.5ms slew to prevent digital clicks).
- **Hardware Boot Menu**: Dual support for **NeoPixel RGB LED** (WGD Melon) and **Legacy PWM LED** (e.g. Hagiwo Mod 2) via a startup hardware menu.

---

## Hardware Configuration (Pin Assignment)

| Connection | MCU Pin / GPIO | Description |
| :--- | :--- | :--- |
| **POT 1** | ADC0 / A0 | Page Parameter 1 |
| **POT 2** | ADC1 / A1 | Page Parameter 2 |
| **POT 3** | ADC1 / A1 | Unused - leave in middle position |
| **CV IN** | ADC2 / A2 | External Audio Input |
| **IN 1** | GPIO 7 | Clock / Sync Input for Tape Delay (Rising Edge) |
| **IN 2** | GPIO 0 | Pseudo-CV Gate Control (HIGH = 100% Wet Mix) |
| **BUTTON** | GPIO 6 | Navigation Button (Internal Pull-Up) |
| **OUT** | GPIO 1 | 10-bit PWM Audio Output @ 36.6 kHz |
| **LED** | GPIO 5 | WS2812B RGB / Legacy PWM LED (Configurable) |

---

## Hardware Selection (Boot Menu)

The firmware supports both the newer NeoPixel RGB hardware variant (like the WGD Melon) and older legacy single LED hardware (like the Hagiwo Mod 2). To configure your hardware:

1. Hold down the navigation **BUTTON** (GPIO 6) while powering on or resetting the module.
2. Keep holding the button. Turn **POT 2** (A1) to make your selection:
   - **Left Half (<= 50%)**: **Legacy LED Mode**. The LED will slowly pulse to confirm selection.
   - **Right Half (> 50%)**: **NeoPixel RGB Mode**. The NeoPixel will cycle through colors to confirm selection.
3. Release the button after 1 second to save your selection. The setting is stored in EEPROM and loaded automatically on future power-ups.

---

## Navigation & UI Pages

The interface uses the single push button to cycle through pages. The LED indicates the active page.

- **Short Press** (< 350ms): Move to the next page.
- **Medium Press** (350ms - 1000ms): Go back to the previous page.

When changing pages, the physical knobs are locked to prevent parameters from instantly jumping. Turn the knob until it matches the stored value to unlock it.

### Page Overview & LED Animations

| Page Index | Page Name | LED Color (NeoPixel) | LED Behavior (Legacy Single LED) | POT 1 Function | POT 2 Function |
| :---: | :--- | :--- | :--- | :--- | :--- |
| **0** | **Bitcrusher** | 🟡 Yellow | Jittery, digital noise flickering | Resolution (16 to 1-bit) | Downsampling Rate |
| **1** | **Flanger** | 🟣 Magenta | Pulsing in sync with sweep LFO rate | LFO Rate (0.05 - 5.05Hz) | Depth & Feedback |
| **2** | **Tape Delay** | 🔵 Cyan | Flashes at current delay rate/tempo | Delay Time / Sync Div | Feedback / Delay Mix |
| **3** | **Reverb** | 🟢 Green | Slow, relaxing breathing pulse | Room Size (0.7 to 0.98) | Dry/Wet Mix |
| **4** | **Master DJ** | 🔵 Blue | Solid steady light | DJ Filter (LP/Flat/HP) | Master Dry/Wet Mix |

---

## Detailed FX Operation

### 1. Bitcrusher & Downsampler (Page 0)
Digital degradation processor.
- **Resolution (POT 1)**: Quantizes the audio signal amplitude. Uses a quadratic control curve ranging from pristine 16-bit down to harsh 1-bit quantization.
- **Downsampling (POT 2)**: Decimates the sample rate by holding samples. Ranges from no decimation (1x) up to holding every 64th sample.

### 2. Flanger (Page 1)
An LFO-modulated delay line creating comb-filter sweeps.
- **Rate**: Sets the speed of the sweep LFO (0.05 Hz to 5.05 Hz).
- **Depth / Feedback**: Combines modulation depth and feedback into a single control. Turning it up increases resonance and width.

### 3. Tape Delay (Page 2)
A delay line offering up to ~890ms of echo time with continuous analog-style pitch tape-warble during time changes.
- **Manual Mode**: If no clock is detected on **IN 1**, POT 1 sweeps the delay time from 20ms to 870ms.
- **Clock Sync Mode**: When an external clock signal is received on **IN 1**, the delay time locks to the clock period. POT 1 selects a sync division multiplier:
  `[ 1/2, 3/4, 1/1, 1.5x, 2.0x, 3.0x, 4.0x ]`
- **Feedback & Mix**: POT 2 controls the feedback amount (up to self-oscillation) and the delay level simultaneously.

### 4. Reverb (Page 3)
A dense room reverb using 8 parallel comb filters and 4 cascaded all-pass filters.
- **Room Size**: Adjusts the feedback decay time of the reverb tail.
- **Mix**: Sets the relative level of the reverb reflections.

### 5. Master DJ Filter & Wet/Dry (Page 4)
The final stage of the audio path.
- **DJ Filter**: A bi-directional filter based on the Cytomic Simper state-variable filter model. 
  - **Knob < 0.45**: Lowpass filter (100 Hz to 20 kHz)
  - **Knob 0.45 - 0.55**: Bypass / Flat response
  - **Knob > 0.55**: Highpass filter (20 Hz to 10 kHz)
- **Master Dry/Wet**: Mixes between the dry input signal (directly after noise gating) and the fully processed FX chain output.

---

## Build & Flash Instructions

### Prerequisites
- Install [Arduino CLI](https://arduino.github.io/arduino-cli/) or the [Arduino IDE](https://www.arduino.cc/en/software).
- Install the Raspberry Pi RP2040/RP2350 board package (`rp2040:rp2040` by Earle F. Philhower, III).

### Compiling via Arduino CLI
Compile the firmware directly from the project directory:
```bash
arduino-cli compile --fqbn rp2040:rp2040:seeed_xiao_rp2350 --export-binaries fxloperformer.ino
```

### Flashing the Module
1. Connect the Seeed Studio Xiao RP2350 to your computer.
2. Put the RP2350 into **BOOTSEL** mode (hold the BOOT button while resetting or plugging in).
3. A mass storage drive named `RPI-RP2` will appear.
4. Drag and drop the compiled `.uf2` file (located in the `build/` directory) onto the drive. The module will reboot and start running FX LoPerformer immediately.

---

## License & Credits
- **Base Hardware**: WGD Modular Melon, inspired by the HAGIWO Mod 2.
- **Base Firmware**: Originally derived from Vincent's [*Generative Drum Synthesizer*](https://github.com/wgd-modular/melon-firmwares/tree/main/drums).
- **Development**: Refactored and optimized with help from Gemini.
- Licensed under **CC0 1.0 Universal** (Public Domain Dedication).
