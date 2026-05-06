/*
  MOD2 MOD303 - "ACIDWALK303" — 303-ish bass voice + generative sequencer
  Target: RP2040 / RP2350 (Earle Philhower Arduino-Pico core)

  POT1 A0: TURING / RANDOMNESS + LENGTH
           CCW  -> locked 8-step
           MID  -> total random (pitch+rhythm evolving)
           CW   -> locked 16-step
  POT2 A1: DECAY (amp decay + bite) ; Accent extends decay
  POT3 A2: TRANSPOSE (quantized semis, shared with CV)

  IN1 (D7 / GPIO7) : Clock in (rising edge advances)
  IN2 (D0 / GPIO0) : Accent hold (HIGH forces accent ON)
  OUT              : PWM audio out (PIN_AUDIO must match your working GPIO)
  BUTTON           : short=scale, double=waveform, long=regen pattern
  LED              : step blink, accent/slide longer blink
*/

#include <Arduino.h>
#define MY_COLOR 0xFF00FF
#include "melon_led.h"
#include "hardware/pwm.h"
#include "hardware/irq.h"
#include "pico/time.h"

static const uint8_t PIN_AUDIO = 1;
static const uint8_t PIN_CLK = 7;    // clock input GPIO
static const uint8_t PIN_ACC = 0;    // accent hold input GPIO
static const uint8_t PIN_BTN = 6;

// -------------------- AUDIO --------------------
static const uint32_t SAMPLE_RATE = 31250;
static const uint16_t PWM_WRAP    = 1023;

static uint slice_num;
static volatile uint32_t phase = 0;
static volatile uint32_t phaseInc = 0;

static inline float fast_clip(float x) {
  if (x > 1.0f) return 1.0f;
  if (x < -1.0f) return -1.0f;
  return x;
}

static inline void setOscFreq(float f) {
  float inc = (f * 4294967296.0f) / (float)SAMPLE_RATE;
  if (inc < 1.0f) inc = 1.0f;
  phaseInc = (uint32_t)inc;
}

static inline float midiToFreq(float midi) {
  return 440.0f * powf(2.0f, (midi - 69.0f) / 12.0f);
}

// Mild output smoothing (replaces resonant ladder filter)
struct OnePoleLP {
  float z = 0.0f;
  float a = 0.22f; // fixed gentle smoothing
  inline float process(float x) { z += a * (x - z); return z; }
};
static OnePoleLP outLP;

// Decay-only envelopes
struct DecayEnv {
  float v = 0.0f;
  float decayCoef = 0.9990f; // updated from POT2
  inline void trig(float accentBoost) { v = 1.0f + accentBoost; }
  inline float tick() {
    v *= decayCoef;
    if (v < 0.00001f) v = 0.0f;
    return v;
  }
};
static DecayEnv envAmp;
static DecayEnv envBite;

// Gate envelope to control note length (short non-accent, longer accent, slide ties)
struct GateEnv {
  float v = 0.0f;
  float relCoef = 0.86f;  // how fast it closes once gateOpen goes false
  inline void open() { v = 1.0f; }
  inline float tick(bool wantOpen) {
    if (wantOpen) v = 1.0f;
    else v *= relCoef;
    if (v < 0.00001f) v = 0.0f;
    return v;
  }
};
static GateEnv gateEnv;
static volatile bool gateOpen = false;

// -------------------- SEQUENCER --------------------
static const int STEPS_MAX = 16;

struct Step {
  int8_t note;     // 0..11
  int8_t oct;      // -1..+1
  bool gate;       // false = rest
  bool accent;
  bool slide;      // slide implies tie into next step
};

static Step pattern[STEPS_MAX];
static volatile int stepIndex = 0;
static int stepLen = 16;

static int rootSemis = 0;
static float currentFreq = 110.0f;
static float targetFreq  = 110.0f;
static float slideCoef   = 0.035f;

// slide/tie state: if previous step had slide, we tie into this step (no retrigger)
static bool prevSlide = false;

// timing
static volatile uint32_t lastClockUs = 0;
static volatile bool clockEdge = false;

static uint32_t stepDurMs   = 125;  // estimated from clock
static uint32_t stepStartMs = 0;
static uint32_t gateOffAtMs = 0;

