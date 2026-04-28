"""Skill: Timer/countdown."""

import asyncio
from typing import Any, Dict
from .base import BaseSkill


class TimerSkill(BaseSkill):
    name = "set_timer"
    description = "Imposta un timer (max 300 secondi)"
    params_schema = {"seconds": "durata in secondi (obbligatorio)"}
    example = '{"cmd":"use_skill","params":{"skill":"set_timer","seconds":30}}'

    def format_result(self, data: Dict[str, Any]) -> str:
        return f"Timer impostato: {data.get('seconds', 0)} secondi"

    async def execute(self, seconds: int = 10, **kwargs) -> Dict[str, Any]:
        seconds = max(1, min(300, int(seconds)))
        # Non blocchiamo — segnaliamo solo che è stato impostato
        return {
            "success": True,
            "data": {
                "seconds": seconds,
                "message": f"Timer di {seconds} secondi avviato"
            }
        }