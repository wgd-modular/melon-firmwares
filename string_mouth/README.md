# String Mouth

Creator / origin: **s0ca**.

String Mouth is a triggered plucked-string physical-model voice for the HAGIWO
MOD2 / XIAO RP2350. It is designed for the normal AC-coupled audio output path.

It starts as a playable string voice, then adds body/formant stages so it can
move toward vowel, wood, bass, metal, and glassy resonator colours.

---

## Quick Start

1. Leave **JP2 open** for the normal AC-coupled audio output.
2. Patch **OUT** to a mixer.
3. Patch a trigger or gate into **IN1**.
4. Start with **POT1** around 10-11 o'clock.
5. Put **POT2** around 2 o'clock for audible decay.
6. Put **POT3** near the centre.
7. Press the button to cycle the voice model.

Do not power the module from USB and eurorack at the same time.

---

## Connections

| Jack | Function |
|---|---|
| **IN1** | Trigger input |
| **IN2** | Accent / harder excitation |
| **CV / A2** | Body/formant modulation, shared with POT3 |
| **OUT** | Audio output |

---

## Controls

| Control | Function |
|---|---|
| **POT1** | Pitch |
| **POT2** | Decay / damping, short pluck to long ringing |
| **POT3** | Body / formant / pickup tone morph |
| **Button short press** | Cycle voice model: String, Mouth, Metal, Bass, Glass |
| **Button hold >700 ms** | Save current voice model |

`POT3` moves through a folded body/tone morph rather than a simple linear
minimum-to-maximum sweep. This is intentional: on MOD2, `POT3` and the external
`CV` input share the `A2` analog mixer, so the pot behaves partly like an offset
and range control. The folded response makes the physical pot audible on its
own, while a patched LFO still sweeps the same body/formant character.

---

## Voice Models

| Model | Sound |
|---|---|
| **String** | Cleanest plucked-string voice, warm body control |
| **Mouth** | Vowel-ish resonant body, more vocal and nasal |
| **Metal** | Bright inharmonic body with hard pickup tone |
| **Bass** | Rounder low string with longer body resonance |
| **Glass** | Bright ringing resonator, glassy and more synthetic |

The LED blinks 1-5 times to show the selected model.

---

## LED Hardware Mode

String Mouth supports both original MOD2 plain LED mode and MELON RGB LED mode.
The boot LED selector is adapted from WGD Modular's FX LoPerformer hardware
selection menu.

String Mouth boots in MELON WS2812B RGB LED mode by default. Original MOD2
Legacy PWM LED can be selected from the boot menu for that session.

To change LED hardware mode:

1. Hold the button while powering on or resetting the module, and keep holding
   for about one second.
2. Keep holding and turn **POT2**:
   - left half = original MOD2 Legacy PWM LED
   - right half = MELON WS2812 / NeoPixel RGB LED
3. Release the button to save.

On MELON RGB LED mode:

| State | Color |
|---|---|
| String model | Amber |
| Mouth model | Pink |
| Metal model | Pale blue |
| Bass model | Green |
| Glass model | Ice white |
| Saving model | Full brightness |

---

## Board Setup

| Jumper | Setting | Reason |
|---|---|---|
| **JP1** | 10n or 10n+22n | 10n is brighter, 10n+22n is smoother |
| **JP2** | **OPEN** | Use the normal AC-coupled audio output |

This is an audio firmware. It does not require the DC output mod.

---

## Technical Notes

The core is a short noise burst fed into a Karplus-Strong style delay line
running at 36,621 Hz. `POT2` controls feedback damping. `POT3/CV` moves the
resonant body stage after the string.

The pitch control is intentionally broad rather than calibrated 1V/oct. Treat it
as a playable percussion/string voice, not a precision oscillator.

---

## License

CC0 1.0 Universal - public domain.
