// Crysis Coop: campaign progress (see CoopSave.h).
//
// CryAction's savegame is single player only: SaveGame / LoadGame refuse to
// run while gEnv->bMultiplayer is set, so the server runs them in single
// player mode for the moment. A load into the running level does two things
// a network server cannot live with, both undone right after it:
//  - it sets the server's player limit to 1 (CGameServerNub::SetMaxPlayers,
//    "when loading a save game ... this is hopefully singleplayer"): everybody
//    else is kicked and nobody can join any more;
//  - the game and physics clocks go back to the time of the save; the network
//    runs on them and stops answering new connections while they are behind.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "StdAfx.h"
#include "CoopSave.h"
#include "CoopAI.h"
#include "Game.h"
#include "GameRules.h"
#include "IActorSystem.h"
#include "IItemSystem.h"
#include "ILevelSystem.h"
#include "IViewSystem.h"
#include "ICryPak.h"
#include "StringUtils.h"

#include <map>
#include <set>
#include <vector>
#include <time.h>

namespace
{
	// ------------------------------------------------------------------
	// the server's player limit after a load

	struct SRange
	{
		const char* lo;
		const char* hi;
		bool Contains(const void* p) const { return p >= lo && p < hi; }
	};

	bool ModuleRange(const char* name, SRange& r)
	{
		HMODULE h = GetModuleHandleA(name);
		if (!h)
			return false;
		const IMAGE_DOS_HEADER* pDos = (const IMAGE_DOS_HEADER*)h;
		const IMAGE_NT_HEADERS* pNt = (const IMAGE_NT_HEADERS*)((const char*)h + pDos->e_lfanew);
		r.lo = (const char*)h;
		r.hi = r.lo + pNt->OptionalHeader.SizeOfImage;
		return true;
	}

