"""
ROBOT SERVER - Fase 1: MIC/STT + LED (CORRETTO)
Stack: FastAPI + WebSockets | FasterWhisper STT | Ollama | Piper TTS

Avvio:
    uvicorn server:app --host 0.0.0.0 --port 8765 --reload
"""


import os
import asyncio
import json
import logging
import subprocess
import unicodedata
import numpy as np
import time
from datetime import datetime
from typing import Optional

import uvicorn
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from faster_whisper import WhisperModel
import ollama

#Import skill manager
from skills import execute_skill, format_skill_result, get_skills_prompt_section

# ─── LOGGING ─────────────────────────────────────────────────────────────────
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    datefmt="%H:%M:%S"
)
log = logging.getLogger("robot")

# ─── CONFIG ──────────────────────────────────────────────────────────────────
SAMPLE_RATE      = 16000
BITS_PER_SAMPLE  = 16
CHANNELS         = 1
WHISPER_MODEL    = "base"
OLLAMA_MODEL     = "gemma4:e4b"
OLLAMA_HOST      = "http://127.0.0.1:11434"
MAX_AUDIO_SEC    = 30


VALID_EMOTIONS = (
    "neutral", "happy", "sad", "angry", "surprised", "sleepy",
    "thinking", "love", "wink", "skeptical", "excited", "confused"
)

# ─── SYSTEM PROMPT (generato dinamicamente) ───────────────────────────────────
_SKILLS_SECTION = get_skills_prompt_section()
_EMOTIONS_STR = ", ".join(VALID_EMOTIONS)

SYSTEM_PROMPT = f"""Sei Monty, un robot. Il tuo padrone si chiama Andrea.

Hardware: 4 LED NeoPixel (0-3), 2 motori DC differenziali, 2 bumper, microfono, speaker, display OLED 128x64, sensore BMP280 (temperatura + pressione barometrica), IMU (accelerometro + giroscopio), sensore ultrasuoni HC-SR04 frontale.

Rispondi SEMPRE e SOLO con JSON valido:
{{"commands":[...],"speech":"<max 2 frasi>","emotion":"<emozione>"}}
Campo opzionale: "display":{{...}}

═══ EMOZIONI ═══
{_EMOTIONS_STR}

═══ COMANDI ═══
LED:
- set_led: {{"r":0-255,"g":0-255,"b":0-255}} oppure {{"led":0-3,"r","g","b"}} per singolo LED
- set_led_off: {{}}

MOTORI (speed:0-255, duration_ms:0-10000, USA SEMPRE duration_ms>0):
- move_forward/move_backward/turn_left/turn_right: {{"speed":150,"duration_ms":2000}}
- stop: {{}}

ULTRASUONI (sensore frontale HC-SR04, anticollisione sempre attiva):
- us_get_distance: {{}} → risponde con distanza attuale in cm
- us_set_mode: {{"mode":"monitor|follow|scan"}} (monitor=anticollisione, follow=segui persona, scan=mappa 360°)
- us_scan: {{}} → avvia scansione 360° per creare mappa dell'ambiente
- us_stop_follow: {{}} → ferma modalità follow-me
- us_follow_config: {{"speed":120}} → configura velocità follow (60-255)
- us_calibrate_yaw: {{}} → resetta orientamento a 0°
NOTA: L'anticollisione è SEMPRE attiva. Se chiedi di andare avanti e c'è un ostacolo, il robot si ferma da solo.

{_SKILLS_SECTION}

DISPLAY (opzionale, torna agli occhi dopo duration_ms):
- display_text: {{"line1":"...","line2":"...","line3":"...","line4":"...","size":1-3,"duration_ms":5000}}
- display_icon: {{"icon_id":0-5,"text":"...","duration_ms":3000}} (0=WiFi,1=Batteria,2=Temp,3=Musica,4=Check,5=Errore)
- display_split: {{"line1":"...","line2":"...","line3":"...","duration_ms":10000}}
- display_progress: {{"percent":0-100,"label":"...","duration_ms":5000}}

═══ ESEMPI ═══
"Ciao!" → {{"commands":[],"speech":"Ciao Andrea!","emotion":"happy"}}
"Vai avanti" → {{"commands":[{{"cmd":"move_forward","params":{{"speed":150,"duration_ms":2000}}}}],"speech":"Vado!","emotion":"excited"}}
"LED rosso" → {{"commands":[{{"cmd":"set_led","params":{{"r":255,"g":0,"b":0}}}}],"speech":"Rosso!","emotion":"happy"}}
"Che ore sono?" → {{"commands":[{{"cmd":"use_skill","params":{{"skill":"get_current_datetime"}}}}],"speech":"Controllo...","emotion":"thinking"}}
"Fermati" → {{"commands":[{{"cmd":"stop","params":{{}}}}],"speech":"Fermo!","emotion":"neutral"}}
"""

# ─── APP ─────────────────────────────────────────────────────────────────────
app = FastAPI(title="Robot Server", version="1.1")
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

# ─── STATO GLOBALE ───────────────────────────────────────────────────────────
class RobotState:
    def __init__(self):
        self.audio_ws: Optional[WebSocket] = None
        self.cmd_ws:   Optional[WebSocket] = None
        self.dashboard_ws: list[WebSocket] = []
        self.audio_buffer: list[bytes] = []
        self.audio_lock = asyncio.Lock() 
        self.is_recording = False
        self.current_state = "idle"
        self.led_color = {"r": 0, "g": 0, "b": 0}
        self.current_emotion = "neutral"          
        self.display_mode = "eyes"         
        self.last_sensor_data: dict = {}   # ultimo report sensori
        self.last_sensor_broadcast: float = 0  # timestamp ultimo broadcast sensori
        # ── Ultrasuoni ──
        self.us_distance: float = -1.0          # ultima distanza filtrata (cm)
        self.us_raw_distance: float = -1.0      # distanza grezza
        self.us_obstacle: bool = False           # ostacolo rilevato
        self.us_mode: str = "monitor"            # monitor | follow | scan
        self.us_scanning: bool = False           # scansione in corso
        self.us_scan_data: list[dict] = []       # dati ultima scansione [{a:angolo, d:distanza}, ...]
        self.us_scan_progress: int = 0           # % completamento scansione
        self.us_yaw: float = 0.0                 # heading corrente dal IMU
        self.last_us_broadcast: float = 0        # timestamp ultimo broadcast US        

    def log_event(self, event_type: str, data: dict):
        """Notifica tutte le dashboard connesse."""
        try:
            msg = {
                "type": event_type,
                "ts":   datetime.now().isoformat(),
                **data
            }
            # Verifica che ci sia un event loop attivo
            try:
                loop = asyncio.get_running_loop()
                if loop.is_running():
                    asyncio.create_task(self._broadcast_dashboard(json.dumps(msg)))
            except RuntimeError:
                # Nessun event loop in esecuzione
                pass
        except Exception as e:
            log.warning("[Event] Broadcast fallito: %s", e)

    async def _broadcast_dashboard(self, msg: str):
        dead = []
        for ws in self.dashboard_ws:
            try:
                await ws.send_text(msg)
            except Exception:
                dead.append(ws)
        for ws in dead:
            if ws in self.dashboard_ws:
                self.dashboard_ws.remove(ws)

robot = RobotState()

