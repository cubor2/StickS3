# ------------------------------------------------------------
# transcribe.py — transcription vocale via une API compatible OpenAI.
# Marche avec OpenAI, mais aussi tout endpoint compatible
# (Groq, Azure OpenAI compatible, proxy local, etc.).
# ------------------------------------------------------------
from __future__ import annotations

import os
import requests


def resolve_api_key() -> str:
    """La clé vient de l'environnement — jamais d'un fichier commité."""
    for var in ("STT_API_KEY", "OPENAI_API_KEY"):
        val = os.environ.get(var, "").strip()
        if val:
            return val
    return ""


def transcribe(wav_bytes: bytes, cfg: dict) -> str:
    """
    Envoie le WAV à l'API et renvoie le texte transcrit.
    Lève une exception en cas de souci réseau/API.
    """
    base = cfg.get("stt_base_url", "https://api.openai.com/v1").rstrip("/")
    model = cfg.get("stt_model", "gpt-4o-mini-transcribe")
    language = cfg.get("stt_language", "fr")
    prompt = cfg.get("stt_prompt", "")

    api_key = resolve_api_key()
    if not api_key:
        raise RuntimeError(
            "Clé API absente. Définis STT_API_KEY (ou OPENAI_API_KEY) dans l'environnement."
        )

    headers = {"Authorization": f"Bearer {api_key}"}
    files = {"file": ("clip.wav", wav_bytes, "audio/wav")}
    data: dict[str, str] = {
        "model": model,
        "response_format": "json",
    }
    if language:
        data["language"] = language
    if prompt:
        data["prompt"] = prompt

    resp = requests.post(
        f"{base}/audio/transcriptions",
        headers=headers,
        files=files,
        data=data,
        timeout=120,
    )
    if resp.status_code != 200:
        raise RuntimeError(f"API {resp.status_code} : {resp.text[:200]}")

    body = resp.json()
    text = (body.get("text") or "").strip()
    return text
