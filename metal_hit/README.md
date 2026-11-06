# Metal Hit

A struck metal sound: six inharmonic partials, a short filtered noise click at
the start and a shimmer on top. It uses both cores of the RP2350 and renders
at 48 kHz.

## Controls

| Control | Function |
|---|---|
| **POT1** | Ring time, 40 ms to 1.2 s |
| **POT2** | Spread of the partials. Fully left they are harmonic, from there on inharmonic |
| **POT3** | Tune, from 40 Hz up (shared with CV, 1V/oct) |
| **IN1** | Trigger |
| **IN2** | Accent, a hit plays at half volume if it is high when the trigger arrives |
| **OUT** | Audio output |
| **Button** | Moves the tune up two octaves, press again to go back |

The knobs are read when a hit starts, so turning them changes the next hit,
not the one that is ringing. The button setting is saved and survives a
power cycle.

## LED

| Colour | Meaning |
|---|---|
| $\color{#00DC50}{\text{██}}$ **Green** | Normal tune |
| $\color{#A000FF}{\text{██}}$ **Violet** | Two octaves up |
| $\color{#E0E0E0}{\text{██}}$ **White** | Flashes on every trigger |

## Author

Hagiwo, from the original [MOD2](https://note.com/solder_state/n/nce8f7defcf98) firmwares.
