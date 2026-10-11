# Handoff — ToneForge, émulateur guitare NAM (2026-10-11)

## Objectif
Émulateur guitare perso (JUCE + NAM + Airwindows), autonome et VST3 dans REAPER, avec interface façon AmpliTube UI.

## État actuel
- Fait et validé par l'utilisateur : son NAM, chaîne Airwindows, 29 IR de baffle + interrupteur, nouvelle UI + sauvegarde des réglages, revue de code corrigée (f848594), VST3 fonctionnel dans REAPER.
- Fait, **pas encore écouté** : rééchantillonnage autour de NAM quand projet ≠ taux de la capture (98e4856). VST3 installé à jour (17:43:58).
- Fichiers clés :
  - `Source/PluginProcessor.cpp/.h` — chaîne gate → comp → entrée → NAM (rééchantillonné si besoin) → IR → EQ → chorus/delay/reverb → sortie ; état = paramètres + chemins model/ir
  - `Source/PluginEditor.cpp/.h` — UI (blocs à LED, navigateur captures/IR, vumètres), sauvegarde auto si changement
  - `Source/Effects.h` — liste `fxDefs()` des effets Airwindows ; `Source/aw/audioeffectx.h` — shim SDK VST2
  - `tools/t3k_bank.py` — Banque TONE3000 (API, jeton sauvegardé)
  - `models/`, `irs/` — bibliothèque ; `CMakeLists.txt`

## Ce qui a marché
- Build : `E:/VS2022/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe --build build --config Release --target ToneForge_Standalone ToneForge_VST3 --parallel` (jamais `/m`).
- Avant build : `taskkill //IM ToneForge.exe` **sans /F** (sinon réglages perdus).
- Installation VST3 : REAPER fermé, robocopy admin (UAC) vers `C:\Program Files\Common Files\VST3\ToneForge.vst3`, puis comparer l'horodatage.
- Rééchantillonnage : Lagrange JUCE, amorce de 12 échantillons ; simulation 20 000 blocs = 0 sous-alimentation.

## Ce qui n'a pas marché (et pourquoi)
- Réglages perdus : JUCE autonome ne sauvait qu'à la fermeture par X → sauvegarde auto via `StandalonePluginHolder::savePluginState`.
- `M_PI` non défini dans Airwindows → `_USE_MATH_DEFINES` dans le shim.

## Contraintes / décisions prises
- Bibliothèque = 1er dossier parent du plugin contenant `models/`, sinon `%APPDATA%\ToneForge` (jonctions `models`/`irs` → E:\Dev\ToneForge).
- Ne jamais saisir de code de connexion ni mot de passe pour l'utilisateur ; ne pas contourner la licence AmpliTube.
- Répondre en français, concis.

## Prochaines étapes
1. Faire écouter le rééchantillonnage : projet REAPER à 44,1 kHz, vérifier le bandeau « Capture à 48000 Hz, rééchantillonnée depuis 44100 Hz » et l'absence de craquements.
2. Proposer `/code-review` sur 98e4856.
