#pragma once
#include <math.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

class MULTIEngine {
public:
    float ph[5];
    float lastS;
    
    MULTIEngine() { reset(); }
    void reset() { for(int i=0; i<5; i++) ph[i] = 0.0f; lastS = 0.0f; }
    
    float renderSample(float env, float P[12], float invFs, float randomValue) {
        float envE = env * env;
        float baseFreq = 25.0f + (P[0] * P[0] * P[0]) * 1500.0f;
        
        // 1. Ratios (P2: Mode)
        int ratIdx = (int)(P[2] * 7.99f); 
        float r[5] = {0, 1.0f, 1.0f, 1.0f, 1.0f};
        switch(ratIdx) {
            case 0: r[2]=1.0f; r[3]=1.0f; r[4]=1.0f; break; 
            case 1: r[2]=2.0f; r[3]=3.0f; r[4]=4.0f; break; 
            case 2: r[2]=0.5f; r[3]=2.0f; r[4]=4.0f; break; 
            case 3: r[2]=1.414f; r[3]=1.732f; r[4]=2.236f; break; 
            case 4: r[2]=4.02f; r[3]=3.01f; r[4]=1.005f; break; 
            case 5: r[2]=0.75f; r[3]=1.25f; r[4]=1.5f; break; 
            case 6: r[2]=8.0f; r[3]=0.25f; r[4]=16.0f; break; 
            case 7: r[2]=15.1f; r[3]=1.01f; r[4]=0.505f; break; 
        }
        
        // --- 2. DETUNE: Moved to P6 (Shift-Poti 3) ---
        r[2] *= (1.0f + P[6] * 0.08f);
        r[3] *= (1.0f - P[6] * 0.04f);
        r[4] *= (1.0f + P[6] * 0.12f);

        for(int k=1; k<=4; k++) {
            ph[k] += baseFreq * r[k] * invFs;
            if(ph[k] >= 1.0f) ph[k] -= 1.0f;
        }

        // 3. TIMBRE: FM Index (P1)
        float mIdx = P[1] * 6.0f;
        float sI = P[3] * 4.0f; 
        
        // --- 4. OP DECAYS: Moved main FM Decay to P5 (Main-Poti 3 / CVable) ---
        float opDecays[4];
        opDecays[0] = m_fast_powf(env, 1.0f + P[5] * 5.0f); // Expressive CV
        opDecays[1] = m_fast_powf(env, 1.0f + P[7] * 5.0f);
        opDecays[2] = m_fast_powf(env, 1.0f + P[8] * 5.0f);
        opDecays[3] = m_fast_powf(env, 1.0f + P[10] * 8.0f); 

        // 5. Algorithms (P4)
        int alg = (int)(P[4] * 7.99f);
        float s = 0.0f;
        float o1=0, o2=0, o3=0, o4=0;
        float phA = ph[1]*6.2831853f, phB = ph[2]*6.2831853f, phC = ph[3]*6.2831853f, phD = ph[4]*6.2831853f;

        auto fmSin = [&](float phase, float mod) { return sinf(phase + mod * mIdx); };

        switch(alg) {
            case 0: 
                o4 = sinf(phD + lastS * sI); lastS = o4;
                o3 = fmSin(phC, o4 * opDecays[2]);
                o2 = fmSin(phB, o3 * opDecays[1]);
                s  = fmSin(phA, o2 * opDecays[0]);
                break;
            case 1: 
                o4 = sinf(phD + lastS * sI); lastS = o4;
                o3 = sinf(phC); o2 = sinf(phB);
                s  = fmSin(phA, (o4 + o3 + o2) * 0.33f * opDecays[0]);
                break;
            case 2: 
                o4 = sinf(phD); o3 = sinf(phC);
                o2 = fmSin(phB, o4 * opDecays[1]);
                o1 = fmSin(phA, o3 * opDecays[0]);
                s = (o1 + o2) * 0.5f;
                break;
            case 3: 
                o4 = sinf(phD); o3 = sinf(phC);
                o2 = fmSin(phB, (o4+o3) * 0.5f * opDecays[1]);
                s = fmSin(phA, o2 * opDecays[0]);
                break;
            case 4: 
                o4 = sinf(phD);
                o3 = fmSin(phC, o4 * opDecays[2]);
                o2 = fmSin(phB, o4 * opDecays[1]);
                o1 = fmSin(phA, o4 * opDecays[0]);
                s = (o1 + o2 + o3) * 0.33f;
                break;
            case 5: 
                o4 = randomValue * P[9]; 
                o3 = sinf(phC); o2 = sinf(phB);
                s = fmSin(phA, (o4 + o3 + o2) * opDecays[0]);
                break;
            case 6: 
                o4 = sinf(phD); o3 = sinf(phC);
                s = sinf(phA) * fmSin(phB, o4) * fmSin(phC, o3);
                break;
            case 7: 
                s = (sinf(phA) + sinf(phB) + sinf(phC) + sinf(phD)) * 0.25f;
                break;
        }

        float drive = 1.0f + P[11] * 15.0f;
        s = tanhf(s * drive);
        // Use linear env instead of envE (which is ^2) because the operators already have heavy decay (powf)
        return s * env;
    }
};
