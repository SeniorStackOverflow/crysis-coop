#include "StdAfx.h"
#include "IAISystem.h"
#include "IAgent.h"
#include "IEntitySystem.h"
#include "IEntityProxy.h"
#include "IActorSystem.h"
#include "IActionMapManager.h"
#include "IItemSystem.h"
#include "ILevelSystem.h"
#include "IVehicleSystem.h"
#include "Menus/FlashMenuObject.h"
#include "IVideoPlayer.h"
#include "IMaterial.h"
#include "IShader.h"
#include "StringUtils.h"
#include "ScriptHelpers.h"

#include <set>
#include "IFlowSystem.h"
#include "IGameTokens.h"
#include "IRenderer.h"
#include "ICryAnimation.h"
#include "IMovieSystem.h"
#include "IViewSystem.h"
#include "ISound.h"
#include "NanoSuit.h"
#include <stdarg.h>
#include <algorithm>
#include <map>

#include "CoopAI.h"
#include "CoopRelay.h"
#include "CoopSave.h"
#include "Game.h"
#include "GameCVars.h"
#include "GameRules.h"
#include "HUD/HUD.h"
#include "HUD/HUDRadar.h"
#include "Player.h"
#include "Item.h"
#include "Weapon.h"
#include "IPlayerInput.h"

namespace
{
	ICVar* s_pDebugFlags = nullptr;
	ICVar* s_pFlowMirror = nullptr;
	void RegisterFlowInspector();
	void ResetFlowMirror();
	int s_totalAIShots = 0;
	void ResetSync();
	void UpdateSequenceSync(float frameTime);
	void UpdatePendingPickups(float frameTime);
	void UpdateLoadoutCatchup(float frameTime);
	void UpdateCoopVision(float frameTime);
	void UpdateJoinerProximity(float frameTime);
	void ResetJoinerProximity();
	void UpdateAIDump(float frameTime);
	void UpdateAIWakeNearJoiners(float frameTime);
	void ResetAIWake();
	void UpdateFakeRender();
	void ClearAlwaysAnimated();
	void ResetLoadoutCatchup();
	void UpdateTimeOfDaySync(float frameTime, int channelId = 0);
	void UpdateTeamMates(float frameTime);
	void UpdateAIStateSync(float frameTime);
	void UpdateCutscenePresentation();
	void UpdateUsableSync(float frameTime);
	void UpdatePlayerSeats(float frameTime);
	void UpdatePlayerMovers();
	void UpdateVehicleExits(float frameTime);
	void UpdateTestCursor();
	void UpdateTestCmdFile();
	void ScanUsableObjects(bool atLoad);
	void UpdateDebugExplosions(float frameTime);
	void UpdateKillWatch(float frameTime);
	void UpdateMirrorVideoTrace(float frameTime);
	void UpdateDebugKillGunners(float frameTime);
	float s_autopilotHoldUntil = 0.0f; // a debug kill is being watched: the autopilot waits
	void SendSyncHistory(int channelId);
	void ClearProtectedVehicles();
	void ClearInventorySnapshots(const char* levelName);
	ICVar* s_pAspectMask = nullptr;
	ICVar* s_pAllAspectMask = nullptr;
	ICVar* s_pKeepCat = nullptr;

	bool IsCoopServer()
	{
		if (s_pDebugFlags && (s_pDebugFlags->GetIVal() & 1))
			return false;
		return gEnv->pAISystem && gEnv->bServer && gEnv->bMultiplayer;
	}

	// CryAction's AI script bindings (AI.RegisterWithAI, ...) refuse to work
	// while gEnv->bMultiplayer is set, even with the AI system enabled. On the
	// server we pretend single player for the duration of such calls only.
	int s_spScopeDepth = 0;
	bool s_spScopeSavedMultiplayer = false;

	void BeginSinglePlayerScope()
	{
		if (s_spScopeDepth++ == 0)
		{
			s_spScopeSavedMultiplayer = gEnv->bMultiplayer;
			gEnv->bMultiplayer = false;
		}
	}

	void EndSinglePlayerScope()
	{
		if (s_spScopeDepth > 0 && --s_spScopeDepth == 0)
			gEnv->bMultiplayer = s_spScopeSavedMultiplayer;
	}

	struct SSinglePlayerScope
	{
		SSinglePlayerScope() { BeginSinglePlayerScope(); }
		~SSinglePlayerScope() { EndSinglePlayerScope(); }
	};

	IActor* GetActor(IEntity* pEntity)
	{
		return g_pGame->GetIGameFramework()->GetIActorSystem()->GetActor(pEntity->GetId());
	}

	bool IsPlayerEntity(IEntity* pEntity)
	{
		if (!pEntity)
			return false;
		IActor* pActor = GetActor(pEntity);
		return pActor && pActor->IsPlayer();
	}

	bool HasFunction(IScriptTable* pScript, const char* name)
	{
		return pScript && pScript->GetValueType(name) == svtFunction;
	}

	// Scripted AI entities (BasicAI and everything derived from it: Grunt,
	// Civilian, aliens...) expose RegisterAI(); that is how we recognise them.
	bool IsScriptedAI(IEntity* pEntity)
	{
		return HasFunction(pEntity->GetScriptTable(), "RegisterAI");
	}

	struct SStats
	{
		int scripted = 0;
		int withAI = 0;
		int enabled = 0;
		int moving = 0;
		int alerted = 0;
		int targetingPlayer = 0;
		int players = 0;
		int playersWithAI = 0;
	};

	SStats CollectStats()
	{
		SStats stats;
		IEntityItPtr pIt = gEnv->pEntitySystem->GetEntityIterator();
		pIt->MoveFirst();
		while (IEntity* pEntity = pIt->Next())
		{
			if (IsPlayerEntity(pEntity))
			{
				stats.players++;
				if (pEntity->GetAI())
					stats.playersWithAI++;
				continue;
			}

			if (!IsScriptedAI(pEntity))
				continue;

			stats.scripted++;

			if (IAIObject* pAI = pEntity->GetAI())
			{
				stats.withAI++;
				if (pAI->IsEnabled())
					stats.enabled++;
				if (pAI->IsMoving())
					stats.moving++;
				if (IUnknownProxy* pProxy = pAI->GetProxy())
				{
					if (pProxy->GetAlertnessState() > 0)
						stats.alerted++;
				}
				if (IPipeUser* pPipeUser = pAI->CastToIPipeUser())
				{
					IAIObject* pTarget = pPipeUser->GetAttentionTarget();
					if (pTarget && IsPlayerEntity(pTarget->GetEntity()))
						stats.targetingPlayer++;
				}
			}
		}
		return stats;
	}

	void LogStats(const char* tag)
	{
		const SStats s = CollectStats();
		CryLogAlways("[CoopAI] %s: scriptedAI=%d withAIObject=%d enabled=%d moving=%d alerted=%d targetingPlayer=%d players=%d playersWithAI=%d",
			tag, s.scripted, s.withAI, s.enabled, s.moving, s.alerted, s.targetingPlayer, s.players, s.playersWithAI);
	}

	// Repeat, in single-player scope, what the entity scripts do on spawn for
	// every AI-capable entity that still has no AI object:
	//  - soldiers/aliens (BasicAI): RegisterAI() + OnReset()
	//  - vehicles (VehicleBaseAI):  InitAI()
	//  - players (player.lua):      AI.RegisterWithAI(id, AIOBJECT_PLAYER, ...)
	//    without a player AI object the AI cannot see players (only hears
	//    gunshots) and has nothing to aim at.
	// one attempt per entity per level: re-running OnReset() on something that
	// still cannot get an AI object every second would keep resetting it
	std::set<EntityId> s_attempted;

	int RegisterMissingAI()
	{
		SSinglePlayerScope spScope;

		SmartScriptTable aiTable;
		gEnv->pScriptSystem->GetGlobalValue("AI", aiTable);

		int count = 0;
		IEntityItPtr pIt = gEnv->pEntitySystem->GetEntityIterator();
		pIt->MoveFirst();
		while (IEntity* pEntity = pIt->Next())
		{
			if (pEntity->GetAI())
				continue;

			IScriptTable* pScript = pEntity->GetScriptTable();
			if (!pScript)
				continue;

			if (IActor* pActor = GetActor(pEntity))
			{
				if (pActor->GetHealth() <= 0)
					continue;
			}

			if (!s_attempted.insert(pEntity->GetId()).second)
				continue;

			if (IsPlayerEntity(pEntity))
			{
				if (!aiTable)
					continue;
				SmartScriptTable props, propsInstance;
				pScript->GetValue("Properties", props);
				pScript->GetValue("PropertiesInstance", propsInstance);
				if (!props || !propsInstance)
					continue;
				Script::CallMethod(aiTable, "RegisterWithAI", ScriptHandle(pEntity->GetId()), (int)AIOBJECT_PLAYER, props, propsInstance);
				count++;
			}
			else if (HasFunction(pScript, "RegisterAI"))
			{
				Script::CallMethod(pScript, "RegisterAI");
				if (HasFunction(pScript, "OnReset"))
					Script::CallMethod(pScript, "OnReset");
				count++;
			}
			else if (HasFunction(pScript, "InitAI") && pScript->GetValueType("AIType") != svtNull)
			{
				Script::CallMethod(pScript, "InitAI");
				count++;
			}
		}
		return count;
	}

	void CmdStats(IConsoleCmdArgs*)
	{
		if (gEnv->pEntitySystem)
			LogStats("coop_aistats");
	}

	// coop_sp_scope 1|0 - used by the Lua wrapper around AI.RegisterWithAI
	void CmdSinglePlayerScope(IConsoleCmdArgs* pArgs)
	{
		if (!gEnv->bServer || pArgs->GetArgCount() < 2)
			return;
		if (atoi(pArgs->GetArg(1)) != 0)
			BeginSinglePlayerScope();
		else
			EndSinglePlayerScope();
	}

	void CmdDumpEntities(IConsoleCmdArgs* pArgs);

	// Player inventories, snapshotted every 2 s while alive. A respawn gives
	// the player his own last inventory back (like a campaign checkpoint), a
	// joining player gets a copy of the host's. Items/ammo the level scripts
	// hand out are therefore kept (story scripts check them).
	typedef CoopAI::SInventory SInvSnapshot;
	std::map<EntityId, SInvSnapshot> s_invSnapshots;
	float s_invTimer = 0.0f;

	// Level to level (the coop map change after a level end): every player's
	// inventory by player name, given back when the player is first equipped
	// on the next level. The campaign's own Inventory:StorePlayerInventory /
	// RestorePlayerInventory nodes only know the local player (the host).
	std::map<string, SInvSnapshot> s_invCarry;
	string s_invCarryLevel; // the coop map the carry is meant for

	const char* LevelShortName(const char* level)
	{
		const char* p = level;
		for (const char* q = level; *q; ++q)
			if (*q == '/' || *q == '\\')
				p = q + 1;
		return p;
	}

	void ClearInventorySnapshots(const char* levelName)
	{
		s_invSnapshots.clear();
		// a level loaded any other way (menu, console) starts from scratch
		if (!s_invCarry.empty() && (!levelName || stricmp(LevelShortName(levelName), s_invCarryLevel.c_str())))
		{
			CryLogAlways("[CoopInv] level %s is not %s: carried inventories dropped", levelName ? levelName : "?", s_invCarryLevel.c_str());
			s_invCarry.clear();
		}
	}

	bool TakeInventorySnapshot(IActor* pActor, SInvSnapshot& snap)
	{
		IInventory* pInv = pActor->GetInventory();
		if (!pInv || pInv->GetCount() == 0)
			return false;
		for (int i = 0; i < pInv->GetCount(); ++i)
			if (IEntity* pItem = gEnv->pEntitySystem->GetEntity(pInv->GetItem(i)))
				snap.items.push_back(pItem->GetClass()->GetName());
		for (int i = 0; i < pInv->GetAmmoTypeCount(); ++i)
			if (IEntityClass* pAmmo = pInv->GetAmmoTypeByIdx(i))
				snap.ammo.push_back(std::make_pair(string(pAmmo->GetName()), pInv->GetAmmoCount(pAmmo)));
		if (IEntity* pCur = gEnv->pEntitySystem->GetEntity(pInv->GetCurrentItem()))
			snap.current = pCur->GetClass()->GetName();
		return true;
	}

	void SnapshotInventories(float frameTime)
	{
		s_invTimer += frameTime;
		if (s_invTimer < 2.0f)
			return;
		s_invTimer = 0.0f;
		IGameFramework* pFramework = g_pGame->GetIGameFramework();
		IActorIteratorPtr pIt = pFramework->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = pIt->Next())
		{
			if (!pActor->IsPlayer() || pActor->GetHealth() <= 0)
				continue;
			SInvSnapshot snap;
			if (TakeInventorySnapshot(pActor, snap))
				s_invSnapshots[pActor->GetEntityId()] = snap;
		}
	}

	// The campaign's global game tokens ("Game.*": Game.General.Previous_Level,
	// which the next level checks to give the player his weapons back, the
	// tutorial hints already shown) keep their values from level to level in
	// single player; a network level change resets them. The server records
	// their changes and sets them again on the next coop map.
	struct SGlobalTokenWatch : public IGameTokenEventListener
	{
		std::map<string, string> values;
		virtual void OnGameTokenEvent(EGameTokenEvent event, IGameToken* pToken)
		{
			if (event != EGAMETOKEN_EVENT_CHANGE || !pToken || !gEnv->bServer)
				return;
			const char* name = pToken->GetName();
			if (name && !strnicmp(name, "Game.", 5))
				values[name] = pToken->GetValueAsString();
		}
	};
	SGlobalTokenWatch s_globalTokens;
	std::map<string, string> s_tokenCarry;
	string s_tokenCarryLevel;

	void WatchGlobalTokens(const char* levelName)
	{
		s_globalTokens.values.clear();
		if (!s_tokenCarry.empty() && (!levelName || stricmp(LevelShortName(levelName), s_tokenCarryLevel.c_str())))
			s_tokenCarry.clear();
		if (IGameTokenSystem* pTokens = g_pGame->GetIGameFramework()->GetIGameTokenSystem())
		{
			pTokens->UnregisterListener(&s_globalTokens);
			pTokens->RegisterListener(&s_globalTokens);
		}
	}

	void RestoreGlobalTokens()
	{
		IGameTokenSystem* pTokens = g_pGame->GetIGameFramework()->GetIGameTokenSystem();
		if (!pTokens || s_tokenCarry.empty())
			return;
		for (std::map<string, string>::iterator it = s_tokenCarry.begin(); it != s_tokenCarry.end(); ++it)
		{
			IGameToken* pToken = pTokens->FindToken(it->first.c_str());
			if (!pToken)
				pToken = pTokens->SetOrCreateToken(it->first.c_str(), TFlowInputData(it->second));
			if (pToken)
				pToken->SetValueAsString(it->second.c_str());
			CryLogAlways("[CoopToken] %s=%s (from the previous level)", it->first.c_str(), it->second.c_str());
		}
		s_tokenCarry.clear();
	}

	// coop_change_map <map>: the next level with every connected player, the
	// way a multiplayer server goes to the next map of its rotation (the game
	// context changes, the players stay connected and load the level too).
	// "map <level> s" starts a new server instead: the others are disconnected.
	void CmdChangeMap(IConsoleCmdArgs* pArgs)
	{
		if (!gEnv->bServer || pArgs->GetArgCount() < 2)
			return;
		IGameFramework* pFramework = g_pGame->GetIGameFramework();
		const char* level = pArgs->GetArg(1);
		IEntity* pRules = pFramework->GetIGameRulesSystem()->GetCurrentGameRulesEntity();
		const char* rules = pRules ? pRules->GetClass()->GetName() : "TeamInstantAction";
		ILevelRotation* pRotation = pFramework->GetILevelSystem()->GetLevelRotation();
		if (!pFramework->StartedGameContext() || !pRotation)
		{
			CryLogAlways("[Coop] no game running: map %s s", level);
			string cmd;
			cmd.Format("map %s s", level);
			gEnv->pConsole->ExecuteString(cmd.c_str());
			return;
		}
		CryLogAlways("[Coop] changing the level to %s (%s), the connected players come along", level, rules);
		s_tokenCarry = s_globalTokens.values;
		s_tokenCarryLevel = LevelShortName(level);
		pRotation->Reset();
		pRotation->AddLevel(level, rules);
		pRotation->First();
		pRotation->ChangeLevel();
		pRotation->Reset();
	}

	// coop_inv_carry <next map>: keep every player's inventory for the next
	// level (a dead player: his last snapshot)
	void CmdInvCarry(IConsoleCmdArgs* pArgs)
	{
		if (!gEnv->bServer || pArgs->GetArgCount() < 2)
			return;
		std::map<string, SInvSnapshot> inventories;
		CoopAI::CollectInventories(inventories, true);
		CoopAI::SetInventoryCarry(pArgs->GetArg(1), inventories);
	}

	IActor* FindActorByName(const char* name)
	{
		IEntity* pEntity = gEnv->pEntitySystem->FindEntityByName(name);
		return pEntity ? g_pGame->GetIGameFramework()->GetIActorSystem()->GetActor(pEntity->GetId()) : 0;
	}

	// what this player carries over (from the last level or the checkpoint
	// the game goes on from): by his player key, else by his name
	std::map<string, SInvSnapshot>::iterator FindCarry(IActor* pActor)
	{
		std::map<string, SInvSnapshot>::iterator it = s_invCarry.find(CoopAI::PlayerKey(pActor));
		if (it == s_invCarry.end())
			it = s_invCarry.find(string("name:") + pActor->GetEntity()->GetName());
		return it;
	}

	// coop_inv_restore <player> [<from player>]: returns 1 in the log line if
	// a snapshot was applied
	void CmdInvRestore(IConsoleCmdArgs* pArgs)
	{
		if (!gEnv->bServer || pArgs->GetArgCount() < 2)
			return;
		IActor* pTarget = FindActorByName(pArgs->GetArg(1));
		IActor* pFrom = pArgs->GetArgCount() > 2 ? FindActorByName(pArgs->GetArg(2)) : pTarget;
		if (!pTarget || !pFrom)
			return;
		std::map<EntityId, SInvSnapshot>::iterator it = s_invSnapshots.find(pFrom->GetEntityId());
		std::map<string, SInvSnapshot>::iterator carried = FindCarry(pTarget);
		SInvSnapshot snap;
		const char* source = pFrom->GetEntity()->GetName();
		if (it != s_invSnapshots.end() && !it->second.items.empty())
			snap = it->second;
		else if (pFrom == pTarget && carried != s_invCarry.end())
		{
			// first equipment on this level: what he had at the end of the last
			// one, or at the checkpoint the game goes on from
			snap = carried->second;
			s_invCarry.erase(carried);
			source = "the previous level";
		}
		if (snap.items.empty())
		{
			CryLogAlways("[CoopInv] no inventory snapshot of %s", pFrom->GetEntity()->GetName());
			gEnv->pConsole->GetCVar("coop_inv_result")->Set(0);
			return;
		}
		CoopAI::GiveInventory(pTarget, snap, false);
		CryLogAlways("[CoopInv] %s got %d items / %d ammo types (snapshot of %s)", pTarget->GetEntity()->GetName(),
			(int)snap.items.size(), (int)snap.ammo.size(), source);
		gEnv->pConsole->GetCVar("coop_inv_result")->Set(1);
	}

	// coop_vehicle_drive <entity name>: make a (restored / new) vehicle drivable
	// by the passenger in the driver seat: movement processing and authority
	// on the server, engine on
	void CmdVehicleDrive(IConsoleCmdArgs* pArgs)
	{
		if (!gEnv->bServer || pArgs->GetArgCount() < 2)
			return;
		IEntity* pEntity = gEnv->pEntitySystem->FindEntityByName(pArgs->GetArg(1));
		IVehicle* pVehicle = pEntity ? g_pGame->GetIGameFramework()->GetIVehicleSystem()->GetVehicle(pEntity->GetId()) : 0;
		IVehicleMovement* pMovement = pVehicle ? pVehicle->GetMovement() : 0;
		if (!pMovement)
			return;
		IVehicleSeat* pSeat = pVehicle->GetSeatById(1);
		EntityId driverId = pSeat ? pSeat->GetPassenger() : 0;
		pMovement->EnableMovementProcessing(true);
		pMovement->SetAuthority(true);
		pMovement->DisableEngine(false);
		bool started = driverId ? pMovement->StartEngine(driverId) : false;
		CryLogAlways("[CoopAI] vehicle %s drive: driver=%u started=%d powered=%d processing=%d destroyed=%d",
			pEntity->GetName(), driverId, (int)started, (int)pMovement->IsPowered(),
			(int)pMovement->IsMovementProcessingEnabled(), (int)pVehicle->IsDestroyed());
	}

	// coop_vehtest [seconds]: the local player presses "forward"+"up" in his
	// vehicle for a while; the vehicle's speed is logged (testing controls)
	float s_vehTestTime = -1.0f;
	void CmdVehTest(IConsoleCmdArgs* pArgs)
	{
		s_vehTestTime = pArgs->GetArgCount() > 1 ? (float)atof(pArgs->GetArg(1)) : 4.0f;
		IActor* pActor = g_pGame->GetIGameFramework()->GetClientActor();
		IVehicle* pVehicle = pActor ? pActor->GetLinkedVehicle() : 0;
		CryLogAlways("[CoopVehTest] start: actor=%s vehicle=%s", pActor ? pActor->GetEntity()->GetName() : "-",
			pVehicle ? pVehicle->GetEntity()->GetName() : "-");
		if (pVehicle)
		{
			IVehicleSeat* pSeat = pVehicle->GetSeatForPassenger(pActor->GetEntityId());
			CryLogAlways("[CoopVehTest] seat=%s driver=%d destroyed=%d", pSeat ? pSeat->GetSeatName() : "-",
				pSeat ? (int)pSeat->IsDriver() : 0, (int)pVehicle->IsDestroyed());
			pVehicle->OnAction(eVAI_MoveForward, eAAM_OnPress, 1.0f, pActor->GetEntityId());
			pVehicle->OnAction(eVAI_MoveUp, eAAM_OnPress, 1.0f, pActor->GetEntityId());
		}
	}

	ICVar* s_pAutoVehTest = nullptr;
	float s_linkedTime = 0.0f;
	bool s_autoVehTestDone = false;

	void UpdateVehTest(float frameTime)
	{
		if (s_pAutoVehTest && s_pAutoVehTest->GetIVal() > 0 && !s_autoVehTestDone)
		{
			IActor* pA = g_pGame->GetIGameFramework()->GetClientActor();
			if (pA && pA->GetLinkedVehicle())
			{
				s_linkedTime += frameTime;
				if (s_linkedTime >= (float)s_pAutoVehTest->GetIVal())
				{
					s_autoVehTestDone = true;
					gEnv->pConsole->ExecuteString("coop_vehtest 8");
				}
			}
			else
				s_linkedTime = 0.0f;
		}
		if (s_vehTestTime < 0.0f)
			return;
		IActor* pActor = g_pGame->GetIGameFramework()->GetClientActor();
		IVehicle* pVehicle = pActor ? pActor->GetLinkedVehicle() : 0;
		static float s_acc = 0.0f;
		s_acc += frameTime;
		s_vehTestTime -= frameTime;
		if (pVehicle && s_acc >= 1.0f)
		{
			s_acc = 0.0f;
			pe_status_dynamics dyn;
			IPhysicalEntity* pPhys = pVehicle->GetEntity()->GetPhysics();
			float speed = (pPhys && pPhys->GetStatus(&dyn)) ? dyn.v.len() : -1.0f;
			Vec3 p = pVehicle->GetEntity()->GetWorldPos();
			CryLogAlways("[CoopVehTest] speed=%.1f pos=(%.0f,%.0f,%.0f)", speed, p.x, p.y, p.z);
			pVehicle->OnAction(eVAI_MoveForward, eAAM_OnHold, 1.0f, pActor->GetEntityId());
			pVehicle->OnAction(eVAI_MoveUp, eAAM_OnHold, 1.0f, pActor->GetEntityId());
		}
		if (s_vehTestTime < 0.0f && pVehicle)
		{
			pVehicle->OnAction(eVAI_MoveForward, eAAM_OnRelease, 0.0f, pActor->GetEntityId());
			pVehicle->OnAction(eVAI_MoveUp, eAAM_OnRelease, 0.0f, pActor->GetEntityId());
			CryLogAlways("[CoopVehTest] done");
		}
	}

	// coop_vehicle_restore <entity name>: bring a destroyed vehicle back
	// (the same entity, so level scripts keep working with it)
	void CmdVehicleRestore(IConsoleCmdArgs* pArgs)
	{
		if (!gEnv->bServer || pArgs->GetArgCount() < 2)
			return;
		IEntity* pEntity = gEnv->pEntitySystem->FindEntityByName(pArgs->GetArg(1));
		IVehicle* pVehicle = pEntity ? g_pGame->GetIGameFramework()->GetIVehicleSystem()->GetVehicle(pEntity->GetId()) : 0;
		if (!pVehicle)
		{
			CryLogAlways("[CoopAI] coop_vehicle_restore: no vehicle '%s'", pArgs->GetArg(1));
			return;
		}
		const Matrix34 tm = pEntity->GetWorldTM();
		pVehicle->Reset(true);
		pEntity->SetWorldTM(tm);
		// the reset may bring back the level-start state (hidden, asleep)
		pEntity->Hide(false);
		pEntity->Physicalize(pVehicle->GetPhysicsParams());
		if (IVehicleMovement* pMovement = pVehicle->GetMovement())
		{
			pMovement->Physicalize();
			pMovement->PostPhysicalize();
		}
		pEntity->SetWorldTM(tm);
		if (IPhysicalEntity* pPhys = pEntity->GetPhysics())
		{
			pe_action_awake awake;
			awake.bAwake = 1;
			pPhys->Action(&awake);
		}
		pe_status_dynamics dyn;
		float mass = (pEntity->GetPhysics() && pEntity->GetPhysics()->GetStatus(&dyn)) ? dyn.mass : -1.0f;
		CryLogAlways("[CoopAI] vehicle %s after restore: hidden=%d physics=%d mass=%.0f", pEntity->GetName(),
			(int)pEntity->IsHidden(), pEntity->GetPhysics() ? (int)pEntity->GetPhysics()->GetType() : -1, mass);
		CryLogAlways("[CoopAI] vehicle %s restored (destroyed=%d)", pEntity->GetName(), (int)pVehicle->IsDestroyed());
	}

	// coop_mp_scope 1|0 - the opposite of coop_sp_scope: network mode for the
	// duration of a script call (used around Net.Expose, which must register
	// an entity class' network data the same way on every machine, whatever
	// mode the level is being loaded in)
	int s_mpScopeDepth = 0;
	bool s_mpScopeSaved = false;
	void CmdMultiplayerScope(IConsoleCmdArgs* pArgs)
	{
		if (pArgs->GetArgCount() < 2)
			return;
		if (atoi(pArgs->GetArg(1)) != 0)
		{
			if (s_mpScopeDepth++ == 0)
			{
				s_mpScopeSaved = gEnv->bMultiplayer;
				gEnv->bMultiplayer = true;
			}
		}
		else if (s_mpScopeDepth > 0 && --s_mpScopeDepth == 0)
			gEnv->bMultiplayer = s_mpScopeSaved;
	}

	void RegisterCommands()
	{
		static bool s_added = false;
		if (s_added || !gEnv->pConsole)
			return;
		gEnv->pConsole->AddCommand("coop_aistats", CmdStats, 0, "Crysis Coop: log AI object statistics");
		gEnv->pConsole->AddCommand("coop_dump_entities", CmdDumpEntities, 0, "Crysis Coop: write all entities to coop_entities_<side>_<tag>.txt");
		gEnv->pConsole->AddCommand("coop_vehicle_restore", CmdVehicleRestore, 0, "Crysis Coop: restore a destroyed vehicle by entity name (server)");
		gEnv->pConsole->AddCommand("coop_vehtest", CmdVehTest, 0, "Crysis Coop: drive the local player's vehicle forward/up for N seconds and log its speed");
		gEnv->pConsole->AddCommand("coop_vehicle_drive", CmdVehicleDrive, 0, "Crysis Coop: make a vehicle drivable by its driver (server)");
		gEnv->pConsole->AddCommand("coop_change_map", CmdChangeMap, 0, "Crysis Coop: go to another coop map with every connected player (server)");
		gEnv->pConsole->AddCommand("coop_inv_carry", CmdInvCarry, 0, "Crysis Coop: keep every player's inventory for the next coop map (server)");
		gEnv->pConsole->AddCommand("coop_inv_restore", CmdInvRestore, 0, "Crysis Coop: give a player his (or another player's) last inventory snapshot (server)");
		gEnv->pConsole->AddCommand("coop_mp_scope", CmdMultiplayerScope, 0, "Crysis Coop: 1 = begin / 0 = end network-mode scope for script calls");
		gEnv->pConsole->AddCommand("coop_sp_scope", CmdSinglePlayerScope, 0, "Crysis Coop: 1 = begin / 0 = end single-player scope for AI script calls (server only)");
		s_added = true;
	}

	// diagnostics: dump every entity (id, class, name, flags) to a file in
	// the game root, to compare the server's and the client's worlds
	void DumpEntities(const char* tag)
	{
		if (!gEnv->pEntitySystem)
			return;
		char path[256];
		_snprintf(path, sizeof(path), "coop_entities_%s_%s.txt", gEnv->bServer ? "server" : "client", tag);
		path[sizeof(path)-1] = 0;
		FILE* f = fopen(path, "w");
		if (!f)
			return;
		int n = 0;
		IEntityItPtr pIt = gEnv->pEntitySystem->GetEntityIterator();
		pIt->MoveFirst();
		while (IEntity* pEntity = pIt->Next())
		{
			char ext[256] = "";
			if (IGameObject* pGO = g_pGame->GetIGameFramework()->GetGameObject(pEntity->GetId()))
			{
				static const char* names[] = { "Interactor", "VoiceListener", "AnimatedCharacter", "Inventory",
					"ScriptControlledPhysics", "PlayerFeature", "Player", "Grunt", "Item", "Weapon", "Lam",
					"PlayerInput", "DamageEffectController", "SmartObject", "ProceduralObjectController" };
				for (int i = 0; i < (int)(sizeof(names)/sizeof(names[0])); ++i)
				{
					if (pGO->QueryExtension(names[i]))
					{
						strncat(ext, names[i], sizeof(ext) - strlen(ext) - 2);
						strncat(ext, ",", sizeof(ext) - strlen(ext) - 1);
					}
				}
			}
			fprintf(f, "%u\t%s\t%s\t%08x\t%d\t%s\n", pEntity->GetId(), pEntity->GetClass()->GetName(),
				pEntity->GetName(), pEntity->GetFlags(), (int)pEntity->IsGarbage(), ext);
			n++;
		}
		fclose(f);
		CryLogAlways("[CoopAI] dumped %d entities to %s", n, path);
	}

	void CmdDumpEntities(IConsoleCmdArgs* pArgs)
	{
		DumpEntities(pArgs->GetArgCount() > 1 ? pArgs->GetArg(1) : "manual");
	}

	// diagnostics (coop_debug_flags 16): log every spawned entity
	bool s_inLoading = false;
	bool s_coopServerLoading = false;
	bool s_coopLevelLoading = false;
	bool s_coopLevelActive = false;

	// entity classes whose instances are created by the server and sent to
	// the clients: items, vehicles, actors and the vehicle seat helpers
	bool IsNetworkedClass(IEntityClass* pClass)
	{
		const char* name = pClass->GetName();
		IGameFramework* pFramework = g_pGame ? g_pGame->GetIGameFramework() : nullptr;
		if (!pFramework)
			return true;
		if (pFramework->GetIItemSystem() && pFramework->GetIItemSystem()->IsItemClass(name))
			return true;
		if (pFramework->GetIVehicleSystem() && pFramework->GetIVehicleSystem()->IsVehicleClass(name))
			return true;
		if (!stricmp(name, "VehicleSeatSerializer"))
			return true;
		const char* script = pClass->GetScriptFile();
		if (script && (CryStringUtils::stristr(script, "entities/ai/") || CryStringUtils::stristr(script, "entities/actor/")))
			return true;
		return false;
	}
	// projectiles and debris: thousands per minute of combat, not traced
	bool TraceNoisyClass(IEntity* pEntity)
	{
		const char* c = pEntity->GetClass()->GetName();
		return CryStringUtils::stristr(c, "bullet") || CryStringUtils::stristr(c, "shell") || !stricmp(c, "Default");
	}

	struct SSpawnLog : public IEntitySystemSink
	{
		FILE* f = nullptr;
		int n = 0;
		virtual bool OnBeforeSpawn(SEntitySpawnParams& params)
		{
			// Anything the server creates with an automatic id while a coop
			// level loads (accessories of weapons lying in the level, parts of
			// scripted objects, ...) would be bound as a "static" part of the
			// level, which the network matches by entity id against the
			// client's own level load. Clients do not create the same set in
			// the same order, so the ids drift apart and the client is kicked
			// ("Failed ReconfigureObject"). Bound as ordinary dynamic objects
			// they are simply sent to the clients.
			if (!params.pClass)
				return true;
			// the gamerules keep their fixed static binding on every machine.
			// When the next level replaces this one in the same game (coop map
			// change) they are created while the old level is still marked as
			// running; a dynamic server copy cannot be matched by the clients'
			// own gamerules (their Init fails)
			if (g_pGame && g_pGame->GetIGameFramework()->GetIGameRulesSystem()->HaveGameRules(params.pClass->GetName()))
				return true;
			// during play on a coop level everything the server creates is a
			// dynamic network object (a script creating an entity with a
			// fixed id would otherwise be bound as a "static" object the
			// clients do not expect)
			if (!s_inLoading && s_coopLevelActive)
			{
				if (gEnv->bServer)
					params.nFlags |= ENTITY_FLAG_NEVER_NETWORK_STATIC;
				return true;
			}
			if (!s_inLoading || !s_coopLevelLoading || params.id != 0)
				return true;
			if (IsNetworkedClass(params.pClass))
			{
				if (gEnv->bServer)
					params.nFlags |= ENTITY_FLAG_NEVER_NETWORK_STATIC;
			}
			else
			{
				// helpers scripts create for themselves on every machine (e.g.
				// the lever of a light switch): purely local, never networked,
				// otherwise the client's copy gets a network id that collides
				// with the server's objects
				params.nFlags |= gEnv->bServer ? ENTITY_FLAG_SERVER_ONLY : ENTITY_FLAG_CLIENT_ONLY;
			}
			return true;
		}
		virtual void OnSpawn(IEntity* pEntity, SEntitySpawnParams& params)
		{
			if (!s_inLoading && CoopAI::TraceOn() && !TraceNoisyClass(pEntity))
			{
				Vec3 p = pEntity->GetWorldPos();
				CoopAI::Trace("SPAWN %s %s id=%u pos=(%.2f,%.2f,%.2f) flags=%08x", pEntity->GetClass()->GetName(),
					pEntity->GetName(), pEntity->GetId(), p.x, p.y, p.z, params.nFlags);
			}
			if (!(g_pGame && CoopAI::DebugFlags() & 16))
				return;
			if (!f)
				f = fopen(gEnv->bServer ? "coop_spawns_server.txt" : "coop_spawns_client.txt", "w");
			if (!f)
				return;
			fprintf(f, "%d\t%u\t%u\t%s\t%s\tload=%d\tmp=%d\tflags=%08x\n", n++, pEntity->GetId(), params.id,
				pEntity->GetClass()->GetName(), pEntity->GetName(), (int)s_inLoading, (int)gEnv->bMultiplayer, params.nFlags);
			fflush(f);
		}
		virtual bool OnRemove(IEntity* pEntity)
		{
			if (!s_inLoading && CoopAI::TraceOn() && !TraceNoisyClass(pEntity))
				CoopAI::Trace("REMOVE %s %s id=%u", pEntity->GetClass()->GetName(), pEntity->GetName(), pEntity->GetId());
			return true;
		}
		virtual void OnEvent(IEntity* pEntity, SEntityEvent& event)
		{
			// only these are looked at. The entity is not touched for any other
			// event: after a level was replaced by the next one in the same
			// game, an event reached this sink for an entity of the old level
			// that no longer existed (crash in the class name check)
			switch (event.event)
			{
			case ENTITY_EVENT_HIDE:
			case ENTITY_EVENT_UNHIDE:
			case ENTITY_EVENT_ENTERAREA:
			case ENTITY_EVENT_LEAVEAREA:
			case ENTITY_EVENT_SCRIPT_EVENT:
			case ENTITY_EVENT_RESET:
				break;
			default:
				return;
			}
			if ((event.event == ENTITY_EVENT_HIDE || event.event == ENTITY_EVENT_UNHIDE) && !s_inLoading && !CoopAI::TraceOn())
			{
				CoopAI::OnEntityHidden(pEntity, event.event == ENTITY_EVENT_HIDE);
				return;
			}
			if (s_inLoading || !CoopAI::TraceOn() || TraceNoisyClass(pEntity))
				return;
			switch (event.event)
			{
			case ENTITY_EVENT_HIDE:
			case ENTITY_EVENT_UNHIDE:
				CoopAI::OnEntityHidden(pEntity, event.event == ENTITY_EVENT_HIDE);
				CoopAI::Trace("%s %s %s", event.event == ENTITY_EVENT_HIDE ? "HIDE" : "UNHIDE",
					pEntity->GetClass()->GetName(), pEntity->GetName());
				break;
			case ENTITY_EVENT_ENTERAREA:
			case ENTITY_EVENT_LEAVEAREA:
				{
					IEntity* pTrigger = gEnv->pEntitySystem->GetEntity((EntityId)event.nParam[0]);
					IEntity* pArea = gEnv->pEntitySystem->GetEntity((EntityId)event.nParam[2]);
					// only the story triggers, not the ambient sound volumes
					if (!CryStringUtils::stristr(pEntity->GetClass()->GetName(), "Trigger") && !(pTrigger && CryStringUtils::stristr(pTrigger->GetClass()->GetName(), "Trigger")))
						break;
					CoopAI::Trace("AREA_%s target=%s by=%s area=%s", event.event == ENTITY_EVENT_ENTERAREA ? "ENTER" : "LEAVE",
						pEntity->GetName(), pTrigger ? pTrigger->GetName() : "?", pArea ? pArea->GetName() : "?");
				}
				break;
			case ENTITY_EVENT_SCRIPT_EVENT:
				{
					const char* name = (const char*)event.nParam[0];
					char value[128] = "";
					const void* pv = (const void*)event.nParam[2];
					if (pv)
					{
						switch (event.nParam[1])
						{
						case IEntityClass::EVT_INT: _snprintf(value, sizeof(value), "%d", *(const int*)pv); break;
						case IEntityClass::EVT_FLOAT: _snprintf(value, sizeof(value), "%.3f", *(const float*)pv); break;
						case IEntityClass::EVT_BOOL: _snprintf(value, sizeof(value), "%d", (int)*(const bool*)pv); break;
						case IEntityClass::EVT_VECTOR: { const Vec3& v = *(const Vec3*)pv; _snprintf(value, sizeof(value), "(%.2f,%.2f,%.2f)", v.x, v.y, v.z); } break;
						case IEntityClass::EVT_ENTITY: { IEntity* e = gEnv->pEntitySystem->GetEntity(*(const EntityId*)pv); _snprintf(value, sizeof(value), "%s", e ? e->GetName() : "?"); } break;
						case IEntityClass::EVT_STRING: _snprintf(value, sizeof(value), "<string>"); break;
						}
						value[sizeof(value)-1] = 0;
					}
					CoopAI::Trace("EVENT %s %s.%s %s", pEntity->GetClass()->GetName(), pEntity->GetName(), name ? name : "?", value);
				}
				break;
			case ENTITY_EVENT_RESET:
				CoopAI::Trace("RESET %s %s", pEntity->GetClass()->GetName(), pEntity->GetName());
				break;
			default:
				break;
			}
		}
	};
	SSpawnLog s_spawnLog;
	bool s_spawnLogAdded = false;

	// Objects physicalized by their Lua scripts (doors, props, switches...)
	// change their physics type on the server during play (an AI opens a
	// door -> rigid hinge) while the client still has the old type; the
	// network physics snapshot then has a different size on both ends and
	// the client is dropped. Their script state (opened/closed...) is still
	// synchronised; only the raw physics stream is not.
	void DisableScriptPhysicsSync()
	{
		IGameFramework* pFramework = g_pGame->GetIGameFramework();
		int n = 0;
		IEntityItPtr pIt = gEnv->pEntitySystem->GetEntityIterator();
		pIt->MoveFirst();
		while (IEntity* pEntity = pIt->Next())
		{
			if (IsNetworkedClass(pEntity->GetClass()))
				continue;
			IGameObject* pGameObject = pFramework->GetGameObject(pEntity->GetId());
			if (!pGameObject)
				continue;
			pGameObject->EnableAspect(eEA_Physics, false);
			n++;
		}
		CryLogAlways("[CoopAI] physics network sync disabled for %d script objects", n);
	}

	bool s_allAspectsApplied = false;
	std::set<EntityId> s_aiAspectsOff;
	float s_updateTimer = 0.0f;
	float s_dumpTimer = -1.0f;
	float s_clientNetDumpTimer = -1.0f;
	bool s_equipPending = false;
	float s_equipDelay = 0.0f;

	// hand out the AI equipment queued during loading, once the game runs
	void FlushDeferredEquip()
	{
		gEnv->pScriptSystem->SetGlobalValue("g_coopLoading", false);
		CGameRules* pGameRules = g_pGame->GetGameRules();
		if (!pGameRules)
			return;
		IScriptTable* pScript = pGameRules->GetEntity()->GetScriptTable();
		if (pScript && pScript->GetValueType("CoopFlushDeferredEquip") == svtFunction)
			Script::CallMethod(pScript, "CoopFlushDeferredEquip");
	}
	bool s_levelReady = false;
	bool s_coopServer = false;

	// coop_sp_world 1: after the level has loaded, the server keeps
	// gEnv->bMultiplayer = false for the rest of the level, so every engine
	// system that silently refuses to work in network games (mission
	// flowgraph, dialogs, AI entering vehicles...) behaves as in single
	// player. Networking itself runs on the server game context, not on
	// this flag. Restored when the next level starts loading.
	ICVar* s_pSPWorld = nullptr;
	bool s_spWorldActive = false;
	bool s_clientSPLoad = false;
	bool s_levelReadyClient = false;

	// while the server runs the level as single player the engine would
	// happily quick-save, auto-save at checkpoints and quick-load, which a
	// network session cannot survive
	bool s_saveLoadBlocked = false;
	int s_savedAutoSave = 1;

	void BlockSaveLoad(bool block)
	{
		if (block == s_saveLoadBlocked)
			return;
		s_saveLoadBlocked = block;

		if (ICVar* pAutoSave = gEnv->pConsole->GetCVar("g_enableAutoSave"))
		{
			if (block)
			{
				s_savedAutoSave = pAutoSave->GetIVal();
				pAutoSave->Set(0);
			}
			else
				pAutoSave->Set(s_savedAutoSave);
		}

		IActionMapManager* pAM = g_pGame->GetIGameFramework()->GetIActionMapManager();
		if (!pAM)
			return;
		static IActionFilter* s_pFilter = nullptr;
		if (!s_pFilter)
		{
			s_pFilter = pAM->CreateActionFilter("coop_no_saveload", eAFT_ActionFail);
			if (s_pFilter)
			{
				s_pFilter->Filter(ActionId("save"));
				s_pFilter->Filter(ActionId("load"));
				s_pFilter->Filter(ActionId("loadLastSave"));
				s_pFilter->Filter(ActionId("reload"));
			}
		}
		if (s_pFilter)
			s_pFilter->Enable(block);
	}
}

