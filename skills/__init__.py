"""
Skill Manager — Auto-discovery delle skill.
Ogni file *_skill.py nella cartella viene caricato automaticamente.
"""

import importlib
import pkgutil
import logging
from typing import Any, Dict
from .base import BaseSkill

log = logging.getLogger("robot.skills")

# ── Registry globale ──
_SKILLS: Dict[str, BaseSkill] = {}


def _discover_skills():
    """Scansiona il package e registra tutte le classi che ereditano BaseSkill."""
    import skills as pkg

    for importer, modname, ispkg in pkgutil.iter_modules(pkg.__path__):
        if modname == "base" or not modname.endswith("_skill"):
            continue
        try:
            module = importlib.import_module(f".{modname}", package="skills")
            for attr_name in dir(module):
                attr = getattr(module, attr_name)
                if (
                    isinstance(attr, type)
                    and issubclass(attr, BaseSkill)
                    and attr is not BaseSkill
                    and attr.name  # ha un nome definito
                ):
                    instance = attr()
                    _SKILLS[instance.name] = instance
                    log.info("[Skills] Registrata: %s", instance.name)
        except Exception as e:
            log.error("[Skills] Errore caricamento '%s': %s", modname, e)


# Esegui discovery all'import del package
_discover_skills()


# ── API pubblica ──

async def execute_skill(skill_name: str, **kwargs) -> Dict[str, Any]:
    """Esegue una skill per nome con i parametri forniti."""
    if skill_name not in _SKILLS:
        return {
            "success": False,
            "error": f"Skill '{skill_name}' non trovata. Disponibili: {list(_SKILLS.keys())}"
        }

    skill = _SKILLS[skill_name]
    try:
        result = await skill.execute(**kwargs)
        return result
    except TypeError as e:
        return {"success": False, "error": f"Parametri errati: {str(e)}"}
    except Exception as e:
        log.error("[Skills] Errore esecuzione '%s': %s", skill_name, e)
        return {"success": False, "error": str(e)}


def format_skill_result(skill_name: str, result: Dict[str, Any]) -> str:
    """Formatta il risultato di una skill in testo leggibile per l'LLM."""
    if skill_name not in _SKILLS:
        return f"- {skill_name}: skill sconosciuta"

    if not result or not result.get("success", False):
        return f"- {skill_name}: ERRORE - {result.get('error', 'sconosciuto')}"

    skill = _SKILLS[skill_name]
    data = result.get("data", {})
    return f"- {skill.format_result(data)}"


def get_skills_prompt_section() -> str:
    """
    Genera automaticamente la sezione SKILL del system prompt.
    Chiamata una volta all'avvio o quando cambiano le skill.
    """
    if not _SKILLS:
        return ""

    lines = [
        "SKILL (dati in tempo reale, usa cmd=\"use_skill\"):"
    ]
    for skill in _SKILLS.values():
        lines.append(f"- {skill.name}: {skill.description}")
        if skill.params_schema:
            params = ", ".join(
                f"{k} ({v})" for k, v in skill.params_schema.items()
            )
            lines.append(f"  Params: {params}")
        lines.append(f"  Es: {skill.example}")

    return "\n".join(lines)


def get_registered_skills() -> list[str]:
    """Restituisce la lista dei nomi delle skill registrate."""
    return list(_SKILLS.keys())