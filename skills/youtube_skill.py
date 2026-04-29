"""Skill: Cerca e riproduce musica da YouTube tramite speaker ESP32."""

import os
import asyncio
import subprocess
import logging
from typing import Any, Dict, Optional
from .base import BaseSkill

log = logging.getLogger("robot.skills.youtube")

# Cartella temporanea per audio scaricato
AUDIO_CACHE_DIR = os.path.join(os.path.dirname(__file__), "..", "audio_cache")
os.makedirs(AUDIO_CACHE_DIR, exist_ok=True)

# Durata massima riproduzione (secondi)
MAX_PLAY_SECONDS = 60
# Sample rate output (deve matchare quello dell'ESP32)
OUTPUT_SAMPLE_RATE = 22050


class YouTubeSkill(BaseSkill):
    name = "play_music"
    description = "Cerca una canzone su YouTube e la riproduce dall'altoparlante"
    params_schema = {
        "query": "titolo canzone o artista (obbligatorio)",
        "duration": "secondi da riprodurre, max 60 (default: 30)"
    }
    example = '{"cmd":"use_skill","params":{"skill":"play_music","query":"Bohemian Rhapsody Queen"}}'

    def format_result(self, data: Dict[str, Any]) -> str:
        if data.get("audio_ready"):
            return (
                f"Musica pronta: \"{data.get('title', '?')}\" "
                f"({data.get('duration', '?')}s)"
            )
        return f"Musica: errore"

    async def execute(
        self, query: str = "", duration: int = 30, **kwargs
    ) -> Dict[str, Any]:
        if not query:
            return {"success": False, "error": "Nessuna canzone specificata"}

        duration = max(5, min(MAX_PLAY_SECONDS, int(duration)))

        try:
            # 1. Cerca su YouTube e scarica audio
            audio_path, title = await self._download_audio(query, duration)

            if not audio_path or not os.path.exists(audio_path):
                return {"success": False, "error": "Impossibile scaricare l'audio"}

            # 2. Converti in PCM16 mono (stesso formato TTS)
            pcm_path = await self._convert_to_pcm(audio_path, duration)

            if not pcm_path or not os.path.exists(pcm_path):
                return {"success": False, "error": "Errore conversione audio"}

            # Leggi i byte PCM
            with open(pcm_path, "rb") as f:
                pcm_bytes = f.read()

            # Cleanup file temporanei
            self._cleanup(audio_path, pcm_path)

            log.info("[YouTube] Pronto: '%s', %d bytes PCM (%.1fs)",
                     title, len(pcm_bytes), len(pcm_bytes) / (OUTPUT_SAMPLE_RATE * 2))

            return {
                "success": True,
                "data": {
                    "title": title,
                    "duration": duration,
                    "audio_bytes": len(pcm_bytes),
                    "audio_ready": True,
                    "_pcm_data": pcm_bytes
                }
            }

        except Exception as e:
            log.error("[YouTube] Errore: %s", repr(e))
            return {"success": False, "error": str(e)}

    async def _download_audio(self, query: str, max_duration: int) -> tuple:
        """Scarica audio da YouTube usando yt-dlp."""

        # Pulisci la cache
        for f in os.listdir(AUDIO_CACHE_DIR):
            try:
                os.remove(os.path.join(AUDIO_CACHE_DIR, f))
            except Exception:
                pass

        output_template = os.path.join(AUDIO_CACHE_DIR, "%(id)s.%(ext)s")

        # Step 1: Ottieni titolo (separato, più affidabile)
        title_cmd = [
            "yt-dlp",
            f"ytsearch1:{query}",
            "--print", "title",
            "--no-download",
            "--no-playlist",
            "--quiet",
            "--no-warnings",
        ]

        # Step 2: Scarica audio
        download_cmd = [
            "yt-dlp",
            f"ytsearch1:{query}",
            "--extract-audio",
            "--audio-format", "mp3",
            "--audio-quality", "9",
            "--no-playlist",
            "--output", output_template,
            "--quiet",
            "--no-warnings",
        ]

        def run_title():
            return subprocess.run(
                title_cmd, capture_output=True, text=True, timeout=15
            )

        def run_download():
            return subprocess.run(
                download_cmd, capture_output=True, text=True, timeout=60
            )

        # Ottieni titolo
        title = "Brano sconosciuto"
        try:
            title_result = await asyncio.to_thread(run_title)
            if title_result.returncode == 0 and title_result.stdout.strip():
                title = title_result.stdout.strip().split("\n")[0]
                log.info("[YouTube] Titolo: '%s'", title)
        except Exception as e:
            log.warning("[YouTube] Titolo non ottenuto: %s", e)

        # Scarica
        result = await asyncio.to_thread(run_download)

        if result.returncode != 0:
            stderr = result.stderr[:500] if result.stderr else "nessun errore specifico"
            log.error("[YouTube] yt-dlp errore (code %d): %s",
                      result.returncode, stderr)
            return None, None

        # Cerca il file scaricato
        filepath = None
        for ext in (".mp3", ".m4a", ".webm", ".opus", ".ogg"):
            for f in os.listdir(AUDIO_CACHE_DIR):
                if f.endswith(ext):
                    filepath = os.path.join(AUDIO_CACHE_DIR, f)
                    break
            if filepath:
                break

        if not filepath:
            log.error("[YouTube] Nessun file audio in %s. Contenuto: %s",
                      AUDIO_CACHE_DIR, os.listdir(AUDIO_CACHE_DIR))
            return None, None

        log.info("[YouTube] File: %s (%.1f KB)", filepath,
                 os.path.getsize(filepath) / 1024)
        return filepath, title

    async def _convert_to_pcm(self, input_path: str, max_seconds: int) -> str:
        """Converte audio in PCM16 mono, stesso formato del TTS ESP32."""

        output_path = os.path.join(AUDIO_CACHE_DIR, "playback.raw")

        cmd = [
            "ffmpeg",
            "-y",
            "-i", input_path,
            "-t", str(max_seconds),
            "-ar", str(OUTPUT_SAMPLE_RATE),
            "-ac", "1",
            "-f", "s16le",
            "-acodec", "pcm_s16le",
            output_path
        ]

        def run():
            return subprocess.run(
                cmd, capture_output=True, text=True, timeout=60
            )

        result = await asyncio.to_thread(run)

        if result.returncode != 0:
            log.error("[YouTube] ffmpeg errore: %s", result.stderr[:300])
            return None

        file_size = os.path.getsize(output_path)
        log.info("[YouTube] PCM: %d bytes (%.1fs @ %dHz)",
                 file_size, file_size / (OUTPUT_SAMPLE_RATE * 2), OUTPUT_SAMPLE_RATE)
        return output_path

    def _cleanup(self, *paths):
        """Rimuovi file temporanei."""
        for path in paths:
            try:
                if path and os.path.exists(path):
                    os.remove(path)
            except Exception:
                pass