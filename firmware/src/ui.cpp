// ------------------------------------------------------------
// ui.cpp — Nori Face : interface portrait, calme, sans chrome.
// Le visage est l'état ; les seuls signes fonctionnels sont le chrono
// et le niveau pendant l'enregistrement.
// ------------------------------------------------------------
#include "ui.h"
#include <math.h>

namespace ui {

static constexpr int W = 135;
static constexpr int H = 240;
static constexpr int MIC_CY = 112;  // ancre visuelle, identique dans tous les états

static void fontSmall() { M5.Lcd.setFont(&fonts::Font2); M5.Lcd.setTextSize(1); }
static void clear(uint16_t color = FACE_MINT) { M5.Lcd.fillScreen(color); }

static void stroke(int x1, int y1, int x2, int y2, uint16_t color, int width = 1) {
  M5.Lcd.drawLine(x1, y1, x2, y2, color);
  if (width >= 2) M5.Lcd.drawLine(x1 + 1, y1, x2 + 1, y2, color);
  if (width >= 3) M5.Lcd.drawLine(x1, y1 + 1, x2, y2 + 1, color);
}

static void microphone(int cx, int cy, uint16_t fill = FACE_MINT) {
  // Icône mic en trait, volontairement sans caractère anthropomorphique.
  M5.Lcd.fillRoundRect(cx - 16, cy - 38, 32, 54, 16, FACE_INK);
  M5.Lcd.fillRoundRect(cx - 12, cy - 34, 24, 46, 12, fill);
  M5.Lcd.drawLine(cx - 24, cy - 2, cx - 24, cy + 10, FACE_INK);
  M5.Lcd.drawLine(cx + 24, cy - 2, cx + 24, cy + 10, FACE_INK);
  M5.Lcd.drawLine(cx - 24, cy + 10, cx - 14, cy + 22, FACE_INK);
  M5.Lcd.drawLine(cx + 24, cy + 10, cx + 14, cy + 22, FACE_INK);
  M5.Lcd.drawLine(cx - 14, cy + 22, cx + 14, cy + 22, FACE_INK);
  M5.Lcd.drawLine(cx, cy + 22, cx, cy + 35, FACE_INK);
  M5.Lcd.drawLine(cx - 18, cy + 35, cx + 18, cy + 35, FACE_INK);
}

static void micIdle() { clear(); microphone(W / 2, MIC_CY); }

static void micListening() { clear(); microphone(W / 2, MIC_CY, FACE_GREEN); }

static void processingSpinner(uint32_t now) {
  // Roue de chargement lisible juste sous le micro.
  constexpr int cx = W / 2, cy = 177;
  M5.Lcd.fillRect(cx - 22, cy - 22, 45, 45, FACE_MINT);
  int active = (now / 90) % 12;
  for (int i = 0; i < 12; ++i) {
    float a = (float)i * 0.52359878f;  // 2π / 12
    int x1 = cx + (int)(cosf(a) * 9);
    int y1 = cy + (int)(sinf(a) * 9);
    int x2 = cx + (int)(cosf(a) * 17);
    int y2 = cy + (int)(sinf(a) * 17);
    int delta = (i - active + 12) % 12;
    // Base en encre : contraste net sur le menthe. La traînée verte porte
    // seule le mouvement, au lieu de disparaître dans un blanc trop pâle.
    uint16_t color = delta <= 3 ? FACE_GREEN : FACE_INK;
    stroke(x1, y1, x2, y2, color, 3);
  }
}

static void micThinking(uint32_t now) {
  clear();
  // Même remplissage que pendant l'écoute : aucune micro-variation parasite
  // au moment précis où l'envoi commence.
  microphone(W / 2, MIC_CY, FACE_GREEN);
  processingSpinner(now);
}

static int lastEnterFrame = -1;
static uint32_t enterStartedAt = 0;

static void micEnterFrame(uint32_t now) {
  // Animation calée sur l'appui, jamais sur une phase arbitraire de millis().
  uint32_t e = now - enterStartedAt;
  int frame = (int)(e / 90);
  if (frame == lastEnterFrame) return;
  lastEnterFrame = frame;

  clear(FACE_GREEN);
  microphone(W / 2, MIC_CY, FACE_MINT);  // ancre : jamais déplacée

  // 1. Ondes de validation qui s'échappent du micro (0 → 1,15 s).
  if (e < 1150) {
    for (int k = 0; k < 2; ++k) {
      uint32_t ph = (e + (uint32_t)k * 370) % 740;
      if (ph > 670) continue;             // l'onde meurt avant de boucler
      int r = 36 + (int)(ph * 52 / 740);  // 36 → 88 px
      M5.Lcd.drawCircle(W / 2, MIC_CY, r, FACE_MINT);
      if (ph < 370) M5.Lcd.drawCircle(W / 2, MIC_CY, r + 1, FACE_MINT);
    }
  }

}

static void micDone() {
  clear();
  microphone(W / 2, MIC_CY);
  stroke(94, 157, 101, 164, FACE_GREEN, 3);
  stroke(101, 164, 115, 146, FACE_GREEN, 3);
}

static void micError() {
  clear(FACE_PALE);
  microphone(W / 2, MIC_CY, FACE_PALE);
  M5.Lcd.drawLine(98, 151, 114, 167, FACE_RED);
  M5.Lcd.drawLine(114, 151, 98, 167, FACE_RED);
}

static void drawTime(uint32_t elapsedMs) {
  char buf[8];
  uint32_t total = elapsedMs / 1000;
  snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)(total / 60),
           (unsigned long)(total % 60));
  M5.Lcd.fillRect(38, 204, 60, 16, FACE_MINT);
  fontSmall();
  M5.Lcd.setTextDatum(middle_center);
  M5.Lcd.setTextColor(FACE_INK);
  M5.Lcd.drawString(buf, W / 2, 212);
}

