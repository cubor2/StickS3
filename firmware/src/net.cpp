// ------------------------------------------------------------
// net.cpp — client TCP + encadrement de trames.
//
// ARCHITECTURE : tout le réseau vit dans une tâche FreeRTOS dédiée
// (cœur 0, comme la capture micro). La boucle UI (cœur 1) n'appelle plus
// AUCUNE fonction bloquante : trois morts vérifiées (roue figée, erreur,
// boutons morts) venaient de write/connect bloquants quand le lien Wi-Fi
// roam ou agonise. Les demandes d'envoi (WAV, Entrée) passent par des
// handoffs atomiques ; les résultats remontent par la file d'événements.
// ------------------------------------------------------------
#include "net.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

// Anciens config.h locaux sans ces réglages : valeurs conventionnelles
// avec le service PC, pour ne pas imposer de modifier ce fichier.
#ifndef DISCOVERY_PORT
#define DISCOVERY_PORT 8789
#endif
#ifndef SERVICE_TOKEN
#define SERVICE_TOKEN ""
#endif

namespace net {

// Événement interne (jamais émis sur le réseau) : résultat d'un upload WAV.
static const uint8_t EV_SEND_RESULT = 0xFD;

static WiFiClient client;
static WiFiUDP discovery;

// --- état partagé ---------------------------------------------
// svcUp : écrit par la tâche réseau, lu par l'UI. Jamais d'accès direct
// au socket depuis deux tâches : la tâche tient ce drapeau à jour.
static std::atomic<bool> g_svcUp{false};

// Nom du PC auquel on est attaché : écrit par la tâche réseau sous section
// critique, lu par l'UI.
static portMUX_TYPE nameMux = portMUX_INITIALIZER_UNLOCKED;
static char svcName[20] = {0};

static void sanitizeName(const char* src, char* dst, size_t cap) {
  size_t n = 0;
  for (; src[n] && n + 1 < cap; ++n) {
    char c = src[n];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-';
    dst[n] = ok ? c : '-';
  }
  dst[n] = 0;
}

static void commitName(const char* src) {
  char tmp[20];
  sanitizeName(src, tmp, sizeof(tmp));
  portENTER_CRITICAL(&nameMux);
  memcpy(svcName, tmp, sizeof(svcName));
  svcName[sizeof(svcName) - 1] = 0;
  portEXIT_CRITICAL(&nameMux);
}

// --- file d'événements ----------------------------------------
// Producteur : tâche réseau. Consommateur : boucle UI.
struct EventItem {
  uint8_t type;
  char buf[384];
};
static QueueHandle_t evQueue = nullptr;

static void pushEvent(uint8_t type, const char* p, uint32_t len) {
  if (!evQueue) return;
  EventItem it;
  it.type = type;
  uint32_t n = min((uint32_t)sizeof(it.buf) - 1, len);
  memcpy(it.buf, p, n);
  it.buf[n] = 0;
  if (xQueueSend(evQueue, &it, 0) != pdTRUE) {
    // File pleine : on sacrifie le plus ancien pour le plus récent.
    EventItem drop;
    if (xQueueReceive(evQueue, &drop, 0) == pdTRUE) {
      xQueueSend(evQueue, &it, 0);
    }
  }
}

uint8_t pollEvent(char* buf, size_t cap) {
  if (!evQueue) return 0;
  EventItem it;
  if (xQueueReceive(evQueue, &it, 0) != pdTRUE) return 0;
  snprintf(buf, cap, "%s", it.buf);
  return it.type;
}

// --- demandes d'envoi (producteur : UI ; consommateur : tâche réseau) ---
static struct {
  uint8_t hdr[44];
  const int16_t* pcm;
  size_t bytes;
} sendReq;
static std::atomic<bool> sendPending{false};
static std::atomic<bool> sendCancelled{false};
static std::atomic<bool> enterPending{false};

bool requestSend(const uint8_t hdr44[44], const int16_t* pcm, size_t pcmBytes) {
  // Producteur unique (boucle UI) : load + store suffisent. Un envoi déjà
  // en vol est rejeté — un seul WAV à la fois par construction.
  if (sendPending.load(std::memory_order_acquire)) return false;
  memcpy(sendReq.hdr, hdr44, 44);
  sendReq.pcm = pcm;
  sendReq.bytes = pcmBytes;
  sendCancelled.store(false, std::memory_order_relaxed);
  sendPending.store(true, std::memory_order_release);
  return true;
}

void cancelSend() {
  // Demande d'avortement posée par l'UI (plafond d'attente atteint) :
  // l'upload s'arrête entre deux chunks. Un write DÉJÀ bloqué ne peut pas
  // être interrompu — lwIP finira par lâcher, et l'UI a déjà tourné la page.
  sendCancelled.store(true, std::memory_order_relaxed);
}

bool requestEnter() {
  // Commande volontairement sans payload : pas de relais clavier arbitraire.
  if (!g_svcUp.load(std::memory_order_relaxed)) return false;
  enterPending.store(true, std::memory_order_release);
  return true;
}

bool sendBusy() {
  // Vrai tant que l'upload du WAV est en vol : le timer de patience de
  // l'UI ne doit pas compter pendant ce temps.
  return sendPending.load(std::memory_order_acquire);
}

// --- état réseau (tâche réseau uniquement) --------------------
static uint32_t lastConnectTry = 0;
static uint32_t lastDiscoveryTry = 0;
static bool wasConnected = false;
static bool discoveryStarted = false;
static bool serviceKnown = false;
static bool connectFailed = false;  // la dernière tentative vers la cible a échoué
static IPAddress serviceIp;
static uint16_t servicePort = SERVICE_PORT;

static const char* DISCOVERY_REQUEST = "STICKS3_DISCOVER_V1";
static const char* DISCOVERY_RESPONSE = "STICKS3_HERE_V1 ";
// Un PC peut réclamer le Stick même s'il est connecté ailleurs : broadcast
// UDP entendu sur la même socket que la découverte, sans tour de main TCP.
static const char* DISCOVERY_CLAIM = "STICKS3_CLAIM_V1 ";

// "PORT [NOM] [CLAIM]" : nom optionnel (compat ancien service), CLAIM
// optionnel = le PC demandé réclame le stick via sa sonde de découverte.
// Le nom et le drapeau sont rendus à l'appelant, qui les adopte SEULEMENT
// s'il accepte la réponse — un effet de bord ici ferait osciller le nom
// affiché (bug vécu, 2026-09-28).
static bool parseResponse(const char* rest, unsigned int* port,
                          char* parsedName, size_t cap, bool* claimFlag) {
  char tmp[20] = {0};
  char flag[8] = {0};
  int fields = sscanf(rest, "%u %19s %7s", port, tmp, flag);
  if (fields < 1 || *port == 0 || *port > 65535) return false;
  if (fields >= 2) snprintf(parsedName, cap, "%s", tmp);
  else if (cap) parsedName[0] = 0;
  *claimFlag = (fields >= 3 && strncmp(flag, "CLAIM", 5) == 0);
  return true;
}

// Le PC répond au broadcast UDP avec son IP et le port TCP du service.
// Cela évite d'encoder une IP locale dans le firmware.
static void discoveryTick(uint32_t now) {
  if (!discoveryStarted) {
    discoveryStarted = discovery.begin(DISCOVERY_PORT);
    if (Serial && discoveryStarted) {
      Serial.printf("[discovery] ecoute UDP :%d\n", DISCOVERY_PORT);
    }
  }
  if (!discoveryStarted) return;

  int packetSize = discovery.parsePacket();
  if (packetSize > 0) {
    char response[64];
    int n = discovery.read(response, min(packetSize, (int)sizeof(response) - 1));
    response[max(0, n)] = 0;
    unsigned int port = 0;
    char parsedName[20] = {0};
    bool claimFlag = false;
    bool isClaim = false;
    bool isHere = false;
    if (strncmp(response, DISCOVERY_CLAIM, strlen(DISCOVERY_CLAIM)) == 0) {
      isClaim = parseResponse(response + strlen(DISCOVERY_CLAIM), &port,
                              parsedName, sizeof(parsedName), &claimFlag);
    } else if (strncmp(response, DISCOVERY_RESPONSE,
                       strlen(DISCOVERY_RESPONSE)) == 0) {
      isHere = parseResponse(response + strlen(DISCOVERY_RESPONSE), &port,
                             parsedName, sizeof(parsedName), &claimFlag);
    }

    if (isClaim || isHere) {
      IPAddress from = discovery.remoteIP();
      // Une candidature doit venir du même sous-réseau que le stick : les
      // adaptateurs virtuels/APIPA des PC émettent aussi des broadcasts et
      // leur adresse source est injoignable pour lui.
      IPAddress localIp = WiFi.localIP();
      IPAddress netMask = WiFi.subnetMask();
      bool sameSubnet = true;
      for (int i = 0; i < 4; ++i) {
        if ((localIp[i] & netMask[i]) != (from[i] & netMask[i])) {
          sameSubnet = false;
          break;
        }
      }

      // Quand basculer :
      //  - réponse marquée CLAIM : le PC demandé a répondu à la sonde du
      //    stick LUI-MÊME (unicast, fiable même à travers les ponts de
      //    bandes Wi-Fi qui avalent les broadcasts) ;
      //  - trame CLAIM broadcastée (ancien chemin, conservé) ;
      //  - stick détaché et cible inconnue ou défaillante.
      // Déjà attaché à ce PC : ne pas couper la connexion pour rien.
      bool alreadyHere = (from == serviceIp) && client.connected();
      bool shouldSwitch =
          !alreadyHere && (isClaim || claimFlag ||
                           (!client.connected() &&
                            (!serviceKnown || connectFailed)));
      if (sameSubnet && shouldSwitch) {
        serviceIp = from;
        servicePort = (uint16_t)port;
        serviceKnown = true;
        // La cible adoptée est intouchable tant qu'elle ne déçoit pas : une
        // réponse concurrente ne doit pas annuler un claim fraîchement reçu.
        connectFailed = false;
        if (parsedName[0]) commitName(parsedName);
        if (client.connected()) {
          client.stop();
          wasConnected = false;
        }
        if (Serial) {
          Serial.printf("[discovery] %s : %s:%u\n",
                        (isClaim || claimFlag) ? "claim" : "service",
                        from.toString().c_str(), servicePort);
        }
      }
    }
  }

  if (now - lastDiscoveryTry >= 2000) {
    lastDiscoveryTry = now;
    discovery.beginPacket(IPAddress(255, 255, 255, 255), DISCOVERY_PORT);
    discovery.write((const uint8_t*)DISCOVERY_REQUEST, strlen(DISCOVERY_REQUEST));
    discovery.endPacket();
  }
}

// --- sortie (tâche réseau uniquement) -------------------------
static bool sendFrame(uint8_t type, const uint8_t* payload, uint32_t len) {
  if (!client.connected()) return false;
  uint8_t hdr[5];
  hdr[0] = (uint8_t)(len);
  hdr[1] = (uint8_t)(len >> 8);
  hdr[2] = (uint8_t)(len >> 16);
  hdr[3] = (uint8_t)(len >> 24);
  hdr[4] = type;
  client.setNoDelay(true);
  if (client.write(hdr, 5) != 5) return false;
  if (len == 0) return true;
  uint32_t sent = 0;
  while (sent < len) {
    uint32_t chunk = min((uint32_t)4096, len - sent);
    size_t n = client.write(payload + sent, chunk);
    if (n == 0) return false;
    sent += n;
    vTaskDelay(1);
  }
  return true;
}

static bool sendHello() {
  // Le jeton protège l'injection de texte : sans le bon jeton, le service
  // coupe la connexion sans discussion. Vide = pas d'authentification.
  char payload[112];
  snprintf(payload, sizeof(payload), "%s\tfw-1.1\t%s", DEVICE_NAME, SERVICE_TOKEN);
  return sendFrame(0x01, (const uint8_t*)payload, strlen(payload));
}

// sendWav asynchrone : évite de dupliquer les 2 Mo de PCM — header puis
// buffer directement, depuis la tâche réseau. La boucle UI, elle, respire.
static bool uploadWav(const uint8_t hdr44[44], const int16_t* pcm, size_t pcmBytes) {
  if (!client.connected()) return false;

  // Porte de vivacité : un ping juste avant l'upload. Si le pair est un
  // zombie (connexion morte que lwIP croit encore établie — service tué
  // pendant que le stick regardait ailleurs), le RST arrive en un aller-
  // retour et l'échec est immédiat, au lieu de retransmettre pendant des
  // minutes dans le vide.
  sendFrame(0x06, nullptr, 0);
  vTaskDelay(pdMS_TO_TICKS(300));
  if (!client.connected()) return false;

  uint32_t payloadLen = 44 + pcmBytes;
  uint8_t hdr[5];
  hdr[0] = (uint8_t)(payloadLen);
  hdr[1] = (uint8_t)(payloadLen >> 8);
  hdr[2] = (uint8_t)(payloadLen >> 16);
  hdr[3] = (uint8_t)(payloadLen >> 24);
  hdr[4] = 0x02;  // MSG_WAV
  client.setNoDelay(true);
  if (client.write(hdr, 5) != 5) return false;
  if (client.write(hdr44, 44) != 44) return false;

  // Deadline totale : au-delà de 90 s, l'envoi est avorté — un lien
  // vraiment lent finit en "ko" propre plutôt qu'en roue éternelle.
  uint32_t deadline = millis() + 90000;
  const uint8_t* bytes = (const uint8_t*)pcm;
  uint32_t sent = 0;
  while (sent < pcmBytes) {
    if (!client.connected()) return false;
    if (sendCancelled.load(std::memory_order_relaxed)) return false;
    if ((int32_t)(millis() - deadline) >= 0) return false;
    uint32_t chunk = min((uint32_t)4096, (uint32_t)(pcmBytes - sent));
    size_t n = client.write(bytes + sent, chunk);
    if (n == 0) return false;
    sent += n;
    vTaskDelay(1);  // laisse respirer le WiFi (et le micro sur ce cœur)
  }
  return true;
}

// --- entrée : assemblage de trames ----------------------------
// Machine à états minimale, non bloquante (tâche réseau uniquement).
enum { RD_LEN = 0, RD_TYPE = 1, RD_PAYLOAD = 2 };
static uint8_t  rdPhase = RD_LEN;
static uint8_t  rdHdr[4];
static uint8_t  rdHdrGot = 0;
static uint8_t  rdType = 0;
static uint32_t rdLen = 0;
static uint32_t rdGot = 0;
static uint8_t  rxBuf[768];

static void pumpInbound() {
  while (client.connected() && client.available()) {
    int c = client.read();
    if (c < 0) break;
    uint8_t b = (uint8_t)c;
    switch (rdPhase) {
      case RD_LEN:
        rdHdr[rdHdrGot++] = b;
        if (rdHdrGot == 4) {
          rdLen = (uint32_t)rdHdr[0] | ((uint32_t)rdHdr[1] << 8) |
                  ((uint32_t)rdHdr[2] << 16) | ((uint32_t)rdHdr[3] << 24);
          rdHdrGot = 0;
          if (rdLen > 4096) {  // trame absurde : on resynchronise en coupant
            client.stop();
            return;
          }
          rdPhase = RD_TYPE;
        }
        break;
      case RD_TYPE:
        rdType = b;
        rdGot = 0;
        rdPhase = (rdLen == 0) ? RD_LEN : RD_PAYLOAD;
        if (rdLen == 0) pushEvent(rdType, "", 0);
        break;
      case RD_PAYLOAD: {
        if (rdGot < sizeof(rxBuf)) rxBuf[rdGot] = b;
        rdGot++;
        if (rdGot >= rdLen) {
          pushEvent(rdType, (const char*)rxBuf, min(rdGot, (uint32_t)sizeof(rxBuf)));
          rdPhase = RD_LEN;
        }
        break;
      }
    }
  }
}

// --- pilotage WiFi + logs de diagnostic ----------------------
// Les raisons ESP32 de déconnexion disent tout :
//   203 = réseau non trouvé, 15/201 = mot de passe refusé,
//   205 = échec auth, 1 = déconnexion normale.
static void onWifiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  if (!Serial) return;
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("[wifi] IP obtenu : %s\n",
                    WiFi.localIP().toString().c_str());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.printf("[wifi] deconnexion, reason=%u\n",
                    info.wifi_sta_disconnected.reason);
      break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("[wifi] association OK (avant DHCP)");
      break;
    default:
      break;
  }
}