	// committed, writable, unguarded memory
	bool Writable(const void* p, size_t size)
	{
		MEMORY_BASIC_INFORMATION mbi;
		if (!p || !VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
			return false;
		if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))
			return false;
		if (!(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
			return false;
		return (const char*)p + size <= (const char*)mbi.BaseAddress + mbi.RegionSize;
	}

	// an int field of a CryAction object that holds the player limit
	struct SField
	{
		const char* object;
		const void* vtable;
		int* value;
	};
	std::vector<SField> s_fields;
	int s_maxPlayers = 0;
	float s_gameTimeBefore = 0.0f;
	int s_physTimeBefore = 0;

	const size_t OBJECT_SCAN = 0x200;

	// CryAction objects (their vtable lies in CryAction.dll) that root points
	// to, depth levels deep; every int field equal to value is remembered.
	// The game server nub (CGameServerNub, which holds the limit) is one of
	// them: the network nub and CryAction's game both point to it.
	void Collect(const char* root, size_t rootSize, int depth, const SRange& action, int value, std::set<const char*>& seen)
	{
		for (size_t off = 0; off + sizeof(void*) <= rootSize; off += sizeof(void*))
		{
			const char* p = *(const char* const*)(root + off);
			if (seen.count(p) || !Writable(p, OBJECT_SCAN))
				continue;
			const void* vtable = *(const void* const*)p;
			if (!action.Contains(vtable))
				continue;
			seen.insert(p);
			for (size_t f = sizeof(void*); f + sizeof(int) <= OBJECT_SCAN; f += sizeof(int))
			{
				if (*(const int*)(p + f) == value)
				{
					SField field = { p, vtable, (int*)(p + f) };
					s_fields.push_back(field);
				}
			}
			if (depth > 1)
				Collect(p, OBJECT_SCAN, depth - 1, action, value, seen);
		}
	}

	void NetLoadBegin()
	{
		s_fields.clear();
		s_gameTimeBefore = gEnv->pTimer->GetCurrTime();
		s_physTimeBefore = gEnv->pPhysicalWorld ? gEnv->pPhysicalWorld->GetiPhysicsTime() : 0;
		ICVar* pMax = gEnv->pConsole->GetCVar("sv_maxplayers");
		s_maxPlayers = pMax ? pMax->GetIVal() : 0;
		SRange action;
		IGameFramework* pFramework = g_pGame->GetIGameFramework();
		if (s_maxPlayers <= 1 || !ModuleRange("CryAction.dll", action))
			return;
		std::set<const char*> seen;
		if (const char* pNub = (const char*)pFramework->GetServerNetNub())
			if (Writable(pNub, 0x400))
				Collect(pNub, 0x400, 1, action, s_maxPlayers, seen);
		if (Writable(pFramework, 0x400))
			Collect((const char*)pFramework, 0x400, 2, action, s_maxPlayers, seen);
	}

	int NetLoadEnd()
	{
		const float gameTime = gEnv->pTimer->GetCurrTime();
		const int physTime = gEnv->pPhysicalWorld ? gEnv->pPhysicalWorld->GetiPhysicsTime() : 0;
		if (gameTime < s_gameTimeBefore)
			gEnv->pTimer->SetTimer(ITimer::ETIMER_GAME, s_gameTimeBefore);
		if (gEnv->pPhysicalWorld && physTime < s_physTimeBefore)
			gEnv->pPhysicalWorld->SetiPhysicsTime(s_physTimeBefore);

		int restored = 0;
		for (size_t i = 0; i < s_fields.size(); ++i)
		{
			const SField& f = s_fields[i];
			// the same object is still there and its limit went down to one player
			if (!Writable(f.object, OBJECT_SCAN) || *(const void* const*)f.object != f.vtable || *f.value != 1)
				continue;
			*f.value = s_maxPlayers;
			++restored;
		}
		CryLogAlways("[CoopSave] after the load: player limit %d %s (%d candidates); game clock of the save %.1f s, before the load %.1f s", s_maxPlayers,
			restored == 1 ? "restored" : restored ? "restored in several places" : "NOT FOUND", (int)s_fields.size(), gameTime, s_gameTimeBefore);
		s_fields.clear();
		return restored;
	}

	// ------------------------------------------------------------------
	// progress file: the last checkpoint and the other players' equipment

	const char* const PROGRESS_FILE = "%USER%/SaveGames/coop_progress.txt";
	const char* const PROGRESS_PREV_FILE = "%USER%/SaveGames/coop_progress_prev.txt";
	const char* const SAVE_NAME = "coop_checkpoint_%c.CRYSISJMSF";

	struct SProgress
	{
		string level;       // short name: rescue
		string save;        // CryAction savegame name
		string checkpoint;
		string time;
		std::map<string, CoopAI::SInventory> players;
	};

	string UserPath(const char* path)
	{
		char buf[ICryPak::g_nMaxPath];
		const char* p = gEnv->pCryPak->AdjustFileName(path, buf, ICryPak::FLAGS_NO_MASTER_FOLDER_MAPPING | ICryPak::FLAGS_FOR_WRITING);
		return p ? string(p) : string(path);
	}

	void Split(const string& s, char sep, std::vector<string>& out)
	{
		out.clear();
		size_t start = 0;
		while (start <= s.length())
		{
			size_t end = s.find(sep, start);
			if (end == string::npos)
				end = s.length();
			out.push_back(s.substr(start, end - start));
			start = end + 1;
		}
	}

	bool WriteProgress(const char* file, const SProgress& p)
	{
		const string path = UserPath(file);
		FILE* f = fopen(path.c_str(), "wb");
		if (!f)
		{
			CryLogAlways("[CoopSave] cannot write %s", path.c_str());
			return false;
		}
		fprintf(f, "version=1\nlevel=%s\nsave=%s\ncheckpoint=%s\ntime=%s\n", p.level.c_str(), p.save.c_str(), p.checkpoint.c_str(), p.time.c_str());
		for (std::map<string, CoopAI::SInventory>::const_iterator it = p.players.begin(); it != p.players.end(); ++it)
		{
			const CoopAI::SInventory& inv = it->second;
			string items, ammo;
			for (size_t i = 0; i < inv.items.size(); ++i)
				items += (i ? "," : "") + inv.items[i];
			for (size_t i = 0; i < inv.ammo.size(); ++i)
			{
				string a;
				a.Format("%s%s:%d", i ? "," : "", inv.ammo[i].first.c_str(), inv.ammo[i].second);
				ammo += a;
			}
			fprintf(f, "player=%s\t%s\t%s\t%s\n", it->first.c_str(), inv.current.c_str(), items.c_str(), ammo.c_str());
		}
		fclose(f);
		return true;
	}

	bool ReadProgress(const char* file, SProgress& p)
	{
		FILE* f = fopen(UserPath(file).c_str(), "rb");
		if (!f)
			return false;
		char line[4096];
		std::vector<string> fields, list, pair;
		while (fgets(line, sizeof(line), f))
		{
			string s = string(line).TrimRight("\r\n");
			const size_t eq = s.find('=');
			if (eq == string::npos)
				continue;
			const string key = s.substr(0, eq), value = s.substr(eq + 1);
			if (key == "level")
				p.level = value;
			else if (key == "save")
				p.save = value;
			else if (key == "checkpoint")
				p.checkpoint = value;
			else if (key == "time")
				p.time = value;
			else if (key == "player")
			{
				Split(value, '\t', fields);
				if (fields.size() < 4 || fields[0].empty())
					continue;
				CoopAI::SInventory& inv = p.players[fields[0]];
				inv.current = fields[1];
				Split(fields[2], ',', list);
				for (size_t i = 0; i < list.size(); ++i)
					if (!list[i].empty())
						inv.items.push_back(list[i]);
				Split(fields[3], ',', list);
				for (size_t i = 0; i < list.size(); ++i)
				{
					Split(list[i], ':', pair);
					if (pair.size() == 2 && !pair[0].empty())
						inv.ammo.push_back(std::make_pair(pair[0], atoi(pair[1].c_str())));
				}
			}
		}
		fclose(f);
		return !p.level.empty() && !p.save.empty();
	}

	// ------------------------------------------------------------------
	// saving

	ICVar* s_pCheckpoints = 0;
	bool s_pending = false;
	string s_pendingName;
	float s_pendingFor = 0.0f;
	float s_retryTimer = 0.0f;
	string s_readyLevel;          // the coop level the server runs
	bool s_levelStartSaved = false;
	string s_lastSave;            // the slot written last

	const char* ShortLevelName(const char* level)
	{
		const char* p = level ? level : "";
		for (const char* q = p; *q; ++q)
			if (*q == '/' || *q == '\\')
				p = q + 1;
		if (!strnicmp(p, "coop_", 5))
			p += 5;
		return p;
	}

	// the AI system writes every object into the log while it (de)serializes
	// with verbose AI logging: seconds of stall
	struct SQuietAILog
	{
		ICVar* pVars[2];
		int values[2];
		SQuietAILog()
		{
			const char* names[2] = { "ai_LogFileVerbosity", "ai_LogConsoleVerbosity" };
			for (int i = 0; i < 2; ++i)
			{
				pVars[i] = gEnv->pConsole->GetCVar(names[i]);
				values[i] = pVars[i] ? pVars[i]->GetIVal() : 0;
				if (pVars[i])
					pVars[i]->Set(0);
			}
		}
		~SQuietAILog()
		{
			for (int i = 0; i < 2; ++i)
				if (pVars[i])
					pVars[i]->Set(values[i]);
		}
	};

	// the other players are not part of a single player save: their actors
	// and everything they carry are left out (ENTITY_FLAG_NO_SAVE) meanwhile
	void MarkUnsaved(IEntity* pEntity, std::vector<EntityId>& marked)
	{
		if (!pEntity || (pEntity->GetFlags() & ENTITY_FLAG_NO_SAVE))
			return;
		pEntity->SetFlags(pEntity->GetFlags() | ENTITY_FLAG_NO_SAVE);
		marked.push_back(pEntity->GetId());
		for (int i = 0; i < pEntity->GetChildCount(); ++i)
			MarkUnsaved(pEntity->GetChild(i), marked);
	}

	void MarkRemotePlayersUnsaved(std::vector<EntityId>& marked)
	{
		IGameFramework* pFramework = g_pGame->GetIGameFramework();
		IActor* pLocal = pFramework->GetClientActor();
		IActorIteratorPtr pIt = pFramework->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = pIt->Next())
		{
			if (!pActor->IsPlayer() || pActor == pLocal)
				continue;
			MarkUnsaved(pActor->GetEntity(), marked);
			if (IInventory* pInv = pActor->GetInventory())
				for (int i = 0; i < pInv->GetCount(); ++i)
					MarkUnsaved(gEnv->pEntitySystem->GetEntity(pInv->GetItem(i)), marked);
		}
	}

