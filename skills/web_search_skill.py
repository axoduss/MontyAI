"""Skill: Ricerca web via DuckDuckGo."""

import re
from typing import Any, Dict
import httpx
import logging
from .base import BaseSkill

log = logging.getLogger("robot.skills.search")


class WebSearchSkill(BaseSkill):
    name = "web_search"
    description = "Ricerca su internet"
    params_schema = {
        "query": "termini di ricerca (obbligatorio)",
        "limit": "numero risultati (default: 3)"
    }
    example = '{"cmd":"use_skill","params":{"skill":"web_search","query":"capitale Italia"}}'

    def format_result(self, data: Dict[str, Any]) -> str:
        results = data.get("results", [])
        if not results:
            return "Ricerca web: nessun risultato"
        search_text = "\n".join(
            f"  • {r.get('title', '?')}" for r in results
        )
        return f"Ricerca web (query: {data.get('query', '?')}):\n{search_text}"

    async def execute(
        self, query: str = "", limit: int = 3, **kwargs
    ) -> Dict[str, Any]:
        if not query:
            return {"success": False, "error": "Query vuota"}
        try:
            async with httpx.AsyncClient(
                timeout=10.0, follow_redirects=True
            ) as client:
                response = await client.post(
                    "https://html.duckduckgo.com/html/",
                    data={"q": query, "kl": "it-it"},
                    headers={
                        "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                                      "AppleWebKit/537.36"
                    }
                )
                response.raise_for_status()

                matches = re.findall(
                    r'<a class="result__a" href="([^"]+)">([^<]+)</a>',
                    response.text, re.IGNORECASE
                )

                results = [
                    {
                        "title": title.replace("&amp;", "&")
                                      .replace("&lt;", "<")
                                      .replace("&gt;", ">")
                                      .strip(),
                        "url": link
                    }
                    for link, title in matches[:limit]
                ]

                return {
                    "success": True,
                    "data": {
                        "query": query,
                        "results": results,
                        "count": len(results)
                    }
                }
        except Exception as e:
            log.error("[WebSearch] %s", e)
            return {"success": False, "error": str(e)}