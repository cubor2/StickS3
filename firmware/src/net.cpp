// ------------------------------------------------------------
// net.cpp — client TCP + encadrement de trames.
// ------------------------------------------------------------
#include "net.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiUdp.h>

// Les anciens config.h locaux n'avaient pas encore ce réglage. La valeur
// reste conventionnelle avec le service PC sans imposer de modifier ce fichier.
#ifndef DISCOVERY_PORT
#define DISCOVERY_PORT 8789
#endif

namespace net {

static WiFiClient client;
static WiFiUDP discovery;
static uint32_t lastConnectTry = 0;
static uint32_t lastDiscoveryTry = 0;
static bool wasConnected = false;
static bool discoveryStarted = false;
static bool serviceKnown = false;
static IPAddress serviceIp;
static uint16_t servicePort = SERVICE_PORT;

static const char* DISCOVERY_REQUEST = "STICKS3_DISCOVER_V1";
static const char* DISCOVERY_RESPONSE = "STICKS3_HERE_V1 ";
// Un PC peut réclamer le Stick même s'il est connecté ailleurs : broadcast
// UDP entendu sur la même socket que la découverte, sans tour de main TCP.
static const char* DISCOVERY_CLAIM = "STICKS3_CLAIM_V1 ";

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
    if (strncmp(response, DISCOVERY_CLAIM, strlen(DISCOVERY_CLAIM)) == 0 &&
        sscanf(response + strlen(DISCOVERY_CLAIM), "%u", &port) == 1 &&
        port > 0 && port <= 65535) {
      IPAddress claimer = discovery.remoteIP();
      // Déjà attaché à ce PC : ne pas couper la connexion pour rien.
      if (!(claimer == serviceIp) || !client.connected()) {
        serviceIp = claimer;
        servicePort = (uint16_t)port;
        serviceKnown = true;
        if (client.connected()) {
          client.stop();
          wasConnected = false;
        }
        if (Serial) {
          Serial.printf("[discovery] claim : %s:%u\n",
                        claimer.toString().c_str(), servicePort);
        }
      }
    } else if (strncmp(response, DISCOVERY_RESPONSE, strlen(DISCOVERY_RESPONSE)) == 0 &&
        sscanf(response + strlen(DISCOVERY_RESPONSE), "%u", &port) == 1 &&
        port > 0 && port <= 65535) {
      serviceIp = discovery.remoteIP();
      servicePort = (uint16_t)port;
      serviceKnown = true;
      if (Serial) {
        Serial.printf("[discovery] service : %s:%u\n",
                      serviceIp.toString().c_str(), servicePort);
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

// --- sortie ---------------------------------------------------
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
    delay(0);
  }
  return true;
}

// sendWav : évite de dupliquer les 2 Mo de PCM — on émet le header
// puis le buffer directement.
bool sendWav(const uint8_t hdr44[44], const int16_t* pcm, size_t pcmBytes) {
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

  const uint8_t* bytes = (const uint8_t*)pcm;
  uint32_t sent = 0;
  while (sent < pcmBytes) {
    uint32_t chunk = min((uint32_t)4096, (uint32_t)(pcmBytes - sent));
    size_t n = client.write(bytes + sent, chunk);
    if (n == 0) return false;
    sent += n;
    delay(0);  // laisse respirer le WiFi
  }
  return true;
}

bool sendHello() {
  char payload[96];
  snprintf(payload, sizeof(payload), "%s\tfw-1.0", DEVICE_NAME);
  return sendFrame(0x01, (const uint8_t*)payload, strlen(payload));
}

bool sendEnter() {
  // Commande volontairement sans payload : pas de relais clavier arbitraire.
  return sendFrame(0x05, nullptr, 0);
}

// --- entrée : assemblage de trames ----------------------------
// Machine à états minimale, non bloquante.
enum { RD_LEN = 0, RD_TYPE = 1, RD_PAYLOAD = 2 };
static uint8_t  rdPhase = RD_LEN;
static uint8_t  rdHdr[4];
static uint8_t  rdHdrGot = 0;
static uint8_t  rdType = 0;
static uint32_t rdLen = 0;
static uint32_t rdGot = 0;
static uint8_t  rxBuf[768];

// Petite file de 4 événements (titre/message/extrait).
static const int EV_SLOTS = 4;
static char  evBuf[EV_SLOTS][384];
static uint8_t evType[EV_SLOTS];
static uint8_t evHead = 0, evTail = 0;

static void pushEvent(uint8_t type, const uint8_t* p, uint32_t len) {
  uint8_t slot = evHead;
  uint32_t n = min((uint32_t)(sizeof(evBuf[0]) - 1), len);
  memcpy(evBuf[slot], p, n);
  evBuf[slot][n] = 0;
  evType[slot] = type;
  evHead = (uint8_t)((evHead + 1) % EV_SLOTS);
  if (evHead == evTail) evTail = (uint8_t)((evTail + 1) % EV_SLOTS);  // écrase le plus ancien
}

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
        if (rdLen == 0) pushEvent(rdType, (const uint8_t*)"", 0);
        break;
      case RD_PAYLOAD: {
        if (rdGot < sizeof(rxBuf)) rxBuf[rdGot] = b;
        rdGot++;
        if (rdGot >= rdLen) {
          pushEvent(rdType, rxBuf, min(rdGot, (uint32_t)sizeof(rxBuf)));
          rdPhase = RD_LEN;
        }
        break;
      }
    }
  }
}

