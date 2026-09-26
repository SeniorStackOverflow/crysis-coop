// Crysis Coop: play over the internet without opening ports.
//
// The game runs on the host's PC as before. A small relay service (Relay/
// coop_relay.py, behind Caddy on a public server) connects the host and his
// friends: Coop.dll on both sides keeps a WebSocket tunnel to it and turns
// it into local UDP for the game. The host gets a code (always the same one
// for his player key), friends join with "coop_join <code>".
#pragma once

namespace CoopRelay
{
	void Init();                    // console variables and commands
	void Update(float frameTime);   // main thread: start/stop the tunnel, tell the host the code, rejoin
	// the local client left a game (EDisconnectionCause, the reason's text)
	void OnDisconnected(int cause, const char* description);
	void Shutdown();

	// this player's secret key (16 bytes) and public id (16 hex digits, ""
	// when there is none: no key could be made)
	void GetPlayerKey(unsigned char key[16]);
	string LocalPlayerId();
	// host: the player id of the friend whose game talks to the server from
	// this local UDP port ("" when not known: a LAN player, an old mod version)
	string PlayerIdOfPort(int port);
	const char* RelayUrl();
	// random hex digits (2 per byte, at most 32 bytes)
	string RandomHex(int bytes);

	// host: the game restarts (a new game, a checkpoint) with this map
	// command. The friends are told to wait and rejoin by themselves; the
	// command runs a moment later, when they know. A friend who becomes a
	// host leaves the game he joined.
	void RestartGame(const char* mapCommand);
	// friend joined through the relay: a lost connection to the host is
	// handled here (rejoining, or the reason shown), not by the menu
	bool HandlesDisconnect();
}
