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
#include "CoopRevive.h"
#include "CoopAI.h"
#include "CoopCloud.h"
#include "CoopRelay.h"
#include "Game.h"
#include "GameRules.h"
#include "IActorSystem.h"
#include "IItemSystem.h"
#include "ILevelSystem.h"
#include "IViewSystem.h"
#include "IPlayerProfiles.h"
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
		CoopAI::ResetHitchTimer();
		CryLogAlways("[CoopSave] after the load: player limit %d %s (%d candidates); game clock of the save %.1f s, before the load %.1f s", s_maxPlayers,
			restored == 1 ? "restored" : restored ? "restored in several places" : "NOT FOUND", (int)s_fields.size(), gameTime, s_gameTimeBefore);
		s_fields.clear();
		return restored;
	}

	// ------------------------------------------------------------------
	// campaigns: every game started with coop_host is a campaign of its own,
	// kept in %USER%/SaveGames/coop/<id>/ (progress.txt: the last checkpoint,
	// progress_prev.txt: the one before). Its savegames are
	// coop_checkpoint_<id>_a / _b, used in turn.

	const char* const COOP_DIR = "%USER%/SaveGames/coop";
	const char* const LEGACY_PROGRESS = "%USER%/SaveGames/coop_progress.txt";
	const char* const LEGACY_PROGRESS_PREV = "%USER%/SaveGames/coop_progress_prev.txt";
	const char* const SAVE_EXT = ".CRYSISJMSF";
	const char* const LEVELS[] = { "island", "village", "rescue", "harbor", "tank", "mine", "core", "ice", "sphere", "ascension", "fleet" };

	struct SProgress
	{
		string campaign;    // 16 hex digits
		string name;
		string level;       // short name: rescue
		string save;        // CryAction savegame name
		string checkpoint;
		string time;
		unsigned int stamp; // unix time of the checkpoint
		string host;        // the host's player key (CoopAI::PlayerKey)
		string hostname;
		std::map<string, CoopAI::SInventory> players;   // by player key, the host too
		SProgress() : stamp(0) {}
	};

	string UserPath(const char* path)
	{
		char buf[ICryPak::g_nMaxPath];
		const char* p = gEnv->pCryPak->AdjustFileName(path, buf, ICryPak::FLAGS_NO_MASTER_FOLDER_MAPPING | ICryPak::FLAGS_FOR_WRITING);
		return p ? string(p) : string(path);
	}

	string CampaignFile(const string& campaign, const char* file)
	{
		return UserPath((string(COOP_DIR) + "/" + campaign + "/" + file).c_str());
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

	unsigned int ParseTime(const string& text)
	{
		struct tm t;
		memset(&t, 0, sizeof(t));
		if (sscanf(text.c_str(), "%d-%d-%d %d:%d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec) != 6)
			return 0;
		t.tm_year -= 1900;
		t.tm_mon -= 1;
		t.tm_isdst = -1;
		const time_t v = mktime(&t);
		return v > 0 ? (unsigned int)v : 0;
	}

	string ProgressText(const SProgress& p)
	{
		string text;
		text.Format("version=2\ncampaign=%s\nname=%s\nlevel=%s\nsave=%s\ncheckpoint=%s\ntime=%s\nstamp=%u\nhost=%s\nhostname=%s\n",
			p.campaign.c_str(), p.name.c_str(), p.level.c_str(), p.save.c_str(), p.checkpoint.c_str(), p.time.c_str(), p.stamp,
			p.host.c_str(), p.hostname.c_str());
		for (std::map<string, CoopAI::SInventory>::const_iterator it = p.players.begin(); it != p.players.end(); ++it)
		{
			const CoopAI::SInventory& inv = it->second;
			string items, ammo, line;
			for (size_t i = 0; i < inv.items.size(); ++i)
				items += (i ? "," : "") + inv.items[i];
			for (size_t i = 0; i < inv.ammo.size(); ++i)
			{
				string a;
				a.Format("%s%s:%d", i ? "," : "", inv.ammo[i].first.c_str(), inv.ammo[i].second);
				ammo += a;
			}
			line.Format("player=%s\t%s\t%s\t%s\t%s\n", it->first.c_str(), inv.name.c_str(), inv.current.c_str(), items.c_str(), ammo.c_str());
			text += line;
		}
		return text;
	}

	void ParseProgress(const string& text, SProgress& p)
	{
		std::vector<string> lines, fields, list, pair;
		Split(text, '\n', lines);
		int version = 1;
		for (size_t l = 0; l < lines.size(); ++l)
		{
			const string s = string(lines[l]).TrimRight("\r");
			const size_t eq = s.find('=');
			if (eq == string::npos)
				continue;
			const string key = s.substr(0, eq), value = s.substr(eq + 1);
			if (key == "version")
				version = atoi(value.c_str());
			else if (key == "campaign")
				p.campaign = value;
			else if (key == "name")
				p.name = value;
			else if (key == "level")
				p.level = value;
			else if (key == "save")
				p.save = value;
			else if (key == "checkpoint")
				p.checkpoint = value;
			else if (key == "time")
				p.time = value;
			else if (key == "stamp")
				p.stamp = (unsigned int)strtoul(value.c_str(), 0, 10);
			else if (key == "host")
				p.host = value;
			else if (key == "hostname")
				p.hostname = value;
			else if (key == "player")
			{
				// version 1 (mod 0.3): name current items ammo
				// version 2: key name current items ammo
				Split(value, '\t', fields);
				if (version < 2)
					fields.insert(fields.begin(), "name:" + (fields.empty() ? string() : fields[0]));
				if (fields.size() < 5 || fields[0].length() < 4)
					continue;
				CoopAI::SInventory& inv = p.players[fields[0]];
				inv.name = fields[1];
				inv.current = fields[2];
				Split(fields[3], ',', list);
				for (size_t i = 0; i < list.size(); ++i)
					if (!list[i].empty())
						inv.items.push_back(list[i]);
				Split(fields[4], ',', list);
				for (size_t i = 0; i < list.size(); ++i)
				{
					Split(list[i], ':', pair);
					if (pair.size() == 2 && !pair[0].empty())
						inv.ammo.push_back(std::make_pair(pair[0], atoi(pair[1].c_str())));
				}
			}
		}
		if (!p.stamp)
			p.stamp = ParseTime(p.time);
	}

	bool ReadFile(const string& path, std::vector<char>& out)
	{
		FILE* f = fopen(path.c_str(), "rb");
		if (!f)
			return false;
		fseek(f, 0, SEEK_END);
		const long size = ftell(f);
		fseek(f, 0, SEEK_SET);
		out.resize(size > 0 ? size : 0);
		const bool ok = size <= 0 || fread(&out[0], 1, size, f) == (size_t)size;
		fclose(f);
		return ok;
	}

	bool WriteFile(const string& path, const void* data, size_t size)
	{
		FILE* f = fopen(path.c_str(), "wb");
		if (!f)
			return false;
		const bool ok = !size || fwrite(data, 1, size, f) == size;
		fclose(f);
		return ok;
	}

	bool ReadProgress(const string& path, SProgress& p)
	{
		std::vector<char> data;
		if (!ReadFile(path, data) || data.empty())
			return false;
		ParseProgress(string(&data[0], data.size()), p);
		return !p.level.empty() && !p.save.empty();
	}

	bool WriteProgress(const string& path, const SProgress& p)
	{
		const string text = ProgressText(p);
		if (WriteFile(path, text.c_str(), text.length()))
			return true;
		CryLogAlways("[CoopSave] cannot write %s", path.c_str());
		return false;
	}

	// where CryAction keeps a savegame of this profile (the folder the last
	// save went to, else the profile manager's rule: CGame::OnSaveGame)
	string s_engineSaveDir;         // resolved, with the profile prefix: ".../SaveGames/default_"
	string EngineSavePath(const string& save)
	{
		if (!s_engineSaveDir.empty())
			return s_engineSaveDir + save;
		IPlayerProfileManager* pManager = g_pGame->GetIGameFramework()->GetIPlayerProfileManager();
		const char* user = pManager ? pManager->GetCurrentUser() : 0;
		IPlayerProfile* pProfile = pManager && user ? pManager->GetCurrentProfile(user) : 0;
		const char* profile = pProfile ? pProfile->GetName() : "default";
		const char* shared = pManager ? pManager->GetSharedSaveGameFolder() : 0;
		if (shared && *shared)
			return UserPath((string(shared) + "/" + profile + "_" + save).c_str());
		return UserPath((string("%USER%/Profiles/") + profile + "/SaveGames/" + save).c_str());
	}

	void MakeDirs(const string& campaign)
	{
		gEnv->pCryPak->MakeDir(UserPath(COOP_DIR).c_str());
		gEnv->pCryPak->MakeDir(CampaignFile(campaign, "").c_str());
	}

	// the campaigns on this PC, newest first
	void ListLocal(std::vector<SProgress>& out)
	{
		out.clear();
		WIN32_FIND_DATAA fd;
		HANDLE h = FindFirstFileA((UserPath(COOP_DIR) + "/*").c_str(), &fd);
		if (h == INVALID_HANDLE_VALUE)
			return;
		do
		{
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || strlen(fd.cFileName) != 16)
				continue;
			SProgress p;
			if (ReadProgress(CampaignFile(fd.cFileName, "progress.txt"), p))
			{
				p.campaign = fd.cFileName;
				out.push_back(p);
			}
		}
		while (FindNextFileA(h, &fd));
		FindClose(h);
		for (size_t i = 0; i < out.size(); ++i)
			for (size_t j = i + 1; j < out.size(); ++j)
				if (out[j].stamp > out[i].stamp)
					std::swap(out[i], out[j]);
	}

	// the progress of mod 0.3 (one campaign, no id) becomes a campaign
	void ImportLegacy()
	{
		SProgress p;
		if (!ReadProgress(UserPath(LEGACY_PROGRESS), p))
			return;
		p.campaign = CoopRelay::RandomHex(8);
		p.name = "Campaign (0.3)";
		MakeDirs(p.campaign);
		WriteProgress(CampaignFile(p.campaign, "progress.txt"), p);
		SProgress prev;
		if (ReadProgress(UserPath(LEGACY_PROGRESS_PREV), prev))
		{
			prev.campaign = p.campaign;
			prev.name = p.name;
			WriteProgress(CampaignFile(p.campaign, "progress_prev.txt"), prev);
		}
		DeleteFileA(UserPath(LEGACY_PROGRESS).c_str());
		DeleteFileA(UserPath(LEGACY_PROGRESS_PREV).c_str());
		CryLogAlways("[CoopSave] the co-op progress of mod 0.3 is now the campaign \"%s\"", p.name.c_str());
	}

	// ------------------------------------------------------------------
	// saving

	ICVar* s_pCheckpoints = 0;
	bool s_pending = false;
	string s_pendingName;
	float s_pendingFor = 0.0f;

	// Before a checkpoint is saved the soldiers forget the other players
	// (friends, the AI companion): those are not in the savegame, and a
	// soldier who still had one of them as a target came back from the load
	// with an empty target: the AI system crashed on the first update
	// (CPuppet::UpdatePuppetInternalState). Their AI objects are switched off
	// a moment (the soldiers drop them as they do a downed player), the game
	// is saved, and they are switched on again.
	std::vector<EntityId> s_quieted;
	float s_quietUntil = -1.0f;

	bool QuietRemotePlayers()
	{
		s_quieted.clear();
		IGameFramework* pFramework = g_pGame->GetIGameFramework();
		IActor* pLocal = pFramework->GetClientActor();
		IActorIteratorPtr pIt = pFramework->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = pIt->Next())
		{
			if (!pActor->IsPlayer() || pActor == pLocal)
				continue;
			IAIObject* pAI = pActor->GetEntity()->GetAI();
			if (pAI && pAI->IsEnabled())
			{
				pAI->Event(AIEVENT_DISABLE, 0);
				s_quieted.push_back(pActor->GetEntityId());
			}
		}
		return !s_quieted.empty();
	}

	void UnquietRemotePlayers()
	{
		for (size_t i = 0; i < s_quieted.size(); ++i)
		{
			IActor* pActor = g_pGame->GetIGameFramework()->GetIActorSystem()->GetActor(s_quieted[i]);
			IAIObject* pAI = pActor ? pActor->GetEntity()->GetAI() : 0;
			// a downed one stays off (no target while he waits to be revived)
			if (pAI && !pAI->IsEnabled() && pActor->GetHealth() > 0)
				pAI->Event(AIEVENT_ENABLE, 0);
		}
		s_quieted.clear();
	}
	float s_retryTimer = 0.0f;
	string s_readyLevel;            // the coop level the server runs
	bool s_levelStartSaved = false;
	// the campaign the host plays (empty: none yet, the first checkpoint
	// starts one)
	string s_campaign;
	string s_campaignName;
	string s_lastSave;              // the savegame slot written last
	string s_lastEngineFile;        // CryAction's file of the last savegame
	string s_menuStatus;            // what the co-op menu shows as the last result

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

	void StartCampaign(const string& name)
	{
		std::vector<SProgress> local;
		ListLocal(local);
		s_campaign = CoopRelay::RandomHex(8);
		s_campaignName = name;
		if (s_campaignName.empty())
			s_campaignName.Format("Campaign %d", (int)local.size() + 1);
		s_lastSave.clear();
		CryLogAlways("[CoopSave] new campaign \"%s\" (%s)", s_campaignName.c_str(), s_campaign.c_str());
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

	// on the host's own screen (in the game) and in the console
	void TellHost(const char* msg)
	{
		CryLogAlways("[CoopSave] %s", msg);
		if (g_pGame->GetIGameFramework()->IsGameStarted())
			if (CGameRules* pRules = g_pGame->GetGameRules())
				pRules->OnTextMessage(eTextMessageError, msg);
	}

	// a checkpoint for the cloud: "CCSV" ver progress_len progress save_len save
	void UploadCheckpoint(const SProgress& p)
	{
		std::vector<char> save;
		const string savePath = !s_lastEngineFile.empty() ? s_lastEngineFile : EngineSavePath(p.save);
		if (!ReadFile(savePath, save) || save.empty())
		{
			CryLogAlways("[CoopCloud] savegame %s not found: the checkpoint stays on this PC", savePath.c_str());
			return;
		}
		const string text = ProgressText(p);
		std::vector<char> blob;
		blob.reserve(16 + text.length() + save.size());
		const unsigned int head[3] = { 1, (unsigned int)text.length(), 0 };
		blob.insert(blob.end(), "CCSV", "CCSV" + 4);
		blob.insert(blob.end(), (const char*)&head[0], (const char*)&head[0] + 8);
		blob.insert(blob.end(), text.begin(), text.end());
		const unsigned int saveLen = (unsigned int)save.size();
		blob.insert(blob.end(), (const char*)&saveLen, (const char*)&saveLen + 4);
		blob.insert(blob.end(), save.begin(), save.end());
		CoopCloud::Upload(p.campaign, blob);
	}

	bool SaveCheckpoint(const string& name)
	{
		IGameFramework* pFramework = g_pGame->GetIGameFramework();
		if (s_campaign.empty())
			StartCampaign("");
		string save;
		save.Format("coop_checkpoint_%s_%c%s", s_campaign.c_str(), s_lastSave.length() > strlen(SAVE_EXT) + 1 && s_lastSave[s_lastSave.length() - strlen(SAVE_EXT) - 1] == 'a' ? 'b' : 'a', SAVE_EXT);

		SProgress progress;
		progress.campaign = s_campaign;
		progress.name = s_campaignName;
		progress.level = s_readyLevel;
		progress.save = save;
		progress.checkpoint = name;
		char stamp[64];
		const time_t now = time(0);
		strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
		progress.time = stamp;
		progress.stamp = (unsigned int)now;
		if (IActor* pHost = pFramework->GetClientActor())
		{
			progress.host = CoopAI::PlayerKey(pHost);
			progress.hostname = pHost->GetEntity()->GetName();
		}
		CoopAI::CollectInventories(progress.players, true);

		std::vector<EntityId> unsaved;
		MarkRemotePlayersUnsaved(unsaved);
		bool ok;
		s_lastEngineFile.clear();
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

		// the previous checkpoint stays as progress_prev.txt (its savegame is the
		// other slot)
		MakeDirs(s_campaign);
		const string cur = CampaignFile(s_campaign, "progress.txt"), prev = CampaignFile(s_campaign, "progress_prev.txt");
		MoveFileExA(cur.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);
		WriteProgress(cur, progress);
		s_lastSave = save;
		CryLogAlways("[CoopSave] checkpoint %s saved: campaign \"%s\", %s, %s (%d players' equipment)", name.c_str(), s_campaignName.c_str(),
			progress.level.c_str(), save.c_str(), (int)progress.players.size());
		s_menuStatus.Format("Saved: checkpoint %s, %s", name.c_str(), stamp);
		Notify("@game_saved");
		if (CoopCloud::Enabled())
			UploadCheckpoint(progress);
		return true;
	}

	// ------------------------------------------------------------------
	// coop_continue / coop_campaigns: the campaigns on this PC and in the
	// cloud, the newest checkpoint of the one chosen (downloaded if the cloud
	// has a newer one), then its level is hosted and the save loaded into it

	enum ELoadState
	{
		eLS_None,
		eLS_WaitMap,    // the level is about to be hosted (CoopRelay::RestartGame)
		eLS_WaitLevel,  // the level is being hosted
		eLS_Loading,
	};
	ELoadState s_loadState = eLS_None;
	SProgress s_load;
	float s_loadWait = 0.0f;
	const float LOAD_TIMEOUT = 180.0f;

	struct SEntry
	{
		string campaign, name, level, checkpoint, host;
		unsigned int localStamp, cloudStamp;
		bool cloudOwn, cloudPrevious;
		SEntry() : localStamp(0), cloudStamp(0), cloudOwn(false), cloudPrevious(false) {}
		unsigned int Stamp() const { return localStamp > cloudStamp ? localStamp : cloudStamp; }
	};

	void Merge(const std::vector<SProgress>& local, const std::vector<CoopCloud::SCampaign>& cloud, std::vector<SEntry>& out)
	{
		out.clear();
		for (size_t i = 0; i < local.size(); ++i)
		{
			SEntry e;
			e.campaign = local[i].campaign;
			e.name = local[i].name;
			e.level = local[i].level;
			e.checkpoint = local[i].checkpoint;
			e.host = local[i].hostname;
			e.localStamp = local[i].stamp;
			out.push_back(e);
		}
		for (size_t i = 0; i < cloud.size(); ++i)
		{
			const CoopCloud::SCampaign& c = cloud[i];
			SEntry* pEntry = 0;
			for (size_t j = 0; j < out.size() && !pEntry; ++j)
				if (out[j].campaign == c.id)
					pEntry = &out[j];
			if (!pEntry)
			{
				out.push_back(SEntry());
				pEntry = &out.back();
				pEntry->campaign = c.id;
			}
			pEntry->cloudStamp = c.stamp;
			pEntry->cloudOwn = c.own;
			pEntry->cloudPrevious = c.hasPrevious;
			if (c.stamp >= pEntry->localStamp)
			{
				pEntry->name = c.name;
				pEntry->level = c.level;
				pEntry->checkpoint = c.checkpoint;
				pEntry->host = c.host;
			}
		}
		for (size_t i = 0; i < out.size(); ++i)
			for (size_t j = i + 1; j < out.size(); ++j)
				if (out[j].Stamp() > out[i].Stamp())
					std::swap(out[i], out[j]);
	}

	string Describe(const SEntry& e, int number)
	{
		char when[32] = "?";
		const time_t t = (time_t)e.Stamp();
		if (t)
			strftime(when, sizeof(when), "%Y-%m-%d %H:%M", localtime(&t));
		string where;
		if (e.localStamp && e.cloudStamp)
			where = e.cloudStamp > e.localStamp ? "this PC, newer in the cloud" : "this PC and the cloud";
		else if (e.localStamp)
			where = "this PC";
		else
			where = e.cloudOwn ? string("the cloud") : "the cloud, hosted by " + e.host;
		string text;
		text.Format("%d. \"%s\" - %s, checkpoint %s, %s (%s)", number, e.name.c_str(), e.level.c_str(), e.checkpoint.c_str(), when, where.c_str());
		return text;
	}

	enum EContinueState { eCS_None, eCS_Listing, eCS_Downloading };
	EContinueState s_contState = eCS_None;
	bool s_contListOnly = false;        // coop_campaigns: print the list only
	bool s_contForMenu = false;         // the co-op menu's list: not printed
	bool s_menuListReady = false;

	// a message for the player: the console, and the co-op menu's status line
	void Status(const char* text)
	{
		CryLogAlways("[CoopSave] %s", text);
		s_menuStatus = text;
	}
	bool s_contDelete = false;          // coop_campaign_delete
	string s_contArg;                   // the campaign asked for ("": the newest)
	bool s_contPrev = false;
	float s_contSince = 0.0f;
	SEntry s_contEntry;
	std::vector<SEntry> s_contList;

	void HostCheckpoint(const SProgress& progress)
	{
		CryLogAlways("[CoopSave] continuing campaign \"%s\": %s, checkpoint %s (saved %s)", progress.name.c_str(), progress.level.c_str(),
			progress.checkpoint.c_str(), progress.time.c_str());
		s_load = progress;
		s_loadState = eLS_WaitMap;
		s_loadWait = 0.0f;
		s_campaign = progress.campaign;
		s_campaignName = progress.name;
		s_lastSave = progress.save;
		gEnv->pConsole->ExecuteString("exec coop_settings.cfg");
		string cmd;
		cmd.Format("map multiplayer/tia/coop_%s s", progress.level.c_str());
		CoopRelay::RestartGame(cmd.c_str());
	}

	// the checkpoint of the chosen campaign on this PC (and hosted)
	void ContinueLocal(const SEntry& e, bool prev)
	{
		SProgress p;
		if (!ReadProgress(CampaignFile(e.campaign, prev ? "progress_prev.txt" : "progress.txt"), p))
		{
			const char* text = prev ? "This campaign has no previous checkpoint on this PC" : "The campaign's checkpoint is missing on this PC";
			TellHost(text);
			s_menuStatus = text;
			return;
		}
		p.campaign = e.campaign;
		HostCheckpoint(p);
	}

	// a downloaded checkpoint: stored as the campaign's newest on this PC
	bool ApplyDownload(const SEntry& e, const std::vector<char>& blob, bool prev)
	{
		unsigned int version = 0, plen = 0, slen = 0;
		if (blob.size() < 16 || memcmp(&blob[0], "CCSV", 4))
			return false;
		memcpy(&version, &blob[4], 4);
		memcpy(&plen, &blob[8], 4);
		if (12 + (size_t)plen + 4 > blob.size())
			return false;
		memcpy(&slen, &blob[12 + plen], 4);
		if (12 + (size_t)plen + 4 + slen != blob.size())
			return false;
		SProgress p;
		ParseProgress(string(&blob[12], plen), p);
		if (p.save.empty() || p.level.empty() || !CoopSave::IsCoopSaveName(p.save.c_str()))
			return false;
		p.campaign = e.campaign;
		const string savePath = EngineSavePath(p.save);
		if (!WriteFile(savePath, &blob[16 + plen], slen))
		{
			CryLogAlways("[CoopCloud] cannot write %s", savePath.c_str());
			return false;
		}
		MakeDirs(e.campaign);
		const string cur = CampaignFile(e.campaign, "progress.txt");
		if (!prev)
			MoveFileExA(cur.c_str(), CampaignFile(e.campaign, "progress_prev.txt").c_str(), MOVEFILE_REPLACE_EXISTING);
		WriteProgress(prev ? CampaignFile(e.campaign, "progress_prev.txt") : cur, p);
		CryLogAlways("[CoopCloud] checkpoint %s of campaign \"%s\" downloaded (%d KB)", p.checkpoint.c_str(), p.name.c_str(), (int)(blob.size() / 1024));
		return true;
	}

	void StartListing(bool listOnly, bool remove, const string& arg, bool prev, bool forMenu = false)
	{
		ImportLegacy();
		s_contForMenu = forMenu;
		if (forMenu)
			s_menuListReady = false;
		else
			s_menuStatus.clear();
		s_contListOnly = listOnly;
		s_contDelete = remove;
		s_contArg = arg;
		s_contPrev = prev;
		s_contSince = gEnv->pTimer->GetAsyncCurTime();
		s_contState = eCS_Listing;
		if (CoopCloud::Enabled())
		{
			CryLogAlways("[CoopSave] looking for the campaigns (this PC and the cloud)...");
			CoopCloud::RequestList();
		}
	}

	// the campaign asked for: a number of the list, its name or its id
	const SEntry* Pick(const std::vector<SEntry>& list, const string& arg)
	{
		if (list.empty())
			return 0;
		if (arg.empty())
			return &list[0];
		const int number = atoi(arg.c_str());
		if (number >= 1 && number <= (int)list.size() && string().Format("%d", number) == arg)
			return &list[number - 1];
		for (size_t i = 0; i < list.size(); ++i)
			if (!stricmp(list[i].name.c_str(), arg.c_str()) || !stricmp(list[i].campaign.c_str(), arg.c_str()))
				return &list[i];
		return 0;
	}

	void DeleteCampaign(const SEntry& e)
	{
		if (e.localStamp)
		{
			for (int slot = 0; slot < 2; ++slot)
			{
				string save;
				save.Format("coop_checkpoint_%s_%c%s", e.campaign.c_str(), 'a' + slot, SAVE_EXT);
				const string path = EngineSavePath(save);
				DeleteFileA(path.c_str());
				DeleteFileA((path.substr(0, path.length() - strlen(SAVE_EXT)) + ".xml").c_str());
			}
			DeleteFileA(CampaignFile(e.campaign, "progress.txt").c_str());
			DeleteFileA(CampaignFile(e.campaign, "progress_prev.txt").c_str());
			RemoveDirectoryA(CampaignFile(e.campaign, "").c_str());
		}
		if (e.cloudStamp)
			CoopCloud::RequestDelete(e.campaign);
		string text;
		text.Format("Campaign \"%s\" deleted%s", e.name.c_str(), e.cloudStamp ? (e.cloudOwn ? " (also from the cloud)" : " (and off your cloud list)") : "");
		Status(text.c_str());
		if (e.campaign == s_campaign)
			s_campaign.clear();
	}

	void UpdateContinue()
	{
		if (s_contState == eCS_Listing)
		{
			std::vector<CoopCloud::SCampaign> cloud;
			const CoopCloud::EStatus status = CoopCloud::Enabled() ? CoopCloud::ListStatus(&cloud) : CoopCloud::eS_Failed;
			const bool timedOut = gEnv->pTimer->GetAsyncCurTime() - s_contSince > 8.0f;
			if (status == CoopCloud::eS_Busy && !timedOut)
				return;
			s_contState = eCS_None;
			std::vector<SProgress> local;
			ListLocal(local);
			Merge(local, cloud, s_contList);
			if (s_contListOnly && s_contForMenu)
			{
				s_menuListReady = true;
				return;
			}
			if (s_contListOnly)
			{
				if (s_contList.empty())
					CryLogAlways("No co-op campaigns yet. Start one with: coop_host");
				for (size_t i = 0; i < s_contList.size(); ++i)
					CryLogAlways("%s", Describe(s_contList[i], (int)i + 1).c_str());
				if (status != CoopCloud::eS_Done && CoopCloud::Enabled())
					CryLogAlways("(the cloud did not answer: campaigns on this PC only)");
				return;
			}
			const SEntry* pEntry = Pick(s_contList, s_contArg);
			if (!pEntry)
			{
				Status(s_contList.empty() ? "No saved co-op progress. Start a new game with: coop_host"
					: "No such campaign (coop_campaigns lists them)");
				return;
			}
			s_contEntry = *pEntry;
			if (s_contDelete)
			{
				DeleteCampaign(s_contEntry);
				return;
			}
			const bool cloudNewer = s_contEntry.cloudStamp > s_contEntry.localStamp && (!s_contPrev || s_contEntry.cloudPrevious);
			if (cloudNewer)
			{
				Status("Downloading the campaign's checkpoint from the cloud...");
				CoopCloud::RequestDownload(s_contEntry.campaign, s_contPrev ? 1 : 0);
				s_contState = eCS_Downloading;
				return;
			}
			ContinueLocal(s_contEntry, s_contPrev);
		}
		else if (s_contState == eCS_Downloading)
		{
			std::vector<char> blob;
			const CoopCloud::EStatus status = CoopCloud::DownloadStatus(&blob);
			if (status == CoopCloud::eS_Busy)
				return;
			s_contState = eCS_None;
			if (status == CoopCloud::eS_Done && ApplyDownload(s_contEntry, blob, s_contPrev))
				ContinueLocal(s_contEntry, s_contPrev);
			else if (s_contEntry.localStamp)
			{
				Status("The cloud's checkpoint is not available: continuing from the one on this PC");
				ContinueLocal(s_contEntry, s_contPrev);
			}
			else
				Status("The campaign could not be downloaded");
		}
	}

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
			const DWORD t0 = GetTickCount();
			ok = pFramework->LoadGame(s_load.save.c_str(), true, true);
			CryLogAlways("[CoopSave] the saved game took %.1f s to load", (GetTickCount() - t0) / 1000.0f);
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
		// whoever hosts now gets his own equipment (the save has the one of
		// the player who hosted then, who gets his back when he joins)
		std::map<string, CoopAI::SInventory> carry = s_load.players;
		if (IActor* pHost = pFramework->GetClientActor())
		{
			const string key = CoopAI::PlayerKey(pHost);
			std::map<string, CoopAI::SInventory>::iterator own = carry.find(key);
			if (key != s_load.host && own != carry.end())
			{
				CoopAI::GiveInventory(pHost, own->second, true);
				CryLogAlways("[CoopSave] %s hosts this time: his own equipment (%d items)", pHost->GetEntity()->GetName(), (int)own->second.items.size());
			}
			if (own != carry.end())
				carry.erase(own);
		}
		CoopAI::SetInventoryCarry(s_load.level.c_str(), carry);
		CoopAI::OnGameLoaded();
		CryLogAlways("[CoopSave] progress loaded: %s, checkpoint %s (saved %s)", s_load.level.c_str(), s_load.checkpoint.c_str(), s_load.time.c_str());
		// where the team goes back to when everybody is down
		CoopRevive::OnCheckpoint("loaded");
	}

	// a new campaign on this level (short name)
	void HostNew(const char* level, const string& name)
	{
		ImportLegacy();
		StartCampaign(name);
		s_loadState = eLS_None;
		s_contState = eCS_None;
		s_menuStatus.clear();
		gEnv->pConsole->ExecuteString("exec coop_settings.cfg");
		string cmd;
		cmd.Format("map multiplayer/tia/coop_%s s", level);
		CoopRelay::RestartGame(cmd.c_str());
	}

	// coop_host [level] [campaign name]: a new campaign
	void CmdHost(IConsoleCmdArgs* pArgs)
	{
		const char* level = LEVELS[0];
		int first = 1;
		if (pArgs->GetArgCount() > 1)
		{
			const char* arg = pArgs->GetArg(1);
			const int number = atoi(arg);
			for (int i = 0; i < 11; ++i)
				if (number == i + 1 || !stricmp(arg, LEVELS[i]))
				{
					level = LEVELS[i];
					first = 2;
				}
		}
		string name;
		for (int i = first; i < pArgs->GetArgCount(); ++i)
			name += (name.empty() ? "" : " ") + string(pArgs->GetArg(i));
		HostNew(level, name);
	}

	// coop_continue [campaign] [prev]
	void CmdContinue(IConsoleCmdArgs* pArgs)
	{
		string arg;
		bool prev = false;
		for (int i = 1; i < pArgs->GetArgCount(); ++i)
		{
			if (!stricmp(pArgs->GetArg(i), "prev"))
				prev = true;
			else
				arg += (arg.empty() ? "" : " ") + string(pArgs->GetArg(i));
		}
		StartListing(false, false, arg, prev);
	}

	// coop_load [prev]: the host goes back to the last checkpoint of the
	// campaign he plays; the friends join again by themselves
	void CmdLoad(IConsoleCmdArgs* pArgs)
	{
		CoopSave::LoadCheckpoint(pArgs->GetArgCount() > 1 && !stricmp(pArgs->GetArg(1), "prev"));
	}

	void CmdCampaigns(IConsoleCmdArgs*)
	{
		StartListing(true, false, "", false);
	}

	void CmdDelete(IConsoleCmdArgs* pArgs)
	{
		string arg;
		for (int i = 1; i < pArgs->GetArgCount(); ++i)
			arg += (arg.empty() ? "" : " ") + string(pArgs->GetArg(i));
		if (arg.empty())
		{
			CryLogAlways("usage: coop_campaign_delete <number or name>   (see coop_campaigns)");
			return;
		}
		StartListing(false, true, arg, false);
	}

	void CmdSave(IConsoleCmdArgs*)
	{
		CoopSave::SaveNow();
	}
}

