from typing import Any, Dict
from .base import BaseSkill

class SensorSkill(BaseSkill):
    name = "get_sensor_data"
    description = "Legge i sensori hardware del robot (temperatura in °C, pressione in hPa, inclinazione, distanza ostacoli, orientamento)."
    params_schema = {}
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
        if "distance_cm" in data:
            dist = data["distance_cm"]
            if dist < 0:
                parts.append("Distanza ultrasuoni: nessun ostacolo rilevato")
            else:
                parts.append(f"Distanza ostacolo frontale: {dist:.0f} cm")
        if "us_obstacle" in data:
            parts.append(f"Ostacolo critico: {'sì' if data['us_obstacle'] else 'no'}")
        if "us_mode" in data:
            parts.append(f"Modalità ultrasuoni: {data['us_mode']}")
        if "yaw" in data:
            parts.append(f"Orientamento (yaw): {data['yaw']:.0f}°")
            
        if not parts:
            return "Dati sensori non disponibili al momento."
            
        return "Dati sensori attuali: " + ", ".join(parts)

    async def execute(self, **kwargs) -> Dict[str, Any]:
        """
        Esegue la skill. I dati vengono iniettati dal server tramite kwargs.
        """
        sensor_data = kwargs.get("sensor_data")
        
        if not sensor_data:
            return {
                "success": False, 
                "error": "Nessun dato ricevuto dai sensori. L'ESP32 potrebbe non averli ancora inviati."
            }

        result_data = {}
        
        if "temp" in sensor_data:
            result_data["temperature"] = round(sensor_data["temp"], 1)
            
        if "press" in sensor_data:
            result_data["pressure"] = round(sensor_data["press"], 1)
            
        if "tilt" in sensor_data:
            tilt = sensor_data["tilt"]
            if isinstance(tilt, dict):
                result_data["tilt"] = f"X={tilt.get('x', 0)}°, Y={tilt.get('y', 0)}°"
            else:
                result_data["tilt"] = str(tilt)

        # Dati ultrasuoni (iniettati dal server separatamente)
        ultrasonic_data = kwargs.get("ultrasonic_data")
        if ultrasonic_data:
            dist = ultrasonic_data.get("distance_cm", -1)
            result_data["distance_cm"] = round(dist, 1) if dist >= 0 else -1
            result_data["us_obstacle"] = ultrasonic_data.get("obstacle", False)
            result_data["us_mode"] = ultrasonic_data.get("mode", "monitor")
            result_data["yaw"] = round(ultrasonic_data.get("yaw", 0), 1)

        return {"success": True, "data": result_data}