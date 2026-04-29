#pragma once

// braids dsp

//const uint16_t decimation_factors[] = { 1, 2, 3, 4, 6, 12, 24 };
const uint16_t bit_reduction_masks[] = {
  0xffff,
  0xfff0,
  0xff00,
  0xf800,
  0xf000,
  0xe000,
  0xc000
};

#define     MI_SAMPLERATE      96000.f
#define     BLOCK_SIZE          32      // --> macro_oscillator.h !
#define     SAMP_SCALE          (float)(1.0 / 32756.0)



typedef struct
{
  braids::MacroOscillator *osc;

  float       samps[BLOCK_SIZE] ;
  int16_t     buffer[BLOCK_SIZE];
  uint8_t     sync_buffer[BLOCK_SIZE];

} PROCESS_CB_DATA ;

char shared_buffer[16384];

//float a0 = (440.0 / 8.0) / kSampleRate; //48000.00;
const size_t   kBlockSize = BLOCK_SIZE;


struct Unit {
  braids::Quantizer   *quantizer;
  braids::SignatureWaveshaper *ws;
  //braids::Envelope *envelope;

  bool            last_trig;
  // resampler
  //SRC_STATE       *src_state;

  PROCESS_CB_DATA pd;
  float           *samples;
  float           ratio;
};

static long src_input_callback(void *cb_data, float **audio);

struct Unit voices[1];

// Plaits modulation vars, reusing names
int16_t morph_in = 4000; // IN(4);
float trigger_in; //IN(5);
float level_in = 0.0f; //IN(6);
float harm_in = 0.1f;
int16_t timbre_in = 4000;
int engine_in;
int32_t previous_pitch;
int32_t pitch_in = 60 << 7;
int16_t pitch_fm;
int16_t pitch_adj = 100;

float fm_mod = 0.0f ; //IN(7);
float timb_mod = 0.0f; //IN(8);
float morph_mod = 0.0f; //IN(9);
float decay_in = 0.5f; // IN(10);
float lpg_in = 0.1f ;// IN(11);


void updateBraidsAudio() {
    int16_t *buffer = voices[0].pd.buffer;
    uint8_t *sync_buffer = voices[0].pd.sync_buffer;
    size_t size = BLOCK_SIZE;
    
    braids::MacroOscillator *osc = voices[0].pd.osc;
    
    // Set pitch
    osc->set_pitch(pitch_in);
    
    // Set shape/model
    uint8_t shape = (int)(engine_in);
    if (shape >= braids::MACRO_OSC_SHAPE_LAST)
        shape -= braids::MACRO_OSC_SHAPE_LAST;
    osc->set_shape(static_cast<braids::MacroOscillatorShape>(shape));
    
    // Edge detection for trigger
    bool trigger = (trigger_in > 0.5f);
    bool trigger_flag = (trigger && (!voices[0].last_trig));
    bool trigger_release = (!trigger && voices[0].last_trig);
    voices[0].last_trig = trigger;
    
    // Percussive models with built-in envelopes
    bool is_percussive = (
        shape == braids::MACRO_OSC_SHAPE_PLUCKED ||
        shape == braids::MACRO_OSC_SHAPE_BOWED ||
        shape == braids::MACRO_OSC_SHAPE_BLOWN ||
        shape == braids::MACRO_OSC_SHAPE_FLUTED ||
        shape == braids::MACRO_OSC_SHAPE_STRUCK_BELL ||
        shape == braids::MACRO_OSC_SHAPE_STRUCK_DRUM ||
        shape == braids::MACRO_OSC_SHAPE_KICK ||
        shape == braids::MACRO_OSC_SHAPE_SNARE ||
        shape == braids::MACRO_OSC_SHAPE_CYMBAL ||
        shape == braids::MACRO_OSC_SHAPE_PARTICLE_NOISE ||
        shape == braids::MACRO_OSC_SHAPE_DIGITAL_MODULATION
    );
    
    // Models that respond to continuous gate
    bool is_gated = (
        shape == braids::MACRO_OSC_SHAPE_VOWEL ||
        shape == braids::MACRO_OSC_SHAPE_VOWEL_FOF
    );
    
    // Handle different envelope behaviors
    if (is_percussive) {
        // Percussive models: trigger on rising edge only
        if (trigger_flag) {
            osc->Strike();
        }
        
        // Timbre often controls decay/damping in percussive models
        // Morph often controls tone/character
        osc->set_parameters(timbre_in, morph_in);
        
    } else if (is_gated) {
        // Gated models: respond to both trigger and release
        if (trigger_flag) {
            osc->Strike();  // Start the sound
        }
        
        // For vowel models, timbre=formant, morph=vowel
        osc->set_parameters(timbre_in, morph_in);
        
        // Some models use the sync buffer for gate info
        memset(sync_buffer, trigger ? 0xFF : 0x00, size);
        
    } else {
        // Continuous oscillators: add simple AR envelope
        static float envelope_level = 0.0f;
        static float target_level = 0.0f;
        
        // Trigger sets target
        if (trigger_flag) {
            osc->Strike();  // Reset phase
            target_level = 1.0f;
        } else if (trigger_release) {
            target_level = 0.0f;
        }
        
        // Simple attack/release envelope
        const float attack_rate = 0.01f;   // Adjust: smaller = slower
        const float release_rate = 0.001f; // Adjust: smaller = slower
        
        if (target_level > envelope_level) {
            envelope_level += attack_rate;
            if (envelope_level > target_level) 
                envelope_level = target_level;
        } else {
            envelope_level -= release_rate;
            if (envelope_level < 0.0f) 
                envelope_level = 0.0f;
        }
        
        // Set oscillator parameters
        osc->set_parameters(timbre_in, morph_in);
        
        // Render audio
        osc->Render(sync_buffer, buffer, size);
        
        // Apply envelope to continuous oscillators
        for (int i = 0; i < size; i++) {
            buffer[i] = (int16_t)(buffer[i] * envelope_level);
        }
        
        return; // Skip the normal render below
    }
    
    // Render audio for percussive and gated models
    osc->Render(sync_buffer, buffer, size);
    
    // Optional: Apply additional VCA control based on trigger
    // This can add punch to percussive sounds or gate continuous ones
    if (!is_percussive && trigger_in < 0.5f) {
        // Fade out non-percussive sounds when gate is off
        static float gate_level = 1.0f;
        gate_level *= 0.99f; // Simple decay
        
        if (trigger_in > 0.5f) {
            gate_level = 1.0f; // Reset on new trigger
        }
        
        for (int i = 0; i < size; i++) {
            buffer[i] = (int16_t)(buffer[i] * gate_level);
        }
    }
}

