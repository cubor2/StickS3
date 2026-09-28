# StickS3 — micro à dictée + sonnette de fin d'IA

Ton M5 StickS3 en deux accessoires pour le vibe code :

1. **Micro sans fil** — un appui sur le bouton, tu parles, un deuxième
   appui : le texte est transcrit (API) et collé dans la fenêtre active
   (OpenCode). Comme Voicy, mais le micro est dans ta main.
2. **Sonnette de fin d'IA** — quand une IA finit son boulot sous OpenCode,
   le Stick fait ding et affiche une confirmation. Tu peux te balader dans la
   maison et savoir que c'est prêt.

```
   ┌────────────┐   WiFi/TCP    ┌──────────────────┐   HTTP    ┌───────────┐
   │  StickS3   │ ────────────► │ sticks3_service  │ ◄──────── │ OpenCode  │
   │ micro+ding │ ◄──────────── │ transcrit + colle│           │  plugin   │
   └────────────┘   notif       └──────────────────┘           └───────────┘
```

## Contenu

| Dossier | Contenu |
|---|---|
| `firmware/` | le programme du StickS3 (PlatformIO + M5Unified) |
| `service/` | le service PC : reçoit l'audio, transcrit, colle, fait ding |
| `opencode-plugin/` | le plugin OpenCode qui déclenche la sonnette |

## Architecture

Le Stick ne connaît aucune adresse IP de PC. Une fois connecté au Wi-Fi, il
diffuse périodiquement une requête UDP sur le port `8789`. Le service PC
répond avec son IP, son port TCP (`8787`) et son **nom**, que le Stick
affiche sur son écran d'accueil ; puis le Stick garde cette cible et se
reconnecte automatiquement. Cela permet de passer du fixe au portable sans
modifier ni reflasher le firmware.

```
StickS3 -- UDP broadcast :8789 --> service Windows
StickS3 <-- IP + port TCP -------- service Windows
StickS3 == WAV / commandes TCP ==> service Windows == API STT + collage ==> application active
OpenCode == HTTP localhost:8788 ==> service Windows == notification TCP ==> StickS3
```

Le service n'accepte volontairement qu'une commande clavier fixe (`Entrée`),
sans payload, plutôt qu'un relais de touches arbitraires.

### Changer de PC en un double-clic

Plusieurs PC peuvent faire tourner le service en même temps. Pour attacher le
Stick au PC **où tu te trouves**, double-clique `service\Prendre-Stick.cmd` :
il diffuse une réclamation UDP que le Stick entend même s'il est connecté à
une autre machine, et il bascule en quelques secondes. L'écran d'accueil du
Stick affiche en bas le nom du PC auquel il parle. Astuce : épingler ce
fichier à la barre des tâches. Au démarrage du Stick, si plusieurs services
répondent en même temps, il prend le premier — un double-clic sur
`Prendre-Stick.cmd` tranche.

### Authentification Stick ↔ service

Toute personne présente sur le même réseau peut tenter le port TCP : le
service n'exécute donc que ce qui est **authentifié par un jeton partagé**.
Le Stick envoie ce jeton dans son `HELLO` ; sans le bon jeton, la connexion
est coupée sans réponse.

1. Génère un jeton : `python -c "import secrets; print(secrets.token_urlsafe(18))"`
2. Mets-le dans `firmware/src/config.h` (`SERVICE_TOKEN`) — reflashe ensuite.
3. Mets le même dans `service/config.json` (`service_token`) sur **chaque PC**.

Si `service_token` est vide, le service accepte toute connexion et le signale
dans ses logs au démarrage. Le jeton ne se commit jamais (comme les clés Wi-Fi).

## 1. Service PC (à lancer en premier)

### Installation simple sur un PC Windows