// ---------------------------------------------------------------------------
// detailed trace (coop_trace 1): every sample (coop_trace_hz per second) the
// players, the AI near them and the vehicles near them; every change of AI /
// vehicle state anywhere, game tokens, entity events and player input.
// Written to coop_trace_server.log / coop_trace_client.log in the game folder.
namespace
{
	ICVar* s_pTrace = nullptr;
	ICVar* s_pTraceHz = nullptr;
	FILE* s_traceFile = nullptr;
	bool s_traceFileOpened = false;
	float s_traceTimer = 0.0f;
	int s_traceSample = 0;
	float s_traceLookYaw = 0.0f, s_traceLookPitch = 0.0f;
	int s_traceLookCount = 0;
	float s_lastMoveInput = -100.0f;
	bool s_playingMirroredVoice = false;
	float s_traceTOD = -100.0f;
	float s_traceTODSpeed = -1.0f;
	std::set<string> s_traceSequences;

	struct STraceAI
	{
		int hp, alert;
		bool enabled, hidden;
		EntityId vehicle;
		string target, behaviour;
		bool operator!=(const STraceAI& o) const
		{
			return hp != o.hp || alert != o.alert || enabled != o.enabled || hidden != o.hidden
				|| vehicle != o.vehicle || target != o.target || behaviour != o.behaviour;
		}
	};
	std::map<EntityId, STraceAI> s_traceAI;

	struct STraceVeh
	{
		EntityId driver;
		int passengers, damage;
		bool destroyed, hidden;
		bool operator!=(const STraceVeh& o) const
		{
			return driver != o.driver || passengers != o.passengers || damage != o.damage
				|| destroyed != o.destroyed || hidden != o.hidden;
		}
	};
	std::map<EntityId, STraceVeh> s_traceVeh;

	struct STraceTokens : public IGameTokenEventListener
	{
		virtual void OnGameTokenEvent(EGameTokenEvent event, IGameToken* pToken)
		{
			if (!pToken || !CoopAI::TraceOn())
				return;
			// weapon customization menu positions, hundreds per minute
			if (!strnicmp(pToken->GetName(), "hud.", 4))
				return;
			const char* value = event == EGAMETOKEN_EVENT_CHANGE ? pToken->GetValueAsString() : "<deleted>";
			CoopAI::Trace("TOKEN %s=%s", pToken->GetName(), value ? value : "");
			CryLogAlways("[CoopToken] %s=%s", pToken->GetName(), value ? value : "");
		}
	};
	STraceTokens s_traceTokens;

	// every voice sound (dialog lines, AI speech) that starts or stops
	struct STraceVoice : public ISoundSystemEventListener
	{
		virtual void OnSoundSystemEvent(ESoundSystemCallbackEvent event, ISound* pSound)
		{
			if (!pSound)
				return;
			if (event != SOUNDSYSTEM_EVENT_ON_START && event != SOUNDSYSTEM_EVENT_ON_STOP)
				return;
			const Vec3 p = pSound->GetPosition();
			const char* name = pSound->GetName();
			// the server repeats every voice sound on the clients: their AI has no
			// AI system (no barks) and dialogs run in the server's scripts. The
			// suit voice belongs to the host's own suit.
			if (event == SOUNDSYSTEM_EVENT_ON_START && name && gEnv->bServer && gEnv->bMultiplayer && CoopAI::IsCoopSession()
				&& !CryStringUtils::stristr(name, "/suit/") && !s_playingMirroredVoice)
			{
				if (CGameRules* pRules = g_pGame->GetGameRules())
					pRules->CoopSendVoice(name, p, pSound->GetFlags());
			}
			if (!CoopAI::TraceOn())
				return;
			CoopAI::Trace("VOICE %s %s pos=(%.1f,%.1f,%.1f) len=%dms", event == SOUNDSYSTEM_EVENT_ON_START ? "start" : "stop",
				name ? name : "?", p.x, p.y, p.z, pSound->GetLengthMs());
			if (event == SOUNDSYSTEM_EVENT_ON_START)
				CryLogAlways("[CoopVoice] %s", name ? name : "?");
		}
	};
	STraceVoice s_traceVoice;
	bool s_traceVoiceAdded = false;

	void RegisterVoiceListener()
	{
		if (!s_traceVoiceAdded && gEnv->pSoundSystem)
		{
			gEnv->pSoundSystem->AddEventListener(&s_traceVoice, true);
			s_traceVoiceAdded = true;
		}
	}

	void TraceOpen(const char* levelName)
	{
		if (s_traceFile)
		{
			fclose(s_traceFile);
			s_traceFile = nullptr;
		}
		s_traceAI.clear();
		s_traceVeh.clear();
		s_traceSequences.clear();
		s_traceTOD = -100.0f;
		s_traceTODSpeed = -1.0f;
		s_traceSample = 0;
		if (!s_pTrace || !s_pTrace->GetIVal())
			return;
		s_traceFile = fopen(gEnv->bServer ? "coop_trace_server.log" : "coop_trace_client.log", s_traceFileOpened ? "a" : "w");
		s_traceFileOpened = true;
		if (s_traceFile)
			fprintf(s_traceFile, "=== level %s (%s) ===\n", levelName ? levelName : "?", gEnv->bServer ? "server" : "client");
		if (IGameTokenSystem* pTokens = g_pGame->GetIGameFramework()->GetIGameTokenSystem())
		{
			pTokens->UnregisterListener(&s_traceTokens);
			pTokens->RegisterListener(&s_traceTokens);
		}
		RegisterFlowInspector();
	}

	const char* TraceBehaviour(IEntity* pEntity)
	{
		static char buf[64];
		buf[0] = 0;
		IScriptTable* pScript = pEntity->GetScriptTable();
		SmartScriptTable behaviour;
		const char* name = 0;
		if (pScript && pScript->GetValue("Behaviour", behaviour) && behaviour->GetValue("Name", name) && name)
			_snprintf(buf, sizeof(buf), "%s", name);
		buf[sizeof(buf)-1] = 0;
		return buf;
	}

	Vec3 TraceVelocity(IEntity* pEntity)
	{
		if (IPhysicalEntity* pPhys = pEntity->GetPhysics())
		{
			pe_status_dynamics dyn;
			if (pPhys->GetStatus(&dyn))
				return dyn.v;
		}
		return Vec3(0, 0, 0);
	}

	void TraceSample(float frameTime)
	{
		if (!s_traceFile || !CoopAI::TraceOn())
			return;
		s_traceTimer += frameTime;
		const float hz = s_pTraceHz ? max(0.2f, s_pTraceHz->GetFVal()) : 5.0f;
		if (s_traceTimer < 1.0f / hz)
			return;
		s_traceTimer = 0.0f;
		++s_traceSample;

		IGameFramework* pFramework = g_pGame->GetIGameFramework();
		IActor* pLocal = pFramework->GetClientActor();
		std::vector<Vec3> players;

		CoopAI::Trace("--- sample %d", s_traceSample);
		IActorIteratorPtr pActors = pFramework->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = pActors->Next())
		{
			if (!pActor->IsPlayer())
				continue;
			IEntity* pEntity = pActor->GetEntity();
			CActor* pA = static_cast<CActor*>(pActor);
			const Vec3 p = pEntity->GetWorldPos();
			const Vec3 v = TraceVelocity(pEntity);
			const Ang3 a(pEntity->GetWorldAngles());
			IItem* pItem = pActor->GetCurrentItem();
			IVehicle* pVehicle = pActor->GetLinkedVehicle();
			int suitMode = -1;
			float energy = 0.0f;
			if (pA->GetActorClass() == CPlayer::GetActorClassType())
				if (CNanoSuit* pSuit = static_cast<CPlayer*>(pA)->GetNanoSuit())
				{
					suitMode = pSuit->GetMode();
					energy = pSuit->GetSuitEnergy();
				}
			CoopAI::Trace("P %s id=%u ch=%d%s pos=(%.2f,%.2f,%.2f) vel=(%.2f,%.2f,%.2f) spd=%.2f ang=(%.1f,%.1f,%.1f) stance=%d hp=%d/%d suit=%d:%.0f item=%s veh=%s spec=%d hidden=%d look=(%.3f,%.3f)x%d",
				pEntity->GetName(), pEntity->GetId(), (int)pActor->GetChannelId(), pActor == pLocal ? " local" : "",
				p.x, p.y, p.z, v.x, v.y, v.z, v.GetLength(), RAD2DEG(a.x), RAD2DEG(a.y), RAD2DEG(a.z),
				(int)pA->GetStance(), pActor->GetHealth(), pActor->GetMaxHealth(), suitMode, energy,
				pItem ? pItem->GetEntity()->GetClass()->GetName() : "-",
				pVehicle ? pVehicle->GetEntity()->GetName() : "-", (int)pA->GetSpectatorMode(), (int)pEntity->IsHidden(),
				pActor == pLocal ? s_traceLookYaw : 0.0f, pActor == pLocal ? s_traceLookPitch : 0.0f, pActor == pLocal ? s_traceLookCount : 0);
			if (pActor->GetHealth() > 0)
				players.push_back(p);
		}
		s_traceLookYaw = s_traceLookPitch = 0.0f;
		if (CHUD* pHUD = g_pGame->GetHUD())
		{
			if (CHUDRadar* pRadar = pHUD->GetRadar())
				CoopAI::TraceHUD("state", "radar_jamming=%.2f", pRadar->GetJamming());
			// the active objectives (written when the list changes)
			string active;
			const std::vector<CHUDMissionObjective>& objs = pHUD->GetMissionObjectiveSystem().GetObjectives();
			for (size_t i = 0; i < objs.size(); ++i)
				if (objs[i].GetStatus() == CHUDMissionObjective::ACTIVATED)
				{
					IEntity* pTracked = objs[i].GetTrackedEntity() ? gEnv->pEntitySystem->GetEntity(objs[i].GetTrackedEntity()) : 0;
					active += string(" ") + objs[i].GetID() + (objs[i].IsSecondary() ? "(sec)" : "") + "@" + (pTracked ? pTracked->GetName() : "-");
				}
			CoopAI::TraceHUD("objectives_active", "%s", active.empty() ? " none" : active.c_str());
		}

		// time of day
		if (ITimeOfDay* pTOD = gEnv->p3DEngine->GetTimeOfDay())
		{
			ITimeOfDay::SAdvancedInfo info;
			pTOD->GetAdvancedInfo(info);
			const float hour = pTOD->GetTime();
			if (fabsf(hour - s_traceTOD) >= 0.01f || info.fAnimSpeed != s_traceTODSpeed)
			{
				const bool jump = fabsf(hour - s_traceTOD) > 0.25f;
				CoopAI::Trace("TOD %02d:%02d (%.3f h) speed=%.4f range=%.2f-%.2f%s", (int)hour, (int)((hour - (int)hour) * 60.0f), hour,
					info.fAnimSpeed, info.fStartTime, info.fEndTime, jump ? " JUMP" : "");
				if (jump || info.fAnimSpeed != s_traceTODSpeed)
					CryLogAlways("[CoopTOD] %02d:%02d speed=%.4f", (int)hour, (int)((hour - (int)hour) * 60.0f), info.fAnimSpeed);
				s_traceTOD = hour;
				s_traceTODSpeed = info.fAnimSpeed;
			}
		}

		// cutscenes / track view sequences
		if (IMovieSystem* pMovie = gEnv->pMovieSystem)
		{
			std::set<string> playing;
			if (ISequenceIt* pIt = pMovie->GetSequences(true, false))
			{
				for (IAnimSequence* pSeq = pIt->first(); pSeq; pSeq = pIt->next())
				{
					const string name = pSeq->GetName();
					playing.insert(name);
					if (!s_traceSequences.count(name))
					{
						const int flags = pSeq->GetFlags();
						const Range r = pSeq->GetTimeRange();
						CoopAI::Trace("CUTSCENE start %s cutscene=%d noplayer=%d flags=%x length=%.1fs", name.c_str(),
							(flags & IAnimSequence::CUT_SCENE) ? 1 : 0, (flags & IAnimSequence::NO_PLAYER) ? 1 : 0, flags, r.end - r.start);
						CryLogAlways("[CoopCutscene] start %s (cutscene=%d noplayer=%d length=%.1fs)", name.c_str(),
							(flags & IAnimSequence::CUT_SCENE) ? 1 : 0, (flags & IAnimSequence::NO_PLAYER) ? 1 : 0, r.end - r.start);
					}
				}
				pIt->Release();
			}
			for (std::set<string>::iterator it = s_traceSequences.begin(); it != s_traceSequences.end(); ++it)
				if (!playing.count(*it))
				{
					CoopAI::Trace("CUTSCENE stop %s", it->c_str());
					CryLogAlways("[CoopCutscene] stop %s", it->c_str());
				}
			s_traceSequences.swap(playing);
			if (IViewSystem* pView = g_pGame->GetIGameFramework()->GetIViewSystem())
				CoopAI::TraceHUD("cutscene_state", "playing_cutscene=%d", (int)pView->IsPlayingCutScene());
		}
		s_traceLookCount = 0;

		// AI: state changes anywhere, full line when near a player
		IActorIteratorPtr pAll = pFramework->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = pAll->Next())
		{
			if (pActor->IsPlayer())
				continue;
			IEntity* pEntity = pActor->GetEntity();
			IAIObject* pAI = pEntity->GetAI();
			STraceAI st;
			st.hp = pActor->GetHealth();
			st.alert = pAI && pAI->GetProxy() ? pAI->GetProxy()->GetAlertnessState() : -1;
			st.enabled = pAI ? pAI->IsEnabled() : false;
			st.hidden = pEntity->IsHidden();
			IVehicle* pVehicle = pActor->GetLinkedVehicle();
			st.vehicle = pVehicle ? pVehicle->GetEntityId() : 0;
			IAIObject* pTarget = pAI && pAI->CastToIPipeUser() ? pAI->CastToIPipeUser()->GetAttentionTarget() : 0;
			st.target = pTarget ? pTarget->GetName() : "-";
			st.behaviour = TraceBehaviour(pEntity);

			std::map<EntityId, STraceAI>::iterator it = s_traceAI.find(pEntity->GetId());
			if (it == s_traceAI.end() || it->second != st)
			{
				const Vec3 p = pEntity->GetWorldPos();
				CoopAI::Trace("AI~ %s%s hp=%d alert=%d ai=%d hidden=%d veh=%s target=%s beh=%s pos=(%.1f,%.1f,%.1f)",
					pEntity->GetName(), it == s_traceAI.end() ? " (new)" : "", st.hp, st.alert, (int)st.enabled, (int)st.hidden,
					pVehicle ? pVehicle->GetEntity()->GetName() : "-", st.target.c_str(), st.behaviour.c_str(), p.x, p.y, p.z);
				s_traceAI[pEntity->GetId()] = st;
			}

			if (st.hidden)
				continue;
			const Vec3 p = pEntity->GetWorldPos();
			float nearest = 1e9f;
			for (size_t i = 0; i < players.size(); ++i)
				nearest = min(nearest, (p - players[i]).GetLength());
			if (nearest > 150.0f)
				continue;
			const Vec3 v = TraceVelocity(pEntity);
			const Ang3 a(pEntity->GetWorldAngles());
			CoopAI::Trace("A %s d=%.1f pos=(%.2f,%.2f,%.2f) vel=(%.2f,%.2f,%.2f) yaw=%.1f stance=%d hp=%d alert=%d target=%s beh=%s vis=%d distant=%d",
				pEntity->GetName(), nearest, p.x, p.y, p.z, v.x, v.y, v.z, RAD2DEG(a.z), (int)static_cast<CActor*>(pActor)->GetStance(),
				st.hp, st.alert, st.target.c_str(), st.behaviour.c_str(),
				(int)pActor->GetGameObject()->IsProbablyVisible(), (int)pActor->GetGameObject()->IsProbablyDistant());
		}

		// vehicles
		IVehicleIteratorPtr pVehicles = pFramework->GetIVehicleSystem()->CreateVehicleIterator();
		while (IVehicle* pVehicle = pVehicles->Next())
		{
			IEntity* pEntity = pVehicle->GetEntity();
			STraceVeh st;
			IActor* pDriver = pVehicle->GetDriver();
			st.driver = pDriver ? pDriver->GetEntityId() : 0;
			st.passengers = pVehicle->GetStatus().passengerCount;
			st.damage = (int)(pVehicle->GetDamageRatio() * 100.0f);
			st.destroyed = pVehicle->IsDestroyed();
			st.hidden = pEntity->IsHidden();
			std::map<EntityId, STraceVeh>::iterator it = s_traceVeh.find(pEntity->GetId());
			if (it == s_traceVeh.end() || it->second != st)
			{
				const Vec3 p = pEntity->GetWorldPos();
				CoopAI::Trace("VEH~ %s%s class=%s driver=%s passengers=%d damage=%d%% destroyed=%d hidden=%d pos=(%.1f,%.1f,%.1f)",
					pEntity->GetName(), it == s_traceVeh.end() ? " (new)" : "", pEntity->GetClass()->GetName(),
					pDriver ? pDriver->GetEntity()->GetName() : "-", st.passengers, st.damage, (int)st.destroyed, (int)st.hidden, p.x, p.y, p.z);
				s_traceVeh[pEntity->GetId()] = st;
			}
			if (st.hidden)
				continue;
			const Vec3 p = pEntity->GetWorldPos();
			float nearest = 1e9f;
			for (size_t i = 0; i < players.size(); ++i)
				nearest = min(nearest, (p - players[i]).GetLength());
			if (nearest > 200.0f)
				continue;
			const Vec3 v = TraceVelocity(pEntity);
			const Ang3 a(pEntity->GetWorldAngles());
			CoopAI::Trace("V %s d=%.1f pos=(%.2f,%.2f,%.2f) vel=(%.2f,%.2f,%.2f) ang=(%.1f,%.1f,%.1f) driver=%s passengers=%d damage=%d%%",
				pEntity->GetName(), nearest, p.x, p.y, p.z, v.x, v.y, v.z, RAD2DEG(a.x), RAD2DEG(a.y), RAD2DEG(a.z),
				pDriver ? pDriver->GetEntity()->GetName() : "-", st.passengers, st.damage);
		}
		fflush(s_traceFile);
	}
}

namespace
{
	std::vector<Vec3> s_joinerPositions;

	void UpdateJoinerPositions()
	{
		s_joinerPositions.clear();
		if (!gEnv->bServer || !gEnv->bMultiplayer || !g_pGame || !CoopAI::IsCoopSession())
			return;
		IGameFramework* pFramework = g_pGame->GetIGameFramework();
		IActor* pLocal = pFramework->GetClientActor();
		IActorIteratorPtr pIt = pFramework->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = pIt->Next())
			if (pActor->IsPlayer() && pActor != pLocal && pActor->GetHealth() > 0)
				s_joinerPositions.push_back(pActor->GetEntity()->GetWorldPos());
	}
}

bool CoopAI::KeepFullUpdate(IEntity* pEntity)
{
	if (!pEntity || s_joinerPositions.empty())
		return false;
	ICVar* pOn = gEnv->pConsole->GetCVar("coop_full_update_near_joiners");
	if (pOn && !pOn->GetIVal())
		return false;
	const Vec3 pos = pEntity->GetWorldPos();
	for (size_t i = 0; i < s_joinerPositions.size(); ++i)
		if ((pos - s_joinerPositions[i]).GetLengthSquared() < 100.0f * 100.0f)
			return true;
	return false;
}

bool CoopAI::GodMode()
{
	static ICVar* pGod = 0;
	if (!pGod && gEnv->pConsole)
		pGod = gEnv->pConsole->GetCVar("coop_god");
	return pGod && pGod->GetIVal() != 0;
}

bool CoopAI::TraceOn()
{
	return s_traceFile && s_pTrace && s_pTrace->GetIVal() && g_pGame;
}

void CoopAI::Trace(const char* fmt, ...)
{
	if (!TraceOn())
		return;
	char buf[2048];
	va_list args;
	va_start(args, fmt);
	_vsnprintf(buf, sizeof(buf) - 1, fmt, args);
	va_end(args);
	buf[sizeof(buf) - 1] = 0;
	for (char* c = buf; *c; ++c)
		if ((unsigned char)*c < 32)
			*c = '|';
	fprintf(s_traceFile, "%10.3f %7d %s\n", gEnv->pTimer->GetCurrTime(), gEnv->pRenderer ? gEnv->pRenderer->GetFrameID(false) : 0, buf);
}

void CoopAI::TraceInput(const char* action, int mode, float value)
{
	if (!action)
		return;
	// the test autopilot leaves the player alone while he moves himself
	if (!strncmp(action, "move", 4) || !strcmp(action, "jump") || !strcmp(action, "sprint"))
		s_lastMoveInput = gEnv->pTimer->GetCurrTime();
	if (!TraceOn())
		return;
	// mouse look arrives every frame: summed into the next player sample
	if (strstr(action, "rotateyaw") || !strcmp(action, "hud_mousex"))
	{
		s_traceLookYaw += value;
		++s_traceLookCount;
		return;
	}
	if (strstr(action, "rotatepitch") || !strcmp(action, "hud_mousey"))
	{
		s_traceLookPitch += value;
		++s_traceLookCount;
		return;
	}
	Trace("IN %s mode=%d value=%.3f", action, mode, value);
}

void CoopAI::TraceHUD(const char* key, const char* fmt, ...)
{
	if (!TraceOn())
		return;
	char buf[1024];
	va_list args;
	va_start(args, fmt);
	_vsnprintf(buf, sizeof(buf) - 1, fmt, args);
	va_end(args);
	buf[sizeof(buf) - 1] = 0;
	// the same call repeated every frame is written once
	static std::map<string, string> s_last;
	string& last = s_last[key];
	if (last == buf)
		return;
	last = buf;
	Trace("HUD %s %s", key, buf);
}

void CoopAI::TraceScriptCall(const char* function, IFunctionHandler* pH)
{
	if (!TraceOn() || !pH)
		return;
	string args;
	for (int i = 1; i <= pH->GetParamCount(); ++i)
	{
		char buf[160] = "?";
		switch (pH->GetParamType(i))
		{
		case svtString: { const char* v = 0; pH->GetParam(i, v); _snprintf(buf, sizeof(buf), "\"%s\"", v ? v : ""); } break;
		case svtNumber: { float v = 0; pH->GetParam(i, v); _snprintf(buf, sizeof(buf), "%g", v); } break;
		case svtBool: { bool v = false; pH->GetParam(i, v); _snprintf(buf, sizeof(buf), "%s", v ? "true" : "false"); } break;
		case svtPointer:
			{
				ScriptHandle h;
				pH->GetParam(i, h);
				IEntity* pEntity = gEnv->pEntitySystem->GetEntity((EntityId)h.n);
				_snprintf(buf, sizeof(buf), "#%u(%s)", (unsigned)h.n, pEntity ? pEntity->GetName() : "-");
			}
			break;
		case svtObject:
			{
				SmartScriptTable t;
				ScriptHandle h;
				pH->GetParam(i, t);
				IEntity* pEntity = (t.GetPtr() && t->GetValue("id", h)) ? gEnv->pEntitySystem->GetEntity((EntityId)h.n) : 0;
				_snprintf(buf, sizeof(buf), "%s", pEntity ? pEntity->GetName() : "{table}");
			}
			break;
		case svtNull: _snprintf(buf, sizeof(buf), "nil"); break;
		default: break;
		}
		buf[sizeof(buf) - 1] = 0;
		if (i > 1)
			args += ", ";
		args += buf;
	}
	Trace("HUDLUA %s(%s)", function, args.c_str());
}

void CoopAI::Init()
{
	RegisterCommands();
	CoopRelay::Init();
	CoopSave::Init();
	if (gEnv->pConsole && !s_pTrace)
	{
		s_pTrace = gEnv->pConsole->RegisterInt("coop_trace", 1, 0, "Crysis Coop debugging: 1 = detailed trace to coop_trace_server.log / coop_trace_client.log");
		s_pTraceHz = gEnv->pConsole->RegisterFloat("coop_trace_hz", 5.0f, 0, "Crysis Coop debugging: coop_trace samples per second");
	}
	if (gEnv->pConsole && !s_pFlowMirror)
		s_pFlowMirror = gEnv->pConsole->RegisterString("coop_fg_mirror",
			"HUD:;CrysisFX:;Image:;Camera:;Music:;Sound:;Animations:PlaySequence;Animations:StopSequence;Input:;MaterialFX:;Environment:;Entity:VideoPlayer;Entity:Material;Entity:MaterialParam",
			0, "Crysis Coop: flow node types repeated on the clients (\"Group:\" = the whole group, else that one type)");
	if (gEnv->pConsole)
	{
		gEnv->pConsole->RegisterInt("coop_debug_killhost", 0, 0, "Crysis Coop testing: kill the host N s after he entered a vehicle");
		gEnv->pConsole->RegisterInt("coop_debug_destroyveh", 0, 0, "Crysis Coop testing: 1 = also destroy his vehicle");
	}
	if (gEnv->pConsole)
	{
		gEnv->pConsole->RegisterString("coop_debug_use", "", 0, "Crysis Coop testing: entity the host uses coop_debug_use_at seconds after the game started");
		gEnv->pConsole->RegisterInt("coop_debug_use_at", 30, 0, "Crysis Coop testing: see coop_debug_use");
		gEnv->pConsole->RegisterString("coop_debug_movejoiner", "", 0, "Crysis Coop testing (server): move joining players to this entity 5 s before coop_debug_use_at");
		gEnv->pConsole->RegisterString("coop_debug_client_use", "", 0, "Crysis Coop testing (client): entity the local player uses coop_debug_use_at s after loading");
		gEnv->pConsole->RegisterString("coop_debug_token", "", 0, "Crysis Coop testing: game token logged every 5 s");
	}
	if (gEnv->pConsole)
		gEnv->pConsole->RegisterInt("coop_god", 0, 0, "Crysis Coop testing: 1 = players take no damage");
	if (gEnv->pConsole)
		gEnv->pConsole->RegisterInt("coop_debug_autopilot", 0, 0, "Crysis Coop testing: local player walks to the active objective, one step every N s");
	if (gEnv->pConsole)
		gEnv->pConsole->RegisterInt("coop_debug_autopilot_who", 0, 0, "Crysis Coop testing: autopilot moves 0 the host, 1 every player, 2 only joined players");
	if (gEnv->pConsole)
		gEnv->pConsole->RegisterInt("coop_debug_autopilot_target", 0, 0, "Crysis Coop testing: autopilot walks to 0 the objective, 1 the nearest enemy");
	if (gEnv->pConsole)
	{
		gEnv->pConsole->RegisterInt("coop_debug_wake", 1, 0, "Crysis Coop: how soldiers near a joined player are woken (0 not, 1 AI enable, 2 +AI activation, 3 +wake-up, 4 activation+forced update, 5 forced update)");
		gEnv->pConsole->RegisterInt("coop_vision_frame", 1, 0, "Crysis Coop: confirm soldiers' sight of joined players every frame");
		gEnv->pConsole->RegisterInt("coop_animate_near_joiners", 1, 0, "Crysis Coop: the server animates joined players and the soldiers near them even when the host does not see them");
		gEnv->pConsole->RegisterInt("coop_joiner_proximity", 1, 0, "Crysis Coop: joined players get the local player's 'in range' box (AI, updates, anchors)");
		gEnv->pConsole->RegisterInt("coop_full_update_near_joiners", 1, 0, "Crysis Coop: actors near a joined player skip the 'idle and unseen' shortcut of the player update");
		gEnv->pConsole->RegisterString("coop_debug_visit", "", 0, "Crysis Coop debugging: the joined player is put next to this entity (and looks at it) coop_debug_visit_time s after he spawned");
		gEnv->pConsole->RegisterFloat("coop_debug_visit_time", 30.0f, 0, "Crysis Coop debugging: see coop_debug_visit");
		gEnv->pConsole->RegisterString("coop_debug_visit_event", "", 0, "Crysis Coop debugging: this flow event (EnableUsable...) is sent to the coop_debug_visit entity on the server 6 s after the visit");
		gEnv->pConsole->RegisterString("coop_debug_physquery", "", 0, "Crysis Coop debugging: \"x,y;x,y;...\": the physical objects within 3 m of these points are traced coop_debug_physquery_time s after the level loaded (on every machine that has it set)");
		gEnv->pConsole->RegisterFloat("coop_debug_physquery_time", 30.0f, 0, "Crysis Coop debugging: see coop_debug_physquery");
		gEnv->pConsole->RegisterString("coop_debug_physdump", "", 0, "Crysis Coop debugging: every 2 s the physics state of living soldiers whose name contains this is traced (on every machine that has it set)");
		gEnv->pConsole->RegisterInt("coop_keep_vehicles", 1, 0, "Crysis Coop: 1 = vehicles the players leave stay (no network 'abandoned vehicle' destruction), as in single player");
		// per machine: not taken over from the server when joining (the engine
		// sends a server's console variables to every client)
		gEnv->pConsole->RegisterString("coop_test_cmdfile", "", 0, "Crysis Coop testing: the lines of this file (relative to the game folder) are run as console commands, then the file is deleted")
			->SetFlags(VF_NOT_NET_SYNCED);
		gEnv->pConsole->RegisterInt("coop_test_free_cursor", 0, 0, "Crysis Coop testing: 1 keeps the system cursor free (as with a menu open): a test window never takes the mouse")
			->SetFlags(VF_NOT_NET_SYNCED);
		gEnv->pConsole->RegisterString("coop_debug_flow", "", 0, "Crysis Coop debugging: \"<graph entity> <node> <output port index or name>\" is activated on the server coop_debug_flow_time s after the game started");
		gEnv->pConsole->RegisterFloat("coop_debug_flow_time", 20.0f, 0, "Crysis Coop debugging: see coop_debug_flow");
		gEnv->pConsole->RegisterFloat("coop_debug_menu_at", 0.0f, 0, "Crysis Coop debugging: the host opens the in-game menu this many s after coop_debug_flow fired (0: never)");
		gEnv->pConsole->RegisterInt("coop_hud_enemy_names", 0, 0, "Crysis Coop: 1 shows the multiplayer names over soldiers that were shot (0: none, as in single player)");
		gEnv->pConsole->RegisterFloat("coop_debug_kill_view_dist", 10.0f, 0, "Crysis Coop debugging: how far from the soldier coop_debug_kill_gunners puts the joined player");
		gEnv->pConsole->RegisterInt("coop_debug_kill_shots", 0, 0, "Crysis Coop debugging: screenshots right after a soldier died in a vehicle (0 s, 0.3 s, 1.5 s, 4 s)");
		gEnv->pConsole->RegisterFloat("coop_debug_kill_gunners", 0.0f, 0, "Crysis Coop debugging: every N seconds a soldier sitting in a vehicle within 60 m of a joined player is shot dead by him (0 = off)");
		gEnv->pConsole->RegisterFloat("coop_debug_explode_joiners", 0.0f, 0, "Crysis Coop debugging: every N seconds an explosion (grenade strength) next to every joined player (0 = off)");
		gEnv->pConsole->RegisterInt("coop_fake_render", 1, 0, "Crysis Coop: soldiers near a joined player count as rendered (the engine runs unrendered soldiers in a reduced mode: they barely aim and fire)");
		gEnv->pConsole->RegisterInt("coop_vision", 1, 0, "Crysis Coop: server-side sight of joined players for the soldiers (0 = engine only)");
		gEnv->pConsole->RegisterInt("coop_debug_aidump", 0, 0, "Crysis Coop debugging: every N s dump the AI state of the players and the soldier nearest to a joined player");
	}
	if (gEnv->pConsole)
		gEnv->pConsole->RegisterInt("coop_defer_equip", 0, 0, "Crysis Coop: 1 = hand out equipment only after the level loaded");
	if (gEnv->pConsole)
		gEnv->pConsole->RegisterInt("coop_inv_result", 0, 0, "Crysis Coop: result of the last coop_inv_restore (1 = applied)");
	if (gEnv->pConsole && !s_pAutoVehTest)
		s_pAutoVehTest = gEnv->pConsole->RegisterInt("coop_autovehtest", 0, 0, "Crysis Coop testing: run coop_vehtest after N s in a vehicle");
	if (gEnv->pConsole && !s_pSPWorld)
		s_pSPWorld = gEnv->pConsole->RegisterInt("coop_sp_world", 2, 0,
			"Crysis Coop: 1 = server treats the running level as single player (story scripts, AI vehicles)");
	if (gEnv->pConsole && !s_pKeepCat)
		s_pKeepCat = gEnv->pConsole->RegisterInt("coop_debug_keep_dyn_cat", 0, 0,
			"Crysis Coop debugging: category keeping dynamic aspects (1 actors 2 items 3 vehicles 4 other)");
	if (gEnv->pConsole && !s_pAllAspectMask)
		s_pAllAspectMask = gEnv->pConsole->RegisterInt("coop_debug_all_aspects_off", 0, 0,
			"Crysis Coop debugging: aspect bits disabled on the server for every game object");
	if (gEnv->pConsole && !s_pAspectMask)
		s_pAspectMask = gEnv->pConsole->RegisterInt("coop_debug_actor_aspects_off", 0, 0,
			"Crysis Coop debugging: aspect bits disabled on the server for every actor");
	if (gEnv->pConsole && !s_pDebugFlags)
		s_pDebugFlags = gEnv->pConsole->RegisterInt("coop_debug_flags", 0, 0,
			"Crysis Coop debugging: 1 = no server AI, 2 = no linked physics profile for AI in vehicles");
}

