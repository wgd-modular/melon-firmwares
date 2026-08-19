/*
MELON Elements - Mutable Instruments Elements modal / physical-model voice

  A single Elements voice for the HAGIWO MOD2 / XIAO RP2350 melon module.
  CV sets the pitch (quantised to semitones, ~1V/oct); the button switches the
  exciter that drives the resonator, and the two pots shape the resonator tone.

  --Pin assign---
POT1     A0       Macro: GEOMETRY (sweeps once) + BRIGHTNESS (cycles twice)
POT2     A1       Macro: DAMPING (sweeps once) + POSITION (cycles twice)
CV       A2       Pitch, quantised to semitones (~1V/oct); knob = coarse tune
TRIG     D5       Strike / gate input
OUT      D7       PWM audio output @ 32 kHz
BUTTON   D4       Tap = next exciter (Bow/Blow/Strike); hold = shift; double = env/strength
LED      GPIO5    WS2812B: colour = exciter; white = shift; breathing = env/strength

  Button layers:
    Hold + POT1  -> exciter META (character)      Hold + POT2  -> SPACE (reverb)
    Double-tap, then POT1 -> envelope shape       POT2 -> strength   (breathing LED)

  Bow and Blow are continuous exciters, so the voice drones at the CV pitch on
  its own. Strike is a percussive exciter: it plucks on every new CV note and on
  a rising edge at TRIG.

  Elements DSP (c) Emilie Gillet, MIT. Arduino port (c) Mark Washeim / poetaster.
*/

#include <Arduino.h>
#include "pico/stdlib.h"
#include "potentiometer.h"

#include <PWMAudio.h>

#include <STMLIB.h>
#include <ELEMENTS.h>

#include <Adafruit_NeoPixel.h>
#include <Bounce2.h>

#define TIMER_INTERRUPT_DEBUG     0
#define _TIMERINTERRUPT_LOGLEVEL_ 4
#include "RPi_Pico_TimerInterrupt.h"

// ── Audio config ──────────────────────────────────────────────────────────────
#define SAMPLERATE   32000   // Elements' native rate. 48k cut aliasing a touch
                             // but the reverb couldn't keep real time (stutter).
#define PWMOUT       D7
#define BUTTON_PIN   D4
#define TRIG_PIN     D5

static const size_t MI_BLOCK = elements::kMaxBlockSize;   // 16

// Elements keeps its sample rate in statics; they must be defined by the sketch.
// setSr() recomputes them at boot; these are just the definitions.
float elements::Dsp::kSampleRate         = (float)SAMPLERATE;
float elements::Dsp::kSrFactor           = 32000.0f / (float)SAMPLERATE;
float elements::Dsp::kIntervalCorrection = 0.0f;

PWMAudio DAC(PWMOUT);
RPI_PICO_Timer ITimer0(0);
// 31.25 us period => 32 kHz (interval is microseconds; 20.833 would be 48 kHz).
#define TIMER0_INTERVAL_US 31.25

