"""
SKILL MANAGER - Gestione dinamica delle abilità del robot
- get_current_datetime: data e ora attuali
- get_weather: meteo tramite Open-Meteo API (gratuita, no key)
- get_news: ultime notizie da feed RSS
- web_search: ricerche generiche (simulato con DuckDuckGo HTML parsing)
"""

import asyncio
import logging
from datetime import datetime
from typing import Optional, Dict, Any

import httpx
import feedparser

log = logging.getLogger("robot.skills")

# ─── CONFIG SKILL ──────────────────────────────────────────────────────────────
# Coordinate default (Roma, modificabile in base alla posizione reale)
#DEFAULT_LAT = 41.9028
#DEFAULT_LON = 12.4964

DEFAULT_LAT = 45.1742
DEFAULT_LON = 9.6586


# ─── SKILL DEFINITIONS ─────────────────────────────────────────────────────────

async def get_current_datetime() -> Dict[str, Any]:
    """Restituisce data e ora attuali."""
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


async def get_weather(location: Optional[str] = None, lat: float = DEFAULT_LAT, lon: float = DEFAULT_LON) -> Dict[str, Any]:
    """
    Ottiene il meteo attuale tramite Open-Meteo API (gratuita, no API key).
    Se location è specificata, si potrebbero usare geocoding (per ora usa lat/lon default).
    """
    try:
        async with httpx.AsyncClient(timeout=10.0) as client:
            url = "https://api.open-meteo.com/v1/forecast"
            params = {
                "latitude": lat,
                "longitude": lon,
                "current_weather": True,
                "timezone": "auto"
            }
            response = await client.get(url, params=params)
            response.raise_for_status()
            data = response.json()
            
            current = data.get("current_weather", {})
            weather_code = current.get("weathercode", 0)
            
            # Mappa WMO weather codes a descrizioni
            weather_descriptions = {
                0: "Sereno",
                1: "Prevalentemente sereno",
                2: "Parzialmente nuvoloso",
                3: "Nuvoloso",
                45: "Nebbia",
                48: "Nebbia con brina",
                51: "Pioggia leggera",
                53: "Pioggia moderata",
                55: "Pioggia forte",
                61: "Pioggia debole",
                63: "Pioggia moderata",
                65: "Pioggia forte",
                71: "Neve debole",
                73: "Neve moderata",
                75: "Neve forte",
                80: "Rovesci deboli",
                81: "Rovesci moderati",
                82: "Rovesci violenti",
                95: "Temporale",
                96: "Temporale con grandine debole",
                99: "Temporale con grandine forte"
            }
            
            description = weather_descriptions.get(weather_code, "Condizioni sconosciute")
            
            return {
                "success": True,
                "data": {
                    "temperature": current.get("temperature", 0),
                    "windspeed": current.get("windspeed", 0),
                    "winddirection": current.get("winddirection", 0),
                    "weather_code": weather_code,
                    "description": description,
                    "location": location or f"Lat: {lat}, Lon: {lon}"
                }
            }
    except Exception as e:
        log.error("[Weather] Errore: %s", e)
        return {"success": False, "error": str(e)}


async def get_news(category: str = "general", limit: int = 3) -> Dict[str, Any]:
    """
    Ottiene ultime notizie da feed RSS italiani.
    Category: general, cronaca, politica, economia, sport, tecnologia
    """
    # Feed RSS pubblici italiani (ANSA, Corriere, Repubblica, ecc.)
    feeds = {
        "general": "https://www.ansa.it/sito/ansait_rss.xml",
        "cronaca": "https://www.ansa.it/sito/notizie/cronaca/cronaca_rss.xml",
        "politica": "https://www.ansa.it/sito/notizie/politica/politica_rss.xml",
        "economia": "https://www.ansa.it/sito/notizie/economia/economia_rss.xml",
        "sport": "https://www.ansa.it/sito/notizie/sport/sport_rss.xml",
        "tecnologia": "https://www.ansa.it/canale_tecnologia/notizie/tecnologia_rss.xml"
    }
    
    feed_url = feeds.get(category, feeds["general"])
    
    try:
        async with httpx.AsyncClient(timeout=10.0) as client:
            response = await client.get(feed_url)
            response.raise_for_status()
            
            feed = feedparser.parse(response.content)
            entries = feed.entries[:limit]
            
            news_list = []
            for entry in entries:
                news_list.append({
                    "title": entry.title,
                    "link": entry.link,
                    "published": entry.get("published", "Data non disponibile")
                })
            
            return {
                "success": True,
                "data": {
                    "category": category,
                    "news": news_list,
                    "source": feed.feed.get("title", "Fonte sconosciuta")
                }
            }
    except Exception as e:
        log.error("[News] Errore: %s", e)
        return {"success": False, "error": str(e)}


