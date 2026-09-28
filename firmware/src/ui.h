// ------------------------------------------------------------
// ui.h — interface portrait « Nori Face » du StickS3.
// ------------------------------------------------------------
#pragma once
#include "M5Unified.h"

namespace ui {

static const uint16_t FACE_MINT  = 0xB79B;
static const uint16_t FACE_INK   = 0x1124;
static const uint16_t FACE_PALE  = 0xEFFF;
static const uint16_t FACE_GREEN = 0x3E98;
static const uint16_t FACE_WARM  = 0xFD88;
static const uint16_t FACE_RED   = 0xD965;

void begin();
void bootSplash();
void wifiWaiting(uint32_t now);
void wifiTick(uint32_t now);
void idle(bool svcUp, const char* pcName, uint32_t now);
void volume(int step, int maxStep);
void rec(uint32_t elapsedMs, uint32_t maxMs);  // dessin complet, 1x
void recTime(uint32_t elapsedMs, uint32_t maxMs);
void recVU(int level);
void sending();
void waiting(uint32_t now);
void waitingTick(uint32_t now);     // spinner uniquement
void enter(uint32_t now);            // validation bouton Entrée
void enterTick(uint32_t now);
void done(const char* text);
void error(const char* detail);
void cancelled();
void notify(const char* title, const char* message, bool frites);
void setDimmed(bool dim);
void setScreenSleeping(bool sleeping);

}  // namespace ui
