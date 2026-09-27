// Crysis Coop: downed players and reviving them. With coop_auto_respawn 0
// (the default) a player who dies stays down until a teammate holds the use
// key next to him for a few seconds; when everybody is down, the host goes
// back to the last checkpoint. The rules are the server's Lua
// (CoopGameRules.lua, CoopReviveInput and the revive section); this is the
// rest: the use key of the local player, the server's end of it, the state
// the server sends (who is reviving whom, the checkpoint countdown) and the
// revive HUD on every machine.
#pragma once

struct IUIDraw;
struct IFFont;
class CPlayer;

namespace CoopRevive
{
	void Init();                // coop_auto_respawn

	// coop_auto_respawn 0: the dead wait for a teammate
	bool IsReviveMode();

	// the local player's use key; true when it went to a downed teammate
	// (and must not reach the rest of the game)
	bool OnUse(CPlayer* pPlayer, int activationMode);

	// server: a player's use key on a downed teammate (from his client, or
	// the host's own); the Lua rules decide
	void OnInput(EntityId reviver, EntityId target, bool press);

	// the revive state from the server's rules (fromScript: on the server,
	// then sent to the clients): 1 target is being revived by reviver
	// (seconds: how long it takes), 2 stopped, 3 revived, 4 everybody is down
	// (seconds: until the checkpoint loads), 5 not any more
	void OnState(int op, EntityId target, EntityId reviver, float seconds, bool fromScript);

	// the same from the other machine: players by name (their entity ids
	// differ between the machines)
	void OnStateFromServer(int op, const char* target, const char* reviver, float seconds);
	void OnInputFromClient(EntityId reviver, const char* target, bool press);

	// server: a checkpoint of the story was reached (the rules bring the
	// downed back)
	void OnCheckpoint();

	// the HUD: the prompt, the progress bar, who is down, the countdown
	void RenderHud(IUIDraw* pUIDraw, IFFont* pFont);
}