	void ClearUnsaved(const std::vector<EntityId>& marked)
	{
		for (size_t i = 0; i < marked.size(); ++i)
			if (IEntity* pEntity = gEnv->pEntitySystem->GetEntity(marked[i]))
				pEntity->SetFlags(pEntity->GetFlags() & ~ENTITY_FLAG_NO_SAVE);
	}

	// why a checkpoint cannot be saved right now (0: it can)
	const char* CannotSave()
	{
		IGameFramework* pFramework = g_pGame->GetIGameFramework();
		if (!pFramework->IsGameStarted() || !CoopAI::IsCoopSession())
			return "no coop game";
		if (IViewSystem* pView = pFramework->GetIViewSystem())
			if (pView->IsPlayingCutScene())
				return "cutscene";
		if (!pFramework->CanSave())
			return "saving not allowed now";
		IActor* pHost = pFramework->GetClientActor();
		if (!pHost || pHost->GetHealth() <= 0)
			return "the host is dead";
		if (CoopAI::IsAirborne(pHost->GetEntityId()))
			return "the host is in the air";
		return 0;
	}

	void Notify(const char* msg)
	{
		if (CGameRules* pRules = g_pGame->GetGameRules())
			pRules->SendTextMessage(eTextMessageInfo, msg, eRMI_ToRemoteClients);
	}

