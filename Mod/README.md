# Crysis Coop

The Crysis (2007) single player campaign for 2–4 players. The host plays the
campaign as usual: enemies, story, cutscenes. Friends join him over the
internet with a short code. Nobody has to open ports on a router.

*Русская версия: [README_RU.md](README_RU.md)*

## Requirements

* Crysis (2007) version 1.2.1: GOG, Steam and EA are fine. A DVD copy needs the official patches 1.2 and 1.2.1.
* Every player needs their own copy of the game and the mod.
* Windows 8 or later.

## Installation

Run `install.bat` from the release archive. The installer:

* finds the game;
* installs [C1-Launcher](https://github.com/ccomrade/c1-launcher) if needed (the original `Bin32\Crysis.exe` is kept as `Crysis.exe.original`);
* copies the mod to `Mods\Coop`;
* builds the co-op versions of the campaign levels from your own game files (almost no extra disk space; the original files are not changed);
* creates a **Crysis Coop** shortcut on the desktop.

Without downloading the archive first (PowerShell):

```powershell
powershell -ExecutionPolicy Bypass -c "irm https://github.com/SeniorStackOverflow/crysis-coop/releases/latest/download/install.ps1 | iex"
```

## How to play

**Host:** start **Crysis Coop**, open the console (`~`) and type

```
coop_host
```

The campaign starts from the first level. To start from another level (co-op has no saved games), give its name:
`coop_host village`. The levels are island, village, rescue, harbor, tank, mine, core, ice, sphere, ascension and fleet (or `1`–`11`).

When the level has loaded, a code appears on the screen, for example `Co-op code: 64117`.

**Friend:** start **Crysis Coop**, open the console and type the host's code:

```
coop_join 64117
```

The friend appears next to the host once the host has landed or finished the level's intro. At the end of a level, everybody goes on to the next one together, and the code stays the same.

## Console commands

| Command | |
|---|---|
| `coop_host [level]` | host a co-op game |
| `coop_join <code>` | join a friend's game |
| `coop_relay_status` | the code, who is connected, latency |
| `coop_tp` | teleport to the leader (if stuck) |
| `coop_status` | players and their state |
| `coop_direct 0` | never connect directly, always through the relay |
| `coop_english_keyboard 0` | keep the system keyboard layout in the game window |

## Connection

The mod's relay server brings the host and his friends together. Right after
joining, the two machines also try to reach each other directly (UDP hole
punching). That usually works within a second, and then the latency is simply
the one between the two computers. Where it cannot work (strict NAT,
blocked UDP), the game keeps going through the relay.

## Good to know

* A dead player comes back next to a teammate after 8 seconds.
* Enemies, objectives, cutscenes and the story are the host's. Friends see and play them with him.
* If the host leaves, the game ends for everyone.

## Uninstall

Run `Mods\Coop\uninstall.ps1` (`-RestoreLauncher` also puts the original `Crysis.exe` back).

---

This mod is not endorsed by or affiliated with Crytek or Electronic Arts.
Trademarks are the property of their respective owners. Game content copyright Crytek.
