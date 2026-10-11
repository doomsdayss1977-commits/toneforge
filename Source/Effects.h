#pragma once
// Effets Airwindows (MIT) compilés tels quels via le shim aw/audioeffectx.h.
#include <memory>
#include <vector>
#include "aw/audioeffectx.h"

#define TF_AW(name) AudioEffect* createEffectInstance_##name(audioMasterCallback);
TF_AW(Pressure5)
TF_AW(ChorusEnsemble)
TF_AW(TapeDelay2)
TF_AW(Galactic3)
#undef TF_AW

struct FxParam { const char* label; float def; };
struct FxDef
{
    const char* key;    // préfixe des ID de paramètres
    const char* title;
    bool preAmp;        // avant la capture NAM (mono) ou après (stéréo)
    AudioEffect* (*make)(audioMasterCallback);
    std::vector<FxParam> params;   // dans l'ordre kParamA, kParamB...
};

inline const std::vector<FxDef>& fxDefs()
{
    static const std::vector<FxDef> d {
        { "comp", "Compresseur (Pressure5)", true, createEffectInstance_Pressure5,
          { { "Pression", 0.3f }, { "Vitesse", 0.25f }, { "Douceur", 1.f }, { "Griffe", 0.5f },
            { "Sortie", 0.5f }, { "Mix", 1.f } } },
        { "chorus", "Chorus (ChorusEnsemble)", false, createEffectInstance_ChorusEnsemble,
          { { "Vitesse", 0.5f }, { "Profondeur", 0.5f }, { "Mix", 0.5f } } },
        { "delay", "Delay à bande (TapeDelay2)", false, createEffectInstance_TapeDelay2,
          { { "Temps", 0.5f }, { "Répétitions", 0.3f }, { "Fréquence", 0.5f }, { "Résonance", 0.f },
            { "Flutter", 0.1f }, { "Mix", 0.3f } } },
        { "reverb", "Reverb (Galactic3)", false, createEffectInstance_Galactic3,
          { { "Remplace", 0.5f }, { "Brillance", 0.5f }, { "Désaccord", 0.5f }, { "Définition", 1.f },
            { "Taille", 0.8f }, { "Mix", 0.25f } } },
    };
    return d;
}
