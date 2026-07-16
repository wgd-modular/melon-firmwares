/*
MOD2 Braids - Mutable Instruments Braids macro-oscillator (47 models)

  • VCO first: drones with no trigger; a trigger excites the physical/struck
    models, hard-syncs the oscillators and retriggers the AD envelope
  • Envelope config mode (long-press): pots edit attack / release, with soft
    takeover; release fully clockwise = drone (pure VCO)
  • Auto-save: model / envelope / drone persisted to EEPROM after 10s idle

  --Pin assign---
POT1     A0       Timbre
POT2     A1       Morph
POT3     A2       Pitch
TRIG     D5       Trigger / gate input
OUT      D7       PWM audio output @ 48 kHz
BUTTON   D4       Tap = next model, medium hold = prev, long hold = config, tap = exit
LED      GPIO5    WS2812B: solid = drone, breathing = envelope, rainbow = config,
                  soft-white flash on trigger; hue = model group

See README.md for the full control / model / LED reference.

  (c) 2025 blueprint@poetaster.de
  GPLv3 the libraries are MIT as the originals for STM from MI were also MIT.
*/

bool debug = true;

#include <Arduino.h>
#include "stdio.h"
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "potentiometer.h"

long midiTimer;

float pitch_offset = 36;
float max_voltage_of_adc = 3.3;
float voltage_division_ratio = 0.3333333333333;
float notes_per_octave = 12;
float volts_per_octave = 1;

float mapping_upper_limit = (max_voltage_of_adc / voltage_division_ratio) * notes_per_octave * volts_per_octave;

#include <hardware/pwm.h>
#include <PWMAudio.h>

#define SAMPLERATE  48000
#define PWMOUT      D7
#define BUTTON_PIN  D4
#define TRIG_PIN    D5

