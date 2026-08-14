/*
PALIMPSEST v0.3.3  —  HAGIWO MOD2 (Seeed XIAO RP2350)

A tape echo in which the analog front end IS the tape.

The CV jack is used as an AUDIO INPUT. This is not a hack: on the MOD2 the CV
path has no series capacitor and no filter before its buffer —

    J2(CV) - R3 10k - R4 100k -+- U1B (follower) - R10 10k -+
                            R6 220k                         |
                               |                            +- U2A (inv. sum, G=-1)
                              GND                           |      | R15 10k
    POT3 - R5 10k - U1A (follower) ----- R9 10k ------------+      |
                                                            |      +- R16 1k -+- A2
    -12V - R11 33k - R14 1k --------------------------------+     C12 22n ----+  D7/D8 clamp

  - divider R3+R4 / R6           = 0.667, DC coupled, ~200 kHz bandwidth
  - R16 + C12                    = 7.2 kHz single-pole  <- the only real limit
  - offset from -12V via R11+R14 = 10k * 12/34k = +3.53 V
  - both POT3 and CV land on A2 with gain -1:

        V(A2) = 3.53 - V(POT3) - 0.667 * V(CV)

A Roland Space Echo rolls its repeats off around 5-8 kHz. This input rolls off
at 7.2 kHz. The Schottky clamps (D7/D8, limited to ~1.4 mA by R16) soft-clip
like tape saturation. The front end is not a compromise to work around — it is
the frequency response and the distortion we are trying to emulate, already in
hardware. So we lean on it.

Consequences of V(A2) = 3.53 - V(POT3) - 0.667*V(CV), all of which we exploit:

  - Audio is INVERTED at the ADC          -> we negate it back.
  - POT3 sets the DC bias of the input    -> it is the DRIVE control. At ~57%
    of travel the signal is centered (max headroom); away from that the signal
    is pushed against one clamp and clips asymmetrically. That is an ANALOG
    distortion character that cannot be reproduced in software.
  - Audio is AC, so the DC average of A2 recovers POT3's position:
        V(POT3) = 3.53 - mean(V(A2))
    The slow DC tracker we need anyway to extract the audio therefore hands us
    POT3 for free. v0.1 does not need it as a parameter, but it is there.
  - Max input before clamping is about +/-2.5 V (5 Vpp). Eurorack at +/-5 V
    will overdrive by ~6 dB. That is a feature; attenuate if you don't want it.
    The LED double-blinks when you are into the clamps.

STORAGE — why 16-bit linear at 18.3 kHz for this test

The input is band-limited to 7.2 kHz by hardware, so storing at 36.6 kHz pays
double for silence. Storing at 18,310.5 Hz (exactly FS_OUT/2) gives 9.15 kHz of
bandwidth — more than a Space Echo's repeats have — and halves the footprint.
v0.3 uses 16-bit linear at 18.3 kHz. This halves the maximum RAM tape time
compared with the original 8-bit version, but it avoids crushing weak audio
coming from the MOD2 CV input path. Earlier 8-bit builds were too dependent on
hot input level: weak signals only used a handful of tape levels and sounded
distorted even when the gain staging was reduced.

  - random access is free      -> multiple playback heads are just pointers.
                                  In ADPCM each head needs its own predictor
                                  state and block buffer, and every head move
                                  is a re-seek plus a block re-decode.
  - degradation is HISS, not gravel. ADPCM fails by slope overload: it destroys
    transients while sparing steady tones. 16-bit linear keeps enough resolution
    for the weak MOD2 CV-audio input to survive the tape stage without turning
    immediately into quantisation crunch.
  - multiple playback heads stay cheap enough for the RP2350 to run as a simple
    live effect, without block decoders or buffering tricks.

The TPDF dither is the highest-value code in this firmware. Without it,
repeated requantisation produces correlated distortion that sounds like a
broken toy. With it, generation loss sounds like tape hiss.

  --Pin assign---
POT1     A0       Delay time (20 ms .. 7.0 s, exponential)
POT2     A1       Feedback (0 .. 0.62)
POT3     A2       Input DRIVE / bias (analog; see above)
IN1      GPIO7    Tap tempo / sync
IN2      GPIO0    Freeze (high = stop recording, repeat forever)
CV       A2       AUDIO INPUT (shared with POT3)
BUTTON   GPIO6    short = tap | >3 s = clear RAM tape
                  hold + POT1 = head mode (6) | hold + POT2 = wow & flutter
                  hold + POT3 = tape character (4)
OUT      GPIO1    10-bit PWM audio (~36.6 kHz), 2-pole RC, U2B gain 3.06 -> +/-5 V
LED      GPIO5    Legacy PWM LED or MELON RGB LED, selected at boot.
                  Envelope + clip + status.
EEPROM   N/A      (active tape lives in SRAM and is cleared at boot)

JP1 selects the output reconstruction filter: 10n = 15.9 kHz, 10n+22n = 5.0 kHz.
At 5.0 kHz the 36.6 kHz sampling images sit ~32 dB down; at 15.9 kHz only ~12 dB
down, so you gain top end and hear some aliasing. Either is musically valid here.
JP2 shorts C18 and DC-couples the output — leave it OPEN for this firmware.

CC0 1.0 Universal (CC0 1.0) Public Domain Dedication
*/

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>
#include "pico/platform.h"
#include "pico/multicore.h"
#include "hardware/pwm.h"
#include "hardware/irq.h"
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

/* ============================ pins ====================================
 * Prefixed MOD2_ because the XIAO variant header already defines PIN_LED. */