1. Installe [Python 3](https://www.python.org/downloads/windows/) si besoin,
   en cochant **Add python.exe to PATH**.
2. Double-clique `service\Installer-demarrage-StickS3.cmd`.
3. À la première exécution, le lanceur installe la dépendance Python et
   demande la clé API une seule fois. Elle est enregistrée dans les variables
   d'environnement du **compte Windows**, jamais dans le dépôt.

Ce même clic ajoute le service au démarrage de Windows. Pour le lancer à la
main sans modifier le démarrage, double-clique `service\Lancer-StickS3.cmd`.
Laisser sa fenêtre ouverte arrête le service avec `Ctrl+C`.

Si une ancienne fenêtre `sticks3_service.py` est déjà ouverte, ferme-la avec
`Ctrl+C` avant ce premier lancement : elle ne connaît pas encore la découverte
automatique.

Le Stick trouve automatiquement le PC qui exécute le service sur le même
Wi-Fi : il n'y a plus d'adresse IP à rechercher ou à mettre dans le firmware.
Au premier lancement, Windows peut demander d'autoriser Python sur les
**réseaux privés** ; il faut l'accepter pour la découverte du Stick.

### Lancement manuel (développement)

```powershell
cd service
py -m pip install -r requirements.txt
$env:STT_API_KEY = "sk-..."
py sticks3_service.py
```

Test rapide de la sonnette sans attendre une IA :

```powershell
curl -X POST http://127.0.0.1:8788/notify -d '{\"title\":\"C EST PRET !\",\"message\":\"test\",\"sound\":\"frites\"}'
```

### Config (`service/config.json`)

| Clé | Défaut | Rôle |
|---|---|---|
| `stt_base_url` | `https://api.openai.com/v1` | endpoint de transcription (compatible OpenAI) |
| `stt_model` | `gpt-4o-mini-transcribe` | modèle (ou `whisper-1`) |
| `stt_language` | `fr` | langue forcée (vide = auto) |
| `paste_mode` | `clipboard` | `clipboard` (Ctrl+V) ou `type` (frappe lettre à lettre) |
| `paste_shortcut` | `ctrl+v` | raccourci de collage (ex. `ctrl+shift+v`) |
| `discovery_port` | `8789` | port UDP de découverte automatique du Stick |
| `service_token` | *(vide)* | jeton que le Stick doit présenter (cf. Authentification) |

La clé API ne se met **jamais** dans un fichier du repo : variable
d'environnement `STT_API_KEY` (ou `OPENAI_API_KEY`). Le lanceur Windows
enregistre `STT_API_KEY` une fois pour le compte utilisateur.

## 2. Plugin OpenCode (la sonnette)

```powershell
copy opencode-plugin\sticks3-notify.js "$env:USERPROFILE\.config\opencode\plugins\"
```

Puis **redémarre OpenCode** (la config n'est pas rechargée à chaud).
Quand une IA finit son tour (`session.idle`), le plugin prévient le
service, le stick fait ding. Sans service lancé, le plugin reste silencieux.

## 3. Firmware du StickS3

```powershell
cd firmware
copy src\config.example.h src\config.h
# remplis seulement WIFI_SSID et WIFI_PASSWORD
py -m platformio run -t upload
```

Le flash se fait en USB-C. Si le port n'apparaît pas : maintiens le
bouton reset sur le côté, le LED verte clignote = mode download.

### Gestes

| Geste | Action |
|---|---|
| **A** (écran accueil) | démarrer la dictée — bip montant |
| **A maintenu** (accueil) | réglage du volume : chaque appui sur **B** monte d'un cran (0–5), relâcher A valide — mémorisé |
| **A** (pendant la dictée) | envoyer — bip descendant, texte collé sur le PC |
| **B** (pendant la dictée) | annuler |
| **B** (écran accueil) | envoie `Entrée` dans la fenêtre PC active |

### Batterie et veille

Après une période d'inactivité sur l'écran d'accueil, le Stick réduit sa
luminosité à 90 secondes, éteint l'écran à 3 minutes et coupe son alimentation
à 5 minutes. Le premier clic après la veille réveille **et** effectue son
action : pas besoin de cliquer deux fois.

L'interface est volontairement minimale : fond menthe, micro fixe et retours
visuels locaux (VU-mètre, spinner, ondes pour `Entrée`).

### Config (`firmware/src/config.h`)

| Clé | Rôle |
|---|---|
| `WIFI_SSID` / `WIFI_PASSWORD` | WiFi 2.4 GHz (pas de 5 GHz avec l'ESP32) |
| `DISCOVERY_PORT` | port UDP de découverte du service (8789 par défaut) |
| `SERVICE_TOKEN` | jeton partagé avec le service (authentification) |
| `SPEAKER_VOLUME` | volume usine ; réglable ensuite sur le Stick (A maintenu) |
| `MAX_RECORD_SEC` | durée max d'une dictée (défaut 60 s) |

## Notes techniques

- **Micro et haut-parleur sont exclusifs** sur ce stick (codec ES8311
  partagé) : le firmware bascule proprement autour des bips. Un bip qui
  sonne pendant l'enregistrement est impossible — les notifications
  reçues en pleine dictée sont jouées juste après.
- Audio : PCM 16 bits mono 16 kHz, buffer en PSRAM (8 Mo dispo).
- Protocole maison minimal : trames `[u32 longueur][u8 type][payload]`,
  cf. `firmware/src/net.h`.
- Après transcription, le service normalise quelques variantes de `Nori`
  (`Nory`, `Noury`, `Nauri`, `Nozri`…) avant affichage et collage.
- `firmware/src/config.h` et `service/config.json` sont ignorés par git
  (pas de mot de passe WiFi ni de clé API dans le repo).

## Points d'attention pour reprendre le projet

- **Plusieurs services peuvent cohabiter** : le Stick suit le premier qui
  répond à la découverte ; `Prendre-Stick.cmd` sur le PC voulu tranche
  instantanément.
- **Pare-feu Windows** : autoriser Python sur les réseaux privés lors du
  premier lancement, sinon la découverte et le TCP seront bloqués.
- **Focus Windows** : le collage et `Entrée` visent toujours la fenêtre active.
  Le Stick ne sait pas choisir une application à distance.
- **Audio partagé** : micro et haut-parleur utilisent le codec ES8311 ; une
  notification reçue pendant une dictée est donc jouée après celle-ci.
- **Animations** : ne pas redessiner l'écran complet dans une boucle rapide.
  Les écrans Wi-Fi et transcription dessinent le micro une fois, puis n'animent
  localement que le spinner. C'est ce qui évite le clignotement au boot.
- **États temporisés** : comparer `millis() - stateSince`, jamais une valeur
  `now` capturée avant un changement d'état, sinon un underflow peut faire
  expirer l'animation immédiatement.

## Dépannage

| Symptôme | Piste |
|---|---|
| Écran « PC KO » | le service ne tourne pas ; lance `Lancer-StickS3.cmd` et accepte l'accès Python aux réseaux privés si Windows le demande |
| « transcription ratee » | clé API absente/expirée, ou quota atteint |
| « collage » mais rien n'apparaît | la fenêtre cible n'avait pas le focus ; essaie `paste_mode: "type"` |
| Pas de ding quand l'IA finit | plugin non installé, ou OpenCode pas redémarré |
| Le stick ne vibre pas... | il n'y a pas de moteur de vibration, c'est du son. Promis, on a regardé. |
