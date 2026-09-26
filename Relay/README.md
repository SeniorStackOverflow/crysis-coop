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
sudo caddy validate --config /etc/caddy/Caddyfile && sudo systemctl reload caddy
journalctl -u crysis-coop-relay -f
```

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
