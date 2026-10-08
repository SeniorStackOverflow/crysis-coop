// Crysis Coop: the AI companion.
//
// A host who has no friend at hand turns on "AI companion" in the co-op
// menu (coop_companion 1). The host's game then starts a second, light copy
// of the game in the background (no sound, a small window, few frames,
// below-normal priority, other CPU cores) that joins the host's game as the
// second player, directly on this PC (connect 127.0.0.1 <sv_port>).
//
// In that copy (coop_agent 1) a bot plays the second player: it follows the
// host, fights the enemies it sees, revives a downed teammate, rides along
// in the host's vehicle. Its player is driven through the normal input path
// (CPlayerInput::PreUpdate asks SteerInput), so the network sees a player.
//
// An AI agent may command it: the copy listens on 127.0.0.1:coop_agent_port
// (a line per request: "<id> <command> [arguments]", a line of JSON back),
// and "CrysisCoop.exe -coop_mcp" is an MCP server (stdio) for agents such as
// Claude that speaks to it: what the companion sees (text, a screenshot on
// request), the chat, orders (follow, hold, go to, attack, revive...).
#pragma once

class CPlayer;

namespace CoopAgent
{
	void Init();
	// every frame (CGame::Update); EndFrame at its end (the companion's frame limit)
	void Update(float frameTime);
	void EndFrame();

	// this game is the AI companion (coop_agent 1)
	bool IsCompanion();

	// the companion's player input (CPlayerInput::PreUpdate): turning,
	// moving, sprinting, jumping as the bot wants them
	void SteerInput(CPlayer* pPlayer, Ang3& deltaRotation, Vec3& deltaMovement, uint32& actions);

	// every chat line this machine shows (the agent reads them)
	void OnChat(EntityId source, const char* text);

	// server: a companion's request (SvCoopSync kind 15): 1 catch up with
	// the player "name", 2 leave the vehicle, 3 get into the vehicle "name"
	void OnServerRequest(EntityId agent, int op, const char* name);
	// the companion: the host's answer to op 5, a way to walk to "goal"
	// ("x y z;x y z;..." in text; empty: none found)
	void OnPath(const char* goal, const char* text);

	// host: the companion's state for the co-op menu (0 not started yet,
	// 1 joining, 2 in the game, 3 its game would not start, 4 waiting for
	// the host's game, 5 its game closed and is started again)
	int CompanionState();
	// how many times its game was started
	int CompanionTries();
	// seconds after which a companion still not in the game counts as
	// failed (it joins in about 45 s; the menu and the HUD say so)
	const int JOIN_TIMEOUT = 90;
	// seconds since the companion's game was started (0: none)
	float CompanionSeconds();
}
