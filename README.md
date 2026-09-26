![](logo.jpg)

# Crysis Coop

The **Crysis (2007) single player campaign in co-op** for 2–4 players, built as a
Crysis mod (C++ game DLL, Lua and configs).

* The host plays the campaign exactly as in single player: AI, story scripts, cutscenes, objectives, level to level.
* Friends join over the internet with a short code (`coop_join 64117`). Nobody has to open or forward ports.
* The game connects the players directly when it can (UDP hole punching), otherwise through a small relay server.

*Русское описание для игроков: [Mod/README_RU.md](Mod/README_RU.md)*

## Install and play

Download the latest [release](https://github.com/SeniorStackOverflow/crysis-coop/releases/latest), extract it and
run `install.bat`, or run this in PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -c "irm https://github.com/SeniorStackOverflow/crysis-coop/releases/latest/download/install.ps1 | iex"
```

* Host: start the **Crysis Coop** shortcut, open the console (`~`) and type `coop_host`.
* Friend: start **Crysis Coop** and type `coop_join <the code the host sees>`.

Needs Crysis 1.2.1 (GOG, Steam, EA; DVD + patches) on Windows 8 or later. Each player needs their own copy of the game.
The player guide is in [Mod/README.md](Mod/README.md).

## How it works

Crysis 1 has no co-op. The mod runs the single player levels as network maps.

* **Server side.** The host's game runs the full single player simulation:
  * the AI system (the engine switches it off in network games);
  * the level flow graphs and story;
  * the gamerules of the campaign.
* **Clients.** They receive what the stock network code does not carry:
  * AI actions and shots;
  * cutscenes and HUD/flow effects (mirrored flow nodes);
  * objectives, map markers, usable objects;
  * player placement by scripts;
  * nanosuit modes.
* **Campaign.** Level changes keep everybody connected, and inventories and campaign state carry over.

The files:

| Path | |
|---|---|
| `Code/Mod/` | the game DLL (`Coop.dll`), based on the CryENGINE 2 Mod SDK game code. The coop work is mostly in `CoopAI.cpp`, `CoopRelay.cpp`, `Nodes/CoopFlowNodes.cpp`, plus hooks in the stock files |
| `Code/CryEngine/` | engine interface headers from the SDK |
| `Mod/` | the mod's Lua scripts and configs (copied to `Mods/Coop`) |
| `Installer/` | `install.ps1`, `install.bat`, `uninstall.ps1` |
| `Relay/` | the relay server (Python, no dependencies) and its deployment |
| `tools/` | `package.ps1` (release archive), `lua_check.py` |

The co-op levels are **not** in this repository or in the releases: the installer builds them from the player's own
game files.

## Building

Requirements:
* Visual Studio 2019 or later, or its Build Tools, with the "Desktop development with C++" workload.
* The repository cloned into the Crysis folder, e.g. `C:\GOG Games\Crysis\crysis-coop`.

```bat
build_coop.bat
```

This builds the 32-bit `Coop.dll` and copies it, together with `Mod/`, into `..\Mods\Coop`. Run the game with
`Bin32\Crysis.exe -mod Coop` ([C1-Launcher](https://github.com/ccomrade/c1-launcher) is required). Edit scripts and
configs in `Mod/`, not in `Mods\Coop`.

`tools\package.ps1` makes the release archive in `dist\`. The GitHub workflow builds the DLL and attaches the archive
to a release when a `v*` tag is pushed.

The relay server: see [Relay/README.md](Relay/README.md). The mod uses `wss://crysis.46-225-103-75.sslip.io/` by
default. Point the `coop_relay` cvar at your own relay to use that instead.

## Credits

* [c1-mod-sdk](https://github.com/ccomrade/c1-mod-sdk) by ccomrade: the CryENGINE 2 Mod SDK set up for modern MSVC, on which this project is built.
* [C1-Launcher](https://github.com/ccomrade/c1-launcher) by ccomrade: required to run the mod.

## License

The game code is based on the CryENGINE 2 Mod SDK and is distributed under its license, [LICENSE.txt](LICENSE.txt).
Free distribution only, and a legal copy of Crysis is required.

This site is not endorsed by or affiliated with Crytek or Electronic Arts. Trademarks are the property of their
respective owners. Game content copyright Crytek.
