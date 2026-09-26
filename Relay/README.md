# Crysis Coop relay

Lets anyone host a coop game on their own PC and have friends join from anywhere,
without opening or forwarding ports. The game runs on the host's PC; this service
only forwards the game's datagrams.

```
friend's game --UDP--> Coop.dll (127.0.0.1) ==wss==> Caddy :443 --> relay (127.0.0.1:64190)
                                                                     ==wss==> Coop.dll on the host
                                                                              --UDP--> 127.0.0.1:64087 (host's game)
```

* `coop_relay.py` is the service (Python 3, no dependencies). Its docstring describes the protocol.
* `crysis-coop-relay.service` is the systemd unit.
* `crysis-coop.caddy` is the Caddy site. It goes into `/etc/caddy/conf.d/`, which the main Caddyfile imports.
* Mod side: `Code/Mod/CoopRelay.cpp`.
  * The host's tunnel starts by itself when a coop server runs, and the host sees `Co-op code: N`.
  * Friends type `coop_join N`.
  * The URL is in the cvar `coop_relay` (default `wss://crysis.46-225-103-75.sslip.io/`).

Direct path: through the relay, the host and each friend swap the UDP addresses they
can be reached at (their local ones and the public one that STUN reports). Both then
punch through their NATs. When that works, the game's datagrams go straight between
them, and the relay only keeps the pings and stays as the fallback. That removes the
detour through the VPS: the latency becomes the direct host↔friend round trip instead
of host↔VPS plus friend↔VPS. Disable it with `coop_direct 0`.

WebSocket over 443 is used because the VPS provider's firewall drops inbound UDP.
The optional UDP ports (control 64100, sessions 64101-64164) only work where UDP is let in.

## Deploy (Ubuntu, Caddy already running)

```bash
sudo useradd --system --no-create-home --shell /usr/sbin/nologin coop-relay
sudo mkdir -p /srv/crysis-coop-relay
sudo install -m 0755 coop_relay.py /srv/crysis-coop-relay/
sudo install -m 0644 crysis-coop-relay.service /etc/systemd/system/
sudo install -m 0644 crysis-coop.caddy /etc/caddy/conf.d/
sudo systemctl daemon-reload && sudo systemctl enable --now crysis-coop-relay
sudo caddy validate --config /etc/caddy/Caddyfile && sudo systemctl reload caddy
journalctl -u crysis-coop-relay -f
```

Limits:
* 64 games, 8 friends per game, 4 games per host address.
* 2 MB/s per game and direction. A coop game uses about 5 KB/s.
* About 4 MB of memory.
