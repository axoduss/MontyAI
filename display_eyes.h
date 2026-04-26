#ifndef DISPLAY_EYES_H
#define DISPLAY_EYES_H

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ─── DISPLAY CONFIG ──────────────────────────────────────────────────────────
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64
#define OLED_RESET      -1
#define SCREEN_ADDRESS  0x3C
#define DISPLAY_SDA     GPIO_NUM_17
#define DISPLAY_SCL     GPIO_NUM_18

// ─── DISPLAY MODE ────────────────────────────────────────────────────────────
enum DisplayMode {
  DMODE_EYES,       // Modalità occhi espressivi
  DMODE_TEXT,       // Testo libero (deciso da LLM)
  DMODE_ICON,       // Icona + testo
  DMODE_PROGRESS,   // Barra di progresso
  DMODE_CUSTOM_GFX, // Grafica custom (pixel art, forme)
  DMODE_SPLIT       // Metà occhi, metà info
};

// ─── ESPRESSIONI OCCHI ───────────────────────────────────────────────────────
enum EyeExpression {
  EXP_NEUTRAL,      // Occhi normali, aperti
  EXP_HAPPY,        // Occhi sorridenti (arco in basso)
  EXP_SAD,          // Occhi tristi (arco in alto, palpebre cadenti)
  EXP_ANGRY,        // Occhi arrabbiati (sopracciglia inclinate)
  EXP_SURPRISED,    // Occhi spalancati grandi
  EXP_SLEEPY,       // Occhi semichiusi
  EXP_THINKING,     // Un occhio più piccolo, sguardo in alto
  EXP_LOVE,         // Occhi a cuore
  EXP_WINK,         // Occhiolino (sinistro chiuso)
  EXP_SKEPTICAL,    // Un sopracciglio alzato
  EXP_EXCITED,      // Occhi grandi + stelline
  EXP_CONFUSED,     // Occhi asimmetrici, spirale
  EXP_BLINK,        // Stato intermedio per blink
  EXP_COUNT         // Numero totale espressioni
};

// ─── DIREZIONE SGUARDO ───────────────────────────────────────────────────────
enum LookDirection {
  LOOK_CENTER,
  LOOK_LEFT,
  LOOK_RIGHT,
  LOOK_UP,
  LOOK_DOWN
};

// ─── STRUTTURA STATO DISPLAY ─────────────────────────────────────────────────
struct DisplayState {
  // Modalità corrente
  DisplayMode mode = DMODE_EYES;
  
  // Stato occhi
  EyeExpression expression = EXP_NEUTRAL;
  EyeExpression targetExpression = EXP_NEUTRAL;
  LookDirection lookDir = LOOK_CENTER;
  
  // Animazione blink
  bool isBlinking = false;
  uint32_t nextBlinkMs = 0;
  uint32_t blinkStartMs = 0;
  uint8_t blinkPhase = 0;  // 0=aperto, 1=chiudendo, 2=chiuso, 3=aprendo
  
  // Animazione sguardo casuale
  uint32_t nextLookMs = 0;
  
  // Transizione espressione
  uint32_t expressionChangeMs = 0;
  bool inTransition = false;
  uint8_t transitionFrame = 0;
  
  // Contenuto custom (per LLM)
  char textLine1[32] = "";
  char textLine2[32] = "";
  char textLine3[32] = "";
  char textLine4[32] = "";
  uint8_t textSize = 1;
  
  // Progress bar
  uint8_t progressPercent = 0;
  char progressLabel[24] = "";
  
  // Icona
  uint8_t iconId = 0;
  char iconText[32] = "";
  
  // Timeout per tornare agli occhi
  uint32_t customModeUntilMs = 0;
  
  // Parametri occhi (per animazione fluida)
  float leftEyeOpenness = 1.0f;   // 0.0 = chiuso, 1.0 = aperto
  float rightEyeOpenness = 1.0f;
  float pupilOffsetX = 0.0f;      // -1.0 .. +1.0
  float pupilOffsetY = 0.0f;
  float leftBrowAngle = 0.0f;     // angolo sopracciglio
  float rightBrowAngle = 0.0f;
  float leftBrowHeight = 0.0f;    // offset verticale sopracciglio
  float rightBrowHeight = 0.0f;
};

