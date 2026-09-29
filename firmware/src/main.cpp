// ------------------------------------------------------------
// StickS3 « micro à dictée + sonnette de fin d'IA »
//
// Flux dictée :  A (bip) → on parle → A (bip) → WAV vers le PC
//                → le service transcrit et colle le texte dans
//                la fenêtre active (OpenCode, F4 dans Voicy, etc.).
// Flux notif :   le service pousse NOTIFY → ding de friteuse +
//                cornet de frites à l'écran.
//
// Contrainte matérielle : micro (ES8311) et haut-parleur ne peuvent
// pas tourner ensemble. Le bip d'ouverture a lieu AVANT Mic.begin(),
// celui de fermeture APRÈS Mic.end().
// ------------------------------------------------------------
#include "M5Unified.h"
#include "esp_heap_caps.h"
#include <Preferences.h>
#include <math.h>
#include "config.h"
#include "ui.h"
#include "net.h"

static_assert(SAMPLE_RATE == 16000, "le service s'attend a du 16 kHz");

// ---------- états ---------------------------------------------
enum State : uint8_t {
  ST_WIFI,
  ST_IDLE,
  ST_RECORDING,
  ST_WAITING,
  ST_NOTIFY,      // écran temporaire (sonnette / message)
  ST_ENTER,       // brève validation visuelle du bouton Entrée
  ST_TRANSIENT,   // écrans courts (collé / annulé / erreur)
  ST_VOLUME,      // réglage du volume (A maintenu, B crans)
};
static State state = ST_WIFI;
static uint32_t stateSince = 0;
static constexpr uint32_t DIM_AFTER_MS = 90UL * 1000UL;
static constexpr uint32_t SCREEN_SLEEP_AFTER_MS = 3UL * 60UL * 1000UL;
static constexpr uint32_t POWER_OFF_AFTER_MS = 5UL * 60UL * 1000UL;
// IMU : au-delà de ce delta (en G, somme des axes), le stick est considéré
// porté et la veille est repoussée. La présence de l'IMU est vérifiée à
// l'exécution — le code reste inerte sur un stick sans accéléromètre.
static constexpr float IMU_WAKE_DELTA = 0.10f;
static uint32_t lastActivity = 0;
static uint32_t lastImuCheck = 0;
static bool screenSleeping = false;
static void go(State s) {
  state = s;
  stateSince = millis();
  Serial.printf("[etat] -> %u\n", (unsigned)s);
}

static void wakeScreen() {
  if (screenSleeping) {
    ui::setScreenSleeping(false);
    screenSleeping = false;
  }
  ui::setDimmed(false);
}

// ---------- notif mise en attente -----------------------------
// Une sonnette qui sonne pendant qu'on dicte, ça se perd : on stocke
// et on la joue à la première occasion.
static bool  pendOn = false;
static char  pendTitle[40] = {0};
static char  pendMsg[80]   = {0};
static char  pendSnd[16]   = "frites";
static bool  pendTranscriptOn = false;
static char  pendTranscriptStatus[12] = {0};
static char  pendTranscriptMsg[128] = {0};
static char  pendTranscriptExcerpt[96] = {0};

// ---------- audio ---------------------------------------------
// PCM 16 bits mono 16 kHz → 32 ko/s → 60 s ≈ 1,9 Mo en PSRAM.
static constexpr size_t CHUNK_SAMPLES = 512;  // ~32 ms par tour

static int16_t* pcmBuf = nullptr;
static size_t   pcmLen  = 0;   // en échantillons
static size_t   pcmCap  = 0;
static int16_t  chunk[CHUNK_SAMPLES];
static volatile bool micChunkReady = false;
static volatile uint32_t micChunksReceived = 0;
static uint32_t micRequestSince = 0;

