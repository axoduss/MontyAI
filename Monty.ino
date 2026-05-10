/*
 * ROBOT ESP32-S3 N16R8
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
#include <Adafruit_Sensor.h>
#include <Adafruit_BMP280.h>
#include <MPU6500_WE.h> 

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


// ─── ULTRASUONI HC-SR04 ──────────────────────────────────────────────────────
#define US_TRIG_PIN       GPIO_NUM_9
#define US_ECHO_PIN       GPIO_NUM_10
#define US_MAX_DISTANCE_CM    400   // oltre questo, consideriamo "nessun ostacolo"
#define US_TIMEOUT_US         25000 // timeout echo (~4.3m)
#define US_READ_INTERVAL_MS   50    // lettura ogni 50ms (20Hz)
#define US_ANTICOLLISION_CM   20    // distanza minima anticollisione
#define US_SLOWDOWN_CM        40    // distanza a cui rallentare
#define US_FOLLOW_TARGET_CM   80    // distanza target follow-me
#define US_FOLLOW_TOLERANCE   15    // ±15cm dal target = fermo
#define US_FOLLOW_MAX_CM      200   // oltre questo, perso il target
#define US_FOLLOW_MIN_CM      30    // troppo vicino, indietreggia
#define US_SCAN_STEP_DEG      10    // gradi per step nella scansione 360
#define US_SCAN_SETTLE_MS     150   // ms di attesa dopo rotazione per stabilizzare lettura
#define US_MEDIAN_SAMPLES     3     // letture per mediana (più robusto)



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


// Indirizzi I2C di default 
#define MPU6500_ADDR 0x68
#define BMP280_ADDR  0x76 


// ─── CONFIGURAZIONE SENSORI IMU ──────────────────────────────────────────────────
#define SENSOR_READ_INTERVAL_MS    20   // Lettura accelerometro a 10Hz (per tap detection)
#define SENSOR_REPORT_INTERVAL_MS  1000  // Invio dati al server ogni 1s
#define TAP_THRESHOLD              0.2f  // Soglia accelerazione per "colpetto" (in g)
#define TAP_COOLDOWN_MS            1000  // Cooldown tra tap consecutivi
#define TILT_THRESHOLD_DEG         20.0f // Gradi di inclinazione per allarme
#define TILT_SUSTAINED_MS          500   // ms di inclinazione continua per triggerare
#define FREEFALL_THRESHOLD         0.3f  // Sotto 0.3g = caduta libera
#define FREEFALL_DURATION_MS       150   // ms di caduta libera per triggerare
#define TEMP_OFFSET                -4.0f  // Offset statico per calibrazione BMP280 (in °C)



// ─── OGGETTI GLOBALI ─────────────────────────────────────────────────────────
WebSocketsClient wsAudio;   // WebSocket per stream audio → server
WebSocketsClient wsCmd;     // WebSocket per comandi JSON ← server

Adafruit_NeoPixel strip(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
DisplayManager display;

// Per i sensori
Adafruit_BMP280 bmp;
MPU6500_WE mpu = MPU6500_WE(MPU6500_ADDR);

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




// Struttura per stato sensori (condivisa con altri task se necessario)
struct SensorState {
  // BMP280
  float temperature;
  float pressure;
  
  // MPU6500 - valori correnti
  float accelX, accelY, accelZ;
  float gyroX, gyroY, gyroZ;
  
  // Valori derivati
  float accelMagnitude;  // |a| = sqrt(x²+y²+z²)
  float tiltAngleX;      // inclinazione asse X (gradi)
  float tiltAngleY;      // inclinazione asse Y (gradi)
  
  // Eventi
  bool tapDetected;
  float tapIntensity;
  bool tiltAlert;
  bool freefallDetected;
  
  // Timestamp
  uint32_t lastTapMs;
  uint32_t tiltStartMs;
  uint32_t freefallStartMs;
};

volatile SensorState sensorState = {};
SemaphoreHandle_t sensorMutex;


// ─── STATO ULTRASUONI ────────────────────────────────────────────────────────
enum UltrasonicMode { 
  US_MODE_MONITOR,      // Solo monitoraggio distanza + anticollisione
  US_MODE_FOLLOW,       // Follow-me
  US_MODE_SCAN          // Scansione 360°
};

struct UltrasonicState {
  float distanceCm;           // Ultima distanza misurata
  float filteredDistanceCm;   // Distanza filtrata (media mobile)
  bool  obstacleDetected;     // Ostacolo sotto soglia anticollisione
  UltrasonicMode mode;        // Modalità corrente
  
  // Follow-me
  bool followActive;
  uint8_t followSpeed;
  
  // Scansione 360°
  bool scanRequested;         // Flag: avvia scansione
  bool scanInProgress;        // Scansione in corso
  float scanData[36];         // Distanze ogni 10° (36 punti)
  float scanAngles[36];       // Angoli reali (da IMU)
  uint8_t scanPoints;         // Punti raccolti
  bool scanComplete;          // Scansione completata, dati pronti
  float scanStartYaw;         // Yaw iniziale dalla IMU
};

volatile UltrasonicState usState = {};
SemaphoreHandle_t usMutex;

// Heading (yaw) integrato dal giroscopio — aggiornato nel task sensori
volatile float imuYawDeg = 0.0f;


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
TaskHandle_t taskSensorsHandle = NULL;
TaskHandle_t taskUltrasonicHandle = NULL;


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

// Direzione corrente motori (per anticollisione intelligente)
enum MotorDirection { MOT_DIR_STOP, MOT_DIR_FORWARD, MOT_DIR_BACKWARD, MOT_DIR_TURN };
volatile MotorDirection motorDirection = MOT_DIR_STOP;

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
void setupSensors();
void setupMotors();
void setupBumpers();
void setupUltrasonic();

void taskSensors(void* param);
void taskMic(void* param);
void taskSpeaker(void* param);
void taskStatusLed(void* param);
void taskCommandProcessor(void* param);
void taskMotorWatchdog(void* param);
void taskBumperMonitor(void* param);
void taskDisplay(void* param);
void taskUltrasonic(void* param);

void handleCommand(const char* json);
int16_t computeRMS(int16_t* samples, size_t count);
void flushTtsQueue();

void setLedColor(uint8_t r, uint8_t g, uint8_t b);
void setLedEffect(const char* effect);
void motorSetLeft(int16_t speed);
void motorSetRight(int16_t speed);
void motorStop();
void motorForward(uint8_t speed);
void motorBackward(uint8_t speed);
void motorTurnLeft(uint8_t speed);
void motorTurnRight(uint8_t speed);
float usReadDistanceCm();
float usReadMedianCm(uint8_t samples);
void usAnticollisionCheck(float distCm);
void usFollowMeLogic(float distCm);
void usScan360();


void wsAudioEvent(WStype_t type, uint8_t* payload, size_t length);
void wsCmdEvent(WStype_t type, uint8_t* payload, size_t length);

EyeExpression parseExpression(const char* str);



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
  setupUltrasonic();     
  setupWebSockets();
  setupSensors();

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
  xTaskCreatePinnedToCore(taskSensors, "SensTask", 4096, NULL, 2, &taskSensorsHandle, 0);
  xTaskCreatePinnedToCore(taskUltrasonic, "USTask", 8192, NULL, 5, &taskUltrasonicHandle, 0);


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

// ─── SETUP ULTRASUONI ────────────────────────────────────────────────────────
void setupUltrasonic() {
  pinMode(US_TRIG_PIN, OUTPUT);
  pinMode(US_ECHO_PIN, INPUT);
  digitalWrite(US_TRIG_PIN, LOW);
  
  usMutex = xSemaphoreCreateMutex();
  
  // Inizializza stato
  memset((void*)&usState, 0, sizeof(UltrasonicState));
  usState.mode = US_MODE_MONITOR;
  for (int i = 0; i < 36; i++) usState.scanData[i] = -1.0f;
  
  Serial.println("[US] HC-SR04 pronto (Trig=GPIO9, Echo=GPIO10).");
}

// ─── LETTURA DISTANZA (singola, non bloccante per il task) ───────────────────
float usReadDistanceCm() {
  // Trigger pulse: 10µs HIGH
  digitalWrite(US_TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(US_TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(US_TRIG_PIN, LOW);
  
  // Misura durata echo
  unsigned long duration = pulseIn(US_ECHO_PIN, HIGH, US_TIMEOUT_US);
  
  if (duration == 0) {
    return -1.0f;  // Nessun echo (fuori range o errore)
  }
  
  // Velocità suono ~343 m/s → 0.0343 cm/µs → andata+ritorno /2
  float distance = (duration * 0.0343f) / 2.0f;
  
  if (distance > US_MAX_DISTANCE_CM) {
    return -1.0f;
  }
  
  return distance;
}

// ─── LETTURA CON MEDIANA (più robusta) ───────────────────────────────────────
float usReadMedianCm(uint8_t samples) {
  float readings[samples];
  uint8_t validCount = 0;
  
  for (uint8_t i = 0; i < samples; i++) {
    float d = usReadDistanceCm();
    if (d > 0) {
      readings[validCount++] = d;
    }
    if (i < samples - 1) delayMicroseconds(2500); // pausa tra letture
  }
  
  if (validCount == 0) return -1.0f;
  
  // Bubble sort per mediana (array piccolo, OK)
  for (uint8_t i = 0; i < validCount - 1; i++) {
    for (uint8_t j = 0; j < validCount - i - 1; j++) {
      if (readings[j] > readings[j + 1]) {
        float tmp = readings[j];
        readings[j] = readings[j + 1];
        readings[j + 1] = tmp;
      }
    }
  }
  
  return readings[validCount / 2];
}

// ─── ANTICOLLISIONE ──────────────────────────────────────────────────────────
/*
 * Integra con i bumper: gli ultrasuoni fermano il robot PRIMA dell'impatto.
 * I bumper restano come ultima linea di difesa (contatto fisico).
 */
