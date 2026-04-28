"""Classe base per tutte le skill del robot."""

from abc import ABC, abstractmethod
from typing import Any, Dict, Optional


class BaseSkill(ABC):
    """Ogni skill deve ereditare da questa classe."""

    # ── Metadati (ogni skill li sovrascrive) ──
    name: str = ""                    # ID univoco (es. "get_weather")
    description: str = ""             # Descrizione breve per il system prompt
    params_schema: Dict[str, str] = {}  # Parametri accettati
    example: str = ""                 # Esempio JSON per il prompt
    
    # ── Formato risposta per il 2° passaggio LLM ──
    @abstractmethod
    def format_result(self, data: Dict[str, Any]) -> str:
        """Formatta i dati della skill in testo leggibile per l'LLM."""
        pass

    @abstractmethod
    async def execute(self, **kwargs) -> Dict[str, Any]:
        """
        Esegue la skill.
        Deve restituire: {"success": True/False, "data": {...}}
        """
        pass

    def get_prompt_entry(self) -> str:
        """Genera automaticamente la riga per il system prompt."""
        params_str = ""
        if self.params_schema:
            params_list = [f'"{k}"' for k in self.params_schema.keys()]
            params_str = f" Params: {', '.join(params_list)}."
        
        return f"- use_skill: {{\"skill\":\"{self.name}\"}}{params_str}\n  {self.description}\n  Es: {self.example}"