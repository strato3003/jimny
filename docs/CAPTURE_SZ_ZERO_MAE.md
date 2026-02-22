# Capture SZ pour MAE nulle (décodé = OCR)

Pour que la comparaison **décodé vs OCR** donne une MAE nulle partout, il faut que **chaque trame brute** soit alignée au **même instant** que l’image vidéo qui affiche les valeurs. La méthode actuelle (log à la seconde + plusieurs frames par seconde) ne le permet pas.

## Limites de la méthode actuelle

- **Log (ex. trames.log)** : timestamps à la **seconde** (HH:MM:SS). Plusieurs requêtes/réponses peuvent avoir lieu dans la même seconde ; on ne sait pas à quel moment précis chaque réponse 21A0/21A2/21A5/21CD a été reçue.
- **Vidéo** : extraite à 2–3 fps → plusieurs **frames par seconde**. On associe à chaque frame la même seconde log, donc le **même** bloc raw pour 2–3 images différentes.
- **Conséquence** : on compare les mêmes octets à plusieurs écrans (à t, t+0,5 s, t+1 s). Une seule frame par seconde peut être correctement alignée ; les autres biaisent la MAE.

## Méthode cible : timestamps précis + vidéo fine

### 1. Côté trames (Wireshark ou équivalent)

- **Capturer** le trafic entre SZ Viewer et le vLinker (Bluetooth SPP ou USB selon le lien PC ↔ vLinker).
- **Exporter** chaque message (requête + réponse) avec un **timestamp précis** (microsecondes ou millisecondes), par exemple :
  - Wireshark : export CSV/JSON avec colonne `frame.time_epoch` ou `frame.time_relative`.
  - Ou un proxy/sniffeur custom qui log `timestamp_us, direction, hex_payload` pour chaque paquet.
- **Format utile** : une ligne par réponse complète (61 A0 …, 61 A2 …, etc.) avec l’heure exacte de réception.

Exemple de structure cible :

```
timestamp_utc_ms,page,hex_ascii
1708360613123,21A0,363141304646...
1708360613189,21A2,363141323030...
```

### 2. Côté vidéo (screencast SZ Viewer)

- **Enregistrer** l’écran pendant la même session (même PC, même horloge si possible).
- **Découper** la vidéo en images avec un pas **très fin** (ex. 10–30 fps, ou une image tous les 50–100 ms) pour avoir au moins une frame proche de chaque réception de trame.
- **Timestamp de chaque frame** : soit via l’heure d’affichage lue à l’écran (si présente), soit via `t = frame_index / fps` + une **ancrage unique** (une frame dont l’heure réelle est connue).

### 3. Alignement

- Pour chaque **frame** à l’instant `t_video` :
  - Trouver dans le log des trames la **dernière** réponse reçue **avant** (ou la plus proche de) `t_video` pour chaque page 21A0, 21A2, 21A5, 21CD.
- Ou inversement : pour chaque **trame** reçue à `t_log`, associer la frame vidéo dont le timestamp est le plus proche de `t_log`.

Ainsi, chaque ligne du jsonl correspond à **un** instant précis : une frame vidéo ↔ un jeu de 4 réponses avec des timestamps cohérents. Décodé et OCR décrivent alors le même état → MAE théoriquement nulle (à erreur OCR près).

### 4. Synchronisation PC / horloge

- Idéalement : **une seule horloge** (PC). Le capture Wireshark et l’enregistrement vidéo utilisent la même référence (ex. `time.time()` ou horodatage fichier).
- Si la vidéo n’a pas de timestamp embarqué, un **point d’ancrage** (ex. une frame où l’heure est lue à l’écran) suffit pour dériver `t_video` pour toutes les frames à condition que le fps d’extraction soit connu et stable.

## Résumé

| Élément        | Actuel              | Cible (zero MAE)                    |
|----------------|---------------------|-------------------------------------|
| Log trames     | Seconde (HH:MM:SS)  | Timestamp précis (ms / µs)          |
| Source         | Log texte / MIM     | Wireshark (ou proxy) + export précis|
| Vidéo          | 2–3 fps             | 10–30 fps (ou pas ~50–100 ms)       |
| Alignement     | 1 seconde = N frames| 1 frame ↔ trames les plus proches en temps |

Une fois une telle capture disponible, on peut adapter `sz_sync.py` (ou un nouveau script) pour lire le log avec timestamps précis et produire un jsonl où **chaque ligne = une frame + les 4 pages au même instant** → optimisation du décodeur sur ce jsonl pour viser MAE = 0.
