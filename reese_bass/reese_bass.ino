/*
  HAGIWO MOD2 / WGD MODULAR Melone - REESE BASS
  ========================
  Reese Bass for Seeed XIAO RP2350 (HAGIWO MOD2 / WGD MODULAR Melone Eurorack module):
  3 detuned sawtooth oscillators with a slow LFO on the detune
  amount for the classic "alive" chorus character.

  Pin assign (MOD2 standard):
  POT1  A0   Two-pole low-pass filter cutoff
  POT2  A1   Detune amount
  POT3  A2   Pitch (knob + 1V/Oct CV combined, Braids-style single pot)
  IN1   D7   Gate in (attack; 0.1s hold + 1s exponential release)
  IN2   D0   not used
  OUT   D1   Audio out (PWMAudio library, 16-bit, DMA)
  BUTTON D6  Toggles detune/LFO preset (narrow/wide)
  LED   D5   WS2812B, shows active preset + envelope brightness

  Pitch/V-Oct scaling, the single-pot pitch approach, and pot
  reading (averaging + lock/hysteresis) are adapted from the
  melon-firmwares Braids clone and VCO reference for MOD2.
  TUNE_CAL trims 1V/Oct tracking against a tuner if needed.

  Audio uses the arduino-pico PWMAudio library (16-bit, DMA-paced),
  which keeps the noise floor low.

  Board: "Raspberry Pi Pico/RP2040/RP2350" (arduino-pico) - select
  "Seeed XIAO RP2350". Requires Adafruit NeoPixel; PWMAudio ships
  with arduino-pico.

  CC0 1.0 - Public Domain, in the style of the original HAGIWO
  MOD2 sketches.
*/

#include <Arduino.h>
#include <PWMAudio.h>
#include <math.h>
#include <Adafruit_NeoPixel.h>

/* --------------------------------------------------
   System configuration
-------------------------------------------------- */

// ---- SAFE TO CHANGE: musical/tuning parameters, defaults shown ----
const float sampleRate           = 48000.0f; // audio sample rate (Hz)
const float OUTPUT_GAIN          = 0.9f;     // overall output level / headroom
const float NOISE_GATE_THRESHOLD = 0.01f;    // envLevel cutoff -> hard silence (higher = closes earlier)
const float TUNE_CAL             = 0.992f;   // 1V/Oct calibration trim
const float REF_FREQ             = 880.0f;   // pitch at the low end of the pot/CV range
const float PITCH_OCTAVE_OFFSET  = 1.0f;     // shifts the whole CV/pitch range up (octaves)
const float FILTER_MIN_HZ        = 80.0f;    // filter cutoff range, POT1 (A0)
const float FILTER_MAX_HZ        = 8000.0f;
const float RESONANCE            = 0.3f;     // filter feedback, 0 = none (>~0.9-1.0 can self-oscillate)
const float FILTER_TRACKING      = 1.0f / 3.0f; // pitch tracking amount on the cutoff
const float ATTACK_TIME_S        = 0.01f;    // default envelope attack (s)
const float HOLD_TIME_S          = 0.1f;     // hold after gate low, before release starts (s)
const float RELEASE_TIME_S       = 1.0f;     // default exponential release tail (s)
#define NUM_OSC 3                            // oscillator count - 2 or 3 (see updateIncrements())

// ---- DO NOT CHANGE: derived from the above, or tied to setup() ----
// Runtime envelope controls. Defaults are reset on every power-up; nothing
// is written to flash. The pots use logarithmic time ranges for useful control.
const float ATTACK_MIN_S  = 0.002f; // 2 ms
const float ATTACK_MAX_S  = 1.000f; // 1 s
const float RELEASE_MIN_S = 0.010f; // 10 ms
const float RELEASE_MAX_S = 5.000f; // 5 s
volatile float attackTimeS  = ATTACK_TIME_S;
volatile float releaseTimeS = RELEASE_TIME_S;
volatile float attackInc    = 1.0f / (ATTACK_TIME_S * sampleRate);
volatile float releaseCoeff = expf(-1.0f / (RELEASE_TIME_S * sampleRate));
const uint32_t HOLD_SAMPLES = (uint32_t)(HOLD_TIME_S * sampleRate);

/* --------------------------------------------------
   PWMAudio output (D1 / GPIO1), 16-bit samples via DMA
-------------------------------------------------- */
PWMAudio audio(1); // mono, GPIO1

