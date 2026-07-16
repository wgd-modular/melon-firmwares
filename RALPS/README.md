# RALPS — Random Algorithmic Looping Percussion Synthesizer

**RALPS** is a complex percussion synthesizer module firmware for Eurorack, designed for spontaneous sound design and evolving rhythmic structures. RALPS is based on **controlled randomness**. It generates unique patches and rhythmic sequences at the touch of a button, intelligently mapping synthesis parameters of 16 sound engines to an intuitive macro-interface. Whether you use it as a standalone drum synth or an algorithmic loop generator, RALPS ensures you are always just one "Chaos" hit away from a new inspiring sound. RALPS runs on WGD Modular Melon and Hagiwo Mod 2.

![RALPS Header](https://img.shields.io/badge/Hardware-RP2350-blue) ![Firmware](https://img.shields.io/badge/Version-1.2-green) ![Platform](https://img.shields.io/badge/Melon-WGD_Modular-orange)

---

## 🛠️ Hardware Platform

- **WGD Melon** (Recommended): RALPS works best on the Melon hardware by **WGD Modular**, an evolution of the Hagiwo MOD2, equipped with a Neopixel LED for instant visual feedback. 
- **Hagiwo MOD2**: RALPS also runs on the original Hagiwo MOD2 hardware with limited visual feedback. LED type is selectable via boot menu. 

---
## Getting Started

Start the module, push the button and you will hear a random sound. Push again to hear another one. Feed a trigger to the Trig 1 input to repeat it and turn the potentiometers to sculpt it. If you want groove, push and hold the button until the light flashes and release it to enter loop mode. Keep playing with the potentiometers to change all sounds at once. Push the button to change the sounds. Soon you will have a nice percussion loop going. RALPS has some more tricks up its sleeve. Continue reading or watch the video to learn more about the advanced features of RALPS.


---
## Introduction Video
[![RALPS introduction](http://img.youtube.com/vi/dKPXP8bYqSg/0.jpg)](http://www.youtube.com/watch?v=dKPXP8bYqSg "One Button = Infinite Drum Sounds? (Meet RALPS)
")

## 🔄 Operating Modes

RALPS has two operating modes: One-Shot Mode and Loop Mode. On startup, the module is in One-Shot Mode. You can switch between modes by holding the button for 2 seconds.

### 1. One-Shot Mode (Manual)
The module acts as a standard percussion voice. `Trig 1` input fires the current sound. `Trig 2` or a quick button tap randomizes the engine's parameters instantly.

**Sound Design (Shift Mode — Hold Button):**
*   **Pot 1**: **Decay** (Envelope Release)
*   **Pot 2**: **Timbre A** (Controls one or two randomly selected sound parameters)
*   **Pot 3**: **Timbre B** (Controls one or two randomly selected sound parameters. CVable)
*   **Shift + Pot 1**: **Engine Select** (Cycles through all 16 synthesis engines).
*   **Shift + Pot 2**: **Parameter A** ("Hidden" parameter for deeper sound sculpting).
*   **Shift + Pot 3**: **Parameter B** (Second "hidden" parameter for deeper sound sculpting).

### 2. Loop Mode (Algorithmic)
In loop mode, RALPS becomes a self-contained rhythm generator. It cycles through eight internal memory slots based on selected **Groove Patterns**. On startup, the eight slots are filled with random patches. Every time you hit the "Chaos" button, the current slot is randomized and written to the buffer. By default, RALPS cycles through all eight slots, but you can shape your loop by selecting a groove and restricting the cycle to 2-8 different sounds using the **Slot Limit** control. Potentiometers control the sound design as in One-Shot Mode, for all slots on the fly.

**Sound Design:**
*   **Pot 1**: **Decay** (Envelope Release)
*   **Pot 2**: **Timbre A** (Controls one or two randomly selected sound parameters)
*   **Pot 3**: **Timbre B** (Controls one or two randomly selected sound parameters. CVable)
**Sequencer Config (Shift Mode — Hold Button):**
*   **Shift + Pot 1**: **Slot Limit** (Restrict the cycle to 2-8 different sounds).
*   **Shift + Pot 2**: **Groove Select** (Select patterns like Swing, Breakbeat, or "Drunk Walk").
*   **Shift + Pot 3**: **Random Bias** (Biases the random selection towards specific engine groups. Full CCW: physical modeling, Full CW: Harsher, noise-based engines. Center: No bias/full random).

---

## 🔀 Smart Potentiometer Routing (Dynamic Mapping 2.0)
RALPS uses a sophisticated **Symmetric Chaos** logic to build its performance interface. Every time you hit the "Chaos" button, you get a unique configuration of the current sound engine's parameters. RALPS shuffles all 6 synthesis parameters and builds a unique, layered macro system.


---

## 🧬 Synthesis Engines
RALPS features **16 distinct engines**, from physical modeling to complex FM synthesis. Some engines are based on legendary open-source designs most of them are completely original. In **One-Shot Mode**, you can manually select the engine with **Shift + Pot 1** and adjust "hidden" parameters with **Shift + Pot 2** and **Shift + Pot 3**.

| ID | Color | Engine | Description |
| :--- | :--- | :--- | :--- |
| **0** | $\color{#FF0000}{\text{██}}$ | **KLONK** | Modal resonator for bell and percussion sounds. Mimics struck surfaces with deep control over inharmonicity. (Based on code by Mutable Instruments Rings) |
| **1** | $\color{#800000}{\text{██}}$ | **PLUCK** | Karplus-Strong physical string modeling. Ranging from harpsichord-like plucks to dampened percussive strikes. |
| **2** | $\color{#FF4500}{\text{██}}$ | **WOODY** | Specialized resonator for wooden tones. Ideal for mimicking marimbas, log drums, and organic, earthy elements. |
| **3** | $\color{#FFFF00}{\text{██}}$ | **TUBE** | Simulates a physical pipe excited by air. Produces deep, hollow tones and industrial resonances. (Based on code by Mutable Instruments Elements) |
| **4** | $\color{#800080}{\text{██}}$ | **PWM** | Classic pulse-width modulated square oscillator with a sub-generator. Edgy. |
| **5** | $\color{#FF7F50}{\text{██}}$ | **BOUNCE** | Gravity-based physics model of a bouncing ball. Creates natural "rolling" and "accelerating" rhythms. |
| **6** | $\color{#00FFFF}{\text{██}}$ | **RESO2** | Dual-pinged filter bank for complex metallic textures. Excellent for modeling cymbals, gongs, and metallic clangs. |
| **7** | $\color{#00FF00}{\text{██}}$ | **VOX** | Vowel-synthesis engine for vocal textures. Uses formants to create eerie, human-like "mouth" sounds. |
| **8** | $\color{#32CD32}{\text{██}}$ | **2OPFM** | High-fidelity 2-Operator Phase Modulation FM. Delivers the classic FM "knock" and digital transients. (Based on code by Super Synthesis 2OPFM) |
| **9** | $\color{#FF00FF}{\text{██}}$ | **FOLD** | Heavy wave-folding engine that bends simple waves into harmonic chaos. Great for distorted, gritty percussion. |
| **10** | $\color{#0000FF}{\text{██}}$ | **MULTI** | Multi-algorithm FM stack for complex textures. Perfect for bright metallic bells and FM-grime textures. |
| **11** | $\color{#008080}{\text{██}}$ | **ZAP** | Dedicated pitch-sweep generator. Covers everything from deep sub-kicks to electronic "zap" effects. |
| **12** | $\color{#FFFFFF}{\text{██}}$ | **SNARE** | Specialized snare-drum model with independent control over the body and the snare-wire noise. (Based on code by Mutable Instruments Plaits) |
| **13** | $\color{#FFBF00}{\text{██}}$ | **BYTE** | Bit-based algorithmic noise and tone generation. Creates quirky 8-bit artifacts and digital glitches. |
| **14** | $\color{#EE82EE}{\text{██}}$ | **HIHAT** | Oscillator-cluster modeled after classic 808-style hats with a stable bandpass filter. (Based on code by Mutable Instruments Plaits) |
| **15** | $\color{#87CEEB}{\text{██}}$ | **SHAKR** | Stochastic particle synthesis for shaker-type instruments. Mimics organic rattles with granular control. |

---

## 🌓 Boot Menu for Hardware Selection
RALPS by default is configured to work with the **WGD Modular Melon** hardware with NeoPixel RGB LED. However, it is designed to be backwards compatible with hardware variants like the original Hagiwo MOD2. In the boot menu you can configure your hardware type:
1. **Hold the Button** while powering on the module.
2. **Turn Pot 1 (Top)** to the right (CW)
3. **Turn Pot 2 (Middle)** to select your LED type:
   - **Right (CW)**: **NeoPixel Mode** (Blinking Colors). Optimized for the Melon NeoPixel RGB variant.
   - **Left (CCW)**: **Legacy Mode** (Static LED). Optimized for single-color LED hardware.
4. **Release the Button** to save your selection to non-volatile memory and start regular operation.
5. Note that turning Pot 1 to the left in boot mode activates a currently undocumented alternative firmware.

---

## 📜 Credits & Licensing
- **Idea & Development**: [MXZR](https://instagram.com/mxzrmxzrmxzr)
- **Code Assistance**: Code was conceived and developed by a human using AI assistance (Antigravity). If that's a concern for you, that's valid.
- **Inspiration**: 
  - [Intellijel Plonk](https://intellijel.com/eurorack-modules/plonk/) (Randomized Physical Modeling concept).
  - [Music Thing Modular Turing Machine](https://www.musicthing.co.uk/Turing-Machine/) (Random buffer concept).
- **Algorithmic Sources**:
  - [Mutable Instruments](https://github.com/pichenettes/eurorack) (code fragments from Rings, Plaits, Elements).
  - [Super Synthesis](https://github.com/supersynthesis) (2OPFM logic).
- **Hardware**:
  - [Hagiwo](https://note.com/solder_state/n/nce8f7defcf98) (Original MOD2 hardware design).
  - [WGD Modular](https://wgdmodular.de/module/melon/) (Melon hardware design).
- **License**:
  - [Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International License](https://creativecommons.org/licenses/by-nc-sa/4.0/)

---

## 🚀 Flashing the Firmware
1. Download the latest `RALPS.ino.uf2` file.
2. Put your RP2350 (Pico 2) into **BOOTSEL** mode.
3. Drag and drop the `.uf2` file onto the RPI-RP2 drive.
4. The module will reboot and start the color-palette scan.

*Warning: RALPS is an experimental tool. It thrives on chaos. If it sounds broken, or not broken enough, press the button!*
