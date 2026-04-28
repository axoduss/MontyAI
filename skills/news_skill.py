"""Skill: Ultime notizie da feed RSS italiani."""

from typing import Any, Dict
import httpx
import feedparser
import logging
from .base import BaseSkill

log = logging.getLogger("robot.skills.news")

FEEDS = {
    "general": "https://www.ansa.it/sito/ansait_rss.xml",
    "cronaca": "https://www.ansa.it/sito/notizie/cronaca/cronaca_rss.xml",
    "politica": "https://www.ansa.it/sito/notizie/politica/politica_rss.xml",
    "economia": "https://www.ansa.it/sito/notizie/economia/economia_rss.xml",
    "sport": "https://www.ansa.it/sito/notizie/sport/sport_rss.xml",
    "tecnologia": "https://www.ansa.it/canale_tecnologia/notizie/tecnologia_rss.xml"
}


class NewsSkill(BaseSkill):
    name = "get_news"
    description = "Ultime notizie (general/cronaca/politica/economia/sport/tecnologia)"
    params_schema = {
        "category": "categoria notizie (default: general)",
        "limit": "numero notizie (default: 3)"
    }
    example = '{"cmd":"use_skill","params":{"skill":"get_news","category":"sport","limit":2}}'

    def format_result(self, data: Dict[str, Any]) -> str:
        news_list = data.get("news", [])
        if not news_list:
            return "Notizie: nessuna trovata"
        news_text = "\n".join(f"  • {n.get('title', '?')}" for n in news_list)
        return f"Notizie ({data.get('source', '?')}):\n{news_text}"

    async def execute(
        self, category: str = "general", limit: int = 3, **kwargs
    ) -> Dict[str, Any]:
        feed_url = FEEDS.get(category, FEEDS["general"])
        try:
            async with httpx.AsyncClient(timeout=10.0) as client:
                response = await client.get(feed_url)
                response.raise_for_status()
                feed = feedparser.parse(response.content)

                news_list = [
                    {
                        "title": entry.title,
                        "link": entry.link,
                        "published": entry.get("published", "N/D")
                    }
                    for entry in feed.entries[:limit]
                ]

                return {
                    "success": True,
                    "data": {
                        "category": category,
                        "news": news_list,
                        "source": feed.feed.get("title", "Fonte sconosciuta")
                    }
                }
        except Exception as e:
            log.error("[News] %s", e)
            return {"success": False, "error": str(e)}