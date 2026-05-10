from typing import Any, Dict
from .base import BaseSkill

class SensorSkill(BaseSkill):
    name = "get_sensor_data"
    description = "Legge i sensori hardware del robot (temperatura in °C, pressione in hPa, inclinazione)."
    params_schema = {}  # Nessun parametro richiesto dall'LLM
    example = '{"cmd":"use_skill","params":{"skill":"get_sensor_data"}}'

    def format_result(self, data: Dict[str, Any]) -> str:
        """Formatta i dati per il secondo passaggio dell'LLM."""
        parts = []
        if "temperature" in data:
            parts.append(f"Temperatura: {data['temperature']}°C")
        if "pressure" in data:
            parts.append(f"Pressione: {data['pressure']} hPa")
        if "tilt" in data:
            parts.append(f"Inclinazione: {data['tilt']}")
            
        if not parts:
            return "Dati sensori non disponibili al momento."
            
        return "Dati sensori attuali: " + ", ".join(parts)

    async def execute(self, **kwargs) -> Dict[str, Any]:
        """
        Esegue la skill. I dati dei sensori vengono iniettati dal server 
        tramite i kwargs per evitare import circolari.
        """
        sensor_data = kwargs.get("sensor_data")
        
        if not sensor_data:
            return {
                "success": False, 
                "error": "Nessun dato ricevuto dai sensori. L'ESP32 potrebbe non averli ancora inviati."
            }

        # Estraiamo e puliamo i dati per l'LLM
        result_data = {}
        
        if "temp" in sensor_data:
            result_data["temperature"] = round(sensor_data["temp"], 1)
            
        if "press" in sensor_data:
            result_data["pressure"] = round(sensor_data["press"], 1)
            
        if "tilt" in sensor_data:
            # Assumendo che tilt sia un dizionario con x e y
            tilt = sensor_data["tilt"]
            if isinstance(tilt, dict):
                result_data["tilt"] = f"X={tilt.get('x', 0)}°, Y={tilt.get('y', 0)}°"
            else:
                result_data["tilt"] = str(tilt)

        return {"success": True, "data": result_data}