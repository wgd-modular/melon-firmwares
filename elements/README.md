# Elements

Mutable Instruments Elements as a melon voice. An exciter (bow, blow or strike)
drives a tuned resonator, so it behaves like a bowed string, a blown pipe or a
struck bar depending on the mode. Patch a CV for pitch and it plays; the two
pots are macros over the resonator, and the button opens two more layers for the
rest of the controls.

## Controls

| Control | Function |
|---|---|
| **CV (A2)** | Pitch, quantised to semitones (~1V/oct); the knob is a coarse tune |
| **POT1** | Geometry (full sweep) + Brightness (cycles twice across the knob) |
| **POT2** | Damping (full sweep) + Position (cycles twice) |
| **TRIG** | Strike / gate input |
| **Button tap** | Next exciter: Bow → Blow → Strike |
| **Button double-tap** | Envelope / strength edit: POT1 = envelope shape, POT2 = strength |
| **Hold + POT1** | Exciter character (meta) |
| **Hold + POT2** | Space (reverb amount) |

Each pot fans out to two parameters: the first sweeps once across the knob, the
second cycles twice, so one knob still reaches every combination. Both pots use
soft takeover — when a layer changes, a knob only takes over once you move it, so
nothing jumps.

## Exciters

- **Bow** and **Blow** are continuous, so the voice drones at the CV pitch with
  nothing patched to TRIG.
- **Strike** is percussive: it plucks on a rising edge at TRIG, or on each new CV
  note when no trigger is patched.

## LED

| State | LED |
|---|---|
| Normal | Exciter colour — amber = Bow, cyan = Blow, magenta = Strike; brightness follows the voice |
| Holding the button | White |
| Envelope / strength edit | Exciter colour, slowly breathing |

## Credits

Elements DSP by Emilie Gillet (Mutable Instruments), MIT. Arduino / RP2350 port
by Mark Washeim (poetaster).