static const uint MOD2_AUDIO  = 1;    // PWM audio out
static const uint MOD2_IRQ    = 2;    // dummy PWM slice, ISR timebase only
static const uint MOD2_LED    = 5;    // D9 via R24 3.3k
static const uint MOD2_BUTTON = 6;    // active low, needs pullup
static const uint MOD2_IN2    = 0;    // freeze
static const uint MOD2_IN1    = 7;    // tap / sync
static const uint8_t LED_MODE_LEGACY = 0;
static const uint8_t LED_MODE_MELON = 1;
static const uint32_t LED_CFG_OFFSET = 0x1F0000;
static const uint32_t LED_CFG_MAGIC = 0x4C454431u; // "LED1"

/* ============================ rates ===================================
 * Audio ISR: 150 MHz / 4096 = 36,621.09 Hz.  Tape: exactly half of that.
 *
 * The tape transport is clocked by the ADC (48 MHz, PLL_USB) and the playback
 * ISR by the PWM (150 MHz, PLL_SYS). Those are different PLLs but both lock to
 * the same 12 MHz crystal, so they are coherent — there is no free-running
 * drift between them. What is left is a fixed ~13 ppm offset, because the ADC
 * divider quantises to 1/256 and the rate we want is not exactly representable.
 * 13 ppm sounds ignorable, and for the delay time it is. It is not ignorable
 * for the read head: at 0.26 samples/s the head would creep past the write
 * head, and a 20 ms delay would be audibly wrong inside half an hour. So the
 * read position is slaved to the write head by the small PLL in the ISR. */
static const float SYS_CLK  = 150000000.0f;
static const int   IRQ_WRAP = 4095;                       // -> 36,621.09 Hz
static const float FS_OUT   = SYS_CLK / (IRQ_WRAP + 1);
static const float FS_TAPE  = FS_OUT / 2.0f;              // 18,310.55 Hz

static const int   PWM_WRAP = 1023;                       // 10-bit, 146 kHz carrier
static const float INPUT_GAIN  = 1.35f;                   // tape-record gain before saturation
static const float OUTPUT_GAIN = 11.0f;                   // wet makeup gain before soft limit
static const float DRY_GAIN    = 0.0f;                    // intentionally off in v0.3
static const float INPUT_GATE_ADC = 18.0f;                // suppress idle ADC/front-end hiss
static const float INPUT_GATE_KNEE = 70.0f;               // soft-open range in ADC counts
static const bool  LOAD_SAVED_TAPE_ON_BOOT = false;       // tape echo mode: no boot loop recall

/* ============================ the tape ================================
 * v0.3 keeps the live tape in SRAM and uses flash for persistence only.
 * 256 KB @ 18,310.5 Hz, 16-bit = 7.16 s. Flash streaming is a later target.
 * TAPE_LEN must stay a power of two: the position arithmetic masks, not mods. */
static const uint32_t TAPE_LEN = 131072;                  // 2^17 int16 samples
#define TAPE_SECONDS (TAPE_LEN / FS_TAPE)                 // 7.16 s

static int16_t g_tape[TAPE_LEN];

/* Playback position is Q12 fixed point, not float, and that is not fussiness.
 * TAPE_LEN is 2^17; a float has 24 bits of mantissa, so near the end of the
 * tape its resolution is 1/32 of a sample. The PLL correction below is a few
 * 1/4096ths of a sample per ISR — it would round to zero and the loop would
 * silently stop tracking at high positions. 17 + 12 = 29 bits fits a uint32
 * with exact resolution everywhere. */
#define POS_FRAC  12
#define POS_ONE   (1u << POS_FRAC)
#define POS_SPAN  (TAPE_LEN << POS_FRAC)                  // 2^29
#define POS_MASK  (POS_SPAN - 1u)

/* Flash image. Tape at a 64 KB-aligned offset so flash_range_erase() can use
 * block erases (4 x ~150 ms) instead of 64 sector erases (~2.9 s). The sketch
 * itself is ~57 KB, so 1.5 MB is a very safe floor. */
static const uint32_t FLASH_TAPE_OFFSET = 0x180000;       // 1.5 MB, 64 KB aligned
static const uint32_t FLASH_TAPE_SIZE   = TAPE_LEN * sizeof(g_tape[0]);
static const uint32_t FLASH_HDR_OFFSET  = 0x1C0000;       // one 4 KB sector
static const uint32_t FLASH_HDR_MAGIC   = 0x504C4D50u;    // 'PLMP'
static const uint8_t  FLASH_HDR_VERSION = 2;

typedef struct {
  uint32_t magic;
  uint8_t  version;
  uint8_t  pad[3];
  uint32_t len;
  uint32_t fnv;          // FNV-1a over the tape image
  uint8_t  head_mode;
  uint8_t  character;
  uint8_t  pad2[2];
} tape_hdr_t;

/* ============================ ADC ====================================
 * Round-robin A0/A1/A2 into a block DMA, restarted from ADC0 each block so the
 * channel phase can never drift (the Metal_Hit / Arcade_Laser trick).
 *
 * Block geometry is chosen so one block yields a whole number of tape samples:
 *   A2 rate      = 6 * FS_TAPE = 109,863.3 Hz   (decimate 6:1)
 *   ADC total    = 3 * that    = 329,589.8 Hz
 *   block        = 432 samples = 144 per channel = exactly 24 tape samples
 *   block period = 1.31 ms
 *
 * A box average of 6 is not a lazy filter — for decimate-by-6 it puts its
 * nulls exactly on the alias centres (18.3 kHz and multiples), costs an add,
 * and hands back ~1.3 bits of oversampling gain. Its -3.8 dB droop at 9 kHz is
 * more tape character. The pots get 144 samples averaged per block for free. */
