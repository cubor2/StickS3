// ------------------------------------------------------------
// Config locale du StickS3 — copier en `config.h` puis remplir.
// `config.h` est ignoré par git (pas de mot de passe WiFi dans le repo).
// ------------------------------------------------------------
#pragma once

// WiFi de la maison (2.4 GHz uniquement — l'ESP32-S3 ne fait pas le 5 GHz)
#define WIFI_SSID       "MON_WIFI"
#define WIFI_PASSWORD   "mon_mot_de_passe"

// Le Stick découvre automatiquement le PC qui exécute le service sur le
// réseau local : pas d'IP à renseigner ni de reflash quand on change de PC.
// Ce port UDP doit correspondre à `discovery_port` du service.
#define DISCOVERY_PORT  8789
#define SERVICE_PORT    8787

// Nom affiché dans les logs du service
#define DEVICE_NAME     "sticks3-cuisine"

// Volume du haut-parleur (0-255). Sur batterie, rester sous ~75%
// sinon le StickS3 peut redémarrer (note constructeur).
#define SPEAKER_VOLUME  96

// Durée max d'une dictée, en secondes (buffer PSRAM)
#define MAX_RECORD_SEC  60

// Fréquence d'échantillonnage audio — standard STT
#define SAMPLE_RATE     16000

// Jeton partagé avec le service (service_token dans service/config.json).
// Il doit être identique des deux côtés. Génère le tien, par exemple :
//   python -c "import secrets; print(secrets.token_urlsafe(18))"
// Vide = authentification désactivée (déconseillé sur un réseau partagé).
#define SERVICE_TOKEN   ""