void CoopSave::Init()
{
	s_pCheckpoints = gEnv->pConsole->RegisterInt("coop_checkpoints", 1, 0,
		"Crysis Coop: 1 = the host saves the campaign progress at its checkpoints and at every level start (coop_continue)");
	gEnv->pConsole->AddCommand("coop_host", CmdHost, 0,
		"Crysis Coop: host a new co-op campaign: coop_host [level] [campaign name]  (default: the first level)");
	gEnv->pConsole->AddCommand("coop_continue", CmdContinue, 0,
		"Crysis Coop: host a co-op campaign from its last checkpoint: coop_continue [number or name] [prev]  (default: the newest)");
	gEnv->pConsole->AddCommand("coop_load", CmdLoad, 0,
		"Crysis Coop: the host goes back to the last checkpoint (coop_load prev: the one before); friends join again by themselves");
	gEnv->pConsole->AddCommand("coop_campaigns", CmdCampaigns, 0, "Crysis Coop: the co-op campaigns on this PC and in the cloud");
	gEnv->pConsole->AddCommand("coop_campaign_delete", CmdDelete, 0, "Crysis Coop: delete a co-op campaign: coop_campaign_delete <number or name>");
	gEnv->pConsole->AddCommand("coop_save", CmdSave, 0, "Crysis Coop: the host saves the co-op progress now");
}