void usAnticollisionCheck(float distCm) {
  if (distCm < 0) return;  // Lettura non valida, non agire
  
  if (!motorsRunning || motorDirection != MOT_DIR_FORWARD) return;  // Motori fermi, niente da fare
  
  // Controlla solo se il robot sta andando AVANTI
  // (non bloccare retromarcia o rotazioni)
  // Leggiamo i registri PWM per capire la direzione
  // Semplificazione: usiamo un flag globale
  
  if (distCm <= US_ANTICOLLISION_CM) {
    // STOP IMMEDIATO — ostacolo troppo vicino
    Serial.printf("[US] ⚠️ ANTICOLLISIONE! Distanza: %.1f cm\n", distCm);
    motorStop();
    
    // Feedback visivo
    userLedOverride = true;
    userLedOverrideUntil = millis() + 2000;
    setLedColor(255, 50, 0);  // arancione
    
    // Notifica server
    if (wsCmdConnected) {
      char payload[128];
      snprintf(payload, sizeof(payload),
        "{\"event\":\"us_anticollision\",\"distance_cm\":%.1f}", distCm);
      wsCmd.sendTXT(payload);
    }
    
    xSemaphoreTake(usMutex, portMAX_DELAY);
    usState.obstacleDetected = true;
    xSemaphoreGive(usMutex);
  } else if (distCm <= US_SLOWDOWN_CM) {
    // Zona di rallentamento — potremmo ridurre la velocità
    // Per ora solo segnalazione (il server può decidere)
    xSemaphoreTake(usMutex, portMAX_DELAY);
    usState.obstacleDetected = false;
    xSemaphoreGive(usMutex);
  } else {
    xSemaphoreTake(usMutex, portMAX_DELAY);
    usState.obstacleDetected = false;
    xSemaphoreGive(usMutex);
  }
}

