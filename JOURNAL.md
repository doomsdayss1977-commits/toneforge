# JOURNAL — ToneForge

## 2026-10-16
- Revue précédente soldée (d601a25) : alignement A/B mesuré (14,86 échantillons à 44,1 kHz = théorie 2 + 14/ratio), `stateLock` pour la macro, gestes d'automation, clic droit sans glissé (`KnobSlider`), clé d'étiquette calculée au balayage.
- Revue 98e4856 soldée (0929e26) : latence déclarée depuis le timer (pédale + max(A, B) quand B joue), elliptique −70 dB à Nyquist avant l'étage qui baisse le taux, amorce `max(12, ceil(3·ratio)+8)`.
- Banc de test temporaire (variable `TF_SR` + journal dans `build/`) : 0 sous-alimentation, latence 9,3 / 15,4 / 33,0 échantillons à 12 / 44,1 / 96 kHz. Retiré avant commit.
- VST3 installé (robocopy admin, horodatage vérifié).
- Piège : les interpolateurs `Lagrange` de JUCE ne sont pas copiables → copie de `Resampler` champ par champ.

## 2026-10-15
- Ampli B, étiquettes, looper + métronome, macro : ROADMAP AmpliTube 1-7 terminée (b3f083a → 0710793, revue 9e2b6b3).
- Interface « studio néon » en 3 étapes + revue (a73ef7c, 17c608d, 419d03c, 1601e4e).
- Piège : `renderBackground` imposait la couleur `ink` → valeur des molettes invisible ; `MouseEvent::eventType` n'existe pas → survol suivi par le timer.
