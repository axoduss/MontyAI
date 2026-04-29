/*
 * ROBOT ESP32-S3 N16R8 - Firmware Base
 * Fase 1: MIC I2S (INMP441), WebSocket, NeoPixel WS2812
 * 
 * Arduino IDE 2.3.8 | ESP32 Core 3.x | FreeRTOS
 * 
 * Librerie richieste:
 *   - WebSockets by Markus Sattler (2.x)
 *   - Adafruit NeoPixel
 *   - ArduinoJson (7.x)
 *   - ESP32 core built-in: driver/i2s.h, WiFi.h, ArduinoOTA.h
 */

#include "credentials.h"
#include "display_eyes.h"
#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include "driver/i2s.h"

// ─── CONFIG WIFI ─────────────────────────────────────────────────────────────
const char* wifi_ssid     = WIFI_SSID;
const char* wifi_pass     = WIFI_PASSWORD;

// ─── CONFIG SERVER ───────────────────────────────────────────────────────────
const char* SERVER_HOST   = "192.168.1.8"; // IP del PC con il server Python
const uint16_t SERVER_PORT = 8765;
const char* WS_AUDIO_PATH  = "/audio";
const char* WS_CMD_PATH    = "/cmd";

// ─── PIN DEFINITIONS ─────────────────────────────────────────────────────────
// INMP441 - Microfono I2S
#define I2S_MIC_WS    GPIO_NUM_5
#define I2S_MIC_SD    GPIO_NUM_4
#define I2S_MIC_SCK   GPIO_NUM_6
#define I2S_MIC_PORT  I2S_NUM_0

// MAX98357A - Speaker I2S
#define I2S_SPK_LRC   GPIO_NUM_1
#define I2S_SPK_BCLK  GPIO_NUM_2
#define I2S_SPK_DIN   GPIO_NUM_42
#define I2S_SPK_SD    GPIO_NUM_41
#define I2S_SPK_PORT  I2S_NUM_1

// NeoPixel WS2812
#define NEOPIXEL_PIN  48
#define NEOPIXEL_COUNT 4

// Motore sinistro
#define MOT_L_IN1  GPIO_NUM_19
#define MOT_L_IN2  GPIO_NUM_20
// Motore destro
#define MOT_R_IN1  GPIO_NUM_13
#define MOT_R_IN2  GPIO_NUM_14

// PWM config (LEDC)
#define PWM_FREQ       20000   // 20kHz
#define PWM_RESOLUTION 8      // 8 bit → 0-255
// Canali LEDC (ESP32-S3 ha 8 canali)
#define PWM_CH_L_IN1   0
#define PWM_CH_L_IN2   1
#define PWM_CH_R_IN1   2
#define PWM_CH_R_IN2   3

// ─── BUMPER (microswitch finecorsa) ──────────────────────────────────────────
#define BUMPER_LEFT_PIN   GPIO_NUM_47
#define BUMPER_RIGHT_PIN  GPIO_NUM_11
#define BUMPER_DEBOUNCE_MS 50  // debounce software

// ─── PARAMETRI AUDIO ─────────────────────────────────────────────────────────
#define SAMPLE_RATE       16000
#define BITS_PER_SAMPLE   16
#define I2S_DMA_BUF_COUNT 4
#define I2S_DMA_BUF_LEN   512
#define AUDIO_CHUNK_SIZE  (I2S_DMA_BUF_LEN * 2) // byte

// ─── VAD (Voice Activity Detection) semplice ─────────────────────────────────
#define VAD_THRESHOLD     1000     // Ampiezza minima RMS
#define VAD_SILENCE_MS    1200    // ms silenzio per chiudere utterance
#define VAD_MIN_SPEECH_MS 300     // ms minimi di parlato valido


// ─── POST-TTS COOLDOWN ──────────────────────────────────────────────────────
volatile uint32_t micEnableAfterMs = 10;  // millis() dopo cui il mic può ascoltare
#define POST_TTS_COOLDOWN_MS  2000        // ms di silenzio forzato dopo TTS (regolabile)


// ─── OGGETTI GLOBALI ─────────────────────────────────────────────────────────
WebSocketsClient wsAudio;   // WebSocket per stream audio → server
WebSocketsClient wsCmd;     // WebSocket per comandi JSON ← server

Adafruit_NeoPixel strip(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
DisplayManager display;

// ─── STATE ───────────────────────────────────────────────────────────────────
enum RobotState { IDLE, LISTENING, PROCESSING, SPEAKING, PLAYING_MUSIC };

// Mutex per proteggere robotState da accessi concorrenti
SemaphoreHandle_t stateMutex;
volatile RobotState robotState = IDLE;

// Getter/setter thread-safe per robotState
RobotState getState() {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  RobotState s = robotState;
  xSemaphoreGive(stateMutex);
  return s;
}

void setState(RobotState newState) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  robotState = newState;
  xSemaphoreGive(stateMutex);
}

bool wsAudioConnected = false;
bool wsCmdConnected   = false;

// Flag per LED override da comandi utente
volatile bool userLedOverride = false;
volatile uint32_t userLedOverrideUntil = 0;
#define LED_OVERRIDE_DURATION_MS 60000  // mantieni colore utente per 60s


// Espressione impostata dal server (LLM) — ha priorità sull'automatica
volatile bool serverExpressionActive = false;
EyeExpression serverExpression = EXP_NEUTRAL;

// ─── TTS con struct che include lunghezza ──────────────────────
typedef struct {
  uint8_t* data;
  size_t   length;
} TtsChunk;

#define TTS_QUEUE_SIZE 64
QueueHandle_t ttsQueue;

// Flag per segnale fine TTS dal server
volatile bool ttsEndReceived = false;
volatile bool ttsPlaying     = false;

// ─── Coda per invio audio thread-safe ───────────────────────────────
typedef struct {
  uint8_t  data[I2S_DMA_BUF_LEN * sizeof(int16_t)];  // max 1024 byte
  size_t   length;
} AudioOutChunk;

#define AUDIO_OUT_QUEUE_SIZE 16
QueueHandle_t audioOutQueue;

// Coda per messaggi di testo da inviare via wsAudio (es. end_of_speech)
#define TEXT_OUT_QUEUE_SIZE 4
#define TEXT_OUT_MAX_LEN    64
typedef struct {
  char text[TEXT_OUT_MAX_LEN];
} TextOutMsg;

QueueHandle_t textOutQueue;

// Task handles
TaskHandle_t taskMicHandle    = NULL;
TaskHandle_t taskSpeakerHandle = NULL;
TaskHandle_t taskStatusHandle  = NULL;
TaskHandle_t taskDisplayHandle = NULL;


// ─── CODA COMANDI ────────────────────────────────────────────────────────────
// I comandi vengono accodati dal callback WS e processati in un task dedicato
// Questo evita che handleCommand() blocchi il loop WebSocket

