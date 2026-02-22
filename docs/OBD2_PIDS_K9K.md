# PIDs OBD2 utilisés par Car Scanner pour le K9K (Suzuki Jimny)

Source : log Car Scanner ↔ vLinker MC-IOS (BLE), profil **Suzuki OBD-II / EOBD**, véhicule Jimny DDIS 1.5 (moteur K9K). Fichier `medias/car-scanner-log.txt`.

## Connexion et init (extrait du log)

- **Appareil** : vLinker MC-IOS (Bluetooth LE)
- **Profil** : Suzuki OBD-II / EOBD
- **Protocole détecté** : KWP (ATDPN → 5), format ELM = KWP
- **Séquence init** : ATZ, ATE0, ATD0, ATH1, ATSP0, ATST64, puis 0100 / 0120 pour interroger les PIDs supportés

Les réponses sont au format KWP : préfixe type `83F17A41` ou `84F17A41` (longueur, adresse ECU, 7A = réponse mode 01, 41 = mode 01), puis **PID (1 octet)** et **données** (2 ou 4 octets selon le PID).

## PIDs mode 01 (données en temps réel) utilisés

| Requête ELM | PID SAE | Description (SAE J1979) | Exemple réponse (hex) | Formule valeur |
|-------------|---------|--------------------------|------------------------|----------------|
| 0100 | 00 | Supported PIDs 01-20 | 86F17A41009839801194 | bitmap |
| 0120 | 20 | Supported PIDs 21-40 | 86F17A4120A0000000F2 | bitmap |
| 0101 | 01 | Monitor status (DTC) | 86F17A410103068000BC | - |
| 0104 | 04 | Calculated engine load | 83F17A41040033 | A * 100/255 % |
| 0105 | 05 | Coolant temp | 83F17A4105588C | A - 40 °C |
| 010B | 0B | Intake manifold abs. pressure | 83F17A410B659F | A kPa |
| 010C | 0C | Engine RPM | 84F17A410C00003C | (A*256+B)/4 tr/min |
| 010D | 0D | Vehicle speed | 83F17A410D003C | A km/h |
| 0110 | 10 | MAF flow rate | 84F17A4110000040 | (A*256+B)/100 g/s |
| 0111 | 11 | Throttle position | 83F17A41112565 | A*100/255 % |
| 011C | 1C | OBD standard (obd_std) | 83F17A411C0651 | A |
| 0121 | 21 | Distance with MIL on | 84F17A4121000051 | (A*256+B) km |
| 0123 | 23 | Fuel rail pressure | 84F17A4123000D60 | (A*256+B)*10 kPa |

Les requêtes avec un **« 1 » en suffixe** (ex. `01011`, `010C1`) apparaissent aussi dans le log (variante ou typo de l’app) ; côté ECU on envoie en général **0101**, **010C**, etc.

## Correspondance avec les champs SZ Viewer (idée pour l’ESP32)

| Champ SZ / MQTT | PID OBD2 | Remarque |
|------------------|----------|----------|
| engine_rpm | 010C | (A*256+B)/4 |
| speed_kmh | 010D | A |
| engine_temp_c | 0105 | A - 40 (liquid coolant) |
| intake_c | 010F (IAT) si supporté, sinon dérivé | 010F = A - 40 °C ; à vérifier support K9K |
| bar_pressure_kpa / requested_in_pressure_mbar | 010B | MAP en kPa (×10 pour mbar) |
| accelerator_pct | 0111 | Throttle % |
| air_flow_estimate_mgcp | 0110 | MAF g/s → conversion vers mg/cp selon doc |
| rail_pressure_bar | 0123 | (A*256+B)*10 kPa → bar = /100 |
| battery_v | ATRV | Tension batterie (déjà dans le log : 12.3V, 12.4V) |

D’autres champs SZ (ex. EGR, desired idle, gear ratio) peuvent être absents en PIDs standard ; à compléter par PIDs constructeur (mode 21/22) ou garder le décode SZ en secours.

## Parsing des réponses KWP (vLinker)

- Réponse brute exemple : `84F17A410C00003C`
  - `84` = longueur (optionnel selon firmware)
  - `F1` = adresse ECU
  - `7A` = réponse au mode 01 (0x7A = 0x01 + 0x40 + en-tête KWP)
  - `41` = mode 01 (Current data)
  - `0C` = PID
  - `00003C` = données (pour 010C : A=0x00, B=0x3C → RPM = 60/4 = 15 ? ou autre interprétation selon ordre des octets)

Vérifier l’ordre des octets (big-endian / little-endian) sur des valeurs connues (ex. vitesse affichée vs 010D, régime vs 010C) pour valider les formules côté ESP32.
