// Crysis Coop: downed players and reviving them (see CoopRevive.h).
#include "StdAfx.h"
#include "CoopRevive.h"
#include "CoopAI.h"
#include "Game.h"
#include "GameActions.h"
#include "GameRules.h"
#include "IPlayerInput.h"
#include "Player.h"
#include "IUIDraw.h"

#include <vector>

namespace
{
	const int SYNC_REVIVE = 13;         // CoopAI's sync kind for the revive state
	const float USE_RANGE = 4.0f;       // m from a downed teammate for the use key (the server allows a little more)

	ICVar* s_pAutoRespawn = 0;

	struct SRevive
	{
		EntityId target, reviver;
		float start, duration;
	};
	std::vector<SRevive> s_revives;     // running on the server now
	float s_allDownUntil = 0.0f;        // everybody is down: the checkpoint loads then
	EntityId s_holding = 0;             // the local player holds the use key on this teammate
	int s_white = -1;

	float Now()
	{
		return gEnv->pTimer->GetAsyncCurTime();
	}

	IActor* LocalActor()
	{
		return g_pGame ? g_pGame->GetIGameFramework()->GetClientActor() : 0;
	}

	// a player who is dead and not a spectator waits for a teammate
	bool IsDown(IActor* pActor)
	{
		return pActor && pActor->IsPlayer() && pActor->GetHealth() <= 0 && static_cast<CActor*>(pActor)->GetSpectatorMode() == 0;
	}

	const char* Name(EntityId id)
	{
		IEntity* pEntity = id ? gEnv->pEntitySystem->GetEntity(id) : 0;
		return pEntity ? pEntity->GetName() : "?";
	}

	// the nearest downed teammate within range of the player (0: none);
	// pDistance: how far he is
	EntityId NearestDown(IActor* pPlayer, float range, float* pDistance = 0)
	{
		const Vec3 pos = pPlayer->GetEntity()->GetWorldPos();
		EntityId best = 0;
		float bestDist = range;
		IActorIteratorPtr it = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = it->Next())
		{
			if (pActor == pPlayer || !IsDown(pActor))
				continue;
			const float d = pActor->GetEntity()->GetWorldPos().GetDistance(pos);
			if (d <= bestDist)
			{
				bestDist = d;
				best = pActor->GetEntityId();
			}
		}
		if (pDistance)
			*pDistance = bestDist;
		return best;
	}

	const SRevive* FindRevive(EntityId target, EntityId reviver)
	{
		for (size_t i = 0; i < s_revives.size(); ++i)
			if ((target && s_revives[i].target == target) || (reviver && s_revives[i].reviver == reviver))
				return &s_revives[i];
		return 0;
	}

	// players go by name between the machines: their entity ids differ
	EntityId IdOfName(const char* name)
	{
		IEntity* pEntity = name && name[0] ? gEnv->pEntitySystem->FindEntityByName(name) : 0;
		return pEntity ? pEntity->GetId() : 0;
	}

	void SendInput(IActor* pLocal, EntityId target, bool press)
	{
		if (gEnv->bServer)
			CoopRevive::OnInput(pLocal->GetEntityId(), target, press);
		else if (CGameRules* pRules = g_pGame->GetGameRules())
			pRules->CoopSendSyncToServer(SYNC_REVIVE, press ? 1 : 2, 0, Name(target), "", 0, 0.0f);
	}

	// ---- drawing: text and bars centered on the screen, in IUIDraw's
	// 800x600 space (a centered image stays centered on any aspect)
	void CenterText(IUIDraw* pUI, IFFont* pFont, float y, float size, const char* text, float r, float g, float b, float a = 1.0f)
	{
		pUI->DrawText(pFont, 0, y, size, size, text, a, r, g, b,
			UIDRAWHORIZONTAL_CENTER, UIDRAWVERTICAL_TOP, UIDRAWHORIZONTAL_CENTER, UIDRAWVERTICAL_TOP);
	}

	void Bar(IUIDraw* pUI, float y, float fraction, float r, float g, float b)
	{
		if (s_white < 0)
			s_white = pUI->CreateTexture("Textures/Defaults/White.dds");
		if (s_white <= 0)
			return;
		const float w = 220.0f, h = 10.0f;
		fraction = fraction < 0.0f ? 0.0f : fraction > 1.0f ? 1.0f : fraction;
		pUI->DrawImage(s_white, 400.0f - w * 0.5f - 2, y - 2, w + 4, h + 4, 0.0f, 0.02f, 0.05f, 0.03f, 0.85f);
		if (fraction > 0.0f)
			pUI->DrawImage(s_white, 400.0f - w * 0.5f, y, w * fraction, h, 0.0f, r, g, b, 0.95f);
	}
}