#define CMD_QUEUE_SIZE    16
#define CMD_MAX_LEN      512

typedef struct {
  char json[CMD_MAX_LEN];
} CmdMsg;

QueueHandle_t cmdQueue;
TaskHandle_t taskCmdHandle = NULL;



// ─── STATO MOTORI ────────────────────────────────────────────────────────────
volatile bool motorsRunning = false;
volatile uint32_t motorStopTime = 0;  // millis() a cui fermare (0 = no timeout)

// Bumper state
volatile bool bumperLeftHit  = false;
volatile bool bumperRightHit = false;
volatile uint32_t lastBumperLeftMs  = 0;
volatile uint32_t lastBumperRightMs = 0;

// Task handle
TaskHandle_t taskMotorHandle  = NULL;
TaskHandle_t taskBumperHandle = NULL;



// ─── PROTOTIPI ───────────────────────────────────────────────────────────────
void setupWiFi();
void setupOTA();
void setupI2S_Mic();
void setupI2S_Speaker();
void setupNeoPixel();
void setupWebSockets();
void taskMic(void* param);
void taskSpeaker(void* param);
void taskStatusLed(void* param);
void taskCommandProcessor(void* param);
void wsAudioEvent(WStype_t type, uint8_t* payload, size_t length);
void wsCmdEvent(WStype_t type, uint8_t* payload, size_t length);
void handleCommand(const char* json);
void setLedColor(uint8_t r, uint8_t g, uint8_t b);
void setLedEffect(const char* effect);
int16_t computeRMS(int16_t* samples, size_t count);
void flushTtsQueue();
void taskDisplay(void* param);
EyeExpression parseExpression(const char* str);



void setupMotors();
void setupBumpers();
void taskMotorWatchdog(void* param);
void taskBumperMonitor(void* param);
void motorSetLeft(int16_t speed);
void motorSetRight(int16_t speed);
void motorStop();
void motorForward(uint8_t speed);
void motorBackward(uint8_t speed);
void motorTurnLeft(uint8_t speed);
void motorTurnRight(uint8_t speed);




// ─── SETUP ───────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[ROBOT] Avvio...");

  // Crea mutex
  stateMutex = xSemaphoreCreateMutex();

  setupNeoPixel();
  setLedColor(10, 0, 20); // viola: boot

  setupWiFi();
  setupOTA();
  setupI2S_Mic();
  setupI2S_Speaker();
  setupMotors();      
  setupBumpers();     
  setupWebSockets();

   // Display OLED
  if (!display.begin()) {
    Serial.println("[SETUP] ERRORE: Display non trovato!");
  }

  // La coda contiene PUNTATORI a TtsChunk, non blocchi da 1024 byte
  ttsQueue = xQueueCreate(TTS_QUEUE_SIZE, sizeof(TtsChunk));

  // Code per comunicazione thread-safe con WebSocket
  audioOutQueue = xQueueCreate(AUDIO_OUT_QUEUE_SIZE, sizeof(AudioOutChunk));
  textOutQueue  = xQueueCreate(TEXT_OUT_QUEUE_SIZE, sizeof(TextOutMsg));
  
  // Coda per gestione comandi multipli
  cmdQueue      = xQueueCreate(CMD_QUEUE_SIZE, sizeof(CmdMsg));

  // Task FreeRTOS
  xTaskCreatePinnedToCore(taskMic,       "MicTask",    8192, NULL, 5, &taskMicHandle,     1);
  xTaskCreatePinnedToCore(taskSpeaker,   "SpkTask",    8192, NULL, 4, &taskSpeakerHandle,  1);
  xTaskCreatePinnedToCore(taskStatusLed, "LedTask",    4096, NULL, 1, &taskStatusHandle,   0);
  xTaskCreatePinnedToCore(taskMotorWatchdog, "MotTask",  4096, NULL, 3, &taskMotorHandle,  0);
  xTaskCreatePinnedToCore(taskBumperMonitor, "BumpTask", 4096, NULL, 6, &taskBumperHandle, 0);
  xTaskCreatePinnedToCore(taskCommandProcessor, "CmdTask",  8192, NULL, 7, &taskCmdHandle,      0);
  xTaskCreatePinnedToCore(taskDisplay, "DispTask", 8192, NULL, 2, &taskDisplayHandle, 0);


  setLedColor(0, 5, 0); // verde: pronto
  delay(2000);
  Serial.println("[ROBOT] Pronto.");
}

// ─── LOOP PRINCIPALE ─────────────────────────────────────────────────────────
void loop() {
  wsAudio.loop();
  wsCmd.loop();
  ArduinoOTA.handle();

  // Invia audio dalla coda (thread-safe)
  // wsAudio.sendBIN() viene chiamato SOLO qui, nello stesso contesto di wsAudio.loop()
  AudioOutChunk aoc;
  while (xQueueReceive(audioOutQueue, &aoc, 0) == pdTRUE) {
    if (wsAudioConnected) {
      wsAudio.sendBIN(aoc.data, aoc.length);
    }
  }

  // Invia messaggi di testo dalla coda (es. end_of_speech)
  TextOutMsg tom;
  while (xQueueReceive(textOutQueue, &tom, 0) == pdTRUE) {
    if (wsAudioConnected) {
      wsAudio.sendTXT(tom.text);
    }
  }

  delay(1);
}

// ─── WIFI ────────────────────────────────────────────────────────────────────
void setupWiFi() {
  Serial.printf("[WiFi] Connessione a %s...\n", wifi_ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_ssid, wifi_pass);
  uint8_t attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connesso! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WiFi] ERRORE connessione!");
  }
}

// ─── OTA ─────────────────────────────────────────────────────────────────────
void setupOTA() {
  ArduinoOTA.setHostname("robot-esp32s3");
  ArduinoOTA.onStart([]() {
    Serial.println("[OTA] Avvio aggiornamento...");
    setLedColor(20, 10, 0);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("[OTA] Completato!");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] Errore: %u\n", error);
  });
  ArduinoOTA.begin();
  Serial.println("[OTA] Pronto.");
}

// ─── I2S MICROFONO ───────────────────────────────────────────────────────────
void setupI2S_Mic() {
  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate          = SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT, // INMP441 → 32bit, sample in MSB
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = I2S_DMA_BUF_COUNT,
    .dma_buf_len          = I2S_DMA_BUF_LEN,
    .use_apll             = false,
    .tx_desc_auto_clear   = false,
    .fixed_mclk           = 0
  };
  i2s_pin_config_t pins = {
    .mck_io_num   = I2S_PIN_NO_CHANGE,
    .bck_io_num   = I2S_MIC_SCK,
    .ws_io_num    = I2S_MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = I2S_MIC_SD
  };
  i2s_driver_install(I2S_MIC_PORT, &cfg, 0, NULL);
  i2s_set_pin(I2S_MIC_PORT, &pins);
  i2s_zero_dma_buffer(I2S_MIC_PORT);
  Serial.println("[I2S] Microfono pronto.");
}