uint8_t pollEvent(char* buf, size_t cap) {
  if (evTail == evHead) return 0;
  uint8_t t = evType[evTail];
  snprintf(buf, cap, "%s", evBuf[evTail]);
  evTail = (uint8_t)((evTail + 1) % EV_SLOTS);
  return t;
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

void begin() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(onWifiEvent);
  if (Serial) Serial.printf("[wifi] demarrage, ssid=%s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

bool wifiUp() { return WiFi.status() == WL_CONNECTED; }
bool svcUp()  { return client.connected(); }

void loop() {
  // Heartbeat INCONDITIONNEL toutes les secondes — le diagnostic série
  // doit marcher quel que soit l'état. Guard `if (Serial)` : sans
  // terminal ouvert, on ne risque jamais de bloquer sur le CDC.
  static uint32_t lastHb = 0;
  if (millis() - lastHb >= 1000) {
    lastHb = millis();
    if (Serial) {
      if (wifiUp()) {
        Serial.printf("[hb] wifi OK ip=%s rssi=%d tcp=%d\n",
                      WiFi.localIP().toString().c_str(),
                      (int)WiFi.RSSI(), client.connected() ? 1 : 0);
      } else {
        Serial.printf("[hb] wifi KO status=%d rssi=%d\n",
                      (int)WiFi.status(), (int)WiFi.RSSI());
      }
    }
  }

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
    return;
  }

  uint32_t now = millis();
  discoveryTick(now);

  if (!client.connected()) {
    if (wasConnected) {
      wasConnected = false;
    }
    if (serviceKnown && now - lastConnectTry > 2000) {
      lastConnectTry = now;
      client.stop();
      // La boucle principale gère aussi l'écran et les boutons : une
      // reconnexion vers un PC/service absent ne doit jamais la geler.
      // L'overload ESP32 permet une borne stricte en millisecondes.
      bool ok = client.connect(serviceIp, servicePort, 50);
      if (Serial) {
        Serial.printf("[tcp] connect %s:%u -> %s\n",
                      serviceIp.toString().c_str(), servicePort,
                      ok ? "OK" : "KO");
      }
      if (ok) {
        client.setNoDelay(true);
        wasConnected = true;
        sendHello();
      }
    }
    return;
  }

  wasConnected = true;
  pumpInbound();
}

}  // namespace net