// initialize macro osc
void initVoices() {

  voices[0].ratio = 48000.f / MI_SAMPLERATE;

  // init some params
  voices[0].pd.osc = new braids::MacroOscillator;
  memset(voices[0].pd.osc, 0, sizeof(*voices[0].pd.osc));

  voices[0].pd.osc->Init(48000.f);
  voices[0].pd.osc->set_pitch((48 << 7));
  voices[0].pd.osc->set_shape(braids::MACRO_OSC_SHAPE_VOWEL_FOF);


  voices[0].ws = new braids::SignatureWaveshaper;
  voices[0].ws->Init(123774);

  voices[0].quantizer = new braids::Quantizer;
  voices[0].quantizer->Init();
  voices[0].quantizer->Configure(braids::scales[0]);

  //unit->jitter_source.Init();

  memset(voices[0].pd.buffer, 0, sizeof(int16_t)*BLOCK_SIZE);
  memset(voices[0].pd.sync_buffer, 0, sizeof(voices[0].pd.sync_buffer));
  memset(voices[0].pd.samps, 0, sizeof(float)*BLOCK_SIZE);

  voices[0].last_trig = false;

  //voices[0].envelope = new braids::Envelope;
  //voices[0].envelope->Init();

  // get some samples initially
  updateBraidsAudio();

  /*
    // Initialize the sample rate converter
    int error;
    int converter = SRC_SINC_FASTEST;       //SRC_SINC_MEDIUM_QUALITY;


         // check resample flag
      int resamp = (int)IN0(5);
      CONSTRAIN(resamp, 0, 2);
      switch(resamp) {
          case 0:
              SETCALC(MiBraids_next);
              //Print("resamp: OFF\n");
              break;
          case 1:
              unit->pd.osc->Init(MI_SAMPLERATE);
              SETCALC(MiBraids_next_resamp);
              Print("MiBraids: internal sr: 96kHz - resamp: ON\n");
              break;
          case 2:
              SETCALC(MiBraids_next_reduc);
              Print("MiBraids: resamp: OFF, reduction: ON\n");
              break;
      }
  */
}
/*
const braids::SettingsData kInitSettings = {
  braids::MACRO_OSC_SHAPE_CSAW,

  braids::RESOLUTION_16_BIT,
  braids::SAMPLE_RATE_96K,

  0,  // AD->timbre
  true,  // Trig source auto trigger
  1,  // Trig delay
  false,  // Meta modulation

  braids::PITCH_RANGE_440,
  2,
  0,  // Quantizer is off
  false,
  false,
  false,

  2,  // Brightness
  0,  // AD attack
  5,  // AD decay
  0,  // AD->FM
  0,  // AD->COLOR
  0,  // AD->VCA
  0,  // Quantizer root

  50,
  15401,
  2048,

  { 0, 0 },
  { 32768, 32768 },
  "GREETINGS FROM MUTABLE INSTRUMENTS *EDIT ME*",
};
*/