// ─── I2S SPEAKER ─────────────────────────────────────────────────────────────
void setupI2S_Speaker() {
  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate          = 22050,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = I2S_DMA_BUF_COUNT,
    .dma_buf_len          = 256,
    .use_apll             = false,
    .tx_desc_auto_clear   = true,
    .fixed_mclk           = 0
  };
  i2s_pin_config_t pins = {
    .mck_io_num   = I2S_PIN_NO_CHANGE,
    .bck_io_num   = I2S_SPK_BCLK,
    .ws_io_num    = I2S_SPK_LRC,
    .data_out_num = I2S_SPK_DIN,
    .data_in_num  = I2S_PIN_NO_CHANGE
  };
  pinMode(I2S_SPK_SD, OUTPUT);
  digitalWrite(I2S_SPK_SD, LOW);

  i2s_driver_install(I2S_SPK_PORT, &cfg, 0, NULL);
  i2s_set_pin(I2S_SPK_PORT, &pins);
  Serial.println("[I2S] Speaker pronto.");
}

// ─── NEOPIXEL ────────────────────────────────────────────────────────────────
void setupNeoPixel() {
  strip.begin();
  strip.setBrightness(40);
  strip.show();
}

void setLedColor(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < NEOPIXEL_COUNT; i++)
    strip.setPixelColor(i, strip.Color(r, g, b));
  strip.show();
}



// ─── SETUP MOTORI ────────────────────────────────────────────────────────────
void setupMotors() {
  // Configura i 4 canali PWM con LEDC
  ledcAttach(MOT_L_IN1, PWM_FREQ, PWM_RESOLUTION);
  ledcAttach(MOT_L_IN2, PWM_FREQ, PWM_RESOLUTION);
  ledcAttach(MOT_R_IN1, PWM_FREQ, PWM_RESOLUTION);
  ledcAttach(MOT_R_IN2, PWM_FREQ, PWM_RESOLUTION);

  // Tutto fermo
  motorStop();
  Serial.println("[MOT] Motori pronti.");
}

// ─── SETUP BUMPER ────────────────────────────────────────────────────────────
void setupBumpers() {
  pinMode(BUMPER_LEFT_PIN, INPUT_PULLDOWN);
  pinMode(BUMPER_RIGHT_PIN, INPUT_PULLDOWN);
  Serial.println("[BUMP] Bumper pronti (pulldown interno) .");
}

// ─── WEBSOCKET SETUP ─────────────────────────────────────────────────────────
void setupWebSockets() {
  wsAudio.begin(SERVER_HOST, SERVER_PORT, WS_AUDIO_PATH);
  wsAudio.onEvent(wsAudioEvent);
  wsAudio.setReconnectInterval(3000);

  wsCmd.begin(SERVER_HOST, SERVER_PORT, WS_CMD_PATH);
  wsCmd.onEvent(wsCmdEvent);
  wsCmd.setReconnectInterval(3000);

  Serial.println("[WS] WebSocket configurati.");
}

// ─── WEBSOCKET EVENTS ────────────────────────────────────────────────────────
void wsAudioEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      wsAudioConnected = true;
      Serial.println("[WS Audio] Connesso.");
      break;
    case WStype_DISCONNECTED:
      wsAudioConnected = false;
      Serial.println("[WS Audio] Disconnesso.");
      break;
    default: break;
  }
}

void wsCmdEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      wsCmdConnected = true;
      Serial.println("[WS Cmd] Connesso.");
      wsCmd.sendTXT("{\"type\":\"hello\",\"device\":\"robot-esp32s3\",\"version\":\"1.0\"}");
      break;

    case WStype_DISCONNECTED:
      wsCmdConnected = false;
      Serial.println("[WS Cmd] Disconnesso.");
      break;

    case WStype_TEXT: {
      //handleCommand((const char*)payload);
      if (cmdQueue != NULL && length < CMD_MAX_LEN) {
        CmdMsg msg;
        memcpy(msg.json, payload, length);
        msg.json[length] = '\0';  // null-terminate
        if (xQueueSend(cmdQueue, &msg, 0) != pdTRUE) {
          Serial.println("[WS Cmd] WARN: coda comandi piena, comando droppato!");
        }
      } else if (length >= CMD_MAX_LEN) {
        Serial.printf("[WS Cmd] WARN: comando troppo lungo (%d bytes)\n", length);
      }
      break;
    }
  

    case WStype_BIN: {
      //Chunk audio TTS — salva con lunghezza reale
      if (ttsQueue != NULL && length > 0) {
        TtsChunk chunk;
        // Alloca buffer della dimensione ESATTA ricevuta
        chunk.data = (uint8_t*)malloc(length);
        chunk.length = length;
        if (chunk.data) {
          memcpy(chunk.data, payload, length);
          if (xQueueSend(ttsQueue, &chunk, 0) != pdTRUE) {
            // Coda piena, libera memoria
            free(chunk.data);
            Serial.println("[TTS] Coda piena, chunk droppato.");
          }
        }
        ttsPlaying = true;
      }
      break;
    }

    default: break;
  }
}

// ─── FLUSH CODA TTS ──────────────────────────────────────────────────────────
// Svuota la coda TTS liberando tutta la memoria allocata
void flushTtsQueue() {
  TtsChunk chunk;
  while (xQueueReceive(ttsQueue, &chunk, 0) == pdTRUE) {
    if (chunk.data) {
      free(chunk.data);
    }
  }
  ttsPlaying = false;
  ttsEndReceived = false;
  Serial.println("[TTS] Coda svuotata.");
}


// ─── PARSER ESPRESSIONE DA STRINGA ───────────────────────────────────────────
EyeExpression parseExpression(const char* str) {
  if (!str) return EXP_NEUTRAL;
  
  if (strcmp(str, "neutral") == 0)       return EXP_NEUTRAL;
  if (strcmp(str, "happy") == 0)         return EXP_HAPPY;
  if (strcmp(str, "sad") == 0)           return EXP_SAD;
  if (strcmp(str, "angry") == 0)         return EXP_ANGRY;
  if (strcmp(str, "surprised") == 0)     return EXP_SURPRISED;
  if (strcmp(str, "sleepy") == 0)        return EXP_SLEEPY;
  if (strcmp(str, "thinking") == 0)      return EXP_THINKING;
  if (strcmp(str, "love") == 0)          return EXP_LOVE;
  if (strcmp(str, "wink") == 0)          return EXP_WINK;
  if (strcmp(str, "skeptical") == 0)     return EXP_SKEPTICAL;
  if (strcmp(str, "excited") == 0)       return EXP_EXCITED;
  if (strcmp(str, "confused") == 0)      return EXP_CONFUSED;
  
  Serial.printf("[DISP] Espressione sconosciuta: %s\n", str);
  return EXP_NEUTRAL;
}




