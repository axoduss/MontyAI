# Monty - ESP32-S3 Robot with Voice Assistant

A conversational robot based on ESP32-S3 with voice control, animated eye display, and local AI integration.

## 🎯 Features

- **Voice Control**: Speech-to-text via Faster Whisper
- **Local AI**: Integration with Ollama (Gemma4)
- **Speech Synthesis**: TTS with Piper
- **Eye Display**: OLED animations to express emotions
- **Skill System**: Modular architecture to extend functionalities
- **Bidirectional Audio**: I2S Microphone (INMP441) + I2S Speaker (MAX98357A)
- **Sensors**: BMP280 (pressure/temperature) + MPU6500 (accelerometer/gyroscope) + HC-SR04 (ultrasound) + bumper switch
- **OTA Updates**: Over-the-air updates for the ESP32 firmware

## 📁 Project Structure

```
├── Monty.ino              # Arduino Firmware for ESP32-S3
├── server.py              # Python Server (FastAPI + WebSocket)
├── dashboard.html         # Web interface for monitoring
├── display_eyes.h         # Eye animations library
├── credentials.h.example  # Template for WiFi credentials
├── skills/                # Skill modules (weather, news, timer, etc.)
├── start.sh               # Server startup script
└── README.md              # This documentation
```

## 🛠️ Hardware Requirements

- **Microcontroller**: ESP32-S3 N16R8
- **Microphone**: INMP441 (I2S)
- **Speaker**: MAX98357A (I2S)
- **LED**: NeoPixel WS2812
- **Display**: SS1306
- **Sensors**: BMP280, MPU6500, HC-SR04 (I2C)
- **Motors**: 2x DRV8871(PWM)
- **Bumper**: 2x Microswitch ON/OFF

## 💻 Software Requirements

### For the Server (Python 3.8+)

```bash
pip install fastapi uvicorn websockets numpy faster-whisper ollama
```

### For the ESP32 (Arduino IDE 2.x)

Required libraries:
- WebSockets by Markus Sattler (2.x)
- Adafruit NeoPixel
- ArduinoJson (7.x)
- Adafruit_BMP280
- MPU6500_WE
- ESP32 core (con driver/i2s.h, WiFi.h, ArduinoOTA.h)

## 🚀 Installation

### 1. Configure WiFi Credentials

Copy the example file and enter your credentials:

```bash
cp credentials.h.example credentials.h
```

Edit credentials.h with your SSID and password:

```cpp
#define WIFI_SSID "your_network"
#define WIFI_PASSWORD "your_password"
```

### 2. Configure the Server

Modify the server settings in server.py if necessary:

```python
OLLAMA_MODEL = "gemma4:e4b"
WHISPER_MODEL = "base"
```

Make sure Ollama is running:
```bash
ollama serve
```

### 3. Start the Server

```bash
./start.sh
# oppure
uvicorn server:app --host 0.0.0.0 --port 8765 --reload
```

### 4. Upload Firmware to ESP32

1. Open `Monty.ino` with Arduino IDE
2. Select the ESP32-S3 board
3. Change the server IP in `Monty.ino`:
   ```cpp
   const char* SERVER_HOST = "192.168.1.8"; // IP del tuo PC
   ```
4. Upload the firmware to the board

## 🎭 Available Skills

The system supports a modular skill architecture:

- **DateTime**: Current time and date
- **News**: Latest news
- **Weather**: Weather forecast
- **Timer**: Timer and alarm management
- **Sensor**: Sensor readings (temperature, pressure, motion)
- **Web Search**: Web searches
- **YouTube**: Video search and playback

To add new skills, create a new file in  `skills/` following the `base.py` template.

## 🔌 Robot Emotions

The robot can express different emotions through the eye display:

- neutral, happy, sad, angry, surprised
- sleepy, thinking, love, wink, skeptical
- excited, confused

## 🌐 Dashboard

Open `dashboard.html` in your browser to monitor the robot's status and view real-time logs.

## 🔧 Development

### Adding a New Skill

1. Create a new file in `skills/nome_skill.py`
2. Implement the required methods by inheriting from `BaseSkill`
3. Register the skill in `skills/__init__.py`

### Compiling the ESP32 Firmware

Make sure you have installed:
- Arduino IDE 2.3.8 or higher
- ESP32 Core 3.x
- All the libraries listed above

## 📄 License

This project is open source. Feel free to modify and distribute it.

## 🤝 Contributions

Contributions are welcome! Open an issue or submit a pull request to improve the project.

## 📞 Support

For issues or questions, please open an issue on GitHub.

---

**Note**: This project requires a local setup of Ollama, Faster Whisper, and Piper TTS for full functionality.