// Boutons lus directement : le filtrage de M5Unified a montré un comportement
// erratique avec les clics brefs sur ce StickS3. Les KEY sont actifs-bas.
class Key {
 public:
  explicit Key(uint8_t gpio) : gpio_(gpio) {}
  void begin() {
    pinMode(gpio_, INPUT_PULLUP);
    raw_ = stable_ = !digitalRead(gpio_);
    changedAt_ = millis();
  }
  void update(uint32_t now) {
    bool sample = !digitalRead(gpio_);
    if (sample != raw_) { raw_ = sample; changedAt_ = now; }
    if (raw_ != stable_ && now - changedAt_ >= 15) {
      stable_ = raw_;
      if (!stable_) released_ = true;  // un clic est validé au relâchement
    }
  }
  bool takeReleased() { bool v = released_; released_ = false; return v; }
  bool isPressed() const { return stable_; }
 private:
  uint8_t gpio_;
  bool raw_ = false, stable_ = false, released_ = false;
  uint32_t changedAt_ = 0;
};
static Key keyA(11);
static Key keyB(12);

// M5Unified récent remplit les buffers en tâche de fond. Ce callback est
// appelé quand le bloc demandé est réellement disponible.
static void micBufferReleased(void*, void* data, size_t length) {
  if (data == chunk && length == CHUNK_SAMPLES) {
    ++micChunksReceived;
    micChunkReady = true;
  }
}

// Header WAV 44 octets (PCM s16le mono)
static void buildWavHeader(uint8_t h[44], uint32_t dataBytes) {
  auto put32 = [&](int o, uint32_t v) {
    h[o] = v & 0xFF; h[o + 1] = (v >> 8) & 0xFF;
    h[o + 2] = (v >> 16) & 0xFF; h[o + 3] = (v >> 24) & 0xFF;
  };
  auto put16 = [&](int o, uint16_t v) {
    h[o] = v & 0xFF; h[o + 1] = (v >> 8) & 0xFF;
  };
  memcpy(h, "RIFF", 4);
  put32(4, 36 + dataBytes);
  memcpy(h + 8, "WAVEfmt ", 8);
  put32(16, 16);                    // taille du bloc fmt
  put16(20, 1);                     // PCM
  put16(22, 1);                     // mono
  put32(24, (uint32_t)SAMPLE_RATE);
  put32(28, (uint32_t)SAMPLE_RATE * 2);
  put16(32, 2);                     // alignement
  put16(34, 16);                    // bits
  memcpy(h + 36, "data", 4);
  put32(40, dataBytes);
}

// ---------- petits sons ---------------------------------------
static void beep(uint16_t freq, uint16_t ms) {
  M5.Speaker.tone(freq, ms);
  delay(ms + 25);
}

// Toujours : Speaker.begin() / ... / Speaker.end() autour des bips.
static void sfxStart() {                 // « j'écoute » — montant
  M5.Speaker.begin();
  beep(660, 70);
  beep(880, 90);
  M5.Speaker.end();
}
static void sfxStop() {                  // « c'est envoyé » — descendant
  M5.Speaker.begin();
  beep(880, 70);
  beep(660, 90);
  M5.Speaker.end();
}
static void sfxDing() {                  // LA sonnette de la friteuse
  M5.Speaker.begin();
  beep(1047, 140);
  delay(60);
  beep(1047, 140);
  delay(60);
  beep(1319, 340);
  M5.Speaker.end();
}
static void sfxError() {                 // frigo en panne (gentil)
  M5.Speaker.begin();
  beep(220, 200);
  beep(180, 260);
  M5.Speaker.end();
}
static void sfxOk() {                    // confirmation « collé »
  M5.Speaker.begin();
  beep(784, 60);
  beep(1047, 110);
  M5.Speaker.end();
}
static void sfxEnter() {                 // touche Entrée — clic court, sobre
  M5.Speaker.begin();
  beep(880, 140);
  M5.Speaker.end();
}

// ---------- volume réglable -----------------------------------
// 0-5 crans, plafonnés à ~78 % comme le recommande la note constructeur
// (au-delà, le StickS3 peut redémarrer sur batterie). Persisté en NVS :
// un réglage qui disparaîtrait à chaque extinction auto serait absurde.
static const uint8_t VOL_STEPS[6] = {0, 40, 80, 120, 160, 200};
static uint8_t volStep = 2;
static uint32_t aHeldSince = 0;

static void applyVolume() {
  M5.Speaker.setVolume(VOL_STEPS[volStep]);
}

static void saveVolume() {
  Preferences prefs;
  prefs.begin("sticks3", false);
  prefs.putUChar("vol", volStep);
  prefs.end();
}

