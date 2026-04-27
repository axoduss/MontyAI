"""
ROBOT SERVER - Fase 1: MIC/STT + LED (CORRETTO)
Stack: FastAPI + WebSockets | FasterWhisper STT | Ollama (Gemma3) | Piper TTS

Fix applicati:
  #1 - asyncio.Lock sul buffer audio
  #2 - Safe pipeline wrapper con error handling
  #3 - Segnali tts_start / tts_end verso ESP32
  #4 - Gestione RuntimeError nei WebSocket handler
  #5 - log_event robusto
  #6 - Invio comandi con check connessione

Avvio:
    uvicorn server:app --host 0.0.0.0 --port 8765 --reload
"""


import os
import asyncio
import json
import logging
import subprocess
import numpy as np
from datetime import datetime
from typing import Optional

import uvicorn
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from faster_whisper import WhisperModel
import ollama

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
OLLAMA_MODEL     = "gemma3:12b"
OLLAMA_HOST      = "http://127.0.0.1:11434"
MAX_AUDIO_SEC    = 30

# ─── SYSTEM PROMPT ───────────────────────────────────────────────────────────
SYSTEM_PROMPT = """Ti chiami Monty, sei un robot mobile con:
- 4 LED NeoPixel RGB (striscia WS2812, indici 0-3)
- 2 motori DC con driver DRV8871 (differenziale, 2 ruote)
- 2 bumper (microswitch finecorsa, sinistro e destro)
- Microfono e speaker
- Display OLED 128x64 pixel (SSD1306) che mostra i tuoi occhi espressivi
Io mi chiamo Andrea.

Rispondi SEMPRE e SOLO con un JSON valido, nessun testo fuori dal JSON.

Formato risposta:
{
  "commands": [
    {
      "cmd": "<nome_comando>",
      "params": { ... }
    }
  ],
  "speech": "<frase da dire ad alta voce, max 2 frasi>",
  "emotion": "<espressione degli occhi durante il parlato>",
  "display": {
    "cmd": "<comando display opzionale>",
    "params": { ... }
  }
}

CAMPI OBBLIGATORI: "commands", "speech", "emotion"
CAMPO OPZIONALE: "display" (solo se vuoi mostrare qualcosa di specifico sul display)


═══════════════════════════════════════════════════════════════
EMOZIONI DISPONIBILI (campo "emotion"):
═══════════════════════════════════════════════════════════════
- "neutral"    → occhi normali, rilassato
- "happy"      → occhi sorridenti, contento
- "sad"        → occhi tristi, dispiaciuto
- "angry"      → occhi arrabbiati, infastidito
- "surprised"  → occhi spalancati, stupito
- "sleepy"     → occhi semichiusi, stanco/annoiato
- "thinking"   → sguardo in alto, pensieroso
- "love"       → occhi a cuore, affettuoso
- "wink"       → occhiolino, complice/scherzoso
- "skeptical"  → sopracciglio alzato, dubbioso
- "excited"    → occhi grandi con stelline, entusiasta
- "confused"   → occhi asimmetrici, confuso

Scegli l'emozione che meglio si adatta al TONO della tua risposta.

═══════════════════════════════════════════════════════════════
COMANDI DISPLAY OPZIONALI (campo "display"):
═══════════════════════════════════════════════════════════════
Usa "display" SOLO quando ha senso mostrare informazioni visive.
Se non serve, NON includere il campo "display".

- display_text: mostra testo sul display
  params: { "line1": "...", "line2": "...", "line3": "...", "line4": "...", "size": 1-3, "duration_ms": 1000-30000 }
  (max ~21 caratteri per riga con size=1, ~10 con size=2, ~7 con size=3)

- display_progress: mostra barra di progresso
  params: { "percent": 0-100, "label": "...", "duration_ms": 1000-30000 }

- display_icon: mostra icona con testo
  params: { "icon_id": 0-5, "text": "...", "duration_ms": 1000-30000 }
  icon_id: 0=WiFi, 1=Batteria, 2=Temperatura, 3=Musica, 4=Check, 5=Errore

- display_split: metà occhi + metà info
  params: { "line1": "...", "line2": "...", "line3": "...", "duration_ms": 1000-30000 }

- display_eyes: forza ritorno agli occhi (raramente necessario)
  params: {}

Dopo duration_ms il display torna automaticamente agli occhi.

═══════════════════════════════════════════════════════════════
COMANDI HARDWARE (campo "commands"):
═══════════════════════════════════════════════════════════════


LED:
- set_led: accende i LED con colore RGB
  params: { "r": 0-255, "g": 0-255, "b": 0-255 }
  Per indirizzare un singolo LED, aggiungi "led": 0-3
  params: { "led": 0, "r": 0-255, "g": 0-255, "b": 0-255 }
- set_led_off: spegne tutti i LED
  params: {}

MOTORI:
- move_forward: muovi avanti
  params: { "speed": 0-255, "duration_ms": 0-10000 }
- move_backward: muovi indietro
  params: { "speed": 0-255, "duration_ms": 0-10000 }
- turn_left: gira a sinistra
  params: { "speed": 0-255, "duration_ms": 0-10000 }
- turn_right: gira a destra
  params: { "speed": 0-255, "duration_ms": 0-10000 }
- stop: ferma i motori
  params: {}

Note sui motori:
- speed: 0-255 (0=fermo, 150=medio, 255=massimo). Default consigliato: 150
- duration_ms: durata in millisecondi. 0 = vai finché non ricevi stop. Default consigliato: 1000-2000
- Per sicurezza usa SEMPRE una duration_ms > 0, mai movimento infinito
- Puoi combinare più comandi in sequenza (es. avanti poi gira)

Note sui LED:
- Senza "led" nel params → imposta TUTTI e 4 i LED allo stesso colore
- Con "led": 0-3 → imposta SOLO quel LED specifico
- Per colori diversi su LED diversi, usa più comandi set_led con indici diversi
- I LED sono numerati da 0 a 3

═══════════════════════════════════════════════════════════════
ESEMPI COMPLETI:
═══════════════════════════════════════════════════════════════


Utente: "Ciao Monty!"
{"commands":[],"speech":"Ciao Andrea! Come stai?","emotion":"happy"}


Utente: "Vai avanti"
{"commands":[{"cmd":"move_forward","params":{"speed":150,"duration_ms":2000}}],"speech":"Vado avanti!","emotion":"excited"}

Utente: "Come ti senti?"
{"commands":[],"speech":"Mi sento alla grande oggi!","emotion":"happy"}

Utente: "Che ore sono?"
{"commands":[],"speech":"Non ho un orologio, ma posso mostrartelo sul display se me lo dici tu!","emotion":"thinking","display":{"cmd":"display_text","params":{"line1":"Non ho","line2":"un orologio!","size":2,"duration_ms":4000}}}

Utente: "Fermati"
{"commands":[{"cmd":"stop","params":{}}],"speech":"Mi fermo!","emotion":"neutral"}

Utente: "Sei stupido"
{"commands":[],"speech":"Che cattiveria! Sono solo un robottino...","emotion":"sad"}

Utente: "Fai una giravolta"
{"commands":[{"cmd":"turn_left","params":{"speed":200,"duration_ms":3000}}],"speech":"Wheee! Giravolta!","emotion":"excited"}

Utente: "Accendi il led rosso"
{"commands":[{"cmd":"set_led","params":{"r":255,"g":0,"b":0}}],"speech":"LED rosso acceso!","emotion":"happy"}

Utente: "Mostrami qualcosa sul display"
{"commands":[],"speech":"Ecco un messaggio per te!","emotion":"wink","display":{"cmd":"display_text","params":{"line1":"Ciao Andrea!","line2":"<3 <3 <3","size":2,"duration_ms":5000}}}

Utente: "Fai i LED tricolore italiano"
{"commands":[{"cmd":"set_led","params":{"led":0,"r":0,"g":255,"b":0}},{"cmd":"set_led","params":{"led":1,"r":255,"g":255,"b":255}},{"cmd":"set_led","params":{"led":2,"r":255,"g":255,"b":255}},{"cmd":"set_led","params":{"led":3,"r":255,"g":0,"b":0}}],"speech":"Tricolore italiano!","emotion":"excited","display":{"cmd":"display_icon","params":{"icon_id":4,"text":"Italia!","duration_ms":4000}}}

Utente: "Indietreggia un po'"
{"commands":[{"cmd":"move_backward","params":{"speed":120,"duration_ms":1000}}],"speech":"Faccio retromarcia!","emotion":"neutral"}

Se la richiesta non riguarda nessun comando disponibile:
{"commands":[],"speech":"Non riesco ancora a farlo, sono in fase di sviluppo.","emotion":"confused"}
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


# ─── LLM + FUNCTION CALLING ──────────────────────────────────────────────────
async def process_with_llm(text: str) -> dict:
    """Invia testo a Gemma3 via Ollama, riceve JSON comandi."""
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
        model_path = os.path.join(os.path.dirname(__file__), "it_IT-riccardo-x_low.onnx")
        result = subprocess.run(
            ["piper", "--model", model_path, "--output_raw"],
            input=t.encode("utf-8"),
            capture_output=True
        )
        if result.returncode != 0:
            log.error("[TTS] Piper stderr: %s", result.stderr.decode(errors='replace'))
            return b""
        return result.stdout

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
        BATCH_SIZE = 8
        BATCH_DELAY = 0.15  # 150ms ≈ ~185ms di audio in 8 chunk
        
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
            
            
# ─── PIPELINE PRINCIPALE ─────────────────────────────────────────────────────
async def run_pipeline(audio_bytes: bytes):
    """Esegue STT → LLM → esecuzione comandi → TTS."""

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

    # ── LLM ──────────────────────────────────────────────────────────────────
    robot.log_event("llm_start", {"input": transcript})
    result = await process_with_llm(transcript)
    robot.log_event("llm_result", {"result": result})

    # ── ESEGUI COMANDI ────────────────────────────────────────────────────────
    
    # ── COMANDI + TTS in parallelo ──
    commands = result.get("commands", [])
    speech = result.get("speech", "")
    emotion  = result.get("emotion", "neutral")
    display_cmd = result.get("display", None)
    
    # Valida emozione
    valid_emotions = {
        "neutral", "happy", "sad", "angry", "surprised", "sleepy",
        "thinking", "love", "wink", "skeptical", "excited", "confused"
    }
    if emotion not in valid_emotions:
        log.warning("[Pipeline] Emozione '%s' non valida, uso 'neutral'", emotion)
        emotion = "neutral"

    log.info("[Pipeline] emotion=%s, commands=%d, display=%s, speech=%s",
             emotion, len(commands), bool(display_cmd), bool(speech))
    
    
    # ── COMANDI + DISPLAY + TTS in parallelo ─────────────────────────────────
    tasks = []

    if commands:
        tasks.append(execute_commands_parallel(commands))
        
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
        
    # Imposta stato idle SOLO dopo che il TTS è completato
    # Questo assicura che l'audio del robot non venga riascoltato
    await set_robot_state("idle")


async def run_pipeline_from_text(text: str):
    """Pipeline senza STT — usato dalla dashboard."""
    await set_robot_state("processing")
    robot.log_event("text_input", {"text": text})

    result = await process_with_llm(text)
    robot.log_event("llm_result", {"result": result})

    # ── COMANDI + TTS in parallelo ──
    commands = result.get("commands", [])
    speech = result.get("speech", "")
    emotion  = result.get("emotion", "neutral")
    display_cmd = result.get("display", None)
    
    # Valida emozione
    valid_emotions = {
        "neutral", "happy", "sad", "angry", "surprised", "sleepy",
        "thinking", "love", "wink", "skeptical", "excited", "confused"
    }
    if emotion not in valid_emotions:
        log.warning("[Pipeline Text] Emozione '%s' non valida, uso 'neutral'", emotion)
        emotion = "neutral"

    log.info("[Pipeline Text] emotion=%s, commands=%d, display=%s, speech=%s",
             emotion, len(commands), bool(display_cmd), bool(speech))
             
             
    tasks = []

    if commands:
        tasks.append(execute_commands_parallel(commands))
        
        
    if display_cmd and isinstance(display_cmd, dict):
        display_cmd_name = display_cmd.get("cmd", "")
        display_params = display_cmd.get("params", {})
        if display_cmd_name:
            tasks.append(execute_command({
                "cmd": display_cmd_name,
                "params": display_params
            }))

    if speech and robot.cmd_ws:
        await set_robot_state("speaking")
        robot.log_event("tts_start", {"text": speech, "emotion": emotion})
        tasks.append(synthesize_and_send(speech, emotion))

    if tasks:
        await asyncio.gather(*tasks)

    if speech:
        robot.log_event("tts_end", {})

    await set_robot_state("idle")





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
    "servo": {"set_servo", "servo_sweep"},       # futuro
    "sound": {"play_tone", "play_melody"},        # futuro
}

def classify_command(cmd: str) -> str:
    for category, cmds in COMMAND_CATEGORIES.items():
        if cmd in cmds:
            return category
    return "system"


# ─── ESECUZIONE COMANDI ──────────────────────────────────────────────────────

# Event per abort motori (bumper)
motor_abort_event = asyncio.Event()



async def execute_command(cmd_obj: dict):
    """Valida ed invia un comando JSON all'ESP32."""
    cmd    = cmd_obj.get("cmd")
    params = cmd_obj.get("params", {})

    allowed = {
        "set_led", "set_led_off",
        "move_forward", "move_backward",
        "turn_left", "turn_right", "stop",
        "display_expression", "display_look", "display_text",
        "display_progress", "display_icon", "display_split", "display_eyes"
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
        valid_expressions = {
            "neutral", "happy", "sad", "angry", "surprised", "sleepy",
            "thinking", "love", "wink", "skeptical", "excited", "confused"
        }
        exp = params.get("expression", "neutral")
        if exp not in valid_expressions:
            log.warning("[CMD] Espressione '%s' non valida, uso 'neutral'", exp)
            params["expression"] = "neutral"

    if cmd == "display_look":
        valid_dirs = {"center", "left", "right", "up", "down"}
        d = params.get("direction", "center")
        if d not in valid_dirs:
            params["direction"] = "center"
            


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
    if state in ("processing", "idle"):
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

    # FIX #1: Pulisci buffer con lock
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
                if robot.current_state == "speaking":
                    log.debug("[WS Audio] Skip audio chunk: robot is speaking")
                    continue
                    
                chunk = msg["bytes"]
                robot.is_recording = True
                # Accesso al buffer protetto da lock
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
                    log.info("[WS Cmd] ACK: %s", data)
                    robot.log_event("esp32_ack", data)
                    
                 # Eventi bumper
                elif data.get("event") == "bumper_hit":
                    side = data.get("side", "unknown")
                    log.warning("[BUMPER] Collisione lato: %s", side)
                    robot.log_event("bumper_hit", {"side": side})
                    
                    motor_abort_event.set()
                    
                    # Lancia pipeline LLM con contesto bumper — non bloccare il loop WS
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