static const int ADC_CH_COUNT   = 3;
static const int ADC_DECIM      = 6;
static const int TAPE_PER_BLOCK = 24;
static const int ADC_SAMPLES_PER_CH = ADC_DECIM * TAPE_PER_BLOCK;    // 144
static const int ADC_BLOCK      = ADC_CH_COUNT * ADC_SAMPLES_PER_CH; // 432

static const float ADC_CLK_HZ = 48000000.0f;
static const float ADC_CLKDIV = (ADC_CLK_HZ / (3.0f * ADC_DECIM * FS_TAPE)) - 1.0f;

static uint16_t adc_buffer[ADC_BLOCK];
static int      adc_dma_chan;
volatile uint16_t adc_avg[ADC_CH_COUNT] = {0, 0, 0};   // [0],[1] pots; [2] is the audio

/* ============================ transport =============================== */
volatile uint32_t g_write_pos = 0;      // tape write head, advances at FS_TAPE
volatile bool     g_freeze    = false;
volatile float    g_head_sum  = 0.0f;   // latest summed head output, for feedback
volatile float    g_live_in   = 0.0f;   // latest input sample, for dry passthrough
volatile float    g_env       = 0.0f;   // envelope follower, for the LED
volatile uint32_t g_clip_ms   = 0;      // millis() of last input clamp hit

volatile float g_delay_target = 0.25f;  // seconds
volatile float g_feedback     = 0.35f;
volatile float g_wow_depth    = 0.15f;
volatile int   g_head_mode    = 3;
volatile int   g_character    = 0;

/* Flash park handshake. */
volatile bool g_flash_park_req = false;
volatile bool g_flash_parked   = false;
volatile int  g_flash_status   = 0;     // 0 idle, 1 saving

/* ============================ RAM-resident tables =====================
 * The audio ISR reads these every sample, so they must not live in flash —
 * see the ISR comment for why. __not_in_flash() forces the section.
 *
 * Do NOT "simplify" this by just dropping `const`. That is not enough, and it
 * fails silently: GCC sees an array it can prove is never written and promotes
 * it to .rodata regardless, which is flash behind XIP. Verified on this build —
 * before adding __not_in_flash(), head_frac and head_count landed at 0x10010680
 * and 0x100106c8, and were the ISR's only two flash references. The attribute
 * is the only thing that actually pins them.
 *
 * (lut_sin is uninitialised, so it goes to .bss = RAM either way, and marking
 * it would only cost 1 KB of flash for a zero-init image. head_norm is written
 * in setup() so it cannot be promoted. Both are marked anyway: relying on "the
 * compiler cannot prove this is constant" is exactly the reasoning that just
 * failed above.) */
static float __not_in_flash("pal") lut_sin[256];

static int   __not_in_flash("pal") head_count[6]   = { 1, 2, 3, 3, 3, 3 };
static float __not_in_flash("pal") head_norm[6]    = { 1, 1, 1, 1, 1, 1 };  // set in setup
static float __not_in_flash("pal") head_frac[6][3] = { {1.0f, 0.0f,   0.0f  },
                                                       {1.0f, 0.375f, 0.0f  },
                                                       {1.0f, 0.667f, 0.333f},
                                                       {1.0f, 0.625f, 0.188f},
                                                       {1.0f, 0.875f, 0.750f},
                                                       {1.0f, 0.500f, 0.125f} };

/* Wow ~0.6 Hz (capstan eccentricity), flutter ~7.3 Hz (roller chatter). */
static const uint32_t WOW_INC = (uint32_t)(0.6f / (SYS_CLK / 4096.0f) * 4294967296.0f);
static const uint32_t FLT_INC = (uint32_t)(7.3f / (SYS_CLK / 4096.0f) * 4294967296.0f);

/* ============================ RNG / dither ============================ */
static inline uint32_t __not_in_flash_func(rng_next)() {
  static uint32_t s = 0x9E3779B9u;
  s = s * 1664525u + 1013904223u;
  return s;
}

/* TPDF: difference of two uniforms -> triangular over (-1, +1) LSB.
 * This is what turns requantisation distortion into tape hiss. */
static inline float __not_in_flash_func(tpdf)() {
  const float k = 1.0f / 16777216.0f;
  float a = (float)(rng_next() >> 8) * k;
  float b = (float)(rng_next() >> 8) * k;
  return a - b;
}

/* ============================ tape access =============================
 * Q12 position in, interpolated sample out. No floorf(), no negative handling,
 * no wrap branch — the mask does all three. */
static inline float __not_in_flash_func(tape_read_q)(uint32_t pos_q) {
  pos_q &= POS_MASK;
  uint32_t i0 = pos_q >> POS_FRAC;
  uint32_t i1 = (i0 + 1) & (TAPE_LEN - 1);
  float    f  = (float)(pos_q & (POS_ONE - 1)) * (1.0f / (float)POS_ONE);
  float    a  = (float)g_tape[i0];
  float    b  = (float)g_tape[i1];
  return (a + (b - a) * f) * (1.0f / 32768.0f);
}

/* =======================================================================
 *  AUDIO ISR — core 0, 36,621 Hz.
 *
 *  READS NO FLASH, and that is checked, not assumed. Verify after any edit:
 *
 *    arm-none-eabi-objdump -d --start-address=<audio_isr> --stop-address=... elf
 *      -> no `bl` out of the function  (a libm call would appear here)
 *      -> no `.word 0x10......`        (a .rodata read would appear here)
 *
 *  It holds because: the code is __not_in_flash_func; the tables are pinned
 *  with __not_in_flash; the tape is .bss; this function's float literals live
 *  in its own literal pool, which is emitted inline with the code and so is
 *  also in RAM; and every math call is open-coded (soft limiters, sine LUT) so
 *  nothing branches into libm.
 *
 *  v0.1 does not depend on this yet — it parks core 0 during a save. The
 *  invariant is kept from the start because it is what v0.2 needs to keep
 *  audio alive across an XIP-down window, and it is far harder to retrofit
 *  than to maintain.
 * ==================================================================== */