static uint32_t internalIntervalMs = 125;
static uint32_t lastInternalMs = 0;

// Non-blocking swing: defer step trigger until swing delay elapses
static bool     swingPending  = false;
static uint32_t swingFireAtMs = 0;

// LED blink
static uint32_t ledOffAtMs = 0;
static inline void ledBlink(uint16_t ms) {
  melon_led_color32(MY_COLOR);
  ledOffAtMs = millis() + ms;
}

// scales
static const int8_t SCALE_MINOR[]  = {0,2,3,5,7,8,10};
static const int8_t SCALE_PHRYG[]  = {0,1,3,5,7,8,10};
static const int8_t SCALE_DORIAN[] = {0,2,3,5,7,9,10};
static const int8_t SCALE_MAJOR[]  = {0,2,4,5,7,9,11};

enum ScaleMode { MINOR=0, PHRYG=1, DORIAN=2, MAJOR=3 };
static ScaleMode scaleMode = MINOR;

// waveforms
enum WaveMode { SAW=0, SQUARE=1, TRI=2, PULSE=3, SUPERSAW=4, SINEISH=5 };
static WaveMode waveMode = SUPERSAW;

// -------------------- UTILS --------------------
static inline int quantizeSemisFromA2(int raw10) {
  return map(raw10, 0, 1023, -12, +12);
}

static int8_t pickScaleDegree() {
  const int8_t* sc = nullptr;
  int scLen = 7;
  switch (scaleMode) {
    case MINOR:  sc = SCALE_MINOR; break;
    case PHRYG:  sc = SCALE_PHRYG; break;
    case DORIAN: sc = SCALE_DORIAN; break;
    case MAJOR:  sc = SCALE_MAJOR; break;
  }
  return sc[random(scLen)];
}

static inline uint32_t swingDelayMsForStep(int stepIdx, float amount01) {
  // delay every odd step (16th shuffle)
  if ((stepIdx & 1) == 0) return 0;
  float maxFrac = 0.18f; // subtle
  return (uint32_t)(stepDurMs * (maxFrac * amount01));
}

// TURING mapping:
// x in [0..1]
// - length: left half -> 8, right half -> 16, center -> 16
// - prob: V curve, 1 at center, 0 at ends
static inline void turingParams(float &probOut, int &lenOut) {
  float x = analogRead(A0) / 1023.0f;
  float dist = fabsf(x - 0.5f) / 0.5f; // 0 center .. 1 ends
  float p = 1.0f - dist;               // 1 center .. 0 ends
  if (p < 0.0f) p = 0.0f;
  if (p > 1.0f) p = 1.0f;

  int len = (x < 0.5f) ? 8 : 16;
  if (p > 0.92f) len = 16; // noon = random + full length

  probOut = p;
  lenOut  = len;
}

// keep rhythm musical: limit long rest streaks and enforce step 0 gate
static void enforceRhythmConstraints() {
  pattern[0].gate = true;

  int restRun = 0;
  for (int i=0; i<stepLen; i++) {
    if (!pattern[i].gate) restRun++;
    else restRun = 0;

    if (restRun > 3) {
      pattern[i].gate = true;
      restRun = 0;
    }
    if (pattern[i].slide) pattern[i].gate = true; // slide implies gate
  }

  // classic emphasis: downbeats more likely active
  for (int i=0; i<stepLen; i+=4) pattern[i].gate = true;
}

static void regenBasePattern() {
  for (int i=0; i<STEPS_MAX; i++) {
    bool gate   = (random(100) < 78);
    bool accent = (random(100) < 28);
    bool slide  = (random(100) < 22);

    if ((i % 4) == 0) gate = (random(100) < 92);

    pattern[i].gate   = gate;
    pattern[i].accent = accent;
    pattern[i].slide  = slide;

    pattern[i].note = pickScaleDegree();
    int octPick = random(100);
    pattern[i].oct = (octPick < 18) ? +1 : (octPick < 35 ? -1 : 0);
  }

  // force a small acid slide run
  int start = random(0, STEPS_MAX-4);
  for (int k=0;k<4;k++) {
    pattern[start+k].gate = true;
    pattern[start+k].slide = (k != 0);
    if (k==2) pattern[start+k].accent = true;
  }

  stepLen = 16;
  enforceRhythmConstraints();
}

