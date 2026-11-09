# Reese Bass

Three detuned saws through a low-pass filter, with a slow LFO moving the
detune for that restless Reese sound. A gate on IN1 opens it. When the gate
ends the sound holds for a moment and then fades out over about a second.
Attack and release can be changed in envelope edit mode.

## Controls

| Control | Function |
|---|---|
| **POT1** | Filter cutoff (normal mode) / Attack time (envelope edit mode) |
| **POT2** | Detune (normal mode) / Release time (envelope edit mode) |
| **POT3** | Pitch (shared with CV, 1V/oct) |
| **IN1** | Gate |
| **OUT** | Audio output |
| **Button** | Short press: switch between a narrow and a wide detune preset. Long press (3 seconds): enter or leave envelope edit mode. |

If tracking is slightly off on your unit, trim `TUNE_CAL` in
`reese_bass.ino` against a tuner.

## Envelope Edit Mode

Hold the button for 3 seconds to enter envelope edit mode.

- **POT1:** Attack time, 2 ms – 1 second
- **POT2:** Release time, 10 ms – 5 seconds

The controls use soft takeover, so moving a knob only changes its parameter once it reaches or crosses the current value. This avoids sudden jumps when entering edit mode.

Press the button briefly to leave envelope edit mode.

Envelope settings return to their defaults when the module is powered off.

## LED

| Colour | Meaning |
|---|---|
| $\color{#005AFF}{\text{██}}$ **Blue** | Narrow preset |
| $\color{#FF5A00}{\text{██}}$ **Orange** | Wide preset |
| $\color{#B400FF}{\text{██}}$ **Purple** | Envelope edit mode |

The LED gets brighter while a note plays and fades with it.