# ─── CARICAMENTO MODELLI ─────────────────────────────────────────────────────
log.info("Caricamento Whisper '%s'...", WHISPER_MODEL)
whisper = WhisperModel(WHISPER_MODEL, device="cpu", compute_type="int8")
log.info("Whisper pronto.")

# ─── UTILITÀ AUDIO ───────────────────────────────────────────────────────────
def pcm16_bytes_to_float32(raw: bytes) -> np.ndarray:
    """Converte buffer PCM16 little-endian in float32 normalizzato [-1, 1]."""
    samples = np.frombuffer(raw, dtype=np.int16).astype(np.float32)
    return samples / 32768.0


def transcribe_audio(raw_bytes: bytes) -> str:
    """STT con FasterWhisper."""
    audio_f32 = pcm16_bytes_to_float32(raw_bytes)
    segments, info = whisper.transcribe(
        audio_f32,
        language="it",
        beam_size=5,
        vad_filter=True,
        vad_parameters={"min_silence_duration_ms": 500}
    )
    text = " ".join(seg.text.strip() for seg in segments).strip()
    log.info("[STT] '%s' (lang=%s, prob=%.2f)", text, info.language, info.language_probability)
    return text


async def send_music_to_esp32(pcm_bytes: bytes, title: str):
    """Invia audio musicale all'ESP32, interrompibile."""
    if not robot.cmd_ws:
        log.warning("[Music] Nessun ESP32 connesso")
        return

    music_abort_event.clear()
    log.info("[Music] Invio '%s': %d bytes (%.1fs)",
             title, len(pcm_bytes), len(pcm_bytes) / (22050 * 2))

    # Segnala inizio riproduzione
    await safe_send_cmd(json.dumps({
        "cmd": "music_start",
        "params": {
            "bytes": len(pcm_bytes),
            "title": title,
            "expression": "happy"
        }
    }))

    await set_robot_state("playing_music")
    robot.log_event("music_start", {"title": title, "bytes": len(pcm_bytes)})

    # Invio a chunk con controllo abort
    chunk_size = 1024
    BATCH_SIZE = 4
    BATCH_DELAY = 0.08
    chunks_sent = 0
    aborted = False

    for i in range(0, len(pcm_bytes), chunk_size):
        # Controlla abort
        if music_abort_event.is_set():
            log.info("[Music] Riproduzione interrotta al chunk %d", chunks_sent)
            aborted = True
            break

        chunk = pcm_bytes[i:i + chunk_size]
        try:
            await robot.cmd_ws.send_bytes(chunk)
            chunks_sent += 1
            if chunks_sent % BATCH_SIZE == 0:
                await asyncio.sleep(BATCH_DELAY)
        except Exception as e:
            log.warning("[Music] Invio fallito al chunk %d: %s", chunks_sent, e)
            aborted = True
            break

    # Segnala fine (o interruzione)
    await asyncio.sleep(0.2)
    await safe_send_cmd(json.dumps({
        "cmd": "music_stop",
        "params": {"reason": "aborted" if aborted else "completed"}
    }))

    robot.log_event("music_end", {
        "title": title,
        "chunks_sent": chunks_sent,
        "aborted": aborted
    })

    log.info("[Music] %s: %d chunk inviati",
             "Interrotta" if aborted else "Completata", chunks_sent)

# ─── LLM  ──────────────────────────────────────────────────
async def process_with_llm(text: str) -> dict:
    """Invia testo ad Ollama, riceve JSON comandi."""
    log.info("[LLM] Input: '%s'", text)

    try:
        response = await asyncio.to_thread(
            ollama.chat,
            model=OLLAMA_MODEL,
            messages=[
                {"role": "system",  "content": SYSTEM_PROMPT},
                {"role": "user",    "content": text}
            ],
            options={"temperature": 0.1}
        )
        raw = response["message"]["content"].strip()
        log.info("[LLM] Output raw: %s", raw[:200])

        # Pulisce eventuale markdown ```json ... ```
        if raw.startswith("```"):
            raw = raw.split("```")[1]
            if raw.startswith("json"):
                raw = raw[4:]
            raw = raw.strip()

        result = json.loads(raw)
        return result

    except json.JSONDecodeError as e:
        log.error("[LLM] JSON parse error: %s", e)
        return {"commands": [], "speech": "Ho avuto un problema a capire la risposta."}
    except Exception as e:
        log.error("[LLM] Errore: %s", repr(e))
        return {"commands": [], "speech": "Errore nella comunicazione con il modello."}


# ─── TTS ─────────────────────────────────────────────────────────────────────
async def synthesize_and_send(text: str, emotion: str = "happy"):
    """TTS con Piper — invia audio PCM16 a chunk via WebSocket.
    
    Args:
        text: testo da sintetizzare
        emotion: espressione degli occhi durante il parlato
    """
    if not text or not robot.cmd_ws:
        log.warning("[TTS] Skip: text=%s, cmd_ws=%s", bool(text), bool(robot.cmd_ws))
        return

    log.info("[TTS] Sintesi: '%s'", text)

    def run_piper(t: str) -> bytes:
               
        t = unicodedata.normalize("NFC", t)
        model_path = os.path.join(os.path.dirname(__file__), "it_IT-riccardo-x_low.onnx")
        
        # Forza l'ambiente UTF-8 per il subprocess
        env = os.environ.copy()
        env["LANG"] = "it_IT.UTF-8"
        env["LC_ALL"] = "it_IT.UTF-8"
        env["PYTHONIOENCODING"] = "utf-8"
        
        


        result = subprocess.run(
            ["piper", "--model", model_path, "--output_raw"],
            input=t.encode("utf-8"),
            capture_output=True,
            env=env
        )
        if result.returncode != 0:
            log.error("[TTS] Piper stderr: %s", result.stderr.decode(errors='replace'))
            return b""
        # return result.stdout
        
        
        
        raw_pcm = result.stdout
        if not raw_pcm:
            return b""
        
        # ── Resample da 16000 Hz (Piper x_low) a 22050 Hz (ESP32 I2S) ──
        PIPER_RATE = 16000
        ESP32_RATE = 22050
        
        samples = np.frombuffer(raw_pcm, dtype=np.int16).astype(np.float32)
        original_len = len(samples)
        new_length = int(len(samples) * ESP32_RATE / PIPER_RATE)
        indices = np.linspace(0, len(samples) - 1, new_length)
        resampled = np.interp(indices, np.arange(len(samples)), samples)
        resampled_pcm = resampled.astype(np.int16).tobytes()
        
        log.info("[TTS] Resampled: %d → %d Hz (%d → %d samples, %d → %d bytes)",
                 PIPER_RATE, ESP32_RATE, original_len, new_length,
                 len(raw_pcm), len(resampled_pcm))
        
        return resampled_pcm

    try:
        audio_bytes = await asyncio.to_thread(run_piper, text)
        if not audio_bytes:
            log.error("[TTS] Piper non ha prodotto audio.")
            await safe_send_cmd(json.dumps({"cmd": "tts_end", "params": {}}))
            await set_robot_state("idle") 
            return

        log.info("[TTS] Audio generato: %d byte", len(audio_bytes))

         # Invia segnale tts_start con emozione all'ESP32
        await safe_send_cmd(json.dumps({
            "cmd": "tts_start",
            "params": {
                "bytes": len(audio_bytes),
                "expression": emotion
            }
        }))

        # Invia audio a chunk
        # Invio con pacing per non saturare la coda ESP32
        chunk_size = 1024
        chunks_sent = 0
        # 1024 byte = 512 campioni @ 22050Hz = ~23.2ms di audio
        # Inviamo a gruppi di 8 chunk, poi aspettiamo
        BATCH_SIZE = 4
        BATCH_DELAY = 0.08  # 80ms di audio in 8 chunk
        
        for i in range(0, len(audio_bytes), chunk_size):
            chunk = audio_bytes[i:i + chunk_size]
            try:
                await robot.cmd_ws.send_bytes(chunk)
                chunks_sent += 1
                
                # Pacing: ogni BATCH_SIZE chunk, pausa
                if chunks_sent % BATCH_SIZE == 0:
                    await asyncio.sleep(BATCH_DELAY)
                
            except Exception as e:
                log.warning("[TTS] Invio chunk %d fallito: %s", chunks_sent, e)
                break

        # Attendi un breve delay per assicurarsi che l'ESP32 abbia finito di riprodurre
        await asyncio.sleep(0.3)  # 300ms di buffer dopo la fine dell'audio
        
        # Invia segnale tts_end all'ESP32
        await safe_send_cmd(json.dumps({
            "cmd": "tts_end",
            "params": {}
        }))

        log.info("[TTS] Completato: %d chunk inviati", chunks_sent)

    except FileNotFoundError:
        log.error("[TTS] Piper non trovato! Installa con: pip install piper-tts")
    except Exception as e:
        log.error("[TTS] Errore: %s", repr(e), exc_info=True)
        #Anche in caso di errore, segnala fine TTS per sbloccare ESP32
        try:
            await safe_send_cmd(json.dumps({"cmd": "tts_end", "params": {}}))
        except Exception:
            pass