namespace
{
	// campaign levels installed for coop live in Levels/Multiplayer/TIA/coop_*
	bool IsCoopLevel(const char* levelName)
	{
		if (!levelName)
			return false;
		const char* base = strrchr(levelName, '/');
		base = base ? base + 1 : levelName;
		return strnicmp(base, "coop_", 5) == 0;
	}
}

void CoopNetSerClearTrace();

void CoopAI::OnLoadingStart(const char* levelName)
{
	CoopNetSerClearTrace();
	CoopSave::OnLoadingStart(levelName);
	ResetFlowMirror();
	ResetSync();
	ResetLoadoutCatchup();
	ResetAIWake();
	ResetJoinerProximity();
	TraceOpen(levelName);
	RegisterVoiceListener();
	s_inLoading = true;
	s_coopServerLoading = false;
	s_coopLevelLoading = gEnv->bMultiplayer && IsCoopLevel(levelName);
	s_coopLevelActive = s_coopLevelLoading;
	if (!s_spawnLogAdded && gEnv->pEntitySystem)
	{
		gEnv->pEntitySystem->AddSink(&s_spawnLog);
		s_spawnLogAdded = true;
	}
	RegisterCommands();
	s_levelReady = false;
	s_attempted.clear();
	s_aiAspectsOff.clear();
	ClearProtectedVehicles();
	ClearInventorySnapshots(levelName);
	WatchGlobalTokens(levelName);

	if (s_spWorldActive)
	{
		gEnv->bMultiplayer = true;
		s_spWorldActive = false;
	}

	if (s_clientSPLoad)
	{
		gEnv->bMultiplayer = true;
		s_clientSPLoad = false;
	}

	s_coopServer = IsCoopServer();
	if (!s_coopServer)
	{
		// a coop client must load the level exactly as the server does,
		// otherwise both sides end up with different sets of level entities
		// and the server's static objects cannot be matched
		// ("Failed ReconfigureObject")
		if (gEnv->bMultiplayer && !gEnv->bServer && s_pSPWorld && s_pSPWorld->GetIVal() != 0 && IsCoopLevel(levelName))
		{
			gEnv->bMultiplayer = false;
			s_clientSPLoad = true;
			CryLogAlways("[CoopAI] coop_sp_world=1: client loads this level in single-player mode");
		}
		return;
	}

	s_coopServerLoading = IsCoopLevel(levelName);

	// AI equipment is queued in Lua while this is set (see EquipActor)
	// with load-time item spawns bound as dynamic network objects (see the
	// entity sink) equipment can be handed out while loading, exactly when
	// the campaign does it (level scripts check it right at the start)
	{
		ICVar* pDefer = gEnv->pConsole->GetCVar("coop_defer_equip");
		gEnv->pScriptSystem->SetGlobalValue("g_coopLoading", pDefer && pDefer->GetIVal() != 0);
	}
	s_equipPending = true;
	s_equipDelay = 1.0f;

	// before the level's entities are spawned, so they get AI objects
	CryLogAlways("[CoopAI] level loading: enabling AI system on the server");
	gEnv->pAISystem->Enable(true);

	// before the HUD, objectives, PDA map and entities are created, so the
	// whole level is set up exactly as in the single-player campaign
	CryLogAlways("[CoopAI] level: %s (coop level: %d)", levelName, (int)IsCoopLevel(levelName));
	if (s_pSPWorld && s_pSPWorld->GetIVal() != 0 && IsCoopLevel(levelName))
	{
		gEnv->bMultiplayer = false;
		s_spWorldActive = true;
		BlockSaveLoad(true);
		CryLogAlways("[CoopAI] coop_sp_world=1: server loads this level in single-player mode");
	}
}

namespace
{
	// diagnostics: physical breaks on this machine (trees, planks, glass...),
	// to compare what the server broke with what a client shows
	const char* BreakSource(void* pForeignData, int iForeignData)
	{
		if (iForeignData == PHYS_FOREIGN_ID_ENTITY && pForeignData)
			return static_cast<IEntity*>(pForeignData)->GetName();
		return iForeignData == PHYS_FOREIGN_ID_STATIC ? "static" : iForeignData == PHYS_FOREIGN_ID_FOLIAGE ? "foliage" : "other";
	}

	int OnPhysPartCreated(const EventPhys* pEvent)
	{
		const EventPhysCreateEntityPart* p = static_cast<const EventPhysCreateEntityPart*>(pEvent);
		pe_status_pos pos;
		Vec3 at(0, 0, 0);
		if (p->pEntNew && p->pEntNew->GetStatus(&pos))
			at = pos.pos;
		CoopAI::Trace("BREAK part of %s (%s) at (%.1f,%.1f,%.1f)", BreakSource(p->pForeignData, p->iForeignData),
			p->iReason == EventPhysCreateEntityPart::ReasonJointsBroken ? "joints" : "mesh", at.x, at.y, at.z);
		return 1;
	}

	int OnPhysJointBroken(const EventPhys* pEvent)
	{
		const EventPhysJointBroken* p = static_cast<const EventPhysJointBroken*>(pEvent);
		CoopAI::Trace("BREAK joint of %s at (%.1f,%.1f,%.1f)", BreakSource(p->pForeignData[0], p->iForeignData[0]), p->pt.x, p->pt.y, p->pt.z);
		return 1;
	}
}

namespace
{
	// AI actors: their client/server dynamic aspects (input, suit) are
	// written by CryAction differently on the server (AI active) and on the
	// clients (AI inactive), which breaks the network stream. Positions,
	// health and weapons still travel in the other aspects. This must happen
	// before the clients bind the objects: a client that loads the next
	// level together with the server (coop map change) binds them right
	// after the load, and an aspect switched off later breaks its stream
	// ("Missing end marker for object update", then disconnected).
	int DisableAIDynamicAspects()
	{
		int n = 0;
		IActorIteratorPtr pActors = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = pActors->Next())
		{
			if (pActor->IsPlayer())
				continue;
			if (s_aiAspectsOff.insert(pActor->GetEntityId()).second)
			{
				pActor->GetGameObject()->EnableAspect(eEA_GameClientDynamic | eEA_GameServerDynamic, false);
				++n;
			}
		}
		return n;
	}
}

void CoopAI::OnLoadingComplete()
{
	static bool s_breakTrace = false;
	if (!s_breakTrace && gEnv->pPhysicalWorld)
	{
		s_breakTrace = true;
		gEnv->pPhysicalWorld->AddEventClient(EventPhysCreateEntityPart::id, OnPhysPartCreated, 1);
		gEnv->pPhysicalWorld->AddEventClient(EventPhysJointBroken::id, OnPhysJointBroken, 1);
	}
	{
		const char* names[] = { "es_MaxPhysDist", "es_MaxPhysDistInvisible", "es_UsePhysVisibilityChecks", "g_VisibilityTimeout",
			"g_VisibilityTimeoutTime", "ai_UpdateAllAlways", "ai_IgnoreVisibilityChecks" };
		string line;
		for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); ++i)
			if (ICVar* pVar = gEnv->pConsole->GetCVar(names[i]))
				line += string(" ") + names[i] + "=" + pVar->GetString();
		CryLogAlways("[CoopAI] engine update settings:%s", line.c_str());
	}
	s_inLoading = false;
	s_coopServerLoading = false;
	// the interactive objects' flags as loaded, before the story changes them
	if (gEnv->bServer && g_pGame)
		ScanUsableObjects(true);
	s_coopLevelLoading = false;
	RegisterCommands();

	if (s_clientSPLoad)
	{
		gEnv->bMultiplayer = true;
		s_clientSPLoad = false;
		CryLogAlways("[CoopAI] client level loaded, back to network mode");
		s_levelReadyClient = true;
		if (DebugFlags() & 8)
			DumpEntities("loaded");
		return;
	}

	s_coopServer = IsCoopServer() || (s_spWorldActive && gEnv->bServer && gEnv->pAISystem);
	if (!s_coopServer)
	{
		if (!gEnv->bServer && (DebugFlags() & 8))
		{
			DumpEntities("loaded");
			gEnv->pConsole->ExecuteString("net_dump_object_state");
		}
		return;
	}

	gEnv->pAISystem->Enable(true);
	LogStats("after load");

	const int registered = RegisterMissingAI();
	if (registered > 0)
	{
		CryLogAlways("[CoopAI] registered AI for %d entities that were spawned while AI was disabled", registered);
		LogStats("after re-register");
	}

	DisableScriptPhysicsSync();
	RestoreGlobalTokens();
	CryLogAlways("[CoopAI] dynamic network aspects switched off for %d AI actors", DisableAIDynamicAspects());
	RegisterFlowInspector();

	CryLogAlways("[CoopAI] level loaded: AI system reset for game start");
	gEnv->pAISystem->Reset(IAISystem::RESET_ENTER_GAME);
	LogStats("after reset");
	if (DebugFlags() & 8)
	{
		DumpEntities("loaded");
		s_dumpTimer = 25.0f;
	}

	s_levelReady = true;
	if (ILevel* pLevel = g_pGame->GetIGameFramework()->GetILevelSystem()->GetCurrentLevel())
		if (pLevel->GetLevelInfo() && IsCoopLevel(pLevel->GetLevelInfo()->GetName()))
			CoopSave::OnLevelReady(pLevel->GetLevelInfo()->GetName());

	// coop_sp_world 2: single player only while loading (HUD, objectives,
	// level entities are created as in the campaign), network mode while
	// playing
	if (s_spWorldActive && s_pSPWorld && s_pSPWorld->GetIVal() == 2)
	{
		gEnv->bMultiplayer = true;
		s_spWorldActive = false;
		CryLogAlways("[CoopAI] coop_sp_world=2: level loaded, server back to network mode");
	}
	s_updateTimer = 0.0f;
}

void CoopAI::OnGameEnded()
{
	s_coopLevelActive = false;
	if (!gEnv->bServer)
	{
		CoopNetSerDumpTrace();
		if (DebugFlags() & 8)
			gEnv->pConsole->ExecuteString("net_dump_object_state");
	}
	s_equipPending = false;
	if (gEnv->pScriptSystem)
		gEnv->pScriptSystem->SetGlobalValue("g_coopLoading", false);
	if (DebugFlags() & 8)
		DumpEntities("disconnected");
	s_levelReady = false;
	if (s_spWorldActive || s_clientSPLoad)
	{
		gEnv->bMultiplayer = true;
		s_spWorldActive = false;
		s_clientSPLoad = false;
	}
	BlockSaveLoad(false);
}

void CoopAI::CollectInventories(std::map<string, SInventory>& out, bool includeLocal)
{
	IGameFramework* pFramework = g_pGame->GetIGameFramework();
	IActor* pLocal = pFramework->GetClientActor();
	IActorIteratorPtr pIt = pFramework->GetIActorSystem()->CreateActorIterator();
	while (IActor* pActor = pIt->Next())
	{
		if (!pActor->IsPlayer() || (!includeLocal && pActor == pLocal))
			continue;
		SInvSnapshot snap;
		if (pActor->GetHealth() <= 0 || !TakeInventorySnapshot(pActor, snap))
		{
			std::map<EntityId, SInvSnapshot>::iterator it = s_invSnapshots.find(pActor->GetEntityId());
			if (it == s_invSnapshots.end())
				continue;
			snap = it->second;
		}
		snap.name = pActor->GetEntity()->GetName();
		out[PlayerKey(pActor)] = snap;
	}
	// what the previous level left them and they did not get yet (not joined
	// again, not equipped yet)
	const string localKey = pLocal ? PlayerKey(pLocal) : string();
	for (std::map<string, SInvSnapshot>::const_iterator it = s_invCarry.begin(); it != s_invCarry.end(); ++it)
		if ((includeLocal || it->first != localKey) && out.find(it->first) == out.end())
			out[it->first] = it->second;
}

string CoopAI::PlayerKey(IActor* pActor)
{
	IGameFramework* pFramework = g_pGame->GetIGameFramework();
	string id;
	if (pActor == pFramework->GetClientActor())
		id = CoopRelay::LocalPlayerId();
	else if (INetChannel* pChannel = pActor->GetChannelId() ? pFramework->GetNetChannel(pActor->GetChannelId()) : 0)
	{
		// the host's end of the friend's tunnel: its local port says who he is
		const char* name = pChannel->GetName();
		const char* colon = name ? strrchr(name, ':') : 0;
		if (colon)
			id = CoopRelay::PlayerIdOfPort(atoi(colon + 1));
	}
	return id.empty() ? string("name:") + pActor->GetEntity()->GetName() : "id:" + id;
}

void CoopAI::GiveInventory(IActor* pTarget, const SInventory& inv, bool replace)
{
	IItemSystem* pItemSystem = g_pGame->GetIGameFramework()->GetIItemSystem();
	IInventory* pInv = pTarget->GetInventory();
	if (!pInv)
		return;
	if (replace)
	{
		pInv->Destroy();
		pInv->ResetAmmo();
	}
	for (size_t i = 0; i < inv.items.size(); ++i)
		if (pInv->GetCountOfClass(inv.items[i].c_str()) == 0)
			pItemSystem->GiveItem(pTarget, inv.items[i].c_str(), false, false, false);
	for (size_t i = 0; i < inv.ammo.size(); ++i)
		if (IEntityClass* pAmmo = gEnv->pEntitySystem->GetClassRegistry()->FindClass(inv.ammo[i].first.c_str()))
			pInv->SetAmmoCount(pAmmo, inv.ammo[i].second);
	if (!inv.current.empty())
		if (IScriptTable* pScript = pTarget->GetEntity()->GetScriptTable())
		{
			SmartScriptTable actorTable;
			if (pScript->GetValue("actor", actorTable))
				Script::CallMethod(actorTable, "SelectItemByName", inv.current.c_str());
		}
}

void CoopAI::SetInventoryCarry(const char* level, const std::map<string, SInventory>& inventories)
{
	s_invCarry = inventories;
	s_invCarryLevel = LevelShortName(level);
	for (std::map<string, SInvSnapshot>::const_iterator it = s_invCarry.begin(); it != s_invCarry.end(); ++it)
		CryLogAlways("[CoopInv] %s: %d items / %d ammo types carried to %s", it->first.c_str(),
			(int)it->second.items.size(), (int)it->second.ammo.size(), s_invCarryLevel.c_str());
}

void CoopAI::OnGameLoaded()
{
	// entities the save brought back (soldiers spawned during play, script
	// objects) are new game objects: the same network setup as on level load
	s_aiAspectsOff.clear();
	const int ai = DisableAIDynamicAspects();
	DisableScriptPhysicsSync();
	// the snapshots describe the players before the load
	s_invSnapshots.clear();
	CryLogAlways("[CoopAI] saved game loaded: dynamic network aspects switched off for %d AI actors", ai);
}

int CoopAI::DebugFlags()
{
	return s_pDebugFlags ? s_pDebugFlags->GetIVal() : 0;
}

bool CoopAI::IsCoopSession()
{
	if (!IsNetGame() || !g_pGame)
		return false;
	ILevel* pLevel = g_pGame->GetIGameFramework()->GetILevelSystem()->GetCurrentLevel();
	if (!pLevel || !pLevel->GetLevelInfo())
		return false;
	return IsCoopLevel(pLevel->GetLevelInfo()->GetName());
}

bool CoopAI::IsNetGame()
{
	return gEnv->bMultiplayer || s_spWorldActive;
}

namespace
{
	float s_posLogTimer = 0.0f;

	// every 5 s: where each player is on this machine (compare server/client)
	void LogPlayerPositions(float frameTime)
	{
		if (!gEnv->bMultiplayer || !CoopAI::IsCoopSession())
			return;
		s_posLogTimer += frameTime;
		if (s_posLogTimer < 5.0f)
			return;
		s_posLogTimer = 0.0f;
		IActorIteratorPtr pActors = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
		string line;
		while (IActor* pActor = pActors->Next())
		{
			if (!pActor->IsPlayer())
				continue;
			Vec3 p = pActor->GetEntity()->GetWorldPos();
			char buf[128];
			_snprintf(buf, sizeof(buf), " %s=(%.0f,%.0f,%.0f)hp%d", pActor->GetEntity()->GetName(), p.x, p.y, p.z, pActor->GetHealth());
			buf[sizeof(buf)-1] = 0;
			line += buf;
		}
		CryLogAlways("[CoopPos] %s:%s", gEnv->bServer ? "server" : "client", line.c_str());

		// the 6 AI soldiers closest to the host player (same names on both
		// sides): compare their positions/health server vs client
		{
			IActor* pRef = 0;
			IActorIteratorPtr pIt0 = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
			while (IActor* pA = pIt0->Next())
				if (pA->IsPlayer() && !strcmp(pA->GetEntity()->GetName(), "Nomad")) { pRef = pA; break; }
			if (pRef)
			{
				const Vec3 ref = pRef->GetEntity()->GetWorldPos();
				std::vector<std::pair<float, IActor*> > nearAI;
				IActorIteratorPtr pIt2 = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
				while (IActor* pA = pIt2->Next())
				{
					if (pA->IsPlayer())
						continue;
					float d = (pA->GetEntity()->GetWorldPos() - ref).GetLength();
					if (d < 120.0f)
						nearAI.push_back(std::make_pair(d, pA));
				}
				std::sort(nearAI.begin(), nearAI.end());
				string ai;
				for (size_t i = 0; i < nearAI.size() && i < 6; ++i)
				{
					IActor* pA = nearAI[i].second;
					Vec3 p = pA->GetEntity()->GetWorldPos();
					char buf[128];
					_snprintf(buf, sizeof(buf), " %s=(%.0f,%.0f,%.0f)hp%d", pA->GetEntity()->GetName(), p.x, p.y, p.z, pA->GetHealth());
					buf[sizeof(buf)-1] = 0;
					ai += buf;
				}
				CryLogAlways("[CoopAIPos] %s:%s", gEnv->bServer ? "server" : "client", ai.c_str());
			}
		}

		// the 8 AI actors closest to the first player: compare server/client
		{
			IActor* pRef = g_pGame->GetIGameFramework()->GetClientActor();
			if (pRef)
			{
				const Vec3 ref = pRef->GetEntity()->GetWorldPos();
				std::vector<std::pair<float, IActor*> > near;
				IActorIteratorPtr pIt2 = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
				while (IActor* pA = pIt2->Next())
				{
					if (pA->IsPlayer())
						continue;
					float d = (pA->GetEntity()->GetWorldPos() - ref).GetLength();
					if (d < 150.0f)
						near.push_back(std::make_pair(d, pA));
				}
				std::sort(near.begin(), near.end());
				string ai;
				for (size_t i = 0; i < near.size() && i < 8; ++i)
				{
					IActor* pA = near[i].second;
					Vec3 p = pA->GetEntity()->GetWorldPos();
					char buf[128];
					_snprintf(buf, sizeof(buf), " %s=(%.0f,%.0f,%.0f)hp%d", pA->GetEntity()->GetName(), p.x, p.y, p.z, pA->GetHealth());
					buf[sizeof(buf)-1] = 0;
					ai += buf;
				}
				CryLogAlways("[CoopAIPos] %s:%s", gEnv->bServer ? "server" : "client", ai.c_str());
			}
		}

		// input state of the local player (why can't he drive/move?)
		IActionMapManager* pAM = g_pGame->GetIGameFramework()->GetIActionMapManager();
		IActor* pLocal = g_pGame->GetIGameFramework()->GetClientActor();
		if (pAM && pLocal)
		{
			static const char* filters[] = { "cutscene", "cutscene_no_player", "no_move", "no_mouse",
				"vehicle_no_seat_change_and_exit", "freezetime", "in_vehicle_suit_menu", "no_connectivity", "coop_no_saveload" };
			string f;
			for (int i = 0; i < (int)(sizeof(filters)/sizeof(filters[0])); ++i)
				if (pAM->IsFilterEnabled(filters[i])) { f += " "; f += filters[i]; }
			IActionMap* pVeh = pAM->GetActionMap("vehicle");
			IActionMap* pPl = pAM->GetActionMap("player");
			if (IVehicle* pV = pLocal->GetLinkedVehicle())
			{
				IVehicleMovement* pM = pV->GetMovement();
				CryLogAlways("[CoopInput] vehicle %s powered=%d processing=%d damage=%.2f destroyed=%d", pV->GetEntity()->GetName(),
					pM ? (int)pM->IsPowered() : -1, pM ? (int)pM->IsMovementProcessingEnabled() : -1,
					pM ? pM->GetDamageRatio() : -1.0f, (int)pV->IsDestroyed());
			}
			CryLogAlways("[CoopInput] %s filters:%s | maps vehicle=%d player=%d | vehicle=%s paused=%d", pLocal->GetEntity()->GetName(),
				f.c_str(), pVeh ? (int)pVeh->Enabled() : -1, pPl ? (int)pPl->Enabled() : -1,
				pLocal->GetLinkedVehicle() ? pLocal->GetLinkedVehicle()->GetEntity()->GetName() : "-",
				(int)g_pGame->GetIGameFramework()->IsGamePaused());
		}
	}
}

namespace
{
	// Vehicles carrying players are made indestructible while occupied; when
	// they are about to be destroyed the players die instead and the vehicle
	// goes back, intact, to where it was a few seconds earlier - what a
	// campaign checkpoint would do. The level's own vehicles (story VTOL,
	// tanks, boats) are therefore never lost in the coop.
	struct SProtectedVehicle
	{
		Matrix34 safe[6];
		int count = 0;
		float emptyTime = 0.0f;
	};
	std::map<EntityId, SProtectedVehicle> s_protectedVehicles;
	float s_protectTimer = 0.0f;

	void ClearProtectedVehicles()
	{
		s_protectedVehicles.clear();
	}

	void SetIndestructible(IVehicle* pVehicle, bool on)
	{
		SVehicleEventParams params;
		params.bParam = on;
		pVehicle->BroadcastVehicleEvent(eVE_Indestructible, params);
	}

	void RepairVehicle(IVehicle* pVehicle)
	{
		for (int i = 0; i < pVehicle->GetComponentCount(); ++i)
			if (IVehicleComponent* pComp = pVehicle->GetComponent(i))
				pComp->SetDamageRatio(0.0f);
		SVehicleEventParams params;
		params.fParam = 0.0f;
		pVehicle->BroadcastVehicleEvent(eVE_Repair, params);
	}

	void KillPlayer(IActor* pActor)
	{
		CGameRules* pGameRules = g_pGame->GetGameRules();
		IScriptTable* pRules = pGameRules ? pGameRules->GetEntity()->GetScriptTable() : 0;
		IScriptTable* pPlayer = pActor->GetEntity()->GetScriptTable();
		if (pRules && pPlayer && pRules->GetValueType("KillPlayer") == svtFunction)
			Script::CallMethod(pRules, "KillPlayer", pPlayer);
	}

	void UpdateVehicleProtection(float frameTime)
	{
		s_protectTimer += frameTime;
		if (s_protectTimer < 0.5f)
			return;
		const float dt = s_protectTimer;
		s_protectTimer = 0.0f;

		IGameFramework* pFramework = g_pGame->GetIGameFramework();
		IVehicleSystem* pVehicleSystem = pFramework->GetIVehicleSystem();

		// vehicles with players in them
		std::set<EntityId> occupied;
		IActorIteratorPtr pActors = pFramework->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = pActors->Next())
		{
			if (!pActor->IsPlayer() || pActor->GetHealth() <= 0)
				continue;
			if (IVehicle* pVehicle = pActor->GetLinkedVehicle())
				occupied.insert(pVehicle->GetEntityId());
		}

		for (std::set<EntityId>::iterator it = occupied.begin(); it != occupied.end(); ++it)
		{
			IVehicle* pVehicle = pVehicleSystem->GetVehicle(*it);
			if (!pVehicle || pVehicle->IsDestroyed())
				continue;
			std::map<EntityId, SProtectedVehicle>::iterator pv = s_protectedVehicles.find(*it);
			if (pv == s_protectedVehicles.end())
			{
				SetIndestructible(pVehicle, true);
				pv = s_protectedVehicles.insert(std::make_pair(*it, SProtectedVehicle())).first;
				CryLogAlways("[CoopAI] vehicle %s protected (players aboard)", pVehicle->GetEntity()->GetName());
			}
			SProtectedVehicle& prot = pv->second;
			prot.emptyTime = 0.0f;

			const float damage = pVehicle->GetDamageRatio(true);
			if (damage < 0.5f)
			{
				// keep a short history of safe transforms (~3 s)
				if (prot.count < 6)
					prot.safe[prot.count++] = pVehicle->GetEntity()->GetWorldTM();
				else
				{
					for (int i = 1; i < 6; ++i)
						prot.safe[i-1] = prot.safe[i];
					prot.safe[5] = pVehicle->GetEntity()->GetWorldTM();
				}
			}
			else if (damage >= 0.95f)
			{
				CryLogAlways("[CoopAI] vehicle %s would be destroyed (damage %.2f): players die, vehicle goes back intact",
					pVehicle->GetEntity()->GetName(), damage);
				for (unsigned int s = 1; s <= pVehicle->GetSeatCount(); ++s)
				{
					IVehicleSeat* pSeat = pVehicle->GetSeatById((TVehicleSeatId)s);
					IActor* pPassenger = pSeat ? pFramework->GetIActorSystem()->GetActor(pSeat->GetPassenger()) : 0;
					if (pPassenger && pPassenger->IsPlayer() && pPassenger->GetHealth() > 0)
						KillPlayer(pPassenger);
				}
				RepairVehicle(pVehicle);
				if (prot.count > 0)
					pVehicle->GetEntity()->SetWorldTM(prot.safe[0]);
				if (IPhysicalEntity* pPhys = pVehicle->GetEntity()->GetPhysics())
				{
					pe_action_set_velocity v;
					v.v = Vec3(0, 0, 0);
					v.w = Vec3(0, 0, 0);
					pPhys->Action(&v);
				}
				prot.count = 0;
			}
		}

		// unprotect vehicles left alone for a while (level scripts may
		// want to destroy them)
		for (std::map<EntityId, SProtectedVehicle>::iterator it = s_protectedVehicles.begin(); it != s_protectedVehicles.end(); )
		{
			if (occupied.find(it->first) == occupied.end())
			{
				it->second.emptyTime += dt;
				// players respawn within ~10 s and are put back in
				if (it->second.emptyTime > 30.0f)
				{
					if (IVehicle* pVehicle = pVehicleSystem->GetVehicle(it->first))
						SetIndestructible(pVehicle, false);
					s_protectedVehicles.erase(it++);
					continue;
				}
			}
			++it;
		}
	}
}

namespace
{
	struct SObjResend { int channel; float t; int left; };
	std::vector<SObjResend> s_objResend;

	void UpdateObjectiveResend(float frameTime)
	{
		for (size_t i = 0; i < s_objResend.size(); )
		{
			SObjResend& r = s_objResend[i];
			r.t -= frameTime;
			if (r.t <= 0.0f)
			{
				if (CGameRules* pRules = g_pGame->GetGameRules())
					if (pRules->GetActorByChannelId(r.channel))
					{
						pRules->CoopSendObjectives(r.channel);
						SendSyncHistory(r.channel);
					}
				if (--r.left > 0)
				{
					r.t = 10.0f;
					++i;
					continue;
				}
				s_objResend.erase(s_objResend.begin() + i);
				continue;
			}
			++i;
		}
	}
}

void CoopAI::QueueObjectiveResend(int channelId)
{
	SObjResend r = { channelId, 5.0f, 3 };
	s_objResend.push_back(r);
}

namespace
{
	// AI soldiers' movement to the clients (5 per second, near the players)
	float s_aiMoveTimer = 0.0f;
	void SendAIMovement(float frameTime)
	{
		s_aiMoveTimer += frameTime;
		if (s_aiMoveTimer < 0.2f)
			return;
		s_aiMoveTimer = 0.0f;
		CGameRules* pRules = g_pGame->GetGameRules();
		if (!pRules)
			return;
		IActorSystem* pActorSystem = g_pGame->GetIGameFramework()->GetIActorSystem();
		// positions of the remote players
		std::vector<Vec3> remote;
		IActorIteratorPtr pIt = pActorSystem->CreateActorIterator();
		while (IActor* pA = pIt->Next())
			if (pA->IsPlayer() && pA->GetChannelId() && !pA->IsClient())
				remote.push_back(pA->GetEntity()->GetWorldPos());
		if (remote.empty())
			return;
		IActorIteratorPtr pIt2 = pActorSystem->CreateActorIterator();
		while (IActor* pA = pIt2->Next())
		{
			if (pA->IsPlayer() || pA->GetHealth() <= 0 || static_cast<CActor*>(pA)->GetActorClass() != CPlayer::GetActorClassType())
				continue;
			if (!CoopAI::IsNetBound(pA->GetEntityId()))
				continue;
			const Vec3 p = pA->GetEntity()->GetWorldPos();
			bool nearAny = false;
			for (size_t i = 0; i < remote.size() && !nearAny; ++i)
				nearAny = (p - remote[i]).GetLengthSquared() < 250.0f * 250.0f;
			if (!nearAny)
				continue;
			SSerializedPlayerInput in;
			static_cast<CPlayer*>(pA)->GetAIInputState(in);
			pRules->CoopSendAIMove(pA->GetEntityId(), in.stance, in.deltaMovement, in.lookDirection, in.sprint);
		}
	}
}

namespace
{
	// client test: the local player "uses" an entity (goes through the
	// normal Player:UseEntity path, i.e. the coop forwarding to the server)
	float s_clientUseTime = 0.0f;
	bool s_clientUseDone = false;
	void UpdateClientUseTest(float frameTime)
	{
		ICVar* pName = gEnv->pConsole->GetCVar("coop_debug_client_use");
		if (s_clientUseDone || !pName || !pName->GetString() || !pName->GetString()[0])
			return;
		IActor* pLocal = g_pGame->GetIGameFramework()->GetClientActor();
		if (!pLocal || pLocal->GetHealth() <= 0)
			return;
		IEntity* pTarget = gEnv->pEntitySystem->FindEntityByName(pName->GetString());
		static float s_dbgT = 0.0f;
		s_dbgT += frameTime;
		if (s_dbgT > 10.0f)
		{
			s_dbgT = 0.0f;
			CryLogAlways("[CoopUseTest] waiting: target=%d dist=%.1f local=%s", pTarget ? 1 : 0,
				pTarget ? (pTarget->GetWorldPos() - pLocal->GetEntity()->GetWorldPos()).GetLength() : -1.0f, pLocal->GetEntity()->GetName());
		}
		if (!pTarget || (pTarget->GetWorldPos() - pLocal->GetEntity()->GetWorldPos()).GetLength() > 5.0f)
			return;
		s_clientUseTime += frameTime;
		if (s_clientUseTime < 2.0f)
			return;
		s_clientUseDone = true;
		IScriptTable* pScript = pLocal->GetEntity()->GetScriptTable();
		CryLogAlways("[CoopUseTest] client uses %s (found=%d)", pName->GetString(), pTarget ? 1 : 0);
		if (pTarget && pScript)
		{
			Script::CallMethod(pScript, "UseEntity", ScriptHandle(pTarget->GetId()), 0, true);
			Script::CallMethod(pScript, "UseEntity", ScriptHandle(pTarget->GetId()), 0, false);
		}
	}
}

namespace
{
	// testing: the local (host / single player) player walks by itself to the
	// active mission objective, in steps, so level triggers and AI react as to
	// a real player
	float s_autoTimer = 0.0f;
	float s_autoUseTime = -100.0f;
	int s_autoWander = 0;
	std::map<EntityId, int> s_autoUsed;
	string s_autoUsedFor;

	// can a player use this entity (console, switch, door...)? not actors,
	// vehicles or items: the autopilot must not end up in a vehicle
	int AutopilotUsable(IEntity* pEntity, IEntity* pUser)
	{
		IScriptTable* pScript = pEntity->GetScriptTable();
		if (!pScript || !pUser || pScript->GetValueType("OnUsed") != svtFunction)
			return 0;
		IGameFramework* pFramework = g_pGame->GetIGameFramework();
		const EntityId id = pEntity->GetId();
		if (pFramework->GetIActorSystem()->GetActor(id) || pFramework->GetIVehicleSystem()->GetVehicle(id) || pFramework->GetIItemSystem()->GetItem(id))
			return 0;
		// only things that say they are usable (consoles, switches, doors...)
		if (pScript->GetValueType("IsUsable") != svtFunction)
			return 0;
		// used twice already without the objective changing: try something else
		std::map<EntityId, int>::iterator used = s_autoUsed.find(id);
		if (used != s_autoUsed.end() && used->second >= 2)
			return 0;
		HSCRIPTFUNCTION func = 0;
		if (!pScript->GetValue("IsUsable", func) || !func)
			return 0;
		int usable = 0;
		Script::CallReturn(gEnv->pScriptSystem, func, pScript, pUser->GetScriptTable(), usable);
		gEnv->pScriptSystem->ReleaseFunc(func);
		return usable;
	}

	// the player is in the air (plane, HALO jump, parachute) or a cutscene
	// runs: the level scripts move him, the autopilot must not
	bool AutopilotBusy(CActor* pActor)
	{
		if (const SActorStats* pStats = pActor->GetActorStats())
			if (pStats->inAir > 0.2f)
				return true;
		if (IViewSystem* pView = g_pGame->GetIGameFramework()->GetIViewSystem())
			if (pView->IsPlayingCutScene())
				return true;
		return false;
	}

