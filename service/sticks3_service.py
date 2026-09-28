#!/usr/bin/env python3
# ------------------------------------------------------------
# sticks3_service.py — le cerveau PC du StickS3.
#
# Deux oreilles :
#   1. Serveur TCP (port 8787) : le stick envoie ses dictées (WAV),
#      on transcrit via API et on colle le texte dans la fenêtre active.
#   2. Serveur HTTP (port 8788, localhost) : le plugin OpenCode
#      prévient qu'une IA a fini → on pousse la sonnette au stick.
#
# Lancement :  python sticks3_service.py
# Config :     config.json (copié de config.example.json)
# Clé API :    variable d'environnement STT_API_KEY (ou OPENAI_API_KEY)
# ------------------------------------------------------------
from __future__ import annotations

import json
import re
import socket
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from paste import paste_text, send_chord
from transcribe import transcribe

BASE = Path(__file__).resolve().parent
CONFIG_PATH = BASE / "config.json"

# Types de trame (cf. firmware/src/net.h)
MSG_HELLO = 0x01
MSG_WAV = 0x02
MSG_NOTIFY = 0x03
MSG_TRANSCRIPT = 0x04
MSG_ENTER = 0x05

MAX_WAV = 8 * 1024 * 1024  # 8 Mo : large pour 60 s de 16 kHz mono
DISCOVERY_REQUEST = b"STICKS3_DISCOVER_V1"
DISCOVERY_RESPONSE = b"STICKS3_HERE_V1"

# Nom affiché par le Stick : lettres, chiffres et tirets uniquement.
PC_NAME = re.sub(r"[^A-Za-z0-9-]", "", socket.gethostname())[:16] or "PC"

# Corrections locales après STT : elles ne touchent que des mots entiers pour
# ne pas altérer le reste d'une dictée. Les moteurs de transcription hésitent
# fréquemment sur ce prénom, surtout avec la prononciation française.
NORI_VARIANTS = re.compile(
    r"\b(?:nori|nory|nouri|noury|nauri|naury|nozri|nozry)\b", re.IGNORECASE
)


def normalize_transcript(text: str) -> str:
    return NORI_VARIANTS.sub("Nori", text)


def load_config() -> dict:
    if CONFIG_PATH.exists():
        with open(CONFIG_PATH, encoding="utf-8") as fh:
            return json.load(fh)
    example = BASE / "config.example.json"
    with open(example, encoding="utf-8") as fh:
        return json.load(fh)


CFG = load_config()


def log(msg: str) -> None:
    line = f"[{time.strftime('%H:%M:%S')}] {msg}"
    try:
        print(line, flush=True)
    except UnicodeEncodeError:
        # console restrictive (cp1252 & co) : on dégrade au lieu de
        # planter — un log ne doit jamais faire tomber une requête
        print(line.encode("ascii", "replace").decode("ascii"), flush=True)