# ─── INVIO COMANDI SICURO ────────────────────────────────────────────
async def safe_send_cmd(payload: str):
    """Invia un messaggio testuale al WebSocket cmd con gestione errori."""
    if robot.cmd_ws:
        try:
            await robot.cmd_ws.send_text(payload)
            return True
        except Exception as e:
            log.warning("[CMD] Invio fallito: %s", e)
            # robot.cmd_ws = None
    return False
            
def _format_skill_data_for_llm(skill_results: dict) -> str:
    """Usa il formatter di ogni skill"""
    parts = []
    for skill_name, result in skill_results.items():
        parts.append(format_skill_result(skill_name, result))
    return "\n".join(parts)            

# def format_skill_response(speech: str, skill_results: dict) -> str:
    # """
    # Formatta la risposta speech includendo i dati delle skill eseguite.
    # L'LLM genera uno speech template che può contenere placeholder come:
    # - {datetime}, {date}, {time} per get_current_datetime
    # - {temperature}, {weather}, {description} per get_weather
    # - {news} per get_news
    # - {search_results} per web_search
    # """
    # if not skill_results:
        # return speech
    
    # format_data = {}
    
    # for skill_name, result in skill_results.items():
        # if not result.get("success", False):
            # continue
            
        # data = result.get("data", {})
        
        # if skill_name == "get_current_datetime":
            # format_data["datetime"] = data.get("datetime", "")
            # format_data["date"] = data.get("date", "")
            # format_data["time"] = data.get("time", "")
            # format_data["day_of_week"] = data.get("day_of_week", "")
            
        # elif skill_name == "get_weather":
            # format_data["temperature"] = f"{data.get('temperature', 0)}°C"
            # format_data["description"] = data.get("description", "")
            # format_data["weather"] = f"{format_data['description']}, {format_data['temperature']}"
            # format_data["windspeed"] = f"{data.get('windspeed', 0)} km/h"
            
        # elif skill_name == "get_news":
            # news_list = data.get("news", [])
            # if news_list:
                # news_text = "\n".join(
                    # f"  • {n.get('title', '?')}"
                    # for n in news_list
                # )
                # parts.append(f"- Notizie ({data.get('source', 'fonte sconosciuta')}):\n{news_text}")
            # else:
                # parts.append("- Notizie: nessuna trovata")
                
        # elif skill_name == "web_search":
            # results = data.get("results", [])
            # if results:
                # search_text = "\n".join(
                    # f"  • {r.get('title', '?')}"
                    # for r in results
                # )
                # parts.append(f"- Ricerca web (query: {data.get('query', '?')}):\n{search_text}")
            # else:
                # parts.append("- Ricerca web: nessun risultato")
    
    # # Sostituisci i placeholder nello speech
    # try:
        # formatted = speech.format(**format_data)
        # log.info("[SkillResponse] Speech formattato: %s", formatted)
        # return formatted
    # except KeyError as e:
        # log.warning("[SkillResponse] Placeholder mancante: %s, uso speech originale", e)
        # return speech