// ── WS2812B ───────────────────────────────────────────────────────────────────
#include <Adafruit_NeoPixel.h>
#define LED_PIN   5
#define LED_COUNT 1
Adafruit_NeoPixel led(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// Engine group colors (indexed by engine number 0–46)
// Groups:  0–9  Classic analog  → amber
//         10–14  Filtered/Z      → yellow
//         15–17  Vocal/Formant   → green
//         18–21  FM/Harmonic     → blue
//         22–30  Physical model  → pink
//         31–34  Wavetable       → purple
//         35–46  Noise/Digital   → red  (trigger flash = white)
uint32_t engineGroupColor(int engine) {
  if (engine <=  9) return 0xFF8C00;   // amber
  if (engine <= 14) return 0xFFE000;   // yellow
  if (engine <= 17) return 0x00FF80;   // green
  if (engine <= 21) return 0x0088FF;   // blue
  if (engine <= 30) return 0xFF00AA;   // pink
  if (engine <= 34) return 0xAA00FF;   // purple
  return 0xFF1800;                     // red  (noise/digital/unknown 35–46)
}

// Scale a packed RGB color to a 0..255 brightness.
uint32_t scaleColor(uint32_t c, uint8_t bright) {
  uint8_t r  = (uint8_t)(((uint32_t)((c >> 16) & 0xFF) * bright) / 255);
  uint8_t g  = (uint8_t)(((uint32_t)((c >>  8) & 0xFF) * bright) / 255);
  uint8_t bl = (uint8_t)(((uint32_t)( c        & 0xFF) * bright) / 255);
  return led.Color(r, g, bl);
}

bool     ledTrigOn    = false;
uint32_t ledTrigOffAt = 0;
bool     lastTrigPin  = false;   // previous TRIG_PIN state, for edge detection
// ──────────────────────────────────────────────────────────────────────────────

#include "utility.h"
#include <STMLIB.h>
#include <BRAIDS.h>
#include "braids.h"

#include <Bounce2.h>
Bounce2::Button button = Bounce2::Button();

#include <EEPROM.h>

PWMAudio DAC(PWMOUT);

const char* engineNames[47] = {
  "CSAW", "MORPH", "SAW_SQ", "FOLD", "SQ_SUB", "SAW_SUB", "SQ_SYNC",
  "SAW_3", "SQ_3", "SAW_COMB", "TOY", "ZLPF", "ZPKF", "ZBPF", "ZHPF",
  "VOSIM", "VOWEL", "VOW_FOF", "HARM", "FM", "FBFM", "WTFM",
  "PLUCK", "BOW", "BLOW", "FLUTE", "BELL", "DRUM", "KICK", "CYMBAL", "SNARE",
  "WTBL", "WMAP", "WLIN", "WTx4",
  "NOISE", "TWNQ", "CLKN", "CLOUD", "PRTC", "QPSK",
  "ENG41", "ENG42", "ENG43", "ENG44", "ENG45", "ENG46"
};

int  engineCount      = 0;
int  engineInc        = 0;

// Button: tap = next model, medium hold = previous model, long hold (~3 s) =
// enter alt-param edit mode, tap = exit. Neither entering nor exiting changes
// the model. While editing, the pots set the envelope: pot0 = attack,
// pot1 = release (fully clockwise = drone / pure VCO).
bool     envEditMode       = false;
uint32_t pressStart        = 0;    // when the current button press began
bool     longHandled       = false;// this press already fired its long action
bool     ignoreNextRelease = false;// swallow the release of the entering hold
uint16_t potSnap0          = 0;    // pot0/1 positions captured on entering edit
uint16_t potSnap1          = 0;
bool     attackEngaged     = false;// pot moved enough to take over the envelope value
bool     releaseEngaged    = false;
uint32_t ledPulseTimer     = 0;    // throttles the LED animation
#define  BACK_HOLD         600     // ms hold (then release) = previous model
#define  LONG_HOLD        3000     // ms hold that enters edit mode
#define  ENV_EDIT_THRESHOLD 30     // pot movement (counts) to grab the envelope value
#define  DRONE_MARGIN       40     // pot1 within this of POT_MAX = drone

// ── Persistent settings (auto-saved 10 s after the last change) ──────────────
// Stores the things that aren't live pot reads: engine, envelope, drone.
#define SETTINGS_SIG 0xB2
struct Settings {
  uint8_t signature;
  uint8_t engine;        // 0..46
  uint8_t drone;         // 0/1
  uint8_t _pad;
  float   attackRate;
  float   releaseRate;
};
uint32_t lastSettingChange = 0;
bool     settingsDirty     = false;

void loadSettings() {
  Settings s;
  EEPROM.get(0, s);
  if (s.signature == SETTINGS_SIG && s.engine <= 46 &&
      s.attackRate  > 0.0f && s.attackRate  <= 1.0f &&
      s.releaseRate > 0.0f && s.releaseRate <= 1.0f) {
    engineCount      = s.engine;
    droneMode        = (s.drone != 0);
    env_attack_rate  = s.attackRate;
    env_release_rate = s.releaseRate;
  } else {
    // Factory default = original configuration (plays without a trigger).
    engineCount      = 22;         // PLUCK
    droneMode        = true;       // pure VCO
    env_attack_rate  = 0.01f;
    env_release_rate = 0.001f;
  }
  engine_in = engineCount;
}

void saveSettings() {
  Settings s;
  s.signature   = SETTINGS_SIG;
  s.engine      = (uint8_t)engineCount;
  s.drone       = droneMode ? 1 : 0;
  s._pad        = 0;
  s.attackRate  = env_attack_rate;
  s.releaseRate = env_release_rate;
  EEPROM.put(0, s);
  EEPROM.commit();
}

inline void markSettingsDirty() {
  settingsDirty     = true;
  lastSettingChange = millis();
}

#define TIMER_INTERRUPT_DEBUG     0
#define _TIMERINTERRUPT_LOGLEVEL_ 4

#include "RPi_Pico_TimerInterrupt.h"

#define TIMER0_INTERVAL_MS   20.833333333333
#define DEBOUNCING_INTERVAL_MS 2
#define LOCAL_DEBUG            0

volatile int counter = 0;

RPI_PICO_Timer ITimer0(0);

bool TimerHandler0(struct repeating_timer *t) {
  (void) t;
  bool sync = true;
  if (DAC.availableForWrite()) {
    for (size_t i = 0; i < BLOCK_SIZE; i++) {
      DAC.write(voices[0].pd.buffer[i], sync);
    }
    counter = 1;
  }
  return true;
}

void cb() {
  bool sync = true;
  if (DAC.availableForWrite() >= BLOCK_SIZE) {
    for (int i = 0; i < BLOCK_SIZE; i++) {
      DAC.write(voices[0].pd.buffer[i]);
    }
  }
}

void HandleNoteOn(byte channel, byte note, byte velocity) {
  pitch_in   = note << 7;
  trigger_in = velocity / 127.0;
}
void HandleNoteOff(byte channel, byte note, byte velocity) {
  trigger_in = 0.0f;
}

void setup() {
  if (debug) {
    Serial.begin(57600);
    Serial.println(F("=== MOD2 BRAIDS FIRMWARE ==="));
  }

  EEPROM.begin(256);
  loadSettings();    // restores engine / envelope / drone (or factory defaults)

  if (debug) {
    Serial.print(F("Loaded engine: "));
    Serial.print(engineCount);
    Serial.print(F(" - "));
    Serial.println(engineNames[engineCount]);
  }

  analogReadResolution(12);
  pinMode(23, OUTPUT);
  digitalWrite(23, HIGH);

  pinMode(TRIG_PIN,  INPUT_PULLDOWN);
  pinMode(AIN0, INPUT);
  pinMode(AIN1, INPUT);
  pinMode(AIN2, INPUT);
  pinMode(SCL,  INPUT_PULLDOWN);

  // ── WS2812B init (replaces pinMode(LED, OUTPUT)) ──────────────────────────
  led.begin();
  led.setBrightness(180);
  led.setPixelColor(0, engineGroupColor(engineCount));  // boot: PLUCK = pink
  led.show();
  // ──────────────────────────────────────────────────────────────────────────

  button.attach(BUTTON_PIN, INPUT_PULLUP);
  button.interval(5);
  button.setPressedState(LOW);

  if (ITimer0.attachInterruptInterval(TIMER0_INTERVAL_MS, TimerHandler0)) {
    if (debug) Serial.print(F("Starting ITimer0 OK, millis() = ")); Serial.println(millis());
  } else {
    if (debug) Serial.println(F("Can't set ITimer0. Select another freq. or timer"));
  }

  DAC.setBuffers(4, 32);
  DAC.setFrequency(SAMPLERATE);
  DAC.begin();

  initVoices();
  voices[0].pd.osc->set_shape(static_cast<braids::MacroOscillatorShape>(engine_in));

  if (debug) {
    Serial.print(F("Oscillator shape set to engine: "));
    Serial.println(engine_in);
  }

  readpot(0);
  readpot(1);
  readpot(2);

  int16_t timbre = map(potvalue[0], POT_MIN, POT_MAX, 0, 32767);
  timbre_in = timbre;
  int16_t morph = map(potvalue[1], POT_MIN, POT_MAX, 0, 32767);
  morph_in  = morph;

  midiTimer = millis();
}

void loop() {
  if (counter > 0) {
    updateBraidsAudio();
    counter = 0;
  }
}

// ── Second core: UI / control rate — LED handling here only ───────────────────

void setup1() {
  delay(200);
}

void loop1() {
  uint32_t now = millis();

  button.update();

  // ── Button gesture ─────────────────────────────────────────────────────────
  // Not editing: tap = next model, medium hold = previous model, hold >=
  // LONG_HOLD = enter edit mode. Editing: tap = exit. Entering/exiting never
  // changes the model.
  if (button.pressed()) {
    pressStart  = now;
    longHandled = false;
  }

  if (!envEditMode) {
    // Enter edit mode once the button has been held for LONG_HOLD.
    if (button.isPressed() && !longHandled && (now - pressStart) >= LONG_HOLD) {
      envEditMode       = true;
      longHandled       = true;
      ignoreNextRelease = true;              // don't let this hold's release exit
      // Soft takeover: keep the current envelope until a pot is actually moved.
      potSnap0 = potvalue[0];
      potSnap1 = potvalue[1];
      attackEngaged = releaseEngaged = false;
      if (debug) Serial.println(F("Envelope edit: ON"));
    }

    if (button.released() && !longHandled) {
      if ((now - pressStart) < BACK_HOLD) {
        engineCount++;                       // tap -> next model
        if (engineCount > 46) engineCount = 0;
      } else {
        engineCount--;                       // medium hold -> previous model
        if (engineCount < 0) engineCount = 46;
      }
      engine_in = engineCount;
      markSettingsDirty();
      if (debug) {
        Serial.print(F("Engine: "));
        Serial.print(engineCount);
        Serial.print(F(" - "));
        Serial.println(engineNames[engineCount]);
      }
    }
  } else if (button.released()) {
    if (ignoreNextRelease) {
      ignoreNextRelease = false;             // swallow the entering hold's release
    } else {
      // Exit edit mode. Lock timbre/morph so they don't jump to the pots'
      // current (envelope) positions until the knobs move again.
      envEditMode = false;
      potlock[0]  = 1;
      potlock[1]  = 1;
      markSettingsDirty();                   // envelope / drone finalized
      if (debug) Serial.println(F("Envelope edit: OFF"));
    }
  }

  // ── Pot mapping ────────────────────────────────────────────────────────────
  if (envEditMode) {
    // pot0 -> attack, pot1 -> release. Soft takeover: a pot only grabs its
    // value once it has moved, so entering edit mode changes nothing yet.
    if (!attackEngaged  && abs((int)potvalue[0] - (int)potSnap0) > ENV_EDIT_THRESHOLD) attackEngaged  = true;
    if (!releaseEngaged && abs((int)potvalue[1] - (int)potSnap1) > ENV_EDIT_THRESHOLD) releaseEngaged = true;

    if (attackEngaged) {
      float a = (float)(potvalue[0] - POT_MIN) / (float)(POT_MAX - POT_MIN);
      if (a < 0.0f) a = 0.0f; else if (a > 1.0f) a = 1.0f;
      // Exponential map so the change spreads evenly across the travel:
      // CCW = ~2 ms attack, CW = ~0.7 s.
      env_attack_rate = 0.30f * powf(0.001f / 0.30f, a);
    }
    if (releaseEngaged) {
      float r = (float)(potvalue[1] - POT_MIN) / (float)(POT_MAX - POT_MIN);
      if (r < 0.0f) r = 0.0f; else if (r > 1.0f) r = 1.0f;
      // Exponential map: CCW = ~2 ms decay, CW = ~3 s, evenly spread.
      env_release_rate = 0.33f * powf(0.0002f / 0.33f, r);
      // pot1 fully clockwise = drone: release fully open, pure VCO.
      droneMode = (potvalue[1] >= (POT_MAX - DRONE_MARGIN));
    }
  } else {
    // Normal control: pot0 -> timbre, pot1 -> morph.
    timbre_in = map(potvalue[0], POT_MIN, POT_MAX, 0, 32767);
    morph_in  = map(potvalue[1], POT_MIN, POT_MAX, 0, 32767);
  }

  // pot2 -> pitch (never repurposed).
  int16_t pitch       = map(potvalue[2], POT_MIN, POT_MAX, 3072, 8192);
  int16_t pitch_delta = abs(previous_pitch - pitch);
  if (pitch_delta > 10) {
    pitch_in       = pitch;
    previous_pitch = pitch;
  }

  bool trigPin = digitalRead(TRIG_PIN);
  if (trigPin) {
    trigger_in = 1.0f;
    if (!lastTrigPin) {        // rising edge only: one brief flash per trigger
      ledTrigOn    = true;
      ledTrigOffAt = now + 45;
    }
  } else {
    trigger_in = 0.0f;
  }
  lastTrigPin = trigPin;

  if ((now - pot_timer) > POT_SAMPLE_TIME) {
    readpot(0);
    readpot(1);
    readpot(2);

    pot_timer = now;
  }

  // ── LED (single owner) ────────────────────────────────────────────────────
  // Priority: editing alt params > trigger flash > drone (pure VCO) > solid.
  if ((now - ledPulseTimer) >= 25) {                 // ~40 fps
    ledPulseTimer = now;
    if (ledTrigOn && now >= ledTrigOffAt) ledTrigOn = false;

    // Smooth breathing (0..1), shared by envelope mode and edit mode. Slow so
    // the swell is gentle and clearly readable.
    float bphase = (now % 4800) / 4800.0f;
    float b = 0.5f - 0.5f * cosf(bphase * 6.28318f);
    uint8_t breath = 20 + (uint8_t)(b * 235.0f);

    uint32_t c = engineGroupColor(engineCount);
    uint32_t color;
    if (envEditMode) {
      // Editing alt params: a rotating rainbow. It carries the same drone-vs-
      // envelope cue as at rest -- steady when drone, breathing when enveloped
      // -- so it's obvious when you cross the drone boundary while editing.
      uint16_t hue = (uint16_t)((uint32_t)(now % 2000) * 65535UL / 2000UL); // ~2 s sweep
      uint32_t rainbow = led.gamma32(led.ColorHSV(hue));
      color = droneMode ? rainbow : scaleColor(rainbow, breath);
    } else if (ledTrigOn) {
      color = led.Color(120, 120, 120);               // softened trigger flash
    } else if (droneMode) {
      color = c;                                        // drone / pure VCO: solid
    } else {
      color = scaleColor(c, breath);                    // envelope mode: breathing
    }
    led.setPixelColor(0, color);
    led.show();
  }
  // ──────────────────────────────────────────────────────────────────────────

  // Auto-save 10 s after the last settings change (engine / envelope / drone).
  if (settingsDirty && (now - lastSettingChange) > 10000) {
    saveSettings();
    settingsDirty = false;
    if (debug) Serial.println(F("Settings saved to EEPROM"));
  }
}
