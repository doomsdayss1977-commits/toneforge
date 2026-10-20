# Bugs

## Ouverts

_Aucun connu._ Test utilisateur en attente : ROADMAP §9.

## Résolus

| Date | Symptôme | Cause | Correctif |
|---|---|---|---|
| 2026-10-11 | Aucun modèle NAM ne charge | Architectures NAM non enregistrées depuis la lib statique | NAM lié hors de la lib statique |
| 2026-10-15 | Valeur des molettes invisible | `renderBackground` imposait la couleur `ink` | Ligne retirée |
| 2026-10-15 | Survol de la liste non suivi | `MouseEvent::eventType` n'existe pas | Survol suivi par le timer |
| 2026-10-16 | Filtrage en peigne avec ampli A + B | Retards de rééchantillonnage différents | Retard mesuré au chargement, la voie la moins retardée attend l'autre (d601a25) |
| 2026-10-16 | Course possible sur l'arbre Macro | Modifié sans verrou pendant `replaceState` | `stateLock` (d601a25) |
| 2026-10-16 | Automation de la macro non enregistrée | Cibles écrites sans gestes | Gestes de changement (d601a25) |
| 2026-10-16 | Clic droit sur une molette la tourne | Le clic droit lançait un glissé | `KnobSlider` (d601a25) |
| 2026-10-16 | Repliement, latence non compensée, sous-alimentation si ratio > 2,3 | Pas d'anti-repliement, pas de `setLatencySamples`, amorce fixe de 12 | Elliptique −70 dB, latence déclarée, amorce `max(12, ceil(3·ratio)+8)` (0929e26) |
| 2026-10-16 | Erreur de compilation à la copie de `Resampler` | Interpolateurs `Lagrange` de JUCE non copiables | Copie champ par champ |