// ---------- bascule micro / haut-parleur ----------------------
static bool micOpen() {
  M5.Speaker.end();          // exclusif : on libère l'I2S
  micChunkReady = false;
  micChunksReceived = 0;
  micRequestSince = 0;
  M5.Mic.setBufferReleaseCallback(nullptr, micBufferReleased);
  return M5.Mic.begin();
}
static void micClose() {
  if (M5.Mic.isEnabled()) M5.Mic.end();
  M5.Mic.setBufferReleaseCallback(nullptr, nullptr);
}

// ---------- machine à états : dictée --------------------------
static void enterIdle();  // forward

static void startRecording() {
  pcmLen = 0;
  sfxStart();                 // bip pendant que le haut-parleur est libre
  if (!micOpen()) {
    ui::error("micro ko");
    sfxError();
    go(ST_TRANSIENT);
    return;
  }
  ui::rec(0, (uint32_t)MAX_RECORD_SEC * 1000UL);
  go(ST_RECORDING);
}

static void stopRecording(bool cancel) {
  micClose();                 // on libère le micro avant tout bip

  if (cancel) {
    ui::cancelled();
    sfxStop();
    go(ST_TRANSIENT);
    return;
  }

  if (pcmLen < 1600) {        // < 100 ms : inutilisable
    ui::error("trop court");
    sfxError();
    go(ST_TRANSIENT);
    return;
  }

  ui::sending();
  sfxStop();

  if (!net::svcUp()) {
    ui::error("pas de PC");
    sfxError();
    go(ST_TRANSIENT);
    return;
  }

  uint8_t hdr[44];
  uint32_t dataBytes = (uint32_t)(pcmLen * 2);
  buildWavHeader(hdr, dataBytes);

  // Envoi DÉLÉGUÉ à la tâche réseau : la boucle UI reste vivante pendant
  // l'upload (la roue tourne, les boutons répondent), quel que soit l'état
  // du lien Wi-Fi. Le résultat revient par la file (0xFD).
  if (!net::requestSend(hdr, pcmBuf, dataBytes)) {
    ui::error("envoi rate");
    sfxError();
    go(ST_TRANSIENT);
    return;
  }
  ui::waiting(millis());
  go(ST_WAITING);
}

// Un chunk par tour (~32 ms) : VU-mètre réactif, chrono à la seconde.
static void recordLoop() {
  // record() ne signifie plus « buffer rempli » : il ne fait que déposer
  // une demande dans la tâche micro. On ne réutilise donc chunk qu'après
  // notification du callback.
  if (!micChunkReady) {
    // isRecording() est l'état atomique réel de la file M5Unified. Un
    // drapeau local pouvait rester coincé après le premier buffer rempli.
    if (!M5.Mic.isRecording()) {
      if (M5.Mic.record(chunk, CHUNK_SAMPLES, (uint32_t)SAMPLE_RATE)) {
        micRequestSince = millis();
      }
    }
    // Ne laisse jamais une capture I2S sans données geler l'interface.
    if (micRequestSince && millis() - micRequestSince > 1500) {
      Serial.printf("[mic] aucun bloc recu (queue=%u chunks=%lu)\n",
                    (unsigned)M5.Mic.isRecording(), (unsigned long)micChunksReceived);
      micClose();
      ui::error("micro sans flux");
      sfxError();
      go(ST_TRANSIENT);
      return;
    }
    delay(1);
    return;
  }
  micChunkReady = false;
  micRequestSince = 0;

  int32_t peak = 0;
  for (size_t i = 0; i < CHUNK_SAMPLES; i++) {
    int32_t v = chunk[i];
    if (v < 0) v = -v;
    if (v > peak) peak = v;
  }
  int level = peak >> 10;                 // 32768 → 32
  if (level > 12) level = 12;
  ui::recVU(level);

  if (pcmLen + CHUNK_SAMPLES <= pcmCap) {
    memcpy(pcmBuf + pcmLen, chunk, CHUNK_SAMPLES * sizeof(int16_t));
    pcmLen += CHUNK_SAMPLES;
  }

  if (pcmLen + CHUNK_SAMPLES > pcmCap) {  // buffer plein : envoi auto
    stopRecording(false);
    return;
  }

  static uint32_t lastUi = 0;
  uint32_t now = millis();
  if (now - lastUi > 500) {
    lastUi = now;
    ui::recTime(pcmLen * 1000UL / (uint32_t)SAMPLE_RATE,
                (uint32_t)MAX_RECORD_SEC * 1000UL);
  }
}