void CoopSave::OnLoadingStart(const char* levelName)
{
	s_readyLevel.clear();
	s_pending = false;
	s_levelStartSaved = false;
	s_quietUntil = -1.0f;
	s_quieted.clear();
	// coop_continue: some other level than the saved one (coop_host, a level
	// change): no load then
	if ((s_loadState == eLS_WaitMap || s_loadState == eLS_WaitLevel) && stricmp(ShortLevelName(levelName), s_load.level.c_str()))
	{
		CryLogAlways("[CoopSave] %s is not the saved level %s: continue cancelled", levelName, s_load.level.c_str());
		s_loadState = eLS_None;
	}
	else if (s_loadState == eLS_WaitMap)
		s_loadState = eLS_WaitLevel;
}

void CoopSave::OnLevelReady(const char* levelName)
{
	s_readyLevel = ShortLevelName(levelName);
	s_levelStartSaved = s_loadState == eLS_WaitLevel;
}

void CoopSave::OnEngineSave(const char* file)
{
	if (!IsCoopSaveName(file))
		return;
	s_lastEngineFile = UserPath(file);
	// the folder and profile prefix the downloaded savegames go to
	const char* name = CryStringUtils::stristr(s_lastEngineFile.c_str(), "coop_checkpoint_");
	if (name)
		s_engineSaveDir = s_lastEngineFile.substr(0, name - s_lastEngineFile.c_str());
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
	// the place the team goes back to when everybody is down; at a story
	// checkpoint the downed come back (the rules decide, see CoopOnCheckpoint)
	CoopRevive::OnCheckpoint(s_pendingName.c_str());
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
	if (!g_pGame)
		return;
	UpdateContinue();
	if (!gEnv->bServer || s_readyLevel.empty())
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
	// first the soldiers forget the other players (see QuietRemotePlayers)
	if (s_quietUntil < 0.0f)
	{
		if (QuietRemotePlayers())
		{
			s_quietUntil = gEnv->pTimer->GetCurrTime() + 0.6f;
			s_retryTimer = 0.0f;
			return;
		}
	}
	else if (gEnv->pTimer->GetCurrTime() < s_quietUntil)
	{
		s_retryTimer = 0.0f;
		return;
	}
	s_quietUntil = -1.0f;
	s_pending = false;
	SaveCheckpoint(s_pendingName);
	UnquietRemotePlayers();
}

