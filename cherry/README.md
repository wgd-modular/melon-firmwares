# Cherry

Hagiwo's additive oscillator, the firmware of WGD Modular's Cherry module. It
adds up a fundamental and seven harmonics, all sine waves. Instead of a level
for each one, two knobs step through Hagiwo's tables of 256 settings: one sets
how loud the partials are, the other which harmonics they play. It plays all
the time, there is no envelope.

## Controls

| Control | Function |
|---|---|
| **POT1** | Harmonic gain, how loud the fundamental and each harmonic are |
| **POT2** | Harmonic spectrum, which harmonics play, up to the 16th |
| **POT3** | Pitch (shared with CV, 1V/oct), C1 with the knob fully left |
| **OUT** | Audio output |
| **Button** | Octave up, steps through 0, +1, +2 and +3 octaves |

IN1 and IN2 are not used. If tracking is slightly off on your unit, trim
`TUNE_CAL` in `cherry.ino` against a tuner.

## Coming from Cherry

The Cherry module has a CV input for the gain and one for the spectrum, Melon
only has the pitch CV. Cherry's pitch knob goes from 0 to 127 Hz and its CV
adds on top. Here POT3 and the CV share one range of about five octaves from
C1, so with POT3 at noon the CV has about two and a half octaves left above
it. The button shifts the whole range up by up to three octaves.

WGD's Cherry plays six of the eight partials, its code left out the last two
because of pops. Here all eight play as in Hagiwo's original, so it sounds
brighter than Cherry. The level never clips. The tables almost always
put two or more partials on the same harmonic, and on Cherry how loud that
harmonic gets depends on how they happen to line up after the spectrum knob
has moved. Here they play as one at the level they have on average, so a
setting always sounds the same however you got there, and turning the
spectrum knob does not change the volume. Cherry plays at 16 kHz, where
partials above 8 kHz fold back as off-key tones. Here they play at 48 kHz and fade
out between 16 and 20 kHz instead.

## LED

The LED shows the octave:

| Colour | Octave |
|---|---|
| $\color{#FF1020}{\text{██}}$ **Red** | 0 |
| $\color{#FF7000}{\text{██}}$ **Orange** | +1 |
| $\color{#30FF30}{\text{██}}$ **Green** | +2 |
| $\color{#2060FF}{\text{██}}$ **Blue** | +3 |

## Author

Hagiwo, from his [additive VCO](https://note.com/solder_state/n/n30b3a8737b1e),
which WGD Modular's Cherry module runs.