static uint sliceAudio, sliceIRQ, sliceLED;
static uint chanLED;
static uint8_t g_led_mode = LED_MODE_MELON;
static Adafruit_NeoPixel g_rgb(1, MOD2_LED, NEO_GRB + NEO_KHZ800);
static uint32_t g_last_rgb_ms = 0;
static uint32_t g_last_rgb_color = 0xFFFFFFFFu;

void __not_in_flash_func(audio_isr)() {
  /* ---- playback clock, slaved to the tape write head -------------------
   * Advance exactly half a tape sample per ISR, then pull gently toward the
   * true write head to absorb the ~13 ppm divider offset. One pole, ~7 ms —
   * far too slow to chase the 18 kHz staircase of the write head itself, far
   * too fast to ever let the heads cross. */
  static uint32_t now_q = 0;
  now_q = (now_q + (POS_ONE / 2)) & POS_MASK;

  int32_t err = (int32_t)(((g_write_pos << POS_FRAC) - now_q) & POS_MASK);
  if (err > (int32_t)(POS_SPAN / 2)) err -= (int32_t)POS_SPAN;
  if (err > (int32_t)(POS_ONE * 8) || err < -(int32_t)(POS_ONE * 8))
    now_q = (g_write_pos << POS_FRAC) & POS_MASK;          // resync after erase/load
  else
    now_q = (now_q + (uint32_t)(err >> 8)) & POS_MASK;

  /* ---- delay time, slewed like tape ------------------------------------
   * A real echo cannot jump its delay time: the tape has to physically change
   * speed, and you hear the pitch glide while it does. That glide is most of
   * what makes this sound like tape and not like a digital delay, so the slew
   * is always on and is not a parameter. ~150 ms time constant. */
  static float delay_cur = 400.0f;
  float delay_tgt = g_delay_target * FS_TAPE;
  delay_cur += (delay_tgt - delay_cur) * 0.00018f;
  if (delay_cur < 4.0f) delay_cur = 4.0f;
  if (delay_cur > (float)(TAPE_LEN - 8)) delay_cur = (float)(TAPE_LEN - 8);

  /* ---- wow & flutter --------------------------------------------------- */
  static uint32_t wow_ph = 0, flt_ph = 0;
  wow_ph += WOW_INC;
  flt_ph += FLT_INC;
  int character = g_character;
  float character_wobble = 0.80f;
  float out_knee = 0.92f;
  if (character == 1) {          // Dub: steady, darker, less seasick.
    character_wobble = 0.65f;
    out_knee = 0.86f;
  } else if (character == 2) {   // Smear: wider pitch haze.
    character_wobble = 1.35f;
    out_knee = 0.90f;
  } else if (character == 3) {   // Unstable: intentionally wobbly.
    character_wobble = 2.15f;
    out_knee = 0.78f;
  }
  float wobble = (lut_sin[wow_ph >> 24] * 0.0035f + lut_sin[flt_ph >> 24] * 0.0008f)
                 * g_wow_depth * character_wobble;

  /* ---- read the heads --------------------------------------------------- */
  int   mode = g_head_mode;
  int   n    = head_count[mode];
  float sum  = 0.0f;
  for (int i = 0; i < n; i++) {
    float d = delay_cur * head_frac[mode][i];
    d += d * wobble;
    if (d < 1.0f) d = 1.0f;
    sum += tape_read_q(now_q - (uint32_t)(d * (float)POS_ONE));
  }
  sum *= head_norm[mode];

  g_head_sum = sum;   // fed back into the tape by core 1; no content above
                      // 9.15 kHz, so resampling it at 18.3 kHz is safe

  /* ---- envelope follower for the LED ------------------------------------ */
  float a = (sum < 0.0f) ? -sum : sum;
  float e = g_env;
  e += (a - e) * ((a > e) ? 0.02f : 0.0008f);
  g_env = e;

  /* ---- output ----------------------------------------------------------
   * Keep most of the range linear. The old always-on tanh made weak, dirty
   * input feel crushed even before the final level was usable. */
  float dry = g_live_in * DRY_GAIN;
  float x = sum * OUTPUT_GAIN + dry;
  float ax = (x < 0.0f) ? -x : x;
  float y = x;
  if (ax > out_knee) {
    float over = ax - out_knee;
    float lim = out_knee + over / (1.0f + over * 4.0f);
    if (lim > 1.0f) lim = 1.0f;
    y = (x < 0.0f) ? -lim : lim;
  }
  int v = (int)(y * 511.0f + 512.0f);
  if (v < 0) v = 0;
  if (v > PWM_WRAP) v = PWM_WRAP;
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B, (uint16_t)v);

  pwm_clear_irq(sliceIRQ);
}

/* =======================================================================
 *  ADC BLOCK HANDLER — core 1, every 1.31 ms.
 *
 *  Owns the tape transport: decimate, remove DC, dither, write. The write head
 *  advances here, at the ADC-derived rate — the transport has its own speed,
 *  independent of the playback electronics, which is both physically honest
 *  and avoids needing an async rate converter between the two clocks.
 * ==================================================================== */
