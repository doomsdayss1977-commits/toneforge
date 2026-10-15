# ROADMAP — ToneForge

Fonctions inspirées d'AmpliTube 5 (vues en rétro-ingénierie, cf. `C:\dev\amplitube_ui\RE_AMPLITUBE5.md`). Aucune ne demande de pédalier MIDI.

## En cours
### 1. Réglages par bloc (modèle : `.at5m` + Copier/Coller d'AmpliTube)
- [x] Clic droit sur un bloc ou bouton « Réglages ▾ » : copier / coller / sauver / charger
- [x] Presets de bloc dans `blocks/<bloc>/*.tfb` (bibliothèque), capture NAM/IR incluse pour Pédale, Ampli, Baffle
- [x] Test à l'oreille (2026-10-14)

### 3. Tap Tempo pour le delay (modèle : `MIDI.Tap_Tempo`)
- [x] Bouton « Tap » sur le bloc Delay : moyenne des derniers coups → « Temps » ; Temps affiché en ms
- [x] Test : taper sur un métronome à 120 BPM → écho à 500 ms

## À faire
### 2. Accordeur (modèle : emplacement 0 de la chaîne)
- Détection de hauteur (YIN) sur l'entrée, note + écart en cents, sortie coupée pendant l'accordage

### 4. Amplis en parallèle A/B (modèle : ChainSlot Amp A/B/C + Cab A/B/C)
- Deuxième capture NAM + IR en parallèle, mélange A/B ; attention CPU (×2)

### 5. Recherche par étiquettes (modèle : `Presets.db` : genre, artiste, micro, caractère)
- Étiquettes sur captures et presets, filtres dans le navigateur

### 6. Looper + métronome (modèle : `LOOP1/2`, `Looper_Metronome`)
- Enregistrer / superposer / lire une boucle, clic au tempo du Tap

### 7. Macro Min/Max/Courbe (modèle : `<MidiAssignments>` Min/Max/Curve)
- Une molette qui pilote plusieurs paramètres, chacun avec sa plage et sa courbe