# ─── PIPELINE PRINCIPALE ─────────────────────────────────────────────────────
async def _core_pipeline(text: str):
    """Logica centralizzata per l'elaborazione del testo: LLM → Comandi/Skill → TTS/Musica."""
    
    # ── LLM (prima richiesta) ───────────────────────────────────────────────
    robot.log_event("llm_start", {"input": text})
    result = await process_with_llm(text)
    robot.log_event("llm_result", {"result": result})

    # ── SEPARAZIONE COMANDI E SKILL ─────────────────────────────────────────
    commands = result.get("commands", [])
    speech = result.get("speech", "")
    emotion  = result.get("emotion", "neutral")
    display_cmd = result.get("display", None)
    
    skill_results = {}
    skill_commands = []
    hardware_commands = []

    for cmd_obj in commands:
        if cmd_obj.get("cmd") == "use_skill":
            skill_commands.append(cmd_obj)
        else:
            hardware_commands.append(cmd_obj)
    
    # =========================================================================
    # Riproduci subito lo speech di attesa se ci sono skill
    # =========================================================================
    if skill_commands and speech:
        log.info("[Core Pipeline] Riproduzione speech di attesa: '%s'", speech)
        await set_robot_state("speaking")
        await synthesize_and_send(speech, emotion)
        await set_robot_state("processing") # Torna in stato processing per l'attesa
        speech = "" # Svuota la variabile per fare spazio alla risposta finale
    # =========================================================================
    
    
    # ── ESECUZIONE SKILL E SECONDO PASSAGGIO LLM ────────────────────────────
    music_pcm = None
    music_title = None
    
    if skill_commands:        
        for cmd_obj in skill_commands:
            skill_name = cmd_obj.get("params", {}).get("skill")
            if skill_name:
                log.info("[Core Pipeline] Esecuzione skill: %s", skill_name)
                skill_result = await execute_command(cmd_obj)
                skill_results[skill_name] = skill_result
        
                # Controlla se la skill ha prodotto audio da riprodurre 
                if (skill_result and 
                    skill_result.get("success") and
                    skill_result.get("data", {}).get("_pcm_data")):
                    music_pcm = skill_result["data"].pop("_pcm_data")
                    music_title = skill_result["data"].get("title", "Musica")

        # ── SECONDO PASSAGGIO LLM con i dati delle skill ──
        if skill_results:
            skill_data_summary = _format_skill_data_for_llm(skill_results)
            log.info("[Core Pipeline] Re-prompt LLM con dati skill: %s", skill_data_summary[:200])

            followup_prompt = (
                f"L'utente ha chiesto: \"{text}\"\n\n"
                f"Ho eseguito le skill e ottenuto questi dati:\n{skill_data_summary}\n\n"
                f"Ora rispondi all'utente usando questi dati reali. "
                f"NON usare use_skill, i dati li hai già. "
                f"Rispondi con il solito formato JSON."
            )

            robot.log_event("llm_followup_start", {"input": followup_prompt})
            result2 = await process_with_llm(followup_prompt)
            robot.log_event("llm_followup_result", {"result": result2})

            # Aggiorna speech, emotion, display dal secondo passaggio
            speech = result2.get("speech", speech)
            emotion = result2.get("emotion", emotion)
            display_cmd = result2.get("display", display_cmd)

            # Aggiungi eventuali comandi hardware dal secondo passaggio
            for cmd_obj in result2.get("commands", []):
                if cmd_obj.get("cmd") != "use_skill":
                    hardware_commands.append(cmd_obj)
    
    # Valida emozione
    if emotion not in VALID_EMOTIONS:
        log.warning("[Core Pipeline] Emozione '%s' non valida, uso 'neutral'", emotion)
        emotion = "neutral"
    

    log.info("[Core Pipeline] emotion=%s, hw_commands=%d, skills=%d, display=%s, speech=%s, music=%s",
             emotion, len(hardware_commands), len(skill_results), bool(display_cmd), bool(speech), bool(music_pcm))
    
    # ── COMANDI HARDWARE + DISPLAY + TTS in parallelo ─────────────────────────
    tasks = []

    if hardware_commands:
        tasks.append(execute_commands_parallel(hardware_commands))
        
    # Comando display opzionale da LLM
    if display_cmd and isinstance(display_cmd, dict):
        display_cmd_name = display_cmd.get("cmd", "")
        display_params = display_cmd.get("params", {})
        if display_cmd_name:
            tasks.append(execute_command({
                "cmd": display_cmd_name,
                "params": display_params
            }))

    # TTS con emozione
    if speech and robot.cmd_ws:
        await set_robot_state("speaking")
        robot.log_event("tts_start", {"text": speech, "emotion": emotion})
        tasks.append(synthesize_and_send(speech, emotion))

    if tasks:
        await asyncio.gather(*tasks)

    if speech:
        robot.log_event("tts_end", {})
        
    # Dopo il TTS (speech), riproduci la musica
    if music_pcm:
        log.info("[Core Pipeline] Avvio riproduzione musica: '%s' (%d bytes)", music_title, len(music_pcm))
        await send_music_to_esp32(music_pcm, music_title)
        
    # Imposta stato idle SOLO dopo che il TTS e l'eventuale musica sono completati
    await set_robot_state("idle")


async def run_pipeline(audio_bytes: bytes):
    """Esegue STT → passa il testo alla pipeline core."""
    if len(audio_bytes) < SAMPLE_RATE * 2 * 0.3:  # meno di 300ms
        log.warning("[Pipeline] Audio troppo corto, skip.")
        await set_robot_state("idle")
        return

    await set_robot_state("processing")
    robot.log_event("pipeline_start", {"audio_bytes": len(audio_bytes)})

    # ── STT ──────────────────────────────────────────────────────────────────
    robot.log_event("stt_start", {})
    transcript = await asyncio.to_thread(transcribe_audio, audio_bytes)
    robot.log_event("stt_result", {"text": transcript})

    if not transcript:
        log.warning("[Pipeline] Trascrizione vuota.")
        await set_robot_state("idle")
        return

    # Chiama la logica centralizzata
    await _core_pipeline(transcript)


async def run_pipeline_from_text(text: str):
    """Pipeline senza STT — usato dalla dashboard."""
    await set_robot_state("processing")
    robot.log_event("text_input", {"text": text})

    # Chiama la logica centralizzata
    await _core_pipeline(text)
   

# ─── SAFE WRAPPERS ───────────────────────────────────────────────────
async def safe_run_pipeline(audio_bytes: bytes):
    """Wrapper con gestione errori — il robot non resta mai bloccato."""
    try:
        await run_pipeline(audio_bytes)
    except Exception as e:
        log.error("[Pipeline] ERRORE: %s", repr(e), exc_info=True)
        await set_robot_state("idle")


async def safe_run_pipeline_from_text(text: str):
    """Wrapper con gestione errori per pipeline da testo."""
    try:
        await run_pipeline_from_text(text)
    except Exception as e:
        log.error("[Pipeline Text] ERRORE: %s", repr(e), exc_info=True)
        await set_robot_state("idle")


# ─── CLASSIFICAZIONE COMANDI ─────────────────────────────────────────────────

COMMAND_CATEGORIES = {
    "motor": {"move_forward", "move_backward", "turn_left", "turn_right", "stop"},
    "led":   {"set_led", "set_led_off"},
    "display": {"display_expression", "display_look", "display_text", 
                "display_progress", "display_icon", "display_split", "display_eyes"},
    "ultrasonic": {"us_get_distance", "us_set_mode", "us_scan", 
               "us_stop_follow", "us_follow_config", "us_get_scan_data",
               "us_calibrate_yaw"},
    "servo": {"set_servo", "servo_sweep"},       # futuro
    "sound": {"play_tone", "play_melody"},        # futuro
}

def classify_command(cmd: str) -> str:
    for category, cmds in COMMAND_CATEGORIES.items():
        if cmd in cmds:
            return category
    return "system"




# ─── KEYWORDS DI STOP MUSICA ─────────────────────────────────────────────────
MUSIC_STOP_KEYWORDS = {
    "stop", "basta", "ferma", "fermati", "smetti",
    "silenzio", "zitto", "spegni", "fine", "basta musica",
    "ferma la musica", "stop musica"
}


async def _check_music_stop_command(audio_bytes: bytes):
    """STT rapido per rilevare comandi di stop durante la musica."""
    try:
        transcript = await asyncio.to_thread(transcribe_audio, audio_bytes)
        transcript_lower = transcript.lower().strip()

        log.info("[MusicStop] Trascritto durante musica: '%s'", transcript)

        # Controlla se contiene una keyword di stop
        is_stop = any(kw in transcript_lower for kw in MUSIC_STOP_KEYWORDS)

        if is_stop:
            log.info("[MusicStop] Comando STOP rilevato!")
            music_abort_event.set()

            # Rispondi brevemente
            await asyncio.sleep(0.5)  # Aspetta che la musica si fermi
            await set_robot_state("speaking")
            await synthesize_and_send("Ok, fermo la musica!", "neutral")
            await set_robot_state("idle")
        else:
            log.debug("[MusicStop] Non è un comando stop, ignoro: '%s'", transcript)

    except Exception as e:
        log.error("[MusicStop] Errore: %s", repr(e))


# ─── ESECUZIONE COMANDI ──────────────────────────────────────────────────────

motor_abort_event = asyncio.Event() # Event per abort motori (bumper)
music_abort_event = asyncio.Event() # Event per abort musica