// ---------- événements du service -----------------------------
static void playNotify(const char* title, const char* msg, const char* snd) {
  lastActivity = millis();  // une notification est une interaction utile
  wakeScreen();
  bool isFrites = (strcmp(snd, "frites") == 0);
  ui::notify(title, msg, isFrites);
  if (strcmp(snd, "error") == 0) sfxError();
  else sfxDing();
  go(ST_NOTIFY);
}

static void handleNet() {
  char buf[384];
  uint8_t type;
  while ((type = net::pollEvent(buf, sizeof(buf))) != 0) {
    // champs séparés par \t : on les isole sur place
    char* f[4] = {buf, nullptr, nullptr, nullptr};
    int nf = 1;
    for (char* p = buf; *p && nf < 4; p++) {
      if (*p == '\t') {
        *p = 0;
        f[nf++] = p + 1;
      }
    }

    if (type == 0x03) {  // NOTIFY : titre \t message \t son
      const char* title = f[0];
      const char* msg   = (nf > 1) ? f[1] : "";
      const char* snd   = (nf > 2) ? f[2] : "frites";

      if (state == ST_RECORDING) {
        // on ne coupe pas une dictée : la sonnette attend.
        pendOn = true;
        snprintf(pendTitle, sizeof(pendTitle), "%s", title);
        snprintf(pendMsg, sizeof(pendMsg), "%s", msg);
        snprintf(pendSnd, sizeof(pendSnd), "%s", snd);
        continue;
      }
      playNotify(title, msg, snd);

    } else if (type == 0x04) {  // TRANSCRIPT : statut \t message \t extrait
      const char* status  = f[0];
      const char* msg     = (nf > 1) ? f[1] : "";
      const char* excerpt = (nf > 2) ? f[2] : "";

      // Le résultat peut arriver pendant l'animation Entrée. On le garde
      // pour ne pas écraser son feedback visuel au même tour de boucle.
      if (state == ST_ENTER) {
        pendTranscriptOn = true;
        snprintf(pendTranscriptStatus, sizeof(pendTranscriptStatus), "%s", status);
        snprintf(pendTranscriptMsg, sizeof(pendTranscriptMsg), "%s", msg);
        snprintf(pendTranscriptExcerpt, sizeof(pendTranscriptExcerpt), "%s", excerpt);
        continue;
      }

      lastActivity = millis();
      wakeScreen();

      if (strcmp(status, "ok") == 0) {
        ui::done(excerpt[0] ? excerpt : msg);
        sfxOk();
      } else {
        ui::error(msg[0] ? msg : "transcription ratee");
        sfxError();
      }
      go(ST_TRANSIENT);

    } else if (type == 0xFD) {  // interne : résultat d'envoi WAV (tâche réseau)
      // L'upload s'est terminé pendant qu'on patiente : soit le service est
      // en train de transcrire (le chrono de patience repart pour 90 s),
      // soit l'envoi a échoué et on l'affiche sans attendre un transcript
      // qui ne viendra jamais.
      if (state == ST_WAITING) {
        if (strcmp(buf, "ok") == 0) {
          stateSince = millis();
        } else {
          ui::error("envoi rate");
          sfxError();
          go(ST_TRANSIENT);
        }
      }
    }
  }
}

// Retour au calme : joue d'abord la sonnette en attente, s'il y en a.
static void enterIdle() {
  if (pendOn) {
    pendOn = false;
    playNotify(pendTitle, pendMsg, pendSnd);
    return;
  }
  char pcName[20];
  net::serviceName(pcName, sizeof(pcName));
  ui::idle(net::svcUp(), pcName, millis());
  go(ST_IDLE);
}

