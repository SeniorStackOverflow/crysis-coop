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

Everything is in **Multiplayer → Co-op game** (in the Russian game **Сеть →
Кооперативная игра**). It takes the place of Quick game: the multiplayer servers
are gone. In the game, Esc opens the same page.

**Host:** start **Crysis Coop** and click **Multiplayer → Co-op game**:

* **Continue** carries on the newest campaign from its last checkpoint.
* **New campaign** starts one on any of the 11 levels. A name is optional.
* **All campaigns** lists your campaigns on this PC and in the cloud. From there
  you can continue one, go back to the checkpoint before, or delete it.

When the level has loaded, the code appears on the screen and in the co-op menu,
for example `Co-op code: 382615`. It is always the same code for the same host.

**Friend:** start **Crysis Coop**, click **Multiplayer → Co-op game → Join a
friend**, type the
host's code and click **Join**. Next time the code is already filled in.

The friend appears next to the host once the host has landed or finished the
level's intro. At the end of a level, everybody goes on to the next one together.

**In the game** (Esc → Co-op game):

* The host sees his code and who is connected, and has:
  * **Save now**;
  * **Back to the last checkpoint** and **The checkpoint before**;
  * other campaigns or a new one.
* A friend sees the connection and can **Leave the game**.

## Saving

The host's game saves the progress by itself, like the single player
campaign: at every level start and at the campaign's checkpoints ("Game
saved" on everybody's screen).

* **Continue** loads the level with everything done in it so far, and the host
  is where he was. Friends get back the weapons they had at that checkpoint.
* When the host's game restarts (a checkpoint, another campaign) or his
  connection drops, the friends' games wait for him and join again by
  themselves ("The host is loading the game").
* Every new campaign is separate; it never overwrites an old one.

**Cloud.** Every checkpoint is also kept on the mod's server, for everybody who
played in that campaign. The campaign is not tied to one PC: any friend who
played in it finds it in his **All campaigns** and can continue it as the host.
The others join him as usual. The checkbox "Keep my checkpoints in the cloud" in
the co-op menu turns it off.

The progress lives in `Documents\My Games\Crysis\SaveGames`: the `coop` folder
and the `coop_checkpoint_*` saves.

## Console commands

The same without the menu (the console opens with `~`):

| Command | |
|---|---|
| `coop_host [level] [name]` | host a new co-op campaign |
| `coop_continue [number or name] [prev]` | host a campaign from its last checkpoint |
| `coop_load [prev]` | back to the last checkpoint (host, during the game) |
| `coop_save` | save now (host) |
| `coop_campaigns` | the campaigns on this PC and in the cloud |
| `coop_campaign_delete <number or name>` | delete a campaign |
| `coop_join [code]` | join a friend's game (no code: the last one) |
| `coop_leave` | leave the friend's game |
| `coop_relay_status` | the code, who is connected, latency |
| `coop_tp` | teleport to the leader (if stuck) |
| `coop_status` | players and their state |
| `coop_cloud 0` | no checkpoints on the server |
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