// ─── GESTIONE COMANDI JSON ────────────────────────────────────────────────────
/*
 * Formato comando atteso dal server:
 * {
 *   "cmd": "set_led",
 *   "params": { "r": 255, "g": 0, "b": 0 }
 * }
 */
void handleCommand(const char* json) {
  Serial.printf("[CMD] Ricevuto: %s\n", json);

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    Serial.printf("[CMD] JSON error: %s\n", err.c_str());
    return;
  }

  const char* cmd = doc["cmd"];
  if (!cmd) return;

  // ══════════════════════════════════════════════════════════════════════════
  // LED
  // ══════════════════════════════════════════════════════════════════════════

  // ── set_led ──────────────────────────────────────────────────────────────
  if (strcmp(cmd, "set_led") == 0) {
    uint8_t r = doc["params"]["r"].as<uint8_t>();
    uint8_t g = doc["params"]["g"].as<uint8_t>();
    uint8_t b = doc["params"]["b"].as<uint8_t>();

    // Comportamento : indirizzo i singoli led
    if (doc["params"].containsKey("led")) {
        int ledIdx = doc["params"]["led"].as<int>();
        if (ledIdx >= 0 && ledIdx < NEOPIXEL_COUNT) {
            strip.setPixelColor(ledIdx, strip.Color(r, g, b));
            strip.show();
        }
    } else {
        // Comportamento : tutti i LED uguali
        setLedColor(r, g, b);
    }


   
    userLedOverride = true;
    userLedOverrideUntil = millis() + LED_OVERRIDE_DURATION_MS;
    // ACK con info LED
    char ack[100];
    if (doc["params"].containsKey("led")) {
        int ledIdx = doc["params"]["led"].as<int>();
        snprintf(ack, sizeof(ack),
            "{\"ack\":\"set_led\",\"led\":%d,\"r\":%d,\"g\":%d,\"b\":%d}",
            ledIdx, r, g, b);
    } else {
        snprintf(ack, sizeof(ack),
            "{\"ack\":\"set_led\",\"r\":%d,\"g\":%d,\"b\":%d}", r, g, b);
    }
    wsCmd.sendTXT(ack);
  }

  // ── set_led_off ───────────────────────────────────────────────────────────
  else if (strcmp(cmd, "set_led_off") == 0) {
    strip.clear();
    strip.show();
    userLedOverride = false;
    wsCmd.sendTXT("{\"ack\":\"set_led_off\"}");
  }

    // ══════════════════════════════════════════════════════════════════════════
  // DISPLAY
  // ══════════════════════════════════════════════════════════════════════════

  // ── display_expression ────────────────────────────────────────────────────
  // {"cmd":"display_expression","params":{"expression":"happy"}}
  else if (strcmp(cmd, "display_expression") == 0) {
    const char* expStr = doc["params"]["expression"].as<const char*>();
    if (expStr) {
      EyeExpression exp = parseExpression(expStr);
      display.setExpression(exp);
      display.showEyes();  // assicura modalità occhi
      
      char ack[80];
      snprintf(ack, sizeof(ack),
        "{\"ack\":\"display_expression\",\"expression\":\"%s\"}", expStr);
      wsCmd.sendTXT(ack);
    }
  }

  // ── display_look ──────────────────────────────────────────────────────────
  // {"cmd":"display_look","params":{"direction":"left"}}
  else if (strcmp(cmd, "display_look") == 0) {
    const char* dirStr = doc["params"]["direction"].as<const char*>();
    if (dirStr) {
      LookDirection dir = LOOK_CENTER;
      if (strcmp(dirStr, "left") == 0)       dir = LOOK_LEFT;
      else if (strcmp(dirStr, "right") == 0) dir = LOOK_RIGHT;
      else if (strcmp(dirStr, "up") == 0)    dir = LOOK_UP;
      else if (strcmp(dirStr, "down") == 0)  dir = LOOK_DOWN;
      
      display.setLookDirection(dir);
      wsCmd.sendTXT("{\"ack\":\"display_look\"}");
    }
  }

  // ── display_text ──────────────────────────────────────────────────────────
  // {"cmd":"display_text","params":{"line1":"Ciao!","line2":"Come stai?","size":2,"duration_ms":5000}}
  else if (strcmp(cmd, "display_text") == 0) {
    const char* l1 = doc["params"]["line1"] | "";
    const char* l2 = doc["params"]["line2"] | "";
    const char* l3 = doc["params"]["line3"] | "";
    const char* l4 = doc["params"]["line4"] | "";
    uint8_t size = doc["params"]["size"] | 1;
    uint32_t dur = doc["params"]["duration_ms"] | 5000;
    
    display.showText(l1, l2, l3, l4, size, dur);
    wsCmd.sendTXT("{\"ack\":\"display_text\"}");
  }

  // ── display_progress ──────────────────────────────────────────────────────
  // {"cmd":"display_progress","params":{"percent":75,"label":"Caricamento...","duration_ms":0}}
  else if (strcmp(cmd, "display_progress") == 0) {
    uint8_t pct = doc["params"]["percent"] | 0;
    const char* label = doc["params"]["label"] | "";
    uint32_t dur = doc["params"]["duration_ms"] | 0;
    
    display.showProgress(pct, label, dur);
    wsCmd.sendTXT("{\"ack\":\"display_progress\"}");
  }

  // ── display_icon ──────────────────────────────────────────────────────────
  // {"cmd":"display_icon","params":{"icon_id":4,"text":"Fatto!","duration_ms":3000}}
  else if (strcmp(cmd, "display_icon") == 0) {
    uint8_t iconId = doc["params"]["icon_id"] | 0;
    const char* text = doc["params"]["text"] | "";
    uint32_t dur = doc["params"]["duration_ms"] | 3000;
    
    xSemaphoreTake(display.mutex, portMAX_DELAY);
    display.state.mode = DMODE_ICON;
    display.state.iconId = iconId;
    strncpy(display.state.iconText, text, sizeof(display.state.iconText) - 1);
    display.state.customModeUntilMs = (dur > 0) ? millis() + dur : 0;
    xSemaphoreGive(display.mutex);
    
    wsCmd.sendTXT("{\"ack\":\"display_icon\"}");
  }

  // ── display_split ─────────────────────────────────────────────────────────
  // {"cmd":"display_split","params":{"line1":"T: 24°C","line2":"Wifi: OK","line3":"Batt: 85%","duration_ms":10000}}
  else if (strcmp(cmd, "display_split") == 0) {
    const char* l1 = doc["params"]["line1"] | "";
    const char* l2 = doc["params"]["line2"] | "";
    const char* l3 = doc["params"]["line3"] | "";
    uint32_t dur = doc["params"]["duration_ms"] | 10000;
    
    xSemaphoreTake(display.mutex, portMAX_DELAY);
    display.state.mode = DMODE_SPLIT;
    strncpy(display.state.textLine1, l1, sizeof(display.state.textLine1) - 1);
    strncpy(display.state.textLine2, l2, sizeof(display.state.textLine2) - 1);
    strncpy(display.state.textLine3, l3, sizeof(display.state.textLine3) - 1);
    display.state.customModeUntilMs = (dur > 0) ? millis() + dur : 0;
    xSemaphoreGive(display.mutex);
    
    wsCmd.sendTXT("{\"ack\":\"display_split\"}");
  }

  // ── display_eyes ──────────────────────────────────────────────────────────
  // {"cmd":"display_eyes"} — forza ritorno a modalità occhi
  else if (strcmp(cmd, "display_eyes") == 0) {
    display.showEyes();
    wsCmd.sendTXT("{\"ack\":\"display_eyes\"}");
  }




  // ══════════════════════════════════════════════════════════════════════════
  // MOTORI
  // ══════════════════════════════════════════════════════════════════════════

  // ── move_forward ──────────────────────────────────────────────────────────
  else if (strcmp(cmd, "move_forward") == 0) {
    uint8_t speed = doc["params"]["speed"].as<uint8_t>();
    uint32_t duration = doc["params"]["duration_ms"].as<uint32_t>();
    if (speed == 0) speed = 150;  // default
    motorForward(speed);
    motorStopTime = (duration > 0) ? millis() + duration : 0;
    char ack[80];
    snprintf(ack, sizeof(ack),
      "{\"ack\":\"move_forward\",\"speed\":%d,\"duration_ms\":%lu}", speed, duration);
    wsCmd.sendTXT(ack);
  }

  // ── move_backward ─────────────────────────────────────────────────────────
  else if (strcmp(cmd, "move_backward") == 0) {
    uint8_t speed = doc["params"]["speed"].as<uint8_t>();
    uint32_t duration = doc["params"]["duration_ms"].as<uint32_t>();
    if (speed == 0) speed = 150;
    motorBackward(speed);
    motorStopTime = (duration > 0) ? millis() + duration : 0;
    char ack[80];
    snprintf(ack, sizeof(ack),
      "{\"ack\":\"move_backward\",\"speed\":%d,\"duration_ms\":%lu}", speed, duration);
    wsCmd.sendTXT(ack);
  }

  // ── turn_left ─────────────────────────────────────────────────────────────
  else if (strcmp(cmd, "turn_left") == 0) {
    uint8_t speed = doc["params"]["speed"].as<uint8_t>();
    uint32_t duration = doc["params"]["duration_ms"].as<uint32_t>();
    if (speed == 0) speed = 150;
    motorTurnLeft(speed);
    motorStopTime = (duration > 0) ? millis() + duration : 0;
    char ack[80];
    snprintf(ack, sizeof(ack),
      "{\"ack\":\"turn_left\",\"speed\":%d,\"duration_ms\":%lu}", speed, duration);
    wsCmd.sendTXT(ack);
  }

  // ── turn_right ────────────────────────────────────────────────────────────
  else if (strcmp(cmd, "turn_right") == 0) {
    uint8_t speed = doc["params"]["speed"].as<uint8_t>();
    uint32_t duration = doc["params"]["duration_ms"].as<uint32_t>();
    if (speed == 0) speed = 150;
    motorTurnRight(speed);
    motorStopTime = (duration > 0) ? millis() + duration : 0;
    char ack[80];
    snprintf(ack, sizeof(ack),
      "{\"ack\":\"turn_right\",\"speed\":%d,\"duration_ms\":%lu}", speed, duration);
    wsCmd.sendTXT(ack);
  }

  // ── stop ──────────────────────────────────────────────────────────────────
  else if (strcmp(cmd, "stop") == 0) {
    motorStop();
    wsCmd.sendTXT("{\"ack\":\"stop\"}");
  }

  // ══════════════════════════════════════════════════════════════════════════
  // SISTEMA
  // ══════════════════════════════════════════════════════════════════════════

  // ── state_update ──────────────────────────────────────────────────────────
  else if (strcmp(cmd, "state_update") == 0) {
    const char* state = doc["params"]["state"].as<const char*>();
    if (state) {
      if (strcmp(state, "processing") == 0) {
        setState(PROCESSING);
        flushTtsQueue();
      }
      else if (strcmp(state, "idle") == 0) {
        setState(IDLE);
      }
    }
  }

  // ── tts_start ─────────────────────────────────────────────────────────────
  else if (strcmp(cmd, "tts_start") == 0) {
    ttsEndReceived = false;
    ttsPlaying = true;
    setState(SPEAKING);
    userLedOverride = false;

     // Imposta espressione dal server
    const char* expStr = doc["params"]["expression"] | (const char*)nullptr;
    if (expStr) {
        serverExpression = parseExpression(expStr);
        serverExpressionActive = true;
        display.setExpression(serverExpression);
    } else {
        serverExpression = EXP_HAPPY;
        serverExpressionActive = true;
        display.setExpression(EXP_HAPPY);
    }

    // NON chiamare showEyes() se c'è un display custom attivo con timeout
    xSemaphoreTake(display.mutex, portMAX_DELAY);
    bool hasCustomContent = (display.state.mode != DMODE_EYES) && 
                            (display.state.customModeUntilMs > millis());
    xSemaphoreGive(display.mutex);
    
    if (!hasCustomContent) {
        display.showEyes();
    }
    // Se c'è contenuto custom, gli occhi torneranno automaticamente
    // quando customModeUntilMs scade, e useranno serverExpression



  }

  // ── tts_end ───────────────────────────────────────────────────────────────
  else if (strcmp(cmd, "tts_end") == 0) {
    ttsEndReceived = true;
  }


    // ── music_start ───────────────────────────────────────────────────────────
  else if (strcmp(cmd, "music_start") == 0) {
    ttsEndReceived = false;
    ttsPlaying = true;
    setState(PLAYING_MUSIC);
    userLedOverride = false;

    // Mostra un'icona musicale sul display
    xSemaphoreTake(display.mutex, portMAX_DELAY);
    display.state.mode = DMODE_ICON;
    display.state.iconId = 3; // Supponendo che 3 sia l'icona della musica
    const char* title = doc["params"]["title"] | "Musica";
    strncpy(display.state.iconText, title, sizeof(display.state.iconText) - 1);
    display.state.customModeUntilMs = millis() + 60000; // Max 60s
    xSemaphoreGive(display.mutex);
  }

  // ── music_stop ────────────────────────────────────────────────────────────
  else if (strcmp(cmd, "music_stop") == 0) {
    ttsEndReceived = true; // Sfruttiamo la stessa logica di fine del TTS
    display.showEyes();    // Torna agli occhi
  }

  // ── ping ──────────────────────────────────────────────────────────────────
  else if (strcmp(cmd, "ping") == 0) {
    wsCmd.sendTXT("{\"ack\":\"pong\"}");
  }

  // ── sconosciuto ───────────────────────────────────────────────────────────
  else {
    Serial.printf("[CMD] Comando sconosciuto: %s\n", cmd);
  }
}