// ─── CLASSE DISPLAY MANAGER ─────────────────────────────────────────────────
class DisplayManager {
public:
  Adafruit_SSD1306 oled;
  DisplayState state;
  SemaphoreHandle_t mutex;
  
  DisplayManager() : oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET) {}
  
  // ── Inizializzazione ────────────────────────────────────────────────────
  bool begin() {
    mutex = xSemaphoreCreateMutex();
    
    Wire.begin(DISPLAY_SDA, DISPLAY_SCL);
    Wire.setClock(400000);  // 400kHz I2C fast mode
    
    if (!oled.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
      Serial.println("[DISP] SSD1306 ERRORE!");
      return false;
    }
    
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.display();
    
    // Inizializza timing animazioni
    state.nextBlinkMs = millis() + random(2000, 5000);
    state.nextLookMs = millis() + random(3000, 8000);
    
    Serial.println("[DISP] SSD1306 pronto.");
    return true;
  }
  
  // ── Imposta espressione (thread-safe) ───────────────────────────────────
  void setExpression(EyeExpression exp) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (state.expression != exp) {
      state.targetExpression = exp;
      state.inTransition = true;
      state.transitionFrame = 0;
      state.expressionChangeMs = millis();
    }
    xSemaphoreGive(mutex);
  }
  
  // ── Imposta direzione sguardo ───────────────────────────────────────────
  void setLookDirection(LookDirection dir) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    state.lookDir = dir;
    xSemaphoreGive(mutex);
  }
  
  // ── Mostra testo (da LLM) ──────────────────────────────────────────────
  void showText(const char* l1, const char* l2 = "", 
                const char* l3 = "", const char* l4 = "",
                uint8_t size = 1, uint32_t durationMs = 5000) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    state.mode = DMODE_TEXT;
    strncpy(state.textLine1, l1, sizeof(state.textLine1) - 1);
    strncpy(state.textLine2, l2, sizeof(state.textLine2) - 1);
    strncpy(state.textLine3, l3, sizeof(state.textLine3) - 1);
    strncpy(state.textLine4, l4, sizeof(state.textLine4) - 1);
    state.textSize = size;
    state.customModeUntilMs = (durationMs > 0) ? millis() + durationMs : 0;
    xSemaphoreGive(mutex);
  }
  
  // ── Mostra progress bar ─────────────────────────────────────────────────
  void showProgress(uint8_t percent, const char* label = "", 
                    uint32_t durationMs = 0) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    state.mode = DMODE_PROGRESS;
    state.progressPercent = constrain(percent, 0, 100);
    strncpy(state.progressLabel, label, sizeof(state.progressLabel) - 1);
    state.customModeUntilMs = (durationMs > 0) ? millis() + durationMs : 0;
    xSemaphoreGive(mutex);
  }
  
  // ── Torna alla modalità occhi ───────────────────────────────────────────
  void showEyes() {
    xSemaphoreTake(mutex, portMAX_DELAY);
    state.mode = DMODE_EYES;
    state.customModeUntilMs = 0;
    xSemaphoreGive(mutex);
  }
  
  // ══════════════════════════════════════════════════════════════════════════
  // UPDATE PRINCIPALE — chiamato dal task display ~30fps
  // ══════════════════════════════════════════════════════════════════════════
  void update() {
    xSemaphoreTake(mutex, portMAX_DELAY);
    
    uint32_t now = millis();
    
    // Check timeout modalità custom → torna a occhi
    if (state.mode != DMODE_EYES && state.customModeUntilMs > 0 
        && now >= state.customModeUntilMs) {
      state.mode = DMODE_EYES;
      state.customModeUntilMs = 0;
    }
    
    oled.clearDisplay();
    
    switch (state.mode) {
      case DMODE_EYES:
        updateAnimations(now);
        drawEyes();
        break;
      case DMODE_TEXT:
        drawText();
        break;
      case DMODE_PROGRESS:
        drawProgress();
        break;
      case DMODE_ICON:
        drawIcon();
        break;
      case DMODE_SPLIT:
        drawSplit();
        break;
      default:
        drawEyes();
        break;
    }
    
    oled.display();
    xSemaphoreGive(mutex);
  }

