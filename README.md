![](logo.jpg)

# Crysis Coop

The **Crysis (2007) single player campaign in co-op** for 2–4 players, built as a
Crysis mod (C++ game DLL, Lua and configs).

* The host plays the campaign exactly as in single player: AI, story scripts, cutscenes, objectives, level to level.
* Progress is saved at the campaign's checkpoints, on the host's PC and in the cloud; `coop_continue` picks the game up
  there, hosted by any player of the campaign. Every new game is a campaign of its own.
* Friends join over the internet with a short code that stays the same (`coop_join 382615`). Nobody has to open or
  forward ports. When the host's game restarts (a checkpoint loads, a new game), the friends join again by themselves.
* The game connects the players directly when it can (UDP hole punching), otherwise through a small relay server.

*Русское описание для игроков: [Mod/README_RU.md](Mod/README_RU.md)*

## Install and play

Download the latest [release](https://github.com/SeniorStackOverflow/crysis-coop/releases/latest), extract it and
run `install.bat`, or run this in PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -c "irm https://github.com/SeniorStackOverflow/crysis-coop/releases/latest/download/install.ps1 | iex"
```

* Host: start the **Crysis Coop** shortcut (the launcher `CrysisCoop.exe` in the game folder), **Multiplayer → Co-op game** (it replaces Quick game; Russian: Сеть → Кооперативная игра), then **New campaign** or **Continue**.
* Friend: **Multiplayer → Co-op game → Join a friend**, the code the host sees, **Join**.
* In the game, Esc → **Co-op game**: the code, the players, saving and going back to a checkpoint, leaving.
* Console commands (`coop_host`, `coop_continue`, `coop_join <code>`...) do the same without the menu.

Needs Crysis 1.2.1 (GOG, Steam, EA; DVD + patches) on Windows 8 or later, or on Linux under Wine (`install_linux.py`). Each player needs their own copy of the game.
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
* **Saving.**
  * At each checkpoint the host saves a single player savegame of the whole level. It is CryAction's own save,
    made in single player mode for a moment, without the other players.
  * Every player's equipment goes to the campaign's progress file, keyed by the player's relay id (not by the
    in-game name, which depends on the order of joining).
  * `coop_continue` hosts that level and loads the save into it. It then puts the server back into shape for
    network play (player limit, clocks), and the friends join (`CoopSave.cpp`).
  * Checkpoints are also uploaded to the relay server (`CoopCloud.cpp`), so every player of the campaign may
    carry it on.
* **Co-op menu.** A panel drawn with IUIDraw over the Flash main and in-game menus (`CoopMenu.cpp`). It takes the
  menu's mouse and keys before the Flash menu does. The HUD shows the host's code for a while.
* **Players and codes.** Every player has a random key made once. The relay keeps only its hash and gives each host
  a permanent code. The host tells the relay when his game restarts, and the friends' tunnels wait and reconnect
  their games (`CoopRelay.cpp`).

The files:

| Path | |
|---|---|
| `Code/Mod/` | the game DLL (`Coop.dll`), based on the CryENGINE 2 Mod SDK game code. The coop work is mostly in `CoopAI.cpp`, `CoopRelay.cpp`, `CoopSave.cpp`, `CoopCloud.cpp`, `CoopMenu.cpp`, `Nodes/CoopFlowNodes.cpp`, plus hooks in the stock files |
| `Code/CryEngine/` | engine interface headers from the SDK |
| `Mod/` | the mod's Lua scripts and configs (copied to `Mods/Coop`) |
| `Launcher/` | `CrysisCoop.exe`: starts the game with the mod, checks what the mod needs first, and updates the mod by itself (`Update.cpp`: signed manifest, from the VPS or GitHub) |
| `Installer/` | `install.ps1`, `install.bat`, `uninstall.ps1`; `install_linux.py` for Linux (the game under Wine) |
| `Relay/` | the relay server (Python, no dependencies: tunnel, codes, cloud checkpoints) and its deployment |
| `tools/` | `package.ps1` (release archive and signed update), `publish_update.ps1` (update to the VPS), `update_key.ps1` (the release key), `lua_check.py` |

The co-op levels are **not** in this repository or in the releases: the installer builds them from the player's own
game files.

## Building

Requirements:
* Visual Studio 2019 or later, or its Build Tools, with the "Desktop development with C++" workload.
* The repository cloned into the Crysis folder, e.g. `C:\GOG Games\Crysis\crysis-coop`.

```bat
build_coop.bat
```

This builds the 32-bit `Coop.dll` and the launcher `CrysisCoop.exe`, and copies them, together with `Mod/`, into
`..\Mods\Coop` (the launcher also into the game folder) and marks it as a development build (`Mods\Coop\noupdate`:
the launcher does not replace it with a release). Run the game with `..\CrysisCoop.exe`, or
`Bin32\Crysis.exe -mod Coop` ([C1-Launcher](https://github.com/ccomrade/c1-launcher) is required). Edit scripts and
configs in `Mod/`, not in `Mods\Coop`.

`tools\package.ps1` makes the release archive in `dist\`, and the automatic update in `dist\update-v<version>\`
(signed with the release key from `tools\update_key.ps1`, kept on the publishing PC only).
`tools\publish_update.ps1` puts the update on the VPS; every player's launcher installs it at its next start. The
GitHub workflow builds the DLL and the launcher and attaches the archive to a release when a `v*` tag is pushed.

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
