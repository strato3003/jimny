# Capture MIM à la milliseconde + screencast

Ce dossier contient une capture **MIM avec timestamps à la ms** et un **screencast** (chronomètre vert en haut à gauche + écran SZ Viewer), pour aligner précisément trames et frames et viser une MAE nulle (décodé = OCR).

## Fichiers

- **jimny_capture.log** : log `[HH:MM:SS.mmm] SEND: / RECV:`
- **jimny_sniffer_ms.py** : script de capture (timestamps ms)
- **2026-02-21_17-50-47.mp4** : screencast (chrono + SZ Viewer)
- **sz_sync_ms.jsonl** : synchro frame ↔ raw (généré par `sz_sync_ms.py`)

## Workflow

### 1. Point d’ancrage

Choisir **une** frame dont tu connais l’instant log (en lisant le chrono à l’écran ou en repérant un moment dans le log). Exemple : frame 1 affiche le chrono 17:36:44.0 et le premier cycle 21A0/21A2/21A5/21CD dans le log est à 17:36:44.038 → utiliser `--anchor-frame 1 --anchor-log 17:36:44.038`.

### 2. Synchro (trames ms ↔ frames)

Le **screencast commence à 17:50:46**. Pour ne traiter que la fenêtre **17:52:51 → 17:53:08** (accélération ~15→46 km/h, régime jusqu’à ~3154 rpm) :

- **17:52:51** = 125 s après le début de la vidéo (17:50:46 + 2 min 5 s).
- **Durée** : 17 s (jusqu’à 17:53:08).

```bash
# Depuis la racine du repo — extraire uniquement le segment 17:52:51–17:53:08 (screencast 30 fps)
python3 tools/sz_sync_ms.py \
  --log recording/jimny_capture.log \
  --video recording/2026-02-21_17-50-47.mp4 \
  --fps 30 \
  --anchor-frame 1 \
  --anchor-log 17:52:51 \
  --video-start-sec 125 \
  --video-duration 17 \
  --out recording/sz_sync_ms_window.jsonl
```

Cela extrait **510 frames** (17 s × 30 fps) dans `recording/2026-02-21_17-50-47_frames_125_17/` et écrit `recording/sz_sync_ms_window.jsonl` (510 lignes).

**Alternatives :**
- **--start-log / --end-log** : si tu extrais toute la vidéo, ne garder que les frames dont l’instant log est dans [start-log, end-log].
- **--limit N** : n’écrire que les N premières frames (pour tester).

### 3. OCR (remplir `values`)

```bash
# Pour la fenêtre 17:52:51–17:53:08 (510 frames à 30 fps)
python3 tools/sz_ocr.py --in recording/sz_sync_ms_window.jsonl --out recording/sz_sync_ms_window_ocr.jsonl
```

### 4. Optimiser le décodeur

```bash
python3 tools/sz_decode_from_ocr_jsonl.py recording/sz_sync_ms_window_ocr.jsonl --update-decode --write-mapping
python3 tools/sz_compare_decode_vs_ocr.py recording/sz_sync_ms_window_ocr.jsonl
```

Avec un ancrage correct et des timestamps ms, chaque frame a les trames reçues **à ou juste avant** son instant → décodé et OCR décrivent le même état → MAE proche de 0.
