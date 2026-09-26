// Crysis Coop: play over the internet without opening ports.
//
// The game runs on the host's PC as before. A small relay service (Relay/
// coop_relay.py, behind Caddy on a public server) connects the host and his
// friends: Coop.dll on both sides keeps a WebSocket tunnel to it and turns
// it into local UDP for the game. The host gets a code, friends join with
// "coop_join <code>".
#pragma once

namespace CoopRelay
{
	void Init();                    // console variables and commands
	void Update(float frameTime);   // main thread: start/stop the tunnel, tell the host the code
	void OnDisconnected();          // the local client left a game
	void Shutdown();
}
