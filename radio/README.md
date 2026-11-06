# Radio

Plays one long recording, made for speech and vocals. POT3 and the CV pick
where in the recording a trigger starts, so you can scrub through a podcast or
an interview and fire off snippets.

## Controls

| Control | Function |
|---|---|
| **POT1** | Speed, 0.5× to 1.5× (the pitch moves with it) |
| **POT2** | Direction, forward on the left half, reverse on the right half |
| **POT3** | Start position, 0 to 100 % of the recording in 1 % steps (shared with CV) |
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
| $\color{#FF2A2A}{\text{██}}$ **Red, pulsing** | There is no recording on Melon yet |

## Your own recording

The [Radio tool](https://wgd-modular.github.io/melon-firmwares/radio/) puts
any audio file on Melon straight from the browser. Trim it, set its level and
fades, hear it on a virtual Melon the way the module will play it, then send it
over USB. Nothing gets uploaded. Sending needs Chrome, Edge or Opera on a
computer; other browsers can download the `.uf2` and copy it onto the RP2350
drive by hand.

Melon has room for 43 seconds at full quality, 10-bit at 36.6 kHz, which is all
its output can play. Lower qualities stretch that up to 2 minutes 40 seconds.

The radio firmware on the releases and on the web flasher comes with an example
recording, so after flashing it you need to send your own recording again. The
Radio tool remembers the last file you used.

## Building

The recording is not compiled into the firmware. It lives in its own part of
the flash, from `0x10020000` up to the EEPROM emulation at `0x101FE000`,
behind a 256-byte header with the magic `RDIO`, the format (packed 10-bit or
8-bit µ-law), the sample rate, the length and a name. `postbuild.py` adds
`sample.wav` to a compiled `radio.uf2`, which CI does for every build. To put
another recording in, pass it along:

```
python3 radio/postbuild.py radio.uf2 my-recording.wav
```

It takes 16-bit WAV files and keeps their sample rate. The Radio tool's source
is in `web/`, see `web/README.md`.

## Author

From modulove's [MOD2 workshop](https://github.com/modulove/MOD2) collection.
