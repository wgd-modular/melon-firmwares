# Clap

An 808-style hand clap. Every hit is two short noise bursts followed by a
noise tail, all shaped by a band-pass filter.

## Controls

| Control | Function |
|---|---|
| **POT1** | Filter width, wide to narrow |
| **POT2** | Length of the tail, 20 to 200 ms |
| **POT3** | Filter frequency, 50 Hz to 8 kHz (shared with CV) |
| **IN1** | Trigger |
| **IN2** | Accent, the clap plays at half volume while it is high |
| **OUT** | Audio output |
| **Button** | Plays a clap |

## LED

The LED lights up with every clap and fades out with it. Its colour follows
the filter frequency, from warm orange when it is low to cool cyan when it is
high.

## Author

Hagiwo, from the original [MOD2](https://note.com/solder_state/n/nce8f7defcf98) firmwares.
