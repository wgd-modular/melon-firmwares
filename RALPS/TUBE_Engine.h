#pragma once
#include <math.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

class TUBEEngine {
public:
    float ks[2048];
    float l_z, dcY, dcX, p_z;
    uint32_t wHead;
    uint32_t phMod;

    TUBEEngine() { reset(); }

    void reset() {
        for(int i=0; i<2048; i++) ks[i] = 0;
        l_z = 0; dcY = 0; dcX = 0; p_z = 0;
        wHead = 0; phMod = 0;
    }

    float renderSample(float env, float P[12], float invFs, float randomValue) {
        float envE = env * env;
        
        // 1. Exciter (P1: Body/Noise, P4: Pressure, P9: Pop)
        float exciter = 0.0f;
        static const int excLen = 300;
        if(wHead < excLen) {
            float eEnv = (1.0f - (float)wHead/(float)excLen);
            float body = sinf((float)wHead * M_PI / (float)excLen) * 0.8f;
            float noise = randomValue * eEnv * 0.6f;
            p_z = p_z + (0.05f + (1.0f - P[4]) * 0.5f) * (noise - p_z);
            float pop = (wHead < 50) ? (randomValue * P[9] * 1.5f) : 0.0f;
            exciter = (body * P[1] + p_z * (1.0f - P[1]) + pop) * eEnv * 2.0f;
        }

        // 2. TUBE Length & Modulation (P0: Pitch, P7: Mod Rate, P8: Mod Depth)
        float modHz = 0.1f + P[7] * 10.0f;
        phMod += (uint32_t)(modHz * invFs * 4294967296.0f);
        float pitchMod = 1.0f + sinf((float)phMod * 2.3283064365e-10f * 2.0f * M_PI) * P[8] * 0.02f;
        
        float baseFreq = 25.0f + (P[0] * P[0] * P[0]) * 1200.0f;
        float delayTime = (1.0f/invFs) / (baseFreq * pitchMod);
        if(delayTime < 2.0f) delayTime = 2.0f; if(delayTime > 2040.0f) delayTime = 2040.0f;

        // 3. Physical Model (P2: Damping, P10: Second Tap)
        float stiffness = 0.02f + P[3] * 0.35f; 
        float damping = 0.94f + P[2] * 0.059f;

        float rPos = (float)wHead - delayTime; 
        while(rPos < 0.0f) rPos += 2048.0f;
        int rInt = (int)rPos; float rFrac = rPos - (float)rInt;
        float delayed = ks[rInt] * (1.0f - rFrac) + ks[(rInt + 1) % 2048] * rFrac;
        
        // Second Tap (P10)
        float tap2Pos = rPos - (delayTime * 0.5f * P[10]);
        while(tap2Pos < 0.0f) tap2Pos += 2048.0f;
        int rInt2 = (int)tap2Pos;
        delayed = delayed * 0.7f + ks[rInt2] * 0.3f * P[10];

        l_z = l_z + stiffness * (delayed - l_z);
        float sig = exciter + l_z * damping;

        // 4. Geometry & Saturation (P3: Drive, P11: Diffusion/Symmetry)
        if(P[3] > 0.01f) {
            float drive = 1.0f + P[3] * 6.0f;
            sig = tanhf(sig * drive);
        }

        ks[wHead % 2048] = sig;
        wHead++;

        // 5. DC Block & Output (P5: Reflection/Tone)
        float dcOut = sig - dcX + 0.995f * dcY;
        dcY = dcOut; dcX = sig;

        float final = (dcOut * (1.0f - P[5]) + (exciter * P[5] * 2.0f)) * 2.5f;
        return final * envE;
    }
};
