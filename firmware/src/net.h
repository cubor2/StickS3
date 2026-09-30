// ------------------------------------------------------------
// net.h — WiFi + protocole binaire minimal vers le service PC.
//
// Tout le réseau tourne dans une tâche FreeRTOS dédiée (net.cpp) :
// l'UI n'appelle ici que des fonctions NON bloquantes. Les demandes
// d'envoi sont déléguées ; les résultats remontent par la file.
//
// Trame : [u32 le longueur_payload][u8 type][payload]
//   0x01 HELLO       stick→svc   nom \t firmware \t jeton
//   0x02 WAV         stick→svc   WAV complet (header 44 o + PCM s16le mono)
//   0x03 NOTIFY      svc→stick   titre \t message \t son (frites|ok|error)
//   0x04 TRANSCRIPT  svc→stick   statut \t message \t extrait (statut: ok|error)
//   0x05 ENTER       stick→svc   appui Entrée fixe dans la fenêtre active
//   0x06 PING        stick→svc   keepalive applicatif (payload vide, sans réponse)
//   0xFD             interne     résultat d'upload WAV : "ok" | "ko"
// Découverte UDP : requête "STICKS3_DISCOVER_V1" en broadcast, réponse
// "STICKS3_HERE_V1 <port_tcp> <nom_pc>" ; réclamation "STICKS3_CLAIM_V1
// <port_tcp> <nom_pc>" pour attacher le Stick à un PC précis. Une réponse
// de découverte peut porter un 4e champ "CLAIM" : le PC demandé réclame le
// Stick via la sonde du Stick lui-même (unicast, fiable entre bandes).
// ------------------------------------------------------------
#pragma once
#include <Arduino.h>

namespace net {

void begin();
bool wifiUp();
bool svcUp();          // atomique : la tâche réseau le tient à jour
void serviceName(char* buf, size_t cap);  // nom du PC attaché, "" si inconnu

// Délègue l'upload du WAV à la tâche réseau (non bloquant). false si un
// envoi est déjà en vol. Le résultat remonte via pollEvent (0xFD).
bool requestSend(const uint8_t hdr44[44], const int16_t* pcm, size_t pcmBytes);
// Avorte l'envoi en vol (plafond d'attente UI atteint). Prend effet entre
// deux chunks ; un write déjà bloqué attendra lwIP.
void cancelSend();
// Délègue l'appui Entrée. true si le service est joignable.
bool requestEnter();
// Vrai tant que l'upload du WAV est en vol (pour geler le timer de patience).
bool sendBusy();

// File d'événements entrants (NOTIFY / TRANSCRIPT / 0xFD) — champs
// \t-séparés. Renvoie le type (0 si rien) et remplit buf.
uint8_t pollEvent(char* buf, size_t cap);

}  // namespace net