async def execute_command(cmd_obj: dict):
    """Valida ed invia un comando JSON all'ESP32."""
    cmd    = cmd_obj.get("cmd")
    params = cmd_obj.get("params", {})

    # Skill esterne (non vengono inviate all'ESP32, gestite dal server)
    if cmd == "use_skill":
        skill_name = params.get("skill")
        if not skill_name:
            log.warning("[SKILL] Nome skill mancante")
            return {"success": False, "error": "Skill name missing"}
        
        # Estrai parametri della skill (rimuovendo "skill" dal dict)
        skill_params = {k: v for k, v in params.items() if k != "skill"}
        
        if skill_name == "get_sensor_data":
            skill_params["sensor_data"] = robot.last_sensor_data
            skill_params["ultrasonic_data"] = {
                "distance_cm": robot.us_distance,
                "obstacle": robot.us_obstacle,
                "mode": robot.us_mode,
                "yaw": robot.us_yaw,
            }
        
        # Aggiorna yaw per la dashboard
        # if "yaw" in data:
            # robot.us_yaw = data["yaw"]
        
        log.info("[SKILL] Esecuzione: %s con params: %s", skill_name, skill_params)
        result = await execute_skill(skill_name, **skill_params)
        return result
    
    allowed = {
        "set_led", "set_led_off",
        "move_forward", "move_backward",
        "turn_left", "turn_right", "stop",
        "display_expression", "display_look", "display_text",
        "display_progress", "display_icon", "display_split", "display_eyes",
        "us_get_distance", "us_set_mode", "us_scan",
        "us_stop_follow", "us_follow_config", "us_get_scan_data",
        "us_calibrate_yaw"
    }
    if cmd not in allowed:
        log.warning("[CMD] Comando non permesso: %s", cmd)
        return

    # Validazione parametri LED
    if cmd == "set_led":
        for ch in ("r", "g", "b"):
            v = params.get(ch, 0)
            params[ch] = max(0, min(255, int(v)))
        # Validazione indice LED singolo (opzionale)
        if "led" in params:
            led_idx = int(params["led"])
            if 0 <= led_idx <= 3:
                params["led"] = led_idx
            else:
                log.warning("[CMD] Indice LED %d fuori range, rimosso.", led_idx)
                del params["led"]  # fallback: tutti i LED
        # Aggiorna stato solo se è "tutti i LED"
        if "led" not in params:
            robot.led_color = {"r": params["r"], "g": params["g"], "b": params["b"]}
        
        

    if cmd == "set_led_off":
        robot.led_color = {"r": 0, "g": 0, "b": 0}

    # Validazione parametri motori
    if cmd in ("move_forward", "move_backward", "turn_left", "turn_right"):
        speed = params.get("speed", 150)
        params["speed"] = max(0, min(255, int(speed)))
        duration = params.get("duration_ms", 2000)
        params["duration_ms"] = max(0, min(10000, int(duration)))
        
              
    # Validazione parametri display
    if cmd == "display_text":
        for key in ("line1", "line2", "line3", "line4"):
            if key in params:
                params[key] = str(params[key])[:31]  # max 31 char (buffer ESP32)
        params["size"] = max(1, min(3, int(params.get("size", 1))))
        params["duration_ms"] = max(0, min(60000, int(params.get("duration_ms", 5000))))

    if cmd == "display_progress":
        params["percent"] = max(0, min(100, int(params.get("percent", 0))))
        if "label" in params:
            params["label"] = str(params["label"])[:23]
        params["duration_ms"] = max(0, min(60000, int(params.get("duration_ms", 0))))

    if cmd == "display_icon":
        params["icon_id"] = max(0, min(5, int(params.get("icon_id", 0))))
        if "text" in params:
            params["text"] = str(params["text"])[:31]
        params["duration_ms"] = max(0, min(60000, int(params.get("duration_ms", 3000))))

    if cmd == "display_split":
        for key in ("line1", "line2", "line3"):
            if key in params:
                params[key] = str(params[key])[:31]
        params["duration_ms"] = max(0, min(60000, int(params.get("duration_ms", 10000))))

    if cmd == "display_expression":
        exp = params.get("expression", "neutral")
        if exp not in VALID_EMOTIONS:
            log.warning("[CMD] Espressione '%s' non valida, uso 'neutral'", exp)
            params["expression"] = "neutral"

    if cmd == "display_look":
        valid_dirs = {"center", "left", "right", "up", "down"}
        d = params.get("direction", "center")
        if d not in valid_dirs:
            params["direction"] = "center"
    
    # Validazione parametri ultrasuoni
    if cmd == "us_set_mode":
        mode = params.get("mode", "monitor")
        if mode not in ("monitor", "follow", "scan"):
            log.warning("[CMD] Modalità US '%s' non valida, uso 'monitor'", mode)
            params["mode"] = "monitor"
        if mode == "follow":
            params["speed"] = max(60, min(255, int(params.get("speed", 120))))

    if cmd == "us_follow_config":
        params["speed"] = max(60, min(255, int(params.get("speed", 120))))
        if "target_cm" in params:
            params["target_cm"] = max(30, min(300, int(params["target_cm"])))
            


    payload = json.dumps({"cmd": cmd, "params": params})
    log.info("[CMD] → ESP32: %s", payload)

    await safe_send_cmd(payload)
    robot.log_event("command_sent", {"cmd": cmd, "params": params})
    
    
    
async def execute_commands_parallel(commands: list[dict]):
    """
    Raggruppa i comandi per categoria e li esegue:
      - Categorie diverse → in PARALLELO
      - Comandi nella stessa categoria → in SEQUENZA (rispettando duration_ms)
    
    Esempio LLM output:
      move_forward 500ms, turn_right 300ms, set_led rosso
    Risultato:
      - MOTOR: forward(500ms) → wait → right(300ms) → wait → stop
      - LED:   set_led rosso (istantaneo)
      - Entrambi partono INSIEME
    """
    if not commands:
        return

    # Raggruppa per categoria mantenendo l'ordine interno
    groups: dict[str, list[dict]] = {}
    for cmd_obj in commands:
        cat = classify_command(cmd_obj.get("cmd", ""))
        groups.setdefault(cat, []).append(cmd_obj)

    cats = ", ".join(f"{k}({len(v)})" for k, v in groups.items())
    log.info("[SEQ] %d comandi → categorie: %s", len(commands), cats)

    # Lancia ogni categoria in parallelo
    tasks = []

    if "motor" in groups:
        motor_abort_event.clear()
        tasks.append(run_motor_sequence(groups["motor"]))

    if "led" in groups:
        tasks.append(run_led_sequence(groups["led"]))
        
    if "display" in groups:
        tasks.append(run_display_sequence(groups["display"]))
        
    if "ultrasonic" in groups:
        tasks.append(run_immediate_commands(groups["ultrasonic"]))

    if "system" in groups:
        tasks.append(run_immediate_commands(groups["system"]))

    await asyncio.gather(*tasks)
    log.info("[SEQ] Tutte le categorie completate.")


async def run_motor_sequence(commands: list[dict]):
    """
    Esegue comandi motore in sequenza, aspettando duration_ms tra uno e l'altro.
    Interrompibile da motor_abort_event (bumper).
    """
    for i, cmd_obj in enumerate(commands):
        # Check abort
        if motor_abort_event.is_set():
            log.warning("[MOTOR] Sequenza abortita al passo %d/%d", i + 1, len(commands))
            await execute_command({"cmd": "stop", "params": {}})
            return

        cmd = cmd_obj.get("cmd", "")
        params = cmd_obj.get("params", {})
        duration_ms = params.get("duration_ms", 0)

        # Invia il comando
        await execute_command(cmd_obj)
        log.info("[MOTOR] %d/%d: %s (dur=%dms)", i + 1, len(commands), cmd, duration_ms)

        # Aspetta la durata, interrompibile da abort
        if duration_ms > 0 and cmd != "stop":
            aborted = await wait_or_abort(duration_ms)
            if aborted:
                log.warning("[MOTOR] Abort durante %s", cmd)
                await execute_command({"cmd": "stop", "params": {}})
                return

    # Fine sequenza: stop per sicurezza
    await execute_command({"cmd": "stop", "params": {}})
    log.info("[MOTOR] Sequenza completata.")