// --- pas de la tâche réseau -----------------------------------
// Tout ce qui bloquait l'UI vit ici : reconnexion, découverte, pompage.
static void step() {
  uint32_t now = millis();

  if (!wifiUp()) {
    if (wasConnected) {
      client.stop();
      wasConnected = false;
    }
    if (discoveryStarted) {
      discovery.stop();
      discoveryStarted = false;
    }
    serviceKnown = false;
    g_svcUp.store(false, std::memory_order_relaxed);
    return;
  }

  discoveryTick(now);

  // Roaming forcé : le SSID 2,4 GHz peut être émis par plusieurs bornes
  // (box + répéteur) et le stick reste collé à une mauvaise après un
  // reconnect. Un lien durablement faible mérite un re-scan ; ça brise
  // aussi les zombies TCP nés d'une connexion perdue en route.
  static uint32_t weakSince = 0;
  {
    int rssi = WiFi.RSSI();
    if (rssi < -75) {
      if (!weakSince) {
        weakSince = now;
      } else if (now - weakSince > 60000) {
        weakSince = 0;
        if (Serial) {
          Serial.printf("[wifi] rssi %d durable : re-scan des bornes\n", rssi);
        }
        client.stop();
        g_svcUp.store(false, std::memory_order_relaxed);
        wasConnected = false;
        WiFi.disconnect(false, false);
        WiFi.reconnect();
        return;
      }
    } else {
      weakSince = 0;
    }
  }

  if (!client.connected()) {
    g_svcUp.store(false, std::memory_order_relaxed);
    if (wasConnected) {
      wasConnected = false;
    }
    if (serviceKnown && now - lastConnectTry > 2000) {
      lastConnectTry = now;
      client.stop();
      // Ici, un connect qui bloque n'arrête plus l'interface : c'était le
      // bug des trois morts. La borne de 50 ms reste une garde honnête.
      bool ok = client.connect(serviceIp, servicePort, 50);
      connectFailed = !ok;
      if (Serial) {
        Serial.printf("[tcp] connect %s:%u -> %s\n",
                      serviceIp.toString().c_str(), servicePort,
                      ok ? "OK" : "KO");
      }
      if (ok) {
        client.setNoDelay(true);
        wasConnected = true;
        sendHello();
        g_svcUp.store(true, std::memory_order_relaxed);
      }
    }
    return;
  }

  g_svcUp.store(true, std::memory_order_relaxed);
  wasConnected = true;
  pumpInbound();

  // Ping applicatif toutes les 10 s : si le pair est mort sans RST reçu
  // (extinction sauvage, process tué), la retransmission déclenche un RST
  // du peer en quelques secondes au lieu d'un TCP zombie éternel.
  static uint32_t lastPing = 0;
  if (now - lastPing >= 10000) {
    lastPing = now;
    sendFrame(0x06, nullptr, 0);
  }
}