	// coop_debug_autopilot_target 1: the nearest hostile soldier that is in
	// the world (not hidden) within 400 m
	IEntity* AutopilotEnemy(CActor* pPlayer)
	{
		IAIObject* pPlayerAI = pPlayer->GetEntity()->GetAI();
		const Vec3 from = pPlayer->GetEntity()->GetWorldPos();
		IEntity* pBest = 0;
		float best = 400.0f;
		IActorIteratorPtr pIt = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = pIt->Next())
		{
			if (pActor->IsPlayer() || pActor->GetHealth() <= 0 || pActor->GetEntity()->IsHidden() || pActor->GetLinkedVehicle())
				continue;
			IAIObject* pAI = pActor->GetEntity()->GetAI();
			if (!pAI || !pAI->CastToIPuppet() || (pPlayerAI && !pAI->IsHostile(pPlayerAI)))
				continue;
			const float d = (pActor->GetEntity()->GetWorldPos() - from).GetLength();
			if (d < best)
			{
				best = d;
				pBest = pActor->GetEntity();
			}
		}
		return pBest;
	}

	// the active objective to walk to: the newest primary objective; objectives
	// that follow a person (tracked entity is an actor) only when nothing else
	const CHUDMissionObjective* AutopilotObjective()
	{
		CHUD* pHUD = g_pGame->GetHUD();
		if (!pHUD)
			return 0;
		const std::vector<CHUDMissionObjective>& objs = pHUD->GetMissionObjectiveSystem().GetObjectives();
		const CHUDMissionObjective* pBest = 0;
		int bestRank = -1;
		IActorSystem* pActorSystem = g_pGame->GetIGameFramework()->GetIActorSystem();
		for (size_t i = 0; i < objs.size(); ++i)
		{
			const CHUDMissionObjective& o = objs[i];
			if (o.GetStatus() != CHUDMissionObjective::ACTIVATED || !o.GetTrackedEntity())
				continue;
			const int rank = (o.IsSecondary() ? 0 : 2) + (pActorSystem->GetActor(o.GetTrackedEntity()) ? 0 : 1);
			if (!pBest || rank > bestRank || (rank == bestRank && o.GetLastTimeChanged() > pBest->GetLastTimeChanged()))
			{
				pBest = &o;
				bestRank = rank;
			}
		}
		return pBest;
	}

	// coop_debug_autopilot N: every N s players step toward the active
	// objective. coop_debug_autopilot_who: 0 the host's own player, 1 every
	// player, 2 only the joined players (the second player drives the story)
	void UpdateAutopilot(float frameTime)
	{
		ICVar* pCVar = gEnv->pConsole->GetCVar("coop_debug_autopilot");
		if (!pCVar || pCVar->GetIVal() <= 0 || !gEnv->bServer)
			return;
		if (gEnv->pTimer->GetCurrTime() < s_autopilotHoldUntil)
			return;
		s_autoTimer += frameTime;
		if (s_autoTimer < (float)pCVar->GetIVal())
			return;
		s_autoTimer = 0.0f;
		ICVar* pWho = gEnv->pConsole->GetCVar("coop_debug_autopilot_who");
		const int who = pWho ? pWho->GetIVal() : 0;
		ICVar* pTargetMode = gEnv->pConsole->GetCVar("coop_debug_autopilot_target");
		const bool enemies = pTargetMode && pTargetMode->GetIVal() == 1;
		CGameRules* pRules = g_pGame->GetGameRules();
		const CHUDMissionObjective* pBest = AutopilotObjective();
		IEntity* pTarget = pBest ? gEnv->pEntitySystem->GetEntity(pBest->GetTrackedEntity()) : 0;
		if (!pRules)
			return;
		IActor* pLocal = g_pGame->GetIGameFramework()->GetClientActor();
		if (!pTarget && !enemies)
		{
			// no objective yet: walk around a little, as a player would (many
			// levels hand out the first objective once the player moves)
			++s_autoWander;
			IActorIteratorPtr pIt = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
			while (IActor* pIActor = pIt->Next())
			{
				if (!pIActor->IsPlayer() || ((who == 0) != (pIActor == pLocal) && who != 1))
					continue;
				CActor* pActor = static_cast<CActor*>(pIActor);
				if (pActor->GetHealth() <= 0 || pActor->GetLinkedVehicle() || pActor->GetSpectatorMode() != 0)
					continue;
				if (pIActor == pLocal && gEnv->pTimer->GetCurrTime() - s_lastMoveInput < 10.0f)
					continue;
				if (AutopilotBusy(pActor))
					continue;
				const float a = s_autoWander * 1.3f;
				Vec3 step = pActor->GetEntity()->GetWorldPos() + Vec3(cosf(a), sinf(a), 0.0f) * 3.0f;
				step.z = max(step.z, gEnv->p3DEngine->GetTerrainElevation(step.x, step.y)) + 0.3f;
				pRules->MovePlayer(pActor, step, Ang3(0, 0, a));
				CryLogAlways("[CoopAuto] %s wanders (no objective)", pActor->GetEntity()->GetName());
			}
			return;
		}
		IActorIteratorPtr pActors = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
		while (IActor* pIActor = pActors->Next())
		{
			if (!pIActor->IsPlayer())
				continue;
			const bool local = pIActor == pLocal;
			if ((who == 0 && !local) || (who == 2 && local))
				continue;
			CActor* pActor = static_cast<CActor*>(pIActor);
			if (pActor->GetHealth() <= 0 || pActor->GetLinkedVehicle() || pActor->GetSpectatorMode() != 0)
				continue;
			if (local && gEnv->pTimer->GetCurrTime() - s_lastMoveInput < 10.0f)
				continue;
			// not during a jump / fall (HALO, parachute) or a cutscene
			if (AutopilotBusy(pActor))
				continue;
			Vec3 from = pActor->GetEntity()->GetWorldPos();
			if (enemies)
			{
				// towards the nearest enemy, stopping where he can see us
				if (IEntity* pEnemy = AutopilotEnemy(pActor))
				{
					Vec3 de = pEnemy->GetWorldPos() - from;
					de.z = 0.0f;
					const float distE = de.GetLength();
					if (distE > 15.0f)
					{
						Vec3 stepE = from + de.GetNormalized() * min(6.0f, distE - 14.0f);
						stepE.z = gEnv->p3DEngine->GetTerrainElevation(stepE.x, stepE.y) + 0.5f;
						if (stepE.z < pEnemy->GetWorldPos().z - 3.0f)
							stepE.z = pEnemy->GetWorldPos().z - 1.0f;
						pRules->MovePlayer(pActor, stepE, Ang3(0, 0, atan2f(-de.x, de.y)));
						CryLogAlways("[CoopAuto] %s toward enemy %s dist=%.0f", pActor->GetEntity()->GetName(), pEnemy->GetName(), distE);
					}
					else
					{
						// face him and stay
						pRules->MovePlayer(pActor, from, Ang3(0, 0, atan2f(-de.x, de.y)));
					}
					continue;
				}
			}
			if (!pTarget)
				continue;
			Vec3 to = pTarget->GetWorldPos();
			Vec3 d = to - from;
			d.z = 0.0f;
			float dist = d.GetLength();
			if (dist < 4.0f)
			{
				// at the objective the host uses it (consoles, switches...): the
				// objective marker itself or the nearest usable thing around it
				if (!local)
					continue;
				IEntity* pUse = AutopilotUsable(pTarget, pActor->GetEntity()) ? pTarget : 0;
				if (!pUse)
				{
					SEntityProximityQuery q;
					q.box = AABB(to - Vec3(20.0f, 20.0f, 10.0f), to + Vec3(20.0f, 20.0f, 10.0f));
					gEnv->pEntitySystem->QueryProximity(q);
					float best = 1e9f;
					for (int n = 0; n < q.nCount; ++n)
					{
						IEntity* pE = q.pEntities[n];
						if (!pE || pE->IsHidden() || !AutopilotUsable(pE, pActor->GetEntity()))
							continue;
						const float dd = (pE->GetWorldPos() - to).GetLengthSquared();
						if (dd < best)
						{
							best = dd;
							pUse = pE;
						}
					}
				}
				if (!pUse)
					continue;
				Vec3 du = pUse->GetWorldPos() - from;
				du.z = 0.0f;
				if (du.GetLength() > 2.0f)
				{
					Vec3 stepU = pUse->GetWorldPos() - du.GetNormalized() * 1.5f;
					stepU.z = max(stepU.z, gEnv->p3DEngine->GetTerrainElevation(stepU.x, stepU.y)) + 0.3f;
					pRules->MovePlayer(pActor, stepU, Ang3(0, 0, atan2f(-du.x, du.y)));
					CryLogAlways("[CoopAuto] %s goes to %s to use it", pActor->GetEntity()->GetName(), pUse->GetName());
					continue;
				}
				if (gEnv->pTimer->GetCurrTime() - s_autoUseTime > 8.0f)
				{
					s_autoUseTime = gEnv->pTimer->GetCurrTime();
					CryLogAlways("[CoopAuto] %s uses %s (%s)", pActor->GetEntity()->GetName(), pUse->GetName(), pBest->GetID());
					CoopAI::Trace("AUTO %s uses %s", pActor->GetEntity()->GetName(), pUse->GetName());
					if (s_autoUsedFor != pBest->GetID())
					{
						s_autoUsed.clear();
						s_autoUsedFor = pBest->GetID();
					}
					++s_autoUsed[pUse->GetId()];
					Script::CallMethod(pUse->GetScriptTable(), "OnUsed", pActor->GetEntity()->GetScriptTable(), 1);
				}
				continue;
			}
			Vec3 step = from + d.GetNormalized() * min(6.0f, dist - 3.0f);
			float ground = gEnv->p3DEngine->GetTerrainElevation(step.x, step.y);
			step.z = max(ground, to.z - 1.0f) + 0.5f;
			if (step.z < ground + 0.5f)
				step.z = ground + 0.5f;
			Ang3 ang(0, 0, atan2f(-d.x, d.y));
			pRules->MovePlayer(pActor, step, ang);
			CryLogAlways("[CoopAuto] %s toward %s (%s) dist=%.0f", pActor->GetEntity()->GetName(), pBest->GetID(), pTarget->GetName(), dist);
		}
	}
}

void CoopAI::Update(float frameTime)
{
	UpdateTestCursor();
	UpdateTestCmdFile();
	CoopRelay::Update(frameTime);
	CoopSave::Update(frameTime);
	UpdateJoinerPositions();
	if (gEnv->bServer && gEnv->bMultiplayer)
		UpdateFakeRender();
	UpdateAutopilot(frameTime);
	if (!gEnv->bServer && s_levelReadyClient)
		UpdateClientUseTest(frameTime);
	if (gEnv->bServer)
		UpdateObjectiveResend(frameTime);
	if (gEnv->bServer && gEnv->bMultiplayer && s_levelReady)
	{
		SendAIMovement(frameTime);
		SnapshotInventories(frameTime);
	}
	LogPlayerPositions(frameTime);
	UpdateVehTest(frameTime);
	UpdatePendingPickups(frameTime);
	UpdateLoadoutCatchup(frameTime);
	UpdateCoopVision(frameTime);
	UpdateJoinerProximity(frameTime);
	UpdateAIDump(frameTime);
	UpdateAIWakeNearJoiners(frameTime);
	UpdateSequenceSync(frameTime);
	UpdateTimeOfDaySync(frameTime);
	UpdateTeamMates(frameTime);
	UpdateAIStateSync(frameTime);
	UpdateCutscenePresentation();
	UpdateUsableSync(frameTime);
	UpdatePlayerSeats(frameTime);
	if (gEnv->bServer && gEnv->bMultiplayer && s_levelReady)
		UpdatePlayerMovers();
	UpdateDebugExplosions(frameTime);
	UpdateKillWatch(frameTime);
	UpdateDebugKillGunners(frameTime);
	if (s_levelReady || s_levelReadyClient || (!IsNetGame() && !s_inLoading && g_pGame->GetIGameFramework()->IsGameStarted()))
		TraceSample(frameTime);

	if (s_clientNetDumpTimer > 0.0f)
	{
		s_clientNetDumpTimer -= frameTime;
		if (s_clientNetDumpTimer <= 0.0f)
			gEnv->pConsole->ExecuteString("net_dump_object_state");
	}

	if (!s_levelReady || !s_coopServer || !gEnv->bServer)
		return;

	if (s_equipPending && g_pGame->GetIGameFramework()->IsGameStarted())
	{
		s_equipDelay -= frameTime;
		if (s_equipDelay <= 0.0f)
		{
			s_equipPending = false;
			FlushDeferredEquip();
			if (DebugFlags() & 8)
				DumpEntities("equipped");
		}
	}

	if (s_dumpTimer > 0.0f)
	{
		s_dumpTimer -= frameTime;
		if (s_dumpTimer <= 0.0f)
		{
			DumpEntities("later");
			gEnv->pConsole->ExecuteString("net_dump_object_state");
		}
	}

	// safety net for anything registered outside the Lua wrapper
	// (players joining later, reinforcements, vehicles spawned by scripts)
	s_updateTimer += frameTime;
	if (s_updateTimer < 1.0f)
		return;
	s_updateTimer = 0.0f;

	UpdateVehicleProtection(1.0f);

	// every 10 s: AI statistics and the players' AI objects
	{
		static int s_statTick = 0;
		if (++s_statTick >= 10)
		{
			s_statTick = 0;
			LogStats("tick");
			IActorIteratorPtr pIt = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
			while (IActor* pA = pIt->Next())
			{
				if (!pA->IsPlayer())
					continue;
				IAIObject* pAI = pA->GetEntity()->GetAI();
				Vec3 e = pA->GetEntity()->GetWorldPos();
				Vec3 a = pAI ? pAI->GetPos() : Vec3(0,0,0);
				CryLogAlways("[CoopAI] player %s ai=%d enabled=%d entity=(%.0f,%.0f,%.0f) aiobj=(%.0f,%.0f,%.0f) spect=%d",
					pA->GetEntity()->GetName(), pAI ? 1 : 0, pAI ? (int)pAI->IsEnabled() : 0, e.x, e.y, e.z, a.x, a.y, a.z,
					(int)static_cast<CActor*>(pA)->GetSpectatorMode());
			}
			// every alerted AI: who it is looking at
			int listed = 0;
			IEntityItPtr pEIt = gEnv->pEntitySystem->GetEntityIterator();
			pEIt->MoveFirst();
			while (IEntity* pEntity = pEIt->Next())
			{
				IAIObject* pAI = pEntity->GetAI();
				if (!pAI || !pAI->GetProxy() || pAI->GetProxy()->GetAlertnessState() <= 0)
					continue;
				IActor* pAct = g_pGame->GetIGameFramework()->GetIActorSystem()->GetActor(pEntity->GetId());
				if (pAct && pAct->GetHealth() <= 0)
					continue;
				IAIObject* pTarget = pAI->CastToIPipeUser() ? pAI->CastToIPipeUser()->GetAttentionTarget() : 0;
				Vec3 p = pEntity->GetWorldPos();
				CryLogAlways("[CoopAIAlert] %s alert=%d target=%s pos=(%.0f,%.0f,%.0f)", pEntity->GetName(),
					pAI->GetProxy()->GetAlertnessState(), pTarget ? pTarget->GetName() : "-", p.x, p.y, p.z);
				if (++listed >= 40)
					break;
			}
			// active objectives
			if (CHUD* pHUD = g_pGame->GetHUD())
			{
				const std::vector<CHUDMissionObjective>& objs = pHUD->GetMissionObjectiveSystem().GetObjectives();
				string act;
				for (size_t i = 0; i < objs.size(); ++i)
					if (objs[i].GetStatus() == CHUDMissionObjective::ACTIVATED)
					{
						act += " ";
						act += objs[i].GetID();
					}
				CryLogAlways("[CoopObjState] active:%s", act.c_str());
			}
		}
	}

	const int registered = RegisterMissingAI();
	if (registered > 0)
		CryLogAlways("[CoopAI] late AI registration for %d entities", registered);

	if (s_pAllAspectMask && s_pAllAspectMask->GetIVal() && !s_allAspectsApplied)
	{
		s_allAspectsApplied = true;
		const uint8 mask = (uint8)s_pAllAspectMask->GetIVal();
		IGameFramework* pFramework = g_pGame->GetIGameFramework();
		IEntityItPtr pIt = gEnv->pEntitySystem->GetEntityIterator();
		pIt->MoveFirst();
		int n = 0;
		while (IEntity* pEntity = pIt->Next())
			if (IGameObject* pGO = pFramework->GetGameObject(pEntity->GetId()))
			{
				if (pGO == g_pGame->GetGameRules()->GetGameObject())
					continue;
				uint8 m = mask;
				const int keep = s_pKeepCat ? s_pKeepCat->GetIVal() : 0;
				if (keep)
				{
					const char* cls = pEntity->GetClass()->GetName();
					const bool isActor = pFramework->GetIActorSystem()->GetActor(pEntity->GetId()) != 0;
					const bool isItem = pFramework->GetIItemSystem()->IsItemClass(cls);
					const bool isVehicle = pFramework->GetIVehicleSystem()->IsVehicleClass(cls);
					const int cat = isActor ? 1 : isItem ? 2 : isVehicle ? 3 : 4;
					if (cat == keep)
						m &= ~0xC0;
				}
				pGO->EnableAspect(m, false);
				n++;
			}
		CryLogAlways("[CoopAI] debug: aspects %02x disabled on %d objects", mask, n);
	}

	// AI actors spawned after the load (see DisableAIDynamicAspects)
	DisableAIDynamicAspects();

	if (s_pAspectMask && s_pAspectMask->GetIVal())
	{
		const uint8 mask = (uint8)s_pAspectMask->GetIVal();
		IActorIteratorPtr pActors = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = pActors->Next())
			pActor->GetGameObject()->EnableAspect(mask, false);
	}
}

void CoopNetSerFail(IEntity* pEntity, int aspect, int profile, bool reading, int line)
{
	static int s_count = 0;
	if (!reading || ++s_count > 200)
		return;
	CryLogAlways("[CoopNet] NetSerialize failed: %s (%s, id %u) aspect=%d profile=%d %s line=%d",
		pEntity ? pEntity->GetName() : "?", pEntity ? pEntity->GetClass()->GetName() : "?",
		pEntity ? pEntity->GetId() : 0, aspect, profile, reading ? "reading" : "writing", line);
}

namespace
{
	struct SNetTrace { EntityId id; int aspect; int profile; };
	SNetTrace s_netTrace[32];
	int s_netTracePos = 0;
}

void CoopNetSerTrace(IEntity* pEntity, int aspect, int profile, bool reading)
{
	if (!reading || !pEntity)
		return;
	SNetTrace& t = s_netTrace[s_netTracePos++ & 31];
	t.id = pEntity->GetId();
	t.aspect = aspect;
	t.profile = profile;
}

void CoopNetSerClearTrace()
{
	memset(s_netTrace, 0, sizeof(s_netTrace));
}

void CoopNetSerDumpTrace()
{
	for (int i = 0; i < 32; ++i)
	{
		const SNetTrace& t = s_netTrace[(s_netTracePos + i) & 31];
		if (!t.id)
			continue;
		IEntity* pEntity = gEnv->pEntitySystem ? gEnv->pEntitySystem->GetEntity(t.id) : nullptr;
		CryLogAlways("[CoopNet] last read #%d: %s (%s, id %u) aspect=%d profile=%d", i,
			pEntity ? pEntity->GetName() : "?", pEntity ? pEntity->GetClass()->GetName() : "?", t.id, t.aspect, t.profile);
	}
}

namespace
{
	struct SPendingPickup { EntityId actor, item; float since; };
	std::vector<SPendingPickup> s_pendingPickups;
	float s_pickupTimer = 0.0f;

	void UpdatePendingPickups(float frameTime)
	{
		if (s_pendingPickups.empty() || !gEnv->bServer || !gEnv->bMultiplayer)
			return;
		s_pickupTimer += frameTime;
		if (s_pickupTimer < 0.25f)
			return;
		s_pickupTimer = 0.0f;
		INetContext* pNetContext = g_pGame->GetIGameFramework()->GetNetContext();
		if (!pNetContext)
			return;
		const float now = gEnv->pTimer->GetCurrTime();
		for (size_t i = 0; i < s_pendingPickups.size(); )
		{
			SPendingPickup& p = s_pendingPickups[i];
			CItem* pItem = static_cast<CItem*>(g_pGame->GetIGameFramework()->GetIItemSystem()->GetItem(p.item));
			CActor* pActor = static_cast<CActor*>(g_pGame->GetIGameFramework()->GetIActorSystem()->GetActor(p.actor));
			bool done = !pItem || !pActor || pItem->GetOwnerId() != p.actor || now - p.since > 60.0f;
			if (!done && pNetContext->IsBound(p.actor) && pNetContext->IsBound(p.item))
			{
				pActor->GetGameObject()->InvokeRMIWithDependentObject(CActor::ClPickUp(),
					CActor::PickItemParams(p.item, pItem->IsSelected(), false), eRMI_ToAllClients|eRMI_NoLocalCalls, p.item);
				CryLogAlways("[CoopNet] delayed pickup: %s now has %s (selected=%d) on the clients",
					pActor->GetEntity()->GetName(), pItem->GetEntity()->GetName(), (int)pItem->IsSelected());
				CoopAI::Trace("NET delayed pickup %s -> %s selected=%d", pItem->GetEntity()->GetName(), pActor->GetEntity()->GetName(), (int)pItem->IsSelected());
				done = true;
			}
			if (done)
			{
				s_pendingPickups[i] = s_pendingPickups.back();
				s_pendingPickups.pop_back();
			}
			else
				++i;
		}
	}
}

namespace
{
	// the other players get what the level gives the host later on (the
	// campaign hands out weapons by script, e.g. after the HALO jump): every
	// item class and ammo type the host has and they lack, once each
	std::map<EntityId, std::set<string> > s_loadoutGiven;
	float s_loadoutTimer = 0.0f;

	void ResetLoadoutCatchup()
	{
		s_loadoutGiven.clear();
	}

	void UpdateLoadoutCatchup(float frameTime)
	{
		if (!gEnv->bServer || !gEnv->bMultiplayer || !CoopAI::IsCoopSession())
			return;
		s_loadoutTimer += frameTime;
		if (s_loadoutTimer < 3.0f)
			return;
		s_loadoutTimer = 0.0f;
		IGameFramework* pFramework = g_pGame->GetIGameFramework();
		IActor* pHost = pFramework->GetClientActor();
		IInventory* pHostInv = pHost ? pHost->GetInventory() : 0;
		if (!pHostInv || pHost->GetHealth() <= 0 || static_cast<CActor*>(pHost)->GetSpectatorMode() != 0)
			return;
		IActorIteratorPtr pIt = pFramework->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = pIt->Next())
		{
			if (!pActor->IsPlayer() || pActor == pHost || pActor->GetHealth() <= 0 || static_cast<CActor*>(pActor)->GetSpectatorMode() != 0)
				continue;
			IInventory* pInv = pActor->GetInventory();
			if (!pInv)
				continue;
			std::set<string>& given = s_loadoutGiven[pActor->GetEntityId()];
			for (int i = 0; i < pHostInv->GetCount(); ++i)
			{
				IEntity* pItem = gEnv->pEntitySystem->GetEntity(pHostInv->GetItem(i));
				if (!pItem)
					continue;
				IEntityClass* pClass = pItem->GetClass();
				if (pInv->GetItemByClass(pClass) || given.count(pClass->GetName()))
					continue;
				given.insert(pClass->GetName());
				pFramework->GetIItemSystem()->GiveItem(pActor, pClass->GetName(), false, false, true);
				CryLogAlways("[CoopInv] %s gets %s like the host", pActor->GetEntity()->GetName(), pClass->GetName());
				CoopAI::Trace("LOADOUT %s gets %s", pActor->GetEntity()->GetName(), pClass->GetName());
			}
			for (int a = 0; a < pHostInv->GetAmmoTypeCount(); ++a)
			{
				IEntityClass* pAmmo = pHostInv->GetAmmoTypeByIdx(a);
				if (!pAmmo || given.count(string("ammo:") + pAmmo->GetName()))
					continue;
				const int hostCount = pHostInv->GetAmmoCount(pAmmo);
				if (hostCount > 0 && pInv->GetAmmoCount(pAmmo) == 0)
				{
					given.insert(string("ammo:") + pAmmo->GetName());
					if (pInv->GetAmmoCapacity(pAmmo) < pHostInv->GetAmmoCapacity(pAmmo))
						pInv->SetAmmoCapacity(pAmmo, pHostInv->GetAmmoCapacity(pAmmo));
					pInv->SetAmmoCount(pAmmo, hostCount);
					pActor->GetGameObject()->InvokeRMI(CActor::ClSetAmmo(), CActor::AmmoParams(pAmmo->GetName(), hostCount),
						eRMI_ToClientChannel, pActor->GetChannelId());
					CoopAI::Trace("LOADOUT %s gets %d %s", pActor->GetEntity()->GetName(), hostCount, pAmmo->GetName());
				}
			}
		}
	}
}

namespace
{
	// The AI system of the engine only really "sees" the local player: soldiers
	// notice a joined player when he shoots or makes noise, and lose him again
	// right away. The server checks what each soldier can see of the other
	// players itself (range, field of view, line of sight, hostility) and hands
	// the soldier the same visual stimulus the engine's own perception would.
	float s_visionTimer = 0.0f;
	std::map<std::pair<EntityId, EntityId>, bool> s_visionSeen;
	std::map<std::pair<EntityId, EntityId>, float> s_visionWhyLogged;
	std::map<std::pair<EntityId, EntityId>, float> s_visionAcqLogged;

	// why the last CoopCanSee said no (for the trace)
	char s_visionWhy[160] = "";

	bool CoopCanSee(IAIObject* pAI, IEntity* pAIEntity, IActor* pPlayer, const AgentParameters& params, bool alerted)
	{
		s_visionWhy[0] = 0;
		const Vec3 eye = pAI->GetPos();
		IEntity* pPlayerEntity = pPlayer->GetEntity();
		IAIObject* pPlayerAI = pPlayerEntity->GetAI();
		// aim at the head (the player's AI object sits at eye height)
		const Vec3 target = pPlayerAI ? pPlayerAI->GetPos() : pPlayerEntity->GetWorldPos() + Vec3(0, 0, 1.5f);
		Vec3 dir = target - eye;
		const float dist = dir.GetLength();
		float range = params.m_PerceptionParams.sightRange;
		if (range <= 0.0f)
			range = 60.0f;
		range *= alerted ? max(params.m_PerceptionParams.sightEnvScaleAlarmed, 0.5f) : max(params.m_PerceptionParams.sightEnvScaleNormal, 0.5f);
		range *= max(params.m_PerceptionParams.perceptionScale.visual, 0.1f);
		if (dist > range || dist < 0.01f)
		{
			_snprintf(s_visionWhy, sizeof(s_visionWhy), "range %.0f > %.0f", dist, range);
			return false;
		}
		// field of view (degrees, full cone); very close players are always
		// noticed, and a soldier already in combat keeps track of him all round
		// (the engine's perception does the same for the host: memory, turning
		// towards him) as long as nothing is in between
		if (dist > 3.0f && !(alerted && dist < 40.0f))
		{
			float fov = alerted ? params.m_PerceptionParams.FOVSecondary : params.m_PerceptionParams.FOVPrimary;
			if (fov <= 0.0f)
				fov = 160.0f;
			const Vec3 view = pAI->GetViewDir();
			if (view.GetLengthSquared() > 0.01f)
			{
				const float cosHalf = cosf(DEG2RAD(min(fov, 359.0f) * 0.5f));
				if (view.GetNormalized().Dot(dir / dist) < cosHalf)
				{
					_snprintf(s_visionWhy, sizeof(s_visionWhy), "outside fov %.0f (dot %.2f)", fov, view.GetNormalized().Dot(dir / dist));
					return false;
				}
			}
		}
		// line of sight: the level geometry, not the two bodies themselves
		IPhysicalEntity* skip[2] = { pAIEntity->GetPhysics(), pPlayerEntity->GetPhysics() };
		ray_hit hit;
		const int hits = gEnv->pPhysicalWorld->RayWorldIntersection(eye, dir, ent_static | ent_terrain | ent_sleeping_rigid | ent_rigid,
			rwi_stop_at_pierceable | rwi_colltype_any, &hit, 1, skip, 2);
		if (hits)
		{
			IEntity* pHitEntity = hit.pCollider ? gEnv->pEntitySystem->GetEntityFromPhysics(hit.pCollider) : 0;
			_snprintf(s_visionWhy, sizeof(s_visionWhy), "blocked at %.1f m by %s", hit.dist, pHitEntity ? pHitEntity->GetName() : "level geometry");
		}
		return hits == 0;
	}

	void UpdateCoopVision(float frameTime)
	{
		if (!gEnv->bServer || !gEnv->bMultiplayer || !gEnv->pAISystem || !CoopAI::IsCoopSession())
			return;
		ICVar* pVisionOn = gEnv->pConsole->GetCVar("coop_vision");
		if (pVisionOn && !pVisionOn->GetIVal())
			return;
		// what a soldier sees is confirmed every frame (the engine drops a
		// target it does not see itself between two of our checks)
		ICVar* pEveryFrame = gEnv->pConsole->GetCVar("coop_vision_frame");
		if (!pEveryFrame || pEveryFrame->GetIVal())
		{
			for (std::map<std::pair<EntityId, EntityId>, bool>::iterator it = s_visionSeen.begin(); it != s_visionSeen.end(); ++it)
			{
				if (!it->second)
					continue;
				IEntity* pSoldier = gEnv->pEntitySystem->GetEntity(it->first.first);
				IEntity* pSeen = gEnv->pEntitySystem->GetEntity(it->first.second);
				IAIObject* pAI = pSoldier ? pSoldier->GetAI() : 0;
				IAIObject* pSeenAI = pSeen ? pSeen->GetAI() : 0;
				if (!pAI || !pSeenAI || !pAI->IsEnabled())
					continue;
				SAIEVENT event;
				event.pSeen = pSeenAI;
				event.vPosition = pSeenAI->GetPos();
				event.fThreat = 1.0f;
				pAI->Event(AIEVENT_ONVISUALSTIMULUS, &event);
			}
		}
		s_visionTimer += frameTime;
		if (s_visionTimer < 0.2f)
			return;
		s_visionTimer = 0.0f;
		IGameFramework* pFramework = g_pGame->GetIGameFramework();
		IActor* pLocal = pFramework->GetClientActor();
		std::vector<IActor*> players;
		IActorIteratorPtr pIt = pFramework->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = pIt->Next())
		{
			if (!pActor->IsPlayer() || pActor == pLocal || pActor->GetHealth() <= 0)
				continue;
			CActor* pA = static_cast<CActor*>(pActor);
			if (pA->GetSpectatorMode() != 0 || !pActor->GetEntity()->GetAI() || pActor->GetEntity()->IsHidden())
				continue;
			if (pA->GetActorClass() == CPlayer::GetActorClassType() && static_cast<CPlayer*>(pA)->IsCloaked())
				continue;
			players.push_back(pActor);
		}
		if (players.empty())
			return;
		IActorIteratorPtr pAIIt = pFramework->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = pAIIt->Next())
		{
			if (pActor->IsPlayer() || pActor->GetHealth() <= 0)
				continue;
			IEntity* pEntity = pActor->GetEntity();
			IAIObject* pAI = pEntity->GetAI();
			if (!pAI || !pAI->IsEnabled() || pEntity->IsHidden() || !pAI->CastToIPuppet())
				continue;
			IAIActor* pAIActor = pAI->CastToIAIActor();
			if (!pAIActor)
				continue;
			const AgentParameters& params = pAIActor->GetParameters();
			const bool alerted = pAI->GetProxy() && pAI->GetProxy()->GetAlertnessState() > 0;
			for (size_t i = 0; i < players.size(); ++i)
			{
				IActor* pPlayer = players[i];
				IAIObject* pPlayerAI = pPlayer->GetEntity()->GetAI();
				if ((pPlayer->GetEntity()->GetWorldPos() - pEntity->GetWorldPos()).GetLengthSquared() > 200.0f * 200.0f)
					continue;
				if (!pAI->IsHostile(pPlayerAI))
				{
					if ((pPlayer->GetEntity()->GetWorldPos() - pEntity->GetWorldPos()).GetLengthSquared() < 30.0f * 30.0f)
					{
						float& last = s_visionWhyLogged[std::make_pair(pEntity->GetId(), pPlayer->GetEntityId())];
						if (gEnv->pTimer->GetCurrTime() - last > 5.0f)
						{
							last = gEnv->pTimer->GetCurrTime();
							CoopAI::Trace("VISION %s ignores %s: not hostile", pEntity->GetName(), pPlayer->GetEntity()->GetName());
						}
					}
					continue;
				}
				const bool sees = CoopCanSee(pAI, pEntity, pPlayer, params, alerted);
				if ((pPlayer->GetEntity()->GetWorldPos() - pEntity->GetWorldPos()).GetLengthSquared() < 30.0f * 30.0f)
				{
					float& lastAcq = s_visionAcqLogged[std::make_pair(pEntity->GetId(), pPlayer->GetEntityId())];
					if (gEnv->pTimer->GetCurrTime() - lastAcq > 5.0f)
					{
						lastAcq = gEnv->pTimer->GetCurrTime();
						IActor* pHostActor = g_pGame->GetIGameFramework()->GetClientActor();
						IAIObject* pHostAI = pHostActor ? pHostActor->GetEntity()->GetAI() : 0;
						CoopAI::Trace("VISION %s canAcquire %s=%d host=%d sees=%d target=%s", pEntity->GetName(), pPlayer->GetEntity()->GetName(),
							(int)pAIActor->CanAcquireTarget(pPlayerAI), pHostAI ? (int)pAIActor->CanAcquireTarget(pHostAI) : -1, (int)sees,
							pAI->CastToIPipeUser() && pAI->CastToIPipeUser()->GetAttentionTarget() ? pAI->CastToIPipeUser()->GetAttentionTarget()->GetName() : "-");
					}
				}
				if (!sees && (pPlayer->GetEntity()->GetWorldPos() - pEntity->GetWorldPos()).GetLengthSquared() < 30.0f * 30.0f)
				{
					float& last = s_visionWhyLogged[std::make_pair(pEntity->GetId(), pPlayer->GetEntityId())];
					if (gEnv->pTimer->GetCurrTime() - last > 5.0f)
					{
						last = gEnv->pTimer->GetCurrTime();
						CoopAI::Trace("VISION %s cannot see %s: %s", pEntity->GetName(), pPlayer->GetEntity()->GetName(), s_visionWhy);
					}
				}
				bool& was = s_visionSeen[std::make_pair(pEntity->GetId(), pPlayer->GetEntityId())];
				if (sees != was)
				{
					CoopAI::Trace("VISION %s %s %s (alerted=%d)", pEntity->GetName(), sees ? "sees" : "lost", pPlayer->GetEntity()->GetName(), (int)alerted);
					was = sees;
				}
				if (!sees)
					continue;
				SAIEVENT event;
				event.pSeen = pPlayerAI;
				event.vPosition = pPlayerAI->GetPos();
				event.bFuzzySight = false;
				event.fThreat = 1.0f;
				pAI->Event(AIEVENT_ONVISUALSTIMULUS, &event);
			}
		}
	}
}

namespace
{
	// The engine runs a soldier's AI and update only while it is seen by or
	// close to the local camera, i.e. the host's: soldiers around a joined
	// player far from the host stood frozen until the host came. Only the
	// soldiers within this distance of a joined player are kept awake.
	const float kWakeDistance = 100.0f;
	std::set<EntityId> s_awakeAI;
	std::map<EntityId, int> s_awakeMode;
	float s_shotLogTimer = 0.0f;
	float s_wakeTimer = 0.0f;
	bool s_updateAllOn = false;
	int s_updateAllSaved = 0;

	void ResetAIWake()
	{
		ClearAlwaysAnimated();
		s_awakeAI.clear();
		s_awakeMode.clear();
		if (s_updateAllOn)
			if (ICVar* pAll = gEnv->pConsole->GetCVar("ai_UpdateAllAlways"))
				pAll->ForceSet(s_updateAllSaved ? "1" : "0");
		s_updateAllOn = false;
	}

	float s_aiDumpTimer = 0.0f;

	// The engine animates a character's skeleton only while it is rendered,
	// except the local player's (CActor sets CS_FLAG_UPDATE_ALWAYS for him).
	// On the server that is the host's view: soldiers near a joined player
	// could not raise their weapons to aim and fire, and the joined player's
	// own body (head, bones) stood still for their eyes and bullets.
	std::set<EntityId> s_alwaysAnimated;

	void ClearAlwaysAnimated()
	{
		s_alwaysAnimated.clear();
	}

	bool SetAlwaysAnimated(IEntity* pEntity, bool on)
	{
		ICharacterInstance* pCharacter = pEntity ? pEntity->GetCharacter(0) : 0;
		if (!pCharacter)
			return false;
		const int flags = pCharacter->GetFlags();
		if (on && !(flags & CS_FLAG_UPDATE_ALWAYS))
		{
			pCharacter->SetFlags(flags | CS_FLAG_UPDATE_ALWAYS);
			s_alwaysAnimated.insert(pEntity->GetId());
			return true;
		}
		if (!on && s_alwaysAnimated.count(pEntity->GetId()))
		{
			pCharacter->SetFlags(flags & ~CS_FLAG_UPDATE_ALWAYS);
			s_alwaysAnimated.erase(pEntity->GetId());
			return true;
		}
		return false;
	}

	// coop_fake_render: soldiers near a joined player are told they were
	// rendered this frame (the host's camera does not see them)
	void UpdateFakeRender()
	{
		ICVar* pOn = gEnv->pConsole->GetCVar("coop_fake_render");
		if (!pOn || !pOn->GetIVal() || s_alwaysAnimated.empty())
			return;
		static SRendParams s_params;
		for (std::set<EntityId>::iterator it = s_alwaysAnimated.begin(); it != s_alwaysAnimated.end(); ++it)
		{
			IEntity* pEntity = gEnv->pEntitySystem->GetEntity(*it);
			if (!pEntity || pEntity->IsHidden())
				continue;
			SEntityEvent event(ENTITY_EVENT_RENDER);
			event.nParam[0] = (INT_PTR)&s_params;
			pEntity->SendEvent(event);
		}
	}