// ---------- setup / loop --------------------------------------
void setup() {
  Serial.begin(115200);   // avant tout : le premier print doit être là
  Serial.println("[boot] StickS3 dictée v1");
  auto cfg = M5.config();
  M5.begin(cfg);
  // KEY1/KEY2 sont des boutons actifs à l'état bas (GPIO11/12).
  // M5Unified les configure en entrée simple ; les pull-up explicites
  // évitent une entrée flottante selon la révision du PCB.
  pinMode(11, INPUT_PULLUP);
  pinMode(12, INPUT_PULLUP);
  keyA.begin();
  keyB.begin();
  lastActivity = millis();
  Serial.printf("[boot] carte detectee: %d, GPIO11=%d GPIO12=%d\n",
                (int)M5.getBoard(), digitalRead(11), digitalRead(12));
  Serial.printf("[boot] imu dispo : %d\n", (int)M5.Imu.isEnabled());
  // Le micro M5Unified est une tâche de capture continue. Sans affinité elle
  // peut prendre le coeur de la loop Arduino (UI/boutons) et donner un stick
  // presque figé. On la réserve au coeur système, avant le premier Mic.begin.
  auto micCfg = M5.Mic.config();
  micCfg.task_pinned_core = 0;
  micCfg.task_priority = 1;
  M5.Mic.config(micCfg);
  M5.Speaker.setVolume(SPEAKER_VOLUME);
  // Le réglage utilisateur (écran volume) écrase la valeur usine si présent.
  {
    Preferences prefs;
    prefs.begin("sticks3", true);
    uint8_t saved = prefs.getUChar("vol", 255);
    prefs.end();
    if (saved <= 5) {
      volStep = saved;
      applyVolume();
    }
  }

  ui::begin();
  ui::bootSplash();

  // buffer audio en PSRAM d'abord (8 Mo dispo)
  pcmCap = (size_t)MAX_RECORD_SEC * (size_t)SAMPLE_RATE;
  pcmBuf = (int16_t*)heap_caps_malloc(pcmCap * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!pcmBuf) {
    pcmCap = 20 * (size_t)SAMPLE_RATE;   // repli RAM interne
    pcmBuf = (int16_t*)heap_caps_malloc(pcmCap * 2, MALLOC_CAP_8BIT);
  }
  if (!pcmBuf) {
    ui::error("ram ko");
    while (true) delay(1000);
  }

  net::begin();
  ui::wifiWaiting(millis());
  go(ST_WIFI);
}