static void netTask(void*) {
  // Heartbeat diagnostic toutes les secondes — hébergé ici pour rester
  // vivant même pendant un upload. Guard `if (Serial)` : sans terminal
  // ouvert, on ne bloque jamais sur le CDC.
  static uint32_t lastHb = 0;
  for (;;) {
    if (millis() - lastHb >= 1000) {
      lastHb = millis();
      if (Serial) {
        char nameNow[20];
        portENTER_CRITICAL(&nameMux);
        memcpy(nameNow, svcName, sizeof(nameNow));
        portEXIT_CRITICAL(&nameMux);
        if (wifiUp()) {
          Serial.printf("[hb] wifi OK ip=%s rssi=%d tcp=%d svc=%s:%u name=%s\n",
                        WiFi.localIP().toString().c_str(),
                        (int)WiFi.RSSI(), g_svcUp.load() ? 1 : 0,
                        serviceIp.toString().c_str(), servicePort, nameNow);
        } else {
          Serial.printf("[hb] wifi KO status=%d rssi=%d\n",
                        (int)WiFi.status(), (int)WiFi.RSSI());
        }
      }
    }

    step();

    // Envoi WAV demandé par l'UI : l'upload vit ici, l'UI respire.
    if (sendPending.load(std::memory_order_acquire)) {
      sendCancelled.store(false, std::memory_order_relaxed);
      bool ok = uploadWav(sendReq.hdr, sendReq.pcm, sendReq.bytes);
      sendPending.store(false, std::memory_order_relaxed);
      pushEvent(EV_SEND_RESULT, ok ? "ok" : "ko", 2);
    }

    // Entrée demandée par l'UI : émise ici, sans write bloquant côté UI.
    if (enterPending.exchange(false)) {
      sendFrame(0x05, nullptr, 0);
    }

    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void begin() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(onWifiEvent);
  if (Serial) Serial.printf("[wifi] demarrage, ssid=%s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  evQueue = xQueueCreate(8, sizeof(EventItem));
  xTaskCreatePinnedToCore(netTask, "sticks3-net", 8192, nullptr, 1, nullptr, 0);
}

bool wifiUp() { return WiFi.status() == WL_CONNECTED; }
bool svcUp()  { return g_svcUp.load(std::memory_order_relaxed); }

// Nom (sanitisé) du PC auquel le Stick est attaché, vide si inconnu.
void serviceName(char* buf, size_t cap) {
  portENTER_CRITICAL(&nameMux);
  snprintf(buf, cap, "%s", svcName);
  portEXIT_CRITICAL(&nameMux);
}

}  // namespace net