	void UpdateAIDump(float frameTime)
	{
		ICVar* pDump = gEnv->pConsole->GetCVar("coop_debug_aidump");
		if (!pDump || pDump->GetIVal() <= 0 || !gEnv->bServer || !gEnv->pAISystem)
			return;
		s_aiDumpTimer += frameTime;
		if (s_aiDumpTimer < (float)pDump->GetIVal())
			return;
		s_aiDumpTimer = 0.0f;
		IGameFramework* pFramework = g_pGame->GetIGameFramework();
		IActor* pLocal = pFramework->GetClientActor();
		IActor* pJoiner = 0;
		IActorIteratorPtr pIt = pFramework->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = pIt->Next())
		{
			if (!pActor->IsPlayer())
				continue;
			if (pActor != pLocal)
				pJoiner = pActor;
			if (IAIObject* pAI = pActor->GetEntity()->GetAI())
			{
				CryLogAlways("[CoopAIDump] ===== player %s (type %d enabled %d)", pActor->GetEntity()->GetName(), (int)pAI->GetAIType(), (int)pAI->IsEnabled());
				gEnv->pAISystem->DumpStateOf(pAI);
			}
		}
		if (!pJoiner)
			return;
		// the soldier nearest to the joined player
		IActor* pNearest = 0;
		float best = 1e9f;
		IActorIteratorPtr pAIIt = pFramework->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = pAIIt->Next())
		{
			if (pActor->IsPlayer() || pActor->GetHealth() <= 0 || !pActor->GetEntity()->GetAI() || pActor->GetEntity()->IsHidden())
				continue;
			const float d = (pActor->GetEntity()->GetWorldPos() - pJoiner->GetEntity()->GetWorldPos()).GetLength();
			if (d < best)
			{
				best = d;
				pNearest = pActor;
			}
		}
		if (pNearest)
		{
			IAIObject* pAI = pNearest->GetEntity()->GetAI();
			CryLogAlways("[CoopAIDump] ===== soldier %s %.0f m from %s (enabled %d, hostile to joiner %d, to host %d)", pNearest->GetEntity()->GetName(), best,
				pJoiner->GetEntity()->GetName(), (int)pAI->IsEnabled(),
				pJoiner->GetEntity()->GetAI() ? (int)pAI->IsHostile(pJoiner->GetEntity()->GetAI()) : -1,
				pLocal && pLocal->GetEntity()->GetAI() ? (int)pAI->IsHostile(pLocal->GetEntity()->GetAI()) : -1);
			gEnv->pAISystem->DumpStateOf(pAI);
		}
	}

	void UpdateAIWakeNearJoiners(float frameTime)
	{
		if (!gEnv->bServer || !gEnv->bMultiplayer || !CoopAI::IsCoopSession())
			return;
		s_wakeTimer += frameTime;
		if (s_wakeTimer < 1.0f)
			return;
		s_wakeTimer = 0.0f;
		IGameFramework* pFramework = g_pGame->GetIGameFramework();
		IActor* pLocal = pFramework->GetClientActor();
		std::vector<Vec3> joiners;
		IActorIteratorPtr pIt = pFramework->GetIActorSystem()->CreateActorIterator();
		ICVar* pAnimCVar = gEnv->pConsole->GetCVar("coop_animate_near_joiners");
		const bool animate = !pAnimCVar || pAnimCVar->GetIVal() != 0;
		while (IActor* pActor = pIt->Next())
		{
			if (pActor->IsPlayer() && pActor != pLocal && pActor->GetHealth() > 0 && static_cast<CActor*>(pActor)->GetSpectatorMode() == 0)
			{
				joiners.push_back(pActor->GetEntity()->GetWorldPos());
				if (animate && SetAlwaysAnimated(pActor->GetEntity(), true))
					CoopAI::Trace("ANIM %s is animated on the server even when the host does not see him", pActor->GetEntity()->GetName());
			}
		}
		// soldiers near a joined player are switched on the way the engine does
		// it near the host's camera (coop_debug_wake picks how), and back when
		// no joined player is within 150 m
		ICVar* pWakeMode = gEnv->pConsole->GetCVar("coop_debug_wake");
		const int mode = pWakeMode ? pWakeMode->GetIVal() : 1;
		int woken = 0, slept = 0, deadOff = 0;
		IActorIteratorPtr pAIIt = pFramework->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = pAIIt->Next())
		{
			if (pActor->IsPlayer())
				continue;
			if (pActor->GetHealth() <= 0)
			{
				// a dead soldier's AI object stays enabled; with every soldier
				// updated (a joined player away from the host) the AI system
				// then updates the dead ones each frame ("CPuppet::Update
				// Trying to update dead character", hundreds of thousands of
				// warnings): switched off, as nothing is left for it to do
				IAIObject* pDeadAI = pActor->GetEntity()->GetAI();
				if (pDeadAI && pDeadAI->IsEnabled())
				{
					pDeadAI->Event(AIEVENT_DISABLE, 0);
					++deadOff;
				}
				s_awakeMode.erase(pActor->GetEntityId());
				s_awakeAI.erase(pActor->GetEntityId());
				continue;
			}
			IEntity* pEntity = pActor->GetEntity();
			IAIObject* pAI = pEntity->GetAI();
			if (!pAI || pEntity->IsHidden())
				continue;
			float nearest = 1e9f;
			for (size_t i = 0; i < joiners.size(); ++i)
				nearest = min(nearest, (pEntity->GetWorldPos() - joiners[i]).GetLength());
			if (animate)
			{
				if (nearest < kWakeDistance)
					SetAlwaysAnimated(pEntity, true);
				else if (nearest > kWakeDistance * 1.5f)
					SetAlwaysAnimated(pEntity, false);
			}
			std::map<EntityId, int>::iterator ours = s_awakeMode.find(pEntity->GetId());
			IGameObject* pGameObject = pActor->GetGameObject();
			if (nearest < kWakeDistance && ours == s_awakeMode.end() && mode > 0)
			{
				// coop_debug_wake: 1 AI enable event, 2 + game object AI
				// activation, 3 + AI wake-up event, 4 activation + forced
				// update, 5 forced update only
				int applied = mode;
				if (!pAI->IsEnabled())
				{
					pAI->Event(AIEVENT_ENABLE, 0);
					applied |= 0x100;
				}
				if (mode == 2 || mode == 4)
					pGameObject->SetAIActivation(eGOAIAM_Always);
				if (mode == 3)
					pAI->Event(AIEVENT_WAKEUP, 0);
				if (mode == 4 || mode == 5)
					pGameObject->ForceUpdate(true);
				s_awakeMode[pEntity->GetId()] = applied;
				s_awakeAI.insert(pEntity->GetId());
				++woken;
			}
			else if (ours != s_awakeMode.end() && nearest > kWakeDistance * 1.5f)
			{
				const int applied = ours->second;
				const int m = applied & 0xff;
				if (m == 2 || m == 4)
					pGameObject->SetAIActivation(eGOAIAM_VisibleOrInRange);
				if (m == 4 || m == 5)
					pGameObject->ForceUpdate(false);
				if ((applied & 0x100) && pAI->IsEnabled())
					pAI->Event(AIEVENT_DISABLE, 0);
				s_awakeMode.erase(ours);
				s_awakeAI.erase(pEntity->GetId());
				++slept;
			}
		}
		if (woken || slept)
			CoopAI::Trace("AIWAKE mode %d: %d woken near joined players, %d back, %d held, %d always animated", mode, woken, slept, (int)s_awakeAI.size(), (int)s_alwaysAnimated.size());
		if (deadOff)
			CoopAI::Trace("AIWAKE %d dead soldiers' AI switched off", deadOff);
		s_shotLogTimer += 1.0f;
		if (s_shotLogTimer >= 10.0f)
		{
			s_shotLogTimer = 0.0f;
			CryLogAlways("[CoopAI] AI shots so far: %d (wake mode %d)", s_totalAIShots, mode);
		}

		// the AI system fully updates (perception, targets, firing) only the
		// enabled soldiers near the local camera; ai_UpdateAllAlways extends
		// that to every *enabled* soldier (a handful: the rest stay asleep).
		// Only while a joined player is away from the host.
		float farthest = 0.0f;
		if (pLocal)
			for (size_t i = 0; i < joiners.size(); ++i)
				farthest = max(farthest, (joiners[i] - pLocal->GetEntity()->GetWorldPos()).GetLength());
		const bool wantAll = farthest > 50.0f;
		if (ICVar* pAll = gEnv->pConsole->GetCVar("ai_UpdateAllAlways"))
		{
			if (wantAll && !s_updateAllOn)
			{
				s_updateAllSaved = pAll->GetIVal();
				pAll->ForceSet("1");
				s_updateAllOn = true;
				CoopAI::Trace("AIWAKE ai_UpdateAllAlways 1 (joined player %.0f m from the host)", farthest);
				CryLogAlways("[CoopAI] joined player %.0f m from the host: enabled soldiers are updated everywhere", farthest);
			}
			else if (!wantAll && s_updateAllOn)
			{
				pAll->ForceSet(s_updateAllSaved ? "1" : "0");
				s_updateAllOn = false;
				CoopAI::Trace("AIWAKE ai_UpdateAllAlways %d (players together)", s_updateAllSaved);
			}
		}
		// the same for physics: the entity system stops simulating what the
		// local camera does not see and is far from it. Soldiers dropped by a
		// helicopter next to a joined player then hung in the air for good
		// (a living entity walks, but never falls, while "forgotten"), and far
		// ragdolls froze in mid-air.
		if (ICVar* pPhysChecks = gEnv->pConsole->GetCVar("es_UsePhysVisibilityChecks"))
		{
			static bool s_physAllOn = false;
			static int s_physChecksSaved = 1;
			if (wantAll && !s_physAllOn)
			{
				s_physChecksSaved = pPhysChecks->GetIVal();
				pPhysChecks->ForceSet("0");
				s_physAllOn = true;
				CoopAI::Trace("AIWAKE es_UsePhysVisibilityChecks 0 (joined player %.0f m from the host)", farthest);
			}
			else if (!wantAll && s_physAllOn)
			{
				pPhysChecks->ForceSet(s_physChecksSaved ? "1" : "0");
				s_physAllOn = false;
				CoopAI::Trace("AIWAKE es_UsePhysVisibilityChecks %d (players together)", s_physChecksSaved);
			}
		}
	}
}

namespace
{
	// CryAction keeps a "PlayerProximityTrigger" box around the local player:
	// everything inside counts as "in range" and gets fully updated (AI,
	// game object updates, anchors, reinforcement spots...), the way the
	// campaign expects around the player. On the server that is only the host.
	// The same box is kept around every joined player: entities inside it
	// (and not already inside the host's) are reported to the host's trigger
	// as having entered it, and as having left when they are out of both.
	std::set<EntityId> s_proximityForwarded;
	EntityId s_hostProximityTrigger = 0;
	float s_proximityTimer = 0.0f;

	void ResetJoinerProximity()
	{
		s_proximityForwarded.clear();
		s_hostProximityTrigger = 0;
	}

	void SendProximityEvent(IEntity* pTrigger, EntityId entered, bool enter)
	{
		SEntityEvent event(enter ? ENTITY_EVENT_ENTERAREA : ENTITY_EVENT_LEAVEAREA);
		event.nParam[0] = entered;
		event.nParam[1] = 0;
		event.nParam[2] = pTrigger->GetId();
		pTrigger->SendEvent(event);
	}

	void UpdateJoinerProximity(float frameTime)
	{
		if (!gEnv->bServer || !gEnv->bMultiplayer || !CoopAI::IsCoopSession())
			return;
		ICVar* pOn = gEnv->pConsole->GetCVar("coop_joiner_proximity");
		if (pOn && !pOn->GetIVal())
			return;
		s_proximityTimer += frameTime;
		if (s_proximityTimer < 0.5f)
			return;
		s_proximityTimer = 0.0f;

		static int s_why = -1;
		IEntity* pTrigger = s_hostProximityTrigger ? gEnv->pEntitySystem->GetEntity(s_hostProximityTrigger) : 0;
		if (!pTrigger)
		{
			pTrigger = gEnv->pEntitySystem->FindEntityByName("PlayerProximityTrigger");
			s_hostProximityTrigger = pTrigger ? pTrigger->GetId() : 0;
			if (!pTrigger)
			{
				if (s_why != 1)
					CoopAI::Trace("PROXIMITY no PlayerProximityTrigger entity");
				s_why = 1;
				return;
			}
		}
		IEntityTriggerProxy* pTriggerProxy = static_cast<IEntityTriggerProxy*>(pTrigger->GetProxy(ENTITY_PROXY_TRIGGER));
		if (!pTriggerProxy)
		{
			if (s_why != 2)
				CoopAI::Trace("PROXIMITY %s has no trigger proxy", pTrigger->GetName());
			s_why = 2;
			return;
		}
		AABB local;
		pTriggerProxy->GetTriggerBounds(local);
		if (s_why != 0)
			CoopAI::Trace("PROXIMITY host trigger %u at (%.0f,%.0f,%.0f) bounds (%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f)", pTrigger->GetId(),
				pTrigger->GetWorldPos().x, pTrigger->GetWorldPos().y, pTrigger->GetWorldPos().z,
				local.min.x, local.min.y, local.min.z, local.max.x, local.max.y, local.max.z);
		s_why = 0;
		// the bounds are in world space, around the host
		if (local.IsEmpty() || local.GetSize().GetLength() < 1.0f)
			return;
		const AABB hostBox = local;
		const Vec3 half = local.GetSize() * 0.5f;

		IGameFramework* pFramework = g_pGame->GetIGameFramework();
		IActor* pLocal = pFramework->GetClientActor();
		std::set<EntityId> inside;
		IActorIteratorPtr pIt = pFramework->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = pIt->Next())
		{
			if (!pActor->IsPlayer() || pActor == pLocal || pActor->GetHealth() <= 0 || static_cast<CActor*>(pActor)->GetSpectatorMode() != 0)
				continue;
			const Vec3 pos = pActor->GetEntity()->GetWorldPos();
			SEntityProximityQuery query;
			query.box = AABB(pos - half, pos + half);
			gEnv->pEntitySystem->QueryProximity(query);
			for (int i = 0; i < query.nCount; ++i)
				if (IEntity* pEntity = query.pEntities[i])
					if (pEntity != pTrigger && pEntity->GetId() != pActor->GetEntityId())
						inside.insert(pEntity->GetId());
		}

		int entered = 0, left = 0;
		for (std::set<EntityId>::iterator it = inside.begin(); it != inside.end(); ++it)
		{
			if (s_proximityForwarded.count(*it))
				continue;
			IEntity* pEntity = gEnv->pEntitySystem->GetEntity(*it);
			if (!pEntity || hostBox.IsContainPoint(pEntity->GetWorldPos()))
				continue;
			SendProximityEvent(pTrigger, *it, true);
			s_proximityForwarded.insert(*it);
			++entered;
		}
		for (std::set<EntityId>::iterator it = s_proximityForwarded.begin(); it != s_proximityForwarded.end(); )
		{
			IEntity* pEntity = gEnv->pEntitySystem->GetEntity(*it);
			if (!pEntity)
			{
				s_proximityForwarded.erase(it++);
				continue;
			}
			const bool nearHost = hostBox.IsContainPoint(pEntity->GetWorldPos());
			if (nearHost)
			{
				// the host's own trigger has it now
				s_proximityForwarded.erase(it++);
				continue;
			}
			if (!inside.count(*it))
			{
				SendProximityEvent(pTrigger, *it, false);
				s_proximityForwarded.erase(it++);
				++left;
				continue;
			}
			++it;
		}
		if (entered || left)
			CoopAI::Trace("PROXIMITY %d entered, %d left the joined players' range (%d held, box %.0fx%.0fx%.0f)", entered, left,
				(int)s_proximityForwarded.size(), local.GetSize().x, local.GetSize().y, local.GetSize().z);
	}
}

void CoopAI::QueuePickup(EntityId actorId, EntityId itemId)
{
	if (!gEnv->bServer || !actorId || !itemId)
		return;
	for (size_t i = 0; i < s_pendingPickups.size(); ++i)
		if (s_pendingPickups[i].item == itemId && s_pendingPickups[i].actor == actorId)
			return;
	SPendingPickup p = { actorId, itemId, gEnv->pTimer->GetCurrTime() };
	s_pendingPickups.push_back(p);
}

bool CoopAI::IsNetBound(EntityId id, EntityId id2)
{
	if (!gEnv->bServer || !gEnv->bMultiplayer || !g_pGame)
		return true;
	INetContext* pNetContext = g_pGame->GetIGameFramework()->GetNetContext();
	if (!pNetContext)
		return true;
	bool ok = pNetContext->IsBound(id) && (!id2 || pNetContext->IsBound(id2));
	if (!ok)
		LogUnbound("IsNetBound", id, id2);
	return ok;
}

void CoopAI::LogUnbound(const char* what, EntityId a, EntityId b)
{
	static int s_n = 0;
	if (++s_n > 40)
		return;
	IEntity* pA = gEnv->pEntitySystem->GetEntity(a);
	IEntity* pB = b ? gEnv->pEntitySystem->GetEntity(b) : 0;
	CryLogAlways("[CoopNet] %s: not bound: %s (%s %u, flags %08x) / %s (%s %u, flags %08x)", what,
		pA ? pA->GetName() : "-", pA ? pA->GetClass()->GetName() : "-", a, pA ? pA->GetFlags() : 0,
		pB ? pB->GetName() : "-", pB ? pB->GetClass()->GetName() : "-", b, pB ? pB->GetFlags() : 0);
}

//------------------------------------------------------------------------
// Flow mirror: presentation flow nodes fired by the level's scripts on the
// server are repeated on every client (HUD, screen FX, dialogs, cutscenes,
// music...). Game logic nodes stay server-only.
//------------------------------------------------------------------------
namespace
{
	const char FLOW_SEP = '\x1f';

	bool IsMirroredType(const char* type)
	{
		if (!type || !s_pFlowMirror)
			return false;
		const char* list = s_pFlowMirror->GetString();
		if (!list || !list[0])
			return false;
		string all(list);
		size_t start = 0;
		while (start <= all.size())
		{
			size_t end = all.find(';', start);
			if (end == string::npos)
				end = all.size();
			string prefix = all.substr(start, end - start);
			// "HUD:" is a group, "Entity:Material" that one type (not
			// Entity:MaterialLayer: it crashes the engine on a client's copy
			// of a cutscene prop)
			const bool group = !prefix.empty() && prefix[prefix.size() - 1] == ':';
			if (!prefix.empty() && (group ? strnicmp(type, prefix.c_str(), prefix.size()) == 0 : stricmp(type, prefix.c_str()) == 0))
				return true;
			start = end + 1;
		}
		return false;
	}

	bool IsReplayedType(const char* type)
	{
		// state that a late joiner must see (not one-shot events): screen
		// state, and what the story did to objects' looks (a monitor's video,
		// swapped materials)
		return !strnicmp(type, "HUD:", 4) || !strnicmp(type, "Image:", 6) || !strnicmp(type, "Environment:", 12)
			|| !stricmp(type, "Entity:Material") || !stricmp(type, "Entity:MaterialParam");
	}

	// Cutscenes place the player with flow nodes (Entity:BeamEntity,
	// Entity:EntityPos, Movement:MoveEntityTo / RotateEntityTo). On the
	// server that moves a joined player's entity only: his position belongs
	// to his own machine, so he stayed where he started the cutscene. When
	// such a node is triggered for a joined player, the result is sent to his
	// machine once (in the same frame, before his own updates come back).
	enum EPlayerMover { ePM_Placed, ePM_MoveTo, ePM_RotateTo };
	struct SPlayerMover { IFlowGraphPtr pGraph; TFlowNodeId node; int kind; };
	std::vector<SPlayerMover> s_playerMovers;

	void WatchPlayerMover(IFlowGraph* pGraph, TFlowNodeId node, const char* type, TFlowPortId port)
	{
		if (stricmp(type, "Entity:BeamEntity") && stricmp(type, "Entity:EntityPos")
			&& stricmp(type, "Movement:MoveEntityTo") && stricmp(type, "Movement:RotateEntityTo"))
			return;
		SFlowNodeConfig cfg;
		pGraph->GetNodeConfiguration(node, cfg);
		int count = 0;
		while (cfg.pInputPorts && cfg.pInputPorts[count].name)
			++count;
		if ((int)port >= count)
			return;
		const char* name = cfg.pInputPorts[port].name;
		int kind = -1;
		if (!stricmp(type, "Entity:BeamEntity") && !stricmp(name, "Beam"))
			kind = ePM_Placed;
		else if (!stricmp(type, "Entity:EntityPos") && (!stricmp(name, "pos") || !stricmp(name, "rotate")))
			kind = ePM_Placed;
		else if (!stricmp(type, "Movement:MoveEntityTo") && !stricmp(name, "Start"))
			kind = ePM_MoveTo;
		else if (!stricmp(type, "Movement:RotateEntityTo") && !stricmp(name, "Start"))
			kind = ePM_RotateTo;
		if (kind < 0)
			return;
		SPlayerMover m;
		m.pGraph = pGraph;
		m.node = node;
		m.kind = kind;
		s_playerMovers.push_back(m);
	}

	// a joined player (not the host), alive in the world
	CActor* JoinedPlayer(EntityId id)
	{
		IActor* pActor = id ? g_pGame->GetIGameFramework()->GetIActorSystem()->GetActor(id) : 0;
		if (!pActor || !pActor->IsPlayer() || pActor == g_pGame->GetIGameFramework()->GetClientActor())
			return 0;
		CActor* pA = static_cast<CActor*>(pActor);
		return (pA->GetChannelId() && pA->GetSpectatorMode() == 0) ? pA : 0;
	}

	bool GetInputVec3(IFlowGraph* pGraph, TFlowNodeId node, const char* portName, Vec3& value)
	{
		SFlowNodeConfig cfg;
		pGraph->GetNodeConfiguration(node, cfg);
		for (int i = 0; cfg.pInputPorts && cfg.pInputPorts[i].name; ++i)
			if (!stricmp(cfg.pInputPorts[i].name, portName))
			{
				const TFlowInputData* pData = pGraph->GetInputValue(node, i);
				return pData && pData->GetValueWithConversion(value);
			}
		return false;
	}

	void UpdatePlayerMovers()
	{
		CGameRules* pRules = g_pGame->GetGameRules();
		std::vector<SPlayerMover> movers;
		movers.swap(s_playerMovers);
		for (size_t i = 0; pRules && i < movers.size(); ++i)
		{
			const SPlayerMover& m = movers[i];
			CActor* pActor = JoinedPlayer(m.pGraph->GetEntityId(m.node));
			if (!pActor)
				continue;
			IEntity* pEntity = pActor->GetEntity();
			Vec3 pos = pEntity->GetWorldPos();
			Ang3 ang(pEntity->GetWorldRotation());
			Vec3 dest;
			if (m.kind == ePM_MoveTo && GetInputVec3(m.pGraph, m.node, "Destination", dest))
				pos = dest;
			else if (m.kind == ePM_RotateTo && GetInputVec3(m.pGraph, m.node, "Destination", dest))
				ang = Ang3(DEG2RAD(dest.x), DEG2RAD(dest.y), DEG2RAD(dest.z));
			pRules->MovePlayer(pActor, pos, ang);
			IEntity* pGraphEntity = gEnv->pEntitySystem->GetEntity(m.pGraph->GetGraphEntity(0));
			CoopAI::Trace("PLAYERMOVE %s -> (%.2f,%.2f,%.2f) yaw %.0f by %s %s", pEntity->GetName(), pos.x, pos.y, pos.z,
				RAD2DEG(ang.z), pGraphEntity ? pGraphEntity->GetName() : "<no entity>", m.pGraph->GetNodeTypeName(m.node));
		}
	}

	struct SFlowMsg
	{
		string type; uint32 key; uint32 node; uint32 port; uint32 entity; string values;
	};
	std::vector<SFlowMsg> s_flowHistory;

	class CCoopFlowInspector : public IFlowGraphInspector
	{
	public:
		CCoopFlowInspector() : m_ref(1) {}
		virtual void AddRef() { ++m_ref; }
		virtual void Release() { if (--m_ref <= 0) m_ref = 1; } // static instance
		virtual void PreUpdate(IFlowGraph*) {}
		virtual void PostUpdate(IFlowGraph*)
		{
			if (m_pending.empty())
				return;
			std::vector<SPending> pending;
			pending.swap(m_pending);
			CGameRules* pRules = g_pGame ? g_pGame->GetGameRules() : 0;
			for (size_t i = 0; i < pending.size(); ++i)
			{
				SPending& p = pending[i];
				if (p.mirror && !pRules)
					continue;
				SFlowMsg msg;
				msg.type = p.type;
				msg.key = p.key;
				msg.node = p.node;
				msg.port = p.port;
				msg.entity = p.pGraph->GetEntityId(p.node);
				// "the graph's entity" placeholders mean nothing on a client
				if (msg.entity == (EntityId)EFLOWNODE_ENTITY_ID_GRAPH1)
					msg.entity = p.pGraph->GetGraphEntity(0);
				else if (msg.entity == (EntityId)EFLOWNODE_ENTITY_ID_GRAPH2)
					msg.entity = p.pGraph->GetGraphEntity(1);
				SFlowNodeConfig cfg;
				p.pGraph->GetNodeConfiguration(p.node, cfg);
				for (int n = 0; cfg.pInputPorts && cfg.pInputPorts[n].name; ++n)
				{
					string v;
					if (const TFlowInputData* pData = p.pGraph->GetInputValue(p.node, n))
						pData->GetValueWithConversion(v);
					if (n)
						msg.values += FLOW_SEP;
					msg.values += v;
				}
				if (!p.mirror)
				{
					// traced only (dialogs in single player / not mirrored)
					IEntity* pE = msg.entity ? gEnv->pEntitySystem->GetEntity(msg.entity) : 0;
					CoopAI::Trace("FLOW %s node=%u port=%u entity=%s values=%s", msg.type.c_str(), msg.node, msg.port, pE ? pE->GetName() : "-", msg.values.c_str());
					continue;
				}
				pRules->CoopSendFlow(msg.type.c_str(), msg.key, msg.node, msg.port, msg.entity, msg.values.c_str(), 0);
				CoopAI::Trace("FLOW> %s node=%u port=%u entity=%u values=%s", msg.type.c_str(), msg.node, msg.port, msg.entity, msg.values.c_str());
				if (IsReplayedType(msg.type.c_str()) && s_flowHistory.size() < 3000)
					s_flowHistory.push_back(msg);
			}
		}
		virtual void NotifyFlow(IFlowGraph* pGraph, const SFlowAddress from, const SFlowAddress to)
		{
			if (!pGraph || to.isOutput)
				return;
			const char* type = pGraph->GetNodeTypeName(to.node);
			if (!type)
				return;
			if (gEnv->bServer && gEnv->bMultiplayer && CoopAI::IsCoopSession())
				WatchPlayerMover(pGraph, to.node, type, to.port);
			const bool mirror = gEnv->bServer && CoopAI::IsCoopSession() && IsMirroredType(type) && stricmp(type, "HUD:ProgressBar");
			// dialogs (and in single player every presentation node) go to the trace
			const bool trace = CoopAI::TraceOn() && (!strnicmp(type, "Dialog:", 7) || (!mirror && IsMirroredType(type)));
			if (!mirror && !trace)
				return;
			SPending p;
			p.mirror = mirror;
			p.pGraph = pGraph;
			p.key = (uint32)(UINT_PTR)pGraph;
			p.node = to.node;
			p.port = to.port;
			p.type = type;
			m_pending.push_back(p);
		}
		virtual void NotifyProcessEvent(IFlowNode::EFlowEvent, IFlowNode::SActivationInfo*, IFlowNode*) {}
		virtual void AddFilter(IFlowGraphInspector::IFilterPtr) {}
		virtual void RemoveFilter(IFlowGraphInspector::IFilterPtr) {}
		virtual void GetMemoryStatistics(ICrySizer*) {}
		// a new level: what the old one still queued is dropped
		void ClearPending() { m_pending.clear(); }
	private:
		// the graph is held: an activation made outside a graph update waits
		// for the next update of any graph, and its own graph may be deleted
		// meanwhile (the old level's graphs when the next level replaces it)
		struct SPending { IFlowGraphPtr pGraph; uint32 key; uint32 node; uint32 port; string type; bool mirror; };
		std::vector<SPending> m_pending;
		int m_ref;
	};
	CCoopFlowInspector s_flowInspector;
	bool s_flowInspectorRegistered = false;

	// client side
	IFlowGraphPtr s_mirrorGraph;
	std::map<std::pair<uint32, uint32>, TFlowNodeId> s_mirrorNodes;
	std::set<TFlowNodeId> s_mirrorInitialized;

	void ResetFlowMirror()
	{
		s_mirrorNodes.clear();
		s_mirrorInitialized.clear();
		s_mirrorGraph = 0;
		s_flowHistory.clear();
		s_flowInspector.ClearPending();
		s_playerMovers.clear();
	}

	void RegisterFlowInspector()
	{
		if (s_flowInspectorRegistered || !gEnv->pFlowSystem)
			return;
		gEnv->pFlowSystem->RegisterInspector(IFlowGraphInspectorPtr(&s_flowInspector), 0);
		s_flowInspectorRegistered = true;
		CryLogAlways("[CoopFlow] presentation flow nodes are mirrored to clients (%s)", s_pFlowMirror ? s_pFlowMirror->GetString() : "");
	}
}

// ---------------------------------------------------------------------------
// map/radar markers and cutscenes follow the host (server -> clients)
namespace
{
	enum ESyncKind { eSync_Radar = 1, eSync_Sequence = 2, eSync_TimeOfDay = 3, eSync_Hide = 5, eSync_AIState = 6, eSync_Progress = 7, eSync_DebugLook = 8, eSync_HostPlayer = 9, eSync_Usable = 10, eSync_PlayerSeat = 11, eSync_SuitMode = 12 };
	// client: the host's player (the campaign's player; the others are extra)
	EntityId s_hostPlayerId = 0;
	// debugging (client): the local player's camera is kept on this entity
	EntityId s_debugLookTarget = 0;
	bool s_debugLookCenter = false;
	bool s_debugLookPoint = false;
	Vec3 s_debugLookAt(0, 0, 0);
	// cutscenes on this machine (see UpdateCutscenePresentation)
	IAnimSequence* s_forcedCutscene = 0;
	// the player whose eyes this machine's view is (see CoopAI::OnEyeView)
	EntityId s_eyeViewTarget = 0;
	float s_eyeViewTime = -1.0f;
	EntityId s_eyeHidden = 0;
	std::set<EntityId> s_cutsceneHidden;
	float s_debugLookUntil = 0.0f;
	// server: the last hidden state of every level entity whose state changed
	std::map<EntityId, std::pair<string, bool> > s_hideState;
	enum ESeqOp { eSeq_Start = 1, eSeq_Stop = 2, eSeq_Time = 3 };
	const char* const s_radarOpNames[] = { "?", "tag", "add", "add_temp", "story_add", "story_remove", "remove", "teammate", "jammer", "show_temp" };

	bool s_applyingSync = false;
	struct SSyncMsg { int op; EntityId entity; string name, text; int type; float f; };
	std::vector<SSyncMsg> s_radarHistory;
	std::map<EntityId, float> s_radarTempSent;
	std::map<string, float> s_seqServer;
	float s_seqTimer = 0.0f;
	float s_seqTimeSync = 0.0f;
	float s_teamMateTimer = 0.0f;
	float s_todTimer = 0.0f;

	// server: the time of day (and how fast it runs) to the clients, every 2 s
	void UpdateTimeOfDaySync(float frameTime, int channelId)
	{
		if (!gEnv->bServer || !gEnv->bMultiplayer || !CoopAI::IsCoopSession())
			return;
		if (!channelId)
		{
			s_todTimer += frameTime;
			if (s_todTimer < 2.0f)
				return;
			s_todTimer = 0.0f;
		}
		ITimeOfDay* pTOD = gEnv->p3DEngine->GetTimeOfDay();
		CGameRules* pRules = g_pGame->GetGameRules();
		if (!pTOD || !pRules)
			return;
		ITimeOfDay::SAdvancedInfo info;
		pTOD->GetAdvancedInfo(info);
		char adv[96];
		_snprintf(adv, sizeof(adv), "%f %f %f", info.fAnimSpeed, info.fStartTime, info.fEndTime);
		adv[sizeof(adv) - 1] = 0;
		pRules->CoopSendSync(eSync_TimeOfDay, 0, 0, "", adv, 0, pTOD->GetTime(), channelId);
	}

	const char* RadarOpName(int op)
	{
		return (op > 0 && op < (int)(sizeof(s_radarOpNames) / sizeof(s_radarOpNames[0]))) ? s_radarOpNames[op] : "?";
	}

	// the clients have no AI objects: what their radar, map and binoculars
	// ask the AI (is he an enemy, is he alerted) comes from the server.
	// state bits: 0-1 alertness (0 idle, 1 suspicious, 2 combat), 2 hostile to
	// the players, 3 AI enabled
	enum { eAIState_Hostile = 4, eAIState_Enabled = 8 };
	std::map<EntityId, std::pair<string, int> > s_aiStateSent; // server
	std::map<EntityId, int> s_aiStateMirror; // client
	float s_aiStateTimer = 0.0f;

	// the HUD progress bar a level's scripts show (data downloads...) is
	// repeated on the clients as it shows on the host
	struct SProgressState { bool visible; int progress; int packed; string text; };
	SProgressState s_progress = { false, 0, 0, "" };

	// Interactive level objects (terminals, consoles, switches, laptops, door
	// locks...): whether one can be used lives in its script, in a state and
	// a few flags the story sets on the server (EnableUsable/DisableUsable,
	// used once, turned off, locked...). A client's copy never heard of
	// those: no "use" prompt, or one on a used-up object. The server sends
	// each object's flags when they change, and all of them to a joining
	// player (a client's copy does not even start like the server's: a
	// Switch is made usable by its server init only); a client sets them on
	// its copy (its own "use" check then works, distance included). Script
	// states are left alone: switching them runs the object's logic there.
	const char* const s_usableFields[] = { "bUsable", "usable", "__usable", "bTemporaryUsable", "bLocked", "bCoolDown" };
	std::vector<EntityId> s_usableList;
	std::map<EntityId, string> s_usableSent; // server: last sent
	float s_usableTimer = 0.0f;
	float s_usableScanTimer = 0.0f;

	bool IsUsableObject(IEntity* pEntity)
	{
		IScriptTable* pTable = pEntity->GetScriptTable();
		if (!pTable || pTable->GetValueType("IsUsable") != svtFunction)
			return false;
		if (CGameRules* pRules = g_pGame->GetGameRules())
			if (pRules->GetEntity() == pEntity)
				return false;
		IGameFramework* pFramework = g_pGame->GetIGameFramework();
		return !pFramework->GetIActorSystem()->GetActor(pEntity->GetId()) && !pFramework->GetIItemSystem()->GetItem(pEntity->GetId())
			&& !pFramework->GetIVehicleSystem()->GetVehicle(pEntity->GetId());
	}

	// "|field=value|..." ("" when the object has none of the flags)
	string UsableSignature(IEntity* pEntity)
	{
		IScriptTable* pTable = pEntity->GetScriptTable();
		if (!pTable)
			return "";
		string sig;
		for (int i = 0; i < sizeof(s_usableFields) / sizeof(s_usableFields[0]); ++i)
		{
			ScriptAnyValue value;
			if (!pTable->GetValueAny(s_usableFields[i], value))
				continue;
			char buf[64];
			if (value.type == ANY_TNUMBER)
				_snprintf(buf, sizeof(buf), "|%s=%g", s_usableFields[i], value.number);
			else if (value.type == ANY_TBOOLEAN)
				_snprintf(buf, sizeof(buf), "|%s=b%d", s_usableFields[i], (int)value.b);
			else
				continue;
			buf[sizeof(buf) - 1] = 0;
			sig += buf;
		}
		return sig;
	}

	// atLoad: right after the level loaded, before its story ran: the flags
	// every machine's copy starts with. Objects found later (story spawns,
	// flags a script sets on first use) are sent at once.
	void ScanUsableObjects(bool atLoad)
	{
		IEntityItPtr pIt = gEnv->pEntitySystem->GetEntityIterator();
		pIt->MoveFirst();
		int added = 0;
		while (IEntity* pEntity = pIt->Next())
		{
			const EntityId id = pEntity->GetId();
			if (s_usableSent.count(id) || !IsUsableObject(pEntity))
				continue;
			const string sig = UsableSignature(pEntity);
			if (sig.empty())
				continue;
			s_usableList.push_back(id);
			s_usableSent[id] = atLoad ? sig : string();
			++added;
		}
		if (added)
			CoopAI::Trace("USABLE %d interactive objects watched (%d in all)", added, (int)s_usableList.size());
	}

	void UpdateUsableSync(float frameTime)
	{
		if (!gEnv->bServer || !gEnv->bMultiplayer || !g_pGame || !CoopAI::IsCoopSession() || !g_pGame->GetIGameFramework()->IsGameStarted())
			return;
		// new objects (story spawns): looked for now and then
		s_usableScanTimer -= frameTime;
		if (s_usableScanTimer <= 0.0f)
		{
			s_usableScanTimer = 10.0f;
			ScanUsableObjects(false);
		}
		s_usableTimer += frameTime;
		if (s_usableTimer < 0.5f)
			return;
		s_usableTimer = 0.0f;
		CGameRules* pRules = g_pGame->GetGameRules();
		if (!pRules)
			return;
		for (size_t i = 0; i < s_usableList.size(); ++i)
		{
			IEntity* pEntity = gEnv->pEntitySystem->GetEntity(s_usableList[i]);
			if (!pEntity)
				continue;
			const string sig = UsableSignature(pEntity);
			string& sent = s_usableSent[s_usableList[i]];
			if (sig == sent)
				continue;
			CoopAI::Trace("USABLE> %s %s -> %s", pEntity->GetName(), sent.c_str(), sig.c_str());
			sent = sig;
			pRules->CoopSendSync(eSync_Usable, 0, pEntity->GetId(), pEntity->GetName(), sig.c_str(), 0, 0.0f, 0);
		}
	}

	// client: an object's state and usable flags as on the server
	void ApplyUsableSignature(IEntity* pEntity, const char* sig)
	{
		IScriptTable* pTable = pEntity->GetScriptTable();
		if (!pTable || !sig)
			return;
		string all(sig);
		size_t pos = all.find('|');
		while (pos != string::npos)
		{
			const size_t next = all.find('|', pos + 1);
			const string item = all.substr(pos + 1, next == string::npos ? string::npos : next - pos - 1);
			const size_t eq = item.find('=');
			if (eq != string::npos)
			{
				const string key = item.substr(0, eq);
				const string value = item.substr(eq + 1);
				if (!value.empty() && value[0] == 'b')
					pTable->SetValueAny(key.c_str(), ScriptAnyValue(atoi(value.c_str() + 1) != 0));
				else
					pTable->SetValueAny(key.c_str(), ScriptAnyValue((float)atof(value.c_str())));
			}
			pos = next;
		}
	}