static void restart_adc_dma() {
  adc_run(false);
  adc_set_round_robin(0);
  adc_select_input(0);              // re-anchor: buffer[i*3+c] is always channel c
  adc_fifo_drain();
  dma_channel_set_read_addr(adc_dma_chan, &adc_hw->fifo, false);
  dma_channel_set_write_addr(adc_dma_chan, adc_buffer, false);
  dma_channel_set_trans_count(adc_dma_chan, ADC_BLOCK, false);
  adc_set_round_robin(0b00000111);
  dma_channel_start(adc_dma_chan);
  adc_run(true);
}

void adc_block_handler() {
  dma_channel_acknowledge_irq0(adc_dma_chan);

  /* ---- pots: 144 samples each, already filtered to 159 Hz by C2/C3 ------ */
  uint32_t s0 = 0, s1 = 0, s2 = 0;
  for (int i = 0; i < ADC_BLOCK; i += ADC_CH_COUNT) {
    s0 += adc_buffer[i + 0];
    s1 += adc_buffer[i + 1];
    s2 += adc_buffer[i + 2];
  }
  adc_avg[0] = (uint16_t)(s0 / ADC_SAMPLES_PER_CH);
  adc_avg[1] = (uint16_t)(s1 / ADC_SAMPLES_PER_CH);
  adc_avg[2] = (uint16_t)(s2 / ADC_SAMPLES_PER_CH);

  /* ---- A2: decimate 6:1, subtract DC, dither, write --------------------- */
  static float dc = 2048.0f;
  bool  frozen  = g_freeze;
  float fb      = g_feedback;
  int   character = g_character;
  bool  clipped = false;

  for (int t = 0; t < TAPE_PER_BLOCK; t++) {
    /* box-6 decimator: nulls land on the alias centres, ~1.3 bits of gain */
    uint32_t acc = 0;
    int base = t * ADC_DECIM * ADC_CH_COUNT + 2;
    for (int k = 0; k < ADC_DECIM; k++) acc += adc_buffer[base + k * ADC_CH_COUNT];
    float a2 = (float)acc * (1.0f / (float)ADC_DECIM);

    /* DC tracker at ~0.5 Hz. Slow enough that a 20 Hz bass note passes
     * untouched (-0.03 dB), fast enough to follow POT3 in ~2 s. Recovering
     * POT3 from it is just `3.53 - dc*3.3/4095` if a later version wants it. */
    dc += (a2 - dc) * 0.00017f;

    /* Audio is inverted by U2A, so negate. Normalised against a fixed
     * half-scale so gain does not depend on where POT3 put the bias — the
     * headroom does, which is the whole point of POT3 being the drive. */
    float dev = a2 - dc;
    if (dev > 1900.0f || dev < -1900.0f) clipped = true;
    float abs_dev = (dev < 0.0f) ? -dev : dev;
    float gate = (abs_dev - INPUT_GATE_ADC) * (1.0f / INPUT_GATE_KNEE);
    if (gate < 0.0f) gate = 0.0f;
    if (gate > 1.0f) gate = 1.0f;
    gate = gate * gate * (3.0f - 2.0f * gate);
    float in = -dev * (INPUT_GAIN / 2047.0f) * gate;
    float input_alpha = 0.48f;
    if (character == 1) input_alpha = 0.34f;       // Dub: rounder repeats.
    else if (character == 2) input_alpha = 0.12f;  // Smear: intentionally soft.
    else if (character == 3) input_alpha = 0.58f;  // Unstable: lets edges hit.
    static float input_lp = 0.0f;
    input_lp += (in - input_lp) * input_alpha;
    in = input_lp;
    g_live_in = in * gate;

    /* Tape colour, INSIDE the feedback path. Keep normal levels mostly linear,
     * then soften the top of the range so feedback can bloom without turning
     * every repeat into full-time square crush. */
    static float fb_lp = 0.0f;
    float fb_alpha = 0.20f;
    if (character == 1) fb_alpha = 0.11f;
    else if (character == 2) fb_alpha = 0.065f;
    else if (character == 3) fb_alpha = 0.34f;
    fb_lp += (g_head_sum - fb_lp) * fb_alpha;

    float char_in = in;
    float char_fb = fb * fb_lp;
    float rec_soft_knee = 0.82f;
    float rec_ceiling = 1.0f;
    if (character == 1) {          // Dub: darker feedback bloom.
      char_fb *= 1.22f;
      char_in *= 0.96f;
      rec_soft_knee = 0.74f;
    } else if (character == 2) {   // Smear: blurrier and less assertive.
      char_in *= 0.82f;
      char_fb *= 0.90f;
      rec_soft_knee = 0.84f;
    } else if (character == 3) {   // Unstable: crunch and motion on purpose.
      char_in *= 1.12f;
      char_fb *= 1.16f;
      rec_soft_knee = 0.58f;
    }
    char_fb *= 1.0f - gate * 0.42f;  // let new input cut through at high feedback

    float rec = char_in + char_fb;
    float arec = (rec < 0.0f) ? -rec : rec;
    if (arec > rec_soft_knee) {
      float over = arec - rec_soft_knee;
      float shaped = rec_soft_knee + over * 0.28f;
      if (shaped > rec_ceiling) shaped = rec_ceiling;
      rec = (rec < 0.0f) ? -shaped : shaped;
    }
    if (character == 2) {
      static float smear = 0.0f;
      smear += (rec - smear) * 0.055f;
      rec = smear;
    } else if (character == 3) {
      rec += tpdf() * 0.0045f;
    }

    /* TPDF dither, then 16-bit. This is the line that makes generation loss
     * sound like a Revox instead of a broken toy. */
    if (!frozen) {
      float s = rec * 32767.0f + tpdf();
      int q = (int)(s + (s < 0.0f ? -0.5f : 0.5f));
      if (q >  32767) q =  32767;
      if (q < -32768) q = -32768;
      g_tape[g_write_pos] = (int16_t)q;
    }
    g_write_pos = (g_write_pos + 1) & (TAPE_LEN - 1);
  }

  if (clipped) g_clip_ms = millis();

  restart_adc_dma();
}

