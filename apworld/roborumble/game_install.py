"""Finding RoboRumble (GOG or Steam), installing the game patch and starting the game.

The patch is a winmm.dll that goes next to the game exe in DATA. The apworld carries it (patch/winmm.dll and the
default settings patch/rrpatch.ini, packed in by tools/build_apworld.py).
"""
import os
import pkgutil
import re
import subprocess

from typing import List, Optional

STEAM_APP_ID = 420970
GOG_GAME_ID = "1947156754"

DLL = "winmm.dll"
INI = "rrpatch.ini"
GOG_WINMM_BACKUP = "winmm.dll.orig"   # GOG ships its own winmm.dll (an ogg player for the CD music)

RENDERERS = {"glide": "rr_glide.exe", "software": "rr_soft.exe", "direct3d": "rr_dx5.exe"}


# ---------------------------------------------------------------- finding the game
def _steam_library_paths() -> List[str]:
    try:
        import winreg

        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as key:
            steam_path: str = winreg.QueryValueEx(key, "SteamPath")[0]
    except (ImportError, OSError):
        return []
    paths = [steam_path]
    try:
        with open(os.path.join(steam_path, "steamapps", "libraryfolders.vdf"), encoding="utf-8") as f:
            paths += [p.replace("\\\\", "\\") for p in re.findall(r'"path"\s+"([^"]+)"', f.read())]
    except OSError:
        pass
    return paths


def _gog_path() -> Optional[str]:
    try:
        import winreg

        for view in (winreg.KEY_WOW64_32KEY, winreg.KEY_WOW64_64KEY):
            try:
                with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, rf"SOFTWARE\GOG.com\Games\{GOG_GAME_ID}",
                                    0, winreg.KEY_READ | view) as key:
                    return winreg.QueryValueEx(key, "path")[0]
            except OSError:
                continue
    except ImportError:
        pass
    return None


def is_game_path(game_path: str) -> bool:
    return os.path.isfile(os.path.join(game_path, "DATA", "rr_soft.exe"))


def find_game_path(configured_path: str = "") -> Optional[str]:
    """The configured folder, else the GOG install, else the Steam install."""
    if configured_path:
        return os.path.normpath(configured_path) if is_game_path(configured_path) else None
    gog = _gog_path()
    if gog and is_game_path(gog):
        return os.path.normpath(gog)
    for library in _steam_library_paths():
        path = os.path.join(library, "steamapps", "common", "RoBoRumble")
        if is_game_path(path):
            return os.path.normpath(path)
    return None


def is_gog(game_path: str) -> bool:
    return os.path.isfile(os.path.join(game_path, f"goggame-{GOG_GAME_ID}.info"))


def has_glide_wrapper(game_path: str) -> bool:
    """rr_glide.exe needs a Glide wrapper: GOG brings nGlide in DATA, on Steam it has to be installed."""
    if os.path.isfile(os.path.join(game_path, "DATA", "glide2x.dll")):
        return True
    windir = os.environ.get("WINDIR", r"C:\Windows")
    return any(os.path.isfile(os.path.join(windir, d, "glide2x.dll")) for d in ("SysWOW64", "System32"))


def default_renderer(game_path: str) -> str:
    return "glide" if has_glide_wrapper(game_path) else "software"


# ---------------------------------------------------------------- the patch
def _bundled(name: str) -> Optional[bytes]:
    try:
        return pkgutil.get_data(__name__, f"patch/{name}")
    except OSError:
        return None


def patch_available() -> bool:
    return _bundled(DLL) is not None


def is_patch_current(game_path: str) -> bool:
    data = _bundled(DLL)
    try:
        with open(os.path.join(game_path, "DATA", DLL), "rb") as f:
            return data is not None and f.read() == data
    except OSError:
        return False


def _set_ini_value(text: str, section: str, key: str, value: str) -> str:
    """Sets key=value in [section], adding the section or the key when missing."""
    lines = text.splitlines()
    start = next((i for i, l in enumerate(lines) if l.strip().lower() == f"[{section}]"), None)
    if start is None:
        return text.rstrip("\r\n") + f"\n\n[{section}]\n{key}={value}\n"
    end = next((i for i in range(start + 1, len(lines)) if lines[i].strip().startswith("[")), len(lines))
    for i in range(start + 1, end):
        if re.match(rf"\s*{re.escape(key)}\s*=", lines[i]):
            lines[i] = f"{key}={value}"
            return "\n".join(lines) + "\n"
    lines.insert(end, f"{key}={value}")
    return "\n".join(lines) + "\n"


def install_patch(game_path: str, port: int) -> None:
    """Copies the patch into DATA; keeps the player's rrpatch.ini but switches Archipelago on in it."""
    data_dir = os.path.join(game_path, "DATA")
    dll = _bundled(DLL)
    if dll is None:
        raise RuntimeError("this apworld has no game patch inside (built without patch/build/winmm.dll)")
    target = os.path.join(data_dir, DLL)
    backup = os.path.join(data_dir, GOG_WINMM_BACKUP)
    if os.path.isfile(target) and not os.path.isfile(backup):
        with open(target, "rb") as f:
            if f.read() != dll and is_gog(game_path):
                os.replace(target, backup)   # GOG's own ogg player, restored by remove_patch
    with open(target, "wb") as f:
        f.write(dll)

    ini_path = os.path.join(data_dir, INI)
    if os.path.isfile(ini_path):
        with open(ini_path, encoding="utf-8", errors="replace") as f:
            text = f.read()
    else:
        text = (_bundled(INI) or b"").decode("utf-8", errors="replace")
    text = _set_ini_value(text, "ap", "enabled", "1")
    text = _set_ini_value(text, "ap", "port", str(port))
    with open(ini_path, "w", encoding="utf-8") as f:
        f.write(text)


def remove_patch(game_path: str) -> None:
    data_dir = os.path.join(game_path, "DATA")
    target = os.path.join(data_dir, DLL)
    if os.path.isfile(target):
        os.remove(target)
    backup = os.path.join(data_dir, GOG_WINMM_BACKUP)
    if os.path.isfile(backup):
        os.replace(backup, target)


# ---------------------------------------------------------------- starting the game
def launch(game_path: str, renderer: str) -> subprocess.Popen:
    data_dir = os.path.join(game_path, "DATA")
    exe = os.path.join(data_dir, RENDERERS[renderer])
    env = dict(os.environ)
    # GOG's compatibility database marks the exes "run as administrator", which they don't need
    env["__COMPAT_LAYER"] = "RunAsInvoker"
    return subprocess.Popen([exe], cwd=data_dir, env=env, creationflags=getattr(subprocess, "DETACHED_PROCESS", 0))