/* --------------------------------------------------
   WS2812B RGB LED
-------------------------------------------------- */
#define LED_PIN 5
#define LED_COUNT 1
Adafruit_NeoPixel pixel(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

/* --------------------------------------------------
   Oscillator state (Q32 phase accumulator)
-------------------------------------------------- */
volatile uint32_t phase[NUM_OSC]    = {0};
volatile uint32_t phaseInc[NUM_OSC] = {0};

volatile bool gateOpen = false;
volatile float envLevel = 0.0f;      // current envelope level, 0..1
volatile uint32_t holdCounter = 0;   // samples remaining in the hold stage

/* LFO for detune movement */
volatile uint32_t lfoPhase = 0;
volatile uint32_t lfoInc   = 0;
#define LFO_DEPTH_SHIFT 6 // default 6 - higher = shallower detune modulation

/* Two-pole (12 dB/oct) low-pass filter: a cascade of identical
   one-pole stages, with slight feedback/resonance.
   Coefficient updated in loop(), states updated in the audio callback. */
#define FILTER_POLES 2 // default 2 (12 dB/oct); 4 = 24 dB/oct, etc.
volatile float filterState[FILTER_POLES] = {0};
volatile float filterAlpha = 1.0f;

/* Parameters calculated from the pots in loop() */
float baseFreq    = 55.0f;   // base frequency in Hz (knob + CV combined)
float detuneCents = 12.0f;   // max detune amount in cents
float lfoRateHz   = 0.2f;    // speed of the detune movement (set via preset)
volatile float lfoDepthScale = 0.0f; // 0..1, proportional to current detune amount

/* Preset: narrow (subtle) / wide (aggressive) detune range */
bool widePreset = false;

// Envelope edit mode and soft-takeover state. Defaults reset on power-up.
bool envelopeEditMode = false;
bool attackPickup = false;
bool releasePickup = false;
int previousA0 = 0;
int previousA1 = 0;

/* --------------------------------------------------
   Potentiometer reading with averaging + lock/hysteresis,
   adapted from the melon-firmwares Braids clone's potentiometer.h.
   Defaults: 8x averaging, unlock past 4 counts, register past 2
   counts once unlocked (10-bit ADC scale).
-------------------------------------------------- */
#define POT_AVERAGING 8
#define POT_LOCK_THRESHOLD 4
#define POT_MIN_COUNTS 2

struct PotState {
  float value = 0.0f;
  bool locked = true;
};
PotState potA0State, potA1State, potA2State; // filter cutoff, detune, pitch/CV

int readPotFiltered(uint8_t pin, PotState &state) {
  long sum = 0;
  for (int j = 0; j < POT_AVERAGING; j++) sum += analogRead(pin);
  int val = sum / POT_AVERAGING;

  if (state.locked) {
    if (fabsf(state.value - (float)val) > POT_LOCK_THRESHOLD) {
      state.locked = false;
      state.value = (float)val;
    } else {
      val = (int)state.value;
    }
  } else {
    if (fabsf(state.value - (float)val) > POT_MIN_COUNTS) {
      state.value = (float)val;
    } else {
      val = (int)state.value;
    }
  }
  return val;
}

/* --------------------------------------------------
   Helper function: calculate phase increments from the
   current parameters (called in loop(), NOT inside the
   audio callback - saves CPU time there)
-------------------------------------------------- */
void updateIncrements() {
  // Detune spread: symmetric down/up for 2 oscillators, or
  // down/center/up for 3 (bottom detuned down, middle stable,
  // top detuned up).
  float centsOffsets[NUM_OSC];
#if NUM_OSC == 2
  centsOffsets[0] = -detuneCents;
  centsOffsets[1] = detuneCents;
#else
  centsOffsets[0] = -detuneCents;
  centsOffsets[1] = 0.0f;
  centsOffsets[2] = detuneCents * 0.85f;
#endif

  for (int i = 0; i < NUM_OSC; i++) {
    float freq = baseFreq * powf(2.0f, centsOffsets[i] / 1200.0f);
    phaseInc[i] = (uint32_t)(freq * 4294967296.0 / sampleRate);
  }

  lfoInc = (uint32_t)(lfoRateHz * 4294967296.0 / sampleRate);
}

/* --------------------------------------------------
   Compute exactly one audio sample (16-bit signed).
   Called from the PWMAudio transmit callback below.
-------------------------------------------------- */
int16_t computeSample() {
  // Envelope: fast (linear) attack while the gate is high; once it
  // goes low, hold the current level for HOLD_TIME_S, then decay
  // exponentially towards 0 (natural-sounding release curve).
  if (gateOpen) {
    holdCounter = HOLD_SAMPLES; // keep the hold timer topped up while gate is high
    envLevel += attackInc;
    if (envLevel > 1.0f) envLevel = 1.0f;
  } else if (holdCounter > 0) {
    holdCounter--; // holding - envelope level stays where it is
  } else {
    envLevel *= releaseCoeff;
    if (envLevel < NOISE_GATE_THRESHOLD) {
      envLevel = 0.0f;
      // Envelope has fully closed - clear the filter's internal
      // states too, otherwise the resonance feedback loop can keep
      // a bit of energy circulating and "ring" or sweep on its own
      // after the note has died, independent of the envelope.
      for (int s = 0; s < FILTER_POLES; s++) filterState[s] = 0.0f;
    }
  }

  // Advance LFO (triangle wave derived from phase accumulator).
  // Its modulation depth is scaled by envLevel (fades out with the
  // note) AND by lfoDepthScale (fades out as the Detune pot is
  // turned down) - at Detune = 0 there is zero wobble at all.
  lfoPhase += lfoInc;
  int32_t lfoTri = (int32_t)(lfoPhase >> 16) - 32768; // -32768..32767
  int32_t lfoMod = (int32_t)((float)(lfoTri >> LFO_DEPTH_SHIFT) * envLevel * lfoDepthScale);

  int32_t mix = 0;
  for (int i = 0; i < NUM_OSC; i++) {
    uint32_t inc = phaseInc[i];
    if (i == 0) inc -= (uint32_t)max(0, -lfoMod);
    if (i == NUM_OSC - 1) inc += (uint32_t)max(0, lfoMod);

    phase[i] += inc;
    // Full 16-bit sawtooth (top 16 bits of the 32-bit phase
    // accumulator) - previously only 10 bits were used here,
    // which was an early source of quantization noise.
    int32_t saw = (int32_t)(phase[i] >> 16) - 32768; // -32768..32767
    mix += saw;
  }
  mix /= NUM_OSC;

  float sample = (float)mix * envLevel;

  // Two-pole low-pass filter (12 dB/oct) with a slight resonance:
  // feed a fraction of the last stage's output back into the first
  // stage's input. The feedback is scaled by the envelope so the
  // resonance fades out together with the note. Cutoff set by
  // POT1 (A0).
  float feedback = filterState[FILTER_POLES - 1] * RESONANCE * envLevel;
  float stageIn = sample - feedback;
  for (int s = 0; s < FILTER_POLES; s++) {
    filterState[s] += filterAlpha * (stageIn - filterState[s]);
    stageIn = filterState[s];
  }
  sample = stageIn;

  // Output gain (headroom) + hard clamp to the valid int16 range.
  sample *= OUTPUT_GAIN;
  sample = constrain(sample, -32767.0f, 32767.0f);

  return (int16_t)sample;
}

// PWMAudio transmit callback: called whenever the DMA buffer has
// room, refill it with freshly computed samples.
void onAudioTransmit() {
  while (audio.availableForWrite()) {
    audio.write(computeSample());
  }
}

// Map a knob position to an envelope time using an exponential/log scale.
float potToTime(int pot, float minTime, float maxTime) {
  float x = constrain((float)pot / 1023.0f, 0.0f, 1.0f);
  return minTime * expf(x * logf(maxTime / minTime));
}

// Find where the current time would sit on the knob, for soft takeover.
int timeToPot(float timeS, float minTime, float maxTime) {
  float t = constrain(timeS, minTime, maxTime);
  float x = logf(t / minTime) / logf(maxTime / minTime);
  return (int)(x * 1023.0f + 0.5f);
}

bool crossedOrReached(int previous, int current, int target) {
  if (abs(current - target) <= 10) return true;
  return ((previous < target && current > target) ||
          (previous > target && current < target));
}

/* --------------------------------------------------
   SETUP
-------------------------------------------------- */
void setup() {
  updateIncrements();

  // --- Audio output via PWMAudio (D1 / GPIO1) ---
  audio.setBuffers(4, 32); // recommended buffer size for this sample rate
  audio.onTransmit(onAudioTransmit);
  audio.begin((uint32_t)sampleRate);

  // --- Gate input ---
  pinMode(7, INPUT_PULLDOWN);

  // --- Button ---
  pinMode(6, INPUT_PULLUP);

  // --- WS2812B LED ---
  pixel.begin();
  pixel.setBrightness(60);
  pixel.show(); // off at startup
}

/* --------------------------------------------------
   LOOP
   - reads the pots
   - calculates new phase increments
   - reacts to gate and button
-------------------------------------------------- */
void loop() {
  // Read gate input.
  gateOpen = digitalRead(7);

  // Button behavior:
  // - hold for 3 seconds to enter/leave envelope edit mode
  // - short press toggles the narrow/wide preset in normal mode
  // - short press exits envelope edit mode
  static bool prevBtn = HIGH;
  static uint32_t pressStartMs = 0;
  static bool longPressHandled = false;
  bool currBtn = digitalRead(6);
  uint32_t now = millis();

  if (prevBtn == HIGH && currBtn == LOW) {
    pressStartMs = now;
    longPressHandled = false;
  }
  if (currBtn == LOW && !longPressHandled && (uint32_t)(now - pressStartMs) >= 3000UL) {
    envelopeEditMode = !envelopeEditMode;
    longPressHandled = true;
    if (envelopeEditMode) {
      attackPickup = false;
      releasePickup = false;
    }
  }
  if (prevBtn == LOW && currBtn == HIGH) {
    if (!longPressHandled) {
      if (envelopeEditMode) {
        envelopeEditMode = false;
      } else {
        widePreset = !widePreset;
      }
    }
  }
  prevBtn = currBtn;

  // Read the three controls once per loop. POT3/A2 remains the combined
  // pitch knob + external 1 V/oct CV input; its existing tracking is unchanged.
  int a0 = readPotFiltered(A0, potA0State);
  int a1 = readPotFiltered(A1, potA1State);
  int a2 = readPotFiltered(A2, potA2State);

  // Envelope edit mode: POT1 = attack, POT2 = release. Soft takeover avoids
  // jumping to the physical knob position when entering the mode.
  if (envelopeEditMode) {
    int attackTarget = timeToPot(attackTimeS, ATTACK_MIN_S, ATTACK_MAX_S);
    int releaseTarget = timeToPot(releaseTimeS, RELEASE_MIN_S, RELEASE_MAX_S);

    if (!attackPickup && crossedOrReached(previousA0, a0, attackTarget)) attackPickup = true;
    if (!releasePickup && crossedOrReached(previousA1, a1, releaseTarget)) releasePickup = true;

    if (attackPickup) {
      float newAttack = potToTime(a0, ATTACK_MIN_S, ATTACK_MAX_S);
      attackTimeS = newAttack;
      attackInc = 1.0f / (newAttack * sampleRate);
    }
    if (releasePickup) {
      float newRelease = potToTime(a1, RELEASE_MIN_S, RELEASE_MAX_S);
      releaseTimeS = newRelease;
      releaseCoeff = expf(-1.0f / (newRelease * sampleRate));
    }
  } else {
    // Normal mode: POT1 = filter cutoff; POT2 = detune amount.
    float cutoffHz = FILTER_MIN_HZ + (a0 / 1023.0f) * (FILTER_MAX_HZ - FILTER_MIN_HZ);
    const float cvOct = -(a2 / 1023.0f) * 8.3f * (33.0f / 55.0f) * TUNE_CAL;
    cutoffHz *= powf(2.0f, cvOct * FILTER_TRACKING);
    cutoffHz = constrain(cutoffHz, FILTER_MIN_HZ, FILTER_MAX_HZ);
    filterAlpha = 1.0f - expf(-2.0f * PI * cutoffHz / sampleRate);

    float maxDetune = widePreset ? 35.0f : 15.0f;
    detuneCents = (a1 / 1023.0f) * maxDetune;
    lfoDepthScale = detuneCents / maxDetune;
  }

  previousA0 = a0;
  previousA1 = a1;

  // Pitch calculation is intentionally unchanged from the supplied Reese
  // Bass source, preserving its existing combined POT3 + 1 V/oct behavior.
  const float cvOct = -(a2 / 1023.0f) * 8.3f * (33.0f / 55.0f) * TUNE_CAL;
  baseFreq = REF_FREQ * powf(2.0f, cvOct + PITCH_OCTAVE_OFFSET);
  baseFreq = constrain(baseFreq, 5.0f, 2000.0f);

  // LFO speed scales with pitch, with the preset setting its base rate.
  float presetBaseRate = widePreset ? 0.6f : 0.2f;
  lfoRateHz = presetBaseRate * (baseFreq / REF_FREQ);
  lfoRateHz = constrain(lfoRateHz, 0.05f, 8.0f);

  // LED: blue = narrow, orange = wide, purple = envelope edit mode.
  if (envelopeEditMode) {
    pixel.setPixelColor(0, pixel.Color(180, 0, 255));
  } else if (widePreset) {
    pixel.setPixelColor(0, pixel.Color(255, 90, 0));
  } else {
    pixel.setPixelColor(0, pixel.Color(0, 90, 255));
  }
  uint8_t brightness = (uint8_t)(15 + envLevel * 80.0f);
  pixel.setBrightness(brightness);
  pixel.show();

  updateIncrements();
  delay(5); // pots don't need to be polled constantly
}
