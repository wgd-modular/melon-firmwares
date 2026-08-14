# Palimpsest

Creator / origin: **s0ca**.

Palimpsest is a dark, gritty tape echo firmware for the HAGIWO MOD2 /
XIAO RP2350. It uses the MOD2 `CV` jack as a lo-fi audio input, writes the sound
into a short 16-bit RAM tape, and plays it back through virtual tape heads.

This is not a clean digital delay. The MOD2 CV input path is bandwidth-limited,
slightly dirty, and easy to overdrive. Palimpsest uses that as part of the sound:
dark repeats, compression, grit, hiss, and unstable tape movement are expected.

---

## Quick Start

1. Set **JP2 open** for normal AC-coupled audio output.
2. Set **JP1** to 10n+22n for the darker 5 kHz output filter.
3. Patch a quiet or attenuated audio signal into **CV**.
4. Patch **OUT** to a mixer.
5. Put **POT3** near the centre.
6. Start with **POT2** fully left.
7. Set **POT1** around 9-10 o'clock for a short delay.
8. Raise **POT2** slowly for repeats.
9. Use **IN2** high to freeze the current tape.

Do not power the module from USB and eurorack at the same time.

---

## Core Specs

```text
Tape storage: 16-bit signed linear
Tape rate:    18,310.5 Hz
Tape length:  131,072 samples
Max time:     7.16 s
Dry signal:   disabled
Autoload:     disabled
```

The tape runs continuously in RAM. Palimpsest does not try to behave like a
sample recorder: it is meant to stay immediate, dirty, and playable as a tape
echo.

---

## Connections

| Jack | Function |
|---|---|
| **CV / A2** | Lo-fi audio input |
| **IN1** | Tap tempo / sync input |
| **IN2** | Freeze gate |
| **OUT** | Audio output |

The CV input is inverted in hardware and corrected in firmware.

---

## Controls

| Control | Function |
|---|---|
| **POT1** | Delay time, about 20 ms to 7.0 s |
| **POT2** | Feedback |
| **POT3** | Analog input drive / bias |
| **Button short press** | Tap tempo |
| **Button hold >3 s, release** | Clear current RAM tape |
| **Button + POT1** | Select playback head mode |
| **Button + POT2** | Set wow and flutter depth |
| **Button + POT3** | Select tape character |

Button-plus-pot gestures use the RALPS-style shift layer: if a pot moves while
the button is held, that pot gesture takes over and the release action is
suppressed.

---

## POT3 Drive

`POT3` is part of the analog input path. It shifts the input bias before the ADC.

- Near the centre: maximum input headroom.
- Away from centre: more asymmetric clipping.
- With eurorack-level audio: the CV input can saturate before conversion.

If the input is too crushed, attenuate the source before the `CV` jack or move
`POT3` back toward the centre.

---

## Head Modes

Hold the button and move `POT1` to select one of six playback head layouts.

| Position | Mode | Character |
|---|---|---|
| 1 | **Single** | One main tape head, clearest repeat |
| 2 | **Slap** | Main head plus short secondary slap |
| 3 | **Triplet** | Three evenly spaced rhythmic heads |
| 4 | **Dub Scatter** | Uneven multi-head dub pattern |
| 5 | **Smear Cloud** | Close heads for blurred, smeared repeats |
| 6 | **Cluster** | Main head plus tight secondary cluster |

Use head modes like delay patterns. They are musical spacings, not exact clock
divisions.

---

## Tape Character

Hold the button and move `POT3` to select one of four tape characters.

| Character | Sound |
|---|---|
| **Tape** | Most neutral response, lower wobble, widest clean range |
| **Dub** | Darker feedback bloom, rounder repeats, earlier soft limiting |
| **Smear** | Soft input, heavily damped feedback, blurrier magnetic memory |
| **Unstable** | More wobble, more edge, controlled crunch, and added dirt |

POT3 still controls analog input drive during normal use. Character selection
only happens while the button is held.

---

## Feedback Behaviour

`POT2` controls feedback. At high feedback settings, Palimpsest ducks the
feedback path when new input is present so fresh audio can still enter the tape.
This keeps feedback-at-full from masking new hits completely.

Feedback is intentionally coloured and limited. It should bloom, smear, and get
gritty, but it should not prevent new input from being recorded.

---

## Freeze

Patch a gate to **IN2**:

- **IN2 low:** the tape records incoming audio and feedback.
- **IN2 high:** recording stops and the existing tape keeps looping.

Freeze is useful for grabbing a moment, making a drone, or turning a delay tail
into a texture.

## LED Hardware Mode

Palimpsest supports both original MOD2 plain LED mode and MELON RGB LED mode.
The boot LED selector is adapted from WGD Modular's FX LoPerformer hardware
selection menu.

Palimpsest boots in MELON WS2812B RGB LED mode by default. A different choice
made from the boot menu is saved and used on later boots.

To change LED hardware mode:

1. Hold the button while powering on or resetting the module.
2. Keep holding and turn **POT2**:
   - left half = original MOD2 Legacy PWM LED
   - right half = MELON WS2812 / NeoPixel RGB LED
3. Release the button to save.

On original MOD2, brightness follows tape level and status.

On MELON RGB LED mode:

| State | Color |
|---|---|
| Normal tape activity | Amber |
| Freeze active | Blue |
| Input clipping | Orange/red |
| Clear armed | Red blink |
| Character selection | Tape amber, Dub green, Smear violet, Unstable pink |

---

## Board Setup

| Jumper | Setting | Reason |
|---|---|---|
| **JP1** | 10n+22n recommended | Darker output filtering, closer to tape echo character |
| **JP2** | **OPEN** | Keep output AC-coupled for audio |

Do not short JP2 for Palimpsest unless you deliberately want DC-coupled audio
output. For normal use, leave JP2 open.

---

## Troubleshooting

**No input appears in the delay:** centre `POT3`, lower feedback, and check that
the audio source is patched to `CV`, not `IN1` or `IN2`.

**Feedback at maximum hides new input:** this should be corrected by
input-priority feedback ducking.

**The input is too distorted:** attenuate the source before `CV` or move `POT3`
closer to centre.

**The output is very dark:** use JP1 10n for a brighter output filter, or keep
10n+22n for the intended darker tape sound.

## Technical Notes

The CV input is DC-coupled into the MOD2 analog path and rolls off around 7.2 kHz
through the `R16 1k + C12 22n` filter. Palimpsest samples that input at six
times the tape rate, box-filters it, and writes a 16-bit tape at 18.3 kHz.

The audio ISR and its lookup tables are kept RAM-resident. The active tape lives
in SRAM and is cleared at boot.

---

## License

CC0 1.0 Universal - public domain.