namespace
{
	// testing: coop_test_kill <player name> (server) kills a player
	void CmdTestKill(IConsoleCmdArgs* pArgs)
	{
		CGameRules* pRules = gEnv->bServer ? g_pGame->GetGameRules() : 0;
		IEntity* pEntity = pArgs->GetArgCount() > 1 ? gEnv->pEntitySystem->FindEntityByName(pArgs->GetArg(1)) : 0;
		if (pRules && pEntity && pEntity->GetScriptTable())
			Script::CallMethod(pRules->GetEntity()->GetScriptTable(), "KillPlayer", pEntity->GetScriptTable());
	}

	// testing: coop_test_revive press|release: the local player's use key,
	// through the player's input like the key; checkpoint: a story
	// checkpoint was reached (server)
	void CmdTestRevive(IConsoleCmdArgs* pArgs)
	{
		const char* what = pArgs->GetArgCount() > 1 ? pArgs->GetArg(1) : "";
		if (!stricmp(what, "checkpoint"))
		{
			CoopRevive::OnCheckpoint();
			return;
		}
		CPlayer* pPlayer = static_cast<CPlayer*>(LocalActor());
		if (pPlayer && !stricmp(what, "near"))
		{
			// the local player walks up to the nearest downed teammate
			const EntityId id = NearestDown(pPlayer, 10000.0f);
			if (IActor* pDown = id ? g_pGame->GetIGameFramework()->GetIActorSystem()->GetActor(id) : 0)
			{
				const Vec3 body = pDown->GetEntity()->GetWorldPos();
				Vec3 away = pPlayer->GetEntity()->GetWorldPos() - body;
				away.z = 0.0f;
				away = away.GetLengthSquared() > 0.01f ? away.GetNormalized() : Vec3(1, 0, 0);
				pPlayer->GetEntity()->SetPos(body + away * 1.5f + Vec3(0, 0, 0.2f));
			}
			return;
		}
		if (pPlayer && pPlayer->GetPlayerInput())
			pPlayer->GetPlayerInput()->OnAction(g_pGame->Actions().use, !stricmp(what, "release") ? eAAM_OnRelease : eAAM_OnPress, 1.0f);
	}
}

void CoopRevive::Init()
{
	if (!gEnv->pConsole || s_pAutoRespawn)
		return;
	gEnv->pConsole->AddCommand("coop_test_kill", CmdTestKill, 0, "Crysis Coop testing: coop_test_kill <player name> kills him (server)");
	gEnv->pConsole->AddCommand("coop_test_revive", CmdTestRevive, 0,
		"Crysis Coop testing: press|release the local player's use key; checkpoint: as if a story checkpoint was reached (server)");
	// the server's rule: synced to the clients (their HUD follows it)
	s_pAutoRespawn = gEnv->pConsole->RegisterInt("coop_auto_respawn", 0, VF_DUMPTODISK,
		"Crysis Coop: 0 = a dead player waits until a teammate revives him (holds the use key next to him), "
		"everybody down = back to the last checkpoint; 1 = the dead come back by themselves after a few seconds");
}

bool CoopRevive::IsReviveMode()
{
	return CoopAI::IsCoopSession() && (!s_pAutoRespawn || s_pAutoRespawn->GetIVal() == 0);
}

bool CoopRevive::OnUse(CPlayer* pPlayer, int activationMode)
{
	if (!IsReviveMode() || !pPlayer || pPlayer->GetHealth() <= 0 || pPlayer->GetLinkedVehicle())
		return false;
	if (activationMode == eAAM_OnPress)
	{
		float distance = 0.0f;
		const EntityId target = NearestDown(pPlayer, USE_RANGE, &distance);
		if (!target)
			return false;
		CryLogAlways("[CoopRevive] use key on %s (%.1f m)", Name(target), distance);
		s_holding = target;
		SendInput(pPlayer, target, true);
		return true;
	}
	if (activationMode == eAAM_OnRelease && s_holding)
	{
		SendInput(pPlayer, s_holding, false);
		s_holding = 0;
		return true;
	}
	return false;
}

void CoopRevive::OnInput(EntityId reviver, EntityId target, bool press)
{
	CGameRules* pRules = gEnv->bServer ? g_pGame->GetGameRules() : 0;
	IScriptTable* pScript = pRules ? pRules->GetEntity()->GetScriptTable() : 0;
	if (pScript)
		Script::CallMethod(pScript, "CoopReviveInput", ScriptHandle(reviver), ScriptHandle(target), press);
}