/* =======================================================================
 *  FLASH PERSISTENCE — the whole point: the loop survives a power cycle.
 *
 *  v0.1 does this the SAFE way — both cores parked, all interrupts off, audio
 *  muted for ~1.1 s. It is an explicit, deliberate user action with LED
 *  feedback, so a gap is acceptable. Keeping audio alive across the XIP-down
 *  window (mask everything except PWM_IRQ_WRAP instead of parking core 0) is
 *  exactly the machinery v0.2 needs for continuous streaming; the ISR is
 *  already flash-free, so that step is a scheduling change, not a rewrite.
 * ==================================================================== */
static uint32_t fnv1a(const uint8_t *p, uint32_t n) {
  uint32_t h = 2166136261u;
  for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
  return h;
}

static void __not_in_flash_func(flash_commit)(const uint8_t *hdr_page) {
  /* Everything from the park to restore_interrupts() must be RAM-resident.
   * flash_range_erase/program already are — pico-sdk marks them. */
  g_flash_park_req = true;
  while (!g_flash_parked) tight_loop_contents();

  uint32_t ints = save_and_disable_interrupts();
  flash_range_erase(FLASH_TAPE_OFFSET, FLASH_TAPE_SIZE);
  flash_range_program(FLASH_TAPE_OFFSET, (const uint8_t *)g_tape, FLASH_TAPE_SIZE);
  flash_range_erase(FLASH_HDR_OFFSET, FLASH_SECTOR_SIZE);
  flash_range_program(FLASH_HDR_OFFSET, hdr_page, FLASH_PAGE_SIZE);
  restore_interrupts(ints);

  g_flash_park_req = false;
}

static void tape_save() {
  static uint8_t hdr_page[FLASH_PAGE_SIZE];
  tape_hdr_t hdr;
  memset(&hdr, 0, sizeof(hdr));
  hdr.magic     = FLASH_HDR_MAGIC;
  hdr.version   = FLASH_HDR_VERSION;
  hdr.len       = FLASH_TAPE_SIZE;
  hdr.fnv       = fnv1a((const uint8_t *)g_tape, FLASH_TAPE_SIZE);   // XIP still up here
  hdr.head_mode = (uint8_t)g_head_mode;
  hdr.character = (uint8_t)g_character;
  memset(hdr_page, 0xFF, FLASH_PAGE_SIZE);
  memcpy(hdr_page, &hdr, sizeof(hdr));

  g_flash_status = 1;
  flash_commit(hdr_page);
  g_flash_status = 0;
}

static bool tape_load() {
  const tape_hdr_t *hdr = (const tape_hdr_t *)(XIP_BASE + FLASH_HDR_OFFSET);
  if (hdr->magic   != FLASH_HDR_MAGIC)   return false;
  if (hdr->version != FLASH_HDR_VERSION) return false;
  if (hdr->len     != FLASH_TAPE_SIZE)   return false;

  const uint8_t *img = (const uint8_t *)(XIP_BASE + FLASH_TAPE_OFFSET);
  if (fnv1a(img, FLASH_TAPE_SIZE) != hdr->fnv) return false;

  memcpy(g_tape, img, FLASH_TAPE_SIZE);
  if (hdr->head_mode < 6) g_head_mode = hdr->head_mode;
  if (hdr->character < 4) g_character = hdr->character;
  return true;
}

typedef struct {
  uint32_t magic;
  uint8_t mode;
  uint8_t pad[3];
  uint32_t crc;
} led_cfg_t;

static void load_led_mode() {
  const led_cfg_t *cfg = (const led_cfg_t *)(XIP_BASE + LED_CFG_OFFSET);
  if (cfg->magic != LED_CFG_MAGIC) return;
  if (cfg->mode > LED_MODE_MELON) return;
  if (fnv1a((const uint8_t *)cfg, offsetof(led_cfg_t, crc)) != cfg->crc) return;
  g_led_mode = cfg->mode;
}

static void __not_in_flash_func(save_led_mode)() {
  static uint8_t page[FLASH_PAGE_SIZE];
  led_cfg_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.magic = LED_CFG_MAGIC;
  cfg.mode = g_led_mode;
  cfg.crc = fnv1a((const uint8_t *)&cfg, offsetof(led_cfg_t, crc));
  memset(page, 0xFF, FLASH_PAGE_SIZE);
  memcpy(page, &cfg, sizeof(cfg));

  uint32_t ints = save_and_disable_interrupts();
  flash_range_erase(LED_CFG_OFFSET, FLASH_SECTOR_SIZE);
  flash_range_program(LED_CFG_OFFSET, page, FLASH_PAGE_SIZE);
  restore_interrupts(ints);
}

static uint32_t pal_color(bool down, bool shift_used, uint32_t held, uint32_t now_ms) {
  if (g_flash_status != 0) return 0xFFFFFF;
  if (down && shift_used) {
    switch (g_character) {
      case 0: return 0xFFB000;   // Tape
      case 1: return 0x20FF80;   // Dub
      case 2: return 0x8060FF;   // Smear
      default: return 0xFF2080;  // Unstable
    }
  }
  if (down && !shift_used && held > 3000) return 0xFF2020;
  if (now_ms - g_clip_ms < 100) return 0xFF3000;
  if (g_freeze) return 0x0040FF;
  return 0xFFB000;
}

