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

#include <vector>

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
	// CryAction writes a savegame to this file (CGame::OnSaveGame)
	void OnEngineSave(const char* file);

	// ---- used by the co-op menu (CoopMenu) and the console commands
	struct SCampaignInfo
	{
		string id, name, level, checkpoint, host;
		unsigned int stamp;     // the newest checkpoint (unix time)
		bool local;             // on this PC
		bool cloud;             // on the server
		bool cloudNewer;        // the server has a newer checkpoint than this PC
		bool own;               // started by this player (else: a friend's he played in)
	};
	// the campaigns on this PC and in the cloud, newest first
	void RequestCampaigns();
	bool CampaignsReady(std::vector<SCampaignInfo>& out);   // false while asking the cloud
	// something is under way (a list, a download, a level to load)
	bool IsBusy();
	// the last result for the player ("" none)
	const char* MenuStatus();

	int LevelCount();
	const char* LevelName(int index);           // "island"
	const char* LevelTitle(const char* level);  // "Contact"

	void NewCampaign(const char* level, const char* name);
	void ContinueCampaign(const char* id, bool previous);
	void DeleteCampaign(const char* id);
	void SaveNow();                     // host
	void LoadCheckpoint(bool previous); // host: the campaign he plays
	const char* CurrentCampaign();      // its name, "" none
}