// ── WS2812B ───────────────────────────────────────────────────────────────────
#define LED_PIN   5
#define LED_COUNT 1
Adafruit_NeoPixel led(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

Bounce2::Button button = Bounce2::Button();

// ── Elements DSP state ────────────────────────────────────────────────────────
elements::Part*             g_part;
elements::Patch*            g_patch;
elements::PerformanceState  g_ps;
uint16_t*                   g_reverb = nullptr;

float   g_out[16];
float   g_aux[16];
float   g_silence[16];
int16_t g_obuff[16];

volatile int counter = 0;

// Exciters (the three "generators")
enum Exciter { EX_BOW = 0, EX_BLOW = 1, EX_STRIKE = 2, EX_COUNT = 3 };

// Control values written by core 1, read by the audio render on core 0.
volatile int   ctl_exciter    = EX_BOW;
volatile float ctl_note       = 60.0f;
volatile float ctl_geometry   = 0.30f;   // POT1 macro (once)
volatile float ctl_brightness = 0.50f;   // POT1 macro (twice)
volatile float ctl_damping    = 0.75f;   // POT2 macro (once)
volatile float ctl_position   = 0.30f;   // POT2 macro (twice)
volatile float ctl_meta       = 0.50f;   // hold + POT1: current exciter's character
volatile float ctl_space      = 0.20f;   // hold + POT2: reverb amount
volatile float ctl_envshape   = 0.50f;   // double-tap + POT1: exciter envelope shape
volatile float ctl_strength   = 0.70f;   // double-tap + POT2: exciter strength
volatile bool  ctl_gate       = true;
volatile float meter_level    = 0.0f;    // resonator level, for the LED

// ── Pitch / CV mapping ────────────────────────────────────────────────────────
// A2 reads pot+CV summed. readpot() already corrects the MOD2 hardware
// inversion, so a higher CV gives a larger count. ~34 counts per semitone gives
// roughly 1V/oct (12 semitones/volt) through the MOD2 input divider.
#define CV_COUNTS_PER_SEMITONE 34.13f
#define CV_NOTE_OFFSET         8.0f

uint32_t engineColor(int ex) {
  switch (ex) {
    case EX_BOW:  return 0xFF7A00;   // amber
    case EX_BLOW: return 0x00C8FF;   // cyan
    default:      return 0xFF00A0;   // magenta (strike)
  }
}

uint32_t scaleColor(uint32_t c, uint8_t bright) {
  uint8_t r  = (uint8_t)(((uint32_t)((c >> 16) & 0xFF) * bright) / 255);
  uint8_t g  = (uint8_t)(((uint32_t)((c >>  8) & 0xFF) * bright) / 255);
  uint8_t bl = (uint8_t)(((uint32_t)( c        & 0xFF) * bright) / 255);
  return led.Color(r, g, bl);
}

// ── DSP render (core 0) ───────────────────────────────────────────────────────
void updateElementsAudio() {
  elements::Patch* p = g_patch;

  float bow = 0.0f, blow = 0.0f, strike = 0.0f;
  switch (ctl_exciter) {
    case EX_BOW:    bow    = 0.90f; break;
    case EX_BLOW:   blow   = 0.75f; break;
    default:        strike = 0.90f; break;   // percussive, cleanest
  }

  // "meta" (hold + POT1) shapes whichever exciter is active: bow has no meta
  // param, so it steers bow timbre; blow/strike get their meta (flow / mallet).
  p->exciter_envelope_shape = ctl_envshape;
  p->exciter_bow_level      = bow;
  p->exciter_bow_timbre     = (ctl_exciter == EX_BOW) ? ctl_meta : 0.45f;
  p->exciter_blow_level     = blow;
  p->exciter_blow_meta      = (ctl_exciter == EX_BLOW) ? ctl_meta : 0.5f;
  p->exciter_blow_timbre    = 0.5f;
  p->exciter_strike_level   = strike;
  p->exciter_strike_meta    = (ctl_exciter == EX_STRIKE) ? ctl_meta : 0.45f;
  p->exciter_strike_timbre  = 0.5f;
  p->exciter_signature      = 0.0f;

  p->resonator_geometry     = ctl_geometry;
  p->resonator_brightness   = ctl_brightness;
  p->resonator_damping      = ctl_damping;
  p->resonator_position     = ctl_position;
  p->resonator_modulation_frequency = 0.0f;   // no internal FM: cleaner, stable tone
  p->resonator_modulation_offset    = 0.0f;

  p->reverb_diffusion = 0.625f;
  p->reverb_lp        = 0.7f;
  p->space            = ctl_space;

  g_ps.note     = ctl_note;
  g_ps.strength = ctl_strength;
  g_ps.gate     = ctl_gate;
  g_ps.modulation = 0.0f;

  g_part->set_resonator_model(elements::RESONATOR_MODEL_STRING);
  g_part->Process(g_ps, g_silence, g_silence, g_out, g_aux, MI_BLOCK);

  // tanh soft-clip instead of a hard clamp: it rounds peaks and reverb tails
  // smoothly rather than squaring them off, which is what made it sound harsh.
  for (size_t i = 0; i < MI_BLOCK; i++) {
    float x = tanhf(g_out[i]);
    g_obuff[i] = (int16_t)(x * 30000.0f);
  }
  meter_level = g_part->resonator_level();
}

void initElements() {
  elements::Dsp::setSr((float)SAMPLERATE);

  g_reverb = (uint16_t*)malloc(32768 * sizeof(uint16_t));
  memset(g_silence, 0, sizeof(g_silence));
  memset(g_obuff,   0, sizeof(g_obuff));

  g_part = new elements::Part;
  memset(g_part, 0, sizeof(*g_part));
  g_part->Init(g_reverb);

  uint32_t seed = 0x1fff7a10;
  g_part->Seed(&seed, 3);
  g_part->set_easter_egg(false);

  g_patch = g_part->mutable_patch();
  g_part->set_resonator_model(elements::RESONATOR_MODEL_STRING);

  g_ps.gate = true;
  g_ps.note = 60.0f;
  g_ps.strength = 0.8f;
  g_ps.modulation = 0.0f;

  updateElementsAudio();
}

// ── DAC feed (timer) ──────────────────────────────────────────────────────────
bool TimerHandler0(struct repeating_timer* t) {
  (void)t;
  if (DAC.availableForWrite() >= (int)MI_BLOCK) {
    for (size_t i = 0; i < MI_BLOCK; i++) DAC.write(g_obuff[i]);
    counter = 1;
  }
  return true;
}

// ── Core 0: audio ─────────────────────────────────────────────────────────────
void setup() {
  analogReadResolution(12);
  pinMode(23, OUTPUT);
  digitalWrite(23, HIGH);        // low-ripple SMPS mode, as the other firmwares

  pinMode(TRIG_PIN, INPUT_PULLDOWN);
  pinMode(AIN0, INPUT);
  pinMode(AIN1, INPUT);
  pinMode(AIN2, INPUT);

  initElements();

  DAC.setBuffers(4, 32);
  DAC.setFrequency(SAMPLERATE);
  DAC.begin();

  ITimer0.attachInterruptInterval(TIMER0_INTERVAL_US, TimerHandler0);
}

void loop() {
  if (counter > 0) {
    updateElementsAudio();
    counter = 0;
  }
}

// ── Core 1: UI / LED ──────────────────────────────────────────────────────────
void setup1() {
  delay(200);
  led.begin();
  led.setBrightness(180);
  led.setPixelColor(0, engineColor(ctl_exciter));
  led.show();

  button.attach(BUTTON_PIN, INPUT_PULLUP);
  button.interval(5);
  button.setPressedState(LOW);
}

#define SHIFT_MOVE  40     // pot movement (counts) needed to grab a new pot value
#define HOLD_MS     300    // press held at least this long = shift layer
#define DOUBLE_MS   300    // second tap within this = double tap

// Pot mode: what each of the two pots controls right now.
enum { MODE_NORMAL = 0, MODE_ENVEDIT = 1 };

static inline float pfrac(float x) { return x - floorf(x); }

// What POT `pot` controls given the current button state / mode.
// 0..2 for POT1, 3..5 for POT2 — used to detect when a pot's job changes.
static int potFunc(int pot, bool hold, int mode) {
  if (pot == 0) return hold ? 1 : (mode == MODE_ENVEDIT ? 2 : 0);
  else          return hold ? 4 : (mode == MODE_ENVEDIT ? 5 : 3);
}

// Apply a pot reading to whatever it currently drives. The two "macro" jobs
// fan one knob out to two params: the first sweeps once across the range, the
// second cycles twice, so a single knob still reaches every combination.
static void applyPot(int func, uint16_t raw) {
  float k = (float)raw / (float)POT_MAX;
  switch (func) {
    case 0: ctl_geometry = k;          ctl_brightness = pfrac(k * 2.0f); break; // POT1 normal
    case 1: ctl_meta     = k;          break;                                    // POT1 hold
    case 2: ctl_envshape = k;          break;                                    // POT1 dbl-tap
    case 3: ctl_damping  = k;          ctl_position   = pfrac(k * 2.0f); break;  // POT2 normal
    case 4: ctl_space    = k;          break;                                    // POT2 hold
    case 5: ctl_strength = k;          break;                                    // POT2 dbl-tap
  }
}

void loop1() {
  uint32_t now = millis();

  button.update();

  static int      mode        = MODE_NORMAL;
  static uint32_t pressStart  = 0;
  static bool     holdActive  = false;
  static bool     pendingTap  = false;
  static uint32_t lastTapTime = 0;

  // Per-pot soft takeover: a pot only starts driving its current job once it
  // has moved, so nothing jumps when the job changes (mode switch / hold).
  static int      curFunc[2]  = { 0, 3 };
  static uint16_t snap[2]     = { 0, 0 };
  static bool     engaged[2]  = { true, true };

  if ((now - pot_timer) > POT_SAMPLE_TIME) {
    readpot(0);
    readpot(1);
    readpot(2);
    pot_timer = now;
  }

  // ── Button gestures: hold = shift, tap = next exciter, double = env/strength.
  if (button.pressed()) {
    pressStart = now;
    holdActive = false;
  }
  if (button.isPressed() && !holdActive && (now - pressStart) >= HOLD_MS) {
    holdActive = true;
  }
  if (button.released()) {
    if (holdActive) {
      holdActive = false;                     // a hold: consumed, not a tap
    } else if (pendingTap && (now - lastTapTime) < DOUBLE_MS) {
      mode = (mode == MODE_NORMAL) ? MODE_ENVEDIT : MODE_NORMAL;   // double tap
      pendingTap = false;
    } else {
      pendingTap  = true;                     // first tap: wait for a possible second
      lastTapTime = now;
    }
  }
  if (pendingTap && (now - lastTapTime) >= DOUBLE_MS) {
    pendingTap = false;                        // confirmed single tap -> next exciter
    int e = ctl_exciter + 1;
    if (e >= EX_COUNT) e = 0;
    ctl_exciter = e;
  }

  // ── Pots -> current job, with soft takeover.
  for (int pot = 0; pot < 2; pot++) {
    int f = potFunc(pot, holdActive, mode);
    if (f != curFunc[pot]) {                   // job changed: re-arm takeover
      curFunc[pot] = f;
      snap[pot]    = potvalue[pot];
      engaged[pot] = false;
    }
    if (!engaged[pot] && abs((int)potvalue[pot] - (int)snap[pot]) > SHIFT_MOVE) engaged[pot] = true;
    if (engaged[pot]) applyPot(f, potvalue[pot]);
  }

  // CV -> quantised pitch. The 0.6 semitone hysteresis stops a note that sits
  // on a boundary from flipping back and forth as the ADC jitters.
  float noteF = CV_NOTE_OFFSET + (float)potvalue[2] / CV_COUNTS_PER_SEMITONE;
  static int note = 60;
  if (noteF - note > 0.6f || note - noteF > 0.6f) note = (int)lroundf(noteF);
  if (note < 8)   note = 8;
  if (note > 120) note = 120;

  static int lastNote = 60;
  bool noteChanged = (note != lastNote);
  lastNote = note;
  ctl_note = (float)note;

  // Gate: bow/blow drone; strike plucks on a trigger, or on a new note when no
  // trigger is patched. A recent trigger suppresses the note-change strike, so
  // a sequencer sending gate + CV together doesn't fire twice as the CV settles.
  static uint32_t strikeHoldUntil = 0;
  static uint32_t lastTrigMs      = 0;
  bool trig = digitalRead(TRIG_PIN);
  if (trig) lastTrigMs = now;
  bool trigActive = (now - lastTrigMs) < 500;
  if (ctl_exciter == EX_STRIKE) {
    if (trig || (noteChanged && !trigActive)) strikeHoldUntil = now + 5;
    ctl_gate = trig || (now < strikeHoldUntil);
  } else {
    ctl_gate = true;
  }

  // LED: exciter colour is always the identity. Brightness tracks the voice
  // level in normal use; white while holding the shift layer; and a slow
  // breathe of the exciter colour in the double-tap env/strength mode.
  static uint32_t ledTimer = 0;
  if ((now - ledTimer) >= 25) {          // ~40 fps
    ledTimer = now;
    float lvl = meter_level;
    if (lvl < 0.0f) lvl = 0.0f;
    if (lvl > 1.0f) lvl = 1.0f;
    if (holdActive) {
      led.setPixelColor(0, led.Color(120, 120, 120));           // shift: white
    } else if (mode == MODE_ENVEDIT) {
      float ph = (now % 1600) / 1600.0f;                        // ~1.6 s breathe
      float b  = 0.5f - 0.5f * cosf(ph * 6.28318f);
      led.setPixelColor(0, scaleColor(engineColor(ctl_exciter), 30 + (uint8_t)(b * 200.0f)));
    } else {
      uint8_t bright = 25 + (uint8_t)(lvl * 230.0f);
      led.setPixelColor(0, scaleColor(engineColor(ctl_exciter), bright));
    }
    led.show();
  }
}
