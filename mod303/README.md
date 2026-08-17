# MOD303

A 303-style acid bass voice with a generative Turing-machine sequencer, ported
from the MOD2 firmware collection. Clock it from IN1 and it walks its own
evolving acid line; the more you open POT1 toward the centre, the more the
pattern mutates.

## Controls

| Control | Function |
|---|---|
| **POT1** | Randomness / length — CCW locked 8 steps, centre fully random, CW locked 16 steps |
| **POT2** | Decay — short pluck to long ringing; accents extend it |
| **POT3** | Transpose in semitones (shared with CV) |
| **IN1** | Clock in — a rising edge advances one step |
| **IN2** | Accent hold — high forces every step to accent |
| **OUT** | Audio output |
| **Button short** | Cycle scale (minor → phrygian → dorian → major) |
| **Button double** | Cycle waveform |
| **Button long** | Regenerate the pattern |

## LED

The WS2812B blinks once per played step:

| Color | Meaning |
|---|---|
| **White** | Normal step (also confirms a button action) |
| **Red** | Accented step |
| **Blue** | Slide / tie into the next step |

## Note on the melon adaptation

The original firmware free-ran on an internal tempo when nothing was patched
into IN1. For the melon version this was removed, so MOD303 now stays silent
until it receives an external clock on IN1.