void loop() {
  M5.update();
  uint32_t now = millis();
  keyA.update(now);
  keyB.update(now);
  bool aClicked = keyA.takeReleased();
  bool bClicked = keyB.takeReleased();
  if (aClicked || bClicked) {
    lastActivity = now;
    if (screenSleeping) {
      // Le clic réveille ET conserve son action : pas de double-geste pénible.
      wakeScreen();
      char pcName[20];
      net::serviceName(pcName, sizeof(pcName));
      ui::idle(net::svcUp(), pcName, now);
    } else {
      ui::setDimmed(false);
    }
  }
  // Le réseau vit désormais dans sa propre tâche (net.cpp) : plus rien de
  // bloquant ici, quel que soit l'état du lien Wi-Fi.
  handleNet();

  // Bouton droit : Entrée fixe dans la fenêtre PC, sauf pendant une prise
  // (annulation) ou le réglage volume (B change le cran). L'envoi est
  // délégué à la tâche réseau : aucun write bloquant sur le fil UI.
  if (bClicked && state != ST_RECORDING && state != ST_VOLUME && net::requestEnter()) {
    sfxEnter();
    // Le bip est bloquant : rafraîchir l'horloge avant de créer un état
    // temporisé, sinon ST_ENTER se croit déjà expiré dans ce même tour.
    now = millis();
    ui::enter(now);
    go(ST_ENTER);
  }

  // Cycle d'inactivité : assombrir → vraie veille écran → coupure matérielle.
  if (state == ST_IDLE) {
    // Un stick porté ne s'endort pas dans la main : l'IMU, s'il y en a un,
    // rafraîchit l'activité au moindre mouvement net.
    if (M5.Imu.isEnabled() && now - lastImuCheck >= 200) {
      lastImuCheck = now;
      M5.Imu.update();
      float ax = 0, ay = 0, az = 0;
      M5.Imu.getAccel(&ax, &ay, &az);
      static float lastAx = 0, lastAy = 0, lastAz = 0;
      float delta = fabsf(ax - lastAx) + fabsf(ay - lastAy) + fabsf(az - lastAz);
      lastAx = ax;
      lastAy = ay;
      lastAz = az;
      if (delta > IMU_WAKE_DELTA) {
        lastActivity = now;
      }
    }
    uint32_t idleFor = now - lastActivity;
    if (idleFor >= POWER_OFF_AFTER_MS) {
      Serial.println("[power] inactif 5 min : extinction");
      if (!screenSleeping) ui::setScreenSleeping(true);
      M5.Power.powerOff();
      delay(1000);  // normalement jamais atteint, garde un repli calme
    } else if (idleFor >= SCREEN_SLEEP_AFTER_MS) {
      if (!screenSleeping) {
        ui::setScreenSleeping(true);
        screenSleeping = true;
      }
    } else if (!screenSleeping) {
      ui::setDimmed(idleFor >= DIM_AFTER_MS);
    }
  }

  switch (state) {
    case ST_WIFI:
      // dessin complet fait a l'entree ; ici, juste les pastilles
      ui::wifiTick(now);
      if (net::wifiUp()) enterIdle();
      break;

    case ST_IDLE: {
      // redessin complet seulement si l'etat du service (ou son nom) change
      bool up = net::svcUp();
      static bool lastUp = true;
      static char lastName[20] = "";
      char pcName[20];
      net::serviceName(pcName, sizeof(pcName));
      if (up != lastUp || strcmp(pcName, lastName) != 0) {
        ui::idle(up, pcName, now);
        lastUp = up;
        snprintf(lastName, sizeof(lastName), "%s", pcName);
      }
      if (aClicked) {
        startRecording();
      }
      // Long-press A : réglage du volume, sans déclencher la dictée.
      if (keyA.isPressed()) {
        if (!aHeldSince) {
          aHeldSince = now;
        } else if (now - aHeldSince > 600) {
          ui::volume(volStep, 5);
          go(ST_VOLUME);
        }
      } else {
        aHeldSince = 0;
      }
      break;
    }

    case ST_VOLUME:
      // B : cran suivant (boucle 0..5) avec bip-test au nouveau volume.
      // A relâché : on valide, on persiste, retour au calme.
      if (bClicked) {
        volStep = (volStep + 1) % 6;
        applyVolume();
        ui::volume(volStep, 5);
        sfxOk();
      }
      if (aClicked || !keyA.isPressed()) {
        saveVolume();
        enterIdle();
      }
      break;

    case ST_RECORDING:
      recordLoop();
       if (aClicked) {
         stopRecording(false);
       } else if (bClicked) {
         stopRecording(true);
      }
      break;

    case ST_WAITING:
      ui::waitingTick(now);
      // millis() et non `now` : stateSince peut être postérieur à la lecture
      // de now en haut de boucle (l'état vient d'être créé). Un `now` plus
      // ancien ferait déborder le calcul en négatif et expirer l'état juste.
      // 90 s : aligné sur la patience du service (120 s API). Trop court,
      // le stick criait « pas de reponse » puis le collé tardif survenait.
      // Le chrono NE COMPTE PAS pendant l'upload (tâche réseau, lien parfois
      // lent en roaming) : il ne mesure que l'attente de la transcription.
      if (!net::sendBusy() && millis() - stateSince > 90000) {
        ui::error("pas de reponse");
        sfxError();
        go(ST_TRANSIENT);
      }
      break;

    case ST_NOTIFY:
      if (millis() - stateSince > 4000) {
        enterIdle();
       } else if (aClicked) {
         enterIdle();
      }
      break;

    case ST_ENTER:
      ui::enterTick(now);
      if (millis() - stateSince > 1600) {
        if (pendTranscriptOn) {
          pendTranscriptOn = false;
          if (strcmp(pendTranscriptStatus, "ok") == 0) {
            ui::done(pendTranscriptExcerpt[0] ? pendTranscriptExcerpt : pendTranscriptMsg);
            sfxOk();
          } else {
            ui::error(pendTranscriptMsg[0] ? pendTranscriptMsg : "transcription ratee");
            sfxError();
          }
          go(ST_TRANSIENT);
        } else {
          enterIdle();
        }
      }
      break;

    case ST_TRANSIENT:
      if (millis() - stateSince > 2500) {
        enterIdle();
      }
      break;

    default:
      break;
  }
}