static void mutateStepAt(int idx, float prob) {
  // Separate mutation speeds (rhythm evolves slower than pitch)
  float pitchP  = prob;          // full influence
  float gateP   = prob * 0.60f;
  float accP    = prob * 0.40f;
  float slideP  = prob * 0.35f;
  float octP    = 0.08f + 0.22f * prob;  // 8%..30%

  // pitch
  if (random(1000) < (int)(pitchP * 1000.0f)) {
    pattern[idx].note = pickScaleDegree();
  }

  // rhythm / rests
  if (random(1000) < (int)(gateP * 1000.0f)) {
    // bias: keep groove (don’t rest too often on downbeats)
    bool downbeat = ((idx % 4) == 0);
    int r = random(100);
    pattern[idx].gate = downbeat ? (r < 92) : (r < 78);
  }

  // accent
  if (random(1000) < (int)(accP * 1000.0f)) {
    pattern[idx].accent = (random(100) < 32);
  }

  // slide
  if (random(1000) < (int)(slideP * 1000.0f)) {
    pattern[idx].slide = (random(100) < 26);
  }

  // octave jumps
  if (random(1000) < (int)(octP * 1000.0f)) {
    int r = random(100);
    if (r < 50) pattern[idx].oct = 0;
    else if (r < 85) pattern[idx].oct = +1; // bias upward for acid energy
    else pattern[idx].oct = -1;
  }

  if (pattern[idx].slide) pattern[idx].gate = true;
}

// -------------------- STEP TRIGGER --------------------
static void triggerStep(int idx) {
  Step &s = pattern[idx];

  // slide implies gate
  if (s.slide) s.gate = true;

  // transpose
  int a2 = analogRead(A2);
  rootSemis = quantizeSemisFromA2(a2);

  int midi = 45 + rootSemis + s.note + (12 * s.oct);

  // accent
  bool accHold = (digitalRead(PIN_ACC) == HIGH);
  bool isAcc   = s.accent || accHold;

  targetFreq = midiToFreq((float)midi);

  // decay from POT2 (A1) + accent extends decay
  int a1 = analogRead(A1);
  float d = a1 / 1023.0f;

  float baseDecay = 0.9955f + d * 0.00435f;  // ~tight..long
  float accExtra  = 0.0022f;                 // accent tail extension

  envAmp.decayCoef  = baseDecay + (isAcc ? accExtra : 0.0f);
  if (envAmp.decayCoef > 0.99995f) envAmp.decayCoef = 0.99995f;

  envBite.decayCoef = 0.9900f + d * 0.00880f; // bite usually falls faster

  // tie behavior from previous slide
  bool tieFromPrev = prevSlide;
  prevSlide = s.slide;

  // schedule gate length: shorter non-accent, longer accent, slide ties (full)
  float gateFrac = isAcc ? 0.78f : 0.50f;     // <- shortened non-accent
  if (s.slide) gateFrac = 1.05f;              // tie through
  uint32_t gateLen = (uint32_t)(stepDurMs * gateFrac);
  gateOffAtMs = stepStartMs + gateLen;

  // Trigger env ONLY if gated AND not tied from previous slide
  if (s.gate && !tieFromPrev) {
    float accBoost = isAcc ? 0.35f : 0.0f;
    envAmp.trig(accBoost);
    envBite.trig(isAcc ? 0.65f : 0.0f);
  }

  // If no slide on this step, snap to target (otherwise glide)
  if (!s.slide) currentFreq = targetFreq;

  // LED feedback
  if (s.gate) {
    if (isAcc) ledBlink(s.slide ? 95 : 55);
    else       ledBlink(s.slide ? 60 : 20);
  }
}

// -------------------- CLOCK ISR --------------------
static void onClockRise() {
  uint32_t now = micros();
  if (now - lastClockUs < 1500) return; // glitch filter
  lastClockUs = now;
  clockEdge = true;
}