// ─── TASK: COMMAND PROCESSOR ─────────────────────────────────────────────────
/*
 * Processa i comandi dalla coda in un task dedicato.
 * Vantaggi:
 *   - Il callback WS ritorna SUBITO → la libreria può ricevere il prossimo msg
 *   - I comandi vengono processati in ordine FIFO
 *   - Se un comando è "lento" (es. invio ACK), non blocca la ricezione
 *   - Priorità 7 = alta, viene eseguito appena c'è un comando in coda
 */
void taskCommandProcessor(void* param) {
  Serial.println("[CMD Task] Avviato.");
  CmdMsg msg;

  for (;;) {
    // Blocca finché non arriva un comando (nessun polling, efficiente)
    if (xQueueReceive(cmdQueue, &msg, portMAX_DELAY) == pdTRUE) {
      handleCommand(msg.json);
    }
  }
}

// ─── TASK: DISPLAY ───────────────────────────────────────────────────────────
/*
 * Aggiorna il display a ~25fps.
 * Gestisce:
 *   - Animazioni occhi autonome (blink, sguardo)
 *   - Cambio espressione in base allo stato robot
 *   - Contenuti custom da LLM
 */
void taskDisplay(void* param) {
  Serial.println("[DISP Task] Avviato.");
  
  RobotState lastState = (RobotState)-1;
  
  for (;;) {
    RobotState s = getState();
    
    // ── Cambio espressione automatico ────────────────────────────────
    // Solo se il server NON ha impostato un'espressione specifica
    if (s != lastState && display.state.mode == DMODE_EYES) {
        if (!serverExpressionActive) {
            // Espressione automatica basata sullo stato
            switch (s) {
                case IDLE:       display.setExpression(EXP_NEUTRAL);   break;
                case LISTENING:  display.setExpression(EXP_SURPRISED); break;
                case PROCESSING: display.setExpression(EXP_THINKING);  break;
                case SPEAKING:   display.setExpression(EXP_HAPPY);     break;
            }
        } else if (s == SPEAKING) {
            // Usa l'espressione del server
            display.setExpression(serverExpression);
        }
        lastState = s;
    }

    // ── Animazione parlato: usa l'espressione LLM come BASE ─────────
    if (s == SPEAKING && display.state.mode == DMODE_EYES) {
        static uint32_t lastSpeakChange = 0;
        if (millis() - lastSpeakChange > 2500) {
            if (serverExpressionActive) {
                // ★ FIX: alterna tra espressione LLM e variazioni coerenti
                int r = random(100);
                if (r < 60) {
                    display.setExpression(serverExpression);  // torna all'originale
                } else if (r < 80) {
                    display.setExpression(EXP_WINK);  // variazione leggera
                } else {
                    display.setExpression(EXP_NEUTRAL);  // pausa neutra
                }
            } else {
                // Fallback: comportamento originale
                int r = random(100);
                if (r < 40) display.setExpression(EXP_HAPPY);
                else if (r < 60) display.setExpression(EXP_NEUTRAL);
                else if (r < 75) display.setExpression(EXP_EXCITED);
                else display.setExpression(EXP_WINK);
            }
            lastSpeakChange = millis();
        }
    }
    
    // ── Reset flag server quando torna IDLE ─────────────────────────
    if (s == IDLE && lastState != IDLE) {
        serverExpressionActive = false;
    }
    
    // ── Disconnessione: occhi tristi ────────────────────────────────────
    if (!wsAudioConnected || !wsCmdConnected) {
      display.setExpression(EXP_SAD);
    }
    
    // ── Update rendering (~25fps) ───────────────────────────────────────
    display.update();
    
    vTaskDelay(pdMS_TO_TICKS(40));  // 25fps
  }
}


