# Monty - Robot ESP32-S3 con Voice Assistant

Un robot conversazionale basato su ESP32-S3 con controllo vocale, display animato per gli occhi e integrazione con AI locale.

## 🎯 Caratteristiche

- **Controllo Vocale**: Speech-to-text tramite Faster Whisper
- **AI Locale**: Integrazione con Ollama (Gemma4)
- **Sintesi Vocale**: TTS con Piper
- **Display Occhi**: Animazioni NeoPixel WS2812 per esprimere emozioni
- **Skill System**: Architettura modulare per estendere le funzionalità
- **Audio Bidirezionale**: Microfono I2S (INMP441) + Speaker I2S (MAX98357A)
- **Sensori**: BMP280 (pressione/temperatura) + MPU6500 (accelerometro/giroscopio)
- **OTA Updates**: Aggiornamenti over-the-air per il firmware ESP32

## 📁 Struttura del Progetto

```
├── Monty.ino              # Firmware Arduino per ESP32-S3
├── server.py              # Server Python (FastAPI + WebSocket)
├── dashboard.html         # Interfaccia web per il monitoraggio
├── display_eyes.h         # Libreria animazioni occhi
├── credentials.h.example  # Template per le credenziali WiFi
├── skills/                # Moduli skill (meteo, news, timer, ecc.)
├── start.sh               # Script di avvio del server
└── README.md              # Questa documentazione
```

## 🛠️ Requisiti Hardware

- **Microcontrollore**: ESP32-S3 N16R8
- **Microfono**: INMP441 (I2S)
- **Speaker**: MAX98357A (I2S)
- **LED**: NeoPixel WS2812
- **Sensori**: BMP280, MPU6500

## 💻 Requisiti Software

### Per il Server (Python 3.8+)

```bash
pip install fastapi uvicorn websockets numpy faster-whisper ollama
```

### Per l'ESP32 (Arduino IDE 2.x)

Librerie richieste:
- WebSockets by Markus Sattler (2.x)
- Adafruit NeoPixel
- ArduinoJson (7.x)
- Adafruit_BMP280
- MPU6500_WE
- ESP32 core (con driver/i2s.h, WiFi.h, ArduinoOTA.h)

## 🚀 Installazione

### 1. Configura le Credenziali WiFi

Copia il file di esempio e inserisci le tue credenziali:

```bash
cp credentials.h.example credentials.h
```

Modifica `credentials.h` con il tuo SSID e password:

```cpp
#define WIFI_SSID "tua_rete"
#define WIFI_PASSWORD "tua_password"
```

### 2. Configura il Server

Modifica le impostazioni del server in `server.py` se necessario:

```python
OLLAMA_MODEL = "gemma4:e4b"
WHISPER_MODEL = "base"
```

Assicurati che Ollama sia in esecuzione:
```bash
ollama serve
```

### 3. Avvia il Server

```bash
./start.sh
# oppure
uvicorn server:app --host 0.0.0.0 --port 8765 --reload
```

### 4. Carica il Firmware sull'ESP32

1. Apri `Monty.ino` con Arduino IDE
2. Seleziona la scheda ESP32-S3
3. Modifica l'IP del server in `Monty.ino`:
   ```cpp
   const char* SERVER_HOST = "192.168.1.8"; // IP del tuo PC
   ```
4. Carica il firmware sulla scheda

## 🎭 Skill Disponibili

Il sistema supporta un'architettura modulare di skill:

- **DateTime**: Ora e data corrente
- **News**: Ultime notizie
- **Weather**: Previsioni meteo
- **Timer**: Gestione timer e sveglie
- **Sensor**: Lettura sensori (temperatura, pressione, movimento)
- **Web Search**: Ricerche sul web
- **YouTube**: Ricerca e riproduzione video

Per aggiungere nuove skill, crea un nuovo file in `skills/` seguendo il template di `base.py`.

## 🔌 Emozioni del Robot

Il robot può esprimere diverse emozioni attraverso il display degli occhi:

- neutral, happy, sad, angry, surprised
- sleepy, thinking, love, wink, skeptical
- excited, confused

## 🌐 Dashboard

Apri `dashboard.html` nel browser per monitorare lo stato del robot e visualizzare i log in tempo reale.

## 🔧 Sviluppo

### Aggiungere una Nuova Skill

1. Crea un nuovo file in `skills/nome_skill.py`
2. Implementa i metodi richiesti ereditando da `BaseSkill`
3. Registra la skill in `skills/__init__.py`

### Compilare il Firmware ESP32

Assicurati di avere installato:
- Arduino IDE 2.3.8 o superiore
- ESP32 Core 3.x
- Tutte le librerie elencate sopra

## 📄 Licenza

Questo progetto è open source. Sentiti libero di modificarlo e distribuirlo.

## 🤝 Contributi

Le contribuzioni sono benvenute! Apri una issue o invia una pull request per migliorare il progetto.

## 📞 Supporto

Per problemi o domande, apri una issue su GitHub.

---

**Nota**: Questo progetto richiede una configurazione locale di Ollama, Faster Whisper e Piper TTS per il funzionamento completo.
