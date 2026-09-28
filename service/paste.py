# ------------------------------------------------------------
# paste.py — injection de texte dans la fenêtre active (Windows).
# Zéro dépendance : ctypes + user32.
#
# Deux modes :
#   clipboard : met le texte dans le presse-papiers et simule Ctrl+V
#   type      : tape les caractères un par un (Unicode SendInput)
# ------------------------------------------------------------
from __future__ import annotations

import ctypes
import time
from ctypes import wintypes

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

# ctypes suppose sinon un retour `int` 32 bits. Sur Windows 64 bits,
# GlobalLock renvoie un pointeur : sans ces signatures l'adresse est tronquée
# et GlobalLock semble échouer alors que l'allocation est bonne.
kernel32.GlobalAlloc.argtypes = (wintypes.UINT, ctypes.c_size_t)
kernel32.GlobalAlloc.restype = wintypes.HANDLE
kernel32.GlobalLock.argtypes = (wintypes.HANDLE,)
kernel32.GlobalLock.restype = ctypes.c_void_p
kernel32.GlobalUnlock.argtypes = (wintypes.HANDLE,)
kernel32.GlobalUnlock.restype = wintypes.BOOL
kernel32.GlobalFree.argtypes = (wintypes.HANDLE,)
kernel32.GlobalFree.restype = wintypes.HANDLE

user32.OpenClipboard.argtypes = (wintypes.HWND,)
user32.OpenClipboard.restype = wintypes.BOOL
user32.EmptyClipboard.argtypes = ()
user32.EmptyClipboard.restype = wintypes.BOOL
user32.SetClipboardData.argtypes = (wintypes.UINT, wintypes.HANDLE)
user32.SetClipboardData.restype = wintypes.HANDLE
user32.CloseClipboard.argtypes = ()
user32.CloseClipboard.restype = wintypes.BOOL

CF_UNICODETEXT = 13
GMEM_MOVEABLE = 0x0002
INPUT_KEYBOARD = 1
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_UNICODE = 0x0004
VK_CONTROL = 0x11
VK_SHIFT = 0x10
VK_MENU = 0x12  # Alt
VK_RETURN = 0x0D

ULONG_PTR = ctypes.c_size_t


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [
        ("dx", wintypes.LONG),
        ("dy", wintypes.LONG),
        ("mouseData", wintypes.DWORD),
        ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ULONG_PTR),
    ]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [
        ("wVk", wintypes.WORD),
        ("wScan", wintypes.WORD),
        ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ULONG_PTR),
    ]


class HARDWAREINPUT(ctypes.Structure):
    _fields_ = [
        ("uMsg", wintypes.DWORD),
        ("wParamL", wintypes.WORD),
        ("wParamH", wintypes.WORD),
    ]


class _INPUTUNION(ctypes.Union):
    _fields_ = [
        ("mi", MOUSEINPUT),
        ("ki", KEYBDINPUT),
        ("hi", HARDWAREINPUT),
    ]


class INPUT(ctypes.Structure):
    _anonymous_ = ("u",)
    _fields_ = [
        ("type", wintypes.DWORD),
        ("u", _INPUTUNION),
    ]


def _key_event(vk: int, up: bool = False) -> INPUT:
    inp = INPUT()
    inp.type = INPUT_KEYBOARD
    inp.ki.wVk = vk
    inp.ki.dwFlags = KEYEVENTF_KEYUP if up else 0
    return inp


def _unicode_event(code_unit: int, up: bool = False) -> INPUT:
    inp = INPUT()
    inp.type = INPUT_KEYBOARD
    inp.ki.wVk = 0
    inp.ki.wScan = code_unit
    inp.ki.dwFlags = KEYEVENTF_UNICODE | (KEYEVENTF_KEYUP if up else 0)
    return inp


def _send(inputs: list[INPUT]) -> None:
    n = len(inputs)
    arr = (INPUT * n)(*inputs)
    sent = user32.SendInput(n, arr, ctypes.sizeof(INPUT))
    if sent != n:
        raise OSError(f"SendInput a échoué ({ctypes.get_last_error()})")


def set_clipboard(text: str) -> None:
    """Place `text` dans le presse-papiers Windows (Unicode)."""
    data = text.encode("utf-16-le") + b"\x00\x00"
    h_global = kernel32.GlobalAlloc(GMEM_MOVEABLE, len(data))
    if not h_global:
        raise OSError("GlobalAlloc a échoué")
    p = kernel32.GlobalLock(h_global)
    if not p:
        kernel32.GlobalFree(h_global)
        raise OSError("GlobalLock a échoué")
    ctypes.memmove(p, data, len(data))
    kernel32.GlobalUnlock(h_global)

    if not user32.OpenClipboard(None):
        kernel32.GlobalFree(h_global)
        raise OSError("OpenClipboard a échoué — une autre appli garde le presse-papiers ?")
    try:
        user32.EmptyClipboard()
        if not user32.SetClipboardData(CF_UNICODETEXT, h_global):
            kernel32.GlobalFree(h_global)
            raise OSError("SetClipboardData a échoué")
        # le presse-papiers est propriétaire de h_global à partir d'ici
    finally:
        user32.CloseClipboard()


_VK = {
    "ctrl": VK_CONTROL,
    "control": VK_CONTROL,
    "shift": VK_SHIFT,
    "alt": VK_MENU,
    "v": ord("V"),
    "insert": 0x2D,
    "enter": VK_RETURN,
}


def send_chord(spec: str = "ctrl+v") -> None:
    """Simule un raccourci, ex. « ctrl+v », « ctrl+shift+v », « insert »."""
    keys = [k.strip().lower() for k in spec.split("+") if k.strip()]
    vks = []
    for k in keys:
        if k not in _VK:
            raise ValueError(f"Touche inconnue dans le raccourci : {k}")
        vks.append(_VK[k])

    events: list[INPUT] = []
    for vk in vks:
        events.append(_key_event(vk, up=False))
    for vk in reversed(vks):
        events.append(_key_event(vk, up=True))
    _send(events)


def type_text(text: str) -> None:
    """Tape le texte caractère par caractère (Unicode, gère les accents)."""
    # utf-16-le : on envoie chaque code unit, y compris les paires de substitution
    units = text.encode("utf-16-le")
    for i in range(0, len(units), 2):
        cu = units[i] | (units[i + 1] << 8)
        _send([_unicode_event(cu, up=False), _unicode_event(cu, up=True)])


def paste_text(
    text: str,
    mode: str = "clipboard",
    shortcut: str = "ctrl+v",
    delay_ms: int = 60,
) -> str:
    """
    Injecte `text` dans la fenêtre active. Retourne un résumé lisible.
    mode : "clipboard" (raccourci de collage) ou "type" (frappe directe).
    """
    if not text:
        return "texte vide"
    delay = max(0, delay_ms) / 1000.0

    if mode == "type":
        time.sleep(delay)
        type_text(text)
        return "frappé au clavier"

    set_clipboard(text)
    time.sleep(delay)  # laisse le presse-papiers se poser
    send_chord(shortcut)
    return f"collé ({shortcut})"
