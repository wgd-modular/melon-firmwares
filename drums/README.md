# Generative Drum Synthesizer

A 15-kit, 3-voice drum machine for the RP2350. Features include real-time synthesis, waveguided resonators, sample playback, a probabilistic Grids sequencer, and a master bus with an SSL-style compressor and DJ filter.

## Navigation and Interface
The UI consists of 8 pages, navigated via push button. The RGB LED indicates the active page. Parameters are modified using the two potentiometers.

- **Short Press** (< 350ms): Next page.
- **Medium Press** (350ms - 1000ms): Previous page.
- **Long Press** (> 1000ms): Toggles Grids Sequencer Mode. When disabled, the module expects external triggers.

## Grids Sequencer
The module features a 3-track probabilistic sequencer inspired by Mutable Instruments Grids. In Grids Mode, it generates patterns based on a 2D "map" of drum styles.
- **Pot 1 (X)**: Navigates the pattern map (style/complexity).
- **Pot 2 (Y)**: Controls pattern density (event probability).
- **Trig 1 (IN1)**: Clock input.
- **Trig 2 (IN2)**: Triggers a 4-step quantized Fill (maximum density).

### UI Pages
1. Red: Kick (Pitch, Decay)
2. Orange: Snare (Pitch, Decay)
3. Yellow: Hihat (Pitch, Decay)
4. Cyan: Tape Delay (Division, FB/Mix)
5. Green: Reverb (Size, Mix)
6. Blue: Master DJ (Filter, Drive)
7. Kit Color: Kit Selection (Pot 1), Master Volume (Pot 2)
8. Ice White: Grids (X/Y Map)

## Drum Kits
On the Kit Configuration page, the first potentiometer selects one of 15 available kits (16 if a User Kit is compiled). LED color indicates the selected kit:

- Red: 808 (Synthesized)
- Orange: 909 (Synthesized)
- Yellow: Realistic (Synthesized)
- Lime: TR-808 (Sampled)
- Green: TR-909 (Sampled)
- Spring Green: TR-606 (Sampled)
- Cyan: LinnDrum (Sampled)
- Light Blue: Simmons SDS-V (Sampled)
- Blue: Rock Drumkit (Sampled)
- Purple: Grit / Industrial (Sampled)
- Violet: Modal Bells (Physical Modeling)
- Magenta: FM Percussion (2-Op FM Synthesis)
- Hot Pink: Analog Toms (Pinged Resonant Filters)
- White: VA Bassline Generator (Virtual Analog TB-303 style synth)
- Silver (Optional): User Custom Sample Kit
- Amber: External Audio FX Loop (Audio Input Processing)

## VA Bassline (Kit 13)
Kit 13 replaces drum synthesis with a dual PolyBLEP sawtooth synth and resonant SVF.
- **Manual Mode**: IN1 (Gate), IN2 (Accent), CV IN (1V/Oct).
- **Grids Mode**: Generative bass patterns based on X/Y selection.
- **Scale Selection (Grids Y)**: Pentatonic, Dorian, Phrygian, Octaves/Fifths.
- **Parameters**: Red (Pitch, Saw/Sq Shape), Orange (Cutoff, Filter Env), Yellow (Res, Decay Time).

## External Audio FX Loop (Kit 14)
Kit 14 bypasses internal synthesis and sequencing to act as a dedicated effects processor for an external audio signal.

- **Audio Input**: The external audio signal is fed into the **CV IN (A2)** jack.
- **Delay Sync**: The **Trig 1 (IN1)** jack acts as a clock/sync input for the Tape Delay.
- **Processing**: The signal runs through the Tape Delay, Room Reverb, Master DJ Filter, and SSL-style Master Bus Compressor. A 4-point moving average filter and noise gate eliminate ADC switching whine and hiss.
- **UI Changes**: The drum parameter pages (Red, Orange, Yellow) and Grids page (White) are hidden to streamline navigation.

## Custom User Sample Kit
Custom drum samples can be compiled into the firmware:
1. Create a folder at `drums/samples/user`.
2. Add your 22kHz wav files named exactly `user_kick.wav`, `user_snare.wav`, and `user_hihat.wav`.
3. Run the master script: `python samples/process_samples.py`. This regenerates the entire `drum_samples.h`.
4. Flash the firmware. The custom kit will be accessible as Kit 15, immediately before the External Audio FX kit.

## Sample Sources
The sample arrays are stored in PROGMEM and interpolated in real-time. The samples utilized in this project are sourced from the public domain:
- Vintage Drum Machines (TR-909, TR-808, TR-606, LinnDrum, Simmons SDS-V): Sourced from the "Drum Machines Collection" archive (https://archive.org/download/drum-machines-collection).
- Legacy Samples (Rock, Grit): Sourced from LMMS and public domain acoustic sample libraries.
- This firmware is designed for the WGD Melon hardware based on HAGIWO Mod 2, featuring a generative engine based on the open-source Grids sequencer by Émilie Gillet (Mutable Instruments).

## Build Instructions
1. Install the `arduino-cli`.
2. Install the RP2040/RP2350 board package (`rp2040:rp2040`).
3. Compile the firmware:
   `arduino-cli compile --fqbn rp2040:rp2040:seeed_xiao_rp2350 --export-binaries drums.ino`
4. Flash the resulting `.uf2` file in the `build/` directory to the microcontroller while in BOOTSEL mode.

Created by Vincent with the help of Gemini.
