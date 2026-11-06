# Reese Bass

Three detuned saws through a low-pass filter, with a slow LFO moving the
detune for that restless Reese sound. A gate on IN1 opens it. When the gate
ends the sound holds for a moment and then fades out over about a second.

## Controls

| Control | Function |
|---|---|
| **POT1** | Filter cutoff |
| **POT2** | Detune |
| **POT3** | Pitch (shared with CV, 1V/oct) |
| **IN1** | Gate |
| **OUT** | Audio output |
| **Button** | Switches between a narrow and a wide detune preset |

If tracking is slightly off on your unit, trim `TUNE_CAL` in
`reese_bass.ino` against a tuner.

## LED

| Colour | Meaning |
|---|---|
| $\color{#005AFF}{\text{██}}$ **Blue** | Narrow preset |
| $\color{#FF5A00}{\text{██}}$ **Orange** | Wide preset |

The LED gets brighter while a note plays and fades with it.
