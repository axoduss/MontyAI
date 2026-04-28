"""Skill: Meteo attuale via Open-Meteo API."""

from typing import Any, Dict, Optional
import httpx
import logging
from .base import BaseSkill

log = logging.getLogger("robot.skills.weather")

DEFAULT_LAT = 45.1742
DEFAULT_LON = 9.6586

WEATHER_CODES = {
    0: "Sereno", 1: "Prevalentemente sereno", 2: "Parzialmente nuvoloso",
    3: "Nuvoloso", 45: "Nebbia", 48: "Nebbia con brina",
    51: "Pioggia leggera", 53: "Pioggia moderata", 55: "Pioggia forte",
    61: "Pioggia debole", 63: "Pioggia moderata", 65: "Pioggia forte",
    71: "Neve debole", 73: "Neve moderata", 75: "Neve forte",
    80: "Rovesci deboli", 81: "Rovesci moderati", 82: "Rovesci violenti",
    95: "Temporale", 96: "Temporale con grandine debole",
    99: "Temporale con grandine forte"
}


class WeatherSkill(BaseSkill):
    name = "get_weather"
    description = "Meteo attuale (temperatura, descrizione, vento)"
    params_schema = {
        "location": "nome località (opzionale)",
        "lat": "latitudine (opzionale)",
        "lon": "longitudine (opzionale)"
    }
    example = '{"cmd":"use_skill","params":{"skill":"get_weather","location":"Milano"}}'

    def format_result(self, data: Dict[str, Any]) -> str:
        return (
            f"Meteo ({data.get('location', '?')}): "
            f"{data.get('description', '?')}, "
            f"{data.get('temperature', '?')}°C, "
            f"vento {data.get('windspeed', '?')} km/h"
        )

    async def execute(
        self,
        location: Optional[str] = None,
        lat: float = DEFAULT_LAT,
        lon: float = DEFAULT_LON,
        **kwargs
    ) -> Dict[str, Any]:
        try:
            async with httpx.AsyncClient(timeout=10.0) as client:
                response = await client.get(
                    "https://api.open-meteo.com/v1/forecast",
                    params={
                        "latitude": lat,
                        "longitude": lon,
                        "current_weather": True,
                        "timezone": "auto"
                    }
                )
                response.raise_for_status()
                current = response.json().get("current_weather", {})

                return {
                    "success": True,
                    "data": {
                        "temperature": current.get("temperature", 0),
                        "windspeed": current.get("windspeed", 0),
                        "winddirection": current.get("winddirection", 0),
                        "weather_code": current.get("weathercode", 0),
                        "description": WEATHER_CODES.get(
                            current.get("weathercode", 0),
                            "Condizioni sconosciute"
                        ),
                        "location": location or f"Lat:{lat}, Lon:{lon}"
                    }
                }
        except Exception as e:
            log.error("[Weather] %s", e)
            return {"success": False, "error": str(e)}