static void wave(int level) {
  // Zone volontairement plus large que les barres : évite tout pixel fantôme
  // à droite quand la dernière barre rétrécit.
  M5.Lcd.fillRect(16, 170, 104, 29, FACE_MINT);
  const int bars = 9;
  for (int i = 0; i < bars; ++i) {
    int distance = abs(i - bars / 2);
    int h = 3 + max(0, level - distance * 2);
    M5.Lcd.fillRoundRect(24 + i * 11, 185 - h / 2, 5, h, 2, FACE_INK);
  }
}

void begin() {
  M5.Lcd.setRotation(0);  // portrait natif : 135 x 240
  M5.Lcd.setTextDatum(middle_center);
  M5.Lcd.setBrightness(80);
  fontSmall();
}

void bootSplash() {
  // Boot sobre : une apparition nette, puis le chargement Wi-Fi prend le
  // relais. Deux animations consécutives sur ce petit écran se parasitent.
  micIdle();
  delay(350);
}

void wifiWaiting(uint32_t now) { micThinking(now); }

// Le micro est déjà dessiné par wifiWaiting(). Ici, on n'anime que la roue :
// redessiner tout l'écran à chaque loop produisait le clignotement au boot.
void wifiTick(uint32_t now) { processingSpinner(now); }

void idle(bool, uint32_t) { micIdle(); }

void rec(uint32_t elapsedMs, uint32_t) {
  micListening();
  wave(0);
  drawTime(elapsedMs);
}

void recTime(uint32_t elapsedMs, uint32_t) { drawTime(elapsedMs); }

void recVU(int level) { wave(level); }

void sending() { micThinking(millis()); }

void waiting(uint32_t now) { micThinking(now); }

void waitingTick(uint32_t now) { processingSpinner(now); }

void enter(uint32_t now) {
  enterStartedAt = now;
  lastEnterFrame = -1;
  micEnterFrame(now);
}

void enterTick(uint32_t now) { micEnterFrame(now); }

void done(const char*) { micDone(); }

void error(const char*) { micError(); }

void cancelled() {
  micIdle();
}

void notify(const char*, const char*, bool) {
  micDone();
}

void setDimmed(bool dim) { M5.Lcd.setBrightness(dim ? 15 : 80); }

void setScreenSleeping(bool sleeping) {
  if (sleeping) {
    M5.Lcd.setBrightness(0);
    M5.Display.sleep();
  } else {
    M5.Display.wakeup();
    M5.Lcd.setBrightness(80);
  }
}

}  // namespace ui
