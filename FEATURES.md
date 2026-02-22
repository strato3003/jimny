# Suivi des Features - OBD2CLOUD

Ce fichier documente toutes les features et améliorations ajoutées au projet, conformément aux règles de développement.

## Version 0.5.1 (non encore testée)

- Ajout du **protocole OBD2** (PIDs mode 01, style Car Scanner) en plus du protocole SZ. Défini par `USE_OBD2_PROTOCOL 1` (défaut). Fichiers : `obd2_decode.h`, docs `OBD2_ALTERNATIVE.md` et `OBD2_PIDS_K9K.md`. **À valider sur véhicule.**

## Version 0.4.11

### Corrections et améliorations

1. **LED: rythme de clignotement accéléré, phase OFF courte**
   - Périodes réduites et duty cycle augmenté (70–80 % ON) pour tous les patterns (WiFi, BLE, combiné, secours).
   - Overlay BLE: burst 120 ms, cycle 20 ms ON / 10 ms OFF. Overlay MQTT: pulse 70 ms ON.

## Version 0.4.10

### Corrections et améliorations

1. **Horodatage sur chaque JSON MQTT**
   - Ajout de `datetime` (NTP si dispo, sinon `UPTIME: ...`) sur `jimny/szviewer/raw` et `jimny/dtc` (succès et “vide/erreur”).

## Version 0.4.9

### Corrections et améliorations

1. **Priorité FSM: Internet (WiFi + NTP) avant BLE, suivi permanent du réseau**
   - Ordre: 1) Internet (WiFi + NTP) pour traces horodatées, 2) MQTT, 3) BLE.
   - NTP configuré dès que le WiFi est connecté, avant CONNECT_MQTT.
   - Si le WiFi tombe en CONNECT_BLE/INIT_ELM/POLL_SZ → retour à CONNECT_WIFI pour rétablir l'accès.

## Version 0.4.8

### Corrections et améliorations

1. **LED: clignotements d’activité (vLinker + MQTT) avec une seule LED**
   - Flash court à chaque échange BLE/ELM (TX ou NOTIFY) pour visualiser l’activité vLinker.
   - Flash/pulse à chaque publication MQTT (status, szviewer, dtc, raw).
   - Conservation de la règle “toujours une LED active quand l’ESP est alimenté” via pattern de base + secours.

## Version 0.4.7

### Corrections et améliorations

1. **`speed_kmh` fiable via PID standard 010D**
   - Lecture périodique de `010D` (OBD-II standard) et override de `speed_kmh` si valeur plausible (0–250 km/h).
   - Évite les offsets propriétaires incertains (valeurs nulles/0 en roulage).
2. **Ajout de `datetime` dans `jimny/szviewer`**
   - Les JSON envoyés sur `jimny/szviewer` incluent maintenant `datetime` (NTP si dispo, sinon `UPTIME: ...`).

## Version 0.4.6

### Corrections et améliorations

1. **engine_rpm : offset 14-15 et échelle 0.125**
   - RPM lu aux octets 14-15. Échelle 0.125 (÷8) pour coller au régime réel ~1500–1800 tr/min.

## Version 0.4.5

### Corrections et améliorations

1. **Flush MQTT après chaque publication**
   - Plusieurs appels à `mqtt.loop()` + court délai après chaque `publish` pour que le paquet TCP soit bien envoyé avant de continuer (évite que les messages restent en buffer côté ESP32).
   - Appliqué aux publications : `jimny/szviewer`, `jimny/szviewer/raw`, `jimny/status`, `jimny/dtc`.
2. **Rappel des topics à la connexion MQTT**
   - À la connexion au broker, log Serial : « Pour recevoir les messages: abonnez-vous à 'jimny/#' sur <broker>:<port> » pour vérifier broker et abonnement côté client.

## Version 0.4.4

### Corrections et améliorations

1. **Amélioration de la gestion MQTT et reconnexion automatique**
   - Reconnexion automatique toutes les 5 secondes si MQTT déconnecté
   - Heartbeat périodique (30s) pour maintenir la connexion MQTT
   - Logs détaillés des codes d'état MQTT (CONNECTION_TIMEOUT, CONNECT_FAILED, etc.)
   - Vérification régulière de la connexion même sans données à publier
   - Réduction de l'allocation FIFO de 30% à 20% pour laisser plus de mémoire à MQTT

## Version 0.4.3

### Corrections et améliorations

1. **Buffer FIFO dimensionné dynamiquement selon la mémoire disponible**
   - Calcul automatique de la taille maximale en fonction de la mémoire libre
   - Utilisation de 20% de la mémoire disponible pour le buffer FIFO
   - Limite minimale : 10 entrées (garantie de fonctionnement de base)
   - **Pas de limite maximale** : utilise toute la mémoire disponible si nécessaire
   - Allocation dynamique au démarrage (plus de limite fixe arbitraire)
   - Logs détaillés de la mémoire utilisée et de la capacité allouée

## Version 0.4.2

### Corrections et améliorations

1. **Buffer FIFO dimensionné dynamiquement selon la mémoire disponible**
   - Calcul automatique de la taille maximale en fonction de la mémoire libre
   - Utilisation de 30% de la mémoire disponible pour le buffer FIFO
   - Limites raisonnables : minimum 10 entrées, maximum 500 entrées
   - Allocation dynamique au démarrage (plus de limite fixe à 50)
   - Logs détaillés de la mémoire utilisée et de la capacité allouée

