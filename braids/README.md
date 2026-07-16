# Braids (MOD2)

A port of the Mutable Instruments **Braids** macro-oscillator to the melon
MOD2 (RP2350). It renders 47 synthesis models through three pots, a button and
a trigger input, with WS2812B feedback.

Like hardware Braids the voice is a **VCO first**: with nothing patched to the
trigger it just drones. A trigger excites the physical/struck models, hard-syncs
the oscillators, and retriggers the optional AD amplitude envelope.

## Controls

| Control | Function |
|---------|----------|
| **POT1** (A0) | Timbre |
| **POT2** (A1) | Morph |
| **POT3** (A2) | Pitch |
| **TRIG** (D5) | Trigger / gate input |
| **OUT** (D7)  | PWM audio out, 48 kHz |
| **BUTTON** (D4) | Model select + envelope config (see below) |
| **LED** (GPIO5) | WS2812B status (see below) |

### Button

| Gesture | Action |
|---------|--------|
| Tap (< 0.6 s) | Next model |
| Medium hold (0.6–3 s) | Previous model |
| Long hold (≥ 3 s) | Enter **envelope config** mode |
| Tap while in config | Exit config mode |

Selecting a model never happens on entering/exiting config mode.

### Envelope config mode

While in config mode the two upper pots edit the amplitude envelope instead of
timbre/morph (soft takeover — a pot only grabs its value once you move it, so
entering the mode changes nothing until you turn a knob):

- **POT1** → attack (CCW ≈ 2 ms → CW ≈ 0.7 s, exponentially spread)
- **POT2** → release / decay (CCW ≈ 2 ms → CW ≈ 3 s, exponentially spread)
- **POT2 fully clockwise** → **drone**: release fully open, VCA held open, pure VCO

So turning POT2 down from fully-clockwise dials the AD envelope in; a trigger
then shapes the amplitude (attack, then decay). Turned all the way up, the voice
drones and a trigger only re-syncs / excites it.

Timbre/morph are locked on exit so they don't jump to the pots' config positions.

## Models

Grouped by LED colour (`engineGroupColor`):

| Models | Group | LED |
|--------|-------|-----|
| 0–9   CSAW, MORPH, SAW_SQ, FOLD, SQ_SUB, SAW_SUB, SQ_SYNC, SAW_3, SQ_3, SAW_COMB | Classic analog | amber |
| 10–14 TOY, ZLPF, ZPKF, ZBPF, ZHPF | Filtered / Z | yellow |
| 15–17 VOSIM, VOWEL, VOW_FOF | Vocal / formant | green |
| 18–21 HARM, FM, FBFM, WTFM | FM / harmonic | blue |
| 22–30 PLUCK, BOW, BLOW, FLUTE, BELL, DRUM, KICK, CYMBAL, SNARE | Physical / struck | pink |
| 31–34 WTBL, WMAP, WLIN, WTx4 | Wavetable | purple |
| 35–46 NOISE, TWNQ, CLKN, CLOUD, PRTC, QPSK, … | Noise / digital | red |

The physical/struck models (pink) decay after excitation, so they stay silent
until triggered — the rest drone. Default model on a fresh flash is **PLUCK**.

## LED legend

| State | Look |
|-------|------|
| Drone (pure VCO) | solid engine colour |
| Envelope mode | slow smooth breathing of the engine colour |
| Config + drone | steady rotating rainbow |
| Config + envelope | breathing rotating rainbow |
| Trigger | brief soft-white flash (once per rising edge) |

Steady vs breathing carries the drone-vs-envelope cue everywhere, including under
the config-mode rainbow.

## Settings persistence

Model, attack, release and drone are auto-saved to EEPROM **10 s after the last
change** and restored on boot. A signature byte guards fresh flash and falls back
to the factory default (PLUCK, drone on, attack 0.01, release 0.001).

## Credits

Braids and stmlib © Mutable Instruments (MIT). MOD2 port © 2025
blueprint@poetaster.de, GPLv3.
