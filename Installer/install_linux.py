#!/usr/bin/env python3
"""Crysis Coop - installer for Linux (the game running under Wine).

Does what install.ps1 does on Windows, for a Crysis folder on Linux:

  1. checks the game (Crysis 1.2.1, build 6156) and C1-Launcher
     (Bin32/Crysis.exe; GOG's current build already ships it)
  2. copies the mod to Mods/Coop
  3. builds the co-op versions of the 11 campaign levels from the game's own
     level files (hard links: almost no extra disk space; the original files
     are never modified)
  4. writes launch_crysis_coop.sh next to the game (Wine, the game's Wine
     prefix, CrysisCoop.exe) and a "Crysis Coop" menu and desktop entry

Run it from the extracted release archive (the folder with Mods/Coop in it):

  python3 install_linux.py [--game ~/Games/Crysis] [--prefix ~/Games/wine-crysis] [--force]

  --game      the Crysis folder (the one with Bin32 and Game in it)
  --prefix    the Wine prefix the game runs in (default: taken from an
              existing launch script, WINEPREFIX, or ~/.wine)
  --force     rebuild the co-op levels even if they exist
  --no-menu   no menu and desktop entries
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys

CAMPAIGN = {
    "island": "Contact", "village": "Recovery", "rescue": "Relic", "harbor": "Assault", "tank": "Onslaught",
    "mine": "Awakening", "core": "Core", "ice": "Paradise Lost", "sphere": "Exodus", "ascension": "Ascension",
    "fleet": "Reckoning",
}
HERE = os.path.dirname(os.path.abspath(__file__))


def say(text=""):
    print(text, flush=True)


def fail(text):
    say("")
    say("ERROR: " + text)
    sys.exit(1)


def child(path, name):
    """path/name, with the name matched without regard to case (Wine games
    come with any case: Game/Levels/core/Core.cry)"""
    exact = os.path.join(path, name)
    if os.path.exists(exact):
        return exact
    try:
        for entry in os.listdir(path):
            if entry.lower() == name.lower():
                return os.path.join(path, entry)
    except OSError:
        pass
    return exact


def game_path(game, *parts):
    path = game
    for p in parts:
        path = child(path, p)
    return path


def is_crysis(path):
    return path and os.path.isfile(game_path(path, "Bin32", "CrySystem.dll")) and \
        os.path.isdir(game_path(path, "Game", "Levels", "island"))


def find_crysis():
    home = os.path.expanduser("~")
    candidates = [os.path.join(HERE, ".."), os.path.join(HERE, "..", ".."), HERE,
                  os.path.join(home, "Games", "Crysis"), os.path.join(home, "Games", "crysis")]
    # Wine prefixes: ~/.wine, Lutris/Bottles/Heroic style folders, Steam (Proton)
    roots = [os.path.join(home, ".wine")]
    for base in (os.path.join(home, "Games"), os.path.join(home, ".local", "share", "bottles", "bottles"),
                 os.path.join(home, ".local", "share", "Steam", "steamapps", "compatdata"),
                 os.path.join(home, ".steam", "steam", "steamapps", "compatdata")):
        try:
            roots += [os.path.join(base, d) for d in os.listdir(base)]
        except OSError:
            pass
    for root in roots:
        drive = os.path.join(root, "drive_c")
        if not os.path.isdir(drive):
            drive = os.path.join(root, "pfx", "drive_c")
        for sub in ("GOG Games/Crysis", "Program Files (x86)/Electronic Arts/Crytek/Crysis",
                    "Program Files (x86)/Steam/steamapps/common/Crysis", "Program Files/Crysis", "Games/Crysis"):
            candidates.append(os.path.join(drive, sub))
    for steam in (os.path.join(home, ".local", "share", "Steam"), os.path.join(home, ".steam", "steam")):
        candidates.append(os.path.join(steam, "steamapps", "common", "Crysis"))
    for c in candidates:
        if is_crysis(c):
            return os.path.realpath(c)
    return None


def find_prefix(game):
    """the Wine prefix of an existing launch script, else WINEPREFIX, else
    the prefix the game folder is in, else ~/.wine"""
    for name in os.listdir(game):
        if name.endswith(".sh") and name != "launch_crysis_coop.sh":
            try:
                text = open(os.path.join(game, name), encoding="utf-8", errors="replace").read()
            except OSError:
                continue
            m = re.search(r'(?:WINEPREFIX|PREFIX)="?([^"\n]+)"?', text)
            if m and "$" not in m.group(1) and os.path.isdir(os.path.expanduser(m.group(1))):
                return os.path.expanduser(m.group(1)), name
    if os.environ.get("WINEPREFIX"):
        return os.environ["WINEPREFIX"], None
    parts = game.split(os.sep)
    if "drive_c" in parts:
        return os.sep.join(parts[:parts.index("drive_c")]), None
    return os.path.expanduser("~/.wine"), None


def build_level(game, levels_dir, level, force):
    src = game_path(game, "Game", "Levels", level)
    if not os.path.isfile(child(src, level + ".cry")):
        say("   %s : not in this game, skipped" % level)
        return
    dst = os.path.join(levels_dir, "coop_" + level)
    if os.path.isfile(os.path.join(dst, "coop_%s.xml" % level)) and not force:
        say("   %s : already there" % level)
        return
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    os.makedirs(dst)
    pretty = "Coop: " + CAMPAIGN[level]
    for folder, _, files in os.walk(src):
        for f in files:
            full = os.path.join(folder, f)
            rel = os.path.relpath(full, src)
            name = rel
            if rel.lower() == level + ".cry":
                name = "coop_%s.cry" % level
            if rel.lower() == level + ".xml":
                # the only file that changes: a real copy, never a link to the original
                xml = open(full, "rb").read().decode("utf-8-sig", errors="surrogateescape")
                xml = re.sub(r"<Gamerules[^>]*/>", '<Gamerules MP1="TeamInstantAction"/>', xml)
                if re.search(r"<Display[^>]*/>", xml):
                    xml = re.sub(r"<Display[^>]*/>", '<Display Name="%s"/>' % pretty, xml)
                else:
                    xml = xml.replace("</MetaData>", '\t<Display Name="%s"/>\r\n</MetaData>' % pretty)
                xml = re.sub(r"<HeaderText[^>]*/>", '<HeaderText text="%s"/>' % pretty, xml)
                with open(os.path.join(dst, "coop_%s.xml" % level), "wb") as out:
                    out.write(xml.encode("utf-8", errors="surrogateescape"))
                continue
            target = os.path.join(dst, name)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            try:
                os.link(full, target)
            except OSError:
                shutil.copy2(full, target)
    say("   %s (%s): ready" % (level, CAMPAIGN[level]))


LAUNCH_SCRIPT = """#!/bin/bash
# Crysis Coop: the game with the co-op mod, under Wine (written by the mod's
# install_linux.py; the environment is the one of %(based_on)s)
GAME_DIR=%(game)s
export WINEPREFIX=%(prefix)s
%(env)scd "$GAME_DIR"
exec wine CrysisCoop.exe "$@"
"""

DESKTOP = """[Desktop Entry]
Version=1.0
Type=Application
Name=Crysis Coop
GenericName=First-Person Shooter
Comment=The Crysis campaign in co-op (Wine)
Exec=%(script)s
Icon=%(icon)s
Path=%(game)s
Terminal=false
Categories=Game;ActionGame;
StartupNotify=true
"""


def quote(s):
    return "'" + s.replace("'", "'\\''") + "'"


def main():
    ap = argparse.ArgumentParser(description="Crysis Coop installer for Linux (Wine)")
    ap.add_argument("--game")
    ap.add_argument("--prefix")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--no-menu", action="store_true")
    args = ap.parse_args()

    say("Crysis Coop installer (Linux, Wine)")
    say("This mod is not endorsed by or affiliated with Crytek or Electronic Arts.")
    say("")
    say("== Looking for Crysis")
    game = os.path.realpath(os.path.expanduser(args.game)) if args.game else find_crysis()
    if not is_crysis(game):
        fail("Crysis was not found%s. Pass the folder with Bin32 and Game in it: --game <path>" %
             (" in " + args.game if args.game else ""))
    say("   " + game)
    system = open(game_path(game, "Bin32", "CrySystem.dll"), "rb").read()
    if "6156".encode("utf-16-le") not in system:
        fail("the mod needs Crysis 1.2.1 (build 6156). GOG, Steam and EA versions are 1.2.1 already; "
             "a DVD version needs the official patches 1.2 and 1.2.1.")
    say("   version 1.2.1 (build 6156)")
    if b"C1-Launcher" not in open(game_path(game, "Bin32", "Crysis.exe"), "rb").read():
        fail("Bin32/Crysis.exe is not C1-Launcher, which the mod needs. Put Bin32/Crysis.exe of "
             "https://github.com/ccomrade/c1-launcher/releases (v7) there, keeping the original, and run this again.")
    say("   C1-Launcher: yes")

    say("")
    say("== Mod files")
    payload = os.path.join(HERE, "Mods", "Coop")
    if not os.path.isfile(os.path.join(payload, "Bin32", "Coop.dll")):
        fail("Mods/Coop/Bin32/Coop.dll is not next to this script: run it from the extracted release archive.")
    mods = child(game, "Mods")
    mod = os.path.join(mods, "Coop")
    if os.path.realpath(payload) != os.path.realpath(mod):
        shutil.copytree(payload, mod, dirs_exist_ok=True)
    launcher = os.path.join(game, "CrysisCoop.exe")
    shutil.copy2(os.path.join(mod, "CrysisCoop.exe"), launcher)
    say("   installed to " + mod)

    say("")
    say("== Co-op levels")
    levels_dir = os.path.join(mod, "Game", "Levels", "Multiplayer", "TIA")
    os.makedirs(levels_dir, exist_ok=True)
    for level in CAMPAIGN:
        build_level(game, levels_dir, level, args.force)

    say("")
    say("== Launcher")
    prefix, based_on = (os.path.expanduser(args.prefix), None) if args.prefix else find_prefix(game)
    text = None
    if based_on:
        # the game's own launch script (its Wine prefix, DXVK overrides and
        # the rest) with the game started through CrysisCoop.exe instead
        lines = open(os.path.join(game, based_on), encoding="utf-8", errors="replace").read().splitlines()
        runs = [i for i, line in enumerate(lines) if re.search(r"\bwine\S*\s+\S*crysis\.exe", line, re.I)]
        if len(runs) == 1:
            lines[runs[0]] = 'cd %s\nexec wine CrysisCoop.exe "$@"' % quote(game)
            if lines and lines[0].startswith("#!"):
                lines.insert(1, "# Crysis Coop: %s with the co-op mod (written by the mod's install_linux.py)" % based_on)
            text = "\n".join(lines) + "\n"
    if text is None:
        env = ""
        if based_on:
            for line in open(os.path.join(game, based_on), encoding="utf-8", errors="replace"):
                if re.match(r"\s*export\s+(WINE|DXVK|VKD3D|MESA|__GL|PROTON)", line) and "WINEPREFIX" not in line:
                    env += line.strip() + "\n"
        text = LAUNCH_SCRIPT % {"game": quote(game), "prefix": quote(prefix), "env": env,
                                "based_on": based_on or "WINEPREFIX=" + prefix}
    script = os.path.join(game, "launch_crysis_coop.sh")
    with open(script, "w", encoding="utf-8") as out:
        out.write(text)
    os.chmod(script, 0o755)
    say("   " + script + "  (Wine prefix " + prefix + ")")
    check = subprocess.run(["wine", launcher, "-coop_check"], env=dict(os.environ, WINEPREFIX=prefix, WINEDEBUG="-all"),
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    why = {2: "Crysis not found", 3: "Mods/Coop/Bin32/Coop.dll missing", 4: "co-op levels missing",
           5: "Bin32/Crysis.exe is not C1-Launcher"}
    if check.returncode != 0:
        fail("the launcher's check failed: %s (code %d)" % (why.get(check.returncode, "wine"), check.returncode))
    say("   checked with Wine: ready")

    if not args.no_menu:
        say("")
        say("== Menu and desktop")
        icon = os.path.join(mod, "CrysisCoop.png")
        if not os.path.isfile(icon):
            icon = "crysis"
        entry = DESKTOP % {"script": script, "icon": icon, "game": game}
        places = [os.path.expanduser("~/.local/share/applications")]
        try:
            desktop = subprocess.run(["xdg-user-dir", "DESKTOP"], capture_output=True, text=True).stdout.strip()
        except OSError:
            desktop = ""
        if desktop and os.path.isdir(desktop) and os.path.realpath(desktop) != os.path.expanduser("~"):
            places.append(desktop)
        for place in places:
            os.makedirs(place, exist_ok=True)
            path = os.path.join(place, "crysis-coop.desktop")
            with open(path, "w", encoding="utf-8") as out:
                out.write(entry)
            os.chmod(path, 0o755)
            # desktops run a desktop file only once it is trusted: GNOME's
            # flag, and Xfce's checksum of the file
            digest = hashlib.sha256(entry.encode("utf-8")).hexdigest()
            for key, kind, value in (("metadata::trusted", "string", "true"),
                                     ("metadata::xfce-exe-checksum", "string", digest)):
                subprocess.run(["gio", "set", "-t", kind, path, key, value],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            say("   " + path)

    say("")
    say("Done. How to play:")
    say("  Start 'Crysis Coop' (or %s)." % script)
    say("  Host:    Multiplayer (the 2nd item of the main menu) > Co-op game > New campaign or Continue.")
    say("  Friend:  Multiplayer > Co-op game > Join a friend, the host's code, Join.")
    say("  Remove:  delete %s, %s and %s" % (mod, launcher, script))


if __name__ == "__main__":
    main()