	// on the host's own screen
	void TellHost(const char* msg)
	{
		CryLogAlways("[CoopSave] %s", msg);
		if (CGameRules* pRules = g_pGame->GetGameRules())
			pRules->OnTextMessage(eTextMessageError, msg);
	}

	bool SaveCheckpoint(const string& name)
	{
		IGameFramework* pFramework = g_pGame->GetIGameFramework();
		if (s_lastSave.empty())
		{
			SProgress last;
			if (ReadProgress(PROGRESS_FILE, last))
				s_lastSave = last.save;
		}
		string save;
		save.Format(SAVE_NAME, s_lastSave == string().Format(SAVE_NAME, 'a') ? 'b' : 'a');

		SProgress progress;
		progress.level = s_readyLevel;
		progress.save = save;
		progress.checkpoint = name;
		char stamp[64];
		const time_t now = time(0);
		strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
		progress.time = stamp;
		CoopAI::CollectInventories(progress.players, false);

		std::vector<EntityId> unsaved;
		MarkRemotePlayersUnsaved(unsaved);
		bool ok;
		{
			SQuietAILog quiet;
			const bool mp = gEnv->bMultiplayer;
			gEnv->bMultiplayer = false;
			ok = pFramework->SaveGame(save.c_str(), true, true, eSGR_QuickSave, true, name.c_str());
			gEnv->bMultiplayer = mp;
		}
		ClearUnsaved(unsaved);
		if (!ok)
		{
			CryLogAlways("[CoopSave] checkpoint %s: the game refused to save", name.c_str());
			return false;
		}

		// the previous checkpoint stays as coop_progress_prev.txt (its save is
		// the other slot)
		const string cur = UserPath(PROGRESS_FILE), prev = UserPath(PROGRESS_PREV_FILE);
		MoveFileExA(cur.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);
		WriteProgress(PROGRESS_FILE, progress);
		s_lastSave = save;
		CryLogAlways("[CoopSave] checkpoint %s saved: %s, %s (%d other players' equipment)", name.c_str(), progress.level.c_str(),
			save.c_str(), (int)progress.players.size());
		Notify("@game_saved");
		return true;
	}

