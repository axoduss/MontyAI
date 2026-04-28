"""Skill: Data e ora attuali."""

from datetime import datetime
from typing import Any, Dict
from .base import BaseSkill

import locale
try:
    locale.setlocale(locale.LC_TIME, 'it_IT.UTF-8')
except locale.Error:
    pass


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
                "day_of_week": now.strftime("%A"),
                "timestamp": now.isoformat()
            }
        }