// ---------------------------------------------------------------------------
// the co-op menu (CoopMenu) and the console commands share these

void CoopSave::RequestCampaigns()
{
	if (s_contState == eCS_None)
		StartListing(true, false, "", false, true);
}

bool CoopSave::CampaignsReady(std::vector<SCampaignInfo>& out)
{
	if (!s_menuListReady || s_contState == eCS_Listing)
		return false;
	out.clear();
	for (size_t i = 0; i < s_contList.size(); ++i)
	{
		const SEntry& e = s_contList[i];
		SCampaignInfo c;
		c.id = e.campaign;
		c.name = e.name;
		c.level = e.level;
		c.checkpoint = e.checkpoint;
		c.host = e.host;
		c.stamp = e.Stamp();
		c.local = e.localStamp != 0;
		c.cloud = e.cloudStamp != 0;
		c.cloudNewer = e.cloudStamp > e.localStamp;
		c.own = !e.cloudStamp || e.cloudOwn;
		out.push_back(c);
	}
	return true;
}

bool CoopSave::IsBusy()
{
	return s_contState != eCS_None || s_loadState != eLS_None;
}

const char* CoopSave::MenuStatus()
{
	return s_menuStatus.c_str();
}

int CoopSave::LevelCount()
{
	return (int)(sizeof(LEVELS) / sizeof(LEVELS[0]));
}

