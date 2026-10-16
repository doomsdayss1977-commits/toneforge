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

### 2. Accordeur (modèle : emplacement 0 de la chaîne)
- [x] Bouton « ♪ Accordeur » : son coupé, note + écart en cents + aiguille sur l'écran LCD (vert ±3 ¢)
- [x] YIN sur l'entrée brute, 28-1400 Hz (si grave de basse 5 cordes) ; test synthétique : 30,87 / 41,2 / 110 Hz exacts, 988 Hz à +1,4 ¢
- [x] Test avec la guitare et la basse (2026-10-14)

### 4. Amplis en parallèle A/B (modèle : ChainSlot Amp A/B/C + Cab A/B/C) 
- [x] Processeur (2026-10-14, pas compilé) : slot `ampB` + `cabB`, params `ampb_on`/`ampb_in`/`ampb_out`/`ab_mix` (fondu linéaire), `loadModel(f, false, true)`, `loadIR(f, true)`, `loadedFile(3/4)`, état `modelB`/`irB`, `modelWarning(int kind)`
- [x] Interface (2026-10-15, compilé) : bloc « Ampli B » (kAmpB = 5, après Baffle ; onId `ampb_on`, réglages Entrée/Sortie/Mélange) ; quand il est sélectionné, onglets Amplis/Baffles chargent dans B (`target(tab)` → 3/4) aux appels de `current(tab)`, `load(…, tab)`, `pick()` ; `blockOf(3|4)` = kAmpB ; `refreshLabels` et presets de bloc (fichier + IR B)
- [x] Compiler
- [x] Test à l'oreille (2026-10-15)

## À faire

### 5. Recherche par étiquettes (modèle : `Presets.db` : genre, artiste, micro, caractère)
- [x] Clic droit sur une capture : cocher des étiquettes (genre, caractère, Favori…) ou en créer ; stockées dans `tags.json` de la bibliothèque (2026-10-15, compilé)
- [x] Affichées en #cyan sur la ligne ; la recherche les trouve ; « Filtrer par étiquette » remplit la recherche
- [x] Test (2026-10-15)

### 6. Looper + métronome (modèle : `LOOP1/2`, `Looper_Metronome`)
- Enregistrer / superposer / lire une boucle, clic au tempo du Tap

### 7. Macro Min/Max/Courbe (modèle : `<MidiAssignments>` Min/Max/Curve)
- Une molette qui pilote plusieurs paramètres, chacun avec sa plage et sa courbe