	// Players in vehicles. A client learns who sits where through two
	// network paths (the seat's state and the passenger's), and after a seat
	// that held a dead soldier they ended out of step once: the joined player
	// linked to the vehicle but in no seat (no vehicle view, no controls).
	// The server says where each player sits; a client that disagrees for a
	// second puts him there itself.
	struct SSeatWant { string vehicle; int seat; float since; float badSince; float repaired; };
	std::map<string, SSeatWant> s_seatWant;                       // client, by player name
	std::map<EntityId, std::pair<string, int> > s_playerSeatSent; // server
	float s_playerSeatTimer = 0.0f;

	// test instances started while someone works at the computer keep the
	// system cursor free, as a menu does
	void UpdateTestCursor()
	{
		static bool s_freed = false;
		ICVar* pFree = gEnv->pConsole ? gEnv->pConsole->GetCVar("coop_test_free_cursor") : 0;
		const bool want = pFree && pFree->GetIVal() != 0;
		if (want == s_freed || !gEnv->pHardwareMouse)
			return;
		if (want)
			gEnv->pHardwareMouse->IncrementCounter();
		else
			gEnv->pHardwareMouse->DecrementCounter();
		s_freed = want;
	}

	// testing: the lines of the file coop_test_cmdfile names (game folder)
	// are run as console commands, then the file is deleted
	void UpdateTestCmdFile()
	{
		static CTimeValue s_next;
		ICVar* pFile = gEnv->pConsole ? gEnv->pConsole->GetCVar("coop_test_cmdfile") : 0;
		if (!pFile || !pFile->GetString()[0] || gEnv->pTimer->GetAsyncTime() < s_next)
			return;
		s_next = gEnv->pTimer->GetAsyncTime() + CTimeValue(0.5f);
		FILE* f = fopen(pFile->GetString(), "rb");
		if (!f)
			return;
		std::vector<string> lines;
		char line[1024];
		while (fgets(line, sizeof(line), f))
		{
			string cmd = string(line).Trim();
			if (!cmd.empty())
				lines.push_back(cmd);
		}
		fclose(f);
		remove(pFile->GetString());
		for (size_t i = 0; i < lines.size(); ++i)
		{
			CryLogAlways("[CoopTest] cmdfile: %s", lines[i].c_str());
			gEnv->pConsole->ExecuteString(lines[i].c_str());
		}
	}

	void GetPlayerSeat(IActor* pActor, string& vehicle, int& seat)
	{
		IVehicle* pVehicle = pActor->GetLinkedVehicle();
		IVehicleSeat* pSeat = pVehicle ? pVehicle->GetSeatForPassenger(pActor->GetEntityId()) : 0;
		vehicle = pVehicle ? pVehicle->GetEntity()->GetName() : "";
		seat = pSeat ? (int)pSeat->GetSeatId() : 0;
	}

	void UpdatePlayerSeats(float frameTime)
	{
		s_playerSeatTimer += frameTime;
		if (s_playerSeatTimer < 0.25f || !gEnv->bMultiplayer || !g_pGame || !CoopAI::IsCoopSession())
			return;
		s_playerSeatTimer = 0.0f;
		IGameFramework* pFramework = g_pGame->GetIGameFramework();
		CGameRules* pRules = g_pGame->GetGameRules();
		if (!pRules)
			return;
		if (gEnv->bServer)
		{
			// a vehicle left empty counts as abandoned in a network game and
			// is destroyed and removed after a while (a pickup the joined
			// player parked went up in flames 60 s later and vanished): the
			// campaign keeps what the players leave, as single player does
			static int s_abandonTick = 0;
			ICVar* pKeep = gEnv->pConsole->GetCVar("coop_keep_vehicles");
			if ((!pKeep || pKeep->GetIVal() != 0) && ++s_abandonTick >= 8)
			{
				s_abandonTick = 0;
				IVehicleIteratorPtr pVehicles = pFramework->GetIVehicleSystem()->CreateVehicleIterator();
				while (IVehicle* pVehicle = pVehicles->Next())
				{
					SmartScriptTable pVehicleTable;
					IScriptTable* pTable = pVehicle->GetEntity()->GetScriptTable();
					if (pTable && !pVehicle->IsDestroyed() && pTable->GetValue("vehicle", pVehicleTable))
						Script::CallMethod(pVehicleTable, "KillAbandonTimer");
				}
			}
			IActorIteratorPtr pIt = pFramework->GetIActorSystem()->CreateActorIterator();
			while (IActor* pActor = pIt->Next())
			{
				if (!pActor->IsPlayer())
					continue;
				string vehicle;
				int seat = 0;
				GetPlayerSeat(pActor, vehicle, seat);
				std::map<EntityId, std::pair<string, int> >::iterator it = s_playerSeatSent.find(pActor->GetEntityId());
				if (it != s_playerSeatSent.end() ? (it->second.first == vehicle && it->second.second == seat) : vehicle.empty())
					continue;
				s_playerSeatSent[pActor->GetEntityId()] = std::make_pair(vehicle, seat);
				pRules->CoopSendSync(eSync_PlayerSeat, seat, pActor->GetEntityId(), pActor->GetEntity()->GetName(), vehicle.c_str(), 0, 0.0f, 0);
				CoopAI::Trace("SEAT> %s in %s seat %d", pActor->GetEntity()->GetName(), vehicle.empty() ? "-" : vehicle.c_str(), seat);
			}
			return;
		}
		const float now = gEnv->pTimer->GetCurrTime();
		for (std::map<string, SSeatWant>::iterator it = s_seatWant.begin(); it != s_seatWant.end(); ++it)
		{
			SSeatWant& want = it->second;
			IEntity* pEntity = gEnv->pEntitySystem->FindEntityByName(it->first.c_str());
			IActor* pActor = pEntity ? pFramework->GetIActorSystem()->GetActor(pEntity->GetId()) : 0;
			if (!pActor)
				continue;
			IEntity* pVehicleEntity = want.vehicle.empty() ? 0 : gEnv->pEntitySystem->FindEntityByName(want.vehicle.c_str());
			IVehicle* pWanted = pVehicleEntity ? pFramework->GetIVehicleSystem()->GetVehicle(pVehicleEntity->GetId()) : 0;
			if (!want.vehicle.empty() && !pWanted)
				continue;
			IVehicle* pLinked = pActor->GetLinkedVehicle();
			IVehicleSeat* pCurrent = pLinked ? pLinked->GetSeatForPassenger(pActor->GetEntityId()) : 0;
			const bool ok = pWanted ? (pLinked == pWanted && pCurrent && (int)pCurrent->GetSeatId() == want.seat) : !pLinked;
			if (ok)
			{
				want.badSince = -1.0f;
				continue;
			}
			if (want.badSince < 0.0f)
				want.badSince = now;
			// the network gets a second; a repair gets two to show
			if (now - want.badSince < 1.0f || now - want.since < 1.0f || now - want.repaired < 2.0f)
				continue;
			want.repaired = now;
			CoopAI::Trace("SEAT< repair %s: here %s seat %s, server %s seat %d", it->first.c_str(), pLinked ? pLinked->GetEntity()->GetName() : "-",
				pCurrent ? pCurrent->GetSeatName() : "-", want.vehicle.empty() ? "-" : want.vehicle.c_str(), want.seat);
			IVehicleSeat* pSeat = pWanted ? pWanted->GetSeatById((TVehicleSeatId)want.seat) : 0;
			if (pCurrent && pCurrent != pSeat)
				pCurrent->Exit(false, true);
			else if (pLinked && !pCurrent)
				pActor->LinkToVehicle(0);
			if (pSeat)
			{
				// whoever the seat still holds here (a dead soldier) goes first
				if (pSeat->GetPassenger() && pSeat->GetPassenger() != pActor->GetEntityId())
					pSeat->Exit(false, true);
				const bool entered = pSeat->GetPassenger() == pActor->GetEntityId() || pSeat->Enter(pActor->GetEntityId(), false);
				CoopAI::Trace("SEAT< %s into %s seat %s: %s", it->first.c_str(), want.vehicle.c_str(), pSeat->GetSeatName(), entered ? "in" : "failed");
			}
		}
	}

	// NanoSuit:ModeControl steps of this level (op bits: 1 add, 2 remove,
	// 4 defect, 8 repair), replayed to a joining player
	std::vector<std::pair<int, int> > s_suitHistory;

	void ResetSync()
	{
		s_suitHistory.clear();
		s_seatWant.clear();
		s_playerSeatSent.clear();
		s_usableList.clear();
		s_usableSent.clear();
		s_usableScanTimer = 0.0f;
		s_hostPlayerId = 0;
		s_forcedCutscene = 0;
		s_cutsceneHidden.clear();
		s_hideState.clear();
		s_radarHistory.clear();
		s_radarTempSent.clear();
		s_seqServer.clear();
		s_aiStateSent.clear();
		s_aiStateMirror.clear();
		s_progress.visible = false;
	}

	// a killed actor's state is traced again a moment later (the ragdoll and
	// the vehicle seat react after the kill)
	struct SKillWatch { EntityId id; float time; int step; };
	std::vector<SKillWatch> s_killWatch;
	// coop_debug_kill_shots: screenshots (r_getscreenshot) after a soldier
	// died in a vehicle, to see what this machine shows of him
	std::vector<float> s_killShots;

	void TraceActorState(const char* tag, IEntity* pEntity)
	{
		IActor* pActor = g_pGame->GetIGameFramework()->GetIActorSystem()->GetActor(pEntity->GetId());
		IPhysicalEntity* pPhys = pEntity->GetPhysics();
		IVehicle* pVehicle = pActor ? pActor->GetLinkedVehicle() : 0;
		ICharacterInstance* pChar = pEntity->GetCharacter(0);
		// the pose: head and right hand in the character's space
		Vec3 head(0, 0, 0), hand(0, 0, 0);
		if (ISkeletonPose* pPose = pChar ? pChar->GetISkeletonPose() : 0)
		{
			const int16 h = pPose->GetJointIDByName("Bip01 Head");
			const int16 r = pPose->GetJointIDByName("Bip01 R Hand");
			if (h >= 0)
				head = pPose->GetAbsJointByID(h).t;
			if (r >= 0)
				hand = pPose->GetAbsJointByID(r).t;
		}
		// the animation graph: state and the inputs a vehicle seat and a
		// death set
		string graph;
		if (IAnimationGraphState* pGraph = pActor ? static_cast<CActor*>(pActor)->GetAnimationGraphState() : 0)
		{
			graph = pGraph->GetCurrentStateName() ? pGraph->GetCurrentStateName() : "?";
			static const char* inputs[] = { "Signal", "Action", "Vehicle", "VehicleSeat", "Health", "Stance", "Item", "Mounted" };
			for (int i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i)
			{
				const IAnimationGraphState::InputID id = pGraph->GetInputId(inputs[i]);
				if (id == (IAnimationGraphState::InputID)-1)
					continue;
				char value[64] = "";
				pGraph->GetInput(id, value);
				graph += string(" ") + inputs[i] + "=" + value;
			}
		}
		// the vehicle gun he holds (its user), if any
		string gun = "-";
		if (pVehicle)
			for (int i = 0; i < pVehicle->GetWeaponCount(); ++i)
				if (IItem* pItem = g_pGame->GetIGameFramework()->GetIItemSystem()->GetItem(pVehicle->GetWeaponId(i)))
					if (pItem->GetOwnerId() == pEntity->GetId())
						gun = pItem->GetEntity()->GetName();
		graph += " gun=" + gun;
		if (ISkeletonAnim* pAnim = pChar ? pChar->GetISkeletonAnim() : 0)
		{
			IAnimationSet* pSet = pChar->GetIAnimationSet();
			for (int layer = 0; layer < 4; ++layer)
				for (int i = 0; i < pAnim->GetNumAnimsInFIFO(layer); ++i)
				{
					CAnimation& a = pAnim->GetAnimFromFIFO(layer, i);
					const char* name = pSet ? pSet->GetNameByAnimID(a.m_LMG0.m_nAnimID[0]) : 0;
					char buf[160];
					_snprintf(buf, sizeof(buf), " L%d:%s t=%.2f f=%x on=%d", layer, name ? name : "?", a.m_fAnimTime, a.m_AnimParams.m_nFlags, (int)a.m_bActivated);
					buf[sizeof(buf) - 1] = 0;
					graph += buf;
				}
		}
		CoopAI::Trace("%s %s hp=%d phys=%d vehicle=%s parent=%s hidden=%d anims=%d pos=(%.2f,%.2f,%.2f) head=(%.2f,%.2f,%.2f) rhand=(%.2f,%.2f,%.2f) ag=[%s]",
			tag, pEntity->GetName(),
			pActor ? pActor->GetHealth() : -1, pPhys ? (int)pPhys->GetType() : -1, pVehicle ? pVehicle->GetEntity()->GetName() : "-",
			pEntity->GetParent() ? pEntity->GetParent()->GetName() : "-", (int)pEntity->IsHidden(),
			pChar && pChar->GetISkeletonAnim() ? pChar->GetISkeletonAnim()->GetNumAnimsInFIFO(0) : -1,
			pEntity->GetWorldPos().x, pEntity->GetWorldPos().y, pEntity->GetWorldPos().z,
			head.x, head.y, head.z, hand.x, hand.y, hand.z, graph.c_str());
	}

	// debug: a light bullet hit reported by the shooter's machine (ClientHit)
	void DebugClientHit(IActor* pShooter, IActor* pVictim)
	{
		CGameRules* pRules = g_pGame->GetGameRules();
		if (!pRules || !pShooter || !pVictim)
			return;
		const Vec3 pos = pVictim->GetEntity()->GetWorldPos();
		HitInfo hit(pShooter->GetEntityId(), pVictim->GetEntityId(), 0, -1, 0.0f, -1, -1,
			pRules->GetHitTypeId("bullet"), pos + Vec3(0, 0, 1.0f), (pos - pShooter->GetEntity()->GetWorldPos()).GetNormalizedSafe(), Vec3(0, 0, 1));
		hit.damage = 1.0f;
		pRules->ClientHit(hit);
	}

	// debug: what physical objects are around some points on this machine
	// (invisible obstacles for a vehicle a client drives: the driver's machine
	// simulates it)
	// one physical object for the trace: what it is and whether it is drawn
	string DescribePhysical(IPhysicalEntity* pPhys)
	{
		pe_status_pos pos;
		pPhys->GetStatus(&pos);
		const int foreign = pPhys->GetiForeignData();
		void* pData = pPhys->GetForeignData(foreign);
		string what;
		if (foreign == PHYS_FOREIGN_ID_ENTITY && pData)
		{
			IEntity* pE = static_cast<IEntity*>(pData);
			what.Format("entity %s (%s) hidden=%d invisible=%d", pE->GetName(), pE->GetClass()->GetName(), (int)pE->IsHidden(), (int)pE->IsInvisible());
		}
		else if (foreign == PHYS_FOREIGN_ID_STATIC && pData)
		{
			IRenderNode* pNode = static_cast<IRenderNode*>(pData);
			what.Format("static %s [%s] hidden=%d minspec=%d viewdist=%.0f", pNode->GetName(), pNode->GetEntityClassName(),
				(pNode->GetRndFlags() & ERF_HIDDEN) ? 1 : 0, pNode->GetMinSpec(), pNode->GetMaxViewDist());
		}
		else
			what.Format("foreign %d", foreign);
		string line;
		line.Format("type=%d at (%.1f,%.1f,%.1f) %s", (int)pPhys->GetType(), pos.pos.x, pos.pos.y, pos.pos.z, what.c_str());
		return line;
	}

	// diagnostics: the local player's vehicle stopped dead (a crash). Traces
	// what physical objects were in front of it on this machine, the one that
	// simulates the vehicle of its own driver ("invisible" obstacles)
	EntityId s_crashVehicle = 0;
	Vec3 s_crashLastVel(0, 0, 0);
	float s_crashLastTime = 0.0f, s_crashTraced = -100.0f;
	void UpdateVehicleCrashTrace()
	{
		IActor* pLocal = g_pGame->GetIGameFramework()->GetClientActor();
		IVehicle* pVehicle = pLocal ? pLocal->GetLinkedVehicle() : 0;
		IPhysicalEntity* pPhys = pVehicle ? pVehicle->GetEntity()->GetPhysics() : 0;
		if (!pPhys || !CoopAI::TraceOn())
		{
			s_crashVehicle = 0;
			return;
		}
		pe_status_dynamics dyn;
		if (!pPhys->GetStatus(&dyn))
			return;
		const float now = gEnv->pTimer->GetCurrTime();
		const Vec3 vel = dyn.v;
		const float was = Vec2(s_crashLastVel.x, s_crashLastVel.y).GetLength(), is = Vec2(vel.x, vel.y).GetLength();
		if (s_crashVehicle == pVehicle->GetEntityId() && was > 6.0f && is < was * 0.5f && now - s_crashLastTime < 0.3f && now - s_crashTraced > 2.0f)
		{
			s_crashTraced = now;
			const Vec3 at = pVehicle->GetEntity()->GetWorldPos() + Vec3(s_crashLastVel.x, s_crashLastVel.y, 0).GetNormalizedSafe() * 2.5f;
			IPhysicalEntity** pList = 0;
			const int n = gEnv->pPhysicalWorld->GetEntitiesInBox(at - Vec3(3, 3, 2), at + Vec3(3, 3, 3), pList, ent_static | ent_sleeping_rigid | ent_rigid | ent_living | ent_independent);
			CoopAI::Trace("CRASH %s %.1f -> %.1f m/s, ahead at (%.1f,%.1f,%.1f): %d objects", pVehicle->GetEntity()->GetName(), was, is, at.x, at.y, at.z, n);
			for (int i = 0; i < n; ++i)
				if (pList[i] != pPhys)
					CoopAI::Trace("CRASH    %s", DescribePhysical(pList[i]).c_str());
		}
		s_crashVehicle = pVehicle->GetEntityId();
		s_crashLastVel = vel;
		s_crashLastTime = now;
	}