// -------------------- PWM IRQ (AUDIO) --------------------
void pwm_wrap_isr() {
  pwm_clear_irq(slice_num);

  // portamento (slide)
  currentFreq += (targetFreq - currentFreq) * slideCoef;
  setOscFreq(currentFreq);

  // oscillator
  phase += phaseInc;
  uint32_t p = phase;

  float osc = 0.0f;
  switch (waveMode) {
    case SAW:
      osc = ((int32_t)(p >> 8) / 8388608.0f) - 1.0f;
      break;
    case SQUARE:
      osc = (p & 0x80000000u) ? 1.0f : -1.0f;
      break;
    case TRI: {
      float s = ((int32_t)(p >> 8) / 8388608.0f) - 1.0f;
      osc = 2.0f * fabsf(s) - 1.0f;
    } break;
    case PULSE:
      osc = ((p >> 30) == 0) ? 1.0f : -1.0f; // ~25%
      break;
    case SUPERSAW: {
      uint32_t p2 = p + (phaseInc * 3);
      uint32_t p3 = p - (phaseInc * 2);
      float s1 = ((int32_t)(p  >> 8) / 8388608.0f) - 1.0f;
      float s2 = ((int32_t)(p2 >> 8) / 8388608.0f) - 1.0f;
      float s3 = ((int32_t)(p3 >> 8) / 8388608.0f) - 1.0f;
      osc = (s1 + 0.6f*s2 + 0.6f*s3) * 0.55f;
    } break;
    case SINEISH: {
      float s = ((int32_t)(p >> 8) / 8388608.0f) - 1.0f;
      float t = 2.0f * fabsf(s) - 1.0f;   // triangle
      osc = t - (t*t*t)*0.25f;            // soften
    } break;
  }

  // envelopes
  float eA = envAmp.tick();
  float eB = envBite.tick();

  // “bite” increases harmonic intensity, especially on accent
  float bite = 0.25f + eB * 0.55f;
  float y = fast_clip(osc * bite);

  // smoothing
  y = outLP.process(y);

  // gate length envelope
  float g = gateEnv.tick(gateOpen);

  // amp
  float amp = eA * 0.65f * g;
  float out = fast_clip(y * amp);

  // PWM
  uint16_t pwmVal = (uint16_t)((out * 0.5f + 0.5f) * (float)PWM_WRAP);
  pwm_set_gpio_level(PIN_AUDIO, pwmVal);
}

// -------------------- BUTTON --------------------
static uint32_t btnDownMs = 0;
static uint32_t lastClickMs = 0;
static int clickCount = 0;

static void handleButton() {
  bool down = (digitalRead(PIN_BTN) == LOW); // active-low
  uint32_t now = millis();
  static bool wasDown = false;

  if (down && !wasDown) {
    btnDownMs = now;
    wasDown = true;
  } else if (!down && wasDown) {
    uint32_t held = now - btnDownMs;
    wasDown = false;

    if (held > 520) {
      regenBasePattern();
      stepIndex = 0;
      prevSlide = false;
      triggerStep(stepIndex);
      clickCount = 0;
      ledBlink(120);
      return;
    }

    if (now - lastClickMs < 350) clickCount++;
    else clickCount = 1;
    lastClickMs = now;

    if (clickCount == 2) {
      waveMode = (WaveMode)(((int)waveMode + 1) % 6);
      ledBlink(90);
      clickCount = 0;
    }
  }

  if (clickCount == 1 && (millis() - lastClickMs) > 380) {
    scaleMode = (ScaleMode)((((int)scaleMode) + 1) & 3);
    ledBlink(70);
    clickCount = 0;
  }
}

