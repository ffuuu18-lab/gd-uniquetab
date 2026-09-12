#!/usr/bin/env python3
"""Find the Grim Dawn installation without writing a path down.

Every tool in this folder that reads the game's own files goes through game_dir(). The order is:

    1. %GD_DIR%                       the explicit answer; always wins
    2. the Steam client's own registry key, plus every library folder Steam lists
    3. give up with a message that says what to set

Nothing here contains a drive letter or a user folder: the game is found, never written down.
"""

import os
import re
import sys

GAME_LEAF = os.path.join("steamapps", "common", "Grim Dawn")
_MARKER = os.path.join("x64", "Grim Dawn.exe")


def _steam_roots():
    """Every Steam library root this machine knows about, most likely first."""
    roots = []
    try:
        import winreg
    except ImportError:
        return roots

    for hive, key, value in (
        (winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam", "SteamPath"),
        (winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam", "InstallPath"),
        (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath"),
        (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Valve\Steam", "InstallPath"),
    ):
        try:
            with winreg.OpenKey(hive, key) as k:
                root = winreg.QueryValueEx(k, value)[0]
        except OSError:
            continue
        root = os.path.normpath(root.replace("/", os.sep))
        if root and root not in roots:
            roots.append(root)

    # Steam keeps the other drives' libraries in this file; "path" lines are all we need from it.
    for root in list(roots):
        vdf = os.path.join(root, "steamapps", "libraryfolders.vdf")
        try:
            with open(vdf, "r", encoding="utf-8", errors="replace") as fh:
                text = fh.read()
        except OSError:
            continue
        for m in re.finditer(r'"path"\s*"([^"]+)"', text):
            lib = os.path.normpath(m.group(1).replace("\\\\", "\\"))
            if lib and lib not in roots:
                roots.append(lib)
    return roots


def find_game_dir():
    """The folder holding x64\\Grim Dawn.exe, or None."""
    env = os.environ.get("GD_DIR")
    if env:
        return os.path.normpath(env.rstrip("\\/"))
    for root in _steam_roots():
        cand = os.path.join(root, GAME_LEAF)
        if os.path.exists(os.path.join(cand, _MARKER)):
            return cand
    return None


def game_dir(required=True):
    """find_game_dir(), but exits with a readable message when required and not found."""
    found = find_game_dir()
    if found or not required:
        return found
    sys.exit(
        "Grim Dawn was not found. Set GD_DIR to the folder that holds x64\\Grim Dawn.exe,\n"
        "for example:  set GD_DIR=<your Steam library>\\steamapps\\common\\Grim Dawn"
    )


if __name__ == "__main__":
    print(find_game_dir() or "not found")