2. **Messages de statut MQTT enrichis avec informations de debug**
   - Ajout d'un objet `debug` dans tous les messages de statut
   - États WiFi, MQTT et BLE (OK/KO) inclus automatiquement
   - Informations WiFi : SSID, IP, RSSI (si connecté)
   - État du buffer FIFO : nombre de trames et capacité
   - État FSM actuel pour diagnostic
   - Logs Serial enrichis avec résumé des états

## Version 0.4.1

### Corrections et améliorations

1. **Adaptation XIAO ESP32S3 SENSE**
   - Correction : Pin 34 est INPUT ONLY, utilisation d'une seule LED (orange) avec patterns combinés
   - Documentation mise à jour pour refléter l'utilisation d'une LED unique

2. **Horodatage des messages de statut MQTT avec datetime lisible**
   - Ajout de datetime lisible (format: YYYY-MM-DD HH:MM:SS) via NTP
   - Configuration automatique de NTP dès la connexion WiFi
   - Fallback sur uptime si NTP n'est pas encore synchronisé
   - Format JSON avec `datetime`, `ts_ms` et `ts_s` pour compatibilité
   - Configuration du fuseau horaire (GMT offset) ajustable

3. **Amélioration du debug**
   - Logs détaillés pour le scan BLE (liste complète des devices avec noms et adresses)
   - Debug du buffer FIFO (état et nombre de trames)
   - Debug des publications MQTT avec datetime
   - Traces détaillées pour la recherche du vLinker (pattern matching)

## Version 0.4.0

### Features ajoutées

1. **Buffer FIFO pour collecte continue**
   - Buffer de 50 trames pour stocker les données collectées
   - Collecte continue même sans WiFi (priorité absolue)
   - Envoi MQTT conditionnel (seulement si WiFi disponible)
   - Reprise automatique de l'envoi dès que le WiFi revient
   - Pas de perte de données pendant les coupures réseau

2. **Support WiFi ouvert (sans password) avec gestion des portails captifs**
   - Détection automatique des réseaux ouverts lors du scan
   - Ajout automatique au WiFiMulti avec password vide
   - Connexion automatique aux réseaux ouverts disponibles
   - **Détection des portails captifs** : Vérification automatique après connexion
   - **Déconnexion automatique** : Si portail captif détecté et non contournable, déconnexion et recherche d'un autre réseau
   - **Blacklist temporaire** : Les réseaux avec portail captif sont blacklistés pendant 5 minutes pour éviter les boucles
   - Test via URLs standard (Google, Apple, Microsoft) pour détecter les redirections
   - Ignore automatiquement les réseaux blacklistés lors du scan

3. **Scan et liste des 5 meilleurs réseaux WiFi**
   - Scan automatique au démarrage
   - Tri par RSSI (signal le plus fort en premier)
   - Affichage des réseaux ouverts vs protégés
   - Ajout automatique des réseaux ouverts au WiFiMulti

4. **Scan et liste des 5 meilleurs devices BLE**
   - Scan et tri par RSSI
   - Identification automatique des devices vLinker
   - Affichage formaté avec nom, adresse MAC et RSSI

5. **Système de LEDs expressif amélioré**
   - Au moins une LED toujours allumée si l'ESP est sous tension
   - Patterns variables avec fréquences (100ms à 2000ms) et duty cycles (0-100%)
   - LED WiFi (orange) : États WiFi/MQTT
   - LED BLE (rouge) : États BLE/OBD
   - Garantie visuelle de fonctionnement

6. **Lecture des DTC (Diagnostic Trouble Codes)**
   - Lecture automatique via ELM327 (Mode 03)
   - Publication sur topic MQTT dédié (`jimny/dtc`)
   - Format JSON avec liste des codes et réponse brute
   - Lecture périodique (toutes les 30 secondes)

## Version 0.3.x

### Features ajoutées

1. **Décodage complet des 20 champs SZ Viewer**
   - Analyse OCR des frames pour identifier offsets et échelles
   - Décodage de tous les 20 champs depuis les pages hexadécimales
   - Gestion des conflits d'offsets avec détection automatique

2. **Détection "NO DATA" et réinitialisation automatique**
   - Détection quand le contact est coupé
   - Réinitialisation automatique de la connexion ELM
   - Gestion robuste des échecs d'initialisation

3. **Amélioration de la gestion ATFI**
   - Réessais multiples (jusqu'à 6 tentatives)
   - Vérification du succès de l'initialisation
   - Réinitialisation de la séquence de configuration entre tentatives

4. **Gestion des LEDs pour feedback visuel**
   - LED WiFi (orange) pour état réseau
   - LED BLE (rouge) pour état connexion OBD
   - Patterns de clignotement selon l'état

## Version 0.2.x

### Features ajoutées

1. **Simulation "au chaud" avec replay**
   - Sketch `sz-replay-mqtt.ino` pour tester sans BLE
   - Support données embarquées (PROGMEM) ou SPIFFS
   - Replay des trames capturées pour développement

2. **Synchronisation SZ Viewer ↔ trames**
   - Script Python pour synchroniser screencast et log
   - Extraction de frames vidéo
   - OCR pour extraction des valeurs affichées