private:
  // ══════════════════════════════════════════════════════════════════════════
  // ANIMAZIONI AUTONOME
  // ══════════════════════════════════════════════════════════════════════════
  
  void updateAnimations(uint32_t now) {
    // ── Blink automatico ────────────────────────────────────────────────
    if (!state.isBlinking && now >= state.nextBlinkMs) {
      state.isBlinking = true;
      state.blinkStartMs = now;
      state.blinkPhase = 1;  // inizia a chiudere
    }
    
    if (state.isBlinking) {
      uint32_t elapsed = now - state.blinkStartMs;
      
      if (elapsed < 60) {
        // Chiudendo
        float t = elapsed / 60.0f;
        state.leftEyeOpenness = 1.0f - t;
        state.rightEyeOpenness = 1.0f - t;
      } else if (elapsed < 120) {
        // Chiuso
        state.leftEyeOpenness = 0.05f;
        state.rightEyeOpenness = 0.05f;
      } else if (elapsed < 200) {
        // Aprendo
        float t = (elapsed - 120) / 80.0f;
        state.leftEyeOpenness = t;
        state.rightEyeOpenness = t;
      } else {
        // Fine blink
        state.leftEyeOpenness = 1.0f;
        state.rightEyeOpenness = 1.0f;
        state.isBlinking = false;
        // Prossimo blink: 2-6 secondi (a volte doppio blink)
        state.nextBlinkMs = now + random(2000, 6000);
      }
    }
    
    // ── Sguardo casuale ─────────────────────────────────────────────────
    if (now >= state.nextLookMs && !state.isBlinking) {
      int r = random(100);
      if (r < 40) {
        state.lookDir = LOOK_CENTER;
      } else if (r < 55) {
        state.lookDir = LOOK_LEFT;
      } else if (r < 70) {
        state.lookDir = LOOK_RIGHT;
      } else if (r < 85) {
        state.lookDir = LOOK_UP;
      } else {
        state.lookDir = LOOK_DOWN;
      }
      state.nextLookMs = now + random(2000, 7000);
    }
    
    // ── Smooth pupil movement ───────────────────────────────────────────
    float targetX = 0, targetY = 0;
    switch (state.lookDir) {
      case LOOK_LEFT:   targetX = -1.0f; break;
      case LOOK_RIGHT:  targetX =  1.0f; break;
      case LOOK_UP:     targetY = -1.0f; break;
      case LOOK_DOWN:   targetY =  1.0f; break;
      default: break;
    }
    // Lerp per movimento fluido
    state.pupilOffsetX += (targetX - state.pupilOffsetX) * 0.15f;
    state.pupilOffsetY += (targetY - state.pupilOffsetY) * 0.15f;
    
    // ── Transizione espressione ─────────────────────────────────────────
    if (state.inTransition) {
      state.transitionFrame++;
      if (state.transitionFrame >= 4) {
        state.expression = state.targetExpression;
        state.inTransition = false;
      }
    }
    
    // ── Aggiorna parametri in base all'espressione ──────────────────────
    updateExpressionParams();
  }