async def run_led_sequence(commands: list[dict]):
    """Esegue comandi LED in sequenza (di solito istantanei)."""
    for i, cmd_obj in enumerate(commands):
        await execute_command(cmd_obj)
        duration_ms = cmd_obj.get("params", {}).get("duration_ms", 0)
        if duration_ms > 0:
            await asyncio.sleep(duration_ms / 1000.0)
        else:
            await asyncio.sleep(0.02)


async def run_display_sequence(commands: list[dict]):
    """Esegue comandi display in sequenza con piccolo delay tra uno e l'altro."""
    for i, cmd_obj in enumerate(commands):
        await execute_command(cmd_obj)
        # Piccolo delay per dare tempo all'ESP32 di processare
        await asyncio.sleep(0.05)
    log.info("[DISPLAY] Sequenza %d comandi completata.", len(commands))


async def run_immediate_commands(commands: list[dict]):
    """Esegue comandi system immediatamente."""
    for cmd_obj in commands:
        await execute_command(cmd_obj)


async def wait_or_abort(duration_ms: int) -> bool:
    """
    Aspetta duration_ms millisecondi.
    Ritorna True se abortito (bumper), False se durata completata.
    """
    try:
        await asyncio.wait_for(
            motor_abort_event.wait(),
            timeout=duration_ms / 1000.0
        )
        return True   # abort triggerato
    except asyncio.TimeoutError:
        return False  # durata completata normalmente


# ─── GESTIONE STATO ──────────────────────────────────────────────────────────
async def set_robot_state(state: str):
    """Aggiorna stato e notifica ESP32 + dashboard."""
    robot.current_state = state

    # Notifica ESP32 solo per stati rilevanti
    if state in ("processing", "idle", "playing_music"):
        msg = json.dumps({"cmd": "state_update", "params": {"state": state}})
        for attempt in range(3):
            if await safe_send_cmd(msg):
                break
            await asyncio.sleep(0.1 * (attempt + 1))

    # Aggiorna espressione display in base allo stato
    if state == "processing":
        robot.current_emotion = "thinking"
        await safe_send_cmd(json.dumps({
            "cmd": "display_expression",
            "params": {"expression": "thinking"}
        }))
    elif state == "playing_music":
        robot.current_emotion = "happy"
        await safe_send_cmd(json.dumps({
            "cmd": "display_expression",
            "params": {"expression": "happy"}
        }))
    elif state == "idle":
        robot.current_emotion = "neutral"
        # Non forzare display_eyes qui — potrebbe esserci un display_text attivo con timeout

    robot.log_event("state_change", {"state": state})
    
    
    # ─── WEBSOCKET: AUDIO (ESP32 → Server) ───────────────────────────────────────
@app.websocket("/audio")
async def ws_audio(ws: WebSocket):
    await ws.accept()
    robot.audio_ws = ws
    robot.is_recording = False
    log.info("[WS Audio] ESP32 connesso.")
    robot.log_event("esp32_audio_connected", {})

    #Pulisci buffer con lock
    async with robot.audio_lock:
        robot.audio_buffer = []

    try:
        while True:
            msg = await ws.receive()

            # Controlla messaggio di disconnessione
            if msg.get("type") == "websocket.disconnect":
                break

            if "bytes" in msg:
                # Ignora audio se il robot sta parlando (evita eco/feedback)
                #if robot.current_state == "speaking":
                if robot.current_state in ["speaking", "processing"]:
                    log.debug("[WS Audio] Skip audio chunk: robot is speaking or processing")
                    continue
                    
                
                # Durante musica: accumula per rilevare "stop"
                if robot.current_state == "playing_music":
                    async with robot.audio_lock:
                        robot.audio_buffer.append(msg["bytes"])
                    continue
                
                
                # Stato normale: accumula
                chunk = msg["bytes"]
                robot.is_recording = True
                async with robot.audio_lock:
                    robot.audio_buffer.append(chunk)

            elif "text" in msg:
                try:
                    data = json.loads(msg["text"])
                except json.JSONDecodeError:
                    log.warning("[WS Audio] JSON invalido ricevuto.")
                    continue

                if data.get("type") == "end_of_speech":
                    # Ignora end_of_speech se il robot sta parlando
                    if robot.current_state == "speaking":
                        log.debug("[WS Audio] Skip end_of_speech: robot is speaking")
                        continue
                        
                    log.info("[WS Audio] end_of_speech ricevuto. Buffer: %d byte, recording: %s",
                        sum(len(c) for c in robot.audio_buffer), robot.is_recording)
                        
                    # Durante musica: controlla se è un comando di stop
                    if robot.current_state == "playing_music":
                        async with robot.audio_lock:
                            if robot.audio_buffer:
                                audio_data = b"".join(robot.audio_buffer)
                                robot.audio_buffer = []
                            else:
                                audio_data = None

                        if audio_data and len(audio_data) > SAMPLE_RATE * 2 * 0.3:
                            # STT rapido per vedere se è un comando stop
                            asyncio.create_task(
                                _check_music_stop_command(audio_data)
                            )
                        continue
                        
                    # Stato normale: pipeline completa
                    async with robot.audio_lock:
                        if robot.audio_buffer:
                            full_audio = b"".join(robot.audio_buffer)
                            robot.audio_buffer = []
                            robot.is_recording = False
                        else:
                            full_audio = None

                    if full_audio:
                        asyncio.create_task(safe_run_pipeline(full_audio))
                    else:
                        log.warning("[WS Audio] ⚠️ Buffer vuoto su end_of_speech — forzo idle")
                        await set_robot_state("idle")

    except WebSocketDisconnect:
        log.info("[WS Audio] ESP32 disconnesso (WebSocketDisconnect).")
    except RuntimeError as e:
        # FastAPI/Starlette a volte lancia RuntimeError invece di WebSocketDisconnect
        log.warning("[WS Audio] ESP32 disconnesso (RuntimeError): %s", e)
    except Exception as e:
        log.error("[WS Audio] Errore inatteso: %s", repr(e))
    finally:
        # Pulizia garantita
        robot.audio_ws = None
        async with robot.audio_lock:
            robot.audio_buffer = []
        robot.is_recording = False
        robot.log_event("esp32_audio_disconnected", {})