	// debug: a soldier's physics as this machine has it (soldiers dropped by a
	// helicopter hung in the air and never moved on the server)
	float s_physDumpTimer = 0.0f;
	void UpdateDebugPhysDump(float frameTime)
	{
		ICVar* pMatch = gEnv->pConsole->GetCVar("coop_debug_physdump");
		if (!pMatch || !pMatch->GetString()[0])
			return;
		s_physDumpTimer += frameTime;
		if (s_physDumpTimer < 2.0f)
			return;
		s_physDumpTimer = 0.0f;
		IActorIteratorPtr pIt = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = pIt->Next())
		{
			IEntity* pEntity = pActor->GetEntity();
			if (pActor->IsPlayer() || pActor->GetHealth() <= 0 || !CryStringUtils::stristr(pEntity->GetName(), pMatch->GetString()))
				continue;
			IPhysicalEntity* pPhys = pEntity->GetPhysics();
			pe_status_living living;
			pe_player_dynamics dyn;
			pe_params_flags flags;
			pe_status_awake awake;
			const bool hasLiving = pPhys && pPhys->GetStatus(&living);
			const bool hasDyn = pPhys && pPhys->GetParams(&dyn);
			const bool hasFlags = pPhys && pPhys->GetParams(&flags);
			const int isAwake = pPhys ? pPhys->GetStatus(&awake) : -1;
			IAnimatedCharacter* pAC = static_cast<CActor*>(pActor)->GetAnimatedCharacter();
			CoopAI::Trace("PHYSDUMP %s pos=(%.1f,%.1f,%.1f) parent=%s phys=%d profile=%d awake=%d flying=%d tFly=%.1f vel=(%.1f,%.1f,%.1f) req=(%.1f,%.1f,%.1f) ground=%.1f active=%d grav=%.1f flags=%x collider=%d linked=%s",
				pEntity->GetName(), pEntity->GetWorldPos().x, pEntity->GetWorldPos().y, pEntity->GetWorldPos().z,
				pEntity->GetParent() ? pEntity->GetParent()->GetName() : "-", pPhys ? (int)pPhys->GetType() : -1,
				(int)pActor->GetGameObject()->GetAspectProfile(eEA_Physics), isAwake,
				hasLiving ? living.bFlying : -1, hasLiving ? living.timeFlying : 0.0f,
				hasLiving ? living.vel.x : 0.0f, hasLiving ? living.vel.y : 0.0f, hasLiving ? living.vel.z : 0.0f,
				hasLiving ? living.velRequested.x : 0.0f, hasLiving ? living.velRequested.y : 0.0f, hasLiving ? living.velRequested.z : 0.0f,
				hasLiving ? living.groundHeight : 0.0f, hasDyn ? dyn.bActive : -1, hasDyn ? dyn.gravity.z : 0.0f, hasFlags ? flags.flags : 0,
				pAC ? (int)pAC->GetPhysicalColliderMode() : -1, pActor->GetLinkedVehicle() ? pActor->GetLinkedVehicle()->GetEntity()->GetName() : "-");
		}
	}

	float s_physQueryTimer = 0.0f;
	bool s_physQueryDone = false;
	void UpdateDebugPhysQuery(float frameTime)
	{
		ICVar* pPoints = gEnv->pConsole->GetCVar("coop_debug_physquery");
		ICVar* pTime = gEnv->pConsole->GetCVar("coop_debug_physquery_time");
		if (!pPoints || !pPoints->GetString()[0] || s_physQueryDone || !gEnv->pPhysicalWorld || !g_pGame->GetIGameFramework()->IsGameStarted())
			return;
		s_physQueryTimer += frameTime;
		if (s_physQueryTimer < (pTime ? pTime->GetFVal() : 30.0f))
			return;
		s_physQueryDone = true;
		string all(pPoints->GetString());
		size_t start = 0;
		while (start < all.size())
		{
			size_t end = all.find(';', start);
			if (end == string::npos)
				end = all.size();
			float x = 0, y = 0;
			if (sscanf(all.substr(start, end - start).c_str(), "%f,%f", &x, &y) == 2)
			{
				const float z = gEnv->p3DEngine->GetTerrainElevation(x, y);
				IPhysicalEntity** pList = 0;
				const int n = gEnv->pPhysicalWorld->GetEntitiesInBox(Vec3(x - 3, y - 3, z - 1), Vec3(x + 3, y + 3, z + 4), pList, ent_static | ent_sleeping_rigid | ent_rigid | ent_living | ent_independent);
				CoopAI::Trace("PHYSQ (%.1f,%.1f) %d objects", x, y, n);
				for (int i = 0; i < n; ++i)
					CoopAI::Trace("PHYSQ    %s", DescribePhysical(pList[i]).c_str());
			}
			start = end + 1;
		}
	}

	// debug: a level flow graph's node output fired by hand (a story step)
	float s_debugFlowTimer = 0.0f;
	bool s_debugFlowDone = false;
	float s_debugFlowAt = -1.0f;
	bool s_debugMenuOpened = false;
	void UpdateDebugFlow(float frameTime)
	{
		ICVar* pMenuAt = gEnv->pConsole->GetCVar("coop_debug_menu_at");
		if (s_debugFlowAt >= 0.0f && !s_debugMenuOpened && pMenuAt && pMenuAt->GetFVal() > 0.0f
			&& gEnv->pTimer->GetCurrTime() - s_debugFlowAt >= pMenuAt->GetFVal())
		{
			// the host at the menu (as when the person plays the joined player)
			s_debugMenuOpened = true;
			if (CFlashMenuObject* pMenu = g_pGame->GetMenu())
				pMenu->ShowInGameMenu(true);
			CoopAI::Trace("DEBUG host opens the menu");
		}
		ICVar* pFlow = gEnv->pConsole->GetCVar("coop_debug_flow");
		ICVar* pTime = gEnv->pConsole->GetCVar("coop_debug_flow_time");
		if (!pFlow || !pFlow->GetString()[0] || s_debugFlowDone || !gEnv->bServer || !g_pGame->GetIGameFramework()->IsGameStarted())
			return;
		s_debugFlowTimer += frameTime;
		if (s_debugFlowTimer < (pTime ? pTime->GetFVal() : 20.0f))
			return;
		s_debugFlowDone = true;
		char graphName[128] = "", nodeName[32] = "", portName[64] = "0";
		if (sscanf(pFlow->GetString(), "%127s %31s %63s", graphName, nodeName, portName) < 2)
			return;
		int port = atoi(portName);
		// several entities may share the name: the one whose graph has the node
		IFlowGraph* pGraph = 0;
		TFlowNodeId node = InvalidFlowNodeId;
		IEntityItPtr pIt = gEnv->pEntitySystem->GetEntityIterator();
		pIt->MoveFirst();
		while (IEntity* pEntity = pIt->Next())
		{
			if (stricmp(pEntity->GetName(), graphName))
				continue;
			IEntityFlowGraphProxy* pProxy = static_cast<IEntityFlowGraphProxy*>(pEntity->GetProxy(ENTITY_PROXY_FLOWGRAPH));
			IFlowGraph* pCandidate = pProxy ? pProxy->GetFlowGraph() : 0;
			const TFlowNodeId found = pCandidate ? pCandidate->ResolveNode(nodeName) : InvalidFlowNodeId;
			if (found != InvalidFlowNodeId)
			{
				pGraph = pCandidate;
				node = found;
				break;
			}
		}
		// the output port by name ("Enter") or by index
		if (node != InvalidFlowNodeId && !isdigit((unsigned char)portName[0]))
			if (IFlowNodeData* pData = pGraph->GetNodeData(node))
			{
				SFlowNodeConfig config;
				pData->GetNode()->GetConfiguration(config);
				for (int i = 0; config.pOutputPorts && config.pOutputPorts[i].name; ++i)
					if (!stricmp(config.pOutputPorts[i].name, portName))
						port = i;
			}
		if (node != InvalidFlowNodeId)
			pGraph->ActivatePort(SFlowAddress(node, (TFlowPortId)port, true), true);
		s_debugFlowAt = gEnv->pTimer->GetCurrTime();
		CoopAI::Trace("DEBUG flow %s node %s (%s) output %d: %s", graphName, nodeName, pGraph && node != InvalidFlowNodeId ? pGraph->GetNodeTypeName(node) : "?",
			port, node != InvalidFlowNodeId ? "activated" : "not found");
	}

	float s_debugVisitTimer = 0.0f;
	bool s_debugVisited = false;
	float s_debugVisitEventTimer = -1.0f;
	int s_debugVisitStep = 0;
	std::vector<EntityId> s_debugBoarded;
	void UpdateDebugVisit(float frameTime)
	{
		ICVar* pName = gEnv->pConsole->GetCVar("coop_debug_visit");
		ICVar* pTime = gEnv->pConsole->GetCVar("coop_debug_visit_time");
		if (s_debugVisitEventTimer >= 0.0f && (s_debugVisitEventTimer -= frameTime) < 0.0f)
		{
			// the event cvar: one flow event, or steps 5 s apart ("board,kill,enter")
			ICVar* pEvent = gEnv->pConsole->GetCVar("coop_debug_visit_event");
			std::vector<string> steps;
			const string all = pEvent ? pEvent->GetString() : "";
			for (size_t pos = 0; pos <= all.size(); )
			{
				const size_t comma = all.find(',', pos);
				const size_t end = comma == string::npos ? all.size() : comma;
				steps.push_back(all.substr(pos, end - pos));
				pos = end + 1;
			}
			const string step = s_debugVisitStep < (int)steps.size() ? steps[s_debugVisitStep] : string();
			if (++s_debugVisitStep < (int)steps.size())
				s_debugVisitEventTimer = 5.0f;
			IEntity* pTarget = pName ? gEnv->pEntitySystem->FindEntityByName(pName->GetString()) : 0;
			IEntityScriptProxy* pScript = pTarget ? static_cast<IEntityScriptProxy*>(pTarget->GetProxy(ENTITY_PROXY_SCRIPT)) : 0;
			IVehicle* pVehicle = pTarget ? g_pGame->GetIGameFramework()->GetIVehicleSystem()->GetVehicle(pTarget->GetId()) : 0;
			IActor* pJoiner = 0;
			IActorIteratorPtr pPlayers = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
			while (IActor* pActor = pPlayers->Next())
				if (pActor->IsPlayer() && pActor != g_pGame->GetIGameFramework()->GetClientActor())
					pJoiner = pActor;
			if (pVehicle && (step == "board" || step == "board2"))
			{
				// the nearest living soldiers get into its first seat(s)
				for (int seatId = 1; seatId <= (step == "board2" ? 2 : 1); ++seatId)
				{
					IActor* pNearest = 0;
					float best = 60.0f * 60.0f;
					IActorIteratorPtr pIt = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
					while (IActor* pActor = pIt->Next())
						if (!pActor->IsPlayer() && pActor->GetHealth() > 0 && !pActor->GetLinkedVehicle() && !pActor->GetEntity()->IsHidden())
						{
							const float d = pActor->GetEntity()->GetWorldPos().GetSquaredDistance(pTarget->GetWorldPos());
							if (d < best)
							{
								best = d;
								pNearest = pActor;
							}
						}
					IVehicleSeat* pSeat = pVehicle->GetSeatById(seatId);
					const bool entered = pNearest && pSeat && pSeat->Enter(pNearest->GetEntityId(), false);
					if (entered)
						s_debugBoarded.push_back(pNearest->GetEntityId());
					CoopAI::Trace("DEBUG event board: %s into %s seat %s: %s", pNearest ? pNearest->GetEntity()->GetName() : "nobody", pTarget->GetName(),
						pSeat ? pSeat->GetSeatName() : "?", entered ? "in" : "failed");
				}
			}
			else if (step == "kill")
			{
				// the joined player shoots the boarded soldiers dead
				for (size_t v = 0; v < s_debugBoarded.size(); ++v)
				{
				IActor* pVictim = g_pGame->GetIGameFramework()->GetIActorSystem()->GetActor(s_debugBoarded[v]);
				CGameRules* pRules = g_pGame->GetGameRules();
				if (pVictim && pJoiner && pRules)
				{
					const Vec3 pos = pVictim->GetEntity()->GetWorldPos();
					HitInfo hit(pJoiner->GetEntityId(), pVictim->GetEntityId(), 0, -1, 0.0f, -1, -1,
						pRules->GetHitTypeId("bullet"), pos + Vec3(0, 0, 1.0f), (pos - pJoiner->GetEntity()->GetWorldPos()).GetNormalizedSafe(), Vec3(0, 0, 1));
					hit.damage = 5000.0f;
					pRules->ServerHit(hit);
				}
				CoopAI::Trace("DEBUG event kill: %s hp %d", pVictim ? pVictim->GetEntity()->GetName() : "nobody", pVictim ? pVictim->GetHealth() : -1);
				}
			}
			else if (step == "break" && pJoiner && g_pGame->GetGameRules())
			{
				CGameRules* pRules = g_pGame->GetGameRules();
				pRules->CoopSendSync(eSync_DebugLook, 5, pJoiner->GetEntityId(), pJoiner->GetEntity()->GetName(), "", 0, 0.0f, pRules->GetChannelId(pJoiner->GetEntityId()));
				CoopAI::Trace("DEBUG event break: %s's seat", pJoiner->GetEntity()->GetName());
			}
			else if ((step == "hit" || step == "hosthit") && pTarget && g_pGame->GetGameRules())
			{
				// a light hit on the visited soldier, by the joined player or the host,
				// the way a bullet reports it on the shooter's machine
				IActor* pShooter = step == "hit" ? pJoiner : g_pGame->GetIGameFramework()->GetClientActor();
				IActor* pVictim = g_pGame->GetIGameFramework()->GetIActorSystem()->GetActor(pTarget->GetId());
				CGameRules* pRules = g_pGame->GetGameRules();
				if (pShooter && pVictim && step == "hit")
					pRules->CoopSendSync(eSync_DebugLook, 6, pVictim->GetEntityId(), pVictim->GetEntity()->GetName(), "", 0, 0.0f, pRules->GetChannelId(pShooter->GetEntityId()));
				else if (pShooter && pVictim)
					DebugClientHit(pShooter, pVictim);
				CoopAI::Trace("DEBUG event %s: %s hits %s, hp %d", step.c_str(), pShooter ? pShooter->GetEntity()->GetName() : "?",
					pTarget->GetName(), pVictim ? pVictim->GetHealth() : -1);
			}
			else if (step == "leave" && pJoiner && pJoiner->GetLinkedVehicle())
			{
				// the joined player gets out of his vehicle
				IVehicle* pLeft = pJoiner->GetLinkedVehicle();
				IVehicleSeat* pSeat = pLeft->GetSeatForPassenger(pJoiner->GetEntityId());
				const bool left = pSeat && pSeat->Exit(true);
				CoopAI::Trace("DEBUG event leave: %s out of %s: %s", pJoiner->GetEntity()->GetName(), pLeft->GetEntity()->GetName(), left ? "out" : "failed");
			}
			else if (step == "wait")
				CoopAI::Trace("DEBUG event wait");
			else if (step == "killcrew" && pVehicle && pJoiner && g_pGame->GetGameRules())
			{
				// the joined player shoots everybody in it dead
				CGameRules* pRules = g_pGame->GetGameRules();
				for (TVehicleSeatId seatId = 1; seatId <= (TVehicleSeatId)pVehicle->GetSeatCount(); ++seatId)
				{
					IVehicleSeat* pSeat = pVehicle->GetSeatById(seatId);
					IActor* pVictim = pSeat ? g_pGame->GetIGameFramework()->GetIActorSystem()->GetActor(pSeat->GetPassenger()) : 0;
					if (!pVictim || pVictim->IsPlayer() || pVictim->GetHealth() <= 0)
						continue;
					const Vec3 pos = pVictim->GetEntity()->GetWorldPos();
					HitInfo hit(pJoiner->GetEntityId(), pVictim->GetEntityId(), 0, -1, 0.0f, -1, -1,
						pRules->GetHitTypeId("bullet"), pos + Vec3(0, 0, 1.0f), (pos - pJoiner->GetEntity()->GetWorldPos()).GetNormalizedSafe(), Vec3(0, 0, 1));
					hit.damage = 5000.0f;
					pRules->ServerHit(hit);
					CoopAI::Trace("DEBUG event killcrew: %s (%s) hp %d", pVictim->GetEntity()->GetName(), pSeat->GetSeatName(), pVictim->GetHealth());
				}
			}
			else if (step == "goto" && pVehicle && pJoiner && g_pGame->GetGameRules() && pVehicle->HasHelper("passenger01Enter_pos"))
			{
				// the joined player to its driver door again (it may have driven off)
				Vec3 door = pVehicle->GetHelper("passenger01Enter_pos")->GetWorldTM().GetTranslation();
				door.z = gEnv->p3DEngine->GetTerrainElevation(door.x, door.y) + 0.3f;
				const Vec3 d = pTarget->GetWorldPos() - door;
				g_pGame->GetGameRules()->MovePlayer(static_cast<CActor*>(pJoiner), door, Ang3(0, 0, atan2f(-d.x, d.y)));
				CoopAI::Trace("DEBUG event goto: %s to the door of %s (%.1f,%.1f,%.1f)", pJoiner->GetEntity()->GetName(), pTarget->GetName(), door.x, door.y, door.z);
			}
			else if (step == "enter" && pTarget && pJoiner && g_pGame->GetGameRules())
			{
				// the joined player presses "use" on it (his machine picks the seat)
				CGameRules* pRules = g_pGame->GetGameRules();
				pRules->CoopSendSync(eSync_DebugLook, 4, pTarget->GetId(), pTarget->GetName(), "", 0, 0.0f, pRules->GetChannelId(pJoiner->GetEntityId()));
				CoopAI::Trace("DEBUG event enter: %s uses %s", pJoiner->GetEntity()->GetName(), pTarget->GetName());
			}
			else if (pScript && !step.empty())
			{
				pScript->CallEvent(step.c_str());
				CoopAI::Trace("DEBUG event %s sent to %s", step.c_str(), pTarget->GetName());
			}
		}
		if (!pName || !pName->GetString() || !pName->GetString()[0] || s_debugVisited || !gEnv->bServer || !gEnv->bMultiplayer)
			return;
		IGameFramework* pFramework = g_pGame->GetIGameFramework();
		IActor* pLocal = pFramework->GetClientActor();
		IActor* pJoiner = 0;
		IActorIteratorPtr pIt = pFramework->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = pIt->Next())
			if (pActor->IsPlayer() && pActor != pLocal && pActor->GetHealth() > 0 && static_cast<CActor*>(pActor)->GetSpectatorMode() == 0)
				pJoiner = pActor;
		if (!pJoiner)
			return;
		s_debugVisitTimer += frameTime;
		if (s_debugVisitTimer < (pTime ? pTime->GetFVal() : 30.0f))
			return;
		s_debugVisited = true;
		CGameRules* pRules = g_pGame->GetGameRules();
		// "x y z yaw": that spot, looking that way (level ahead)
		Vec3 at;
		float yaw = 0.0f;
		if (pRules && sscanf(pName->GetString(), "%f %f %f %f", &at.x, &at.y, &at.z, &yaw) == 4)
		{
			const float r = DEG2RAD(yaw);
			pRules->MovePlayer(static_cast<CActor*>(pJoiner), at, Ang3(0, 0, r));
			// a bit down: consoles, switches and panels are below the eyes; a
			// spot high above the ground ("x y z" off the terrain) looks 20 m
			// ahead instead, at chest height over the ground there
			const float ground = gEnv->p3DEngine->GetTerrainElevation(at.x, at.y);
			Vec3 look = at + Vec3(0, 0, 1.1f) + Vec3(-sinf(r), cosf(r), 0.0f) * 1.5f;
			if (at.z > ground + 2.0f)
			{
				look = at + Vec3(-sinf(r), cosf(r), 0.0f) * 20.0f;
				look.z = gEnv->p3DEngine->GetTerrainElevation(look.x, look.y) + 1.2f;
			}
			char point[96];
			_snprintf(point, sizeof(point), "%f %f %f", look.x, look.y, look.z);
			point[sizeof(point) - 1] = 0;
			pRules->CoopSendSync(eSync_DebugLook, 3, pJoiner->GetEntityId(), pJoiner->GetEntity()->GetName(), point, 0, 15.0f,
				pRules->GetChannelId(pJoiner->GetEntityId()));
			CoopAI::Trace("DEBUG %s visits (%.1f,%.1f,%.1f) yaw %.0f", pJoiner->GetEntity()->GetName(), at.x, at.y, at.z, yaw);
			return;
		}
		IEntity* pTarget = gEnv->pEntitySystem->FindEntityByName(pName->GetString());
		if (!pTarget || !pRules)
		{
			CoopAI::Trace("DEBUG visit: no entity %s", pName->GetString());
			return;
		}
		AABB box;
		pTarget->GetWorldBounds(box);
		const Vec3 center = box.GetCenter();
		// a vehicle: at a door (its "enter" prompt needs a seat's enter spot)
		if (IVehicle* pVehicle = g_pGame->GetIGameFramework()->GetIVehicleSystem()->GetVehicle(pTarget->GetId()))
			for (int i = 1; i <= 6; ++i)
			{
				char helper[32];
				_snprintf(helper, sizeof(helper), "passenger%02dEnter_pos", i);
				helper[sizeof(helper) - 1] = 0;
				if (!pVehicle->HasHelper(helper))
					continue;
				Vec3 door = pVehicle->GetHelper(helper)->GetWorldTM().GetTranslation();
				door.z = gEnv->p3DEngine->GetTerrainElevation(door.x, door.y) + 0.3f;
				const Vec3 body = pTarget->GetWorldPos() + Vec3(0, 0, 1.0f);
				const Vec3 d = body - door;
				pRules->MovePlayer(static_cast<CActor*>(pJoiner), door, Ang3(0, 0, atan2f(-d.x, d.y)));
				char point[96];
				_snprintf(point, sizeof(point), "%f %f %f", body.x, body.y, body.z);
				point[sizeof(point) - 1] = 0;
				pRules->CoopSendSync(eSync_DebugLook, 3, pTarget->GetId(), pTarget->GetName(), point, 0, 90.0f, pRules->GetChannelId(pJoiner->GetEntityId()));
				CoopAI::Trace("DEBUG %s visits %s at its %s (%.1f,%.1f,%.1f)", pJoiner->GetEntity()->GetName(), pTarget->GetName(), helper, door.x, door.y, door.z);
				ICVar* pEvent = gEnv->pConsole->GetCVar("coop_debug_visit_event");
				if (pEvent && pEvent->GetString() && pEvent->GetString()[0])
					s_debugVisitEventTimer = 6.0f;
				return;
			}
		Vec3 away = pJoiner->GetEntity()->GetWorldPos() - center;
		away.z = 0.0f;
		away = away.GetLengthSquared() > 0.01f ? away.GetNormalized() : Vec3(1, 0, 0);
		// 1 m in front of its surface (found by a ray at chest height), on
		// its side facing the player, looking at that spot
		const float reach = max(box.GetSize().x, box.GetSize().y) * 0.5f + 2.0f;
		const float ground = gEnv->p3DEngine->GetTerrainElevation(center.x, center.y);
		Vec3 from = center + away * reach;
		from.z = ground + 1.3f;
		Vec3 spot = Vec3(center.x, center.y, ground + 1.3f);
		ray_hit hit;
		if (gEnv->pPhysicalWorld->RayWorldIntersection(from, spot - from, ent_all, rwi_stop_at_pierceable | rwi_colltype_any, &hit, 1))
			spot = hit.pt;
		Vec3 pos = spot + away * 1.0f;
		pos.z = gEnv->p3DEngine->GetTerrainElevation(pos.x, pos.y) + 0.3f;
		const Vec3 d = spot - pos;
		pRules->MovePlayer(static_cast<CActor*>(pJoiner), pos, Ang3(0, 0, atan2f(-d.x, d.y)));
		char point[96];
		_snprintf(point, sizeof(point), "%f %f %f", spot.x, spot.y, spot.z);
		point[sizeof(point) - 1] = 0;
		pRules->CoopSendSync(eSync_DebugLook, 3, pTarget->GetId(), pTarget->GetName(), point, 0, 15.0f, pRules->GetChannelId(pJoiner->GetEntityId()));
		CoopAI::Trace("DEBUG %s visits %s (surface at %.1f,%.1f,%.1f)", pJoiner->GetEntity()->GetName(), pTarget->GetName(), spot.x, spot.y, spot.z);
		ICVar* pEvent = gEnv->pConsole->GetCVar("coop_debug_visit_event");
		if (pEvent && pEvent->GetString() && pEvent->GetString()[0])
			s_debugVisitEventTimer = 6.0f;
	}

	void UpdateKillWatch(float frameTime)
	{
		UpdateDebugVisit(frameTime);
		UpdateDebugFlow(frameTime);
		UpdateDebugPhysQuery(frameTime);
		UpdateDebugPhysDump(frameTime);
		UpdateVehicleExits(frameTime);
		UpdateVehicleCrashTrace();
		UpdateMirrorVideoTrace(frameTime);
		const float now = gEnv->pTimer->GetCurrTime();
		if (s_debugLookTarget && now < s_debugLookUntil && !gEnv->bServer)
		{
			IEntity* pTarget = gEnv->pEntitySystem->GetEntity(s_debugLookTarget);
			CPlayer* pLocal = static_cast<CPlayer*>(g_pGame->GetIGameFramework()->GetClientActor());
			if (pTarget && pLocal)
			{
				Vec3 aim = pTarget->GetWorldPos() + Vec3(0, 0, 1.2f);
				if (s_debugLookPoint)
					aim = s_debugLookAt;
				else if (s_debugLookCenter)
				{
					AABB box;
					pTarget->GetWorldBounds(box);
					aim = box.GetCenter();
				}
				else if (ICharacterInstance* pChar = pTarget->GetCharacter(0))
				{
					const int16 h = pChar->GetISkeletonPose()->GetJointIDByName("Bip01 Spine2");
					if (h >= 0)
						aim = pTarget->GetWorldTM() * pChar->GetISkeletonPose()->GetAbsJointByID(h).t;
				}
				const Vec3 eye = GetISystem()->GetViewCamera().GetPosition();
				const Vec3 dir = (aim - eye).GetNormalizedSafe(Vec3(0, 1, 0));
				pLocal->SetViewRotation(Quat::CreateRotationVDir(dir));
			}
		}
		else
			s_debugLookTarget = 0;
		for (size_t i = 0; i < s_killShots.size(); )
		{
			if (now < s_killShots[i])
			{
				++i;
				continue;
			}
			s_killShots.erase(s_killShots.begin() + i);
			gEnv->pConsole->ExecuteString("r_getscreenshot 1");
			CoopAI::Trace("KILLSHOT screenshot taken");
			CryLogAlways("[CoopKill] screenshot");
		}
		if (s_killWatch.empty())
			return;
		static const float steps[] = { 0.2f, 0.5f, 1.0f, 1.5f, 3.0f };
		static const char* tags[] = { "KILLED+0.2s", "KILLED+0.5s", "KILLED+1.0s", "KILLED+1.5s", "KILLED+3.0s" };
		for (size_t i = 0; i < s_killWatch.size(); )
		{
			SKillWatch& w = s_killWatch[i];
			if (now - w.time < steps[w.step])
			{
				++i;
				continue;
			}
			if (IEntity* pEntity = gEnv->pEntitySystem->GetEntity(w.id))
				TraceActorState(tags[w.step], pEntity);
			if (++w.step < 5)
			{
				++i;
				continue;
			}
			s_killWatch.erase(s_killWatch.begin() + i);
		}
	}

	// debugging: soldiers in vehicles near a joined player are shot dead by
	// him every N seconds (coop_debug_kill_gunners), to check what the
	// clients show of a dead gunner
	float s_debugKillTimer = 0.0f;
	EntityId s_debugKillVictim = 0;
	EntityId s_debugKillShooter = 0;
	float s_debugKillAimTime = 0.0f;

	// the joined player is put 10 m from the soldier, looking at him
	void DebugFaceVictim(CActor* pShooter, IEntity* pVictim, bool move, bool otherSide = false)
	{
		CGameRules* pRules = g_pGame->GetGameRules();
		const Vec3 target = pVictim->GetWorldPos();
		Vec3 pos = pShooter->GetEntity()->GetWorldPos();
		if (move)
		{
			Vec3 away = pos - target;
			away.z = 0.0f;
			if (away.GetLengthSquared() < 0.01f)
				away = Vec3(1, 0, 0);
			if (otherSide)
				away = Vec3(-away.y, away.x, 0.0f); // from the side
			away.Normalize();
			// a spot 10 m away from where he can be seen (walls, fences...)
			IPhysicalEntity* pSkip[2] = { pVictim->GetPhysics(), pVictim->GetParent() ? pVictim->GetParent()->GetPhysics() : 0 };
			const int nSkip = pSkip[1] ? 2 : 1;
			const Vec3 chest = target + Vec3(0, 0, 1.3f);
			ICVar* pDist = gEnv->pConsole->GetCVar("coop_debug_kill_view_dist");
			const float dist = pDist ? max(3.0f, pDist->GetFVal()) : 10.0f;
			Vec3 best = target + away * dist;
			for (int i = 0; i < 16; ++i)
			{
				const float a = (i / 2) * (gf_PI / 8.0f) * ((i & 1) ? -1.0f : 1.0f);
				const Vec3 dir(away.x * cosf(a) - away.y * sinf(a), away.x * sinf(a) + away.y * cosf(a), 0.0f);
				Vec3 cand = target + dir * dist;
				cand.z = gEnv->p3DEngine->GetTerrainElevation(cand.x, cand.y) + 0.3f;
				ray_hit hit;
				const Vec3 eye = cand + Vec3(0, 0, 1.6f);
				if (!gEnv->pPhysicalWorld->RayWorldIntersection(eye, chest - eye, ent_static | ent_terrain | ent_rigid | ent_sleeping_rigid,
					rwi_stop_at_pierceable | rwi_colltype_any, &hit, 1, pSkip, nSkip))
				{
					best = cand;
					break;
				}
			}
			pos = best;
			pos.z = gEnv->p3DEngine->GetTerrainElevation(pos.x, pos.y) + 0.3f;
		}
		Vec3 d = target - pos;
		pRules->MovePlayer(pShooter, pos, Ang3(atan2f(d.z - 0.4f, Vec2(d.x, d.y).GetLength()), 0, atan2f(-d.x, d.y)));
	}

	void UpdateDebugKillGunners(float frameTime)
	{
		ICVar* pInterval = gEnv->pConsole->GetCVar("coop_debug_kill_gunners");
		if (!pInterval || pInterval->GetFVal() <= 0.0f || !gEnv->bServer || !gEnv->bMultiplayer || !CoopAI::IsCoopSession())
			return;
		if (s_debugKillVictim)
		{
			IActor* pVictim = g_pGame->GetIGameFramework()->GetIActorSystem()->GetActor(s_debugKillVictim);
			CActor* pShooter = static_cast<CActor*>(g_pGame->GetIGameFramework()->GetIActorSystem()->GetActor(s_debugKillShooter));
			if (!pVictim || !pShooter || pVictim->GetHealth() <= 0)
			{
				s_debugKillVictim = 0;
				return;
			}
			if (gEnv->pTimer->GetCurrTime() - s_debugKillAimTime < 2.0f)
			{
				// keep looking at him (he may still turn), twice a second
				static float s_lastFace = 0.0f;
				if (gEnv->pTimer->GetCurrTime() - s_lastFace > 0.5f)
				{
					s_lastFace = gEnv->pTimer->GetCurrTime();
					DebugFaceVictim(pShooter, pVictim->GetEntity(), false);
				}
				return;
			}
			CGameRules* pRules = g_pGame->GetGameRules();
			const Vec3 pos = pVictim->GetEntity()->GetWorldPos();
			const Vec3 from = pShooter->GetEntity()->GetWorldPos();
			HitInfo hit(pShooter->GetEntityId(), pVictim->GetEntityId(), 0, -1, 0.0f, -1, -1,
				pRules->GetHitTypeId("bullet"), pos + Vec3(0, 0, 1.0f), (pos - from).GetNormalizedSafe(), Vec3(0, 0, 1));
			hit.damage = 5000.0f;
			CoopAI::Trace("DEBUG %s shoots %s dead (in %s)", pShooter->GetEntity()->GetName(), pVictim->GetEntity()->GetName(),
				pVictim->GetLinkedVehicle() ? pVictim->GetLinkedVehicle()->GetEntity()->GetName() : "-");
			TraceActorState("KILL-before", pVictim->GetEntity());
			pRules->ServerHit(hit);
			s_debugKillVictim = 0;
			s_debugKillTimer = 0.0f;
			return;
		}
		s_debugKillTimer += frameTime;
		if (s_debugKillTimer < pInterval->GetFVal())
			return;
		s_debugKillTimer = 0.0f;
		CGameRules* pRules = g_pGame->GetGameRules();
		IGameFramework* pFramework = g_pGame->GetIGameFramework();
		IActor* pLocal = pFramework->GetClientActor();
		if (!pRules)
			return;
		std::vector<IActor*> joiners;
		IActorIteratorPtr pIt = pFramework->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = pIt->Next())
			if (pActor->IsPlayer() && pActor != pLocal && pActor->GetHealth() > 0)
				joiners.push_back(pActor);
		if (joiners.empty())
			return;
		// the nearest gunner (a soldier using a vehicle weapon) first, else the
		// nearest other passenger, within 800 m of a joined player
		IActor* pBest = 0;
		IActor* pBestJoiner = 0;
		float bestScore = 1e18f;
		IItemSystem* pItemSystem = pFramework->GetIItemSystem();
		pIt = pFramework->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = pIt->Next())
		{
			IVehicle* pVehicle = pActor->GetLinkedVehicle();
			if (pActor->IsPlayer() || pActor->GetHealth() <= 0 || !pVehicle || pActor->GetEntity()->IsHidden())
				continue;
			bool gunner = false;
			for (int w = 0; w < pVehicle->GetWeaponCount() && !gunner; ++w)
				if (IItem* pItem = pItemSystem->GetItem(pVehicle->GetWeaponId(w)))
					gunner = pItem->GetOwnerId() == pActor->GetEntityId();
			const Vec3 pos = pActor->GetEntity()->GetWorldPos();
			for (size_t j = 0; j < joiners.size(); ++j)
			{
				const float d2 = (pos - joiners[j]->GetEntity()->GetWorldPos()).GetLengthSquared();
				if (d2 > 800.0f * 800.0f)
					continue;
				const float score = d2 + (gunner ? 0.0f : 1e12f);
				if (score < bestScore)
				{
					bestScore = score;
					pBest = pActor;
					pBestJoiner = joiners[j];
				}
			}
		}
		if (!pBest)
			return;
		// first he looks at him from 10 m, then shoots (a moment later)
		s_debugKillVictim = pBest->GetEntityId();
		s_debugKillShooter = pBestJoiner->GetEntityId();
		s_debugKillAimTime = gEnv->pTimer->GetCurrTime();
		s_autopilotHoldUntil = s_debugKillAimTime + 9.0f;
		DebugFaceVictim(static_cast<CActor*>(pBestJoiner), pBest->GetEntity(), true);
		// his camera turns to him on his own machine (a server move does not
		// turn a player's view)
		pRules->CoopSendSync(eSync_DebugLook, 1, pBest->GetEntityId(), pBest->GetEntity()->GetName(), "", 0, 9.0f,
			pRules->GetChannelId(pBestJoiner->GetEntityId()));
		CoopAI::Trace("DEBUG %s aims at %s (%s in %s)", pBestJoiner->GetEntity()->GetName(), pBest->GetEntity()->GetName(),
			bestScore < 1e12f ? "gunner" : "passenger", pBest->GetLinkedVehicle()->GetEntity()->GetName());
	}

	// debugging: an explosion next to every joined player every N seconds
	// (coop_debug_explode_joiners), to check god mode and death handling
	float s_debugExplodeTimer = 0.0f;
	void UpdateDebugExplosions(float frameTime)
	{
		ICVar* pInterval = gEnv->pConsole->GetCVar("coop_debug_explode_joiners");
		if (!pInterval || pInterval->GetFVal() <= 0.0f || !gEnv->bServer || !gEnv->bMultiplayer || !CoopAI::IsCoopSession())
			return;
		s_debugExplodeTimer += frameTime;
		if (s_debugExplodeTimer < pInterval->GetFVal())
			return;
		s_debugExplodeTimer = 0.0f;
		CGameRules* pRules = g_pGame->GetGameRules();
		IGameFramework* pFramework = g_pGame->GetIGameFramework();
		IActor* pLocal = pFramework->GetClientActor();
		if (!pRules)
			return;
		IActorIteratorPtr pIt = pFramework->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = pIt->Next())
		{
			if (!pActor->IsPlayer() || pActor == pLocal || static_cast<CActor*>(pActor)->GetSpectatorMode() != 0)
				continue;
			ExplosionInfo explosion;
			explosion.damage = 400.0f;
			explosion.pos = pActor->GetEntity()->GetWorldPos() + Vec3(1.0f, 0.0f, 0.3f);
			explosion.dir = Vec3(0.0f, 0.0f, 1.0f);
			explosion.minRadius = 2.0f;
			explosion.radius = 6.0f;
			explosion.minPhysRadius = 2.0f;
			explosion.physRadius = 6.0f;
			explosion.pressure = 200.0f;
			explosion.hole_size = 0.0f;
			explosion.type = pRules->GetHitTypeId("frag");
			IItem* pItem = pActor->GetCurrentItem();
			CoopAI::Trace("DEBUG explosion next to %s hp=%d item=%s", pActor->GetEntity()->GetName(), pActor->GetHealth(),
				pItem ? pItem->GetEntity()->GetClass()->GetName() : "-");
			CryLogAlways("[CoopDebug] explosion next to %s (hp %d)", pActor->GetEntity()->GetName(), pActor->GetHealth());
			pRules->ServerExplosion(explosion);
		}
	}

	void UpdateAIStateSync(float frameTime)
	{
		if (!gEnv->bServer || !gEnv->bMultiplayer || !CoopAI::IsCoopSession())
			return;
		s_aiStateTimer += frameTime;
		if (s_aiStateTimer < 0.5f)
			return;
		s_aiStateTimer = 0.0f;
		IGameFramework* pFramework = g_pGame->GetIGameFramework();
		IActor* pLocal = pFramework->GetClientActor();
		IAIObject* pPlayerAI = pLocal ? pLocal->GetEntity()->GetAI() : 0;
		CGameRules* pRules = g_pGame->GetGameRules();
		if (!pPlayerAI || !pRules)
			return;
		int changed = 0;
		IActorIteratorPtr pIt = pFramework->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = pIt->Next())
		{
			if (pActor->IsPlayer() || pActor->GetHealth() <= 0)
				continue;
			IEntity* pEntity = pActor->GetEntity();
			IAIObject* pAI = pEntity->GetAI();
			if (!pAI)
				continue;
			IUnknownProxy* pProxy = pAI->GetProxy();
			int state = pProxy ? CLAMP(pProxy->GetAlertnessState(), 0, 3) : 0;
			if (pAI->IsHostile(pPlayerAI, false))
				state |= eAIState_Hostile;
			if (pAI->IsEnabled())
				state |= eAIState_Enabled;
			std::map<EntityId, std::pair<string, int> >::iterator it = s_aiStateSent.find(pEntity->GetId());
			if (it != s_aiStateSent.end() && it->second.second == state)
				continue;
			const int old = it != s_aiStateSent.end() ? it->second.second : -1;
			s_aiStateSent[pEntity->GetId()] = std::make_pair(string(pEntity->GetName()), state);
			pRules->CoopSendSync(eSync_AIState, state, pEntity->GetId(), pEntity->GetName(), "", 0, 0.0f, 0);
			if (old >= 0)
				CoopAI::Trace("AISTATE> %s alert %d->%d hostile=%d enabled=%d", pEntity->GetName(), old & 3, state & 3,
					(state & eAIState_Hostile) ? 1 : 0, (state & eAIState_Enabled) ? 1 : 0);
			++changed;
		}
		static bool s_first = true;
		if (changed && s_first)
		{
			s_first = false;
			CryLogAlways("[CoopSync] AI states for the clients' radar: %d soldiers", changed);
		}
	}

	// server: the cutscenes playing now, start/stop/time to the clients
	void UpdateSequenceSync(float frameTime)
	{
		if (!gEnv->bServer || !gEnv->bMultiplayer || !gEnv->pMovieSystem || !CoopAI::IsCoopSession())
			return;
		s_seqTimer += frameTime;
		if (s_seqTimer < 0.25f)
			return;
		s_seqTimeSync += s_seqTimer;
		s_seqTimer = 0.0f;
		CGameRules* pRules = g_pGame->GetGameRules();
		if (!pRules)
			return;
		const bool sendTimes = s_seqTimeSync >= 2.0f;
		if (sendTimes)
			s_seqTimeSync = 0.0f;
		std::map<string, float> now;
		if (ISequenceIt* pIt = gEnv->pMovieSystem->GetSequences(true, false))
		{
			for (IAnimSequence* pSeq = pIt->first(); pSeq; pSeq = pIt->next())
			{
				const string name = pSeq->GetName();
				const float t = gEnv->pMovieSystem->GetPlayingTime(pSeq);
				now[name] = t;
				if (!s_seqServer.count(name))
				{
					pRules->CoopSendSync(eSync_Sequence, eSeq_Start, 0, name.c_str(), "", pSeq->GetFlags(), t, 0);
					CoopAI::Trace("SYNC> cutscene start %s t=%.2f", name.c_str(), t);
				}
				else if (sendTimes)
					pRules->CoopSendSync(eSync_Sequence, eSeq_Time, 0, name.c_str(), "", 0, t, 0);
			}
			pIt->Release();
		}
		for (std::map<string, float>::iterator it = s_seqServer.begin(); it != s_seqServer.end(); ++it)
			if (!now.count(it->first))
			{
				pRules->CoopSendSync(eSync_Sequence, eSeq_Stop, 0, it->first.c_str(), "", 0, 0.0f, 0);
				CoopAI::Trace("SYNC> cutscene stop %s", it->first.c_str());
			}
		s_seqServer.swap(now);
	}

	// every machine, in a cutscene:
	//  - a cutscene joined late (a player connecting during the host's intro)
	//    never reached this HUD's OnBeginCutScene: it is started here, so the
	//    cinematic bars, the hidden HUD and the player filters are there too
	//  - bars something else took away (the spectator HUD of a joining player)
	//    are put back
	//  - "no player" cutscenes hide every player, not just this machine's own
	//    (the other players stood in the host's cutscenes), with the weapon
	//    in their hands

	void SetPlayerInvisible(IActor* pActor, bool invisible)
	{
		pActor->GetEntity()->Invisible(invisible);
		if (IItem* pItem = pActor->GetCurrentItem())
			pItem->GetEntity()->Invisible(invisible);
	}

	void UpdateCutscenePresentation()
	{
		if (!gEnv->pMovieSystem || !g_pGame || !CoopAI::IsCoopSession())
			return;
		IAnimSequence* pCut = 0;
		bool noPlayer = false, bars = false;
		if (ISequenceIt* pIt = gEnv->pMovieSystem->GetSequences(true, false))
		{
			for (IAnimSequence* pSeq = pIt->first(); pSeq; pSeq = pIt->next())
			{
				const int flags = pSeq->GetFlags();
				if (!(flags & IAnimSequence::CUT_SCENE))
					continue;
				pCut = pSeq;
				noPlayer |= (flags & IAnimSequence::NO_PLAYER) != 0;
				bars |= (flags & IAnimSequence::IS_16TO9) != 0;
			}
			pIt->Release();
		}
		CHUD* pHUD = g_pGame->GetHUD();
		IActor* pLocal = g_pGame->GetIGameFramework()->GetClientActor();
		if (pHUD && pLocal)
		{
			if (pCut && !pHUD->IsCutscenePlaying() && !s_forcedCutscene)
			{
				pHUD->OnBeginCutScene(pCut, false);
				s_forcedCutscene = pCut;
				CoopAI::Trace("CUTSCENE %s joined late: started on this HUD", pCut->GetName());
			}
			if (!pCut && s_forcedCutscene)
			{
				if (pHUD->IsCutscenePlaying())
					pHUD->OnEndCutScene(s_forcedCutscene);
				CoopAI::Trace("CUTSCENE %s over: ended on this HUD", s_forcedCutscene->GetName());
				s_forcedCutscene = 0;
			}
			if (pCut && bars && pHUD->GetCinematicBarsTarget() != g_pGameCVars->hud_panoramicHeight)
			{
				pHUD->FadeCinematicBars(g_pGameCVars->hud_panoramicHeight);
				CoopAI::Trace("CUTSCENE %s: cinematic bars back", pCut->GetName());
			}
		}
		// the player we look through is not drawn (his arms filled the view)
		const bool eyeView = s_eyeViewTarget && gEnv->pTimer->GetCurrTime() - s_eyeViewTime < 0.25f;
		if (s_eyeHidden && (!eyeView || s_eyeHidden != s_eyeViewTarget))
		{
			if (IActor* pActor = g_pGame->GetIGameFramework()->GetIActorSystem()->GetActor(s_eyeHidden))
				if (!s_cutsceneHidden.count(s_eyeHidden))
					SetPlayerInvisible(pActor, false);
			CoopAI::Trace("EYEVIEW off: %u shown", s_eyeHidden);
			s_eyeHidden = 0;
		}
		if (eyeView)
			if (IActor* pActor = g_pGame->GetIGameFramework()->GetIActorSystem()->GetActor(s_eyeViewTarget))
			{
				if (!pActor->GetEntity()->IsInvisible())
					SetPlayerInvisible(pActor, true);
				if (s_eyeHidden != s_eyeViewTarget)
					CoopAI::Trace("EYEVIEW on: through the eyes of %s", pActor->GetEntity()->GetName());
				s_eyeHidden = s_eyeViewTarget;
			}
		if (pCut)
		{
			// the campaign's cutscenes know one player, the host's: the
			// others are never in them; the host's player only goes in
			// "no player" ones (as in the campaign)
			IActorIteratorPtr pIt = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
			while (IActor* pActor = pIt->Next())
			{
				if (!pActor->IsPlayer())
					continue;
				const bool host = gEnv->bServer ? pActor == pLocal
					: (s_hostPlayerId ? pActor->GetEntityId() == s_hostPlayerId : pActor != pLocal);
				if (host && !noPlayer)
					continue;
				if (!pActor->GetEntity()->IsInvisible() || !s_cutsceneHidden.count(pActor->GetEntityId()))
				{
					SetPlayerInvisible(pActor, true);
					if (s_cutsceneHidden.insert(pActor->GetEntityId()).second)
						CoopAI::Trace("CUTSCENE %s: %s hidden", pCut ? pCut->GetName() : "?", pActor->GetEntity()->GetName());
				}
			}
		}
		else if (!s_cutsceneHidden.empty())
		{
			for (std::set<EntityId>::iterator it = s_cutsceneHidden.begin(); it != s_cutsceneHidden.end(); ++it)
				if (IActor* pActor = g_pGame->GetIGameFramework()->GetIActorSystem()->GetActor(*it))
				{
					if (*it == s_eyeHidden)
						continue;
					SetPlayerInvisible(pActor, false);
					CoopAI::Trace("CUTSCENE over: %s shown", pActor->GetEntity()->GetName());
				}
			s_cutsceneHidden.clear();
		}
	}

	// server: what a joining player has missed (markers, running cutscenes)
	void SendSyncHistory(int channelId)
	{
		CGameRules* pRules = g_pGame->GetGameRules();
		if (!pRules)
			return;
		for (size_t i = 0; i < s_radarHistory.size(); ++i)
		{
			const SSyncMsg& m = s_radarHistory[i];
			pRules->CoopSendSync(eSync_Radar, m.op, m.entity, m.name.c_str(), m.text.c_str(), m.type, m.f, channelId);
		}
		for (std::map<string, float>::iterator it = s_seqServer.begin(); it != s_seqServer.end(); ++it)
			pRules->CoopSendSync(eSync_Sequence, eSeq_Time, 0, it->first.c_str(), "", 0, it->second, channelId);
		UpdateTimeOfDaySync(0.0f, channelId);
		for (std::map<EntityId, std::pair<string, bool> >::iterator it = s_hideState.begin(); it != s_hideState.end(); ++it)
			pRules->CoopSendSync(eSync_Hide, it->second.second ? 1 : 0, it->first, it->second.first.c_str(), "", 0, 0.0f, channelId);
		for (std::map<EntityId, std::pair<string, int> >::iterator it = s_aiStateSent.begin(); it != s_aiStateSent.end(); ++it)
			pRules->CoopSendSync(eSync_AIState, it->second.second, it->first, it->second.first.c_str(), "", 0, 0.0f, channelId);
		if (IActor* pHost = g_pGame->GetIGameFramework()->GetClientActor())
			pRules->CoopSendSync(eSync_HostPlayer, 0, pHost->GetEntityId(), pHost->GetEntity()->GetName(), "", 0, 0.0f, channelId);
		for (std::map<EntityId, std::pair<string, int> >::iterator it = s_playerSeatSent.begin(); it != s_playerSeatSent.end(); ++it)
			if (IEntity* pPlayer = gEnv->pEntitySystem->GetEntity(it->first))
				pRules->CoopSendSync(eSync_PlayerSeat, it->second.second, it->first, pPlayer->GetName(), it->second.first.c_str(), 0, 0.0f, channelId);
		for (size_t i = 0; i < s_suitHistory.size(); ++i)
			pRules->CoopSendSync(eSync_SuitMode, s_suitHistory[i].first, 0, "", "", s_suitHistory[i].second, 0.0f, channelId);
		int usable = 0;
		for (std::map<EntityId, string>::iterator it = s_usableSent.begin(); it != s_usableSent.end(); ++it)
			if (!it->second.empty())
			if (IEntity* pEntity = gEnv->pEntitySystem->GetEntity(it->first))
			{
				pRules->CoopSendSync(eSync_Usable, 0, it->first, pEntity->GetName(), it->second.c_str(), 0, 0.0f, channelId);
				++usable;
			}
		CryLogAlways("[CoopSync] sent %d interactive objects to channel %d", usable, channelId);
		if (s_progress.visible)
			pRules->CoopSendSync(eSync_Progress, 1, 0, "", s_progress.text.c_str(), s_progress.packed, (float)s_progress.progress, channelId);
		CryLogAlways("[CoopSync] sent %d AI states, progress bar %s to channel %d", (int)s_aiStateSent.size(),
			s_progress.visible ? "shown" : "hidden", channelId);
		CryLogAlways("[CoopSync] sent %d hidden/shown entity states to channel %d", (int)s_hideState.size(), channelId);
		CryLogAlways("[CoopSync] sent %d map markers and %d running cutscenes to channel %d",
			(int)s_radarHistory.size(), (int)s_seqServer.size(), channelId);
	}

	// every machine: the other players show on the radar as team mates
	void UpdateTeamMates(float frameTime)
	{
		s_teamMateTimer += frameTime;
		if (s_teamMateTimer < 2.0f || !CoopAI::IsCoopSession())
			return;
		s_teamMateTimer = 0.0f;
		CHUD* pHUD = g_pGame->GetHUD();
		CHUDRadar* pRadar = pHUD ? pHUD->GetRadar() : 0;
		IActor* pLocal = g_pGame->GetIGameFramework()->GetClientActor();
		if (!pRadar || !pLocal)
			return;
		IActorIteratorPtr pIt = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = pIt->Next())
		{
			if (!pActor->IsPlayer() || pActor == pLocal)
				continue;
			const bool alive = pActor->GetHealth() > 0 && static_cast<CActor*>(pActor)->GetSpectatorMode() == 0;
			s_applyingSync = true;
			pRadar->SetTeamMate(pActor->GetEntityId(), alive);
			s_applyingSync = false;
		}
	}
}

void CoopAI::OnActorKilling(IEntity* pEntity)
{
	if (!pEntity || !IsCoopSession() || !TraceOn())
		return;
	IActor* pActor = g_pGame->GetIGameFramework()->GetIActorSystem()->GetActor(pEntity->GetId());
	if (!pActor || pActor->IsPlayer() || !pActor->GetLinkedVehicle())
		return;
	TraceActorState("KILL-start", pEntity);
}

void CoopAI::OnActorKilled(IEntity* pEntity, bool inVehicle)
{
	if (!pEntity || !IsCoopSession())
		return;
	IActor* pActor = g_pGame->GetIGameFramework()->GetIActorSystem()->GetActor(pEntity->GetId());
	if (pActor && pActor->IsPlayer())
		return;
	TraceActorState(gEnv->bServer ? "KILL>" : "KILL<", pEntity);
	ICVar* pShots = gEnv->pConsole->GetCVar("coop_debug_kill_shots");
	Trace("KILLSHOT check cvar=%d inVehicle=%d pending=%d", pShots ? pShots->GetIVal() : -1, (int)inVehicle, (int)s_killShots.size());
	if (inVehicle && pShots && pShots->GetIVal() && s_killShots.empty())
	{
		const float now = gEnv->pTimer->GetCurrTime();
		s_killShots.push_back(now);
		s_killShots.push_back(now + 0.3f);
		s_killShots.push_back(now + 1.5f);
		s_killShots.push_back(now + 4.0f);
	}
	if (inVehicle)
		CryLogAlways("[CoopKill] %s died in a vehicle (%s)", pEntity->GetName(), gEnv->bServer ? "server, clients told" : "client");
	// once per actor (a client gets the kill twice: the server's and his script's)
	for (size_t i = 0; i < s_killWatch.size(); ++i)
		if (s_killWatch[i].id == pEntity->GetId())
			return;
	SKillWatch w = { pEntity->GetId(), gEnv->pTimer->GetCurrTime(), 0 };
	s_killWatch.push_back(w);
}

void CoopAI::OnEyeView(EntityId playerId)
{
	s_eyeViewTarget = playerId;
	s_eyeViewTime = gEnv->pTimer->GetCurrTime();
}

bool CoopAI::GetMirroredAI(EntityId id, bool& hostile, int& alertness, bool* pEnabled)
{
	if (gEnv->bServer || s_aiStateMirror.empty())
		return false;
	std::map<EntityId, int>::const_iterator it = s_aiStateMirror.find(id);
	if (it == s_aiStateMirror.end())
		return false;
	hostile = (it->second & eAIState_Hostile) != 0;
	alertness = it->second & 3;
	if (pEnabled)
		*pEnabled = (it->second & eAIState_Enabled) != 0;
	return true;
}

namespace
{
	// Soldiers leaving a vehicle with an exit animation (a helicopter's
	// troops jumping out) are let go by the animation graph when it ends. On
	// the server that animation does not finish for a soldier the host does
	// not see: his collider stayed disabled, his movement physics inactive
	// (no gravity), and he hung in the air where he got out, aiming but never
	// moving. A soldier still like that 2 s after getting out is let go.
	std::map<EntityId, float> s_vehicleExits; // server: id -> time out
	float s_vehicleExitTimer = 0.0f;

	void UpdateVehicleExits(float frameTime)
	{
		if (!gEnv->bServer || s_vehicleExits.empty())
			return;
		s_vehicleExitTimer += frameTime;
		if (s_vehicleExitTimer < 0.5f)
			return;
		s_vehicleExitTimer = 0.0f;
		const float now = gEnv->pTimer->GetCurrTime();
		for (std::map<EntityId, float>::iterator it = s_vehicleExits.begin(); it != s_vehicleExits.end(); )
		{
			CActor* pActor = static_cast<CActor*>(g_pGame->GetIGameFramework()->GetIActorSystem()->GetActor(it->first));
			IPhysicalEntity* pPhys = pActor ? pActor->GetEntity()->GetPhysics() : 0;
			if (!pActor || !pPhys || pActor->GetHealth() <= 0 || pActor->GetLinkedVehicle() || now - it->second > 15.0f)
			{
				s_vehicleExits.erase(it++);
				continue;
			}
			if (now - it->second < 2.0f)
			{
				++it;
				continue;
			}
			IAnimatedCharacter* pAC = pActor->GetAnimatedCharacter();
			pe_player_dynamics dyn;
			const bool inactive = pPhys->GetParams(&dyn) && !dyn.bActive;
			const bool noCollider = pAC && pAC->GetPhysicalColliderMode() == eColliderMode_Disabled;
			if (inactive || noCollider)
			{
				if (pAC)
				{
					const EColliderModeLayer layers[] = { eColliderModeLayer_AnimGraph, eColliderModeLayer_Game, eColliderModeLayer_Script, eColliderModeLayer_ForceSleep };
					for (int i = 0; i < 4; ++i)
						pAC->RequestPhysicalColliderMode(eColliderMode_Undefined, layers[i], "Coop vehicle exit");
					pAC->ForceRefreshPhysicalColliderMode();
				}
				pe_player_dynamics active;
				active.bActive = 1;
				active.bSwimming = 0;
				active.gravity = Vec3(0, 0, -9.81f);
				pPhys->SetParams(&active);
				pe_action_awake wake;
				wake.bAwake = 1;
				pPhys->Action(&wake);
				CoopAI::Trace("EXITFIX %s let go (collider was %s, movement %s)", pActor->GetEntity()->GetName(), noCollider ? "off" : "on", inactive ? "inactive" : "active");
			}
			s_vehicleExits.erase(it++);
		}
	}
}

void CoopAI::OnAIExitedVehicle(EntityId id)
{
	if (gEnv->bServer && IsCoopSession())
		s_vehicleExits[id] = gEnv->pTimer->GetCurrTime();
}