	// ------------------------------------------------------------------
	// coop_continue

	enum ELoadState
	{
		eLS_None,
		eLS_WaitLevel,  // the level is being hosted
		eLS_Loading,
	};
	ELoadState s_loadState = eLS_None;
	SProgress s_load;
	float s_loadWait = 0.0f;
	const float LOAD_TIMEOUT = 180.0f;

	void LoadNow()
	{
		IGameFramework* pFramework = g_pGame->GetIGameFramework();
		s_loadState = eLS_Loading;
		CryLogAlways("[CoopSave] loading %s (%s, checkpoint %s)", s_load.save.c_str(), s_load.level.c_str(), s_load.checkpoint.c_str());
		pFramework->AllowLoad(true);
		bool ok;
		int limitRestored;
		{
			SQuietAILog quiet;
			const bool mp = gEnv->bMultiplayer;
			gEnv->bMultiplayer = false;
			NetLoadBegin();
			ok = pFramework->LoadGame(s_load.save.c_str(), true, true);
			limitRestored = NetLoadEnd();
			gEnv->bMultiplayer = mp;
		}
		s_loadState = eLS_None;
		// the level's own start is not a checkpoint any more
		s_levelStartSaved = true;
		s_pending = false;
		if (!ok)
		{
			TellHost("The saved game could not be loaded: the level starts from the beginning");
			s_levelStartSaved = false;
			return;
		}
		if (!limitRestored)
			TellHost("Friends may not be able to join this game: if they cannot, host it again with coop_continue");
		s_lastSave = s_load.save;
		CoopAI::SetInventoryCarry(s_load.level.c_str(), s_load.players);
		CoopAI::OnGameLoaded();
		CryLogAlways("[CoopSave] progress loaded: %s, checkpoint %s (saved %s)", s_load.level.c_str(), s_load.checkpoint.c_str(), s_load.time.c_str());
	}

	void CmdContinue(IConsoleCmdArgs* pArgs)
	{
		const bool prev = pArgs->GetArgCount() > 1 && !stricmp(pArgs->GetArg(1), "prev");
		SProgress progress;
		if (!ReadProgress(prev ? PROGRESS_PREV_FILE : PROGRESS_FILE, progress))
		{
			CryLogAlways("No saved co-op progress%s. Start a new game with: coop_host", prev ? " (previous checkpoint)" : "");
			return;
		}
		CryLogAlways("[CoopSave] continuing: %s, checkpoint %s (saved %s)", progress.level.c_str(), progress.checkpoint.c_str(), progress.time.c_str());
		s_load = progress;
		s_loadState = eLS_WaitLevel;
		s_loadWait = 0.0f;
		gEnv->pConsole->ExecuteString("exec coop_settings.cfg");
		string cmd;
		cmd.Format("map multiplayer/tia/coop_%s s", progress.level.c_str());
		gEnv->pConsole->ExecuteString(cmd.c_str());
	}