# ─── WEBSOCKET: COMANDI (Server ↔ ESP32) ─────────────────────────────────────
@app.websocket("/cmd")
async def ws_cmd(ws: WebSocket):
    await ws.accept()
    robot.cmd_ws = ws
    log.info("[WS Cmd] ESP32 connesso.")
    robot.log_event("esp32_cmd_connected", {})

    try:
        while True:
            msg = await ws.receive()

            #Controlla messaggio di disconnessione
            if msg.get("type") == "websocket.disconnect":
                break

            if "text" in msg:
                try:
                    data = json.loads(msg["text"])
                except json.JSONDecodeError:
                    log.warning("[WS Cmd] JSON invalido ricevuto.")
                    continue
                # Hello
                if data.get("type") == "hello":
                    log.info("[WS Cmd] Hello da ESP32: %s", data)
                    
                # ACK comandi
                elif data.get("ack"):
                    ack_type = data["ack"]
                    log.info("[WS Cmd] ACK: %s", data)
                    robot.log_event("esp32_ack", data)
                    
                    # ── ACK ultrasuoni che richiedono risposta vocale ──
                    if ack_type == "us_get_distance":
                        dist = data.get("distance_cm", -1)
                        obstacle = data.get("obstacle", False)
                        
                        if dist < 0:
                            response_text = "Non rilevo nessun ostacolo davanti a me, la via è libera."
                        elif obstacle:
                            response_text = f"Attenzione! C'è un ostacolo molto vicino, a soli {dist:.0f} centimetri."
                        elif dist < 50:
                            response_text = f"C'è qualcosa a {dist:.0f} centimetri davanti a me, abbastanza vicino."
                        elif dist < 150:
                            response_text = f"Rilevo un ostacolo a circa {dist:.0f} centimetri."
                        else:
                            response_text = f"L'ostacolo più vicino è a {dist:.0f} centimetri, abbastanza lontano."
                        
                        # Rispondi vocalmente
                        if robot.current_state in ("processing", "idle"):
                            await set_robot_state("speaking")
                            await synthesize_and_send(response_text, "neutral")
                            await set_robot_state("idle")
                    
                 # Eventi bumper
                elif data.get("event") == "bumper_hit":
                    side = data.get("side", "unknown")
                    log.warning("[BUMPER] Collisione lato: %s", side)
                    robot.log_event("bumper_hit", {"side": side})
                    
                    motor_abort_event.set()
                    
                    # Lancia pipeline LLM con contesto bumper — non bloccare il loop WS
                    
                    # Se sta suonando musica, fermala anche
                    if robot.current_state == "playing_music":
                        music_abort_event.set()
                    else:
                        asyncio.create_task(
                            safe_run_pipeline_from_text(
                            f"[EVENTO AUTOMATICO] Monty ha appena colpito un ostacolo con il bumper {side}. "
                            f"Ha già fatto retromarcia e si è allontanato. "
                            f"Commenta brevemente la situazione in modo spontaneo e un po' ironico."
                            )
                        )


                # Eventi motore
                elif data.get("event") == "motor_timeout":
                    log.info("[MOT] Timeout motore raggiunto.")
                    robot.log_event("motor_timeout", {})
                    
                    
                    
                # ── Eventi sensori IMU ────────────────────────────────────────
                elif data.get("event") == "sensor_data":
                    # Dati periodici — broadcast alla dashboard, no reazione LLM
                    robot.last_sensor_data = data  # salva per consultazione
                    now = time.time()
                    if now - robot.last_sensor_broadcast >= 0.5:
                        robot.last_sensor_broadcast = now
                        robot.log_event("sensor_data", {
                            "temp": data.get("temp"),
                            "press": data.get("press"),
                            "accel": data.get("accel"),
                            "gyro": data.get("gyro"),
                            "tilt": data.get("tilt"),
                            "state": data.get("state"),
                        })

                elif data.get("event") == "tap_detected":
                    intensity = data.get("intensity", 0)
                    log.warning("[SENSOR] 🤜 TAP rilevato! Intensità: %.2fg", intensity)
                    robot.log_event("tap_detected", {"intensity": intensity})

                    # Reazione LLM solo se il robot è IDLE (non interrompere conversazioni)
                    if robot.current_state == "idle":
                        # Intensità diversa → reazione diversa
                        if intensity > 1:
                            prompt = (
                                f"[EVENTO FISICO] Ti hanno dato un colpo molto forte (intensità: {intensity:.1f}g)! "
                                f"Reagisci in modo arrabbiatissimo."
                            )
                        elif intensity > 0.5:
                            prompt = (
                                f"[EVENTO FISICO] Ti hanno dato un colpetto medio (intensità: {intensity:.1f}g). "
                                f"Reagisci in modo un po' infastidito. "
                            )
                        else:
                            prompt = (
                                f"[EVENTO FISICO] TI hanno toccato leggermente (intensità: {intensity:.1f}g). "
                                f"Reagisci in modo curioso o divertito."
                            )
                        asyncio.create_task(safe_run_pipeline_from_text(prompt))

                elif data.get("event") == "tilt_alert":
                    angle_x = data.get("angle_x", 0)
                    angle_y = data.get("angle_y", 0)
                    log.warning("[SENSOR] ⚠️ TILT! X=%.1f° Y=%.1f°", angle_x, angle_y)
                    robot.log_event("tilt_alert", {"angle_x": angle_x, "angle_y": angle_y})

                    if robot.current_state == "idle":
                        asyncio.create_task(safe_run_pipeline_from_text(
                            f"[EVENTO FISICO] Ti stai inclinando troppo! Angolo X={angle_x:.0f}°, Y={angle_y:.0f}°. "
                            f"Sei preoccupato di cadere. Reagisci con panico/preoccupazione. Max 1 frase breve."
                        ))

                elif data.get("event") == "tilt_recovered":
                    log.info("[SENSOR] ✓ Inclinazione recuperata")
                    robot.log_event("tilt_recovered", {})

                    if robot.current_state == "idle":
                        asyncio.create_task(safe_run_pipeline_from_text(
                            "[EVENTO FISICO] Eri inclinato ma ora sei tornato dritto. "
                            "Esprimi sollievo in modo buffo. Max 1 frase brevissima."
                        ))

                elif data.get("event") == "freefall_detected":
                    log.critical("[SENSOR] 🆘 CADUTA LIBERA!")
                    robot.log_event("freefall_detected", {})

                    if robot.current_state in ("idle", "speaking"):
                        asyncio.create_task(safe_run_pipeline_from_text(
                            "[EVENTO CRITICO] Stai cadendo! Caduta libera rilevata! "
                            "Urla brevemente di paura. Max 3-4 parole."
                        ))
                # ── Eventi ultrasuoni ─────────────────────────────────────────
                elif data.get("event") == "us_data":
                    # Report periodico distanza (ogni 500ms dall'ESP32)
                    robot.us_distance = data.get("distance_cm", -1.0)
                    robot.us_raw_distance = data.get("raw_cm", -1.0)
                    robot.us_obstacle = data.get("obstacle", False)
                    robot.us_mode = data.get("mode", "monitor")
                    robot.us_scanning = data.get("scanning", False)
                    
                    # Broadcast alla dashboard (throttled a 500ms)
                    now = time.time()
                    if now - robot.last_us_broadcast >= 0.5:
                        robot.last_us_broadcast = now
                        robot.log_event("us_data", {
                            "distance_cm": robot.us_distance,
                            "raw_cm": robot.us_raw_distance,
                            "obstacle": robot.us_obstacle,
                            "mode": robot.us_mode,
                            "scanning": robot.us_scanning,
                        })

                elif data.get("event") == "us_anticollision":
                    dist = data.get("distance_cm", 0)
                    log.warning("[US] ⚠️ Anticollisione! Distanza: %.1f cm", dist)
                    robot.log_event("us_anticollision", {"distance_cm": dist})
                    
                    # Abort sequenza motori in corso
                    motor_abort_event.set()
                    
                    # Reazione LLM (solo se idle, per non interrompere conversazioni)
                    if robot.current_state == "idle":
                        asyncio.create_task(safe_run_pipeline_from_text(
                            f"[EVENTO AUTOMATICO] Il sensore ultrasuoni ha rilevato un ostacolo a {dist:.0f} cm "
                            f"e ti sei fermato automaticamente. Commenta brevemente."
                        ))

                elif data.get("event") == "move_blocked":
                    dist = data.get("distance_cm", 0)
                    reason = data.get("reason", "unknown")
                    log.warning("[US] Movimento bloccato: %s a %.1f cm", reason, dist)
                    robot.log_event("move_blocked", {"reason": reason, "distance_cm": dist})

                elif data.get("event") == "follow_target_lost":
                    log.info("[US] Follow-me: target perso")
                    robot.log_event("follow_target_lost", {})
                    
                    if robot.current_state == "idle":
                        asyncio.create_task(safe_run_pipeline_from_text(
                            "[EVENTO AUTOMATICO] Stavi seguendo qualcuno ma l'hai perso di vista. "
                            "Commenta brevemente con tono dispiaciuto."
                        ))

                elif data.get("event") == "scan_started":
                    log.info("[US] Scansione 360° avviata")
                    robot.us_scanning = True
                    robot.us_scan_data = []
                    robot.us_scan_progress = 0
                    robot.log_event("scan_started", {})

                elif data.get("event") == "scan_point":
                    # Punto singolo della scansione (aggiornamento live)
                    step = data.get("step", 0)
                    angle = data.get("angle", 0)
                    distance = data.get("distance", -1)
                    
                    robot.us_scan_data.append({"a": angle, "d": distance})
                    robot.us_scan_progress = int((step + 1) * 100 / 36)
                    
                    # Broadcast live alla dashboard per rendering in tempo reale
                    robot.log_event("scan_point", {
                        "step": step,
                        "angle": angle,
                        "distance": distance,
                        "progress": robot.us_scan_progress,
                    })

                elif data.get("event") == "scan_complete":
                    points = data.get("points", 0)
                    scan_data = data.get("data", [])
                    log.info("[US] Scansione completata: %d punti", points)
                    
                    robot.us_scanning = False
                    robot.us_scan_data = scan_data
                    robot.us_scan_progress = 100
                    
                    robot.log_event("scan_complete", {
                        "points": points,
                        "data": scan_data,
                    })

    except WebSocketDisconnect:
        log.info("[WS Cmd] ESP32 disconnesso (WebSocketDisconnect).")
    except RuntimeError as e:
        log.warning("[WS Cmd] ESP32 disconnesso (RuntimeError): %s", e)
    except Exception as e:
        log.error("[WS Cmd] Errore inatteso: %s", repr(e))
    finally:
        robot.cmd_ws = None
        robot.log_event("esp32_cmd_disconnected", {})