const char* CoopSave::LevelName(int index)
{
	return index >= 0 && index < LevelCount() ? LEVELS[index] : "";
}

const char* CoopSave::LevelTitle(const char* level)
{
	static const char* titles[] = { "Contact", "Recovery", "Relic", "Assault", "Onslaught", "Awakening", "Core",
		"Paradise Lost", "Exodus", "Ascension", "Reckoning" };
	for (int i = 0; i < LevelCount(); ++i)
		if (level && !stricmp(level, LEVELS[i]))
			return titles[i];
	return level ? level : "";
}

void CoopSave::NewCampaign(const char* level, const char* name)
{
	HostNew(level && level[0] ? level : LEVELS[0], name ? name : "");
}

void CoopSave::ContinueCampaign(const char* id, bool previous)
{
	StartListing(false, false, id ? id : "", previous);
}

void CoopSave::DeleteCampaign(const char* id)
{
	if (id && id[0])
		StartListing(false, true, id, false);
}

void CoopSave::SaveNow()
{
	if (!gEnv->bServer || !CoopAI::IsCoopSession())
	{
		Status("Only the host of a co-op game can save");
		return;
	}
	RequestCheckpoint("manual");
	if (const char* why = CannotSave())
	{
		string text;
		text.Format("The game will be saved as soon as possible (%s)", why);
		Status(text.c_str());
	}
	else
		s_menuStatus = "Saving...";
}

void CoopSave::LoadCheckpoint(bool previous)
{
	if (!gEnv->bServer || s_campaign.empty())
	{
		Status("No campaign is being played here (Continue picks one)");
		return;
	}
	StartListing(false, false, s_campaign, previous);
}

const char* CoopSave::CurrentCampaign()
{
	return s_campaignName.c_str();
}