// ══════════════════════════════════════════════════════════════════════════
  // PARAMETRI ESPRESSIONE
  // ══════════════════════════════════════════════════════════════════════════
  
  void updateExpressionParams() {
    // Target values per ogni espressione
    float tLeftOpen = 1.0f, tRightOpen = 1.0f;
    float tLeftBrowAngle = 0.0f, tRightBrowAngle = 0.0f;
    float tLeftBrowH = 0.0f, tRightBrowH = 0.0f;
    
    EyeExpression exp = state.inTransition ? state.targetExpression : state.expression;
    
    switch (exp) {
      case EXP_NEUTRAL:
        // Defaults già ok
        break;
        
      case EXP_HAPPY:
        tLeftOpen = 0.7f;
        tRightOpen = 0.7f;
        tLeftBrowH = -2.0f;
        tRightBrowH = -2.0f;
        break;
        
      case EXP_SAD:
        tLeftOpen = 0.6f;
        tRightOpen = 0.6f;
        tLeftBrowAngle = 15.0f;    // sopracciglia inclinate verso centro-alto
        tRightBrowAngle = -15.0f;
        tLeftBrowH = 3.0f;
        tRightBrowH = 3.0f;
        break;
        
      case EXP_ANGRY:
        tLeftOpen = 0.7f;
        tRightOpen = 0.7f;
        tLeftBrowAngle = -20.0f;   // sopracciglia inclinate verso centro-basso
        tRightBrowAngle = 20.0f;
        tLeftBrowH = -1.0f;
        tRightBrowH = -1.0f;
        break;
        
      case EXP_SURPRISED:
        tLeftOpen = 1.3f;   // occhi più grandi del normale
        tRightOpen = 1.3f;
        tLeftBrowH = -5.0f;
        tRightBrowH = -5.0f;
        break;
        
      case EXP_SLEEPY:
        tLeftOpen = 0.3f;
        tRightOpen = 0.35f;
        tLeftBrowH = 2.0f;
        tRightBrowH = 2.0f;
        break;
        
      case EXP_THINKING:
        tLeftOpen = 0.6f;
        tRightOpen = 0.9f;
        tLeftBrowAngle = 10.0f;
        tRightBrowAngle = -5.0f;
        tRightBrowH = -3.0f;
        // Sguardo in alto a destra
        state.pupilOffsetX = 0.6f;
        state.pupilOffsetY = -0.5f;
        break;
        
      case EXP_LOVE:
        tLeftOpen = 0.9f;
        tRightOpen = 0.9f;
        break;
        
      case EXP_WINK:
        tLeftOpen = 0.05f;   // occhio sinistro chiuso
        tRightOpen = 1.0f;
        tRightBrowH = -2.0f;
        break;
        
      case EXP_SKEPTICAL:
        tLeftOpen = 0.5f;
        tRightOpen = 1.0f;
        tLeftBrowAngle = 10.0f;
        tRightBrowAngle = 0.0f;
        tRightBrowH = -4.0f;
        break;
        
      case EXP_EXCITED:
        tLeftOpen = 1.2f;
        tRightOpen = 1.2f;
        tLeftBrowH = -4.0f;
        tRightBrowH = -4.0f;
        break;
        
      case EXP_CONFUSED:
        tLeftOpen = 0.8f;
        tRightOpen = 1.1f;
        tLeftBrowAngle = 12.0f;
        tRightBrowAngle = -8.0f;
        tLeftBrowH = 1.0f;
        tRightBrowH = -3.0f;
        break;
        
      default:
        break;
    }
    
    // Lerp fluido verso i target (non sovrascrivere se blink attivo)
    float lerpSpeed = 0.12f;
    
    if (!state.isBlinking) {
      state.leftEyeOpenness  += (tLeftOpen - state.leftEyeOpenness) * lerpSpeed;
      state.rightEyeOpenness += (tRightOpen - state.rightEyeOpenness) * lerpSpeed;
    }
    
    state.leftBrowAngle  += (tLeftBrowAngle - state.leftBrowAngle) * lerpSpeed;
    state.rightBrowAngle += (tRightBrowAngle - state.rightBrowAngle) * lerpSpeed;
    state.leftBrowHeight += (tLeftBrowH - state.leftBrowHeight) * lerpSpeed;
    state.rightBrowHeight += (tRightBrowH - state.rightBrowHeight) * lerpSpeed;
  }
  
  // ══════════════════════════════════════════════════════════════════════════
  // DISEGNO OCCHI
  // ══════════════════════════════════════════════════════════════════════════
  
  void drawEyes() {
    // Posizioni centro occhi
    const int16_t leftEyeCX  = 32;
    const int16_t rightEyeCX = 96;
    const int16_t eyeCY      = 32;
    
    // Dimensioni base occhio
    const int16_t eyeRadiusX = 18;
    const int16_t eyeRadiusY = 20;
    const int16_t pupilRadius = 5;
    
    // Disegna occhio sinistro
    drawSingleEye(leftEyeCX, eyeCY, eyeRadiusX, eyeRadiusY, pupilRadius,
                  state.leftEyeOpenness, state.leftBrowAngle, state.leftBrowHeight,
                  true);
    
    // Disegna occhio destro
    drawSingleEye(rightEyeCX, eyeCY, eyeRadiusX, eyeRadiusY, pupilRadius,
                  state.rightEyeOpenness, state.rightBrowAngle, state.rightBrowHeight,
                  false);
    
    // Decorazioni speciali per alcune espressioni
    EyeExpression exp = state.expression;
    
    if (exp == EXP_HAPPY) {
      drawHappyMouth();
    } else if (exp == EXP_SAD) {
      drawSadMouth();
    } else if (exp == EXP_LOVE) {
      drawHeartEyes(leftEyeCX, eyeCY);
      drawHeartEyes(rightEyeCX, eyeCY);
    } else if (exp == EXP_EXCITED) {
      drawSparkles();
    } else if (exp == EXP_CONFUSED) {
      drawQuestionMark();
    }
  }
  
  void drawSingleEye(int16_t cx, int16_t cy, int16_t rx, int16_t ry,
                      int16_t pupilR, float openness, float browAngle,
                      float browHeight, bool isLeft) {
    
    // Scala verticale in base a openness
    int16_t actualRY = (int16_t)(ry * constrain(openness, 0.05f, 1.5f));
    
    if (openness < 0.1f) {
      // Occhio chiuso — linea orizzontale
      oled.drawFastHLine(cx - rx, cy, rx * 2, SSD1306_WHITE);
      return;
    }
    
    // ── Contorno occhio (ellisse) ─────────────────────────────────────────
    // Disegna ellisse piena bianca, poi riempi interno nero, poi pupilla
    
    // Bordo esterno bianco
    drawEllipse(cx, cy, rx, actualRY, true);
    
    // Interno nero (2px più piccolo)
    if (rx > 3 && actualRY > 3) {
      drawEllipseFilled(cx, cy, rx - 2, actualRY - 2, SSD1306_BLACK);
    }
    
    // ── Pupilla ───────────────────────────────────────────────────────────
    // Offset pupilla in base allo sguardo
    int16_t maxPupilOffX = rx - pupilR - 4;
    int16_t maxPupilOffY = actualRY - pupilR - 4;
    
    int16_t pupilX = cx + (int16_t)(state.pupilOffsetX * maxPupilOffX);
    int16_t pupilY = cy + (int16_t)(state.pupilOffsetY * maxPupilOffY);
    
    // Pupilla piena bianca
    oled.fillCircle(pupilX, pupilY, pupilR, SSD1306_WHITE);
    
    // Riflesso luce (piccolo punto nero in alto a destra della pupilla)
    oled.fillCircle(pupilX + 2, pupilY - 2, 1, SSD1306_BLACK);
    
    // ── Sopracciglio ──────────────────────────────────────────────────────
    int16_t browY = cy - actualRY - 4 + (int16_t)browHeight;
    int16_t browLen = rx + 4;
    
    // Calcola punti sopracciglio con angolo
    float angleRad = browAngle * PI / 180.0f;
    int16_t dx = (int16_t)(browLen * cos(angleRad));
    int16_t dy = (int16_t)(browLen * sin(angleRad));
    
    int16_t browStartX = cx - dx / 2;
    int16_t browStartY = browY - dy / 2;
    int16_t browEndX   = cx + dx / 2;
    int16_t browEndY   = browY + dy / 2;
    
    // Sopracciglio spesso (2 linee)
    oled.drawLine(browStartX, browStartY, browEndX, browEndY, SSD1306_WHITE);
    oled.drawLine(browStartX, browStartY - 1, browEndX, browEndY - 1, SSD1306_WHITE);
    
    // ── Palpebra (per espressioni tipo sleepy/angry) ──────────────────────
    if (openness < 0.8f && openness >= 0.1f) {
      // Disegna palpebra come rettangolo nero sopra l'occhio
      int16_t lidHeight = (int16_t)((1.0f - openness) * actualRY * 0.8f);
      oled.fillRect(cx - rx - 1, cy - actualRY - 1, rx * 2 + 2, lidHeight, SSD1306_BLACK);
    }
  }
  
  // ── Helper: Ellisse contorno ────────────────────────────────────────────
  void drawEllipse(int16_t cx, int16_t cy, int16_t rx, int16_t ry, bool white) {
    // Algoritmo Bresenham per ellisse
    int16_t x = 0, y = ry;
    int32_t rx2 = (int32_t)rx * rx;
    int32_t ry2 = (int32_t)ry * ry;
    int32_t twoRx2 = 2 * rx2;
    int32_t twoRy2 = 2 * ry2;
    int32_t p;
    int32_t px = 0, py = twoRx2 * y;
    
    uint16_t color = white ? SSD1306_WHITE : SSD1306_BLACK;
    
    // Regione 1
    p = (int32_t)(ry2 - rx2 * ry + 0.25f * rx2);
    while (px < py) {
      oled.drawPixel(cx + x, cy + y, color);
      oled.drawPixel(cx - x, cy + y, color);
      oled.drawPixel(cx + x, cy - y, color);
      oled.drawPixel(cx - x, cy - y, color);
      x++;
      px += twoRy2;
      if (p < 0) {
        p += ry2 + px;
      } else {
        y--;
        py -= twoRx2;
        p += ry2 + px - py;
      }
    }
    
    // Regione 2
    p = (int32_t)(ry2 * (x + 0.5f) * (x + 0.5f) + rx2 * (y - 1) * (y - 1) - rx2 * ry2);
    while (y >= 0) {
      oled.drawPixel(cx + x, cy + y, color);
      oled.drawPixel(cx - x, cy + y, color);
      oled.drawPixel(cx + x, cy - y, color);
      oled.drawPixel(cx - x, cy - y, color);
      y--;
      py -= twoRx2;
      if (p > 0) {
        p += rx2 - py;
      } else {
        x++;
        px += twoRy2;
        p += rx2 - py + px;
      }
    }
  }
  
  // ── Helper: Ellisse piena ───────────────────────────────────────────────
  void drawEllipseFilled(int16_t cx, int16_t cy, int16_t rx, int16_t ry, 
                          uint16_t color) {
    for (int16_t y = -ry; y <= ry; y++) {
      // Calcola x per questa riga dell'ellisse
      float xf = rx * sqrt(1.0f - (float)(y * y) / (float)(ry * ry));
      int16_t xi = (int16_t)xf;
      oled.drawFastHLine(cx - xi, cy + y, xi * 2 + 1, color);
    }
  }
  
  // ══════════════════════════════════════════════════════════════════════════
  // DECORAZIONI ESPRESSIONI
  // ══════════════════════════════════════════════════════════════════════════
  
  void drawSadMouth() {
    // Bocca triste: arco invertito
    int16_t mouthCX = 64;
    int16_t mouthCY = 54;
    // Arco con segmenti
    for (int16_t x = -12; x <= 12; x++) {
      float normalized = (float)x / 12.0f;
      int16_t y = (int16_t)(4.0f * normalized * normalized);
      oled.drawPixel(mouthCX + x, mouthCY + y, SSD1306_WHITE);
      oled.drawPixel(mouthCX + x, mouthCY + y + 1, SSD1306_WHITE);
    }
  }
  
  void drawHappyMouth() {    
    // Sorriso: arco in basso al centro
    int16_t mouthCX = 64;
    int16_t mouthCY = 58;
    for (int16_t x = -10; x <= 10; x++) {
      float normalized = (float)x / 10.0f;
      int16_t y = -(int16_t)(3.0f * normalized * normalized);
      oled.drawPixel(mouthCX + x, mouthCY + y, SSD1306_WHITE);
      oled.drawPixel(mouthCX + x, mouthCY + y + 1, SSD1306_WHITE);
    }
  }
  
  void drawHeartEyes(int16_t cx, int16_t cy) {
    // Cuoricino piccolo 9x8 pixel al posto della pupilla
    // Bitmap cuore
    static const uint8_t heart[] = {
      0b01101100,
      0b11111110,
      0b11111110,
      0b11111110,
      0b01111100,
      0b00111000,
      0b00010000,
      0b00000000
    };
    
    int16_t startX = cx - 4;
    int16_t startY = cy - 4;
    
    for (int row = 0; row < 7; row++) {
      for (int col = 0; col < 8; col++) {
        if (heart[row] & (0x80 >> col)) {
          oled.drawPixel(startX + col, startY + row, SSD1306_WHITE);
        }
      }
    }
  }
  
  void drawSparkles() {
    // Stelline attorno agli occhi (per EXP_EXCITED)
    uint32_t t = millis() / 200;  // cambia ogni 200ms
    
    // 4 stelline in posizioni che ruotano
    const int16_t sparklePos[][2] = {
      {10, 10}, {118, 10}, {15, 55}, {113, 55},
      {50, 5},  {78, 5},   {50, 60}, {78, 60}
    };
    
    for (int i = 0; i < 8; i++) {
      if ((t + i) % 3 == 0) {  // solo alcune visibili per volta
        int16_t sx = sparklePos[i][0];
        int16_t sy = sparklePos[i][1];
        // Stellina: croce 3px
        oled.drawPixel(sx, sy, SSD1306_WHITE);
        oled.drawPixel(sx - 1, sy, SSD1306_WHITE);
        oled.drawPixel(sx + 1, sy, SSD1306_WHITE);
        oled.drawPixel(sx, sy - 1, SSD1306_WHITE);
        oled.drawPixel(sx, sy + 1, SSD1306_WHITE);
      }
    }
  }
  
  void drawQuestionMark() {
    // Punto interrogativo sopra l'occhio destro (per EXP_CONFUSED)
    oled.setTextSize(1);
    oled.setCursor(108, 2);
    oled.print("?");
  }
  
  // ══════════════════════════════════════════════════════════════════════════
  // MODALITÀ TESTO (contenuto deciso da LLM)
  // ══════════════════════════════════════════════════════════════════════════
  
  void drawText() {
    oled.setTextSize(state.textSize);
    oled.setTextWrap(true);
    
    int16_t lineHeight = 8 * state.textSize;
    int16_t startY = 2;
    
    // Centra verticalmente se poche righe
    int numLines = 0;
    if (strlen(state.textLine1) > 0) numLines++;
    if (strlen(state.textLine2) > 0) numLines++;
    if (strlen(state.textLine3) > 0) numLines++;
    if (strlen(state.textLine4) > 0) numLines++;
    
    if (numLines > 0) {
      startY = (SCREEN_HEIGHT - numLines * lineHeight) / 2;
      if (startY < 0) startY = 0;
    }
    
    const char* lines[] = {
      state.textLine1, state.textLine2, 
      state.textLine3, state.textLine4
    };
    
    for (int i = 0; i < 4; i++) {
      if (strlen(lines[i]) > 0) {
        // Centra orizzontalmente
        int16_t textWidth = strlen(lines[i]) * 6 * state.textSize;
        int16_t x = (SCREEN_WIDTH - textWidth) / 2;
        if (x < 0) x = 0;
        
        oled.setCursor(x, startY);
        oled.print(lines[i]);
        startY += lineHeight + 2;
      }
    }
  }
  
  // ══════════════════════════════════════════════════════════════════════════
  // MODALITÀ PROGRESS BAR
  // ══════════════════════════════════════════════════════════════════════════
  
  void drawProgress() {
    // Label in alto
    if (strlen(state.progressLabel) > 0) {
      oled.setTextSize(1);
      int16_t tw = strlen(state.progressLabel) * 6;
      oled.setCursor((SCREEN_WIDTH - tw) / 2, 8);
      oled.print(state.progressLabel);
    }
    
    // Barra di progresso
    int16_t barX = 10;
    int16_t barY = 28;
    int16_t barW = SCREEN_WIDTH - 20;
    int16_t barH = 14;
    
    // Contorno
    oled.drawRect(barX, barY, barW, barH, SSD1306_WHITE);
    
    // Riempimento
    int16_t fillW = (int16_t)((barW - 4) * state.progressPercent / 100.0f);
    if (fillW > 0) {
      oled.fillRect(barX + 2, barY + 2, fillW, barH - 4, SSD1306_WHITE);
    }
    
    // Percentuale sotto
    char pctStr[8];
    snprintf(pctStr, sizeof(pctStr), "%d%%", state.progressPercent);
    oled.setTextSize(1);
    int16_t tw = strlen(pctStr) * 6;
    oled.setCursor((SCREEN_WIDTH - tw) / 2, barY + barH + 6);
    oled.print(pctStr);
  }
  
  // ══════════════════════════════════════════════════════════════════════════
  // MODALITÀ ICONA + TESTO
  // ══════════════════════════════════════════════════════════════════════════
  
  void drawIcon() {
    // Icona a sinistra (32x32), testo a destra
    drawBuiltinIcon(4, 16, state.iconId);
    
    // Testo a destra dell'icona
    oled.setTextSize(1);
    oled.setTextWrap(true);
    oled.setCursor(40, 20);
    oled.print(state.iconText);
  }
  
  void drawBuiltinIcon(int16_t x, int16_t y, uint8_t id) {
    switch (id) {
      case 0: // WiFi
        oled.drawCircle(x + 12, y + 20, 10, SSD1306_WHITE);
        oled.drawCircle(x + 12, y + 20, 6, SSD1306_WHITE);
        oled.fillCircle(x + 12, y + 20, 2, SSD1306_WHITE);
        break;
      case 1: // Batteria
        oled.drawRect(x + 2, y + 4, 20, 12, SSD1306_WHITE);
        oled.fillRect(x + 22, y + 7, 3, 6, SSD1306_WHITE);
        oled.fillRect(x + 4, y + 6, 8, 8, SSD1306_WHITE);
        break;
      case 2: // Temperatura
        oled.drawCircle(x + 12, y + 18, 5, SSD1306_WHITE);
        oled.drawRect(x + 10, y + 2, 5, 16, SSD1306_WHITE);
        break;
      case 3: // Musica
        oled.fillCircle(x + 6, y + 18, 3, SSD1306_WHITE);
        oled.fillCircle(x + 18, y + 16, 3, SSD1306_WHITE);
        oled.drawLine(x + 9, y + 18, x + 9, y + 4, SSD1306_WHITE);
        oled.drawLine(x + 21, y + 16, x + 21, y + 2, SSD1306_WHITE);
        oled.drawLine(x + 9, y + 4, x + 21, y + 2, SSD1306_WHITE);
        break;
      case 4: // Check
        oled.drawLine(x + 4, y + 12, x + 10, y + 18, SSD1306_WHITE);
        oled.drawLine(x + 10, y + 18, x + 22, y + 6, SSD1306_WHITE);
        break;
      case 5: // Errore (X)
        oled.drawLine(x + 4, y + 4, x + 20, y + 20, SSD1306_WHITE);
        oled.drawLine(x + 20, y + 4, x + 4, y + 20, SSD1306_WHITE);
        break;
      default:
        oled.drawRect(x + 2, y + 2, 20, 20, SSD1306_WHITE);
        break;
    }
  }
  
  // ══════════════════════════════════════════════════════════════════════════
  // MODALITÀ SPLIT (metà occhi, metà info)
  // ══════════════════════════════════════════════════════════════════════════
  
  void drawSplit() {
    // Metà superiore: mini occhi
    drawMiniEyes(0);
    
    // Linea separatrice
    oled.drawFastHLine(0, 33, SCREEN_WIDTH, SSD1306_WHITE);
    
    // Metà inferiore: testo
    oled.setTextSize(1);
    oled.setCursor(2, 38);
    oled.print(state.textLine1);
    oled.setCursor(2, 48);
    oled.print(state.textLine2);
    oled.setCursor(2, 58);
    oled.print(state.textLine3);
  }
  
  void drawMiniEyes(int16_t yOffset) {
    // Versione compatta degli occhi (metà superiore 32px)
    int16_t cy = 16 + yOffset;
    int16_t rx = 10, ry = 10;
    
    float actualRYL = ry * constrain(state.leftEyeOpenness, 0.05f, 1.2f);
    float actualRYR = ry * constrain(state.rightEyeOpenness, 0.05f, 1.2f);
    
    // Occhio sinistro
    oled.drawRoundRect(32 - rx, cy - (int16_t)actualRYL, 
                        rx * 2, (int16_t)(actualRYL * 2), 4, SSD1306_WHITE);
    int16_t pxL = 32 + (int16_t)(state.pupilOffsetX * 4);
    int16_t pyL = cy + (int16_t)(state.pupilOffsetY * 3);
    oled.fillCircle(pxL, pyL, 3, SSD1306_WHITE);
    
    // Occhio destro
    oled.drawRoundRect(96 - rx, cy - (int16_t)actualRYR, 
                        rx * 2, (int16_t)(actualRYR * 2), 4, SSD1306_WHITE);
    int16_t pxR = 96 + (int16_t)(state.pupilOffsetX * 4);
    int16_t pyR = cy + (int16_t)(state.pupilOffsetY * 3);
    oled.fillCircle(pxR, pyR, 3, SSD1306_WHITE);
  }
};

#endif // DISPLAY_EYES_H





