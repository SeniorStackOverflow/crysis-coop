// Crysis Coop: campaign progress.
//
// The host's game saves at the campaign's own checkpoints (System:SaveGame
// flow nodes) and at every level start, with CryAction's single player
// savegame: the whole level (AI, flow graphs, objectives, what was destroyed)
// and the host's player. The other players are left out of it; their
// equipment is kept by player name in coop_progress.txt next to the save.
// "coop_continue" hosts the saved level again, loads the save into it and
// the friends join as usual.
#pragma once

namespace CoopSave
{
	void Init();                                  // console variables and commands
	void Update(float frameTime);                 // server: pending checkpoint / pending load
	void OnLoadingStart(const char* levelName);   // any level starts loading
	void OnLevelReady(const char* levelName);     // coop server: the level is loaded

	// server: the campaign reached a checkpoint (System:SaveGame); saved as
	// soon as the game allows it (not during a cutscene, the host alive...)
	void RequestCheckpoint(const char* name);

	// coop_continue: the level runs but the save is not loaded yet (friends
	// must not join before: the load disconnects everybody but the host)
	bool IsLoadPending();

	// a coop checkpoint's savegame file: not offered by the single player
	// menus (Load game, Resume), which would load it as a single player game
	bool IsCoopSaveName(const char* name);
}