void CoopRevive::OnState(int op, EntityId target, EntityId reviver, float seconds, bool fromScript)
{
	if (fromScript && gEnv->bServer && gEnv->bMultiplayer)
		if (CGameRules* pRules = g_pGame->GetGameRules())
			pRules->CoopSendSync(SYNC_REVIVE, op, 0, target ? Name(target) : "", reviver ? Name(reviver) : "", 0, seconds, 0);
	for (size_t i = 0; i < s_revives.size(); ++i)
		if (s_revives[i].target == target)
		{
			s_revives.erase(s_revives.begin() + i);
			break;
		}
	if (op == 1)
	{
		SRevive r = { target, reviver, Now(), seconds > 0.1f ? seconds : 0.1f };
		s_revives.push_back(r);
	}
	else if (op == 3 && target == s_holding)
		s_holding = 0;
	else if (op == 4)
		s_allDownUntil = Now() + seconds;
	else if (op == 5)
		s_allDownUntil = 0.0f;
}

void CoopRevive::OnStateFromServer(int op, const char* target, const char* reviver, float seconds)
{
	OnState(op, IdOfName(target), IdOfName(reviver), seconds, false);
}

void CoopRevive::OnInputFromClient(EntityId reviver, const char* target, bool press)
{
	if (const EntityId id = IdOfName(target))
		OnInput(reviver, id, press);
}

void CoopRevive::OnCheckpoint()
{
	CGameRules* pRules = gEnv->bServer && IsReviveMode() ? g_pGame->GetGameRules() : 0;
	IScriptTable* pScript = pRules ? pRules->GetEntity()->GetScriptTable() : 0;
	if (pScript)
		Script::CallMethod(pScript, "CoopReviveAllDowned", "checkpoint");
}

void CoopRevive::RenderHud(IUIDraw* pUIDraw, IFFont* pFont)
{
	IActor* pLocal = LocalActor();
	if (!pUIDraw || !pFont || !pLocal || !IsReviveMode())
	{
		s_revives.clear();
		s_allDownUntil = 0.0f;
		return;
	}
	const float now = Now();
	string text;
	if (s_allDownUntil > now)
	{
		CenterText(pUIDraw, pFont, 240, 24, "EVERYBODY IS DOWN", 1.0f, 0.45f, 0.35f);
		text.Format("Back to the last checkpoint in %d", (int)(s_allDownUntil - now) + 1);
		CenterText(pUIDraw, pFont, 272, 16, text.c_str(), 0.95f, 0.9f, 0.8f);
		return;
	}
	const EntityId self = pLocal->GetEntityId();
	if (IsDown(pLocal))
	{
		if (const SRevive* r = FindRevive(self, 0))
		{
			text.Format("%s is reviving you", Name(r->reviver));
			CenterText(pUIDraw, pFont, 400, 18, text.c_str(), 0.75f, 1.0f, 0.75f);
			Bar(pUIDraw, 428, (now - r->start) / r->duration, 0.45f, 0.95f, 0.5f);
		}
		else
		{
			CenterText(pUIDraw, pFont, 380, 24, "YOU ARE DOWN", 1.0f, 0.45f, 0.35f);
			CenterText(pUIDraw, pFont, 412, 16, "A teammate can revive you: he holds F next to you", 0.95f, 0.9f, 0.8f);
		}
		return;
	}
	if (pLocal->GetHealth() <= 0)
		return;
	if (const SRevive* r = FindRevive(0, self))
	{
		text.Format("Reviving %s", Name(r->target));
		CenterText(pUIDraw, pFont, 400, 18, text.c_str(), 0.75f, 1.0f, 0.75f);
		Bar(pUIDraw, 428, (now - r->start) / r->duration, 0.45f, 0.95f, 0.5f);
	}
	else if (EntityId near = NearestDown(pLocal, USE_RANGE))
	{
		text.Format("Hold F to revive %s", Name(near));
		CenterText(pUIDraw, pFont, 400, 18, text.c_str(), 0.95f, 1.0f, 0.95f);
	}
	// every downed teammate, and how far
	float y = 70.0f;
	const Vec3 pos = pLocal->GetEntity()->GetWorldPos();
	IActorIteratorPtr it = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
	while (IActor* pActor = it->Next())
	{
		if (pActor == pLocal || !IsDown(pActor))
			continue;
		text.Format("%s is down - %d m", pActor->GetEntity()->GetName(), (int)pActor->GetEntity()->GetWorldPos().GetDistance(pos));
		CenterText(pUIDraw, pFont, y, 15, text.c_str(), 1.0f, 0.55f, 0.45f);
		y += 20.0f;
	}
}
