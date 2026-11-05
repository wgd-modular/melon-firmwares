# Hi Hat

A noise hi-hat with a band-pass filter. It plays blue or white noise, where
blue noise has more top end.

## Controls

| Control | Function |
|---|---|
| **POT1** | Decay time |
| **POT2** | Decay curve |
| **POT3** | Filter frequency, 100 Hz to 16 kHz (shared with CV) |
| **IN1** | Trigger |
| **IN2** | Accent, the hi-hat plays at half volume while it is high |
| **OUT** | Audio output |
| **Button short** | Plays the hi-hat |
| **Button long** | Switches between blue and white noise (hold for half a second) |

## LED

| Colour | Meaning |
|---|---|
| $\color{#0050FF}{\text{██}}$ **Blue** | Blue noise |
| $\color{#E0E0E0}{\text{██}}$ **White** | White noise |
| $\color{#00FF50}{\text{██}}$ **Green** | Flashes on every trigger |

## Author

Hagiwo, from the original [MOD2](https://note.com/solder_state/n/nce8f7defcf98) firmwares.