static void write_led(uint16_t level, uint32_t color) {
  level = (level > 1023) ? 1023 : level;
  if (g_led_mode == LED_MODE_LEGACY) {
    pwm_set_chan_level(sliceLED, chanLED, level);
    return;
  }

  uint32_t now = millis();
  uint8_t brightness = (uint8_t)((uint32_t)level * 160u / 1023u);
  uint8_t r = ((color >> 16) & 0xFF) * brightness / 255;
  uint8_t g = ((color >> 8) & 0xFF) * brightness / 255;
  uint8_t b = (color & 0xFF) * brightness / 255;
  uint32_t scaled = g_rgb.Color(r, g, b);
  if (scaled == g_last_rgb_color && (now - g_last_rgb_ms) < 50) return;
  if ((now - g_last_rgb_ms) < 25) return;
  g_rgb.setPixelColor(0, scaled);
  g_rgb.show();
  g_last_rgb_color = scaled;
  g_last_rgb_ms = now;
}

static void boot_led_menu() {
  if (digitalRead(MOD2_BUTTON) != LOW) return;

  uint8_t selected = g_led_mode;
  uint32_t start = millis();
  while (digitalRead(MOD2_BUTTON) == LOW) {
    selected = (analogRead(A1) < 2048) ? LED_MODE_LEGACY : LED_MODE_MELON;
    uint8_t pulse = (uint8_t)(32 + ((millis() / 6) & 0x7F));
    if (selected == LED_MODE_LEGACY) {
      gpio_set_function(MOD2_LED, GPIO_FUNC_PWM);
      sliceLED = pwm_gpio_to_slice_num(MOD2_LED);
      chanLED = pwm_gpio_to_channel(MOD2_LED);
      pwm_set_wrap(sliceLED, 1023);
      pwm_set_enabled(sliceLED, true);
      pwm_set_chan_level(sliceLED, chanLED, (uint16_t)pulse * 8);
    } else {
      g_rgb.begin();
      g_rgb.setPixelColor(0, g_rgb.Color(pulse, 0, 180 - pulse));
      g_rgb.show();
    }
    delay(10);
    if (millis() - start > 10000) break;
  }

  g_led_mode = selected;
  save_led_mode();
  if (g_led_mode == LED_MODE_MELON) {
    g_rgb.begin();
    g_rgb.setPixelColor(0, g_rgb.Color(0, 80, 255));
    g_rgb.show();
    delay(180);
    g_rgb.clear();
    g_rgb.show();
  }
}

/* =======================================================================
 *  SETUP — core 0
 * ==================================================================== */
void setup() {
  analogReadResolution(12);
  pinMode(MOD2_BUTTON, INPUT_PULLUP);
  pinMode(MOD2_IN1, INPUT);
  pinMode(MOD2_IN2, INPUT);
  load_led_mode();
  boot_led_menu();

  for (int i = 0; i < 256; i++) lut_sin[i] = sinf(2.0f * (float)M_PI * (float)i / 256.0f);
  for (int m = 0; m < 6; m++)   head_norm[m] = 1.0f / sqrtf((float)head_count[m]);

  memset(g_tape, 0, FLASH_TAPE_SIZE);
  if (LOAD_SAVED_TAPE_ON_BOOT) tape_load();

  /* Audio PWM: GPIO1 -> slice 0 chan B, 10-bit, 146 kHz carrier */
  gpio_set_function(MOD2_AUDIO, GPIO_FUNC_PWM);
  sliceAudio = pwm_gpio_to_slice_num(MOD2_AUDIO);
  pwm_set_clkdiv(sliceAudio, 1);
  pwm_set_wrap(sliceAudio, PWM_WRAP);
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B, PWM_WRAP / 2);
  pwm_set_enabled(sliceAudio, true);

  if (g_led_mode == LED_MODE_LEGACY) {
    /* LED PWM: GPIO5 -> slice 2 chan B, ~18 kHz so it cannot flicker */
    gpio_set_function(MOD2_LED, GPIO_FUNC_PWM);
    sliceLED = pwm_gpio_to_slice_num(MOD2_LED);
    chanLED  = pwm_gpio_to_channel(MOD2_LED);
    pwm_set_clkdiv(sliceLED, 8);
    pwm_set_wrap(sliceLED, 1023);
    pwm_set_chan_level(sliceLED, chanLED, 0);
    pwm_set_enabled(sliceLED, true);
  } else {
    g_rgb.begin();
    g_rgb.setBrightness(255);
    g_rgb.clear();
    g_rgb.show();
  }

  /* ISR timebase: GPIO2 -> slice 1, wrap 4095 -> 36,621 Hz. GPIO2 is not
   * routed to anything on the MOD2; the slice exists purely for its wrap IRQ.
   * Same convention as every other firmware in this collection. */
  gpio_set_function(MOD2_IRQ, GPIO_FUNC_PWM);
  sliceIRQ = pwm_gpio_to_slice_num(MOD2_IRQ);
  pwm_clear_irq(sliceIRQ);
  pwm_set_irq_enabled(sliceIRQ, true);
  irq_set_exclusive_handler(PWM_IRQ_WRAP, audio_isr);
  irq_set_priority(PWM_IRQ_WRAP, 0x00);         // highest: it must outrank everything
  irq_set_enabled(PWM_IRQ_WRAP, true);
  pwm_set_clkdiv(sliceIRQ, 1);
  pwm_set_wrap(sliceIRQ, IRQ_WRAP);
  pwm_set_enabled(sliceIRQ, true);
}

/* Core 0 owns the audio ISR and NOTHING else. This function is RAM-resident
 * and never returns, so core 0 stops fetching from flash entirely once setup
 * is done — which is what lets core 1 erase and program underneath it. */
