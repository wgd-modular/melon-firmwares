#pragma once
#include <math.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

class BOUNCEEngine {
public:
    float vEnv[4], vPh[4], vLP[4], vBP[4], vJit[4];
    float interval, nextPing;
    float drift;
    uint32_t sampleCount;

    BOUNCEEngine() { reset(); }

    void reset() {
        for(int i=0; i<4; i++) { vEnv[i]=0; vPh[i]=0; vLP[i]=0; vBP[i]=0; vJit[i]=1.0f; }
        interval = 0; nextPing = 0; sampleCount = 0;
        drift = 0.96f + (float)rand()/(float)RAND_MAX * 0.08f;
    }

    inline float sin_u32(uint32_t ph) {
        float x = (float)ph * 2.3283064365e-10f; 
        return sinf(x * 2.0f * M_PI);
    }

    float renderSample(float env, float P[12], float invFs, float randomValue) {
        if(sampleCount == 0) {
            float slow = 2.0f + P[1] * 40.0f;
            float fast = 60.0f + P[1] * 240.0f;
            float initHz = (P[2] < 0.5f) ? (slow + P[2]*2.0f*(10.0f-slow)) : (10.0f + (P[2]-0.5f)*2.0f*(fast-10.0f));
            interval = (1.0f/invFs) / initHz;
            nextPing = 0;
        }
        sampleCount++;

        float gravity = 1.0f - (P[2] - 0.5f) * 0.25f; 
        float pDamping = 0.997f - (1.0f - P[5]) * 0.035f; 
        float seqBaseFreq = (25.0f + P[0]*P[0]*1200.0f) * drift;
        float seqPitchDrop = 1.0f - (1.0f - env) * P[3] * 0.6f;

        // Scheduler: Check for sub-trigger
        if(nextPing <= 0.0f && env > 0.03f) {
            for(int v=0; v<4; v++) {
                if(vEnv[v] < 0.01f) {
                    vEnv[v] = 1.0f; vPh[v] = 0; vLP[v] = 0; vBP[v] = 0;
                    vJit[v] = 0.994f + (float)rand()/(float)RAND_MAX * 0.012f * (1.0f + P[10]*5.0f);
                    break;
                }
            }
            float elasticity = 0.5f + P[9] * 0.49f;
            nextPing = interval; interval *= (gravity * elasticity);
            if(interval < (25.0f + P[8]*1000.0f)) interval = (25.0f + P[8]*1000.0f); // P8: Floor Limit
        }
        nextPing -= 1.0f;

        float out = 0.0f;
        for(int v=0; v<4; v++) {
            if(vEnv[v] > 0.005f) {
                float instFreq = seqBaseFreq * seqPitchDrop * (0.94f + vEnv[v] * 0.06f) * vJit[v];
                
                // Base OSC (P6: Weight/Body)
                uint32_t ph_u = (uint32_t)vPh[v];
                ph_u += (uint32_t)(instFreq * invFs * 4294967296.0f);
                vPh[v] = (float)ph_u;

                float osc = sin_u32(ph_u) + 0.4f * sin_u32(ph_u >> 1) * P[6];
                float noise = randomValue * (sampleCount < 240 ? 0.3f : 0.0f) * P[7];
                float input = (osc + noise) * vEnv[v] * vEnv[v];

                // Filter (P7: Hardness/Tone)
                float gf = 2.0f * sinf(M_PI * (instFreq * (1.0f + P[7]*4.0f)) * invFs);
                if(gf > 1.4f) gf = 1.4f;
                float gq = 1.0f / (1.5f + (1.0f-P[7]) * 18.0f);

                float nEx = input - (vBP[v] * gq) - vLP[v];
                vBP[v] += nEx * gf; vLP[v] += vBP[v] * gf;
                
                out += vBP[v];
                vEnv[v] *= pDamping;
            }
        }

        float s = tanhf(out * 2.8f * (1.0f + P[11]*2.0f));
        return s * env * env;
    }
};
