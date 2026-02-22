# Alternative : protocole OBD2 (comme Car Scanner)

## Contexte

- **Actuellement** : l’ESP32 reproduit le dialogue **SZ Viewer ↔ vLinker** en KWP2000 avec PIDs propriétaires (21A0, 21A2, 21A5, 21CD), puis décode les octets via un mapping reverse‑engineered (OCR + optimisation).
- **Car Scanner** lit très bien les données moteur K9K depuis le **même vLinker** en utilisant le **protocole OBD2** (standard ou PIDs constructeur documentés).

## Intérêt de passer à l’OBD2

| Critère | KWP2000 SZ (actuel) | OBD2 (Car Scanner–style) |
|--------|----------------------|---------------------------|
| **Documentation** | Propriétaire, reverse‑engineering | PIDs standard (ISO 15031, SAE J1979) ou doc constructeur |
| **Maintenance** | Décodeur fragile, MAE à gérer | Formules connues (ex. RPM = A×256+B, vitesse PID 010D) |
| **Compatibilité** | SZ Viewer uniquement | Car Scanner, Torque, etc. |
| **Implémentation** | Requêtes 21A0/21A2/21A5/21CD + `sz_decode.h` | Requêtes mode 01 / 09 + parsing OBD2 classique |

L’ESP32 utilise déjà le **PID 010D** (vitesse OBD2) en complément du décode SZ ; le K9K répond donc au moins partiellement en OBD2 via le vLinker.

## Recommandation

**Oui, implémenter l’ESP32 avec le protocole OBD2 (comme Car Scanner) est en général plus simple** si :

1. Les grandeurs dont tu as besoin sont disponibles via des **PIDs OBD2** (standard ou Renault/Nissan/K9K documentés).
2. Tu acceptes éventuellement un jeu de champs légèrement différent des 20 du SZ Viewer (certains champs SZ peuvent être propriétaires et absents en OBD2 pur).

## Étapes pratiques

1. **Lister les PIDs utilisés par Car Scanner pour le K9K**  
   - Avec Car Scanner connecté au vLinker, noter quels PIDs sont actifs (mode 01, 09, 21/22 selon l’app).  
   - Ou capturer le trafic Car Scanner ↔ vLinker (Bluetooth SPP) et relever les requêtes (ex. `01 0C`, `01 0D`, `01 05`, etc.).

2. **Faire un mapping PIDs → champs**  
   - Ex. : 010C → engine_rpm, 010D → speed_kmh, 0105 → engine_temp_c, 010F → intake_c, etc.  
   - **Fait** : voir [docs/OBD2_PIDS_K9K.md](OBD2_PIDS_K9K.md) (extrait du log `medias/car-scanner-log.txt`).

3. **Implémenter un mode “OBD2” sur l’ESP32**  
   - Envoyer des requêtes ELM327‑style (`01 0C`, `01 0D`, …) au vLinker.  
   - Parser les réponses selon le format OBD2 (nombre d’octets, formules standard).  
   - Remplir la même structure JSON MQTT (ou une version simplifiée) à partir de ces PIDs.  
   - Possibilité de garder le mode SZ en parallèle (option compile ou runtime) pour comparer ou pour les champs non disponibles en OBD2.

4. **Tester**  
   - Comparer les valeurs MQTT avec celles affichées par Car Scanner sur les mêmes PIDs.

En résumé : **utiliser l’OBD2 comme Car Scanner simplifie l’implémentation et la maintenance** ; la partie “reverse‑engineering” des pages SZ peut rester en secours ou être abandonnée si les PIDs OBD2 couvrent tes besoins.

---

## Car Scanner sur Mac M1 et capture du trafic vLinker

Car Scanner s'installe sur MacBook M1, mais l'accès au vLinker en **série Bluetooth** (`/dev/cu.vLinker` ou similaire) est souvent problématique : code d'appairage non reconnu, vLinker qui n'apparaît pas ou ne crée pas de port série. Pistes pour **vérifier la pertinence des questions/réponses** sans dépendre d'un port série fiable sur le Mac.

### Pourquoi c'est difficile sur macOS

- Le vLinker expose un **profil SPP** (Serial Port Profile) en Bluetooth classique ; sur macOS le port série (`/dev/cu.*`) n'apparaît qu'après appairage réussi.
- **Appairage** : certains dongles refusent des codes courants (1234, 0000) ou n'affichent pas de code.
- **Visibilité** : le vLinker peut être en BLE uniquement selon les modèles, ou le Mac ne propose le port série que dans des conditions précises.

### 1. Tenter d'obtenir `/dev/cu.*` sur le Mac

- **Oublier** le vLinker dans Réglages → Bluetooth, éteindre/rallumer le vLinker, ré-appairer. Codes à essayer : **1234**, **0000**, **1111**, **9999**.
- Après appairage : **Informations système** → Bluetooth ; dans Terminal : `ls /dev/cu.*` (nom parfois `vlinker`, `VLINKER`, ou générique).
- Lancer Car Scanner **après** l'appairage et vérifier si l'app propose Bluetooth/Série.
- Si un port apparaît : `screen /dev/cu.vLinker 38400` (ou 9600) pour voir les échanges ELM/OBD.

### 2. Contourner le Mac : capturer depuis un téléphone

- **Android** : connecter Car Scanner au vLinker. Activer **Journal HCI Bluetooth** (Paramètres développeur → "Journal des paquets Bluetooth"). Faire une session OBD, désactiver le journal, récupérer `bt_snoop.log` et l'ouvrir dans **Wireshark** pour analyser requêtes/réponses.
- **iPhone** : pas de snoop système ; utiliser Car Scanner en **référence visuelle** (noter les PIDs affichés) et comparer avec les réponses obtenues par l'ESP32.

### 3. Valider sans capturer : ESP32 + comparaison visuelle

- L'**ESP32** se connecte déjà au vLinker en BLE. Ajouter l'envoi des **PIDs OBD2 standard** (01 0C, 01 0D, 01 05, etc.), logger les réponses (Serial ou MQTT), et **comparer** avec ce que Car Scanner affiche au même moment sur le véhicule.
- Pas besoin de `/dev/cu.vLinker` : tu valides la pertinence en croisant logs ESP32 et affichage Car Scanner (sur téléphone ou Mac si l'app se connecte).

### 4. (Avancé) Pont BLE sur l'ESP32

- Le Mac (Car Scanner) se connecte en BLE à l'**ESP32** ; l'ESP32 se connecte au **vLinker** et relaie en **loggant** tout le trafic.
- Prérequis : que Car Scanner accepte de se connecter à un périphérique BLE autre que le vLinker. Plus lourd mais indépendant du port série Mac.

### Synthèse

| Objectif | Solution la plus simple |
|----------|-------------------------|
| Voir les requêtes/réponses Car Scanner ↔ vLinker | Android + BT HCI snoop + Wireshark, ou pont BLE sur ESP32 |
| Valider les PIDs OBD2 sans port série Mac | ESP32 envoie les PIDs, tu logues les réponses et tu compares avec l'affichage Car Scanner |
| Faire fonctionner Car Scanner sur Mac M1 | Ré-appairage (1234/0000), `ls /dev/cu.*` après connexion ; sinon Car Scanner sur téléphone + validation côté ESP32 |
