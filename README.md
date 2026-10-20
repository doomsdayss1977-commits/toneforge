# ToneForge

Émulateur guitare perso : JUCE + NAM (captures d'amplis) + Airwindows + IR. Autonome (`ToneForge.exe`) et VST3 dans REAPER, interface « studio néon ».

## Aide-mémoire

**Fermer ToneForge avant de compiler, sans /F (sinon les réglages sont perdus) :**
```
taskkill //IM ToneForge.exe
```

Compiler :
```
E:/VS2022/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe --build build --config Release --target ToneForge_Standalone ToneForge_VST3 --parallel
```

Installer le VST3 :
1. Fermer REAPER.
2. Copier en admin (robocopy) `build\ToneForge_artefacts\Release\VST3\ToneForge.vst3` vers `C:\Program Files\Common Files\VST3\ToneForge.vst3`.
3. Vérifier l'horodatage de `Contents\x86_64-win\ToneForge.vst3` (une copie sans admin échoue en silence).

Banque TONE3000 : `tools\Banque TONE3000.bat` (`tools/t3k_bank.py`).

Le studio UE5 a besoin de `ToneForge.exe` lancé (raccourci bureau), sinon il est vide.

## Structure

- `Source/PluginProcessor.*` : chaîne audio, ampli A/B, macro, rééchantillonnage.
- `Source/PluginEditor.*` : interface (`TfLook`, molettes, cartes de la chaîne, vumètres).
- `models/`, `irs/`, `pedals/` : captures NAM, réponses impulsionnelles, pédales.
- `third_party/` : NeuralAmpModelerCore, RTNeural, airwindows, BYOD.
- `tools/` : Banque TONE3000, test OSC.

## Règles

- Sources en CRLF : les éditer par script Python (`newline='\r\n'`).
- Ne jamais commiter `tags.json`, `tools/__pycache__/`, ni `tools/t3k_token.json`.
- Pas de nouvelle dépendance sans demander ; ne pas contourner la licence AmpliTube.

Suivi : [ROADMAP.md](ROADMAP.md) · [JOURNAL.md](JOURNAL.md) · [BUGS.md](BUGS.md) · reprise : [HANDOFF.md](HANDOFF.md).
