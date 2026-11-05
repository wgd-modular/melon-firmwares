# Kick

A sine kick with a pitch sweep and soft clipping. The button switches the
knobs between two modes of three parameters each and saves all of them, so
they are still there after a power cycle.

## Controls

| Control | Mode 0 | Mode 1 |
|---|---|---|
| **POT1** | Pitch | Start of the pitch sweep |
| **POT2** | Soft clipping | End of the pitch sweep |
| **POT3** | Decay | Shape of the pitch sweep |

| Control | Function |
|---|---|
| **IN1** | Trigger |
| **IN2** | Accent, the kick plays at half volume while it is high |
| **OUT** | Audio output |
| **Button** | Switches the mode and saves the parameters |

The kick starts in mode 1. After switching modes a knob only takes over once
you turn it to the value it had in that mode, so nothing jumps.

## LED

| Colour | Meaning |
|---|---|
| $\color{#FF8C00}{\text{██}}$ **Amber** | Mode 0 |
| $\color{#00DCFF}{\text{██}}$ **Cyan** | Mode 1 |
| $\color{#E0E0E0}{\text{██}}$ **White** | Flashes on every trigger |

## Author

Hagiwo, from the original [MOD2](https://note.com/solder_state/n/nce8f7defcf98) firmwares.