// ─── TASK: MICROFONO + VAD ───────────────────────────────────────────────────
void taskMic(void* param) {
  static int32_t rawBuf[I2S_DMA_BUF_LEN];
  static int16_t pcmBuf[I2S_DMA_BUF_LEN];

  bool inSpeech      = false;
  uint32_t lastVoiceMs  = 0;
  uint32_t speechStartMs = 0;

  Serial.println("[MIC Task] Avviato.");

  for (;;) {
    // Non ascoltare se stiamo riproducendo TTS o elaborando
    RobotState currentState = getState();  // accesso thread-safe
    if (currentState == SPEAKING || currentState == PROCESSING) {
      // Se eravamo in ascolto, resetta
      if (inSpeech) {
        inSpeech = false;
        Serial.println("[VAD] Reset — stato cambiato durante ascolto.");
      }
      // Drena continuamente il buffer DMA per evitare accumulo
      // Questo è CRITICO: se non leggiamo, il buffer si riempie di audio TTS
      size_t bytesRead = 0;
      i2s_read(I2S_MIC_PORT, rawBuf, sizeof(rawBuf), &bytesRead, pdMS_TO_TICKS(20));
      // Scarta tutto — non processare

      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    // Flush hardware del buffer I2S
    i2s_zero_dma_buffer(I2S_MIC_PORT);

    // *** COOLDOWN POST-TTS: non ascoltare finché non è passato il tempo ***
    if (millis() < micEnableAfterMs) {
      // Leggi e scarta i campioni per svuotare il buffer DMA del microfono
      size_t bytesRead = 0;
      //i2s_read(I2S_MIC_PORT, rawBuf, sizeof(rawBuf), &bytesRead, portMAX_DELAY);
      esp_err_t err = i2s_read(I2S_MIC_PORT, rawBuf, sizeof(rawBuf), &bytesRead, pdMS_TO_TICKS(10));
      // Non processare — stiamo solo drenando il buffer
      //vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
  
    // Flush hardware del buffer I2S
    i2s_zero_dma_buffer(I2S_MIC_PORT);

    //Serial.println("[MIC] Drain completato, ascolto attivo.");

    // ═══════════════════════════════════════════════════════════════════════
    // ASCOLTO NORMALE con VAD
    // ═══════════════════════════════════════════════════════════════════════

    size_t bytesRead = 0;
    i2s_read(I2S_MIC_PORT, rawBuf, sizeof(rawBuf), &bytesRead, portMAX_DELAY);

    size_t samplesRead = bytesRead / sizeof(int32_t);
    if (samplesRead == 0) continue;

    // INMP441: dato utile nei 18 bit MSB → shift right 14
    for (size_t i = 0; i < samplesRead; i++) {
      pcmBuf[i] = (int16_t)(rawBuf[i] >> 14);
    }

    int16_t rms = computeRMS(pcmBuf, samplesRead);

    if (rms > VAD_THRESHOLD) {
      lastVoiceMs = millis();
      if (!inSpeech) {
        inSpeech      = true;
        speechStartMs = millis();
        setState(LISTENING); 
        // Disattiva LED override quando si inizia ad ascoltare
        userLedOverride = false;
        Serial.println("[VAD] Inizio parlato");
      }

      // Accoda il chunk audio invece di chiamare wsAudio.sendBIN()
      size_t audioLen = samplesRead * sizeof(int16_t);
      if (audioLen <= sizeof(((AudioOutChunk*)0)->data)) {
        AudioOutChunk aoc;
        memcpy(aoc.data, pcmBuf, audioLen);
        aoc.length = audioLen;
        if (xQueueSend(audioOutQueue, &aoc, 0) != pdTRUE) {
          // Coda piena, drop silenzioso (meglio che crashare)
        }
      }

    } else if (inSpeech) {
      uint32_t silenceMs = millis() - lastVoiceMs;

      // Continua a inviare anche nel silenzio per non troncare parole finali
      if ((millis() - speechStartMs) < 15000) {
        size_t audioLen = samplesRead * sizeof(int16_t);
        if (audioLen <= sizeof(((AudioOutChunk*)0)->data)) {
          AudioOutChunk aoc;
          memcpy(aoc.data, pcmBuf, audioLen);
          aoc.length = audioLen;
          xQueueSend(audioOutQueue, &aoc, 0);
        }
      }

      if (silenceMs > VAD_SILENCE_MS) {
        uint32_t speechDuration = lastVoiceMs - speechStartMs;
        if (speechDuration > VAD_MIN_SPEECH_MS) {
          Serial.printf("[VAD] Fine parlato (%lums)\n", speechDuration);

          // Accoda il messaggio end_of_speech via textOutQueue
          TextOutMsg tom;
          snprintf(tom.text, TEXT_OUT_MAX_LEN,
            "{\"type\":\"end_of_speech\"}");
          xQueueSend(textOutQueue, &tom, pdMS_TO_TICKS(100));

          setState(PROCESSING); 
        } else {
          Serial.println("[VAD] Troppo breve, ignorato.");
          setState(IDLE); 
        }
        inSpeech = false;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

// ─── TASK: SPEAKER TTS ───────────────────────────────────────────────────────
void taskSpeaker(void* param) {
  Serial.println("[SPK Task] Avviato.");

  // Contatore per timeout sicurezza (evita blocco infinito in SPEAKING)
  uint32_t lastChunkMs = 0;
  const uint32_t TTS_TIMEOUT_MS = 5000;  // se nessun chunk per 5s, forza fine

  for (;;) {
    TtsChunk chunk;

    if (xQueueReceive(ttsQueue, &chunk, pdMS_TO_TICKS(100)) == pdTRUE) {
      // Chunk ricevuto — riproduci
      if (chunk.data && chunk.length > 0) {
        size_t written = 0;
        // Usa la lunghezza REALE del chunk, non una costante
        i2s_write(I2S_SPK_PORT, chunk.data, chunk.length, &written, portMAX_DELAY);
        // Libera la memoria allocata nel callback wsCmdEvent
        free(chunk.data);
        chunk.data = NULL;

        lastChunkMs = millis();
      }

    } else {
      // Coda vuota — controlliamo se il TTS è finito
      RobotState currentState = getState(); 

      if (currentState == SPEAKING) {
        // Fine TTS solo se:
        //   1. Il server ha inviato tts_end E la coda è vuota
        //   2. OPPURE timeout di sicurezza (nessun chunk per 5s)
        bool serverSaidEnd = ttsEndReceived;
        bool timedOut = (lastChunkMs > 0) && (millis() - lastChunkMs > TTS_TIMEOUT_MS);

        if (serverSaidEnd || timedOut) {
          if (timedOut && !serverSaidEnd) {
            Serial.println("[SPK] TTS timeout — fine forzata.");
          } else {
            Serial.println("[SPK] TTS completato.");
          }

          vTaskDelay(pdMS_TO_TICKS(100)); // Attendi che l'ultimo buffer DMA dello speaker finisca
          i2s_zero_dma_buffer(I2S_SPK_PORT);// Svuota buffer I2S per evitare audio residuo
          vTaskDelay(pdMS_TO_TICKS(100)); // Altra pausa

          // Reset flags
          ttsPlaying = false;
          ttsEndReceived = false;
          lastChunkMs = 0;

          // *** COOLDOWN: blocca il microfono per evitare che senta l'eco ***
          micEnableAfterMs = millis() + POST_TTS_COOLDOWN_MS;
          vTaskDelay(pdMS_TO_TICKS(POST_TTS_COOLDOWN_MS)); // Altra pausa
          Serial.printf("[SPK] TTS completato. Mic cooldown %dms.\n", POST_TTS_COOLDOWN_MS);
                    
          setState(IDLE);
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(1));
  }
}


// ─── TASK: STATUS LED ────────────────────────────────────────────────────────
void taskStatusLed(void* param) {
  RobotState lastState = (RobotState)-1;
  uint32_t   tick      = 0;

  Serial.println("[LED Task] Avviato.");

  for (;;) {
    RobotState s = getState();  //accesso thread-safe
    tick++;

    // Se l'utente ha impostato un colore LED, non sovrascrivere
    if (userLedOverride) {
      if (millis() < userLedOverrideUntil) {
        // Override attivo — non toccare i LED
        lastState = (RobotState)-1;  // reset per rientrare correttamente dopo
        vTaskDelay(pdMS_TO_TICKS(80));
        continue;
      } else {
        // Override scaduto — torna al controllo automatico
        userLedOverride = false;
        Serial.println("[LED] Override scaduto, torno a status.");
      }
    }

    // Nessuna connessione → lampeggio rosso
    if (!wsAudioConnected || !wsCmdConnected) {
      uint8_t v = (tick % 8 < 4) ? 15 : 0;
      setLedColor(v, 0, 0);
    } else {
      switch (s) {
        case IDLE:
          // Verde fisso — imposta solo al cambio stato per non flickerare
          if (s != lastState) {
            setLedColor(0, 5, 0);
          }
          break;

        case LISTENING: {
          // Pulsazione blu
          uint8_t v = (uint8_t)(8 + 7 * sin(tick * 0.3f));
          setLedColor(0, 0, v);
          break;
        }

        case PROCESSING: {
          // Arancione pulsante (più visibile di fisso)
          uint8_t v = (uint8_t)(8 + 7 * sin(tick * 0.4f));
          setLedColor(v, (uint8_t)(v / 2), 0);
          break;
        }

        case SPEAKING: {
          // Pulsazione ciano
          uint8_t v = (uint8_t)(8 + 7 * sin(tick * 0.5f));
          setLedColor(0, v, v);
          break;
        }
      }
    }

    lastState = s;
    vTaskDelay(pdMS_TO_TICKS(80));
  }
}

// ─── CONTROLLO MOTORI ────────────────────────────────────────────────────────
/*
 * DRV8871 truth table:
 *   IN1=PWM, IN2=LOW  → Forward
 *   IN1=LOW, IN2=PWM  → Reverse
 *   IN1=LOW, IN2=LOW  → Coast (libero)
 *   IN1=HIGH,IN2=HIGH → Brake (frenata)
 *
 * speed: -255..+255
 *   positivo = avanti
 *   negativo = indietro
 *   0 = brake
 */

void motorSetLeft(int16_t speed) {
  speed = constrain(speed, -255, 255);
  if (speed > 0) {
    ledcWrite(MOT_L_IN1, (uint8_t)speed);
    ledcWrite(MOT_L_IN2, 0);
  } else if (speed < 0) {
    ledcWrite(MOT_L_IN1, 0);
    ledcWrite(MOT_L_IN2, (uint8_t)(-speed));
  } else {
    // Brake
    ledcWrite(MOT_L_IN1, 255);
    ledcWrite(MOT_L_IN2, 255);
  }
}

void motorSetRight(int16_t speed) {
  speed = constrain(speed, -255, 255);
  if (speed > 0) {
    ledcWrite(MOT_R_IN1, (uint8_t)speed);
    ledcWrite(MOT_R_IN2, 0);
  } else if (speed < 0) {
    ledcWrite(MOT_R_IN1, 0);
    ledcWrite(MOT_R_IN2, (uint8_t)(-speed));
  } else {
    ledcWrite(MOT_R_IN1, 255);
    ledcWrite(MOT_R_IN2, 255);
  }
}

void motorStop() {
  motorSetLeft(0);
  motorSetRight(0);
  motorsRunning = false;
  motorStopTime = 0;
  Serial.println("[MOT] Stop.");
}

void motorForward(uint8_t speed) {
  motorSetLeft(speed);
  motorSetRight(speed);
  motorsRunning = true;
  Serial.printf("[MOT] Avanti @ %d\n", speed);
}

void motorBackward(uint8_t speed) {
  motorSetLeft(-speed);
  motorSetRight(-speed);
  motorsRunning = true;
  Serial.printf("[MOT] Indietro @ %d\n", speed);
}

void motorTurnLeft(uint8_t speed) {
  // Ruota sinistro indietro, destro avanti
  motorSetLeft(-speed);
  motorSetRight(speed);
  motorsRunning = true;
  Serial.printf("[MOT] Gira sinistra @ %d\n", speed);
}

void motorTurnRight(uint8_t speed) {
  // Ruota sinistro avanti, destro indietro
  motorSetLeft(speed);
  motorSetRight(-speed);
  motorsRunning = true;
  Serial.printf("[MOT] Gira destra @ %d\n", speed);
}

// ─── TASK: MOTOR WATCHDOG ────────────────────────────────────────────────────
/*
 * Controlla:
 * 1. Timeout durata: ferma i motori dopo duration_ms
 * 2. Sicurezza: ferma se perde connessione WebSocket
 */
void taskMotorWatchdog(void* param) {
  Serial.println("[MOT Task] Avviato.");

  for (;;) {
    if (motorsRunning) {
      // Check timeout durata
      if (motorStopTime > 0 && millis() >= motorStopTime) {
        Serial.println("[MOT] Timeout durata raggiunto.");
        motorStop();

        // Notifica server
        if (wsCmdConnected) {
          wsCmd.sendTXT("{\"event\":\"motor_timeout\"}");
        }
      }

      // Sicurezza: ferma se disconnesso
      if (!wsCmdConnected) {
        Serial.println("[MOT] Disconnesso — stop sicurezza!");
        motorStop();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ─── TASK: BUMPER MONITOR ────────────────────────────────────────────────────
/*
 * Monitora i bumper con debounce.
 * Se un bumper viene premuto:
 *   1. Ferma immediatamente i motori
 *   2. Breve retromarcia per allontanarsi dall'ostacolo
 *   3. Notifica il server
 */
void taskBumperMonitor(void* param) {
  Serial.println("[BUMP Task] Avviato.");

  bool lastLeft  = false;
  bool lastRight = false;

  for (;;) {
    bool curLeft  = digitalRead(BUMPER_LEFT_PIN) == HIGH;
    bool curRight = digitalRead(BUMPER_RIGHT_PIN) == HIGH;
    uint32_t now  = millis();

    // ── Bumper sinistro ──────────────────────────────────────────────────
    if (curLeft && !lastLeft && (now - lastBumperLeftMs > BUMPER_DEBOUNCE_MS)) {
      lastBumperLeftMs = now;
      Serial.println("[BUMP] LEFT premuto!");

      // Stop immediato
      motorStop();

      // Breve retromarcia + gira a destra per evitare ostacolo
      motorBackward(150);
      vTaskDelay(pdMS_TO_TICKS(300));
      motorTurnRight(150);
      vTaskDelay(pdMS_TO_TICKS(400));
      motorStop();

      // LED flash rosso
      userLedOverride = true;
      userLedOverrideUntil = millis() + 2000;
      setLedColor(255, 0, 0);

      // Notifica server
      if (wsCmdConnected) {
        wsCmd.sendTXT("{\"event\":\"bumper_hit\",\"side\":\"left\"}");
      }
    }

    // ── Bumper destro ────────────────────────────────────────────────────
    if (curRight && !lastRight && (now - lastBumperRightMs > BUMPER_DEBOUNCE_MS)) {
      lastBumperRightMs = now;
      Serial.println("[BUMP] RIGHT premuto!");

      // Stop immediato
      motorStop();

      // Breve retromarcia + gira a sinistra per evitare ostacolo
      motorBackward(150);
      vTaskDelay(pdMS_TO_TICKS(300));
      motorTurnLeft(150);
      vTaskDelay(pdMS_TO_TICKS(400));
      motorStop();

      // LED flash rosso
      userLedOverride = true;
      userLedOverrideUntil = millis() + 2000;
      setLedColor(255, 0, 0);

      // Notifica server
      if (wsCmdConnected) {
        wsCmd.sendTXT("{\"event\":\"bumper_hit\",\"side\":\"right\"}");
      }
    }

    lastLeft  = curLeft;
    lastRight = curRight;

    vTaskDelay(pdMS_TO_TICKS(10));  // polling 100Hz
  }
}

// ─── UTILITY ─────────────────────────────────────────────────────────────────
int16_t computeRMS(int16_t* samples, size_t count) {
  if (count == 0) return 0;
  int64_t sum = 0;
  for (size_t i = 0; i < count; i++) {
    sum += (int64_t)samples[i] * samples[i];
  }
  return (int16_t)sqrt((double)(sum / count));
}