// ─── FOLLOW-ME ───────────────────────────────────────────────────────────────
/*
 * Con un solo sensore frontale:
 *   - Se oggetto nel range target ± tolleranza → fermo
 *   - Se oggetto più lontano del target → avanti
 *   - Se oggetto più vicino del minimo → indietro
 *   - Se nessun oggetto rilevato → fermo (target perso)
 *
 * Limitazione: non può seguire lateralmente. Il server/LLM può integrare
 * con rotazioni periodiche per "cercare" il target.
 */
void usFollowMeLogic(float distCm) {
  if (distCm < 0 || distCm > US_FOLLOW_MAX_CM) {
    // Target perso
    if (motorsRunning) {
      motorStop();
      Serial.println("[US-FOLLOW] Target perso, stop.");
      if (wsCmdConnected) {
        wsCmd.sendTXT("{\"event\":\"follow_target_lost\"}");
      }
    }
    return;
  }
  
  uint8_t speed = usState.followSpeed > 0 ? usState.followSpeed : 120;
  
  if (distCm < US_FOLLOW_MIN_CM) {
    // Troppo vicino — indietreggia
    motorBackward(speed / 2);
  } else if (distCm < (US_FOLLOW_TARGET_CM - US_FOLLOW_TOLERANCE)) {
    // Sotto il target — indietreggia piano
    motorBackward(speed / 3);
  } else if (distCm > (US_FOLLOW_TARGET_CM + US_FOLLOW_TOLERANCE)) {
    // Oltre il target — avvicinati
    // Velocità proporzionale alla distanza
    uint8_t dynSpeed = map(constrain((int)distCm, US_FOLLOW_TARGET_CM, US_FOLLOW_MAX_CM),
                           US_FOLLOW_TARGET_CM, US_FOLLOW_MAX_CM,
                           speed / 2, speed);
    motorForward(dynSpeed);
  } else {
    // Nel range — fermo
    if (motorsRunning) {
      motorStop();
    }
  }
}

// ─── SCANSIONE 360° ─────────────────────────────────────────────────────────
/*
 * Ruota il robot sul posto di 360° a step di US_SCAN_STEP_DEG,
 * leggendo la distanza ad ogni step.
 * Usa il giroscopio (imuYawDeg) per sapere l'angolo reale.
 *
 * NOTA: Questa funzione è BLOCCANTE per il task ultrasuoni.
 * Durante la scansione, l'anticollisione è sospesa.
 */