void __not_in_flash_func(loop)() {
  while (true) {
    if (g_flash_park_req) {
      uint32_t ints = save_and_disable_interrupts();
      g_flash_parked = true;
      while (g_flash_park_req) tight_loop_contents();
      g_flash_parked = false;
      restore_interrupts(ints);
    }
    tight_loop_contents();
  }
}

/* =======================================================================
 *  SETUP / LOOP — core 1: ADC block IRQ, UI, flash
 * ==================================================================== */
void setup1() {
  delay(20);                       // let core 0 finish claiming hardware
  adc_init();
  adc_gpio_init(26);   // A0 POT1
  adc_gpio_init(27);   // A1 POT2
  adc_gpio_init(28);   // A2 POT3 + CV(audio)
  adc_set_clkdiv(ADC_CLKDIV);
  adc_fifo_setup(true, true, 1, false, false);   // FIFO on, DREQ on, 12-bit

  adc_dma_chan = dma_claim_unused_channel(true);
  dma_channel_config c = dma_channel_get_default_config(adc_dma_chan);
  channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
  channel_config_set_read_increment(&c, false);
  channel_config_set_write_increment(&c, true);
  channel_config_set_dreq(&c, DREQ_ADC);
  dma_channel_configure(adc_dma_chan, &c, adc_buffer, &adc_hw->fifo, ADC_BLOCK, false);
  dma_channel_set_irq0_enabled(adc_dma_chan, true);

  irq_set_exclusive_handler(DMA_IRQ_0, adc_block_handler);
  irq_set_enabled(DMA_IRQ_0, true);
  restart_adc_dma();
}

/* --- UI state ---------------------------------------------------------- */
static uint32_t btn_down_ms   = 0;
static bool     btn_was_down  = false;
static bool     shift_used    = false;   // pot moved while held -> suppress release
static uint16_t shift_snap[3] = {0, 0, 0};
static uint32_t last_tap_ms   = 0;
static bool     in1_last      = false;

static float pot_to_delay(uint16_t raw) {
  float x = (float)raw * (1.0f / 4095.0f);
  const float lo = 0.020f, hi = TAPE_SECONDS * 0.98f;
  return lo * powf(hi / lo, x);
}

static void handle_tap(uint32_t now_ms) {
  if (last_tap_ms != 0) {
    uint32_t dt = now_ms - last_tap_ms;
    if (dt > 40 && dt < (uint32_t)(TAPE_SECONDS * 1000.0f)) g_delay_target = (float)dt * 0.001f;
  }
  last_tap_ms = now_ms;
}

void loop1() {
  uint32_t now_ms = millis();

  g_freeze = (digitalRead(MOD2_IN2) == HIGH);

  bool in1 = (digitalRead(MOD2_IN1) == HIGH);
  if (in1 && !in1_last) handle_tap(now_ms);
  in1_last = in1;

  /* ---- button ---------------------------------------------------------- */
  bool down = (digitalRead(MOD2_BUTTON) == LOW);

  if (down && !btn_was_down) {
    btn_down_ms   = now_ms;
    shift_used    = false;
    shift_snap[0] = adc_avg[0];
    shift_snap[1] = adc_avg[1];
    shift_snap[2] = adc_avg[2];
  }

  if (down) {
    /* Shift layer, RALPS idiom: a pot that moves past a threshold while the
     * button is held claims the gesture and suppresses the release action. */
    int d0 = (int)adc_avg[0] - (int)shift_snap[0];
    int d1 = (int)adc_avg[1] - (int)shift_snap[1];
    int d2 = (int)adc_avg[2] - (int)shift_snap[2];
    if (abs(d0) > 200) {
      shift_used = true;
      int m = (int)((float)adc_avg[0] * 6.0f / 4096.0f);
      g_head_mode = (m > 5) ? 5 : m;
    }
    if (abs(d1) > 200) {
      shift_used = true;
      g_wow_depth = (float)adc_avg[1] * (1.0f / 4095.0f);
    }
    if (abs(d2) > 200) {
      shift_used = true;
      int c = (int)((float)adc_avg[2] * 4.0f / 4096.0f);
      g_character = (c > 3) ? 3 : c;
    }
  }

  if (!down && btn_was_down && !shift_used) {
    uint32_t held = now_ms - btn_down_ms;
    if (held > 3000) {
      memset(g_tape, 0, FLASH_TAPE_SIZE);
    }
    else if (held > 20) handle_tap(now_ms);
  }
  btn_was_down = down;

  /* ---- pots (free, when not shifted) ------------------------------------ */
  if (!down) {
    g_delay_target = pot_to_delay(adc_avg[0]);
    g_feedback     = (float)adc_avg[1] * (0.62f / 4095.0f);
  }

  /* ---- LED -------------------------------------------------------------
   * Envelope normally. Held >3 s = fast blink (release to clear the RAM tape).
   * Double-blink while the
   * input is into the Schottky clamps — that is your drive indicator for POT3.
   * R24 is 3.3k so D9 only draws ~0.4 mA; gamma the envelope so the low end is
   * actually visible. */
  int level;
  uint32_t held = now_ms - btn_down_ms;
  if (g_flash_status != 0)                     level = 1023;
  else if (down && !shift_used && held > 3000) level = ((now_ms / 80) & 1) ? 1023 : 0;
  else if (now_ms - g_clip_ms < 100)           level = ((now_ms / 40) & 1) ? 1023 : 0;
  else {
    float e = g_env * 2.0f;
    if (e > 1.0f) e = 1.0f;
    level = (int)(sqrtf(e) * 1023.0f);
  }
  write_led(level, pal_color(down, shift_used, held, now_ms));

  delay(2);
}