bool CoopAI::IsVehicleCrewHostile(IVehicle* pVehicle)
{
	if (!pVehicle || gEnv->bServer || !IsCoopSession())
		return false;
	IActorSystem* pActors = g_pGame->GetIGameFramework()->GetIActorSystem();
	IActor* pEnemy = 0;
	for (TVehicleSeatId seatId = 1; seatId <= (TVehicleSeatId)pVehicle->GetSeatCount() && !pEnemy; ++seatId)
	{
		IVehicleSeat* pSeat = pVehicle->GetSeatById(seatId);
		IActor* pPassenger = pSeat ? pActors->GetActor(pSeat->GetPassenger()) : 0;
		bool hostile = false;
		int alertness = 0;
		if (pPassenger && !pPassenger->IsPlayer() && pPassenger->GetHealth() > 0
			&& GetMirroredAI(pPassenger->GetEntityId(), hostile, alertness) && hostile)
			pEnemy = pPassenger;
	}
	static EntityId s_lastVehicle = 0, s_lastEnemy = 0;
	const EntityId enemyId = pEnemy ? pEnemy->GetEntityId() : 0;
	if (pVehicle->GetEntityId() != s_lastVehicle || enemyId != s_lastEnemy)
	{
		s_lastVehicle = pVehicle->GetEntityId();
		s_lastEnemy = enemyId;
		Trace("USE vehicle %s: %s%s", pVehicle->GetEntity()->GetName(), pEnemy ? pEnemy->GetEntity()->GetName() : "no enemy",
			pEnemy ? " inside, not usable" : " inside");
	}
	return pEnemy != 0;
}

void CoopAI::OnProgressBar(int op, int progress, int posX, int posY, const char* text, bool topText, bool locking)
{
	if (!gEnv->bServer || !g_pGame || !IsCoopSession())
		return;
	if (op == 1)
	{
		s_progress.visible = true;
		s_progress.progress = progress;
		s_progress.packed = CLAMP(posX, 0, 0xfff) | (CLAMP(posY, 0, 0xfff) << 12) | (topText ? (1 << 24) : 0) | (locking ? (1 << 25) : 0);
		s_progress.text = text ? text : "";
	}
	else if (op == 0 || progress < 0)
	{
		op = 0;
		s_progress.visible = false;
	}
	else
		s_progress.progress = progress;
	Trace("SYNC> progress bar %s %d %s", op == 0 ? "hide" : op == 1 ? "show" : "set", progress, (op == 1 && text) ? text : "");
	if (!gEnv->bMultiplayer)
		return;
	if (CGameRules* pRules = g_pGame->GetGameRules())
		pRules->CoopSendSync(eSync_Progress, op, 0, "", op == 1 ? s_progress.text.c_str() : "", s_progress.packed, (float)progress, 0);
}

void CoopAI::OnEntityHidden(IEntity* pEntity, bool hidden)
{
	if (!pEntity || !gEnv->bServer || !g_pGame || s_applyingSync || !IsCoopSession())
		return;
	// the level's own things: not players, weapons (the network has those)
	// or projectiles
	IGameFramework* pFramework = g_pGame->GetIGameFramework();
	if (TraceNoisyClass(pEntity) || pFramework->GetIItemSystem()->IsItemClass(pEntity->GetClass()->GetName()))
		return;
	if (IActor* pActor = pFramework->GetIActorSystem()->GetActor(pEntity->GetId()))
		if (pActor->IsPlayer())
			return;
	s_hideState[pEntity->GetId()] = std::make_pair(string(pEntity->GetName()), hidden);
	if (!gEnv->bMultiplayer)
		return;
	if (CGameRules* pRules = g_pGame->GetGameRules())
		pRules->CoopSendSync(eSync_Hide, hidden ? 1 : 0, pEntity->GetId(), pEntity->GetName(), "", 0, 0.0f, 0);
}

void CoopAI::OnRadarOp(int op, EntityId id, int type, float f, const char* text)
{
	if (!g_pGame || !IsCoopSession())
		return;
	IEntity* pEntity = id ? gEnv->pEntitySystem->GetEntity(id) : 0;
	// temporary markers repeat every frame while they show: once per 2 s
	if ((op == 3 || op == 9) && !s_applyingSync)
	{
		const float now = gEnv->pTimer->GetCurrTime();
		std::map<EntityId, float>::iterator it = s_radarTempSent.find(id);
		if (it != s_radarTempSent.end() && now - it->second < 2.0f)
			return;
		s_radarTempSent[id] = now;
	}
	if (op != 7)
		Trace("MAP %s%s %u(%s) type=%d f=%.1f text=%s", s_applyingSync ? "applied " : "", RadarOpName(op), id,
			pEntity ? pEntity->GetName() : "-", type, f, text ? text : "");
	if (s_applyingSync)
		return;
	CGameRules* pRules = g_pGame->GetGameRules();
	const char* name = pEntity ? pEntity->GetName() : "";
	if (gEnv->bServer)
	{
		// remembered for players joining later (temporary ones are not)
		if (op != 3 && op != 9 && s_radarHistory.size() < 4000)
		{
			SSyncMsg m = { op, id, name, text ? text : "", type, f };
			s_radarHistory.push_back(m);
		}
		if (pRules && gEnv->bMultiplayer)
			pRules->CoopSendSync(eSync_Radar, op, id, name, text, type, f, 0);
	}
	else if (op == 1 && pRules)
	{
		// the joiner tagged something with his binoculars: the host (and so
		// every player) gets the tag too
		pRules->CoopSendSyncToServer(eSync_Radar, op, id, name, "", 0, 0.0f);
	}
}

bool CoopAI::IsAirborne(EntityId id)
{
	IActor* pActor = g_pGame ? g_pGame->GetIGameFramework()->GetIActorSystem()->GetActor(id) : 0;
	if (!pActor || pActor->GetLinkedVehicle())
		return false;
	if (static_cast<CActor*>(pActor)->GetActorClass() == CPlayer::GetActorClassType())
		if (static_cast<SPlayerStats*>(static_cast<CPlayer*>(pActor)->GetActorStats())->inFreefall.Value() != 0)
			return true;
	const Vec3 pos = pActor->GetEntity()->GetWorldPos();
	IPhysicalEntity* pSkip = pActor->GetEntity()->GetPhysics();
	ray_hit hit;
	if (gEnv->pPhysicalWorld->RayWorldIntersection(pos + Vec3(0, 0, 0.5f), Vec3(0, 0, -4.5f), ent_all,
		rwi_stop_at_pierceable | rwi_colltype_any, &hit, 1, pSkip ? &pSkip : 0, pSkip ? 1 : 0) > 0)
		return false;
	const float water = gEnv->p3DEngine->GetWaterLevel(&pos);
	if (water > WATER_LEVEL_UNKNOWN && pos.z - water < 4.0f)
		return false;
	return true;
}

void CoopAI::OnSuitModeControl(int mode, bool add, bool remove, bool defect, bool repair)
{
	if (!gEnv->bServer || !IsCoopSession())
		return;
	const int op = (add ? 1 : 0) | (remove ? 2 : 0) | (defect ? 4 : 0) | (repair ? 8 : 0);
	if (s_suitHistory.size() < 500)
		s_suitHistory.push_back(std::make_pair(op, mode));
	if (gEnv->bMultiplayer)
		if (CGameRules* pRules = g_pGame->GetGameRules())
			pRules->CoopSendSync(eSync_SuitMode, op, 0, "", "", mode, 0.0f, 0);
	Trace("SUIT> mode %d op %d", mode, op);
}

void CoopAI::OnSyncMirror(int kind, int op, uint32 entity, const char* name, const char* text, int type, float f, bool fromClient)
{
	if (fromClient && (kind != eSync_Radar || op != 1))
		return;
	if (kind == eSync_Radar)
	{
		// entity ids match on both sides for level entities; the name catches
		// the rest
		IEntity* pEntity = entity ? gEnv->pEntitySystem->GetEntity(entity) : 0;
		if (name && name[0] && (!pEntity || strcmp(pEntity->GetName(), name)))
			pEntity = gEnv->pEntitySystem->FindEntityByName(name);
		const EntityId id = pEntity ? pEntity->GetId() : 0;
		CHUD* pHUD = g_pGame->GetHUD();
		CHUDRadar* pRadar = pHUD ? pHUD->GetRadar() : 0;
		if (!pRadar || (!id && op != 8))
		{
			Trace("MAP mirror failed %s %u(%s)", RadarOpName(op), entity, name ? name : "");
			return;
		}
		// a client's tag is applied on the server like the host's own (and
		// comes back to every client from there)
		s_applyingSync = !fromClient;
		switch (op)
		{
		case 1: pRadar->AddTaggedEntity(id); break;
		case 2: pRadar->AddEntityToRadar(id); break;
		case 3: pRadar->AddEntityTemporarily(id, f); break;
		case 4: pRadar->AddStoryEntity(id, (FlashRadarType)type, (text && text[0]) ? text : NULL); break;
		case 5: pRadar->RemoveStoryEntity(id); break;
		case 6: pRadar->RemoveFromRadar(id); break;
		case 7: break; // team mates: every machine sets its own
		case 8: pRadar->SetJammer(id, f); break;
		case 9: pRadar->ShowEntityTemporarily((FlashRadarType)type, id, f); break;
		}
		s_applyingSync = false;
		return;
	}
	if (kind == eSync_SuitMode && !gEnv->bServer)
	{
		// what NanoSuit:ModeControl did on the server, on this player's suit
		// (the server's copy of it got the same): modes removed, broken,
		// repaired, given, with the HUD buttons
		static const ENanoMode modes[] = { NANOMODE_SPEED, NANOMODE_DEFENSE, NANOMODE_STRENGTH, NANOMODE_CLOAK };
		IActor* pActor = g_pGame->GetIGameFramework()->GetClientActor();
		if (!pActor || static_cast<CActor*>(pActor)->GetActorClass() != CPlayer::GetActorClassType() || type < 0 || type > 3)
			return;
		CNanoSuit* pSuit = static_cast<CPlayer*>(pActor)->GetNanoSuit();
		if (!pSuit)
			return;
		const ENanoMode mode = modes[type];
		if (op & 1)
			pSuit->ActivateMode(mode, true);
		else if (op & 2)
			pSuit->ActivateMode(mode, false);
		if (op & 4)
			pSuit->SetModeDefect(mode, true);
		else if (op & 8)
			pSuit->SetModeDefect(mode, false);
		// the mode in use was taken away: armor, as the campaign does
		if (!pSuit->IsModeActive(pSuit->GetMode()))
			pSuit->SetMode(NANOMODE_DEFENSE, true);
		Trace("SUIT< mode %d op %d -> in use %d mask %d", (int)mode, op, (int)pSuit->GetMode(),
			(int)pSuit->IsModeActive(NANOMODE_SPEED) | (int)pSuit->IsModeActive(NANOMODE_STRENGTH) << 1
			| (int)pSuit->IsModeActive(NANOMODE_CLOAK) << 2 | (int)pSuit->IsModeActive(NANOMODE_DEFENSE) << 3);
		return;
	}
	if (kind == eSync_PlayerSeat && !gEnv->bServer)
	{
		if (name && name[0])
		{
			SSeatWant& want = s_seatWant[name];
			want.vehicle = text ? text : "";
			want.seat = op;
			want.since = gEnv->pTimer->GetCurrTime();
			want.badSince = -1.0f;
			want.repaired = -100.0f;
			Trace("SEAT< %s in %s seat %d", name, want.vehicle.empty() ? "-" : want.vehicle.c_str(), op);
		}
		return;
	}
	if (kind == eSync_Usable && !gEnv->bServer)
	{
		IEntity* pEntity = entity ? gEnv->pEntitySystem->GetEntity(entity) : 0;
		if (name && name[0] && (!pEntity || strcmp(pEntity->GetName(), name)))
			pEntity = gEnv->pEntitySystem->FindEntityByName(name);
		if (!pEntity)
		{
			Trace("USABLE< %s: not found", name ? name : "");
			return;
		}
		const string before = UsableSignature(pEntity);
		ApplyUsableSignature(pEntity, text);
		const string after = UsableSignature(pEntity);
		if (after != before)
			Trace("USABLE< %s %s -> %s", pEntity->GetName(), before.c_str(), after.c_str());
		return;
	}
	if (kind == eSync_HostPlayer && !gEnv->bServer)
	{
		IEntity* pEntity = entity ? gEnv->pEntitySystem->GetEntity(entity) : 0;
		if (name && name[0] && (!pEntity || strcmp(pEntity->GetName(), name)))
			pEntity = gEnv->pEntitySystem->FindEntityByName(name);
		s_hostPlayerId = pEntity ? pEntity->GetId() : 0;
		Trace("SYNC< host player %s (%u)", pEntity ? pEntity->GetName() : "?", s_hostPlayerId);
		return;
	}
	if (kind == eSync_DebugLook && !gEnv->bServer)
	{
		IEntity* pEntity = entity ? gEnv->pEntitySystem->GetEntity(entity) : 0;
		if (name && name[0] && (!pEntity || strcmp(pEntity->GetName(), name)))
			pEntity = gEnv->pEntitySystem->FindEntityByName(name);
		if (op == 6)
		{
			IActor* pVictim = pEntity ? g_pGame->GetIGameFramework()->GetIActorSystem()->GetActor(pEntity->GetId()) : 0;
			DebugClientHit(g_pGame->GetIGameFramework()->GetClientActor(), pVictim);
			Trace("DEBUG hit %s", pEntity ? pEntity->GetName() : "?");
			return;
		}
		if (op == 5)
		{
			// debug: this machine loses its own seat but stays linked to the
			// vehicle (the state a seat mix-up left a joined player in)
			IActor* pLocal = g_pGame->GetIGameFramework()->GetClientActor();
			IVehicle* pVehicle = pLocal ? pLocal->GetLinkedVehicle() : 0;
			IVehicleSeat* pSeat = pVehicle ? pVehicle->GetSeatForPassenger(pLocal->GetEntityId()) : 0;
			if (pSeat)
			{
				pSeat->Exit(false, true);
				pLocal->LinkToVehicle(pVehicle->GetEntityId());
			}
			Trace("DEBUG break: %s linked %s seat %s", pLocal ? pLocal->GetEntity()->GetName() : "?",
				pLocal && pLocal->GetLinkedVehicle() ? pLocal->GetLinkedVehicle()->GetEntity()->GetName() : "-",
				pLocal && pLocal->GetLinkedVehicle() && pLocal->GetLinkedVehicle()->GetSeatForPassenger(pLocal->GetEntityId()) ? "yes" : "none");
			return;
		}
		if (op == 4)
		{
			// debug: "use" on it, as the use key does
			if (pEntity)
			{
				string lua;
				lua.Format("local e = System.GetEntityByName('%s'); local idx = g_gameRules:IsUsable(g_localActorId, e.id); "
					"System.Log('[Coop] debug client use slot '..tostring(idx)); g_localActor:UseEntity(e.id, idx, true); g_localActor:UseEntity(e.id, idx, false)",
					pEntity->GetName());
				gEnv->pScriptSystem->ExecuteBuffer(lua.c_str(), lua.size(), "coop debug use");
			}
			Trace("DEBUG use %s", pEntity ? pEntity->GetName() : "?");
			return;
		}
		s_debugLookTarget = pEntity ? pEntity->GetId() : 0;
		s_debugLookCenter = op == 2;
		s_debugLookPoint = op == 3 && text && sscanf(text, "%f %f %f", &s_debugLookAt.x, &s_debugLookAt.y, &s_debugLookAt.z) == 3;
		s_debugLookUntil = gEnv->pTimer->GetCurrTime() + f;
		Trace("DEBUG look at %s for %.0f s", pEntity ? pEntity->GetName() : "?", f);
		return;
	}
	if (kind == eSync_AIState && !gEnv->bServer)
	{
		IEntity* pEntity = entity ? gEnv->pEntitySystem->GetEntity(entity) : 0;
		if (name && name[0] && (!pEntity || strcmp(pEntity->GetName(), name)))
			pEntity = gEnv->pEntitySystem->FindEntityByName(name);
		if (!pEntity)
		{
			Trace("AISTATE< %s: not found", name ? name : "");
			return;
		}
		std::map<EntityId, int>::iterator it = s_aiStateMirror.find(pEntity->GetId());
		if (it != s_aiStateMirror.end() && (it->second & 3) != (op & 3))
			Trace("AISTATE< %s alert %d->%d hostile=%d", pEntity->GetName(), it->second & 3, op & 3, (op & eAIState_Hostile) ? 1 : 0);
		s_aiStateMirror[pEntity->GetId()] = op;
		return;
	}
	if (kind == eSync_Progress && !gEnv->bServer)
	{
		CHUD* pHUD = g_pGame->GetHUD();
		if (!pHUD)
			return;
		s_applyingSync = true;
		if (op == 0)
			pHUD->ShowProgress(-1);
		else if (op == 1)
		{
			pHUD->ShowProgress(0, true, type & 0xfff, (type >> 12) & 0xfff, text ? text : "", ((type >> 24) & 1) != 0, ((type >> 25) & 1) != 0);
			pHUD->ShowProgress((int)f);
		}
		else
			pHUD->ShowProgress((int)f);
		s_applyingSync = false;
		Trace("SYNC< progress bar %s %d %s", op == 0 ? "hide" : op == 1 ? "show" : "set", (int)f, text ? text : "");
		return;
	}
	if (kind == eSync_Hide && !gEnv->bServer)
	{
		IEntity* pEntity = entity ? gEnv->pEntitySystem->GetEntity(entity) : 0;
		if (name && name[0] && (!pEntity || strcmp(pEntity->GetName(), name)))
			pEntity = gEnv->pEntitySystem->FindEntityByName(name);
		if (!pEntity)
			return;
		if (pEntity->IsHidden() != (op != 0))
		{
			s_applyingSync = true;
			pEntity->Hide(op != 0);
			s_applyingSync = false;
		}
		return;
	}
	if (kind == eSync_TimeOfDay && !gEnv->bServer)
	{
		ITimeOfDay* pTOD = gEnv->p3DEngine->GetTimeOfDay();
		if (!pTOD)
			return;
		ITimeOfDay::SAdvancedInfo info;
		pTOD->GetAdvancedInfo(info);
		float speed = info.fAnimSpeed, start = info.fStartTime, end = info.fEndTime;
		if (text)
			sscanf(text, "%f %f %f", &speed, &start, &end);
		if (speed != info.fAnimSpeed || start != info.fStartTime || end != info.fEndTime)
		{
			Trace("SYNC< time of day speed %.4f -> %.4f range %.2f-%.2f", info.fAnimSpeed, speed, start, end);
			info.fAnimSpeed = speed;
			info.fStartTime = start;
			info.fEndTime = end;
			pTOD->SetAdvancedInfo(info);
		}
		const float hour = pTOD->GetTime();
		if (fabsf(hour - f) > 0.02f)
		{
			Trace("SYNC< time of day %.3f -> %.3f", hour, f);
			if (fabsf(hour - f) > 0.25f)
				CryLogAlways("[CoopTOD] time of day set to the host's %02d:%02d", (int)f, (int)((f - (int)f) * 60.0f));
			pTOD->SetTime(f, true);
		}
		return;
	}
	if (kind == eSync_Sequence && !gEnv->bServer && gEnv->pMovieSystem)
	{
		IAnimSequence* pSeq = gEnv->pMovieSystem->FindSequence(name);
		if (!pSeq)
		{
			Trace("SYNC< cutscene %s: no such sequence", name);
			return;
		}
		const bool playing = gEnv->pMovieSystem->IsPlaying(pSeq);
		if (op == eSeq_Stop)
		{
			if (playing)
				gEnv->pMovieSystem->StopSequence(pSeq);
			Trace("SYNC< cutscene stop %s (was playing=%d)", name, (int)playing);
			CryLogAlways("[CoopCutscene] host stopped %s", name);
			return;
		}
		if (!playing)
		{
			gEnv->pMovieSystem->PlaySequence(pSeq, true);
			Trace("SYNC< cutscene %s %s at %.2f", op == eSeq_Start ? "start" : "join", name, f);
			CryLogAlways("[CoopCutscene] host plays %s (t=%.1f)", name, f);
		}
		// a small drift is normal (network delay): only real gaps are corrected
		const float t = gEnv->pMovieSystem->GetPlayingTime(pSeq);
		if (fabsf(t - f) > (op == eSeq_Time ? 1.5f : 0.5f))
		{
			gEnv->pMovieSystem->SetPlayingTime(pSeq, f);
			Trace("SYNC< cutscene %s time %.2f -> %.2f", name, t, f);
		}
	}
}

// ---------------------------------------------------------------------------
// AI shots: the server tells the clients, which fire a local copy of the
// soldier's weapon (muzzle flash, sound, tracer, bullet impacts; the damage
// is the server's)
namespace
{
	std::map<EntityId, float> s_lastShotSent;
}

void CoopAI::OnAIShot(EntityId shooterId, IEntity* pWeapon, bool mounted, const Vec3& pos, const Vec3& dir)
{
	if (!pWeapon || !IsCoopSession())
		return;
	IActor* pShooter = g_pGame->GetIGameFramework()->GetIActorSystem()->GetActor(shooterId);
	if (!pShooter || pShooter->IsPlayer())
		return;
	++s_totalAIShots;
	// automatic fire: at most 12 shots a second per soldier go out
	const float now = gEnv->pTimer->GetCurrTime();
	float& last = s_lastShotSent[shooterId];
	if (now - last < 0.08f)
		return;
	last = now;
	static int s_sent = 0;
	static int s_sentMounted = 0;
	if (++s_sent <= 20 || (s_sent % 100) == 0 || (mounted && ++s_sentMounted <= 10))
		Trace("SHOT> #%d %s fires %s%s%s", s_sent, pShooter->GetEntity()->GetName(), pWeapon->GetClass()->GetName(),
			mounted ? " mounted " : "", mounted ? pWeapon->GetName() : "");
	if (CGameRules* pRules = g_pGame->GetGameRules())
		pRules->CoopSendShot(shooterId, pWeapon->GetClass()->GetName(), pos, dir,
			mounted ? pWeapon->GetId() : 0, mounted ? pWeapon->GetName() : 0);
}

namespace
{
	// client: a mounted gun (vehicle weapon, emplacement) fired on the server:
	// the same gun fires here (muzzle flash, sound, tracer, bullet impacts)
	void MirrorMountedShot(EntityId shooterId, const char* weaponClass, const Vec3& pos, const Vec3& dir, EntityId mountedId, const char* mountedName)
	{
		static int s_received = 0;
		const bool log = ++s_received <= 10 || (s_received % 200) == 0;
		IEntity* pEntity = mountedId ? gEnv->pEntitySystem->GetEntity(mountedId) : 0;
		if (!pEntity || strcmp(pEntity->GetName(), mountedName) || strcmp(pEntity->GetClass()->GetName(), weaponClass))
			pEntity = gEnv->pEntitySystem->FindEntityByName(mountedName);
		IItem* pItem = pEntity ? g_pGame->GetIGameFramework()->GetIItemSystem()->GetItem(pEntity->GetId()) : 0;
		CWeapon* pWeapon = pItem ? static_cast<CWeapon*>(pItem->GetIWeapon()) : 0;
		if (!pWeapon)
		{
			if (log)
				CoopAI::Trace("SHOT< #%d mounted %s (%s, id %u): %s", s_received, mountedName, weaponClass, mountedId, pEntity ? "not a weapon" : "not found");
			return;
		}
		pWeapon->NetShootEx(pos, dir, Vec3(0, 0, 0), pos + dir * 200.0f, 1.0f, 0);
		if (log)
		{
			IEntity* pShooter = gEnv->pEntitySystem->GetEntity(shooterId);
			CoopAI::Trace("SHOT< #%d mounted %s fired by %s (id %u -> %u)", s_received, mountedName, pShooter ? pShooter->GetName() : "?",
				mountedId, pEntity->GetId());
		}
	}
}

void CoopAI::OnShotMirror(EntityId shooterId, const char* weaponClass, const Vec3& pos, const Vec3& dir, EntityId mountedId, const char* mountedName)
{
	if (gEnv->bServer || !weaponClass || !weaponClass[0])
		return;
	if (mountedName && mountedName[0])
	{
		MirrorMountedShot(shooterId, weaponClass, pos, dir, mountedId, mountedName);
		return;
	}
	IGameFramework* pFramework = g_pGame->GetIGameFramework();
	CActor* pActor = static_cast<CActor*>(pFramework->GetIActorSystem()->GetActor(shooterId));
	static int s_received = 0;
	if (++s_received <= 20 || (s_received % 100) == 0)
		Trace("SHOT< #%d from %u (%s) %s hp=%d", s_received, shooterId, pActor ? pActor->GetEntity()->GetName() : "no actor", weaponClass, pActor ? pActor->GetHealth() : -1);
	const bool log = s_received <= 10;
	if (!pActor || pActor->GetHealth() <= 0 || !pActor->GetInventory())
	{
		if (log)
			Trace("SHOT mirror: %s: no actor/dead/no inventory (inventory=%d)", weaponClass, pActor && pActor->GetInventory() ? 1 : 0);
		return;
	}
	IEntityClass* pClass = gEnv->pEntitySystem->GetClassRegistry()->FindClass(weaponClass);
	if (!pClass)
	{
		if (log)
			Trace("SHOT mirror: no class %s", weaponClass);
		return;
	}
	EntityId itemId = pActor->GetInventory()->GetItemByClass(pClass);
	if (itemId && log)
	{
		IEntity* pExisting = gEnv->pEntitySystem->GetEntity(itemId);
		IItem* pExistingItem = pFramework->GetIItemSystem()->GetItem(itemId);
		Trace("SHOT mirror: %s already has %s (%s) hidden=%d owner=%u current=%u", pActor->GetEntity()->GetName(), weaponClass,
			pExisting ? pExisting->GetName() : "?", pExisting ? (int)pExisting->IsHidden() : -1,
			pExistingItem ? pExistingItem->GetOwnerId() : 0, pActor->GetCurrentItemId());
	}
	if (!itemId)
	{
		// the soldier's weapon, only on this machine
		SEntitySpawnParams params;
		params.pClass = pClass;
		string name;
		name.Format("%s_coop_%s", pActor->GetEntity()->GetName(), weaponClass);
		params.sName = name.c_str();
		params.nFlags = ENTITY_FLAG_CLIENT_ONLY | ENTITY_FLAG_NO_SAVE;
		params.vPosition = pActor->GetEntity()->GetWorldPos();
		IEntity* pItemEntity = gEnv->pEntitySystem->SpawnEntity(params);
		CItem* pNew = pItemEntity ? static_cast<CItem*>(pFramework->GetIItemSystem()->GetItem(pItemEntity->GetId())) : 0;
		if (!pNew)
		{
			Trace("SHOT mirror: could not create %s for %s", weaponClass, pActor->GetEntity()->GetName());
			return;
		}
		pNew->PickUp(shooterId, false, true, false);
		itemId = pItemEntity->GetId();
		Trace("SHOT mirror: %s holds a local %s", pActor->GetEntity()->GetName(), weaponClass);
	}
	if (pActor->GetCurrentItemId() != itemId)
		pActor->SelectItem(itemId, false);
	CItem* pItem = static_cast<CItem*>(pFramework->GetIItemSystem()->GetItem(itemId));
	CWeapon* pWeapon = pItem ? static_cast<CWeapon*>(pItem->GetIWeapon()) : 0;
	if (!pWeapon)
	{
		if (log)
			Trace("SHOT mirror: %s is not a weapon", weaponClass);
		return;
	}
	pWeapon->NetShootEx(pos, dir, Vec3(0, 0, 0), pos + dir * 200.0f, 1.0f, 0);
	if (log)
		Trace("SHOT mirror: %s fired %s (current=%u selected=%d hidden=%d)", pActor->GetEntity()->GetName(), weaponClass,
			pActor->GetCurrentItemId(), (int)pItem->IsSelected(), (int)pItem->GetEntity()->IsHidden());
}

void CoopAI::OnVoiceMirror(const char* name, const Vec3& pos, uint32 flags)
{
	if (gEnv->bServer || !gEnv->pSoundSystem || !name || !name[0])
		return;
	const bool is3D = (flags & FLAG_SOUND_3D) != 0 && !pos.IsZero();
	_smart_ptr<ISound> pSound = gEnv->pSoundSystem->CreateSound(name, FLAG_SOUND_VOICE | (is3D ? FLAG_SOUND_3D : FLAG_SOUND_2D));
	if (!pSound)
	{
		Trace("VOICE mirror failed %s", name);
		return;
	}
	if (is3D)
		pSound->SetPosition(pos);
	s_playingMirroredVoice = true;
	pSound->Play();
	s_playingMirroredVoice = false;
	Trace("VOICE mirror %s pos=(%.1f,%.1f,%.1f)", name, pos.x, pos.y, pos.z);
}

namespace
{
	// A monitor's video (the story's Entity:VideoPlayer) on a client. The
	// engine's node crashes a client when it runs there, so the client does
	// what it does itself, checking each step: the entity's material, the
	// sub-material, the texture slot, and the video player behind it.
	// port: 4 start, 5 stop, 6 pause, 7 unpause
	IVideoPlayer* s_mirrorVideo = 0;
	float s_mirrorVideoWatch = 0.0f, s_mirrorVideoTimer = 0.0f;

	// diagnostics: what the started video does in its first seconds
	void UpdateMirrorVideoTrace(float frameTime)
	{
		if (!s_mirrorVideo || s_mirrorVideoWatch <= 0.0f)
			return;
		s_mirrorVideoWatch -= frameTime;
		s_mirrorVideoTimer += frameTime;
		if (s_mirrorVideoTimer < 2.0f)
			return;
		s_mirrorVideoTimer = 0.0f;
		CoopAI::Trace("VIDEO status %d per frame %d", (int)s_mirrorVideo->GetStatus(), (int)s_mirrorVideo->IsPerFrameUpdateEnabled());
	}

	void MirrorVideo(IEntity* pEntity, int slot, int subMtl, int texSlot, uint32 port)
	{
		IEntityRenderProxy* pRender = pEntity ? static_cast<IEntityRenderProxy*>(pEntity->GetProxy(ENTITY_PROXY_RENDER)) : 0;
		IMaterial* pMtl = pRender ? pRender->GetRenderMaterial(slot) : 0;
		if (pMtl && subMtl >= 0 && subMtl < pMtl->GetSubMtlCount())
			pMtl = pMtl->GetSubMtl(subMtl);
		IRenderShaderResources* pRes = pMtl ? pMtl->GetShaderItem().m_pShaderResources : 0;
		SEfResTexture* pTex = pRes ? pRes->GetTexture(texSlot) : 0;
		IDynTextureSource* pSource = pTex ? pTex->m_Sampler.m_pDynTexSource : 0;
		void* pPlayer = 0;
		IDynTextureSource::EDynTextureSource kind = IDynTextureSource::DTS_I_FLASHPLAYER;
		if (pSource)
			pSource->GetDynTextureSource(pPlayer, kind);
		IVideoPlayer* pVideo = (pPlayer && kind == IDynTextureSource::DTS_I_VIDEOPLAYER) ? static_cast<IVideoPlayer*>(pPlayer) : 0;
		if (pVideo)
		{
			// the frames advance only with the per-frame update on (the
			// engine's node turns it on: a plain Start showed a still frame)
			if (port == 4)
			{
				pVideo->EnablePerFrameUpdate(true);
				pVideo->Start();
				s_mirrorVideo = pVideo;
				s_mirrorVideoWatch = 20.0f;
			}
			else if (port == 5)
			{
				pVideo->Stop();
				pVideo->EnablePerFrameUpdate(false);
			}
			else if (port == 6 || port == 7)
				pVideo->Pause(port == 6);
		}
		CoopAI::Trace("VIDEO %s port %u: material %s texture %s source %s -> %s (status %d, per frame %d)", pEntity ? pEntity->GetName() : "?", port, pMtl ? pMtl->GetName() : "-",
			pTex ? pTex->m_Name.c_str() : "-", pSource ? (kind == IDynTextureSource::DTS_I_VIDEOPLAYER ? "video" : "flash") : "-", pVideo ? "done" : "nothing to play",
			pVideo ? (int)pVideo->GetStatus() : -1, pVideo ? (int)pVideo->IsPerFrameUpdateEnabled() : -1);
	}
}

void CoopAI::OnFlowMirror(const char* type, uint32 key, uint32 node, uint32 port, uint32 entity, const char* values)
{
	if (gEnv->bServer || !gEnv->pFlowSystem || !type || !type[0])
		return;
	CoopAI::Trace("FLOW< %s node=%u port=%u entity=%u values=%s", type, node, port, entity, values ? values : "");
	static int s_logged = 0;
	if (s_logged < 40)
	{
		++s_logged;
		CryLogAlways("[CoopFlow] mirror %s port %u", type, port);
	}
	if (!stricmp(type, "Entity:VideoPlayer"))
	{
		// entityId|Slot|SubMtlId|TexSlot|Start|Stop|Pause|UnPause
		int id = 0, slot = 0, subMtl = 0, texSlot = 0;
		char buf[256];
		_snprintf(buf, sizeof(buf), "%s", values ? values : "");
		buf[sizeof(buf) - 1] = 0;
		for (char* c = buf; *c; ++c)
			if (*c == FLOW_SEP)
				*c = ' ';
		sscanf(buf, "%d %d %d %d", &id, &slot, &subMtl, &texSlot);
		MirrorVideo(entity ? gEnv->pEntitySystem->GetEntity(entity) : 0, slot, subMtl, texSlot, port);
		return;
	}
	if (!s_mirrorGraph)
	{
		s_mirrorGraph = gEnv->pFlowSystem->CreateFlowGraph();
		if (!s_mirrorGraph)
			return;
		s_mirrorGraph->SetEnabled(true);
		s_mirrorGraph->SetActive(true);
	}
	IFlowGraph* pGraph = s_mirrorGraph.get();
	std::pair<uint32, uint32> k(key, node);
	std::map<std::pair<uint32, uint32>, TFlowNodeId>::iterator it = s_mirrorNodes.find(k);
	TFlowNodeId id;
	if (it == s_mirrorNodes.end())
	{
		char name[64];
		_snprintf(name, sizeof(name), "coop_%08x_%u", key, node);
		name[sizeof(name)-1] = 0;
		id = pGraph->CreateNode(gEnv->pFlowSystem->GetTypeId(type), name);
		if (id == InvalidFlowNodeId)
		{
			CryLogAlways("[CoopFlow] cannot create mirror node of type %s", type);
			return;
		}
		s_mirrorNodes[k] = id;
	}
	else
		id = it->second;
	if (entity)
		pGraph->SetEntityId(id, entity);

	SFlowNodeConfig cfg;
	pGraph->GetNodeConfiguration(id, cfg);
	int count = 0;
	while (cfg.pInputPorts && cfg.pInputPorts[count].name)
		++count;
	std::vector<TFlowInputData> inputs;
	inputs.resize(count);
	string all(values ? values : "");
	size_t start = 0;
	for (int n = 0; n < count; ++n)
	{
		size_t end = all.find(FLOW_SEP, start);
		if (end == string::npos)
			end = all.size();
		string v = start <= all.size() ? all.substr(start, end - start) : string();
		start = end + 1;
		TFlowInputData d = cfg.pInputPorts[n].defaultData;
		d.SetValueWithConversion(v);
		pGraph->SetInputValue(id, n, d);
		inputs[n] = d;
		inputs[n].ClearUserFlag();
	}
	IFlowNodeData* pData = pGraph->GetNodeData(id);
	IFlowNode* pNode = pData ? pData->GetNode() : 0;
	if (!pNode || (int)port >= count)
		return;
	IFlowNode::SActivationInfo info(pGraph, id, 0, count ? &inputs[0] : 0);
	info.pEntity = entity ? gEnv->pEntitySystem->GetEntity(entity) : 0;
	// a node made for an entity never runs without one (the engine's nodes
	// read it unchecked)
	if ((cfg.nFlags & EFLN_TARGET_ENTITY) && !info.pEntity)
	{
		CoopAI::Trace("FLOW< %s skipped: entity %u not here", type, entity);
		return;
	}
	// the material and video nodes work on the entity's render proxy, which
	// a client's copy of a cutscene prop may not have (yet)
	if ((!strnicmp(type, "Entity:Material", 15) || !strnicmp(type, "Entity:VideoPlayer", 18)) && (!info.pEntity || !info.pEntity->GetProxy(ENTITY_PROXY_RENDER)))
	{
		CoopAI::Trace("FLOW< %s skipped: %s has no render proxy here", type, info.pEntity ? info.pEntity->GetName() : "?");
		return;
	}
	// the engine's entity nodes keep the entity they got by eFE_SetEntityId
	// (a graph sends it on its own update, too late for this activation)
	if (info.pEntity)
		pNode->ProcessEvent(IFlowNode::eFE_SetEntityId, &info);
	if (s_mirrorInitialized.insert(id).second)
		pNode->ProcessEvent(IFlowNode::eFE_Initialize, &info);
	inputs[port].SetUserFlag(true);
	pNode->ProcessEvent(IFlowNode::eFE_Activate, &info);
}

void CoopAI::SendFlowHistory(int channelId)
{
	CGameRules* pRules = g_pGame ? g_pGame->GetGameRules() : 0;
	if (!pRules || !gEnv->bServer)
		return;
	for (size_t i = 0; i < s_flowHistory.size(); ++i)
	{
		const SFlowMsg& m = s_flowHistory[i];
		pRules->CoopSendFlow(m.type.c_str(), m.key, m.node, m.port, m.entity, m.values.c_str(), channelId);
	}
	CryLogAlways("[CoopFlow] replayed %d HUD/state flow activations to channel %d", (int)s_flowHistory.size(), channelId);
	// the state as it is now, after the replayed history
	SendSyncHistory(channelId);
}