void usScan360() {
  Serial.println("[US-SCAN] Avvio scansione 360°...");
  
  xSemaphoreTake(usMutex, portMAX_DELAY);
  usState.scanInProgress = true;
  usState.scanComplete = false;
  usState.scanPoints = 0;
  usState.scanStartYaw = imuYawDeg;
  xSemaphoreGive(usMutex);
  
  // Notifica server
  if (wsCmdConnected) {
    wsCmd.sendTXT("{\"event\":\"scan_started\"}");
  }
  
  // Feedback display
  display.showText("Scansione", "360 gradi...", "", "", 1, 0);
  
  int totalSteps = 360 / US_SCAN_STEP_DEG;  // 36 step
  float startYaw = imuYawDeg;
  
  for (int step = 0; step < totalSteps; step++) {
    // Angolo target per questo step
    float targetAngle = startYaw + (step * US_SCAN_STEP_DEG);
    // Normalizza a 0-360
    while (targetAngle >= 360.0f) targetAngle -= 360.0f;
    while (targetAngle < 0.0f) targetAngle += 360.0f;
    
    // Ruota fino all'angolo target
    float currentYaw = imuYawDeg;
    float angleDiff = targetAngle - currentYaw;
    // Normalizza differenza a -180..+180
    while (angleDiff > 180.0f) angleDiff -= 360.0f;
    while (angleDiff < -180.0f) angleDiff += 360.0f;
    
    if (step > 0) {  // Il primo step è la posizione corrente
      // Ruota nella direzione giusta
      if (angleDiff > 0) {
        motorTurnRight(100);
      } else {
        motorTurnLeft(100);
      }
      
            // Attendi di raggiungere l'angolo (con timeout)
      uint32_t rotStart = millis();
      while (millis() - rotStart < 2000) {  // max 2s per step
        currentYaw = imuYawDeg;
        float remaining = targetAngle - currentYaw;
        while (remaining > 180.0f) remaining -= 360.0f;
        while (remaining < -180.0f) remaining += 360.0f;
        
        if (fabsf(remaining) < 3.0f) {  // ±3° di tolleranza
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
      }
      
      motorStop();
      vTaskDelay(pdMS_TO_TICKS(US_SCAN_SETTLE_MS));  // Attendi stabilizzazione
    }
    
    // Leggi distanza (mediana per robustezza)
    float dist = usReadMedianCm(US_MEDIAN_SAMPLES);
    float actualAngle = imuYawDeg;
    
    // Salva nel buffer
    xSemaphoreTake(usMutex, portMAX_DELAY);
    usState.scanData[step] = dist;
    usState.scanAngles[step] = actualAngle;
    usState.scanPoints = step + 1;
    xSemaphoreGive(usMutex);
    
    // Invia punto al server in tempo reale (per aggiornamento live sulla dashboard)
    if (wsCmdConnected) {
      char payload[128];
      snprintf(payload, sizeof(payload),
        "{\"event\":\"scan_point\",\"step\":%d,\"angle\":%.1f,\"distance\":%.1f}",
        step, actualAngle, dist);
      wsCmd.sendTXT(payload);
    }
    
    // Aggiorna progress sul display
    uint8_t pct = (uint8_t)((step + 1) * 100 / totalSteps);
    display.showProgress(pct, "Scansione...", 0);
    
    Serial.printf("[US-SCAN] Step %d/%d: angolo=%.1f° dist=%.1f cm\n",
                  step + 1, totalSteps, actualAngle, dist);
  }
  
  // Scansione completata — torna alla posizione iniziale (opzionale)
  float returnDiff = startYaw - imuYawDeg;
  while (returnDiff > 180.0f) returnDiff -= 360.0f;
  while (returnDiff < -180.0f) returnDiff += 360.0f;
  
  if (fabsf(returnDiff) > 5.0f) {
    if (returnDiff > 0) motorTurnRight(100);
    else motorTurnLeft(100);
    
    uint32_t retStart = millis();
    while (millis() - retStart < 3000) {
      float rem = startYaw - imuYawDeg;
      while (rem > 180.0f) rem -= 360.0f;
      while (rem < -180.0f) rem += 360.0f;
      if (fabsf(rem) < 3.0f) break;
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    motorStop();
  }
  
  // Marca come completata
  xSemaphoreTake(usMutex, portMAX_DELAY);
  usState.scanInProgress = false;
  usState.scanComplete = true;
  xSemaphoreGive(usMutex);
  
  // Invia dati completi al server
  if (wsCmdConnected) {
    // Invia come array JSON compatto
    char header[64];
    snprintf(header, sizeof(header),
      "{\"event\":\"scan_complete\",\"points\":%d,\"data\":[", totalSteps);
    
    // Costruisci il payload completo (max ~1200 byte per 36 punti)
    char fullPayload[1400];
    strcpy(fullPayload, header);
    
    for (int i = 0; i < totalSteps; i++) {
      char point[40];
      snprintf(point, sizeof(point), "{\"a\":%.1f,\"d\":%.1f}%s",
        usState.scanAngles[i], usState.scanData[i],
        (i < totalSteps - 1) ? "," : "");
      strcat(fullPayload, point);
    }
    strcat(fullPayload, "]}");
    
    wsCmd.sendTXT(fullPayload);
  }
  
  // Torna agli occhi
  display.showEyes();
  Serial.println("[US-SCAN] Scansione completata!");
}


// ─── TASK: ULTRASUONI ────────────────────────────────────────────────────────
/*
 * Task dedicato per HC-SR04.
 * Gestisce:
 *   - Lettura periodica distanza (20Hz)
 *   - Anticollisione (sempre attiva in MONITOR mode)
 *   - Follow-me (quando attivato)
 *   - Scansione 360° (su richiesta)
 *
 * Priorità 5: alta come il microfono (sicurezza!)
 */
void taskUltrasonic(void* param) {
  Serial.println("[US Task] Avviato.");
  
  uint32_t lastReadMs = 0;
  uint32_t lastReportMs = 0;
  
  // Buffer per media mobile (filtro rumore)
  float distHistory[5] = {0};
  uint8_t histIdx = 0;
  uint8_t validReadings = 0;
  
  for (;;) {
    uint32_t now = millis();
    
    // ── Controlla se è richiesta una scansione ───────────────────────────
    bool doScan = false;
    xSemaphoreTake(usMutex, portMAX_DELAY);
    if (usState.scanRequested) {
      usState.scanRequested = false;
      doScan = true;
    }
    xSemaphoreGive(usMutex);
    
    if (doScan) {
      usScan360();
      continue;  // Dopo la scansione, riprendi il loop normale
    }
    
    // ── Lettura periodica ────────────────────────────────────────────────
    if (now - lastReadMs >= US_READ_INTERVAL_MS) {
      lastReadMs = now;
      
      float rawDist = usReadDistanceCm();
      
      // Filtraggio: media mobile sulle letture valide
      float filteredDist = rawDist;
      if (rawDist > 0) {
        distHistory[histIdx] = rawDist;
        histIdx = (histIdx + 1) % 5;
        if (validReadings < 5) validReadings++;
        
        // Calcola media (escludendo valori non validi)
        float sum = 0;
        uint8_t cnt = 0;
        for (uint8_t i = 0; i < validReadings; i++) {
          if (distHistory[i] > 0) {
            sum += distHistory[i];
            cnt++;
          }
        }
        filteredDist = (cnt > 0) ? (sum / cnt) : rawDist;
      }
      
      // Aggiorna stato condiviso
      xSemaphoreTake(usMutex, portMAX_DELAY);
      usState.distanceCm = rawDist;
      usState.filteredDistanceCm = filteredDist;
      UltrasonicMode currentMode = usState.mode;
      xSemaphoreGive(usMutex);
      
      // ── Logica in base alla modalità ───────────────────────────────────
      switch (currentMode) {
        case US_MODE_MONITOR:
          // Anticollisione sempre attiva
          usAnticollisionCheck(filteredDist);
          break;
          
        case US_MODE_FOLLOW:
          // Follow-me (include anticollisione implicita)
          usFollowMeLogic(filteredDist);
          break;
          
        case US_MODE_SCAN:
          // La scansione è gestita sopra con scanRequested
          break;
      }
    }
    
    // ── Report periodico al server (ogni 500ms) ─────────────────────────
    if (now - lastReportMs >= 500) {
      lastReportMs = now;
      
      if (wsCmdConnected) {
        xSemaphoreTake(usMutex, portMAX_DELAY);
        float dist = usState.filteredDistanceCm;
        float rawDist = usState.distanceCm;
        bool obstacle = usState.obstacleDetected;
        UltrasonicMode mode = usState.mode;
        bool scanning = usState.scanInProgress;
        xSemaphoreGive(usMutex);
        
        char payload[200];
        snprintf(payload, sizeof(payload),
          "{\"event\":\"us_data\","
          "\"distance_cm\":%.1f,\"raw_cm\":%.1f,"
          "\"obstacle\":%s,\"mode\":\"%s\",\"scanning\":%s}",
          dist, rawDist,
          obstacle ? "true" : "false",
          mode == US_MODE_MONITOR ? "monitor" : 
            (mode == US_MODE_FOLLOW ? "follow" : "scan"),
          scanning ? "true" : "false");
        
        wsCmd.sendTXT(payload);
      }
    }
    
    vTaskDelay(pdMS_TO_TICKS(10));
  }
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

    // ══════════════════════════════════════════════════════════════════════════
  // ULTRASUONI
  // ══════════════════════════════════════════════════════════════════════════

  // ── us_get_distance ───────────────────────────────────────────────────────
  // {"cmd":"us_get_distance"}
  else if (strcmp(cmd, "us_get_distance") == 0) {
    xSemaphoreTake(usMutex, portMAX_DELAY);
    float dist = usState.filteredDistanceCm;
    float raw = usState.distanceCm;
    bool obstacle = usState.obstacleDetected;
    xSemaphoreGive(usMutex);
    
    char ack[128];
    snprintf(ack, sizeof(ack),
      "{\"ack\":\"us_get_distance\",\"distance_cm\":%.1f,\"raw_cm\":%.1f,\"obstacle\":%s}",
      dist, raw, obstacle ? "true" : "false");
    wsCmd.sendTXT(ack);
  }

  // ── us_set_mode ───────────────────────────────────────────────────────────
  // {"cmd":"us_set_mode","params":{"mode":"monitor|follow|scan"}}
  else if (strcmp(cmd, "us_set_mode") == 0) {
    const char* modeStr = doc["params"]["mode"].as<const char*>();
    if (modeStr) {
      xSemaphoreTake(usMutex, portMAX_DELAY);
      if (strcmp(modeStr, "monitor") == 0) {
        usState.mode = US_MODE_MONITOR;
        usState.followActive = false;
      } else if (strcmp(modeStr, "follow") == 0) {
        usState.mode = US_MODE_FOLLOW;
        usState.followActive = true;
        usState.followSpeed = doc["params"]["speed"] | 120;
      } else if (strcmp(modeStr, "scan") == 0) {
        usState.mode = US_MODE_MONITOR;  // Torna a monitor dopo scan
        usState.scanRequested = true;
      }
      xSemaphoreGive(usMutex);
      
      char ack[80];
      snprintf(ack, sizeof(ack), "{\"ack\":\"us_set_mode\",\"mode\":\"%s\"}", modeStr);
      wsCmd.sendTXT(ack);
    }
  }

  // ── us_set_thresholds ─────────────────────────────────────────────────────
  // {"cmd":"us_set_thresholds","params":{"anticollision_cm":25,"slowdown_cm":50}}
  else if (strcmp(cmd, "us_set_thresholds") == 0) {
    // Per ora usiamo le costanti, ma potremmo renderle variabili
    // Questo comando è un placeholder per futura configurabilità
    wsCmd.sendTXT("{\"ack\":\"us_set_thresholds\",\"note\":\"using compile-time defaults\"}");
  }

  // ── us_scan ───────────────────────────────────────────────────────────────
  // {"cmd":"us_scan"} — shortcut per avviare scansione
  else if (strcmp(cmd, "us_scan") == 0) {
    xSemaphoreTake(usMutex, portMAX_DELAY);
    usState.scanRequested = true;
    xSemaphoreGive(usMutex);
    wsCmd.sendTXT("{\"ack\":\"us_scan\",\"status\":\"started\"}");
  }

  // ── us_follow_config ──────────────────────────────────────────────────────
  // {"cmd":"us_follow_config","params":{"target_cm":100,"speed":130}}
  else if (strcmp(cmd, "us_follow_config") == 0) {
    // Nota: per renderli dinamici, dovremmo usare variabili invece di #define
    // Per ora logghiamo e usiamo i default
    uint8_t speed = doc["params"]["speed"] | 120;
    xSemaphoreTake(usMutex, portMAX_DELAY);
    usState.followSpeed = speed;
    xSemaphoreGive(usMutex);
    
    char ack[80];
    snprintf(ack, sizeof(ack), "{\"ack\":\"us_follow_config\",\"speed\":%d}", speed);
    wsCmd.sendTXT(ack);
  }

    // ── us_get_scan_data ──────────────────────────────────────────────────────
  // {"cmd":"us_get_scan_data"} — richiedi ultimi dati scansione
  else if (strcmp(cmd, "us_get_scan_data") == 0) {
    xSemaphoreTake(usMutex, portMAX_DELAY);
    bool hasData = usState.scanComplete;
    uint8_t points = usState.scanPoints;
    xSemaphoreGive(usMutex);
    
    if (!hasData || points == 0) {
      wsCmd.sendTXT("{\"ack\":\"us_get_scan_data\",\"available\":false}");
    } else {
      // Invia i dati (stessa logica di scan_complete)
      char fullPayload[1400];
      snprintf(fullPayload, sizeof(fullPayload),
        "{\"ack\":\"us_get_scan_data\",\"available\":true,\"points\":%d,\"data\":[", points);
      
      xSemaphoreTake(usMutex, portMAX_DELAY);
      for (int i = 0; i < points; i++) {
        char point[40];
        snprintf(point, sizeof(point), "{\"a\":%.1f,\"d\":%.1f}%s",
          usState.scanAngles[i], usState.scanData[i],
          (i < points - 1) ? "," : "");
        strcat(fullPayload, point);
      }
      xSemaphoreGive(usMutex);
      
      strcat(fullPayload, "]}");
      wsCmd.sendTXT(fullPayload);
    }
  }

  // ── us_stop_follow ────────────────────────────────────────────────────────
  // {"cmd":"us_stop_follow"} — ferma follow-me e torna a monitor
  else if (strcmp(cmd, "us_stop_follow") == 0) {
    xSemaphoreTake(usMutex, portMAX_DELAY);
    usState.mode = US_MODE_MONITOR;
    usState.followActive = false;
    xSemaphoreGive(usMutex);
    motorStop();
    wsCmd.sendTXT("{\"ack\":\"us_stop_follow\"}");
  }

  // ── us_calibrate_yaw ──────────────────────────────────────────────────────
  // {"cmd":"us_calibrate_yaw"} — resetta lo yaw a 0 (posizione corrente = nord)
  else if (strcmp(cmd, "us_calibrate_yaw") == 0) {
    imuYawDeg = 0.0f;
    wsCmd.sendTXT("{\"ack\":\"us_calibrate_yaw\",\"yaw\":0}");
    Serial.println("[US] Yaw resettato a 0°.");
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
      //if (currentState == SPEAKING || currentState == PLAYING_MUSIC) {
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
  motorDirection = MOT_DIR_STOP;
  Serial.println("[MOT] Stop.");
}

void motorForward(uint8_t speed) {
  // Check preventivo ultrasuoni prima di partire
  xSemaphoreTake(usMutex, portMAX_DELAY);
  float dist = usState.filteredDistanceCm;
  bool obstacle = usState.obstacleDetected;
  xSemaphoreGive(usMutex);
  
  if (obstacle || (dist > 0 && dist <= US_ANTICOLLISION_CM)) {
    Serial.printf("[MOT] Avanti BLOCCATO — ostacolo a %.1f cm!\n", dist);
    if (wsCmdConnected) {
      char payload[128];
      snprintf(payload, sizeof(payload),
        "{\"event\":\"move_blocked\",\"reason\":\"obstacle\",\"distance_cm\":%.1f}", dist);
      wsCmd.sendTXT(payload);
    }
    return;  // Non muovere!
  }

  motorSetLeft(speed);
  motorSetRight(speed);
  motorsRunning = true;
  motorDirection = MOT_DIR_FORWARD;
  Serial.printf("[MOT] Avanti @ %d\n", speed);
}

void motorBackward(uint8_t speed) {
  motorSetLeft(-speed);
  motorSetRight(-speed);
  motorsRunning = true;
  motorDirection = MOT_DIR_BACKWARD;
  Serial.printf("[MOT] Indietro @ %d\n", speed);
}

void motorTurnLeft(uint8_t speed) {
  // Ruota sinistro indietro, destro avanti
  motorSetLeft(-speed);
  motorSetRight(speed);
  motorsRunning = true;
  motorDirection = MOT_DIR_TURN;
  Serial.printf("[MOT] Gira sinistra @ %d\n", speed);
}

void motorTurnRight(uint8_t speed) {
  // Ruota sinistro avanti, destro indietro
  motorSetLeft(speed);
  motorSetRight(-speed);
  motorsRunning = true;
  motorDirection = MOT_DIR_TURN;
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

// ─── SETUP SENSORI I2C ───────────────────────────────────────────────────────
void setupSensors() {
  Wire.begin(I2C_SDA, I2C_SCL);
  Serial.println("[SENS] Inizializzazione I2C...");

  // Crea mutex per accesso thread-safe ai dati sensori
  sensorMutex = xSemaphoreCreateMutex();

  // Inizializza BMP280
  if (!bmp.begin(BMP280_ADDR)) {
    Serial.println("[SENS] ERRORE: BMP280 non trovato!");
  } else {
    bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                    Adafruit_BMP280::SAMPLING_X2,
                    Adafruit_BMP280::SAMPLING_X16,
                    Adafruit_BMP280::FILTER_X16,
                    Adafruit_BMP280::STANDBY_MS_500);
    Serial.println("[SENS] BMP280 pronto.");
  }

  // Inizializza MPU6500
  if (!mpu.init()) {
    Serial.println("[SENS] ERRORE: MPU6500 non trovato!");
  } else {
    Serial.println("[SENS] MPU6500 pronto. Calibrazione...");
    delay(1000); // Attendi che il robot sia fermo
    mpu.autoOffsets();
    
    // Configura range per migliore sensibilità ai tap
    mpu.setAccRange(MPU6500_ACC_RANGE_4G);   // ±4g: buon compromesso
    mpu.setGyrRange(MPU6500_GYRO_RANGE_500); // ±500°/s
    
    Serial.println("[SENS] MPU6500 calibrato e configurato.");
  }
  
  // Inizializza stato
  memset((void*)&sensorState, 0, sizeof(SensorState));
}

// ─── TASK: LETTURA SENSORI ───────────────────────────────────────────────────
/*
 * Questo task fa DUE cose a frequenze diverse:
 *   - Ogni 100ms: legge accelerometro/giroscopio per rilevare eventi rapidi
 *   - Ogni 1000ms: legge anche BMP280 e invia report completo al server
 *
 * Eventi rilevati localmente (bassa latenza):
 *   - TAP: picco improvviso di accelerazione
 *   - TILT: inclinazione eccessiva sostenuta
 *   - FREEFALL: accelerazione vicina a 0 (caduta)
 */

void taskSensors(void* param) {
  Serial.println("[SENS Task] Avviato.");

  uint32_t lastReportMs = 0;
  uint32_t lastReadMs = 0;
  float lastAccelMag = 1.0f; 
  
  // Buffer per media mobile (smoothing)
  float accelHistory[5] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
  uint8_t histIdx = 0;
  
  // Stato locale per rilevamento eventi
  bool wasTilted = false;
  uint32_t tiltStartMs = 0;
  uint32_t freefallStartMs = 0;
  bool wasInFreefall = false;

  for (;;) {
    uint32_t now = millis();

    // ══════════════════════════════════════════════════════════════════════════
    // LETTURA RAPIDA
    // ══════════════════════════════════════════════════════════════════════════
    if (now - lastReadMs >= SENSOR_READ_INTERVAL_MS) {
      lastReadMs = now;

      // Lettura MPU6500
      xyzFloat gValue = mpu.getGValues();
      xyzFloat gyr = mpu.getGyrValues();

      float ax = gValue.x;
      float ay = gValue.y;
      float az = gValue.z;
      float gx = gyr.x;
      float gy = gyr.y;
      float gz = gyr.z;

      // ── INTEGRAZIONE YAW (per scansione 360°) ─────────────────────────
      // Integra il giroscopio Z per ottenere l'heading approssimato
      // dt = SENSOR_READ_INTERVAL_MS / 1000.0
      float dt = SENSOR_READ_INTERVAL_MS / 1000.0f;
      imuYawDeg += gz * dt;
      // Normalizza a 0-360
      while (imuYawDeg >= 360.0f) imuYawDeg -= 360.0f;
      while (imuYawDeg < 0.0f) imuYawDeg += 360.0f;



      // Magnitudine accelerazione totale
      float accelMag = sqrtf(ax * ax + ay * ay + az * az);

      // Media mobile per filtrare rumore
      accelHistory[histIdx] = accelMag;
      histIdx = (histIdx + 1) % 5;
      float avgMag = 0;
      for (int i = 0; i < 5; i++) avgMag += accelHistory[i];
      avgMag /= 5.0f;

      // Calcolo angoli di inclinazione (gradi)
      float tiltX = atan2f(ay, sqrtf(ax * ax + az * az)) * 57.2958f;
      float tiltY = atan2f(ax, sqrtf(ay * ay + az * az)) * 57.2958f;

      // ── TAP DETECTION ──────────────────────────────────────────────────
      // Un "tap" è un picco improvviso: magnitudine molto sopra 1g
      float deltaAccel = fabsf(accelMag - lastAccelMag);
      // float deviation = fabsf(accelMag - avgMag);
      // bool tapNow = (accelMag > TAP_THRESHOLD) && 
      //               (deviation > 0.8f) &&
      //               (now - sensorState.lastTapMs > TAP_COOLDOWN_MS);
      bool tapNow = (deltaAccel > 0.5f) &&       // Variazione improvvisa
                    (accelMag > TAP_THRESHOLD) && // Supera la soglia minima (es. 1.5f)
                    (now - sensorState.lastTapMs > TAP_COOLDOWN_MS);


      // if (tapNow) {
      //   Serial.printf("[SENS] 🤜 TAP rilevato! Intensità: %.2fg (dev: %.2f)\n", 
      //                 accelMag, deviation);

      if (tapNow) {
        Serial.printf("[SENS] 🤜 TAP rilevato! Intensità: %.2fg (delta: %.2f)\n", 
                      accelMag, deltaAccel);

        xSemaphoreTake(sensorMutex, portMAX_DELAY);
        sensorState.tapDetected = true;
        sensorState.tapIntensity = accelMag;
        sensorState.lastTapMs = now;
        xSemaphoreGive(sensorMutex);

        // Invia evento immediatamente al server
        if (wsCmdConnected) {
          char payload[128];
          snprintf(payload, sizeof(payload),
            "{\"event\":\"tap_detected\",\"intensity\":%.2f,\"accel\":{\"x\":%.2f,\"y\":%.2f,\"z\":%.2f}}",
            accelMag, ax, ay, az);
          wsCmd.sendTXT(payload);
        }

        // Feedback visivo immediato
        userLedOverride = true;
        userLedOverrideUntil = millis() + 1500;
        setLedColor(255, 100, 0); // arancione flash
      }
      // Aggiorna il valore per il prossimo ciclo (mettilo subito dopo l'if del tapNow)
      lastAccelMag = accelMag;

      // ── TILT DETECTION ─────────────────────────────────────────────────
      bool isTilted = (fabsf(tiltX) > TILT_THRESHOLD_DEG) || 
                      (fabsf(tiltY) > TILT_THRESHOLD_DEG);

      if (isTilted && !wasTilted) {
        tiltStartMs = now;
      }

      if (isTilted && (now - tiltStartMs > TILT_SUSTAINED_MS) && !sensorState.tiltAlert) {
        Serial.printf("[SENS] ⚠️ TILT eccessivo! X=%.1f° Y=%.1f°\n", tiltX, tiltY);

        xSemaphoreTake(sensorMutex, portMAX_DELAY);
        sensorState.tiltAlert = true;
        xSemaphoreGive(sensorMutex);

        // Invia evento al server
        if (wsCmdConnected) {
          char payload[128];
          snprintf(payload, sizeof(payload),
            "{\"event\":\"tilt_alert\",\"angle_x\":%.1f,\"angle_y\":%.1f}",
            tiltX, tiltY);
          wsCmd.sendTXT(payload);
        }

        // Feedback: LED rosso lampeggiante
        userLedOverride = true;
        userLedOverrideUntil = millis() + 3000;
        setLedColor(255, 0, 0);
      }

      // Reset tilt quando torna in posizione
      if (!isTilted && sensorState.tiltAlert) {
        xSemaphoreTake(sensorMutex, portMAX_DELAY);
        sensorState.tiltAlert = false;
        xSemaphoreGive(sensorMutex);

        if (wsCmdConnected) {
          wsCmd.sendTXT("{\"event\":\"tilt_recovered\"}");
        }
        userLedOverride = false;
        Serial.println("[SENS] ✓ Inclinazione recuperata.");
      }
      wasTilted = isTilted;

      // ── FREEFALL DETECTION ─────────────────────────────────────────────
      bool inFreefall = (accelMag < FREEFALL_THRESHOLD);

      if (inFreefall && !wasInFreefall) {
        freefallStartMs = now;
      }

      if (inFreefall && (now - freefallStartMs > FREEFALL_DURATION_MS) && !sensorState.freefallDetected) {
        Serial.println("[SENS] 🆘 CADUTA LIBERA rilevata!");

        xSemaphoreTake(sensorMutex, portMAX_DELAY);
        sensorState.freefallDetected = true;
        xSemaphoreGive(sensorMutex);

        if (wsCmdConnected) {
          wsCmd.sendTXT("{\"event\":\"freefall_detected\"}");
        }

        // Stop motori per sicurezza
        motorStop();
        setLedColor(255, 0, 0);
        userLedOverride = true;
        userLedOverrideUntil = millis() + 5000;
      }

      if (!inFreefall && sensorState.freefallDetected) {
        xSemaphoreTake(sensorMutex, portMAX_DELAY);
        sensorState.freefallDetected = false;
        xSemaphoreGive(sensorMutex);
        Serial.println("[SENS] ✓ Caduta terminata (impatto o recupero).");
      }
      wasInFreefall = inFreefall;

      // ── AGGIORNA STATO CONDIVISO ───────────────────────────────────────
      xSemaphoreTake(sensorMutex, portMAX_DELAY);
      sensorState.accelX = ax;
      sensorState.accelY = ay;
      sensorState.accelZ = az;
      sensorState.gyroX = gx;
      sensorState.gyroY = gy;
      sensorState.gyroZ = gz;
      sensorState.accelMagnitude = accelMag;
      sensorState.tiltAngleX = tiltX;
      sensorState.tiltAngleY = tiltY;
      xSemaphoreGive(sensorMutex);
    }

    // ══════════════════════════════════════════════════════════════════════════
    // REPORT PERIODICO (ogni 1s)
    // ══════════════════════════════════════════════════════════════════════════
    if (now - lastReportMs >= SENSOR_REPORT_INTERVAL_MS) {
      lastReportMs = now;

      if (wsCmdConnected) {
        // Lettura BMP280 (lenta, 1Hz è sufficiente)
        float temp = bmp.readTemperature() + TEMP_OFFSET;
        float press = bmp.readPressure() / 100.0F; 

        xSemaphoreTake(sensorMutex, portMAX_DELAY);
        sensorState.temperature = temp;
        sensorState.pressure = press;
        float ax = sensorState.accelX;
        float ay = sensorState.accelY;
        float az = sensorState.accelZ;
        float gx = sensorState.gyroX;
        float gy = sensorState.gyroY;
        float gz = sensorState.gyroZ;
        float tiltX = sensorState.tiltAngleX;
        float tiltY = sensorState.tiltAngleY;
        float mag = sensorState.accelMagnitude;
        xSemaphoreGive(sensorMutex);

        // Payload JSON completo
        char payload[384];
        snprintf(payload, sizeof(payload),
          "{\"event\":\"sensor_data\","
          "\"temp\":%.1f,\"press\":%.1f,"
          "\"accel\":{\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,\"mag\":%.2f},"
          "\"gyro\":{\"x\":%.1f,\"y\":%.1f,\"z\":%.1f},"
          "\"tilt\":{\"x\":%.1f,\"y\":%.1f},"
          "\"yaw\":%.1f,"
           "\"state\":{\"tap\":%s,\"tilt\":%s,\"freefall\":%s}}",
          temp, press,
          ax, ay, az, mag,
          gx, gy, gz,
          tiltX, tiltY,
          imuYawDeg,
          sensorState.tapDetected ? "true" : "false",
          sensorState.tiltAlert ? "true" : "false",
          sensorState.freefallDetected ? "true" : "false");

        wsCmd.sendTXT(payload);

        // Reset flag tap dopo il report (evento one-shot)
        xSemaphoreTake(sensorMutex, portMAX_DELAY);
        sensorState.tapDetected = false;
        xSemaphoreGive(sensorMutex);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(20)); // 50Hz loop interno, eventi controllati dai timer
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
