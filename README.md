# OBD2 in cloud

> **Note** : Pour le suivi détaillé des features et améliorations, voir [FEATURES.md](FEATURES.md)

## Contexte véhicule

- **Adaptateur**: vLinker (OBD2 Bluetooth)
- **Véhicule**: Suzuki Jimny DDIS 1.5 (2008)
- **Moteur**: Renault K9K (diesel)
- **Protocole**: KWP2000 (Keyword Protocol 2000) - PIDs propriétaires
- **PIDs utilisés**: 21A0, 21A2, 21A5, 21CD (PIDs étendus/propriétaires)

### Alternative : OBD2 (comme Car Scanner)

Car Scanner lit très bien les données K9K via le vLinker en **OBD2**. Utiliser ce protocole côté ESP32 est en général **plus simple** (PIDs documentés, pas de reverse-engineering). Voir [docs/OBD2_ALTERNATIVE.md](docs/OBD2_ALTERNATIVE.md) pour la démarche et les étapes (lister les PIDs Car Scanner → mapping → mode OBD2 sur l’ESP32).

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

### Capture pour MAE nulle (décodé = OCR)

Avec un log à la seconde et plusieurs frames par seconde, une même trame est réutilisée pour plusieurs images → la MAE ne peut pas être nulle partout. Pour viser **MAE = 0** :

- **MIM à la milliseconde** : utiliser le sniffer avec timestamps `[HH:MM:SS.mmm]` (ex. `recording/jimny_sniffer_ms.py` + `jimny_capture.log`), puis la synchro **sz_sync_ms** qui associe à chaque frame les dernières trames reçues à cet instant. Voir [recording/README.md](recording/README.md).
- **Spécification générale** : [docs/CAPTURE_SZ_ZERO_MAE.md](docs/CAPTURE_SZ_ZERO_MAE.md).

## Système de LEDs expressif

Le sketch `esp32/sz-mqtt/sz-mqtt.ino` utilise **une seule LED** (orange, pin 21) sur le XIAO ESP32S3 SENSE pour indiquer l'état du système de manière expressive :

### Principe
- **Garantie visuelle** : La LED est toujours allumée (fixe ou clignotante, fréquence min 1s) si l'ESP est sous tension
- **Patterns variables** : Fréquences (100ms à 2000ms) et duty cycles (0-100%) selon l'état
- **LED orange unique** : Affiche les états combinés WiFi/MQTT et BLE/OBD via des patterns intelligents
- **NOTE** : Le XIAO ESP32S3 SENSE n'a qu'une seule LED accessible (orange). Les patterns WiFi et BLE sont combinés sur cette LED unique.

### LED orange (unique) - Patterns combinés WiFi/MQTT + BLE/OBD

La LED orange affiche les états combinés selon la logique suivante :

| Situation | Pattern affiché | Signification |
|-----------|----------------|---------------|
| **WiFi + BLE actifs** | Pattern le plus rapide (priorité activité) | Les deux systèmes fonctionnent |
| **Seulement WiFi actif** | Pattern WiFi | WiFi/MQTT connecté, BLE inactif |
| **Seulement BLE actif** | Pattern BLE | BLE/OBD connecté, WiFi inactif |
| **Aucun actif** | Pattern de secours (1s, 50%) | Système en attente |

#### États WiFi/MQTT (quand actif seul ou combiné)

| État | Pattern | Signification |
|------|---------|---------------|
| **BOOT** | Clignotement lent 1s (50% duty) | Démarrage de l'ESP |
| **CONNECT_WIFI** | Clignotement moyen 500ms (30% duty) | Connexion WiFi en cours |
| **CONNECT_MQTT** | Clignotement rapide 250ms (50% duty) | Connexion MQTT en cours |
| **WiFi + MQTT OK** | Fixe allumé (100% duty) | Tout est connecté |
| **WiFi OK / MQTT KO** | Clignotement lent 1s (70% duty) | WiFi OK mais MQTT déconnecté |
| **WiFi KO** | Clignotement très lent 2s (20% duty) | WiFi déconnecté |

#### États BLE/OBD (quand actif seul ou combiné)

| État | Pattern | Signification |
|------|---------|---------------|
| **BOOT/WiFi/MQTT** | Éteint | Pas encore en mode BLE |
| **CONNECT_BLE** | Clignotement moyen 500ms (50% duty) | Scan/connexion BLE en cours |
| **INIT_ELM** | Clignotement rapide 250ms (50% duty) | Initialisation ELM327 |
| **POLL_SZ actif** | Clignotement très rapide 100ms (30% duty) | Polling OBD actif |
| **BLE connecté (stable)** | Fixe allumé (100% duty) | BLE connecté, stable |
| **BLE déconnecté** | Clignotement lent 1s (20% duty) | BLE déconnecté |

### Légende des patterns
- **Fixe** : LED allumée en continu
- **Clignotement très rapide** (100ms) : Activité intense
- **Clignotement rapide** (250ms) : Activité normale
- **Clignotement moyen** (500ms) : Connexion en cours
- **Clignotement lent** (1000ms) : État d'attente
- **Clignotement très lent** (2000ms) : Problème/erreur
- **Duty cycle faible** (20-30%) : État problématique ou activité minimale
- **Duty cycle moyen** (50%) : État normal
- **Duty cycle élevé** (70-100%) : État optimal

## Simulation "au chaud" (sans BLE) : replay MQTT

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