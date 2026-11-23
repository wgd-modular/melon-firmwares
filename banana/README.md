# Banana

Hagiwo's chord oscillator, the firmware of WGD Modular's Banana module. Four
voices play one of eight chords in one of 16 inversions, and in half of the
inversions a fifth voice adds the root as a bass note. All voices play the
same wave, one of eight. It plays all the time, there is no envelope.

## Controls

| Control | Function |
|---|---|
| **POT1** | Inversion, 16 steps. Turning right moves the chord voices up an octave one after the other, past the middle the bass comes in and they come back down |
| **POT2** | Chord, 8 steps |
| **POT3** | Pitch (shared with CV, 1V/oct), C1 with the knob fully left |
| **OUT** | Audio output |
| **Button** | Next wave |

IN1 and IN2 are not used. If tracking is slightly off on your unit, trim
`TUNE_CAL` in `banana.ino` against a tuner.

## Chords

POT2 from left to right, with the notes of the four voices in semitones above
the root:

| Chord | Notes |
|---|---|
| Major | 0, 4, 7, 12 |
| Major 7th | 0, 4, 7, 11 |
| Major add 9 | 0, 4, 7, 14 |
| Sus2 | 0, 2, 7, 12 |
| Minor add 9 | 0, 3, 7, 14 |
| Minor 7th | 0, 3, 7, 10 |
| Minor | 0, 3, 7, 12 |
| Root only | 0, 0, 0, 0 |

With Root only the inversions stack the root in octaves.

## Waves

The button steps through the waves and the LED shows which one plays. After
power on it starts on the saw.

| Colour | Wave |
|---|---|
| $\color{#FF8000}{\text{██}}$ **Amber** | Saw |
| $\color{#FF2000}{\text{██}}$ **Red** | Square |
| $\color{#00FF80}{\text{██}}$ **Mint** | Triangle with rounded corners |
| $\color{#0080FF}{\text{██}}$ **Blue** | Sine |
| $\color{#CC00FF}{\text{██}}$ **Purple** | Chebyshev, a saw bent by the third Chebyshev polynomial |
| $\color{#FF00AA}{\text{██}}$ **Pink** | Half sine that wraps around at the top of its range |
| $\color{#FFE000}{\text{██}}$ **Yellow** | Sigmoid, a saw with soft corners |
| $\color{#E0E0E0}{\text{██}}$ **White** | Ramp, a saw in 256 steps, which sounds close to the saw |

## Coming from Banana

The Banana module has CV inputs for the chord and the inversion, Melon only
has the pitch CV. Its frequency knob goes from 0 to 255 Hz, here POT3 covers
about five octaves from C1. To change the wave Banana has you turn the inversion
knob fully right and pick it with the chord knob, here the button steps
through the waves.

Banana rounds each voice down to whole hertz, which puts chords around C1 up
to about half a semitone out of tune and lower ones further still. With a
high CV, from about 2.4 V in the middle inversions where the voices sit
highest and from about 3.8 V in the outer ones, it reads past the end of its
pitch table and plays wrong notes. It also plays each voice with only eight
levels at 16 kHz, which sounds gritty. Here every voice is in tune across the
whole range and plays at 48 kHz in full resolution. Voices that land on the
same note, like the bass and the root in the last inversion, play as one note
at the loudness they have on average, instead of adding up or cancelling out
depending on how their phases line up. The waves are smoothed where they
jump, and voices above 5 kHz start to blend into a sine, so high notes do not
fold back as off-key tones. Only the half sine, the brightest of the waves,
still folds back a little on high chords.

## Author

Hagiwo, from his [chord VCO](https://note.com/solder_state/n/n681d2e07e324),
which WGD Modular's Banana module runs.
