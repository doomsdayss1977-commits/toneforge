// Remplacement minimal du SDK VST2 pour compiler les effets Airwindows tels quels.
#pragma once
#define __audioeffect__
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

typedef int32_t VstInt32;
typedef void* audioMasterCallback;
enum VstPlugCategory { kPlugCategUnknown = 0, kPlugCategEffect = 1 };
enum { kVstMaxProgNameLen = 24, kVstMaxParamStrLen = 8, kVstMaxVendorStrLen = 64,
       kVstMaxProductStrLen = 64, kVstMaxEffectNameLen = 32 };

inline char* vst_strncpy(char* d, const char* s, size_t n) { std::strncpy(d, s, n); d[n] = 0; return d; }
inline void float2string(float v, char* t, VstInt32 n) { std::snprintf(t, (size_t) n + 1, "%.2f", v); }

class AudioEffect
{
public:
    AudioEffect(audioMasterCallback, VstInt32, VstInt32) {}
    virtual ~AudioEffect() = default;
    virtual void processReplacing(float** in, float** out, VstInt32 frames) = 0;
    virtual void setParameter(VstInt32, float) {}
    virtual float getParameter(VstInt32) { return 0.f; }
    virtual void getParameterName(VstInt32, char*) {}
    virtual void getParameterDisplay(VstInt32, char*) {}
    virtual void getParameterLabel(VstInt32, char*) {}
    float getSampleRate() const { return sampleRate; }
    void setSampleRate(float s) { sampleRate = s; }
    void setNumInputs(VstInt32) {}
    void setNumOutputs(VstInt32) {}
    void setUniqueID(VstInt32) {}
    void canProcessReplacing(bool = true) {}
    void canDoubleReplacing(bool = true) {}
    void programsAreChunks(bool = true) {}

protected:
    float sampleRate = 44100.f;
};

class AudioEffectX : public AudioEffect
{
public:
    using AudioEffect::AudioEffect;
};
