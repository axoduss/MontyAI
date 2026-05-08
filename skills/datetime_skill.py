"""Skill: Data e ora attuali."""

from datetime import datetime
from typing import Any, Dict
from .base import BaseSkill

# import locale
# try:
    # locale.setlocale(locale.LC_TIME, 'it_IT.UTF-8')
# except locale.Error:
    # pass
    
    
GIORNI = {
    0: "lunedì",
    1: "martedì",
    2: "mercoledì",
    3: "giovedì",
    4: "venerdì",
    5: "sabato",
    6: "domenica"
}

MESI = {
    1: "gennaio", 2: "febbraio", 3: "marzo",
    4: "aprile", 5: "maggio", 6: "giugno",
    7: "luglio", 8: "agosto", 9: "settembre",
    10: "ottobre", 11: "novembre", 12: "dicembre"
}


class DateTimeSkill(BaseSkill):
    name = "get_current_datetime"
    description = "Data e ora attuali"
    params_schema = {}
    example = '{"cmd":"use_skill","params":{"skill":"get_current_datetime"}}'   

    def format_result(self, data: Dict[str, Any]) -> str:
        return (
            f"Data/Ora: {data.get('day_of_week', '')} "
            f"{data.get('date', '')}, ore {data.get('time', '')}"
        )

    async def execute(self, **kwargs) -> Dict[str, Any]:
        now = datetime.now()
        return {
            "success": True,
            "data": {
                "datetime": now.strftime("%Y-%m-%d %H:%M:%S"),
                "date": now.strftime("%d/%m/%Y"),
                "time": now.strftime("%H:%M"),
                "day_of_week": GIORNI[now.weekday()],
                "timestamp": now.isoformat()
            }
        }