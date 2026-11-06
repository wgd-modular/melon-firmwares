# Radio

Plays one long recording, made for speech and vocals. POT3 and the CV pick
where in the recording a trigger starts, so you can scrub through a podcast or
an interview and fire off snippets.

## Controls

| Control | Function |
|---|---|
| **POT1** | Speed, 0.5× to 1.5× (the pitch moves with it) |
| **POT2** | Direction, forward on the left half, reverse on the right half |
| **POT3** | Start position, 0 to 100 % of the recording (shared with CV) |
| **IN1** | Trigger |
| **IN2** | Loop, while it is high playback loops instead of stopping at the end |
| **OUT** | Audio output |
| **Button** | Trigger |

In reverse the recording plays from its end back to the start position.

## LED

| Colour | Meaning |
|---|---|
| $\color{#00FF50}{\text{██}}$ **Green** | Forward |
| $\color{#FF6400}{\text{██}}$ **Orange** | Reverse |
| $\color{#E0E0E0}{\text{██}}$ **White** | Flashes on every trigger |

## Your own sample

The recording is compiled into the firmware. `wav_to_sample.py` turns a WAV
file into `sample.h` (mono, 36.6 kHz, 16-bit), which leaves room for about 24
seconds. Then build the firmware as usual.

## Author

From modulove's [MOD2 workshop](https://github.com/modulove/MOD2) collection.