# ------------------------------------------------------------
# Trames
# ------------------------------------------------------------
def read_exact(sock: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("connexion fermée")
        buf.extend(chunk)
    return bytes(buf)


def read_frame(sock: socket.socket) -> tuple[int, bytes]:
    hdr = read_exact(sock, 5)
    (length,) = struct.unpack("<I", hdr[:4])
    typ = hdr[4]
    if length > MAX_WAV:
        raise ValueError(f"trame trop grosse ({length} octets)")
    payload = read_exact(sock, length) if length else b""
    return typ, payload


def send_frame(sock: socket.socket, typ: int, payload: bytes) -> None:
    sock.sendall(struct.pack("<I", len(payload)) + bytes([typ]) + payload)


def hello_parts(payload: bytes) -> tuple[str, str]:
    """HELLO : nom \t firmware \t jeton. Le jeton est optionnel côté Stick."""
    fields = payload.decode("utf-8", "replace").split("\t")
    name = fields[0].strip() if fields else ""
    token = fields[2].strip() if len(fields) > 2 else ""
    return name, token


# ------------------------------------------------------------
# Gestion des sticks connectés
# ------------------------------------------------------------
class StickHub:
    """Retient les sticks connectés pour pouvoir leur crier dessus."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._socks: dict[str, socket.socket] = {}

    def register(self, name: str, sock: socket.socket) -> None:
        with self._lock:
            self._socks[name] = sock
        log(f"stick connecté : {name}")

    def unregister(self, name: str) -> None:
        with self._lock:
            self._socks.pop(name, None)
        log(f"stick déconnecté : {name}")

    def count(self) -> int:
        with self._lock:
            return len(self._socks)

    def broadcast_notify(self, title: str, message: str, sound: str) -> int:
        payload = f"{title}\t{message}\t{sound}".encode("utf-8")
        sent = 0
        with self._lock:
            dead = []
            for name, sock in self._socks.items():
                try:
                    send_frame(sock, MSG_NOTIFY, payload)
                    sent += 1
                except OSError:
                    dead.append(name)
            for name in dead:
                self._socks.pop(name, None)
        return sent


HUB = StickHub()


# ------------------------------------------------------------
# Traitement d'une dictée : WAV → texte → collage
# ------------------------------------------------------------
def handle_wav(sock: socket.socket, wav: bytes, name: str) -> None:
    def reply(status: str, message: str, excerpt: str = "") -> None:
        payload = f"{status}\t{message}\t{excerpt}".encode("utf-8")
        try:
            send_frame(sock, MSG_TRANSCRIPT, payload)
        except OSError:
            pass

    # la réponse part AVANT tout log : un souci d'affichage ne doit
    # jamais couper la parole au stick

    if len(wav) < 44 or wav[:4] != b"RIFF":
        reply("error", "wav invalide")
        return

    duration = (len(wav) - 44) / (16000 * 2)
    log(f"[{name}] dictée reçue : {duration:.1f}s, transcription...")

    try:
        text = transcribe(wav, CFG)
    except Exception as exc:  # API absente, réseau, clé manquante...
        log(f"[{name}] transcription KO : {exc}")
        reply("error", str(exc)[:120])
        return

    if not text:
        log(f"[{name}] transcription vide")
        reply("error", "rien entendu", "")
        return

    text = normalize_transcript(text)

    log(f"[{name}] texte : {text[:80]}{'...' if len(text) > 80 else ''}")

    # On aplatit les retours ligne : dans un prompt OpenCode, un
    # Entrée ferait partir le message à moitié.
    flat = " ".join(text.split())

    try:
        how = paste_text(
            # Chaque fragment laisse le curseur prêt pour le suivant :
            # « bonjour » puis « Nori » devient « bonjour Nori ».
            flat + " ",
            mode=CFG.get("paste_mode", "clipboard"),
            shortcut=CFG.get("paste_shortcut", "ctrl+v"),
            delay_ms=CFG.get("paste_delay_ms", 60),
        )
    except Exception as exc:
        log(f"[{name}] collage KO : {exc}")
        reply("error", f"collage : {exc}"[:120])
        return

    log(f"[{name}] {how}")
    reply("ok", how, flat[:60])


def handle_stick(sock: socket.socket, addr) -> None:
    name = f"{addr[0]}"
    try:
        sock.settimeout(None)
        typ, payload = read_frame(sock)
        if typ != MSG_HELLO:
            log(f"[{name}] trame inattendue au lieu du HELLO : 0x{typ:02x}")
            return
        stick_name, token = hello_parts(payload)
        expected = str(CFG.get("service_token", "")).strip()
        if expected and token != expected:
            # Sans bon jeton : aucun enregistrement, aucune discussion.
            # Le service n'a aucune raison de justifier sa présence.
            log(f"[{stick_name or name}] connexion refusee : jeton invalide")
            return
        if stick_name:
            name = stick_name
        HUB.register(name, sock)

        while True:
            typ, payload = read_frame(sock)
            if typ == MSG_WAV:
                handle_wav(sock, payload, name)
            elif typ == MSG_ENTER:
                # Commande fixe, sans payload : le Stick ne peut pas devenir
                # un relais de frappes arbitraires sur le réseau.
                try:
                    send_chord("enter")
                    log(f"[{name}] entrée")
                except Exception as exc:
                    log(f"[{name}] entrée KO : {exc}")
            elif typ == MSG_HELLO:
                pass  # déjà fait
            else:
                log(f"[{name}] trame inattendue : 0x{typ:02x}")
    except (ConnectionError, OSError, ValueError) as exc:
        log(f"[{name}] coupure : {exc}")
    finally:
        HUB.unregister(name)
        try:
            sock.close()
        except OSError:
            pass


def tcp_server(host: str, port: int) -> None:
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(4)
    # Sous Windows, accept() sans timeout peut retenir Ctrl+C indéfiniment.
    # Le réveil périodique laisse Python traiter KeyboardInterrupt.
    srv.settimeout(0.5)
    log(f"TCP stick prêt sur {host}:{port}")
    try:
        while True:
            try:
                sock, addr = srv.accept()
            except socket.timeout:
                continue
            threading.Thread(
                target=handle_stick, args=(sock, addr), daemon=True
            ).start()
    finally:
        srv.close()


# ------------------------------------------------------------
# Découverte réseau : le Stick diffuse une requête UDP, le service répond
# directement à son adresse avec le port TCP à joindre. Aucune IP locale
# n'est donc inscrite dans le firmware.
def discovery_server(port: int, tcp_port: int) -> None:
    srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("", port))
    srv.settimeout(0.5)
    log(f"découverte Stick prête en UDP :{port}")
    try:
        while True:
            try:
                payload, addr = srv.recvfrom(256)
            except socket.timeout:
                continue
            if payload == DISCOVERY_REQUEST:
                # Le nom du PC permet au Stick d'afficher à qui il parle.
                srv.sendto(
                    f"{DISCOVERY_RESPONSE.decode()} {tcp_port} {PC_NAME}".encode(),
                    addr,
                )
    finally:
        srv.close()


# ------------------------------------------------------------
# Serveur HTTP : notifications OpenCode + endpoints de debug
# ------------------------------------------------------------
class NotifyHandler(BaseHTTPRequestHandler):
    def _json(self, code: int, obj: dict) -> None:
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:  # noqa: N802
        if self.path == "/health":
            self._json(200, {"ok": True, "sticks": HUB.count(), "discovery": True})
        else:
            self._json(404, {"error": "inconnu"})

    def do_POST(self) -> None:  # noqa: N802
        if self.path not in ("/notify", "/test"):
            self._json(404, {"error": "inconnu"})
            return
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length) if length else b"{}"
        try:
            body = json.loads(raw or b"{}")
        except json.JSONDecodeError:
            self._json(400, {"error": "json invalide"})
            return

        title = str(body.get("title", "C'EST PRET !"))
        message = str(body.get("message", ""))
        sound = str(body.get("sound", "frites"))

        sent = HUB.broadcast_notify(title, message, sound)
        self._json(200, {"ok": True, "sticks": sent})
        log(f"notif '{title}' -> {sent} stick(s)")

    def log_message(self, *args) -> None:  # silence du logger HTTP par défaut
        pass


def http_server(host: str, port: int) -> None:
    srv = ThreadingHTTPServer((host, port), NotifyHandler)
    log(f"HTTP notif prêt sur http://{host}:{port}/notify")
    srv.serve_forever()


# ------------------------------------------------------------
def main() -> None:
    log("=== service StickS3 ===")
    log(f"config : {CONFIG_PATH if CONFIG_PATH.exists() else 'config.example.json (défaut)'}")

    try:
        from transcribe import resolve_api_key

        if resolve_api_key():
            log("clé API : trouvée dans l'environnement")
        else:
            log("clé API : ABSENTE — définis STT_API_KEY avant de dicter")
    except Exception:
        pass

    if str(CFG.get("service_token", "")).strip():
        log("authentification stick : activée")
    else:
        log("authentification stick : DÉSACTIVÉE (service_token vide)")

    threading.Thread(
        target=http_server,
        args=(CFG.get("notify_host", "127.0.0.1"), CFG.get("notify_port", 8788)),
        daemon=True,
    ).start()

    tcp_port = CFG.get("listen_port", 8787)
    threading.Thread(
        target=discovery_server,
        args=(CFG.get("discovery_port", 8789), tcp_port),
        daemon=True,
    ).start()

    # mode test à distance : préviens le stick sans OpenCode
    if "--test-notify" in sys.argv:
        def fire() -> None:
            time.sleep(1)
            sent = HUB.broadcast_notify("C'EST PRET !", "test manuel", "frites")
            log(f"test notif -> {sent} stick(s)")

        threading.Thread(target=fire, daemon=True).start()

    tcp_server(CFG.get("listen_host", "0.0.0.0"), tcp_port)


if __name__ == "__main__":
    main()