	void CmdSave(IConsoleCmdArgs*)
	{
		if (!gEnv->bServer || !CoopAI::IsCoopSession())
		{
			CryLogAlways("coop_save: only the host of a coop game can save");
			return;
		}
		CoopSave::RequestCheckpoint("manual");
		if (const char* why = CannotSave())
			CryLogAlways("[CoopSave] the game will be saved as soon as possible (%s)", why);
	}
}

void CoopSave::Init()
{
	s_pCheckpoints = gEnv->pConsole->RegisterInt("coop_checkpoints", 1, 0,
		"Crysis Coop: 1 = the host saves the campaign progress at its checkpoints and at every level start (coop_continue)");
	gEnv->pConsole->AddCommand("coop_continue", CmdContinue, 0,
		"Crysis Coop: host the co-op campaign from the last checkpoint (coop_continue prev: the one before)");
	gEnv->pConsole->AddCommand("coop_save", CmdSave, 0, "Crysis Coop: the host saves the co-op progress now");
}

void CoopSave::OnLoadingStart(const char* levelName)
{
	s_readyLevel.clear();
	s_pending = false;
	s_levelStartSaved = false;
	// coop_continue: some other level than the saved one (coop_host, a level
	// change): no load then
	if (s_loadState == eLS_WaitLevel && stricmp(ShortLevelName(levelName), s_load.level.c_str()))
	{
		CryLogAlways("[CoopSave] %s is not the saved level %s: continue cancelled", levelName, s_load.level.c_str());
		s_loadState = eLS_None;
	}
}

void CoopSave::OnLevelReady(const char* levelName)
{
	s_readyLevel = ShortLevelName(levelName);
	s_levelStartSaved = s_loadState == eLS_WaitLevel;
}

void CoopSave::RequestCheckpoint(const char* name)
{
	if (!gEnv->bServer || s_readyLevel.empty() || s_loadState != eLS_None)
		return;
	s_pending = true;
	s_pendingName = name && name[0] ? name : "checkpoint";
	s_pendingFor = 0.0f;
	s_retryTimer = 0.0f;
	CryLogAlways("[CoopSave] checkpoint %s reached", s_pendingName.c_str());
}

bool CoopSave::IsLoadPending()
{
	return s_loadState != eLS_None;
}

bool CoopSave::IsCoopSaveName(const char* name)
{
	return name && CryStringUtils::stristr(name, "coop_checkpoint_") != 0;
}

void CoopSave::Update(float frameTime)
{
	if (!gEnv->bServer || s_readyLevel.empty() || !g_pGame)
		return;
	IGameFramework* pFramework = g_pGame->GetIGameFramework();

	if (s_loadState == eLS_WaitLevel)
	{
		// once the host plays the level (his player spawned)
		s_loadWait += frameTime;
		IActor* pHost = pFramework->GetClientActor();
		if (pFramework->IsGameStarted() && pHost && pHost->GetHealth() > 0 && s_loadWait > 1.0f)
			LoadNow();
		else if (s_loadWait > LOAD_TIMEOUT)
		{
			CryLogAlways("[CoopSave] the level did not get ready: the saved game is not loaded");
			s_loadState = eLS_None;
		}
		return;
	}

	if (!s_pCheckpoints || !s_pCheckpoints->GetIVal())
		return;
	if (!s_levelStartSaved && pFramework->IsGameStarted())
	{
		s_levelStartSaved = true;
		if (!s_pending)
			RequestCheckpoint((s_readyLevel + "_start").c_str());
	}
	if (!s_pending)
		return;
	s_pendingFor += frameTime;
	s_retryTimer -= frameTime;
	if (s_retryTimer > 0.0f)
		return;
	s_retryTimer = 1.0f;
	if (const char* why = CannotSave())
	{
		if (s_pendingFor > 300.0f)
		{
			CryLogAlways("[CoopSave] checkpoint %s dropped (%s for 5 minutes)", s_pendingName.c_str(), why);
			s_pending = false;
		}
		return;
	}
	s_pending = false;
	SaveCheckpoint(s_pendingName);
}
