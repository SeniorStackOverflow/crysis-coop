# Crysis Coop relay

Lets anyone host a coop game on their own PC and have friends join from anywhere,
without opening or forwarding ports. The game runs on the host's PC; this service
only forwards the game's datagrams, and keeps the co-op campaigns' checkpoints for
the players ("cloud saves"). It never runs the game.

```
friend's game --UDP--> Coop.dll (127.0.0.1) ==wss==> Caddy :443 --> relay (127.0.0.1:64190)
                                                                     ==wss==> Coop.dll on the host
                                                                              --UDP--> 127.0.0.1:64087 (host's game)
```

Everything goes through one WebSocket per player on port 443 (Caddy). The VPS
provider's firewall drops everything else, and nothing else is needed.

* `coop_relay.py` is the service (Python 3, no dependencies). Its docstring describes the protocol.
* `crysis-coop-relay.service` is the systemd unit.
* `crysis-coop.caddy` is the Caddy site. It goes into `/etc/caddy/conf.d/`, which the main Caddyfile imports.
  It also serves the mod's automatic updates from `/srv/crysis-coop-update` at `/update/` (see below).
* Mod side: `Code/Mod/CoopRelay.cpp` (tunnel, codes, rejoining) and `Code/Mod/CoopCloud.cpp` (checkpoints).
* `test_protocol.py <url>` checks a running relay (codes, rejoining, cloud rights, protocol 1).

## What it does

* **Codes.** Every player's Coop.dll makes a random secret key once (`Documents\My Games\Crysis\coop_player.txt`).
  The relay stores only its public id (the first 8 bytes of its SHA-256) and gives each host a 6-digit code
  that stays the same for as long as he plays (`data/codes.json`; freed after 60 days unused).
* **Rejoining.** The host's Coop.dll tells the relay when his game restarts (a checkpoint loads, a new game)
  and when it is ready again. The relay tells the friends. Their tunnels stay open and their games reconnect
  by themselves. When the host's connection breaks, the friends wait for him for up to 5 minutes.
* **Cloud saves.** After each checkpoint the host uploads it (the progress file plus the savegame, about 1.5 MB)
  to `data/campaigns/<campaign>/`, keeping the last two checkpoints. Access rules:
  * A campaign belongs to the player who stored it first.
  * The players listed in its progress file (everybody who was in the game) may list it, download it
    and store further checkpoints, so any of them can carry the campaign on as the host.
* **Direct path.** Through the relay, the host and each friend swap the UDP addresses they can be reached at,
  then punch through their NATs. When that works, the game's datagrams go straight between them, and the relay
  only keeps the pings and stays as the fallback. Disable it with `coop_direct 0`.

## Deploy (Ubuntu, Caddy already running)

```bash
sudo useradd --system --no-create-home --shell /usr/sbin/nologin coop-relay
sudo mkdir -p /srv/crysis-coop-relay/data
sudo chown coop-relay:coop-relay /srv/crysis-coop-relay/data
sudo install -m 0755 coop_relay.py /srv/crysis-coop-relay/
sudo install -m 0644 crysis-coop-relay.service /etc/systemd/system/
sudo install -m 0644 crysis-coop.caddy /etc/caddy/conf.d/
sudo systemctl daemon-reload && sudo systemctl enable --now crysis-coop-relay
sudo mkdir -p /srv/crysis-coop-update && sudo chown "$USER" /srv/crysis-coop-update
sudo caddy validate --config /etc/caddy/Caddyfile && sudo systemctl reload caddy
journalctl -u crysis-coop-relay -f
```

## Updates

The mod's launcher (`Launcher/Update.cpp`) asks `https://<this host>/update/update.txt` at every start, and GitHub's
latest release when this host does not answer. When the manifest names a newer version, it downloads the package
next to it and installs it before the game starts, without asking the player.

* `update.txt` is signed with the release key (`tools/update_key.ps1`, kept on the publishing PC only). The
  launcher installs nothing that is not signed by it, and checks the package's SHA-256 and size from the
  manifest. A broken or taken-over server can make it skip an update, never install something else.
* Publishing: `tools/package.ps1` (writes `dist/update-v<version>/`), then `tools/publish_update.ps1` (uploads
  the package, then renames `update.txt` into place; keeps the last 3 packages). Attach the same two files to the
  GitHub release as well.
* Going back: put an older version's files there again. The launchers never install an older version than the
  one they have, so this only stops further updates; fix forward with a newer version.

## Limits

* Games:
  * 8 friends per game;
  * 4 games per host address;
  * 20 new players per address and hour;
  * 2 MB/s per game and direction (a coop game uses about 5 KB/s).
* Cloud:
  * 10 campaigns per player;
  * 16 MB per checkpoint;
  * one checkpoint per player every 10 s;
  * 2 GB in total;
  * campaigns untouched for 180 days are deleted.
* Memory: a few MB (checkpoints stream to disk).

Mod 0.2 / 0.3 (protocol 1, codes 64101-64164) keeps working.
