// ------------------------------------------------------------
// net.h — WiFi + protocole binaire minimal vers le service PC.
//
// Trame : [u32 le longueur_payload][u8 type][payload]
//   0x01 HELLO       stick→svc   nom \t firmware \t jeton
//   0x02 WAV         stick→svc   WAV complet (header 44 o + PCM s16le mono)
//   0x03 NOTIFY      svc→stick   titre \t message \t son (frites|ok|error)
//   0x04 TRANSCRIPT  svc→stick   statut \t message \t extrait (statut: ok|error)
//   0x05 ENTER       stick→svc   appui Entrée fixe dans la fenêtre active
//   0x06 PING        stick→svc   keepalive applicatif (payload vide, sans réponse)
// Découverte UDP : requête "STICKS3_DISCOVER_V1" en broadcast, réponse
// "STICKS3_HERE_V1 <port_tcp> <nom_pc>" ; réclamation "STICKS3_CLAIM_V1
// <port_tcp> <nom_pc>" pour attacher le Stick à un PC précis.
// ------------------------------------------------------------
#pragma once
#include <Arduino.h>

namespace net {

void begin();
bool wifiUp();
bool svcUp();
void serviceName(char* buf, size_t cap);  // nom du PC attaché, "" si inconnu
void loop();          // reconnexion + pompage des événements entrants

bool sendHello();
bool sendWav(const uint8_t hdr44[44], const int16_t* pcm, size_t pcmBytes);
bool sendEnter();

// File d'événements entrants (NOTIFY / TRANSCRIPT) — champs \t-séparés.
// Renvoie le type de trame (0 si rien) et remplit buf.
uint8_t pollEvent(char* buf, size_t cap);

}  // namespace net
