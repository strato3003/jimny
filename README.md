# OBD2 in cloud
## prompts agent
```
proposes moi un code pour esp32 en t'inspirant de @esp32/vlinker-gw.ino , capable d'envoyer une sequence json sur mqtt qui reprend l'ensemble des 20 donnees affichees sur la fenetre en noir du programme "SZ Viewer" montré sur cette image attachee et qui proviennent du dialogue entamé entre le SZ Viewer et le vLinker (boitier OBD en Bluetooth) et capturé en MIM via un prog python.
```

## Synchro SZ Viewer ↔ trames (screencast + log)

### Objectif

Générer un fichier `medias/sz_sync.jsonl` qui associe **chaque frame** du screencast (`medias/frames/frame_*.png`) à:
- la seconde correspondante du log (`medias/trames.log`)
- les pages brutes `21A0/21A2/21A5/21CD` vues à cette seconde
- une structure `values` (les 20 champs SZ Viewer) initialisée à `null`

### Génération

Pré-requis: Python 3.

Le script demande un **point d’ancrage**: une frame dont tu connais l’heure affichée (HH:MM:SS) et qui correspond aux timestamps du log.

Exemple (dans `frame_00001.png` on lit `17:36:53`) :

```bash
python3 tools/sz_sync.py --anchor-hhmmss 17:36:53 --fps 2
```

### Étape suivante (décodage)

Remplir progressivement `values` (manuellement ou via OCR) puis en déduire les offsets/échelles à implémenter dans `esp32/sz-mqtt.ino` (`decodeSzFromPages()`).

## Simulation “au chaud” (sans BLE) : replay MQTT

Le sketch `esp32/sz-replay-mqtt/sz-replay-mqtt.ino` permet de simuler la gateway en publiant des JSON sur MQTT.

### Mode embarqué (recommandé, sans SPIFFS)

**Avantage**: Pas besoin d’uploader de fichier SPIFFS, tout est compilé dans le sketch.

1. **Génère le fichier C** à partir de `sz_sync_ocr.jsonl`:
   ```bash
   python3 tools/sz_embed.py medias/sz_sync_ocr.jsonl
   ```
   Cela crée `esp32/sz-replay-mqtt/sz_data.h` automatiquement.

2. **Ouvre le sketch** dans Arduino IDE (`esp32/sz-replay-mqtt/sz-replay-mqtt.ino`)

3. **Compile et upload** normalement (menu **Sketch → Upload**)

4. L’ESP32 publie sur `jimny/szviewer` avec `values` (20 champs OCR) + `raw`

### Mode SPIFFS (alternative)

Si tu préfères utiliser SPIFFS (nécessite un outil d’upload LittleFS/SPIFFS):

- Définis `#undef USE_EMBEDDED_DATA` dans le sketch
- Flashe `medias/sz_sync_ocr.jsonl` dans SPIFFS sous `/sz_sync_ocr.jsonl`
- Ou flashe `medias/trames.log` dans SPIFFS sous `/trames.log` (raw-only)