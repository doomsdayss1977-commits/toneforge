# Handoff — ToneForge, émulateur guitare NAM (2026-10-16)

## Objectif
Émulateur guitare perso (JUCE + NAM + Airwindows), autonome et VST3 dans REAPER, avec une interface « studio néon ».

## État actuel
- Fait et validé par l'utilisateur : son NAM, chaîne Airwindows, IR, VST3 dans REAPER, rééchantillonnage, ROADMAP AmpliTube 1-7 (ampli B, looper, métronome, macro…).
- UI « studio néon » terminée et testée :
  - a73ef7c : molettes néon et icônes vectorielles.
  - 17c608d : chaîne en cartes animées reliées par un câble lumineux.
  - 419d03c : polices Bahnschrift et Cascadia Mono, pastilles de marque, survol de la liste, vumètres LED avec crête maintenue.
  - 1601e4e : correctifs de la revue UI (9 sur 9).
- Fichiers clés :
  - `Source/PluginEditor.cpp/.h` contient l'interface.
    - `TfLook` (molettes, boutons, `getSliderLayout` qui place la valeur éditable au centre).
    - Helpers statiques `monoFont()` (Cascadia Mono, sinon Consolas) et `glow()` (halo en 3 passes).
    - `timerCallback` gère les vumètres, le câble (seulement si `isShowing()`) et le survol de la liste.
  - `Source/PluginProcessor.cpp/.h` contient la chaîne audio, l'ampli A/B et la macro.
  - `tools/t3k_bank.py` gère la Banque TONE3000.

## Ce qui a marché
- Build : `E:/VS2022/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe --build build --config Release --target ToneForge_Standalone ToneForge_VST3 --parallel`.
- Avant le build : `taskkill //IM ToneForge.exe` **sans /F**, sinon les réglages sont perdus.
- Installation du VST3 :
  1. Fermer REAPER.
  2. Copier avec robocopy en admin vers `C:\Program Files\Common Files\VST3\ToneForge.vst3`.
  3. Vérifier l'horodatage de la copie.
- Éditer les sources (en CRLF) avec un script Python dans le scratchpad, en écrivant avec `newline='\r\n'`. Les heredocs bash cassent sur les apostrophes.
- Capture d'écran de la fenêtre : PrintWindow (flag 2) en PowerShell, qui marche même si la fenêtre est couverte.

## Ce qui n'a pas marché (et pourquoi)
- La valeur des molettes était invisible parce que `renderBackground` imposait la couleur `ink`. La ligne a été retirée.
- `MouseEvent::eventType` n'existe pas. Le survol est maintenant suivi par le timer.

## Contraintes / décisions prises
- Répondre en français, concis, avec un diff minimal. Pas de nouvelle dépendance ni de téléchargement sans demander, d'où l'usage des polices système Windows.
- Ne jamais commiter `tags.json` ni `tools/__pycache__/t3k_bank.cpython-314.pyc`.
- Ne pas contourner la licence AmpliTube. Ne jamais saisir de mot de passe ni de code pour l'utilisateur. L'utilisateur n'a pas de pédalier MIDI.
- Trailer de commit : `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Prochaines étapes
Revues ouvertes corrigées (d601a25, 0929e26), ROADMAP/JOURNAL à jour (efa87d9), VST3 installé 2026-10-16 21:51.
1. Test utilisateur (ROADMAP §9) : clic droit sur une molette, recherche par étiquette, macro automatisée dans REAPER, projet à 44,1 kHz avec A/B.
2. Après le test : cocher ROADMAP §9, noter au JOURNAL, ajouter tout bug trouvé dans BUGS.md.