async def web_search(query: str, limit: int = 3) -> Dict[str, Any]:
    """
    Esegue una ricerca web generica usando DuckDuckGo (HTML parsing).
    Nota: Questo è un approccio semplice, per produzione considerare API dedicate.
    """
    try:
        async with httpx.AsyncClient(timeout=10.0, follow_redirects=True) as client:
            # DuckDuckGo HTML search
            url = "https://html.duckduckgo.com/html/"
            params = {"q": query, "kl": "it-it"}
            headers = {
                "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"
            }
            
            response = await client.post(url, data=params, headers=headers)
            response.raise_for_status()
            
            # Parsing semplice dei risultati (cerca i link nei result snippets)
            results = []
            html_content = response.text
            
            # Estrai titoli e link dai risultati (approccio semplificato)
            import re
            title_pattern = r'<a class="result__a" href="([^"]+)">([^<]+)</a>'
            matches = re.findall(title_pattern, html_content, re.IGNORECASE)
            
            for link, title in matches[:limit]:
                # Decodifica eventuali entità HTML
                title = title.replace("&amp;", "&").replace("&lt;", "<").replace("&gt;", ">")
                results.append({
                    "title": title.strip(),
                    "url": link
                })
            
            return {
                "success": True,
                "data": {
                    "query": query,
                    "results": results,
                    "count": len(results)
                }
            }
    except Exception as e:
        log.error("[WebSearch] Errore: %s", e)
        return {"success": False, "error": str(e)}


# ─── SKILL REGISTRY ────────────────────────────────────────────────────────────

SKILL_REGISTRY = {
    "get_current_datetime": {
        "function": get_current_datetime,
        "description": "Ottieni la data e l'ora attuali",
        "params": {}
    },
    "get_weather": {
        "function": get_weather,
        "description": "Ottieni le condizioni meteo attuali (temperatura, descrizione, vento)",
        "params": {
            "location": "string opzionale: nome località (usa coordinate default se non specificato)",
            "lat": "float opzionale: latitudine (default: Roma)",
            "lon": "float opzionale: longitudine (default: Roma)"
        }
    },
    "get_news": {
        "function": get_news,
        "description": "Ottieni le ultime notizie da feed RSS italiani",
        "params": {
            "category": "string: categoria (general, cronaca, politica, economia, sport, tecnologia)",
            "limit": "int: numero di notizie da restituire (default: 3)"
        }
    },
    "web_search": {
        "function": web_search,
        "description": "Esegui una ricerca su internet per trovare informazioni aggiornate",
        "params": {
            "query": "string: termini di ricerca",
            "limit": "int: numero di risultati (default: 3)"
        }
    }
}


async def execute_skill(skill_name: str, **kwargs) -> Dict[str, Any]:
    """
    Esegue una skill dal registry con i parametri forniti.
    """
    if skill_name not in SKILL_REGISTRY:
        return {
            "success": False,
            "error": f"Skill '{skill_name}' non trovata. Skill disponibili: {list(SKILL_REGISTRY.keys())}"
        }
    
    skill_info = SKILL_REGISTRY[skill_name]
    func = skill_info["function"]
    
    try:
        result = await func(**kwargs)
        return result
    except TypeError as e:
        return {"success": False, "error": f"Parametri errati: {str(e)}"}
    except Exception as e:
        log.error("[Skill] Errore esecuzione '%s': %s", skill_name, e)
        return {"success": False, "error": str(e)}


def get_skills_description() -> str:
    """
    Restituisce una descrizione testuale delle skill disponibili per il SYSTEM_PROMPT.
    """
    desc = """\n
═══════════════════════════════════════════════════════════════
SKILL ESTERNE DISPONIBILI (accesso internet e informazioni):
═══════════════════════════════════════════════════════════════
Puoi usare queste skill per ottenere informazioni in tempo reale.
Per usarle, INCLUDI nel campo "commands" un comando speciale con cmd="use_skill".

Formato:
{"cmd": "use_skill", "params": {"skill": "<nome_skill>", ...parametri specifici...}}

Skill disponibili:

1. get_current_datetime - Data e ora attuali
   Parametri: nessuno
   Esempio: {"cmd": "use_skill", "params": {"skill": "get_current_datetime"}}

2. get_weather - Meteo attuale
   Parametri: location (opzionale), lat (opzionale), lon (opzionale)
   Esempio: {"cmd": "use_skill", "params": {"skill": "get_weather", "location": "Milano"}}

3. get_news - Ultime notizie
   Parametri: category (general/cronaca/politica/economia/sport/tecnologia), limit (default 3)
   Esempio: {"cmd": "use_skill", "params": {"skill": "get_news", "category": "politica", "limit": 2}}

4. web_search - Ricerca su internet
   Parametri: query (obbligatorio), limit (default 3)
   Esempio: {"cmd": "use_skill", "params": {"skill": "web_search", "query": "chi è il presidente italiano"}}

NOTA IMPORTANTE: Quando usi una skill, il sistema eseguirà la funzione e otterrà i dati.
Dopo aver ricevuto i dati, potrai formularli in una risposta naturale all'utente.
Se la richiesta dell'utente riguarda informazioni che richiedono dati aggiornati (meteo, notizie, 
ricerche, data/ora), USA SEMPRE la skill appropriata prima di rispondere.
"""
    return desc