# ─── WEBSOCKET: DASHBOARD ────────────────────────────────────────────────────
@app.websocket("/dashboard")
async def ws_dashboard(ws: WebSocket):
    await ws.accept()
    robot.dashboard_ws.append(ws)
    log.info("[Dashboard] Connessa.")

    # Invia stato iniziale
    try:
        await ws.send_text(json.dumps({
            "type": "init",
            "state": robot.current_state,
            "led_color": robot.led_color,
            "esp32_audio": robot.audio_ws is not None,
            "esp32_cmd":   robot.cmd_ws is not None,
             "ultrasonic": {
                "distance_cm": robot.us_distance,
                "obstacle": robot.us_obstacle,
                "mode": robot.us_mode,
                "scanning": robot.us_scanning,
                "scan_progress": robot.us_scan_progress,
                "scan_data": robot.us_scan_data,
                "yaw": robot.us_yaw,
            },
        }))
    except Exception:
        pass

    try:
        while True:
            msg = await ws.receive()

            #Controlla disconnessione
            if msg.get("type") == "websocket.disconnect":
                break

            if "text" in msg:
                try:
                    data = json.loads(msg["text"])
                except json.JSONDecodeError:
                    continue

                # Dashboard può inviare comandi manuali
                if data.get("type") == "manual_command":
                    cmd_obj = data.get("cmd_obj", {})                 
                    await execute_command(cmd_obj)

                # Dashboard può inviare testo direttamente (bypass STT)
                elif data.get("type") == "text_input":
                    text = data.get("text", "")
                    if text:
                        asyncio.create_task(safe_run_pipeline_from_text(text))
                
                # ── Comandi ultrasuoni dalla dashboard ────────────────────────
                elif data.get("type") == "us_command":
                    us_cmd = data.get("command", "")
                    us_params = data.get("params", {})
                    
                    if us_cmd == "get_distance":
                        await execute_command({"cmd": "us_get_distance", "params": {}})
                    
                    elif us_cmd == "set_mode":
                        mode = us_params.get("mode", "monitor")
                        speed = us_params.get("speed", 120)
                        await execute_command({"cmd": "us_set_mode", "params": {"mode": mode, "speed": speed}})
                    
                    elif us_cmd == "scan":
                        await execute_command({"cmd": "us_scan", "params": {}})
                    
                    elif us_cmd == "stop_follow":
                        await execute_command({"cmd": "us_stop_follow", "params": {}})
                    
                    elif us_cmd == "calibrate_yaw":
                        await execute_command({"cmd": "us_calibrate_yaw", "params": {}})
                    
                    elif us_cmd == "follow_config":
                        speed = us_params.get("speed", 120)
                        await execute_command({"cmd": "us_follow_config", "params": {"speed": speed}})

                # ── Richiesta dati scansione dalla dashboard ──────────────────
                elif data.get("type") == "us_get_scan_data":
                    # Invia i dati dell'ultima scansione direttamente dalla cache server
                    await ws.send_text(json.dumps({
                        "type": "scan_data",
                        "data": robot.us_scan_data,
                        "points": len(robot.us_scan_data),
                        "complete": not robot.us_scanning,
                        "progress": robot.us_scan_progress,
                    }))

    except WebSocketDisconnect:
        pass
    except RuntimeError:
        pass
    except Exception as e:
        log.error("[Dashboard] Errore: %s", repr(e))
    finally:
        if ws in robot.dashboard_ws:
            robot.dashboard_ws.remove(ws)
        log.info("[Dashboard] Disconnessa.")


# ─── HTTP: STATUS ─────────────────────────────────────────────────────────────
@app.get("/status")
async def status():
    return {
        "state":         robot.current_state,
        "led_color":     robot.led_color,
        "emotion":       robot.current_emotion,
        "display_mode":  robot.display_mode,
        "esp32_audio":   robot.audio_ws is not None,
        "esp32_cmd":     robot.cmd_ws is not None,
        "dashboard_cnt": len(robot.dashboard_ws),
        "ultrasonic": {
            "distance_cm":  robot.us_distance,
            "obstacle":     robot.us_obstacle,
            "mode":         robot.us_mode,
            "scanning":     robot.us_scanning,
            "scan_progress": robot.us_scan_progress,
            "yaw":          robot.us_yaw,
        },
    }


# ─── ENTRY POINT ─────────────────────────────────────────────────────────────
if __name__ == "__main__":
    uvicorn.run(
        "server:app",
        host="0.0.0.0",
        port=8765,
        reload=True,
        log_level="info"
    )