// -------------------- SETUP --------------------
void setup() {
  pinMode(PIN_CLK, INPUT_PULLDOWN);
  pinMode(PIN_ACC, INPUT_PULLDOWN);

  melon_led_begin(); melon_led_slot_init(MY_COLOR);
  melon_led_dim();

  pinMode(PIN_BTN, INPUT_PULLUP);

  analogReadResolution(10);

  randomSeed(analogRead(A0) ^ (analogRead(A1) << 10) ^ micros());
  regenBasePattern();

  // PWM init
  gpio_set_function(PIN_AUDIO, GPIO_FUNC_PWM);
  slice_num = pwm_gpio_to_slice_num(PIN_AUDIO);

  pwm_config cfg = pwm_get_default_config();

  float targetPwm = (float)SAMPLE_RATE * (float)(PWM_WRAP + 1);
  float div = (float)clock_get_hz(clk_sys) / targetPwm;
  if (div < 1.0f) div = 1.0f;

  pwm_config_set_clkdiv(&cfg, div);
  pwm_config_set_wrap(&cfg, PWM_WRAP);

  pwm_init(slice_num, &cfg, true);
  pwm_set_gpio_level(PIN_AUDIO, PWM_WRAP / 2);

  pwm_clear_irq(slice_num);
  pwm_set_irq_enabled(slice_num, true);
  irq_set_exclusive_handler(PWM_IRQ_WRAP, pwm_wrap_isr);
  irq_set_enabled(PWM_IRQ_WRAP, true);

  attachInterrupt(digitalPinToInterrupt(PIN_CLK), onClockRise, RISING);

  // start
  stepIndex = 0;
  prevSlide = false;
  stepStartMs = millis();
  triggerStep(stepIndex);

  gateOpen = pattern[stepIndex].gate;
  gateEnv.open();
}

// -------------------- LOOP --------------------
void loop() {
  handleButton();

  // LED off
  if (ledOffAtMs && (millis() > ledOffAtMs)) {
    melon_led_dim();
    ledOffAtMs = 0;
  }

  // close gate when time passed
  if (gateOpen && gateOffAtMs && (millis() >= gateOffAtMs)) {
    gateOpen = false; // gateEnv.tick(false) will decay it
  }

  // TURING params
  float prob; int len;
  turingParams(prob, len);
  stepLen = len;

  // Swing amount: subtle, increases with randomness
  float swingAmt = 0.15f + 0.35f * prob; // 0.15..0.50

  // ---------- External clock advance ----------
  if (clockEdge) {
    noInterrupts();
    clockEdge = false;
    interrupts();

    // estimate step duration from clock period
    static uint32_t prevClockUs = 0;
    uint32_t nowUs = micros();
    if (prevClockUs != 0 && (nowUs - prevClockUs) < 2000000UL) {
      uint32_t est = (nowUs - prevClockUs) / 1000UL;
      if (est < 20) est = 20;
      if (est > 500) est = 500;
      stepDurMs = est;
    }
    prevClockUs = nowUs;

    // advance step index within length
    stepIndex++;
    if (stepIndex >= stepLen) stepIndex = 0;

    // mutate this step under Turing probability
    mutateStepAt(stepIndex, prob);
    enforceRhythmConstraints();

    // non-blocking swing: defer trigger if swing delay > 0
    uint32_t dly = swingDelayMsForStep(stepIndex, swingAmt);
    if (dly) {
      swingPending  = true;
      swingFireAtMs = millis() + dly;
    } else {
      stepStartMs = millis();
      triggerStep(stepIndex);
      gateOpen = pattern[stepIndex].gate;
      gateEnv.open();
    }
  }

  // ---------- Fire deferred swing step ----------
  if (swingPending && millis() >= swingFireAtMs) {
    swingPending = false;
    stepStartMs  = millis();
    triggerStep(stepIndex);
    gateOpen = pattern[stepIndex].gate;
    gateEnv.open();
  }

  // ---------- Internal fallback if no external clock ----------
  if ((micros() - lastClockUs) > 1500000UL) {
    // internal tempo from A2 so you can demo without clock
    int a2 = analogRead(A2);
    internalIntervalMs = map(a2, 0, 1023, 60, 220);

    uint32_t nowMs = millis();
    if (nowMs - lastInternalMs >= internalIntervalMs) {
      lastInternalMs = nowMs;
      stepDurMs = internalIntervalMs;

      stepIndex++;
      if (stepIndex >= stepLen) stepIndex = 0;

      mutateStepAt(stepIndex, prob);
      enforceRhythmConstraints();

      uint32_t dly = swingDelayMsForStep(stepIndex, swingAmt);
      if (dly) {
        swingPending  = true;
        swingFireAtMs = millis() + dly;
      } else {
        stepStartMs = millis();
        triggerStep(stepIndex);
        gateOpen = pattern[stepIndex].gate;
        gateEnv.open();
      }
    }
  }
  melon_check_bootloader_hold();
}
