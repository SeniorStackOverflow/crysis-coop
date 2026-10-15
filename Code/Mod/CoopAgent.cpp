// Crysis Coop: the AI companion (see CoopAgent.h).
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")

#include "StdAfx.h"
#include <StringUtils.h>
#include "CoopAgent.h"
#include "CoopAI.h"
#include "Game.h"
#include "GameActions.h"
#include "GameRules.h"
#include "CoopSave.h"
#include "CoopRevive.h"
#include "HUD/HUD.h"
#include "OffHand.h"
#include "Item.h"
#include "IPlayerInput.h"
#include "Player.h"
#include "NanoSuit.h"
#include "VehicleMovementBase.h"
#include "IVehicleSystem.h"
#include "IViewSystem.h"
#include "IItemSystem.h"
#include "IWeapon.h"
#include "IWorldQuery.h"
#include "ILevelSystem.h"
#include "I3DEngine.h"
#include "INetworkService.h"
#include "ISurfaceType.h"

#include <algorithm>
#include <deque>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <vector>

extern void* g_hInst;
HHOOK CoopHookWindowsOf(DWORD threadId);    // GameDll.cpp
bool CoopEncodeJpeg(const unsigned char* rgb, int width, int height, int quality, std::vector<unsigned char>& out);    // CoopJpeg.cpp

namespace
{
	ICVar* s_pCompanion = 0;     // host: the menu's "AI companion"
	ICVar* s_pAgent = 0;         // this game is the companion
	ICVar* s_pPort = 0;          // the companion's bridge port
	ICVar* s_pJoin = 0;          // the companion joins 127.0.0.1:<this>
	ICVar* s_pFps = 0;           // the companion's frame limit
	ICVar* s_pParent = 0;        // the host's process: the companion ends with it
	ICVar* s_pName = 0;          // the companion's player name
	ICVar* s_pFrameLog = 0;      // testing: this game's frames a second in the log

	const char* const COMPANION_SETTINGS =
		// a light game: small, silent, simple; it needs no more to play
		" +sys_spec 1 +r_Width 800 +r_Height 450 +r_Fullscreen 0 +s_SoundEnable 0 +s_MusicEnable 0"
		" +e_ViewDistRatio 40 +e_ViewDistRatioVegetation 30 +e_Particles 0 +r_TexResolution 2"
		" +coop_test_free_cursor 1"
		// no microphone: the voice chat opened it, and Windows' "microphone
		// in use" icon came and went in the host's taskbar
		" +cl_voice_recording 0 +net_enable_voice_chat 0";

	float Now() { return gEnv->pTimer->GetAsyncCurTime(); }

	CPlayer* LocalPlayer()
	{
		IActor* pActor = g_pGame ? g_pGame->GetIGameFramework()->GetClientActor() : 0;
		return pActor && static_cast<CActor*>(pActor)->GetActorClass() == CPlayer::GetActorClassType() ? static_cast<CPlayer*>(pActor) : 0;
	}

	IActor* ActorOf(EntityId id)
	{
		return id ? g_pGame->GetIGameFramework()->GetIActorSystem()->GetActor(id) : 0;
	}

	IActor* ActorByName(const char* name)
	{
		IEntity* pEntity = name && name[0] ? gEnv->pEntitySystem->FindEntityByName(name) : 0;
		return pEntity ? ActorOf(pEntity->GetId()) : 0;
	}

	const char* NameOf(EntityId id)
	{
		IEntity* pEntity = id ? gEnv->pEntitySystem->GetEntity(id) : 0;
		return pEntity ? pEntity->GetName() : "";
	}

	bool IsDown(IActor* pActor)
	{
		return pActor && pActor->IsPlayer() && pActor->GetHealth() <= 0 && static_cast<CActor*>(pActor)->GetSpectatorMode() == 0;
	}

	bool InGame(IActor* pActor)
	{
		return pActor && static_cast<CActor*>(pActor)->GetSpectatorMode() == 0;
	}

	// the host: the other player who was there first... in this game the
	// server's own player is the one with channel 0 on the server; a client
	// knows it from the mirrored host id, or takes the first other player
	IActor* HostPlayer()
	{
		IActor* pLocal = g_pGame->GetIGameFramework()->GetClientActor();
		IActor* pFirst = 0;
		IActorIteratorPtr it = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = it->Next())
		{
			if (!pActor->IsPlayer() || pActor == pLocal)
				continue;
			if (pActor->GetEntityId() == CoopAI::HostPlayerId())
				return pActor;
			if (!pFirst)
				pFirst = pActor;
		}
		return pFirst;
	}

	float Yaw(const Vec3& d) { return atan2_tpl(-d.x, d.y); }
	float Dist2D(const Vec3& a, const Vec3& b) { return sqrt_tpl((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }
	float Pitch(const Vec3& d) { return atan2_tpl(d.z, sqrt_tpl(d.x * d.x + d.y * d.y)); }
	float Wrap(float a)
	{
		while (a > gf_PI) a -= gf_PI2;
		while (a < -gf_PI) a += gf_PI2;
		return a;
	}

	void ViewAngles(CPlayer* pPlayer, float& yaw, float& pitch)
	{
		const Vec3 f = pPlayer->GetViewRotation().GetColumn1();
		yaw = Yaw(f);
		pitch = asin_tpl(clamp_tpl(f.z, -1.0f, 1.0f));
	}

	Vec3 EyePos(IActor* pActor)
	{
		SMovementState state;
		if (IMovementController* pMC = pActor->GetMovementController())
		{
			pMC->GetMovementState(state);
			return state.eyePosition;
		}
		return pActor->GetEntity()->GetWorldPos() + Vec3(0, 0, 1.6f);
	}

	// where to aim at a body: a little above the middle of its box
	Vec3 AimPoint(IEntity* pEntity)
	{
		AABB box;
		pEntity->GetWorldBounds(box);
		Vec3 c = box.GetCenter();
		c.z += (box.max.z - box.min.z) * 0.18f;
		return c;
	}

	// what one walks through: leaves, grass, tree tops, twigs, water; and
	// what one also sees (and shoots) through: wire mesh fences, barbed
	// wire, grates, glass. Everything else is in the way: planks, sheet
	// metal, walls, rocks (the bullets go through some; eyes and feet do not)
	enum EThrough { eT_Walk = 1, eT_See = 2 };
	int Through(int surfaceIdx)
	{
		static signed char s_known[1024];
		static bool s_init = false;
		if (!s_init)
		{
			memset(s_known, -1, sizeof(s_known));
			s_init = true;
		}
		if (surfaceIdx >= 0 && surfaceIdx < 1024 && s_known[surfaceIdx] >= 0)
			return s_known[surfaceIdx];
		IMaterialManager* pMaterials = gEnv->p3DEngine ? gEnv->p3DEngine->GetMaterialManager() : 0;
		ISurfaceTypeManager* pTypes = pMaterials ? pMaterials->GetSurfaceTypeManager() : 0;
		ISurfaceType* pType = pTypes ? pTypes->GetSurfaceType(surfaceIdx) : 0;
		const char* name = pType ? pType->GetName() : "";
		int through = 0;
		if (strstr(name, "leaves") || strstr(name, "grass") || strstr(name, "vegetation") || strstr(name, "canopy")
			|| strstr(name, "twigs") || strstr(name, "bushes") || strstr(name, "water") || strstr(name, "raindrop"))
			through = eT_Walk | eT_See;
		else if (strstr(name, "chainlink") || strstr(name, "barbwire") || strstr(name, "grate")
			|| (strstr(name, "glass") && !strstr(name, "bulletproof")))
			through = eT_See;
		if (surfaceIdx >= 0 && surfaceIdx < 1024)
			s_known[surfaceIdx] = (signed char)through;
		return through;
	}

	// the first thing in the way along a ray for the eyes (eT_See) or the
	// feet (eT_Walk); false: nothing
	bool FirstBlock(const Vec3& from, const Vec3& dir, int objects, IPhysicalEntity** pSkip, int nSkip, ray_hit& block, int mode = eT_See)
	{
		ray_hit hits[12];
		for (int i = 0; i < 12; ++i)
		{
			hits[i].dist = -1.0f;
			hits[i].pCollider = 0;
		}
		gEnv->pPhysicalWorld->RayWorldIntersection(from, dir, objects, rwi_pierceability0 | rwi_colltype_any, hits, 12, pSkip, nSkip);
		bool found = false;
		for (int i = 0; i < 12; ++i)
			if (hits[i].dist >= 0.0f && hits[i].pCollider && !(Through(hits[i].surface_idx) & mode) && (!found || hits[i].dist < block.dist))
			{
				block = hits[i];
				found = true;
			}
		return found;
	}

	// nothing in the way between the eye and the target
	bool Visible(IActor* pFrom, const Vec3& eye, IEntity* pTarget, const Vec3& to)
	{
		IPhysicalEntity* skip[2];
		int n = 0;
		if (IPhysicalEntity* p = pFrom->GetEntity()->GetPhysics())
			skip[n++] = p;
		if (IVehicle* pVehicle = pFrom->GetLinkedVehicle())
			if (IPhysicalEntity* p = pVehicle->GetEntity()->GetPhysics())
				skip[n++] = p;
		// leaves and grass do not hide anyone; a wall, a fence, a plank does
		ray_hit hit;
		const Vec3 dir = to - eye;
		if (!FirstBlock(eye, dir, ent_all, skip, n, hit))
			return true;
		IPhysicalEntity* pTargetPhys = pTarget->GetPhysics();
		if (hit.pCollider && hit.pCollider == pTargetPhys)
			return true;
		// the target's vehicle counts as the target (a gunner behind his gun)
		if (IActor* pActor = ActorOf(pTarget->GetId()))
			if (IVehicle* pVehicle = pActor->GetLinkedVehicle())
				if (hit.pCollider && hit.pCollider == pVehicle->GetEntity()->GetPhysics())
					return true;
		return hit.dist > dir.GetLength() - 0.5f;
	}

	// ------------------------------------------------------------------
	// JSON
	string Esc(const char* text)
	{
		string out = "\"";
		for (const unsigned char* p = (const unsigned char*)(text ? text : ""); *p; ++p)
		{
			switch (*p)
			{
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (*p < 0x20)
				{
					char buf[8];
					sprintf(buf, "\\u%04x", *p);
					out += buf;
				}
				else
					out += (char)*p;
			}
		}
		return out + "\"";
	}

	string Num(float f)
	{
		string s;
		s.Format("%.1f", f);
		return s;
	}

	string Vec(const Vec3& v)
	{
		string s;
		s.Format("[%.1f,%.1f,%.1f]", v.x, v.y, v.z);
		return s;
	}

	const char* Compass(const Vec3& d)
	{
		static const char* names[] = { "north", "north-west", "west", "south-west", "south", "south-east", "east", "north-east" };
		float a = Yaw(d);  // 0 north (+y), counter-clockwise
		int i = (int)floorf((Wrap(a) + gf_PI2 + gf_PI / 8.0f) / (gf_PI / 4.0f)) % 8;
		return names[i];
	}

	// ------------------------------------------------------------------
	// what happened: events and chat, read by the agent (observe)
	struct SLine { int n; float t; string from, text; };
	std::deque<SLine> s_events, s_chat;
	int s_eventN = 0, s_chatN = 0;
	int s_eventsRead = 0, s_chatRead = 0;

	void Event(const char* fmt, ...)
	{
		char buf[512];
		va_list args;
		va_start(args, fmt);
		vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
		buf[sizeof(buf) - 1] = 0;
		SLine l = { ++s_eventN, Now(), "", buf };
		s_events.push_back(l);
		while (s_events.size() > 60)
			s_events.pop_front();
		CryLogAlways("[CoopAgent] %s", buf);
	}

	// ------------------------------------------------------------------
	// the bot
	enum EOrder { eO_Free, eO_Follow, eO_Hold, eO_Goto, eO_Attack, eO_Revive };
	const char* const s_orderNames[] = { "free", "follow", "hold", "goto", "attack", "revive" };

	struct SBrain
	{
		EOrder order;
		string leader;          // follow: whom ("" = the host)
		string target;          // attack / revive: whom ("" = choose)
		Vec3 spot;              // goto / hold
		bool haveSpot;
		bool fireAtWill;
		string doing;           // what it does now, in words
		// fighting
		EntityId enemy;
		float burstUntil, pauseUntil, reloadAt, switchAt;
		bool attackHeld;
		// moving: how it gets on toward where it goes (the least way left,
		// since when), how often it got nowhere there, a few steps along
		// what is in the way
		Vec3 progGoal;
		float progBest, progAt, progSeen, progPath;
		int stuck;
		Vec3 slideDir;
		float slideUntil;
		std::vector<Vec3> local;           // a way round close by, found on its own
		Vec3 localFor;                     // to where
		float localAt;
		int localStage, localPlans;
		Vec3 nextCheckFor, toCheckFor;     // straight there, looked at when
		float nextCheckAt, toCheckAt;
		bool nextClear, toClear;
		float wayCheckAt, wayCheckPath;    // the way's farthest point in reach, looked for when
		float farSince, askAt;
		float reportAt;                    // where it is in its own game, told to the host's then
		// reviving
		bool useHeld;
		EntityId useFor;
		float useSince;
		// what was seen before (events)
		std::map<EntityId, bool> downSeen;
		float lastHealth;
		bool wasDown;
		bool named;
		// weapons: when it looks again, the one it goes to pick up
		float weaponAt;
		EntityId loot;
		float lootSince, useAt, useBlocked;
		int lootUses;            // use pressed at the one it goes for
		float lootNear;          // the closest its eyes came to it
		std::map<EntityId, float> lootFailed;   // given up on, and when (tried again a minute later)
		// a way around what is in the way (from the host's game: its AI
		// navigation), and where to
		std::vector<Vec3> path;
		Vec3 pathGoal;
		float pathAt, pathAskAt;
		float reviveSince;      // at the body of whom it revives, since
		float reachFailSince;   // next to the body with something between, since
		EntityId bodyToldFor;   // said where the host's game has his body
		// staying alive in a fight: a side step, a crouch; falling back hurt
		float dodgeUntil, dodgeSide;
		bool retreating;
		float hitAt;            // when it was last hit
		EntityId lastFought;    // the last one it said it fights (no repeats)
		int bursts, heldFire, stuckTimes, unseen, turning;   // for the log, now and then
		float dropAt;           // a thing it holds in the left hand goes, then
		EntityId downedFor;     // the downed teammate it goes to, since when
		float downedSince;
		float statsAt;
		float enemySeenAt;      // when its enemy was last in sight
		// falling back: to cover the host's game found (out of the enemy's
		// sight), since when, and when it may fall back again
		Vec3 cover;
		bool haveCover;
		float coverAskAt, retreatSince, recoveredAt;
		float coverAt;          // when the host's game named that cover
		float peekUntil;        // fighting from cover: up shooting / down
		bool peekUp;
		SBrain(): order(eO_Free), spot(ZERO), haveSpot(false), fireAtWill(true), enemy(0), burstUntil(0), pauseUntil(0), reloadAt(0),
			switchAt(0), attackHeld(false), progGoal(ZERO), progBest(1e9f), progAt(0), progSeen(-100), progPath(-100), stuck(0),
			slideDir(ZERO), slideUntil(0), localFor(ZERO), localAt(-100), localStage(0), localPlans(0), nextCheckFor(ZERO), toCheckFor(ZERO),
			nextCheckAt(0), toCheckAt(0), nextClear(true), toClear(true), wayCheckAt(0), wayCheckPath(-100), farSince(-1), askAt(0), reportAt(0), useHeld(false), useFor(0), useSince(0), lastHealth(-1), wasDown(false), named(false),
			weaponAt(0), loot(0), lootSince(0), useAt(0), useBlocked(0), pathGoal(ZERO), pathAt(-100), pathAskAt(0), reviveSince(-1), reachFailSince(-1), bodyToldFor(0),
			dodgeUntil(0), dodgeSide(1), retreating(false), hitAt(-100), lastFought(0), bursts(0), heldFire(0), stuckTimes(0), unseen(0), turning(0), statsAt(0), dropAt(0), downedFor(0), downedSince(0), enemySeenAt(-100),
			cover(ZERO), haveCover(false), coverAskAt(0), retreatSince(-100), recoveredAt(-100),
			coverAt(-100), peekUntil(0), peekUp(true) {}
	} s_bot;

	// where the downed teammates lie in the host's game, and when it said so
	struct SHostBody
	{
		Vec3 pos;
		float at;
	};
	std::map<string, SHostBody> s_hostBodies;

	// what SteerInput hands the player input
	struct SControl
	{
		Vec3 moveDir;           // world, horizontal (zero: stand)
		bool look;              // turn to yaw/pitch
		float yaw, pitch;
		float turnRate;         // radians a second
		bool sprint;
		bool jump;
		bool crouch;
		SControl(): moveDir(ZERO), look(false), yaw(0), pitch(0), turnRate(6.0f), sprint(false), jump(false), crouch(false) {}
	} s_ctl;

	void Press(CPlayer* pPlayer, const ActionId& action, bool press)
	{
		if (IPlayerInput* pInput = pPlayer->GetPlayerInput())
			pInput->OnAction(action, press ? eAAM_OnPress : eAAM_OnRelease, press ? 1.0f : 0.0f);
	}

	void Fire(CPlayer* pPlayer, bool on)
	{
		// not with something in the left hand: the fire key would throw it
		if (on)
			if (COffHand* pOffHand = static_cast<COffHand*>(pPlayer->GetWeaponByClass(CItem::sOffHandClass)))
				if (pOffHand->GetOffHandState() & (eOHS_HOLDING_OBJECT | eOHS_HOLDING_NPC | eOHS_PICKING | eOHS_PICKING_ITEM | eOHS_PICKING_ITEM2))
					on = false;
		if (on != s_bot.attackHeld)
		{
			Press(pPlayer, g_pGame->Actions().attack1, on);
			s_bot.attackHeld = on;
		}
	}

	void HoldUse(CPlayer* pPlayer, bool on, EntityId forWhom)
	{
		if (on != s_bot.useHeld)
		{
			Press(pPlayer, g_pGame->Actions().use, on);
			s_bot.useHeld = on;
			s_bot.useSince = Now();
		}
		s_bot.useFor = on ? forWhom : 0;
	}

	void Ask(int op, const char* name)
	{
		if (Now() < s_bot.askAt)
			return;
		s_bot.askAt = Now() + 4.0f;
		if (CGameRules* pRules = g_pGame->GetGameRules())
			pRules->CoopSendSyncToServer(15, op, 0, name ? name : "", "", 0, 0.0f);
	}

	// the weapon in the hands, its clip and what is left
	IWeapon* CurrentWeapon(CPlayer* pPlayer, int* pClip = 0, int* pReserve = 0, string* pName = 0)
	{
		IItem* pItem = pPlayer->GetCurrentItem();
		IWeapon* pWeapon = pItem ? pItem->GetIWeapon() : 0;
		if (pName)
			*pName = pItem ? pItem->GetEntity()->GetClass()->GetName() : "";
		if (pClip) *pClip = -1;
		if (pReserve) *pReserve = -1;
		if (pWeapon)
		{
			IFireMode* pMode = pWeapon->GetFireMode(pWeapon->GetCurrentFireMode());
			if (pMode)
			{
				if (pClip) *pClip = pMode->GetAmmoCount();
				if (pReserve && pMode->GetAmmoType() && pPlayer->GetInventory())
					*pReserve = pPlayer->GetInventory()->GetAmmoCount(pMode->GetAmmoType());
			}
		}
		return pWeapon;
	}

	bool Hostile(IActor* pActor)
	{
		bool hostile = false;
		int alert = 0;
		return !pActor->IsPlayer() && pActor->GetHealth() > 0 && !pActor->GetEntity()->IsHidden()
			&& CoopAI::GetMirroredAI(pActor->GetEntityId(), hostile, alert) && hostile;
	}

	// the enemy to fight: the ordered one, or the nearest one in sight
	EntityId PickEnemy(CPlayer* pMe, const Vec3& eye, float range)
	{
		if (s_bot.order == eO_Attack && !s_bot.target.empty())
		{
			IActor* pTarget = ActorByName(s_bot.target.c_str());
			return pTarget && pTarget->GetHealth() > 0 ? pTarget->GetEntityId() : 0;
		}
		// the one it fights stays its target while it is alive and was seen
		// a moment ago (a branch passing between them is no reason to turn
		// to another one and back)
		if (IActor* pCurrent = s_bot.enemy ? ActorOf(s_bot.enemy) : 0)
		{
			const Vec3 aim = AimPoint(pCurrent->GetEntity());
			if (Hostile(pCurrent) && aim.GetDistance(eye) < range)
			{
				if (Visible(pMe, eye, pCurrent->GetEntity(), aim))
					s_bot.enemySeenAt = Now();
				if (Now() - s_bot.enemySeenAt < 2.5f)
					return s_bot.enemy;
			}
		}
		EntityId best = 0;
		float bestScore = 1e9f;
		IActorIteratorPtr it = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = it->Next())
		{
			if (!Hostile(pActor))
				continue;
			const Vec3 aim = AimPoint(pActor->GetEntity());
			const float d = aim.GetDistance(eye);
			if (d > range || !Visible(pMe, eye, pActor->GetEntity(), aim))
				continue;
			// the one it fights already a little preferred
			const float score = d - (pActor->GetEntityId() == s_bot.enemy ? 8.0f : 0.0f);
			if (score < bestScore)
			{
				bestScore = score;
				best = pActor->GetEntityId();
			}
		}
		return best;
	}

	// what the body bumps into on a walk: what a player collides with
	// (walls, fences, rocks, trunks, houses, vehicles, heavy crates); not
	// leaves, grass, bushes (nothing a player collides with), nor the
	// ground (slopes are walked up)
	const int WALK_GEOM = (geom_colltype_player << rwi_colltype_bit) | rwi_stop_at_pierceable;

	// a loose thing too heavy to be pushed aside by walking into it
	bool Heavy(IPhysicalEntity* pPhys)
	{
		if (!pPhys || pPhys->GetType() == PE_STATIC)
			return true;
		pe_status_dynamics dyn;
		return !pPhys->GetStatus(&dyn) || dyn.mass <= 0.0f || dyn.mass > 60.0f;
	}

	// at a downed teammate: about level with him and nothing solid between
	// (a fence, a wall: the use key is not pressed through it)
	bool CanReach(CPlayer* pMe, const Vec3& pos, IActor* pDowned, const Vec3& body)
	{
		// the middle of the body lying there (it lies elsewhere in this
		// game: about where the middle is at that place)
		IEntity* pBody = pDowned->GetEntity();
		AABB box;
		pBody->GetWorldBounds(box);
		Vec3 at = box.IsEmpty() || Dist2D(pBody->GetWorldPos(), body) > 1.0f ? body + Vec3(0, 0, 0.9f) : box.GetCenter();
		if (fabsf(at.z - pos.z) > 1.6f)
			return false;
		IPhysicalEntity* skip[2];
		int n = 0;
		if (IPhysicalEntity* p = pMe->GetEntity()->GetPhysics())
			skip[n++] = p;
		if (IPhysicalEntity* p = pBody->GetPhysics())
			skip[n++] = p;
		const Vec3 from = pos + Vec3(0, 0, 1.0f);
		const Vec3 dir = at - from;
		ray_hit hit;
		return !gEnv->pPhysicalWorld->RayWorldIntersection(from, dir, ent_static | ent_rigid | ent_sleeping_rigid, WALK_GEOM, &hit, 1, skip, n)
			|| !Heavy(hit.pCollider) || hit.dist > dir.GetLength() - 0.4f;
	}

	IActor* DownedTeammate(CPlayer* pMe, const Vec3& pos, float range)
	{
		IActor* pBest = 0;
		float best = range;
		IActorIteratorPtr it = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = it->Next())
		{
			if (pActor == pMe || !IsDown(pActor))
				continue;
			if (s_bot.order == eO_Revive && !s_bot.target.empty() && stricmp(pActor->GetEntity()->GetName(), s_bot.target.c_str()))
				continue;
			const float d = pActor->GetEntity()->GetWorldPos().GetDistance(pos);
			if (d < best)
			{
				best = d;
				pBest = pActor;
			}
		}
		return pBest;
	}

	// a walk straight from one place to another: a ball the width of the
	// body swept at the knees, the waist and the chest through what does
	// not move (a thin ray slipped past the post of a fence the body then
	// was caught on), rays at the middle and both sides against what moves
	// (a bin is pushed aside, a car or a pallet of crates is not)
	bool ClearWay(CPlayer* pMe, const Vec3& from, const Vec3& to)
	{
		const Vec3 dir = to - from;
		if (dir.x * dir.x + dir.y * dir.y < 0.04f)
			return true;
		IPhysicalEntity* pSkip = pMe ? pMe->GetEntity()->GetPhysics() : 0;
		static const float s_heights[] = { 0.65f, 1.15f, 1.65f };
		for (int i = 0; i < 3; ++i)
		{
			primitives::sphere ball;
			ball.center = from + Vec3(0, 0, s_heights[i]);
			ball.r = 0.3f;
			geom_contact* pContact = 0;
			const float hit = gEnv->pPhysicalWorld->PrimitiveWorldIntersection(ball.type, &ball, dir, ent_static, &pContact, 0,
				WALK_GEOM, 0, 0, 0, &pSkip, pSkip ? 1 : 0);
			if (hit > 0.0f && pContact)
				return false;
		}
		Vec3 side(dir.y, -dir.x, 0);
		side.NormalizeSafe(Vec3(1, 0, 0));
		for (int i = 0; i < 6; ++i)
		{
			const Vec3 at = from + Vec3(0, 0, i < 3 ? 0.65f : 1.25f) + side * (0.3f * (float)(i % 3 - 1));
			ray_hit hit;
			if (gEnv->pPhysicalWorld->RayWorldIntersection(at, dir, ent_rigid | ent_sleeping_rigid, WALK_GEOM, &hit, 1, &pSkip, pSkip ? 1 : 0)
				&& hit.pCollider && Heavy(hit.pCollider))
				return false;
		}
		return true;
	}

	// the way to go when something is in the way right ahead: the nearest
	// direction to the wanted one the body walks free for a couple of
	// metres (the same side as last time first: no dithering left and
	// right). Looked at again a few times a second
	Vec3 Steer(CPlayer* pMe, const Vec3& pos, const Vec3& dir)
	{
		static float s_side = 1.0f;
		static float s_at = -100.0f;
		static Vec3 s_pos(ZERO), s_in(ZERO), s_out(ZERO);
		if (Now() - s_at < 0.15f && s_in.Dot(dir) > 0.995f && Dist2D(s_pos, pos) < 0.3f)
			return s_out;
		static const float s_angles[] = { 0.0f, 0.4f, 0.8f, 1.2f, 1.6f, 2.0f, 2.4f };
		Vec3 out = dir;
		bool found = false;
		for (int i = 0; i < 7 && !found; ++i)
			for (int k = 0; k < (i ? 2 : 1) && !found; ++k)
			{
				const float a = s_angles[i] * (k ? -s_side : s_side);
				const float c = cosf(a), sn = sinf(a);
				const Vec3 d(dir.x * c - dir.y * sn, dir.x * sn + dir.y * c, 0);
				if (ClearWay(pMe, pos, pos + d * 1.8f))
				{
					if (i)
						s_side = k ? -s_side : s_side;
					out = d;
					found = true;
				}
			}
		s_at = Now();
		s_pos = pos;
		s_in = dir;
		s_out = out;
		return out;
	}

	// ------------------------------------------------------------------
	// a way round close by, found on its own: half-metre squares around it
	// (the ground's height in each, whether the body fits there) searched
	// for the shortest walk. For what the host's navigation leaves out or
	// cannot know: fence posts, bins, rocks, a parked truck, a pallet
	const float GRID_CELL = 0.5f;
	struct SGrid
	{
		float x0, y0, z;                   // the corner, the height it looks from
		int n;                             // squares a side
		std::vector<signed char> state;    // -1 not looked at yet, 0 the body does not fit, 1 it does
		std::vector<float> ground;         // the ground's height
		std::vector<unsigned char> cost;   // 1, more where a loose thing is pushed aside
		std::vector<AABB> light;           // loose things pushed aside (walked round if it can)
		std::vector<IPhysicalEntity*> skip;   // not in the way: itself, those loose things, bodies lying there
		SGrid(): x0(0), y0(0), z(0), n(0) {}
	} s_grid;

	// the squares around a place (it stands in the middle of one)
	void GridBegin(CPlayer* pMe, const Vec3& center, float radius)
	{
		SGrid& g = s_grid;
		g.n = 2 * std::max(2, (int)(radius / GRID_CELL)) + 1;
		g.x0 = center.x - g.n * GRID_CELL * 0.5f;
		g.y0 = center.y - g.n * GRID_CELL * 0.5f;
		g.z = center.z;
		g.state.assign(g.n * g.n, -1);
		g.ground.assign(g.n * g.n, center.z);
		g.cost.assign(g.n * g.n, 1);
		g.light.clear();
		g.skip.clear();
		if (IPhysicalEntity* p = pMe ? pMe->GetEntity()->GetPhysics() : 0)
			g.skip.push_back(p);
		IPhysicalEntity** list = 0;
		const float size = g.n * GRID_CELL;
		const int count = gEnv->pPhysicalWorld->GetEntitiesInBox(Vec3(g.x0, g.y0, g.z - 6.0f), Vec3(g.x0 + size, g.y0 + size, g.z + 6.0f),
			list, ent_rigid | ent_sleeping_rigid);
		for (int i = 0; i < count; ++i)
		{
			// a heavy one is in the way as it is (a truck standing askew),
			// a light one is pushed aside, a body lying there is no obstacle
			if (Heavy(list[i]) && list[i]->GetType() != PE_ARTICULATED)
				continue;
			if (std::find(g.skip.begin(), g.skip.end(), list[i]) == g.skip.end())
				g.skip.push_back(list[i]);
			pe_status_pos sp;
			if (list[i]->GetType() != PE_ARTICULATED && list[i]->GetStatus(&sp))
				g.light.push_back(AABB(sp.pos + sp.BBox[0] - Vec3(0.3f, 0.3f, 0.0f), sp.pos + sp.BBox[1] + Vec3(0.3f, 0.3f, 0.0f)));
		}
	}

	// the body fits on that square (looked at once: the ground's height,
	// then a ball at the knees, the waist, the chest)
	bool GridFree(int cx, int cy)
	{
		SGrid& g = s_grid;
		if (cx < 0 || cy < 0 || cx >= g.n || cy >= g.n)
			return false;
		const int i = cy * g.n + cx;
		if (g.state[i] >= 0)
			return g.state[i] == 1;
		g.state[i] = 0;
		const float x = g.x0 + (cx + 0.5f) * GRID_CELL, y = g.y0 + (cy + 0.5f) * GRID_CELL;
		const float top = std::max(g.z, gEnv->p3DEngine->GetTerrainElevation(x, y)) + 2.0f;
		IPhysicalEntity** pSkip = g.skip.empty() ? 0 : &g.skip[0];
		const int nSkip = (int)g.skip.size();
		ray_hit hit;
		if (!gEnv->pPhysicalWorld->RayWorldIntersection(Vec3(x, y, top), Vec3(0, 0, -8.0f), ent_static | ent_terrain, WALK_GEOM, &hit, 1,
			pSkip, nSkip))
			return false;
		g.ground[i] = hit.pt.z;
		for (int k = 0; k < 3; ++k)
		{
			primitives::sphere ball;
			ball.center = Vec3(x, y, hit.pt.z + 0.65f + 0.5f * k);
			ball.r = 0.3f;
			if (gEnv->pPhysicalWorld->PrimitiveWorldIntersection(ball.type, &ball, Vec3(ZERO), ent_static | ent_rigid | ent_sleeping_rigid, 0, 0,
				WALK_GEOM, 0, 0, 0, pSkip, nSkip) > 0.0f)
				return false;
		}
		const Vec3 mid(x, y, hit.pt.z + 0.8f);
		for (size_t k = 0; k < g.light.size(); ++k)
			if (g.light[k].IsContainPoint(mid))
				g.cost[i] = 4;
		g.state[i] = 1;
		return true;
	}

	int GridIndex(const Vec3& p)
	{
		const SGrid& g = s_grid;
		const int cx = (int)floorf((p.x - g.x0) / GRID_CELL), cy = (int)floorf((p.y - g.y0) / GRID_CELL);
		return cx < 0 || cy < 0 || cx >= g.n || cy >= g.n ? -1 : cy * g.n + cx;
	}

	// a straight walk over the squares between two places: each of them
	// free, no step up or down too high
	bool GridLine(const Vec3& a, const Vec3& b)
	{
		SGrid& g = s_grid;
		const int steps = std::max(1, (int)(Dist2D(a, b) / (GRID_CELL * 0.5f)));
		int prev = -1;
		for (int s = 0; s <= steps; ++s)
		{
			const int i = GridIndex(a + (b - a) * ((float)s / (float)steps));
			if (i < 0 || !GridFree(i % g.n, i / g.n))
				return false;
			if (prev >= 0 && prev != i && fabsf(g.ground[i] - g.ground[prev]) > 0.45f)
				return false;
			prev = i;
		}
		return true;
	}

	// the shortest walk over the squares from where it stands to a place
	// (or to the square nearest to it it gets to: the place may be off the
	// squares or in a wall), only the corners of it; false: none
	bool GridPath(const Vec3& from, const Vec3& to, std::vector<Vec3>& out, int* pLooked = 0, bool* pFound = 0)
	{
		out.clear();
		if (pFound)
			*pFound = false;
		SGrid& g = s_grid;
		const int n = g.n;
		const int start = GridIndex(from);
		if (start < 0)
			return false;
		// where it is, it fits (pressed against a fence a ball may touch it)
		if (!GridFree(start % n, start / n))
		{
			g.state[start] = 1;
			g.ground[start] = from.z;
		}
		const float edge = GRID_CELL * 0.5f;
		const Vec3 goal(clamp_tpl(to.x, g.x0 + edge, g.x0 + n * GRID_CELL - edge), clamp_tpl(to.y, g.y0 + edge, g.y0 + n * GRID_CELL - edge), to.z);
		const int target = GridIndex(goal);
		const int tx = target % n, ty = target / n;
		struct SOpen
		{
			float f;
			int i;
			bool operator<(const SOpen& o) const { return f > o.f; }
		};
		std::vector<float> cost(n * n, 1e9f);
		std::vector<int> back(n * n, -1);
		std::vector<bool> closed(n * n, false);
		std::priority_queue<SOpen> open;
		#define COOP_GRID_H(i) (GRID_CELL * (float)(std::max(abs((i) % n - tx), abs((i) / n - ty)) + 0.414f * std::min(abs((i) % n - tx), abs((i) / n - ty))))
		cost[start] = 0.0f;
		const SOpen o0 = { COOP_GRID_H(start), start };
		open.push(o0);
		int best = start, looked = 0;
		float bestH = COOP_GRID_H(start);
		const float startH = bestH;
		bool found = false;
		static const int s_dx[] = { 1, -1, 0, 0, 1, 1, -1, -1 };
		static const int s_dy[] = { 0, 0, 1, -1, 1, -1, 1, -1 };
		while (!open.empty() && looked < 4000)
		{
			const SOpen cur = open.top();
			open.pop();
			if (closed[cur.i])
				continue;
			closed[cur.i] = true;
			++looked;
			if (cur.i == target)
			{
				found = true;
				best = target;
				break;
			}
			const float h = COOP_GRID_H(cur.i);
			if (h < bestH)
			{
				bestH = h;
				best = cur.i;
			}
			const int cx = cur.i % n, cy = cur.i / n;
			for (int k = 0; k < 8; ++k)
			{
				const int nx = cx + s_dx[k], ny = cy + s_dy[k];
				const bool diag = k >= 4;
				if (!GridFree(nx, ny) || (diag && (!GridFree(nx, cy) || !GridFree(cx, ny))))
					continue;
				const int ni = ny * n + nx;
				if (closed[ni] || fabsf(g.ground[ni] - g.ground[cur.i]) > (diag ? 0.6f : 0.45f))
					continue;
				const float c = cost[cur.i] + (diag ? 1.414f : 1.0f) * GRID_CELL * (float)g.cost[ni];
				if (c < cost[ni])
				{
					cost[ni] = c;
					back[ni] = cur.i;
					const SOpen on = { c + COOP_GRID_H(ni), ni };
					open.push(on);
				}
			}
		}
		#undef COOP_GRID_H
		if (pLooked)
			*pLooked = looked;
		if (pFound)
			*pFound = found;
		// not there, and nowhere a metre nearer to it either
		if (!found && bestH > startH - 1.0f)
			return false;
		std::vector<Vec3> cells;
		for (int i = best; i >= 0; i = back[i])
			cells.push_back(Vec3(g.x0 + (i % n + 0.5f) * GRID_CELL, g.y0 + (i / n + 0.5f) * GRID_CELL, g.ground[i]));
		std::reverse(cells.begin(), cells.end());
		// the corners: from each to the farthest one in a straight walk
		Vec3 at = from;
		for (size_t i = 1; i < cells.size(); )
		{
			size_t reach = i;
			for (size_t j = cells.size() - 1; j > i; --j)
				if (GridLine(at, cells[j]))
				{
					reach = j;
					break;
				}
			out.push_back(cells[reach]);
			at = cells[reach];
			i = reach + 1;
		}
		return !out.empty();
	}

	// asks the host's game for a way there (its AI navigation knows the
	// walls, rocks, houses); the answer comes as OnPath
	void AskPath(const Vec3& goal)
	{
		if (Now() < s_bot.pathAskAt)
			return;
		s_bot.pathAskAt = Now() + 3.0f;
		string where;
		where.Format("%.1f %.1f %.1f", goal.x, goal.y, goal.z);
		if (CGameRules* pRules = g_pGame->GetGameRules())
			pRules->CoopSendSyncToServer(15, 5, 0, where.c_str(), "", 0, 0.0f);
	}

	// asks the host's game for cover from an enemy: a place close by out of
	// its sight that it can walk to (the answer comes as OnPath, op 1)
	void AskCover(IActor* pThreat)
	{
		if (Now() < s_bot.coverAskAt || !pThreat)
			return;
		s_bot.coverAskAt = Now() + 4.0f;
		if (CGameRules* pRules = g_pGame->GetGameRules())
			pRules->CoopSendSyncToServer(15, 6, 0, pThreat->GetEntity()->GetName(), "", 0, 0.0f);
	}

	// the length of a walk from here along points
	float WayLeftOf(const Vec3& pos, const std::vector<Vec3>& points)
	{
		float left = 0.0f;
		Vec3 at = pos;
		for (size_t i = 0; i < points.size(); ++i)
		{
			left += Dist2D(at, points[i]);
			at = points[i];
		}
		return left;
	}

	// the way left to the goal along the way it has (none: straight there)
	float WayLeft(const Vec3& pos, const Vec3& to)
	{
		const std::vector<Vec3>& path = s_bot.path;
		if (path.empty() || Dist2D(s_bot.pathGoal, to) >= 4.0f)
			return Dist2D(pos, to);
		float left = Dist2D(pos, path[0]);
		for (size_t i = 1; i < path.size(); ++i)
			left += Dist2D(path[i - 1], path[i]);
		return left + Dist2D(path.back(), to);
	}

	// the point of the way to walk to now (or the goal itself)
	Vec3 Waypoint(CPlayer* pMe, const Vec3& pos, const Vec3& to, float dist, bool blocked)
	{
		std::vector<Vec3>& path = s_bot.path;
		const bool fresh = !path.empty() && Now() - s_bot.pathAt < 12.0f && Dist2D(s_bot.pathGoal, to) < 4.0f;
		if (!fresh)
		{
			path.clear();
			// also close by: an item behind a container's wall is 1 m away
			// and the way in is round by the door
			if (blocked)
				AskPath(to);
			return to;
		}
		// the way starts where the host's game has it (a little off, maybe):
		// from the point nearest to it, and on past every one reached. The
		// goal itself (the way's last point) is not one of them: next to it
		// on the wrong side of a wall, the way round would be thrown away
		size_t nearest = 0;
		for (size_t i = 1; i + 1 < path.size(); ++i)
			if (Dist2D(path[i], pos) < Dist2D(path[nearest], pos))
				nearest = i;
		path.erase(path.begin(), path.begin() + nearest);
		while (!path.empty() && Dist2D(path[0], pos) < 1.0f)
			path.erase(path.begin());
		// the farthest of the next points the body walks to straight (the
		// corner of a fence is walked round, not cut)
		if (Now() >= s_bot.wayCheckAt || s_bot.wayCheckPath != s_bot.pathAt)
		{
			s_bot.wayCheckAt = Now() + 0.25f;
			s_bot.wayCheckPath = s_bot.pathAt;
			for (size_t k = std::min(path.size(), (size_t)6); k-- > 1; )
				if (Dist2D(path[k], pos) < 25.0f && ClearWay(pMe, pos, path[k]))
				{
					path.erase(path.begin(), path.begin() + k);
					break;
				}
		}
		if (path.empty() || (!blocked && dist < 6.0f))
			return to;
		// a fresh way now and then (it moves on, so does the one it goes to)
		if (Now() - s_bot.pathAt > 4.0f)
			AskPath(to);
		return path[0];
	}

	// something low right ahead to jump onto or over (a crate, a low wall);
	// not a fence or a wall: it only ended up on whatever stood next to it
	bool LowObstacleAhead(CPlayer* pMe, const Vec3& pos, const Vec3& dir)
	{
		IPhysicalEntity* pSkip = pMe->GetEntity()->GetPhysics();
		const int ents = ent_static | ent_rigid | ent_sleeping_rigid;
		ray_hit hit;
		if (!gEnv->pPhysicalWorld->RayWorldIntersection(pos + Vec3(0, 0, 0.5f), dir * 1.2f, ents, WALK_GEOM, &hit, 1, &pSkip, pSkip ? 1 : 0))
			return false;
		return !gEnv->pPhysicalWorld->RayWorldIntersection(pos + Vec3(0, 0, 1.3f), dir * 1.6f, ents, WALK_GEOM, &hit, 1, &pSkip, pSkip ? 1 : 0);
	}

	// along the face of what is in the way ahead, the side nearer to where
	// it wants to go
	Vec3 AlongObstacle(CPlayer* pMe, const Vec3& pos, const Vec3& dir, const Vec3& toward)
	{
		IPhysicalEntity* pSkip = pMe->GetEntity()->GetPhysics();
		Vec3 n = -dir;
		ray_hit hit;
		if (gEnv->pPhysicalWorld->RayWorldIntersection(pos + Vec3(0, 0, 1.0f), dir * 2.0f, ent_static | ent_rigid | ent_sleeping_rigid,
			WALK_GEOM, &hit, 1, &pSkip, pSkip ? 1 : 0))
			n = hit.n;
		n.z = 0;
		n.NormalizeSafe(-dir);
		Vec3 along(n.y, -n.x, 0);
		if (along.Dot(toward) < 0.0f)
			along = -along;
		return along;
	}

	// walk toward a point; true when there
	bool MoveTo(CPlayer* pMe, const Vec3& to, float stopAt, bool mayRun, bool mayCatchUp, const char* toName)
	{
		const Vec3 pos = pMe->GetEntity()->GetWorldPos();
		const float now = Now();
		Vec3 d = to - pos;
		d.z = 0;
		const float dist = d.GetLength();
		if (dist <= stopAt)
		{
			s_ctl.moveDir.zero();
			s_bot.stuck = 0;
			s_bot.progAt = now;
			s_bot.slideUntil = 0.0f;
			s_bot.farSince = -1;
			return true;
		}
		// how it gets on: the least way left so far, and when it got there.
		// Walking along a fence with the head against it is moving, not
		// getting anywhere
		if (Dist2D(s_bot.progGoal, to) > 2.0f || now - s_bot.progSeen > 1.5f)
		{
			s_bot.progGoal = to;
			s_bot.progBest = 1e9f;
			s_bot.progAt = now;
			s_bot.stuck = 0;
			s_bot.slideUntil = 0.0f;
		}
		s_bot.progSeen = now;
		// around what is in the way, by the host's navigation (straight
		// there looked at a few times a second)
		if (now >= s_bot.toCheckAt || Dist2D(s_bot.toCheckFor, to) > 0.5f)
		{
			s_bot.toCheckAt = now + 0.2f;
			s_bot.toCheckFor = to;
			s_bot.toClear = ClearWay(pMe, pos, to);
		}
		const bool blocked = s_bot.stuck > 0 || !s_bot.toClear;
		const Vec3 next = Waypoint(pMe, pos, to, dist, blocked);
		if (s_bot.progPath != s_bot.pathAt)
		{
			// a new way: measured along that one from now on (a way round
			// is longer than the straight line it could not walk)
			s_bot.progPath = s_bot.pathAt;
			s_bot.progBest = 1e9f;
		}
		// the point it goes to next not in a straight walk (a fence post,
		// a bin, a truck the host's navigation leaves out), or getting
		// nowhere: the way round close by, found on its own (again when
		// that point moves, now and then, and when it still gets nowhere)
		if (now >= s_bot.nextCheckAt || Dist2D(s_bot.nextCheckFor, next) > 0.5f)
		{
			s_bot.nextCheckAt = now + 0.2f;
			s_bot.nextCheckFor = next;
			s_bot.nextClear = ClearWay(pMe, pos, next);
		}
		const float toNext = Dist2D(pos, next);
		std::vector<Vec3>& local = s_bot.local;
		// close to the goal: the whole way there on its own (the host's way
		// may lead right through what it does not know: under a truck)
		const Vec3 target = dist < 14.0f ? to : next;
		const float toTarget = Dist2D(pos, target);
		if ((!s_bot.nextClear && toNext < 16.0f) || s_bot.stuck > 0)
		{
			const bool moved = Dist2D(s_bot.localFor, target) > 1.5f || s_bot.localStage != s_bot.stuck;
			if (moved || now - s_bot.localAt > 3.0f)
			{
				int looked = 0;
				bool found = false;
				const float t0 = gEnv->pTimer->GetAsyncCurTime();
				GridBegin(pMe, pos, std::min(16.0f, std::max(toTarget, toNext) + 6.0f));
				if (!GridPath(pos, target, local, &looked, &found))
					local.clear();
				// not there within the squares (the way round is longer): to
				// the host's way's next point then
				if (!found && Dist2D(target, next) > 0.5f)
				{
					std::vector<Vec3> alt;
					int more = 0;
					bool there = false;
					if (GridPath(pos, next, alt, &more, &there) && there)
						local.swap(alt);
					looked += more;
				}
				if (moved)
				{
					++s_bot.localPlans;
					CryLogAlways("[CoopAgent] a way round to %s close by: %s (%d squares looked at, %.1f ms)", toName && *toName ? toName : "where it goes",
						local.empty() ? "none" : string().Format("%d corners, %.1f m to the point %.1f m away", (int)local.size(),
							WayLeftOf(pos, local), toTarget).c_str(), looked, (gEnv->pTimer->GetAsyncCurTime() - t0) * 1000.0f);
				}
				s_bot.localFor = target;
				s_bot.localAt = now;
				s_bot.localStage = s_bot.stuck;
				s_bot.progBest = 1e9f;
			}
			while (local.size() > 1 && Dist2D(local[0], pos) < 0.6f)
				local.erase(local.begin());
		}
		else
			local.clear();
		const float tail = Dist2D(target, to) < 0.01f ? 0.0f : WayLeft(target, to);
		const float left = local.empty() ? WayLeft(pos, to) : WayLeftOf(pos, local) + Dist2D(local.back(), target) + tail;
		if (left < s_bot.progBest - 0.5f)
		{
			if (s_bot.progBest < 1e8f)
			{
				s_bot.progAt = now;
				s_bot.stuck = 0;
			}
			s_bot.progBest = left;
		}
		Vec3 dn = (local.empty() ? next : local[0]) - pos;
		dn.z = 0;
		Vec3 dir = dn.GetLength() > 0.1f ? dn.GetNormalized() : d / dist;
		// no nearer for a while: a fresh way from where it stands and a few
		// steps along what is in the way (first to the side the way goes,
		// then to the other, longer each time); a jump only onto something
		// low. Long without getting anywhere, the one it follows: put next
		// to him (never so to a downed teammate: it walks)
		static const float s_stages[] = { 3.0f, 7.0f, 12.0f, 18.0f, 25.0f, 33.0f, 42.0f, 52.0f };
		const float stalled = now - s_bot.progAt;
		if (s_bot.stuck < 8 && stalled > s_stages[s_bot.stuck])
		{
			++s_bot.stuck;
			++s_bot.stuckTimes;
			s_bot.pathAt = -100.0f;    // the way it had led nowhere: a new one
			s_bot.pathAskAt = 0.0f;
			AskPath(to);
			Vec3 along = AlongObstacle(pMe, pos, dir, dn.GetLength() > 0.1f ? dn : d);
			if (!(s_bot.stuck & 1))
				along = -along;
			s_bot.slideDir = along;
			s_bot.slideUntil = now + 1.0f + 0.5f * (float)s_bot.stuck;
			const bool low = LowObstacleAhead(pMe, pos, dir);
			if (low)
				s_ctl.jump = true;
			Event("no nearer to %s for %.0f s (%.0f m to go): a new way, a way round close by%s", toName && *toName ? toName : "where it goes",
				stalled, dist, low ? ", a jump over something low" : "");
		}
		if (s_bot.stuck >= 4 && mayCatchUp && dist > 8.0f)
			Ask(1, toName);
		if (now < s_bot.slideUntil && local.empty())
		{
			// past its end: the way to the point is free again
			if (ClearWay(pMe, pos, next))
				s_bot.slideUntil = 0.0f;
			else
				dir = s_bot.slideDir;
		}
		// far behind for a while: put next to the one it follows
		if (mayCatchUp && dist > 60.0f)
		{
			if (s_bot.farSince < 0)
				s_bot.farSince = now;
			else if (now - s_bot.farSince > 4.0f)
			{
				Ask(1, toName);
				s_bot.farSince = -1;
			}
		}
		else
			s_bot.farSince = -1;
		// the way round close by goes between what is there: no steering
		// off it (that took it back against the fence)
		s_ctl.moveDir = local.empty() ? Steer(pMe, pos, dir) : dir;
		s_ctl.sprint = mayRun && dist > 15.0f;
		return false;
	}

	void LookAt(const Vec3& eye, const Vec3& at, float rate)
	{
		const Vec3 d = at - eye;
		s_ctl.look = true;
		s_ctl.yaw = Yaw(d);
		s_ctl.pitch = Pitch(d);
		s_ctl.turnRate = rate;
	}

	// ------------------------------------------------------------------
	// weapons: the best one with ammo in the hands; more from the ground

	// how good a weapon is to fight with (0: not one: fists, rockets,
	// explosives, tools)
	int WeaponRank(const char* cls)
	{
		static const struct { const char* name; int rank; } s_ranks[] = {
			{ "SCAR", 9 }, { "FY71", 9 }, { "GaussRifle", 8 }, { "SMG", 8 }, { "Shotgun", 7 }, { "Hurricane", 7 },
			{ "MOAC", 7 }, { "MOAR", 6 }, { "DSG1", 6 }, { "SOCOM", 3 },
		};
		for (size_t i = 0; i < sizeof(s_ranks) / sizeof(s_ranks[0]); ++i)
			if (!stricmp(cls, s_ranks[i].name))
				return s_ranks[i].rank;
		return 0;
	}

	// shots it has for a weapon: the clip and what it carries for it
	int WeaponShots(CPlayer* pMe, IItem* pItem)
	{
		IWeapon* pWeapon = pItem ? pItem->GetIWeapon() : 0;
		IFireMode* pMode = pWeapon ? pWeapon->GetFireMode(pWeapon->GetCurrentFireMode()) : 0;
		if (!pMode)
			return 0;
		int shots = pMode->GetAmmoCount();
		if (pMode->GetAmmoType() && pMe->GetInventory())
			shots += pMe->GetInventory()->GetAmmoCount(pMode->GetAmmoType());
		return shots;
	}

	// the best weapon it carries that can shoot (0 rank: none)
	IItem* BestWeapon(CPlayer* pMe, int* pRank = 0)
	{
		IInventory* pInventory = pMe->GetInventory();
		IItemSystem* pItems = g_pGame->GetIGameFramework()->GetIItemSystem();
		IItem* pBest = 0;
		int bestRank = 0, bestShots = 0;
		for (int i = 0; pInventory && i < pInventory->GetCount(); ++i)
		{
			IItem* pItem = pItems->GetItem(pInventory->GetItem(i));
			if (!pItem || !pItem->CanSelect())
				continue;
			const int rank = WeaponRank(pItem->GetEntity()->GetClass()->GetName());
			const int shots = rank ? WeaponShots(pMe, pItem) : 0;
			if (shots > 0 && (rank > bestRank || (rank == bestRank && shots > bestShots)))
			{
				pBest = pItem;
				bestRank = rank;
				bestShots = shots;
			}
		}
		if (pRank)
			*pRank = bestRank;
		return pBest;
	}

	// the best weapon in the hands (not in the middle of a reload)
	void ChooseWeapon(CPlayer* pMe, bool now)
	{
		if (!now && Now() < s_bot.weaponAt)
			return;
		s_bot.weaponAt = Now() + 1.0f;
		if (Now() < s_bot.reloadAt || Now() < s_bot.switchAt)
			return;
		// not while the left hand picks something up: the weapon in the
		// right one would stay in the one-hand pose
		if (COffHand* pOffHand = static_cast<COffHand*>(pMe->GetWeaponByClass(CItem::sOffHandClass)))
			if (pOffHand->GetOffHandState() != eOHS_INIT_STATE)
				return;
		IItem* pCurrent = pMe->GetCurrentItem();
		const int currentRank = pCurrent ? WeaponRank(pCurrent->GetEntity()->GetClass()->GetName()) : 0;
		const int currentShots = currentRank ? WeaponShots(pMe, pCurrent) : 0;
		int bestRank = 0;
		IItem* pBest = BestWeapon(pMe, &bestRank);
		if (!pBest || pBest == pCurrent || (currentShots > 0 && currentRank >= bestRank))
			return;
		const char* cls = pBest->GetEntity()->GetClass()->GetName();
		Fire(pMe, false);
		pMe->SelectItemByName(cls, true);
		s_bot.switchAt = Now() + 1.5f;
		Event("takes the %s%s", cls, currentRank && !currentShots ? " (no ammo left for the other one)" : "");
	}

	bool LootGivenUp(EntityId id)
	{
		std::map<EntityId, float>::iterator it = s_bot.lootFailed.find(id);
		if (it == s_bot.lootFailed.end())
			return false;
		if (Now() - it->second < 60.0f)
			return true;
		s_bot.lootFailed.erase(it);
		return false;
	}

	// this one, and what lies with it (the rest of a box's content behind
	// the same wall), left alone for a minute
	void GiveUpLoot(IEntity* pLoot, const char* why)
	{
		if (!pLoot)
			return;
		const Vec3 at = pLoot->GetWorldPos();
		SEntityProximityQuery query;
		query.box = AABB(at - Vec3(2.0f, 2.0f, 1.5f), at + Vec3(2.0f, 2.0f, 1.5f));
		gEnv->pEntitySystem->QueryProximity(query);
		int n = 0;
		for (int i = 0; i < query.nCount; ++i)
			if (query.pEntities[i] && g_pGame->GetIGameFramework()->GetIItemSystem()->GetItem(query.pEntities[i]->GetId()))
			{
				s_bot.lootFailed[query.pEntities[i]->GetId()] = Now();
				++n;
			}
		s_bot.lootFailed[pLoot->GetId()] = Now();
		if (why)
			Event("leaves the %s and %d more there: %s", pLoot->GetClass()->GetName(), max(0, n - 1), why);
	}

	// a weapon or ammo on the ground worth going for: a better weapon than
	// it has, or ammo for one it has when it runs low
	IEntity* FindLoot(CPlayer* pMe, const Vec3& pos, float range)
	{
		int bestRank = 0;
		IItem* pBest = BestWeapon(pMe, &bestRank);
		const int shots = pBest ? WeaponShots(pMe, pBest) : 0;
		const bool low = !pBest || bestRank < 6 || shots < 60;
		SEntityProximityQuery query;
		query.box = AABB(pos - Vec3(range, range, 4.0f), pos + Vec3(range, range, 4.0f));
		gEnv->pEntitySystem->QueryProximity(query);
		IItemSystem* pItems = g_pGame->GetIGameFramework()->GetIItemSystem();
		IInventory* pInventory = pMe->GetInventory();
		IEntity* pFound = 0;
		float foundDist = range;
		for (int i = 0; i < query.nCount; ++i)
		{
			IEntity* pEntity = query.pEntities[i];
			if (!pEntity || pEntity->IsHidden() || LootGivenUp(pEntity->GetId()))
				continue;
			IItem* pItem = pItems->GetItem(pEntity->GetId());
			if (!pItem || pItem->GetOwnerId())
				continue;
			const char* cls = pEntity->GetClass()->GetName();
			const int rank = WeaponRank(cls);
			const bool have = pInventory && pInventory->GetItemByClass(pEntity->GetClass()) != 0;
			const bool ammoBox = !stricmp(cls, "CustomAmmoPickup");
			// better than its own; or more ammo (the same weapon again, a
			// box) when it runs low
			if (!(rank > bestRank && !have) && !(low && ((have && rank) || ammoBox)))
				continue;
			const float d = Dist2D(pEntity->GetWorldPos(), pos);
			if (d < foundDist && fabsf(pEntity->GetWorldPos().z - pos.z) < 3.0f)
			{
				foundDist = d;
				pFound = pEntity;
			}
		}
		return pFound;
	}

	// goes to what it found on the ground, looks at it, takes it (the use
	// key, as a player: the hand reaches for it); true while busy with it
	bool Loot(CPlayer* pMe, const Vec3& pos, const Vec3& eye, bool fighting)
	{
		IEntity* pLoot = s_bot.loot ? gEnv->pEntitySystem->GetEntity(s_bot.loot) : 0;
		IItem* pItem = pLoot ? g_pGame->GetIGameFramework()->GetIItemSystem()->GetItem(pLoot->GetId()) : 0;
		if (s_bot.loot && (!pItem || pItem->GetOwnerId() || pLoot->IsHidden()))
		{
			// taken (by it, or by someone)
			if (pItem && pItem->GetOwnerId() == pMe->GetEntityId())
				Event("picked up the %s", pLoot->GetClass()->GetName());
			s_bot.loot = 0;
			pLoot = 0;
			ChooseWeapon(pMe, true);
		}
		if (!pLoot)
		{
			// not with an enemy close by, unless it has nothing to shoot with
			int rank = 0;
			const bool armed = BestWeapon(pMe, &rank) != 0;
			if (fighting && armed)
				return false;
			pLoot = FindLoot(pMe, pos, armed ? 20.0f : 45.0f);
			if (!pLoot)
				return false;
			s_bot.loot = pLoot->GetId();
			s_bot.lootSince = Now();
			s_bot.lootUses = 0;
			s_bot.lootNear = 1000.0f;
			Event("goes to pick up the %s (%.0f m, at %.1f %.1f %.1f)", pLoot->GetClass()->GetName(), Dist2D(pLoot->GetWorldPos(), pos),
				pLoot->GetWorldPos().x, pLoot->GetWorldPos().y, pLoot->GetWorldPos().z);
		}
		if (Now() - s_bot.lootSince > 20.0f)
		{
			// out of reach: something else
			string why;
			why.Format("out of reach (its eyes came %.1f m close, use pressed %d times)", s_bot.lootNear, s_bot.lootUses);
			GiveUpLoot(pLoot, why.c_str());
			s_bot.loot = 0;
			return false;
		}
		const Vec3 at = pLoot->GetWorldPos();
		Fire(pMe, false);
		// there only when the hand can reach it: near and nothing solid
		// between (next to a container with the ammo inside it is not)
		// (toward its middle: where it lies, the floor or the shelf under it
		// is hit first; something hit right at it is what it lies on)
		IPhysicalEntity* skip[2] = { pMe->GetEntity()->GetPhysics(), pLoot->GetPhysics() };
		AABB box;
		pLoot->GetWorldBounds(box);
		const Vec3 middle = box.IsEmpty() ? at : box.GetCenter();
		const float away = eye.GetDistance(middle);
		ray_hit wall;
		const bool reach = away < 2.2f
			&& !(FirstBlock(eye, middle - eye, ent_static | ent_rigid | ent_sleeping_rigid, skip, 2, wall, eT_Walk) && wall.dist < away - 0.4f);
		s_bot.lootNear = min(s_bot.lootNear, away);
		if (!MoveTo(pMe, at, reach ? 1.0f : 0.2f, !fighting, false, ""))
		{
			if (Dist2D(at, pos) < 4.0f)
				LookAt(eye, at, 6.0f);
		}
		else
		{
			LookAt(eye, at, 6.0f);
			COffHand* pOffHand = static_cast<COffHand*>(pMe->GetWeaponByClass(CItem::sOffHandClass));
			// what the use key would take: a loose thing lying on the weapon
			// (a spade, a box, a chair) goes into the left hand instead, and
			// the others see the body keep that hand up at the head
			IEntity* pOver = 0;
			if (IWorldQuery* pQuery = pMe->GetGameObject()->GetWorldQuery())
				if (const ray_hit* pRay = pQuery->GetLookAtPoint(3.0f))
					pOver = gEnv->pEntitySystem->GetEntityFromPhysics(pRay->pCollider);
			const bool blocked = pOver && pOver != pLoot && !g_pGame->GetIGameFramework()->GetIItemSystem()->GetItem(pOver->GetId())
				&& pOver->GetPhysics() && pOver->GetPhysics()->GetType() == PE_RIGID;
			if (blocked)
			{
				if (s_bot.useBlocked <= 0.0f)
					s_bot.useBlocked = Now();
				else if (Now() - s_bot.useBlocked > 3.0f)
				{
					Event("leaves the %s: a %s lies on it", pLoot->GetClass()->GetName(), pOver->GetName());
					GiveUpLoot(pLoot, 0);
					s_bot.loot = 0;
					s_bot.useBlocked = 0.0f;
					return false;
				}
			}
			else
				s_bot.useBlocked = 0.0f;
			if (!blocked && Now() >= s_bot.useAt && (!pOffHand || pOffHand->GetOffHandState() == eOHS_INIT_STATE))
			{
				Press(pMe, g_pGame->Actions().use, true);
				Press(pMe, g_pGame->Actions().use, false);
				s_bot.useAt = Now() + 1.2f;
				++s_bot.lootUses;
			}
		}
		s_bot.doing.Format("picking up the %s", pLoot->GetClass()->GetName());
		return true;
	}

	// hurt in a fight: away from the enemy, out of its sight, down low until
	// the suit has healed it (it would die standing there); true meanwhile
	bool Survive(CPlayer* pMe, const Vec3& pos, const Vec3& eye, IActor* pEnemy)
	{
		const float health = (float)pMe->GetHealth() / (float)max(1, pMe->GetMaxHealth());
		const bool underFire = Now() - s_bot.hitAt < 2.0f;
		// shot by someone it does not see (from afar), or recovering: the
		// nearest enemy is the danger
		if (!pEnemy && (s_bot.retreating || Now() - s_bot.hitAt < 4.0f))
		{
			float best = 70.0f;
			IActorIteratorPtr it = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
			while (IActor* pActor = it->Next())
				if (Hostile(pActor) && pActor->GetEntity()->GetWorldPos().GetDistance(pos) < best)
				{
					best = pActor->GetEntity()->GetWorldPos().GetDistance(pos);
					pEnemy = pActor;
				}
		}
		if (!pEnemy)
		{
			if (s_bot.retreating)
			{
				s_bot.recoveredAt = Now();
				if (CNanoSuit* pSuit = pMe->GetNanoSuit())
					if (pSuit->GetMode() == NANOMODE_CLOAK)
						pSuit->SetMode(NANOMODE_DEFENSE);
			}
			s_bot.retreating = false;
			return false;
		}
		// falls back badly hurt (just back from it: only when worse still);
		// back to it only healed and after a while (no running to and fro)
		const float limit = Now() - s_bot.recoveredAt < 8.0f ? 0.35f : 0.6f;
		if (!s_bot.retreating && health < limit)
		{
			// cloaked the soldiers lose sight of it, as of the player
			CNanoSuit* pSuit = pMe->GetNanoSuit();
			if (pSuit && pSuit->GetSuitEnergy() > 40.0f && pSuit->GetMode() != NANOMODE_CLOAK)
			{
				pSuit->SetMode(NANOMODE_CLOAK);
				Event("cloaks to get away");
			}
			s_bot.retreating = true;
			s_bot.retreatSince = Now();
			s_bot.haveCover = false;
			s_bot.coverAskAt = 0.0f;
			Event("hurt (health %d): falls back to cover", pMe->GetHealth());
		}
		else if (s_bot.retreating && health > 0.95f && Now() - s_bot.retreatSince > 6.0f)
		{
			if (CNanoSuit* pSuit = pMe->GetNanoSuit())
				if (pSuit->GetMode() != NANOMODE_DEFENSE)
					pSuit->SetMode(NANOMODE_DEFENSE);
			s_bot.retreating = false;
			s_bot.recoveredAt = Now();
			Event("recovered (health %d): back to it", pMe->GetHealth());
		}
		if (!s_bot.retreating)
			return false;
		Fire(pMe, false);
		const Vec3 enemyAim = AimPoint(pEnemy->GetEntity());
		const bool seen = Visible(pMe, eye, pEnemy->GetEntity(), enemyAim);
		if (!s_bot.haveCover)
			AskCover(pEnemy);
		if (s_bot.haveCover)
		{
			if (!MoveTo(pMe, s_bot.cover, 0.8f, true, false, ""))
			{
				s_ctl.look = true;
				s_ctl.yaw = Yaw(s_ctl.moveDir.GetLengthSquared() > 0.01f ? s_ctl.moveDir : s_bot.cover - pos);
				s_ctl.pitch = 0.0f;
				s_ctl.turnRate = 6.0f;
				s_bot.doing.Format("running to cover (health %d, %.0f m)", pMe->GetHealth(), Dist2D(s_bot.cover, pos));
			}
			else
			{
				// in cover: down low, watching where the danger is
				s_ctl.crouch = true;
				LookAt(eye, enemyAim, 3.0f);
				s_bot.doing.Format("in cover, recovering (health %d)", pMe->GetHealth());
				if (underFire && seen)
				{
					// no cover at all from there: another one
					s_bot.haveCover = false;
					s_bot.coverAskAt = 0.0f;
				}
			}
			return true;
		}
		if (underFire || seen)
		{
			// no cover known yet: away from all of them (not from one into
			// the arms of the others), toward the one it follows
			Vec3 away(ZERO);
			IActorIteratorPtr it = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
			while (IActor* pActor = it->Next())
			{
				if (!Hostile(pActor))
					continue;
				Vec3 d = pos - pActor->GetEntity()->GetWorldPos();
				d.z = 0;
				const float len = d.GetLength();
				if (len > 0.1f && len < 70.0f)
					away += d / (len * len);
			}
			away.NormalizeSafe(Vec3(0, 1, 0));
			// toward the one it follows, if he is not closer to the danger
			if (IActor* pLeader = HostPlayer())
			{
				Vec3 toLeader = pLeader->GetEntity()->GetWorldPos() - pos;
				toLeader.z = 0;
				const Vec3 danger = pEnemy->GetEntity()->GetWorldPos();
				if (pLeader->GetHealth() > 0 && toLeader.GetLength() > 6.0f
					&& Dist2D(pLeader->GetEntity()->GetWorldPos(), danger) > Dist2D(pos, danger))
					away = (away + toLeader.GetNormalized() * 0.6f).GetNormalizedSafe(away);
			}
			s_ctl.moveDir = Steer(pMe, pos, away);
			s_ctl.sprint = true;
			s_ctl.look = true;
			s_ctl.yaw = Yaw(s_ctl.moveDir);
			s_ctl.pitch = 0.0f;
			s_ctl.turnRate = 6.0f;
			s_bot.doing.Format("falling back (health %d)", pMe->GetHealth());
		}
		else
		{
			s_ctl.crouch = true;
			LookAt(eye, enemyAim, 3.0f);
			s_bot.doing.Format("out of sight, recovering (health %d)", pMe->GetHealth());
		}
		return true;
	}

	// standing its ground in a fight: side steps now and then, crouched in
	// between (a harder target than one standing still)
	void Dodge(const Vec3& pos, IActor* pEnemy)
	{
		if (s_ctl.moveDir.GetLengthSquared() > 0.01f)
			return;    // on its way somewhere anyway
		const float now = Now();
		if (now >= s_bot.dodgeUntil)
		{
			if (s_bot.dodgeSide != 0.0f)
			{
				s_bot.dodgeSide = 0.0f;
				s_bot.dodgeUntil = now + 1.5f + cry_frand();
			}
			else
			{
				s_bot.dodgeSide = cry_frand() < 0.5f ? -1.0f : 1.0f;
				s_bot.dodgeUntil = now + 0.7f + cry_frand() * 0.7f;
			}
		}
		if (s_bot.dodgeSide != 0.0f)
		{
			Vec3 to = pEnemy->GetEntity()->GetWorldPos() - pos;
			to.z = 0;
			to.NormalizeSafe(Vec3(0, 1, 0));
			Vec3 side(to.y * s_bot.dodgeSide, -to.x * s_bot.dodgeSide, 0);
			CPlayer* pMe = LocalPlayer();
			if (pMe && !ClearWay(pMe, pos, pos + side * 2.0f))
			{
				// a wall that side: the other one, or down low
				side = -side;
				s_bot.dodgeSide = -s_bot.dodgeSide;
				if (!ClearWay(pMe, pos, pos + side * 2.0f))
				{
					s_bot.dodgeSide = 0.0f;
					s_ctl.crouch = true;
					return;
				}
			}
			s_ctl.moveDir = side;
		}
		else
			s_ctl.crouch = true;
	}

	// where it aims now, the first thing hit is the enemy (or his vehicle):
	// no shots into a wall, a crate, a fence, a teammate
	bool LineOfFire(CPlayer* pMe, const Vec3& eye, IActor* pEnemy)
	{
		IPhysicalEntity* skip[2];
		int n = 0;
		if (IPhysicalEntity* p = pMe->GetEntity()->GetPhysics())
			skip[n++] = p;
		if (IItem* pItem = pMe->GetCurrentItem())
			if (IPhysicalEntity* p = pItem->GetEntity()->GetPhysics())
				skip[n++] = p;
		const Vec3 aim = AimPoint(pEnemy->GetEntity());
		const Vec3 look = pMe->GetViewRotation().GetColumn1();
		const float range = aim.GetDistance(eye) + 2.0f;
		ray_hit hit;
		if (!FirstBlock(eye, look * range, ent_all, skip, n, hit))
			return false;    // aimed at nothing (the enemy is not where it aims)
		IPhysicalEntity* pEnemyPhys = pEnemy->GetEntity()->GetPhysics();
		if (hit.pCollider == pEnemyPhys)
			return true;
		if (IVehicle* pVehicle = pEnemy->GetLinkedVehicle())
			if (hit.pCollider == pVehicle->GetEntity()->GetPhysics())
				return true;
		// the hit is right at him (his weapon, a part of him)
		return hit.pt.GetDistance(aim) < 0.8f;
	}

	// fighting from cover: to it, then up shooting a while, down a while
	// (FightStep aims and shoots first); false: no cover known
	bool FightFromCover(CPlayer* pMe, const Vec3& pos, IActor* pEnemy)
	{
		if (!s_bot.haveCover)
			return false;
		if (!MoveTo(pMe, s_bot.cover, 0.8f, false, false, ""))
		{
			s_bot.doing.Format("to cover (%.0f m), fighting %s", Dist2D(s_bot.cover, pos), pEnemy->GetEntity()->GetName());
			return true;
		}
		if (Now() >= s_bot.peekUntil)
		{
			s_bot.peekUp = !s_bot.peekUp;
			s_bot.peekUntil = Now() + (s_bot.peekUp ? 1.4f + cry_frand() : 1.0f + cry_frand() * 0.8f);
		}
		if (!s_bot.peekUp)
		{
			s_ctl.crouch = true;
			Fire(pMe, false);
			// shot even down there: no cover from it here
			if (Now() - s_bot.hitAt < 0.3f)
			{
				s_bot.haveCover = false;
				s_bot.coverAskAt = 0.0f;
			}
		}
		s_bot.doing.Format("in cover, fighting %s", pEnemy->GetEntity()->GetName());
		return true;
	}

	// the vehicle's gun: its trigger
	void VehicleFire(CPlayer* pMe, IVehicle* pVehicle, bool on)
	{
		static bool s_held = false;
		if (on == s_held)
			return;
		s_held = on;
		pVehicle->OnAction(eVAI_Attack1, on ? eAAM_OnPress : eAAM_OnRelease, on ? 1.0f : 0.0f, pMe->GetEntityId());
		if (on)
			++s_bot.bursts;
	}

	// at a vehicle's gun: the turret follows the view, turned as with the
	// mouse (the vehicle's rotate actions; how much a unit turns it and which
	// way are found out as it goes), firing in bursts when on an enemy with
	// nothing else in the way
	void VehicleGunner(CPlayer* pMe, IVehicle* pVehicle)
	{
		static float s_signYaw = -1.0f, s_signPitch = -1.0f;
		static float s_lastYawErr = 0.0f, s_lastPitchErr = 0.0f;
		static int s_worseYaw = 0, s_worsePitch = 0;
		const CCamera& cam = gEnv->pSystem->GetViewCamera();
		const Vec3 eye = cam.GetPosition();
		const Vec3 look = cam.GetViewdir().GetNormalizedSafe(Vec3(0, 1, 0));
		EntityId enemy = PickEnemy(pMe, eye, 150.0f);
		IActor* pEnemy = ActorOf(enemy);
		if (enemy != s_bot.enemy)
		{
			if (enemy && enemy != s_bot.lastFought)
				Event("fighting %s from %s's gun", NameOf(enemy), pVehicle->GetEntity()->GetName());
			if (enemy)
				s_bot.lastFought = enemy;
			s_bot.enemy = enemy;
		}
		if (!pEnemy)
		{
			VehicleFire(pMe, pVehicle, false);
			s_bot.doing.Format("at the gun of %s", pVehicle->GetEntity()->GetName());
			return;
		}
		const Vec3 aim = AimPoint(pEnemy->GetEntity());
		const Vec3 want = (aim - eye).GetNormalizedSafe(look);
		const float yawErr = Wrap(Yaw(want) - Yaw(look));
		const float pitchErr = Pitch(want) - Pitch(look);
		// a turn the wrong way (the error grows while turning): the other way
		if (fabsf(yawErr) > fabsf(s_lastYawErr) + 0.002f && fabsf(s_lastYawErr) > 0.02f)
		{
			if (++s_worseYaw > 6)
			{
				s_signYaw = -s_signYaw;
				s_worseYaw = 0;
			}
		}
		else
			s_worseYaw = 0;
		if (fabsf(pitchErr) > fabsf(s_lastPitchErr) + 0.002f && fabsf(s_lastPitchErr) > 0.02f)
		{
			if (++s_worsePitch > 6)
			{
				s_signPitch = -s_signPitch;
				s_worsePitch = 0;
			}
		}
		else
			s_worsePitch = 0;
		s_lastYawErr = yawErr;
		s_lastPitchErr = pitchErr;
		static float s_traceAt = 0.0f;
		if (Now() >= s_traceAt)
		{
			s_traceAt = Now() + 1.0f;
			CryLogAlways("[CoopAgent] gun aim at %s: yaw off %.3f pitch off %.3f (signs %.0f %.0f), view (%.2f %.2f %.2f)",
				pEnemy->GetEntity()->GetName(), yawErr, pitchErr, s_signYaw, s_signPitch, look.x, look.y, look.z);
		}
		const float gain = 120.0f;
		if (fabsf(yawErr) > 0.005f)
			pVehicle->OnAction(eVAI_RotateYaw, eAAM_OnPress, s_signYaw * clamp_tpl(yawErr * gain, -25.0f, 25.0f), pMe->GetEntityId());
		if (fabsf(pitchErr) > 0.005f)
			pVehicle->OnAction(eVAI_RotatePitch, eAAM_OnPress, s_signPitch * clamp_tpl(pitchErr * gain, -25.0f, 25.0f), pMe->GetEntityId());
		// on target, seen, nothing else in the way (its own vehicle aside)
		const bool onTarget = fabsf(yawErr) + fabsf(pitchErr) < 0.05f;
		bool clear = false;
		if (onTarget)
		{
			IPhysicalEntity* skip[2];
			int n = 0;
			if (IPhysicalEntity* p = pMe->GetEntity()->GetPhysics())
				skip[n++] = p;
			if (IPhysicalEntity* p = pVehicle->GetEntity()->GetPhysics())
				skip[n++] = p;
			ray_hit hit;
			if (FirstBlock(eye, look * (aim.GetDistance(eye) + 2.0f), ent_all, skip, n, hit))
			{
				clear = hit.pCollider == pEnemy->GetEntity()->GetPhysics() || hit.pt.GetDistance(aim) < 1.0f;
				if (IVehicle* pTheirs = pEnemy->GetLinkedVehicle())
					clear = clear || hit.pCollider == pTheirs->GetEntity()->GetPhysics();
			}
			if (!clear)
				++s_bot.heldFire;
		}
		else
			++s_bot.turning;
		// bursts
		const float now = Now();
		if (clear && now >= s_bot.pauseUntil && now < s_bot.burstUntil)
			VehicleFire(pMe, pVehicle, true);
		else
		{
			VehicleFire(pMe, pVehicle, false);
			if (now >= s_bot.burstUntil)
			{
				s_bot.pauseUntil = now + 0.2f + cry_frand() * 0.3f;
				s_bot.burstUntil = s_bot.pauseUntil + 0.5f + cry_frand() * 0.6f;
			}
		}
		s_bot.doing.Format("at the gun of %s, fighting %s (%.0f m)", pVehicle->GetEntity()->GetName(), pEnemy->GetEntity()->GetName(), aim.GetDistance(eye));
	}

	// one step of a fight with an enemy: turn to it, the weapon ready, fire
	// in bursts when on target, seen, and nothing else in the line of fire
	void FightStep(CPlayer* pMe, const Vec3& eye, IActor* pEnemy)
	{
		const Vec3 aim = AimPoint(pEnemy->GetEntity());
		const bool seen = Visible(pMe, eye, pEnemy->GetEntity(), aim);
		LookAt(eye, aim, 5.0f);
		float yaw, pitch;
		ViewAngles(pMe, yaw, pitch);
		const float off = fabsf(Wrap(s_ctl.yaw - yaw)) + fabsf(s_ctl.pitch - pitch);
		int clip = -1, reserve = -1;
		IWeapon* pWeapon = CurrentWeapon(pMe, &clip, &reserve);
		IItem* pCurrent = pMe->GetCurrentItem();
		const bool fightsWithIt = pWeapon && pCurrent && WeaponRank(pCurrent->GetEntity()->GetClass()->GetName()) > 0;
		if (!fightsWithIt)
		{
			// fists, nothing, a tool: the best weapon it has
			Fire(pMe, false);
			ChooseWeapon(pMe, true);
		}
		else if (clip == 0 && Now() > s_bot.reloadAt)
		{
			Fire(pMe, false);
			if (reserve > 0)
			{
				Press(pMe, g_pGame->Actions().reload, true);
				Press(pMe, g_pGame->Actions().reload, false);
				s_bot.reloadAt = Now() + 2.5f;
			}
			else
				ChooseWeapon(pMe, true);    // none left for it: another weapon
		}
		else if (!seen && clip != 0)
		{
			Fire(pMe, false);
			++s_bot.unseen;
		}
		else if (off >= 0.06f && clip != 0)
		{
			Fire(pMe, false);
			++s_bot.turning;
		}
		else if (seen && off < 0.06f && clip != 0 && !LineOfFire(pMe, eye, pEnemy))
		{
			// something else in the way (a wall, a crate, a fence): no shot
			Fire(pMe, false);
			++s_bot.heldFire;
		}
		else if (seen && off < 0.06f && clip != 0)
		{
			// bursts: held a while, then let go (single shot weapons fire again)
			const float now = Now();
			if (now >= s_bot.pauseUntil && now < s_bot.burstUntil)
			{
				if (!s_bot.attackHeld)
					++s_bot.bursts;
				Fire(pMe, true);
			}
			else if (now >= s_bot.burstUntil)
			{
				Fire(pMe, false);
				s_bot.pauseUntil = now + 0.15f + cry_frand() * 0.2f;
				s_bot.burstUntil = s_bot.pauseUntil + 0.3f + cry_frand() * 0.4f;
			}
			else
				Fire(pMe, false);
		}
		else
			Fire(pMe, false);
		s_bot.doing.Format("fighting %s (%.0f m%s)", pEnemy->GetEntity()->GetName(), aim.GetDistance(eye), seen ? "" : ", out of sight");
	}

	// ------------------------------------------------------------------
	// hand grenades: at enemies standing together, or at the one it fought
	// hiding behind cover where it last saw him; never with one of its own
	// close to where it lands

	struct SGrenade
	{
		int phase;              // 0 none, 1 turning to the throw, 2 the pin out (key held), 3 thrown
		float phaseAt, nextAt, scanAt;
		EntityId target;
		Vec3 at;                // where it should land
		Vec3 targetAt;          // where the one it throws at was to be then
		float yaw, pitch, flight;
		string why;
		bool ordered;           // told to (the AI agent); orderTarget "" = the best one
		string orderTarget;
		float orderAt;
		EntityId hidden;        // the enemy it fought: where and when it last saw him
		Vec3 hiddenPos;
		float hiddenSeenAt;
		int switches;
		int thrown;
		float steadySince;      // aimed at the throw since (the host's game must have that aim too)
		int kills;              // how many it is to kill
		int need;               // and needs to (else it does not throw)
		float checkAt;          // when it looks again whether they are still there
		bool rechecked;         // looked again before letting go
		SGrenade(): phase(0), phaseAt(0), nextAt(0), scanAt(0), target(0), at(ZERO), targetAt(ZERO), yaw(0), pitch(0), flight(0), ordered(false), orderAt(0),
			hidden(0), hiddenPos(ZERO), hiddenSeenAt(-100), switches(0), thrown(0), steadySince(-1), kills(0), rechecked(false), need(2), checkAt(0) {}
	} s_gren;

	const float GRENADE_COOLDOWN = 14.0f;
	const float GRENADE_SAFE = 12.0f;       // nobody of its own this close to where it lands
	const float GRENADE_NEAR = 16.0f;       // it goes off up to 15 m around: never closer to itself

	IEntityClass* GrenadeClass()
	{
		static IEntityClass* s_pClass = 0;
		if (!s_pClass)
			s_pClass = gEnv->pEntitySystem->GetClassRegistry()->FindClass("explosivegrenade");
		return s_pClass;
	}

	int Grenades(CPlayer* pMe)
	{
		IInventory* pInventory = pMe->GetInventory();
		return pInventory && GrenadeClass() ? pInventory->GetAmmoCount(GrenadeClass()) : 0;
	}

	// the throw from the eye to there (22 m/s, 1.5 times that in strength
	// mode): the low arc, or the high one when the low one is blocked; it
	// must land before its 2.5 s fuse and nothing solid may be on the way
	// (leaves and grass let it through); false: out of reach or blocked
	bool GrenadeArc(CPlayer* pMe, const Vec3& eye, const Vec3& to, float& pitch, float& flight)
	{
		float v = 22.0f;
		if (CNanoSuit* pSuit = pMe->GetNanoSuit())
			if (pSuit->GetMode() == NANOMODE_STRENGTH)
				v *= 1.5f;
		const float g = 9.8f;
		const float d = Dist2D(eye, to);
		const float h = to.z - eye.z;
		const float disc = v * v * v * v - g * (g * d * d + 2.0f * h * v * v);
		if (disc < 0.0f || d < 1.0f)
			return false;
		const Vec3 flat = Vec3(to.x - eye.x, to.y - eye.y, 0).GetNormalizedSafe(Vec3(0, 1, 0));
		IPhysicalEntity* pSkip = pMe->GetEntity()->GetPhysics();
		for (int k = 0; k < 2; ++k)
		{
			const float a = atan_tpl((v * v + (k ? 1.0f : -1.0f) * sqrt_tpl(disc)) / (g * d));
			const float t = d / (v * cos_tpl(a));
			if (t > 2.1f)
				continue;
			// it leaves the hand, lower and to the right of the eyes: nothing
			// close in front there either (a pallet, a rock, the cover it
			// stands behind would throw it back at its feet)
			const Vec3 dir0 = flat * cos_tpl(a) + Vec3(0, 0, sin_tpl(a));
			const Vec3 right(flat.y, -flat.x, 0);
			bool clear = true;
			static const float s_hand[4][2] = { { 0.35f, -0.35f }, { 0.35f, 0.0f }, { -0.25f, -0.2f }, { 0.0f, -0.5f } };
			for (int h = 0; h < 4 && clear; ++h)
			{
				ray_hit hit;
				const Vec3 from = eye + right * s_hand[h][0] + Vec3(0, 0, s_hand[h][1]) - flat * 0.2f;
				if (FirstBlock(from, dir0 * 3.0f, ent_static | ent_terrain | ent_rigid | ent_sleeping_rigid, &pSkip, pSkip ? 1 : 0, hit, eT_Walk))
					clear = false;
			}
			// the arc in 16 pieces, three rays side by side (it is no point: a
			// post or a branch just beside the middle throws it back)
			Vec3 prev = eye;
			for (int i = 1; i <= 16 && clear; ++i)
			{
				const float ti = t * i / 16.0f;
				const Vec3 p = eye + flat * (v * cos_tpl(a) * ti) + Vec3(0, 0, v * sin_tpl(a) * ti - 0.5f * g * ti * ti);
				for (int s = -1; s <= 1 && clear; ++s)
				{
					const Vec3 side = right * (0.2f * s);
					ray_hit hit;
					if (FirstBlock(prev + side, p - prev, ent_static | ent_terrain | ent_rigid | ent_sleeping_rigid, &pSkip, pSkip ? 1 : 0, hit, eT_Walk)
						&& hit.pt.GetDistance(to) > 2.5f)
						clear = false;
				}
				prev = p;
			}
			if (clear)
			{
				pitch = a;
				flight = t;
				return true;
			}
		}
		return false;
	}

	// nobody of its own (players, friendly soldiers) close to there
	bool GrenadeSafe(const Vec3& at)
	{
		IActorIteratorPtr it = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = it->Next())
		{
			if (pActor->GetHealth() <= 0 || pActor->GetEntity()->IsHidden() || Hostile(pActor))
				continue;
			if (pActor->GetEntity()->GetWorldPos().GetDistance(at) < GRENADE_SAFE)
				return false;
		}
		return true;
	}

	// where the enemies go: their speed from where they were half a second
	// ago (a running one is not where it lands)
	struct STrack { Vec3 pos; float at; Vec3 vel; int still; };   // still: half seconds standing in a row
	std::map<EntityId, STrack> s_tracks;

	Vec3 Predict(IActor* pActor, float ahead)
	{
		const Vec3 p = pActor->GetEntity()->GetWorldPos();
		std::map<EntityId, STrack>::iterator it = s_tracks.find(pActor->GetEntityId());
		return it == s_tracks.end() ? p : p + it->second.vel * ahead;
	}

	void TrackEnemies()
	{
		const float now = Now();
		IActorIteratorPtr it = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = it->Next())
		{
			if (!Hostile(pActor))
				continue;
			const Vec3 p = pActor->GetEntity()->GetWorldPos();
			STrack& t = s_tracks[pActor->GetEntityId()];
			if (t.at <= 0.0f || now - t.at > 2.0f)
			{
				t.pos = p;
				t.at = now;
				t.vel.zero();
				t.still = 0;
			}
			else if (now - t.at >= 0.5f)
			{
				t.vel = (p - t.pos) / (now - t.at);
				t.still = t.vel.GetLength() < 1.0f ? t.still + 1 : 0;
				t.vel.z = 0.0f;
				t.pos = p;
				t.at = now;
			}
		}
	}

	// a grenade's damage to an enemy so far from where it goes off, as the
	// game works it out (all of it within 5 m, then less and less to 15 m),
	// less behind something solid
	float GrenadeDamage(const Vec3& at, const Vec3& foe, float extra)
	{
		const float d = foe.GetDistance(at) + extra;
		float effect = 1.0f;
		if (d > 5.0f)
		{
			const float k = max(0.0f, (10.0f - min(d - 5.0f, 10.0f)) / 10.0f);
			effect = k * k;
		}
		IPhysicalEntity* pSkip = 0;
		ray_hit hit;
		if (FirstBlock(at + Vec3(0, 0, 0.5f), foe + Vec3(0, 0, 1.0f) - (at + Vec3(0, 0, 0.5f)), ent_static | ent_rigid | ent_sleeping_rigid, &pSkip, 0, hit, eT_Walk))
			effect *= 0.4f;
		return 250.0f * effect;
	}

	// the enemies it knows of now, where they will be when it goes off, their health
	struct SFoe { IActor* pActor; Vec3 at; bool known; bool still; float health; };

	void GrenadeFoes(CPlayer* pMe, const Vec3& pos, const Vec3& eye, float ahead, std::vector<SFoe>& foes)
	{
		IActorIteratorPtr it = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = it->Next())
		{
			if (!Hostile(pActor) || pActor->GetLinkedVehicle())
				continue;
			IEntity* pEntity = pActor->GetEntity();
			if (pEntity->GetWorldPos().GetDistance(pos) > 50.0f)
				continue;
			const bool seen = Visible(pMe, eye, pEntity, AimPoint(pEntity));
			const bool hiding = !seen && pActor->GetEntityId() == s_gren.hidden && Now() - s_gren.hiddenSeenAt < 8.0f
				&& pEntity->GetWorldPos().GetDistance(s_gren.hiddenPos) < 4.0f;
			std::map<EntityId, STrack>::const_iterator tr = s_tracks.find(pActor->GetEntityId());
			const bool still = tr != s_tracks.end() && tr->second.still >= 2;
			// a soldier has 180 on the host (this game may scale it)
			const float health = 180.0f * min(1.0f, (float)pActor->GetHealth() / (float)max(1, pActor->GetMaxHealth()));
			SFoe f = { pActor, Predict(pActor, ahead), seen || hiding, still, health };
			foes.push_back(f);
		}
	}

	// how many it would kill (and hurt) going off there: with room for where
	// it lands and where they step meanwhile (2.5 m; 4 m for a running one)
	int GrenadeKills(const std::vector<SFoe>& foes, const Vec3& at, int* pHurt = 0)
	{
		int kills = 0, hurt = 0;
		for (size_t k = 0; k < foes.size(); ++k)
		{
			const float dmg = GrenadeDamage(at, foes[k].at, foes[k].still ? 2.5f : 4.0f);
			if (dmg >= foes[k].health)
				++kills;
			else if (dmg > 30.0f)
				++hurt;
		}
		if (pHurt)
			*pHurt = hurt;
		return kills;
	}

	// where to throw: grenades are few to waste one on, so only where it
	// would kill three (two when it has four or more), counted with the
	// game's damage, their health (a hurt one dies farther away) and room for
	// where it lands, at where they will be when it goes off. The places
	// tried: at each enemy it knows of, between each two, the middle of
	// each group. 16 to 32 m away, a clear throw, none of its own close.
	// Told to (the AI agent): one will do
	bool PickGrenadeTarget(CPlayer* pMe, const Vec3& pos, const Vec3& eye, const string& name, bool ordered, int left)
	{
		// measured: where it counted on two it killed 1.1 a throw (they step
		// away, the soldiers dodge a grenade they see), where it counted on
		// three it killed three: three, or two when it has grenades to spare
		const int need = ordered ? 1 : (left >= 4 ? 2 : 3);
		// the throw starts in about 1.5 s and flies about 1.2 s
		std::vector<SFoe> foes;
		GrenadeFoes(pMe, pos, eye, 2.7f, foes);
		std::vector<std::pair<Vec3, size_t> > places;
		for (size_t i = 0; i < foes.size(); ++i)
		{
			if (!foes[i].known)
				continue;
			if (!name.empty() && stricmp(name.c_str(), foes[i].pActor->GetEntity()->GetName()))
				continue;
			places.push_back(std::make_pair(foes[i].at, i));
			Vec3 sum(foes[i].at);
			int n = 1;
			for (size_t k = 0; k < foes.size(); ++k)
				if (k != i && foes[k].at.GetDistance(foes[i].at) < 9.0f)
				{
					places.push_back(std::make_pair((foes[i].at + foes[k].at) * 0.5f, i));
					if (foes[k].at.GetDistance(foes[i].at) < 6.0f)
					{
						sum += foes[k].at;
						++n;
					}
				}
			if (n > 2)
				places.push_back(std::make_pair(sum / (float)n, i));
		}
		float bestScore = 0.0f;
		for (size_t p = 0; p < places.size(); ++p)
		{
			Vec3 aim = places[p].first;
			const size_t i = places[p].second;
			aim.z = foes[i].at.z;
			int hurt = 0;
			const int kills = GrenadeKills(foes, aim, &hurt);
			if (kills < need)
				continue;
			const float d = Dist2D(aim, pos);
			if (d < GRENADE_NEAR || d > 32.0f || fabsf(aim.z - pos.z) > 6.0f)
				continue;
			// more of them killed first, then more of them hurt, then nearer
			const float score = kills * 10.0f + hurt * 2.0f + (32.0f - d) * 0.05f;
			if (score <= bestScore || !GrenadeSafe(aim))
				continue;
			// it skids on 1.4 m on landing (23 throws measured): it lands short
			const Vec3 throwAt = aim - Vec3(aim.x - pos.x, aim.y - pos.y, 0).GetNormalizedSafe(Vec3(0, 1, 0)) * 1.4f;
			float pitch, flight;
			if (!GrenadeArc(pMe, eye, throwAt + Vec3(0, 0, 0.3f), pitch, flight))
				continue;
			bestScore = score;
			s_gren.target = foes[i].pActor->GetEntityId();
			s_gren.at = aim;
			s_gren.targetAt = foes[i].at;
			s_gren.yaw = Yaw(throwAt - eye);
			s_gren.need = need;
			s_gren.pitch = pitch;
			s_gren.flight = flight;
			s_gren.kills = kills;
			s_gren.why.Format("%d to be killed, %d hurt", kills, hurt);
		}
		return bestScore > 0.0f;
	}

	// one step of it; true while it throws (no firing, no walking meanwhile)
	bool GrenadeStep(CPlayer* pMe, const Vec3& pos, const Vec3& eye, IActor* pEnemy)
	{
		TrackEnemies();
		if (pEnemy && Visible(pMe, eye, pEnemy->GetEntity(), AimPoint(pEnemy->GetEntity())))
		{
			s_gren.hidden = pEnemy->GetEntityId();
			s_gren.hiddenPos = pEnemy->GetEntity()->GetWorldPos();
			s_gren.hiddenSeenAt = Now();
		}
		COffHand* pOffHand = static_cast<COffHand*>(pMe->GetWeaponByClass(CItem::sOffHandClass));
		if (!pOffHand)
			return false;
		const float now = Now();
		if (s_gren.phase == 0)
		{
			if (s_gren.ordered && now - s_gren.orderAt > 8.0f)
			{
				Event("no grenade: %s", Grenades(pMe) <= 0 ? "none left" : "no enemy in reach (16-32 m) with a clear throw and none of its own close");
				s_gren.ordered = false;
			}
			if (!s_gren.ordered && (now < s_gren.nextAt || !s_bot.fireAtWill))
				return false;
			if (now < s_gren.scanAt || now < s_bot.reloadAt || now < s_bot.switchAt || Grenades(pMe) <= 0)
				return false;
			s_gren.scanAt = now + 0.5f;
			if (pOffHand->GetOffHandState() != eOHS_INIT_STATE)
				return false;
			// the explosive ones in the left hand (not the flashbangs, the smoke)
			IFireMode* pMode = pOffHand->GetFireMode(pOffHand->GetCurrentFireMode());
			if (!pMode || pMode->GetAmmoType() != GrenadeClass())
			{
				if (s_gren.switches < 4)
				{
					++s_gren.switches;
					Press(pMe, g_pGame->Actions().handgrenade, true);
					Press(pMe, g_pGame->Actions().handgrenade, false);
				}
				return false;
			}
			s_gren.switches = 0;
			if (!PickGrenadeTarget(pMe, pos, eye, s_gren.ordered ? s_gren.orderTarget : string(), s_gren.ordered, Grenades(pMe)))
				return false;
			s_gren.ordered = false;
			s_gren.phase = 1;
			s_gren.phaseAt = now;
			s_gren.steadySince = -1.0f;
			s_gren.rechecked = false;
			Fire(pMe, false);
			Event("throws a grenade at %s (%s, %.0f m, %.0f degrees up, %.1f s in the air, at %s from %s)", NameOf(s_gren.target), s_gren.why.c_str(),
				Dist2D(s_gren.at, pos), RAD2DEG(s_gren.pitch), s_gren.flight, Vec(s_gren.at).c_str(), Vec(pos).c_str());
		}
		// before the pin is out: still there? (the throw takes 2 or 3 s; a
		// group that broke up is not worth a grenade any more)
		if (s_gren.phase == 1 && now >= s_gren.checkAt)
		{
			s_gren.checkAt = now + 0.25f;
			std::vector<SFoe> foes;
			GrenadeFoes(pMe, pos, eye, 2.5f, foes);
			const int kills = GrenadeKills(foes, s_gren.at);
			// chosen for three: still worth it with two left there
			if (kills < max(1, min(s_gren.need, 2)))
			{
				Event("no grenade after all: they moved (%d to be killed there now)", kills);
				s_gren.phase = 0;
				s_gren.nextAt = now + 2.0f;
				return false;
			}
		}
		Fire(pMe, false);
		s_ctl.look = true;
		s_ctl.yaw = s_gren.yaw;
		s_ctl.pitch = s_gren.pitch;
		s_ctl.turnRate = 6.0f;
		s_ctl.moveDir.zero();
		float yaw, pitch;
		ViewAngles(pMe, yaw, pitch);
		const float off = fabsf(Wrap(s_gren.yaw - yaw)) + fabsf(s_gren.pitch - pitch);
		s_bot.doing.Format("throwing a grenade at %s", NameOf(s_gren.target));
		if (s_gren.phase == 1)
		{
			// a teammate walked there meanwhile, or it cannot turn there: no throw
			if (!GrenadeSafe(s_gren.at) || now - s_gren.phaseAt > 3.0f)
			{
				Event("no grenade after all: %s", now - s_gren.phaseAt > 3.0f ? "could not turn to the throw" : "one of its own came close to it");
				s_gren.phase = 0;
				s_gren.nextAt = now + 4.0f;
				return false;
			}
			// aimed and held there a moment: the host's game, which throws it,
			// has its aim a little later
			if (off >= 0.04f)
				s_gren.steadySince = -1.0f;
			else if (s_gren.steadySince < 0.0f)
				s_gren.steadySince = now;
			else if (now - s_gren.steadySince > 0.3f)
			{
				Press(pMe, g_pGame->Actions().grenade, true);
				s_gren.phase = 2;
				s_gren.phaseAt = now;
			}
			return true;
		}
		if (s_gren.phase == 2)
		{
			// the pin out (the hand ready), then let go
			const bool ready = pOffHand->GetOffHandState() == eOHS_HOLDING_GRENADE;
			// about to let go: still as good there (they moved meanwhile)? Else
			// the best place now (the pin is out: it goes somewhere)
			if (ready && !s_gren.rechecked && now - s_gren.phaseAt > 0.4f)
			{
				s_gren.rechecked = true;
				std::vector<SFoe> foes;
				GrenadeFoes(pMe, pos, eye, s_gren.flight + 0.3f, foes);
				const int now_kills = GrenadeKills(foes, s_gren.at);
				if (now_kills < s_gren.kills)
				{
					const Vec3 was = s_gren.at;
					if (PickGrenadeTarget(pMe, pos, eye, string(), true, 99) && Dist2D(s_gren.at, was) > 1.0f)
						Event("throws elsewhere: %s (there now only %d)", s_gren.why.c_str(), now_kills);
				}
			}
			if (off >= 0.04f)
				s_gren.steadySince = now;
			if ((ready && now - s_gren.steadySince > 0.4f && now - s_gren.phaseAt > 0.5f) || now - s_gren.phaseAt > 3.0f)
			{
				Press(pMe, g_pGame->Actions().grenade, false);
				CryLogAlways("[CoopAgent] grenade let go: aimed at %s from %s", Vec(s_gren.at).c_str(), Vec(pos).c_str());
				s_gren.phase = 3;
				s_gren.phaseAt = now;
				++s_gren.thrown;
			}
			return true;
		}
		// thrown: the direction kept a moment (it leaves the hand a little later)
		if (now - s_gren.phaseAt < 0.6f && pOffHand->GetOffHandState() != eOHS_INIT_STATE)
			return true;
		if (now - s_gren.phaseAt < 0.6f)
			return true;
		s_gren.phase = 0;
		s_gren.nextAt = now + GRENADE_COOLDOWN + cry_frand() * 6.0f;
		return false;
	}

	void Think(float frameTime)
	{
		CPlayer* pMe = LocalPlayer();
		s_ctl.moveDir.zero();
		s_ctl.look = false;
		s_ctl.sprint = false;
		s_ctl.jump = false;
		s_ctl.crouch = false;
		if (!pMe || !InGame(pMe))
		{
			s_bot.doing = pMe ? "waiting to spawn" : "not in a game";
			return;
		}
		if (Now() >= s_bot.statsAt)
		{
			if (s_bot.bursts || s_bot.heldFire || s_bot.stuckTimes || s_bot.unseen || s_bot.turning || s_bot.localPlans)
				CryLogAlways("[CoopAgent] the last minute: %d bursts fired; no shot: %d frames something in the way, %d enemy not in sight, %d turning to him; stuck %d times, %d ways round found close by",
					s_bot.bursts, s_bot.heldFire, s_bot.unseen, s_bot.turning, s_bot.stuckTimes, s_bot.localPlans);
			s_bot.bursts = s_bot.heldFire = s_bot.stuckTimes = s_bot.unseen = s_bot.turning = s_bot.localPlans = 0;
			s_bot.statsAt = Now() + 60.0f;
		}
		IViewSystem* pView = g_pGame->GetIGameFramework()->GetIViewSystem();
		const bool down = pMe->GetHealth() <= 0;
		// what happened to it
		if (down != s_bot.wasDown)
		{
			Event(down ? "you are down: a teammate can revive you" : "you are up again");
			s_bot.wasDown = down;
		}
		if (!down && s_bot.lastHealth > 0 && pMe->GetHealth() < s_bot.lastHealth - 5)
		{
			Event("you were hit: health %d", pMe->GetHealth());
			s_bot.hitAt = Now();
		}
		s_bot.lastHealth = (float)pMe->GetHealth();
		{
			IActorIteratorPtr it = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
			while (IActor* pActor = it->Next())
			{
				if (!pActor->IsPlayer() || pActor == pMe)
					continue;
				const bool isDown = IsDown(pActor);
				bool& seen = s_bot.downSeen[pActor->GetEntityId()];
				if (isDown != seen)
					Event(isDown ? "%s is down" : "%s is up again", pActor->GetEntity()->GetName());
				seen = isDown;
			}
		}
		// something caught in the left hand by the use key (a crate, a bottle
		// next to a weapon it went to pick up): its rifle in one hand, the
		// other at its face; thrown away again
		if (COffHand* pOffHand = static_cast<COffHand*>(pMe->GetWeaponByClass(CItem::sOffHandClass)))
		{
			if (pOffHand->GetOffHandState() & (eOHS_HOLDING_OBJECT | eOHS_HOLDING_NPC))
			{
				if (s_bot.dropAt <= 0.0f)
					s_bot.dropAt = Now() + 1.0f;
				else if (Now() >= s_bot.dropAt)
				{
					Press(pMe, g_pGame->Actions().use, true);
					Press(pMe, g_pGame->Actions().use, false);
					s_bot.dropAt = Now() + 1.5f;
					Event("throws away what it caught in the left hand");
				}
			}
			else
				s_bot.dropAt = 0.0f;
		}
		if (down || (pView && pView->IsPlayingCutScene()))
		{
			Fire(pMe, false);
			HoldUse(pMe, false, 0);
			s_bot.doing = down ? "down, waiting for a teammate" : "watching a cutscene";
			return;
		}
		// the name the host sees
		// (again after joining again: a checkpoint load gives it the default
		// name back)
		static float s_nameAt = 0.0f;
		if (s_pName->GetString()[0] && strcmp(pMe->GetEntity()->GetName(), s_pName->GetString()) && Now() >= s_nameAt)
		{
			s_nameAt = Now() + 5.0f;
			s_bot.named = true;
			string cmd;
			cmd.Format("name %s", s_pName->GetString());
			gEnv->pConsole->ExecuteString(cmd.c_str());
		}

		const Vec3 pos = pMe->GetEntity()->GetWorldPos();
		const Vec3 eye = EyePos(pMe);
		IVehicle* pMyVehicle = pMe->GetLinkedVehicle();

		// where it is in its own game, for the host's game: the answer is
		// where the downed teammates lie there (a body falls its own way on
		// each computer), and should the two games have it apart, it is put
		// back where everybody sees it
		if (Now() >= s_bot.reportAt)
		{
			s_bot.reportAt = Now() + 2.0f;
			string where;
			where.Format("%.2f %.2f %.2f %s", pos.x, pos.y, pos.z, pMyVehicle ? pMyVehicle->GetEntity()->GetName() : "-");
			if (CGameRules* pRules = g_pGame->GetGameRules())
				pRules->CoopSendSyncToServer(15, 7, 0, where.c_str(), "", 0, 0.0f);
		}

		// a grenade throw begun is finished first (a second or two)
		if (s_gren.phase != 0)
		{
			if (pMyVehicle || pMe->GetHealth() <= 0)
			{
				if (s_gren.phase == 2)
					Press(pMe, g_pGame->Actions().grenade, false);
				s_gren.phase = 0;
			}
			else if (GrenadeStep(pMe, pos, eye, ActorOf(s_bot.enemy)))
				return;
		}

		// the one it follows
		IActor* pLeader = s_bot.leader.empty() ? HostPlayer() : ActorByName(s_bot.leader.c_str());
		if (!pLeader)
			pLeader = HostPlayer();

		// ---- reviving comes first (unless ordered to stay)
		IActor* pDowned = (s_bot.order == eO_Hold || s_bot.order == eO_Goto) ? 0 : DownedTeammate(pMe, pos, 80.0f);
		if (pDowned && !pMyVehicle)
		{
			// it makes its way there (around walls and rocks, by the host's
			// navigation), shooting back at whoever shoots at it; at the body
			// an enemy close by is dealt with first (it would kill both). The
			// body where the host's game has it: there the revive counts (in
			// its own game the body may have fallen elsewhere, over a fence)
			Vec3 body = pDowned->GetEntity()->GetWorldPos();
			Vec3 hostBody;
			if (CoopAgent::HostBodyPos(pDowned->GetEntityId(), hostBody))
			{
				if (Dist2D(hostBody, body) > 1.0f && s_bot.bodyToldFor != pDowned->GetEntityId())
				{
					s_bot.bodyToldFor = pDowned->GetEntityId();
					Event("%s lies at %.1f %.1f %.1f in the host's game, %.1f m from where its own game has him: goes by the host's",
						pDowned->GetEntity()->GetName(), hostBody.x, hostBody.y, hostBody.z, Dist2D(hostBody, body));
				}
				body = hostBody;
			}
			const float d = Dist2D(body, pos);
			if (s_bot.downedFor != pDowned->GetEntityId())
			{
				s_bot.downedFor = pDowned->GetEntityId();
				s_bot.downedSince = Now();
			}
			ChooseWeapon(pMe, false);
			EntityId enemy = s_bot.fireAtWill ? PickEnemy(pMe, eye, d > 3.0f ? 60.0f : 18.0f) : 0;
			IActor* pEnemy = ActorOf(enemy);
			if (enemy != s_bot.enemy)
			{
				if (enemy && enemy != s_bot.lastFought)
					Event("fighting %s on the way to %s", NameOf(enemy), pDowned->GetEntity()->GetName());
				if (enemy)
					s_bot.lastFought = enemy;
				s_bot.enemy = enemy;
			}
			// at the body: close, about level with it and nothing solid
			// between (behind a fence it goes round, it does not press the
			// use key through it). Close for long and still something
			// between (he lies half in a rock): from there then. A revive
			// under way goes on a step further off and with an enemy not
			// close
			const bool reach = d <= 1.6f && CanReach(pMe, pos, pDowned, body);
			if (d <= 1.6f && !reach)
			{
				if (s_bot.reachFailSince < 0)
				{
					s_bot.reachFailSince = Now();
					Event("next to %s, but something is in the way: around it", pDowned->GetEntity()->GetName());
				}
			}
			else
				s_bot.reachFailSince = -1;
			const bool reviving = s_bot.useHeld && s_bot.useFor == pDowned->GetEntityId() && CoopRevive::IsBeingRevived(pDowned->GetEntityId());
			const bool enemyClose = pEnemy && Dist2D(pEnemy->GetEntity()->GetWorldPos(), pos) < 10.0f;
			const bool atBody = reach || (d <= 1.6f && s_bot.reachFailSince >= 0 && Now() - s_bot.reachFailSince > 8.0f) || (reviving && d <= 2.6f);
			if (pEnemy && !(reviving && atBody && !enemyClose))
				HoldUse(pMe, false, 0);
			// badly hurt: out of the fire first (dead it revives nobody)
			if (Survive(pMe, pos, eye, pEnemy))
				return;
			if (!atBody && pEnemy && d > 4.0f && Dist2D(pEnemy->GetEntity()->GetWorldPos(), pos) < 40.0f
				&& Now() - s_bot.downedSince < 40.0f)
			{
				// an enemy close: the way is cleared first (from here or from
				// cover close by), not walked into
				s_bot.reviveSince = -1;
				FightStep(pMe, eye, pEnemy);
				if (Now() - s_bot.hitAt < 3.0f && !s_bot.haveCover)
					AskCover(pEnemy);
				if (!FightFromCover(pMe, pos, pEnemy))
					Dodge(pos, pEnemy);
				s_bot.doing.Format("clearing the way to %s (%.0f m): fighting %s", pDowned->GetEntity()->GetName(), d, pEnemy->GetEntity()->GetName());
			}
			else if (!atBody)
			{
				MoveTo(pMe, body, 0.9f, !pEnemy, false, pDowned->GetEntity()->GetName());
				s_bot.reviveSince = -1;
				if (pEnemy)
				{
					FightStep(pMe, eye, pEnemy);
					s_bot.doing.Format("making its way to %s (%.0f m), fighting %s", pDowned->GetEntity()->GetName(), d, pEnemy->GetEntity()->GetName());
				}
				else
				{
					Fire(pMe, false);
					HoldUse(pMe, false, 0);
					LookAt(eye, body + Vec3(0, 0, 1.2f), 6.0f);
					s_bot.doing.Format("running to revive %s (%.0f m)", pDowned->GetEntity()->GetName(), d);
				}
			}
			else if (pEnemy && !(reviving && !enemyClose))
			{
				s_ctl.moveDir.zero();
				s_ctl.crouch = true;
				FightStep(pMe, eye, pEnemy);
				s_bot.doing.Format("at %s, fighting %s first", pDowned->GetEntity()->GetName(), pEnemy->GetEntity()->GetName());
			}
			else
			{
				Fire(pMe, false);
				LookAt(eye, body, 6.0f);
				if (s_bot.reviveSince < 0)
					s_bot.reviveSince = Now();
				// the use key goes to the nearest downed teammate within reach
				if (!s_bot.useHeld || s_bot.useFor != pDowned->GetEntityId())
				{
					HoldUse(pMe, false, 0);
					HoldUse(pMe, true, pDowned->GetEntityId());
				}
				else if (Now() - s_bot.useSince > 4.0f && !CoopRevive::IsBeingRevived(pDowned->GetEntityId()))
				{
					// the host's game has it a little elsewhere (something in
					// the way there, not here): a step closer, then again
					HoldUse(pMe, false, 0);
					s_ctl.moveDir = Vec3(body.x - pos.x, body.y - pos.y, 0).GetNormalizedSafe(Vec3(0, 1, 0));
					// still not: a fresh way to the body (never put there: it walks)
					if (Now() - s_bot.reviveSince > 12.0f)
					{
						s_bot.pathAt = -100.0f;
						AskPath(body);
						s_bot.reviveSince = Now();
					}
				}
				else if (Now() - s_bot.useSince > 6.0f)
					HoldUse(pMe, false, 0);    // pressed again next frame
				s_bot.doing.Format("reviving %s", pDowned->GetEntity()->GetName());
			}
			return;
		}
		s_bot.reviveSince = -1;
		s_bot.reachFailSince = -1;
		s_bot.bodyToldFor = 0;
		s_bot.downedFor = 0;
		HoldUse(pMe, false, 0);
		if (s_bot.order == eO_Revive)
			s_bot.order = eO_Free;

		// ---- vehicles: along with the one it follows
		const bool follows = s_bot.order == eO_Free || s_bot.order == eO_Follow;
		if (follows && pLeader)
		{
			IVehicle* pLeaderVehicle = pLeader->GetLinkedVehicle();
			if (pLeaderVehicle && pMyVehicle != pLeaderVehicle)
			{
				if (pMyVehicle)
					Ask(2, "");
				else
				{
					// it walks (runs) to the vehicle and gets in next to it; put
					// in only when the vehicle is far off (it drove away)
					const Vec3 at = pLeaderVehicle->GetEntity()->GetWorldPos();
					const float d = Dist2D(at, pos);
					if (d < 5.0f || d > 80.0f)
					{
						Ask(3, pLeaderVehicle->GetEntity()->GetName());
						s_bot.doing.Format("getting into %s's vehicle", pLeader->GetEntity()->GetName());
					}
					else
					{
						MoveTo(pMe, at, 4.0f, true, false, "");
						s_bot.doing.Format("going to %s's vehicle (%.0f m)", pLeader->GetEntity()->GetName(), d);
						return;
					}
				}
			}
			else if (pMyVehicle && !pLeaderVehicle)
			{
				Ask(2, "");
				s_bot.doing = "getting out of the vehicle";
			}
		}
		if (pMyVehicle)
		{
			Fire(pMe, false);
			IVehicleSeat* pSeat = pMyVehicle->GetSeatForPassenger(pMe->GetEntityId());
			if (pSeat && pSeat->IsGunner() && !pSeat->IsDriver() && s_bot.fireAtWill)
			{
				VehicleGunner(pMe, pMyVehicle);
				return;
			}
			VehicleFire(pMe, pMyVehicle, false);
			if (s_bot.doing.empty() || s_bot.doing.find("vehicle") == string::npos)
				s_bot.doing.Format("riding in %s", pMyVehicle->GetEntity()->GetName());
			return;
		}

		// ---- fighting
		const bool fights = s_bot.fireAtWill || s_bot.order == eO_Attack;
		EntityId enemy = fights ? PickEnemy(pMe, eye, s_bot.order == eO_Attack ? 150.0f : 80.0f) : 0;
		if (enemy != s_bot.enemy)
		{
			if (s_bot.enemy)
			{
				IActor* pOld = ActorOf(s_bot.enemy);
				if (pOld && pOld->GetHealth() <= 0)
					Event("%s is dead", NameOf(s_bot.enemy));
			}
			if (enemy && enemy != s_bot.lastFought)
				Event("fighting %s", NameOf(enemy));
			if (enemy)
				s_bot.lastFought = enemy;
			s_bot.enemy = enemy;
		}
		IActor* pEnemy = ActorOf(enemy);
		bool aiming = false;
		// the best weapon in the hands (a rifle, not the pistol it may have
		// been given)
		ChooseWeapon(pMe, false);
		// out of ammo, or a better weapon lying close: it picks it up
		if (s_bot.order != eO_Hold && Loot(pMe, pos, eye, pEnemy != 0))
			return;
		if (Survive(pMe, pos, eye, pEnemy))
			return;
		// a grenade at enemies standing together or hiding behind cover
		if (GrenadeStep(pMe, pos, eye, pEnemy))
			return;
		// a cover is good for a while (the enemies move)
		if (s_bot.haveCover && Now() - s_bot.coverAt > 20.0f)
			s_bot.haveCover = false;
		if (pEnemy)
		{
			FightStep(pMe, eye, pEnemy);
			aiming = true;
			// shot at: it takes cover close by and fights from there (it
			// does not stand in the open)
			if (Now() - s_bot.hitAt < 3.0f && !s_bot.haveCover)
				AskCover(pEnemy);
		}
		else
		{
			Fire(pMe, false);
			if (s_bot.order == eO_Attack)
			{
				Event("the attack is over: back to following");
				s_bot.order = eO_Free;
				s_bot.target.clear();
			}
		}

		// ---- moving
		const bool fightsFromCover = aiming && pEnemy && s_bot.haveCover && s_bot.order != eO_Hold && s_bot.order != eO_Goto
			&& (!pLeader || Dist2D(s_bot.cover, pLeader->GetEntity()->GetWorldPos()) < 30.0f);
		if (fightsFromCover)
			FightFromCover(pMe, pos, pEnemy);
		else
		switch (s_bot.order)
		{
		case eO_Hold:
			if (s_bot.haveSpot && !MoveTo(pMe, s_bot.spot, 1.5f, false, false, ""))
				s_bot.doing = aiming ? s_bot.doing : string("going back to where it holds");
			else if (!aiming)
				s_bot.doing = "holding the position";
			break;
		case eO_Goto:
			if (MoveTo(pMe, s_bot.spot, 2.0f, true, false, ""))
			{
				Event("arrived: holding there");
				s_bot.order = eO_Hold;
			}
			else if (!aiming)
				s_bot.doing.Format("going to %s", Vec(s_bot.spot).c_str());
			break;
		case eO_Attack:
			if (pEnemy && (pEnemy->GetEntity()->GetWorldPos().GetDistance(pos) > 35.0f || !Visible(pMe, eye, pEnemy->GetEntity(), AimPoint(pEnemy->GetEntity()))))
				MoveTo(pMe, pEnemy->GetEntity()->GetWorldPos(), 20.0f, true, false, "");
			break;
		default:
			if (pLeader && InGame(pLeader) && pLeader->GetHealth() > 0)
			{
				const Vec3 at = pLeader->GetEntity()->GetWorldPos();
				// in a fight it keeps a little more room
				if (!MoveTo(pMe, at, aiming ? 10.0f : 4.0f, !aiming, true, pLeader->GetEntity()->GetName()))
				{
					if (!aiming)
						s_bot.doing.Format("following %s (%.0f m)", pLeader->GetEntity()->GetName(), Dist2D(at, pos));
				}
				else if (!aiming)
				{
					s_bot.doing.Format("next to %s", pLeader->GetEntity()->GetName());
					// looks where the leader looks
					const Vec3 f = pLeader->GetEntity()->GetWorldRotation().GetColumn1();
					s_ctl.look = true;
					s_ctl.yaw = Yaw(f);
					s_ctl.pitch = 0.0f;
					s_ctl.turnRate = 2.0f;
				}
			}
			else if (!aiming)
				s_bot.doing = "waiting (nobody to follow)";
			break;
		}
		if (aiming && pEnemy && !fightsFromCover)
			Dodge(pos, pEnemy);
		// facing the way it walks when it has nobody to aim at
		if (!aiming && s_ctl.moveDir.GetLengthSquared() > 0.01f)
		{
			s_ctl.look = true;
			s_ctl.yaw = Yaw(s_ctl.moveDir);
			s_ctl.pitch = 0.0f;
			s_ctl.turnRate = 4.0f;
		}
	}

	// ------------------------------------------------------------------
	// observe: what the companion knows, for the agent
	string Observe()
	{
		CPlayer* pMe = LocalPlayer();
		string j = "{";
		ILevel* pLevel = g_pGame->GetIGameFramework()->GetILevelSystem()->GetCurrentLevel();
		j += "\"level\":" + Esc(pLevel && pLevel->GetLevelInfo() ? pLevel->GetLevelInfo()->GetName() : "");
		IViewSystem* pView = g_pGame->GetIGameFramework()->GetIViewSystem();
		j += ",\"cutscene\":" + string(pView && pView->IsPlayingCutScene() ? "true" : "false");
		if (!pMe)
		{
			j += ",\"status\":\"not in a game (connecting to the host...)\"}";
			return j;
		}
		const Vec3 pos = pMe->GetEntity()->GetWorldPos();
		float yaw, pitch;
		ViewAngles(pMe, yaw, pitch);
		int clip = -1, reserve = -1;
		string weapon;
		CurrentWeapon(pMe, &clip, &reserve, &weapon);
		static const char* modes[] = { "speed", "strength", "cloak", "armor" };
		CNanoSuit* pSuit = pMe->GetNanoSuit();
		string s;
		s.Format(",\"me\":{\"name\":%s,\"health\":%d,\"maxHealth\":%d,\"down\":%s,\"suit\":%s,\"energy\":%.0f,\"weapon\":%s,\"clip\":%d,\"reserve\":%d,\"grenades\":%d,\"position\":%s,\"facing\":%s,\"vehicle\":%s}",
			Esc(pMe->GetEntity()->GetName()).c_str(), pMe->GetHealth(), pMe->GetMaxHealth(), pMe->GetHealth() <= 0 ? "true" : "false",
			Esc(pSuit && pSuit->GetMode() >= 0 && pSuit->GetMode() < 4 ? modes[pSuit->GetMode()] : "none").c_str(), pSuit ? pSuit->GetSuitEnergy() : 0.0f,
			Esc(weapon.c_str()).c_str(), clip, reserve, Grenades(pMe), Vec(pos).c_str(), Esc(Compass(pMe->GetViewRotation().GetColumn1())).c_str(),
			pMe->GetLinkedVehicle() ? Esc(pMe->GetLinkedVehicle()->GetEntity()->GetName()).c_str() : "null");
		j += s;
		s.Format(",\"order\":{\"type\":%s,\"leader\":%s,\"target\":%s,\"fireAtWill\":%s,\"doing\":%s}",
			Esc(s_orderNames[s_bot.order]).c_str(), Esc(s_bot.leader.c_str()).c_str(), Esc(s_bot.target.c_str()).c_str(),
			s_bot.fireAtWill ? "true" : "false", Esc(s_bot.doing.c_str()).c_str());
		j += s;
		// players
		j += ",\"teammates\":[";
		bool first = true;
		const Vec3 eye = EyePos(pMe);
		std::vector<std::pair<float, IActor*> > enemies;
		IActorIteratorPtr it = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = it->Next())
		{
			if (pActor == pMe)
				continue;
			const Vec3 p = pActor->GetEntity()->GetWorldPos();
			const Vec3 d = p - pos;
			if (pActor->IsPlayer())
			{
				s.Format("%s{\"name\":%s,\"health\":%d,\"down\":%s,\"host\":%s,\"distance\":%.0f,\"direction\":%s,\"bearing\":%.0f,\"vehicle\":%s}",
					first ? "" : ",", Esc(pActor->GetEntity()->GetName()).c_str(), pActor->GetHealth(), IsDown(pActor) ? "true" : "false",
					pActor->GetEntityId() == CoopAI::HostPlayerId() ? "true" : "false", d.GetLength(), Esc(Compass(d)).c_str(),
					RAD2DEG(Wrap(yaw - Yaw(d))), pActor->GetLinkedVehicle() ? Esc(pActor->GetLinkedVehicle()->GetEntity()->GetName()).c_str() : "null");
				j += s;
				first = false;
			}
			else if (Hostile(pActor) && d.GetLength() < 150.0f)
				enemies.push_back(std::make_pair(d.GetLength(), pActor));
		}
		j += "]";
		// enemies, the nearest first
		std::sort(enemies.begin(), enemies.end());
		j += ",\"enemies\":[";
		for (size_t i = 0; i < enemies.size() && i < 10; ++i)
		{
			IActor* pActor = enemies[i].second;
			const Vec3 d = pActor->GetEntity()->GetWorldPos() - pos;
			bool hostile = false;
			int alert = 0;
			CoopAI::GetMirroredAI(pActor->GetEntityId(), hostile, alert);
			const Vec3 aim = AimPoint(pActor->GetEntity());
			s.Format("%s{\"name\":%s,\"distance\":%.0f,\"direction\":%s,\"bearing\":%.0f,\"visible\":%s,\"alerted\":%d,\"vehicle\":%s}",
				i ? "," : "", Esc(pActor->GetEntity()->GetName()).c_str(), enemies[i].first, Esc(Compass(d)).c_str(), RAD2DEG(Wrap(yaw - Yaw(d))),
				Visible(pMe, eye, pActor->GetEntity(), aim) ? "true" : "false", alert,
				pActor->GetLinkedVehicle() ? Esc(pActor->GetLinkedVehicle()->GetEntity()->GetName()).c_str() : "null");
			j += s;
		}
		s.Format("],\"enemiesWithin150m\":%d", (int)enemies.size());
		j += s;
		// vehicles close by
		j += ",\"vehicles\":[";
		first = true;
		IVehicleIteratorPtr vit = g_pGame->GetIGameFramework()->GetIVehicleSystem()->CreateVehicleIterator();
		while (IVehicle* pVehicle = vit->Next())
		{
			IEntity* pEntity = pVehicle->GetEntity();
			const float d = pEntity->GetWorldPos().GetDistance(pos);
			if (pEntity->IsHidden() || d > 50.0f)
				continue;
			int free = 0;
			for (unsigned int i = 1; i <= pVehicle->GetSeatCount(); ++i)
				if (IVehicleSeat* pSeat = pVehicle->GetSeatById((TVehicleSeatId)i))
					free += pSeat->GetPassenger() ? 0 : 1;
			s.Format("%s{\"name\":%s,\"class\":%s,\"distance\":%.0f,\"freeSeats\":%d,\"destroyed\":%s}", first ? "" : ",",
				Esc(pEntity->GetName()).c_str(), Esc(pEntity->GetClass()->GetName()).c_str(), d, free, pVehicle->IsDestroyed() ? "true" : "false");
			j += s;
			first = false;
		}
		j += "]";
		// news since the last look
		j += ",\"events\":[";
		first = true;
		for (size_t i = 0; i < s_events.size(); ++i)
			if (s_events[i].n > s_eventsRead)
			{
				j += (first ? "" : ",") + Esc(s_events[i].text.c_str());
				first = false;
			}
		s_eventsRead = s_eventN;
		j += "],\"chat\":[";
		first = true;
		for (size_t i = 0; i < s_chat.size(); ++i)
			if (s_chat[i].n > s_chatRead)
			{
				j += string(first ? "" : ",") + "{\"from\":" + Esc(s_chat[i].from.c_str()) + ",\"text\":" + Esc(s_chat[i].text.c_str()) + "}";
				first = false;
			}
		s_chatRead = s_chatN;
		j += "]}";
		return j;
	}

	// ------------------------------------------------------------------
	// the bridge: 127.0.0.1:coop_agent_port, a request per line
	SOCKET s_listen = INVALID_SOCKET, s_client = INVALID_SOCKET;
	string s_in, s_out;
	bool s_wsa = false;
	float s_listenRetry = 0.0f;
	bool s_agentConnected = false;
	// a screenshot for a request: the frame after it is kept (not shown), then
	// read. The engine's own ScreenShot crashed the game (an allocation of a
	// negative size); this is the savegame thumbnail's way.
	string s_shotId;
	float s_shotSince = 0.0f;
	int s_shotFrames = 0;

	void Reply(const char* id, const string& json)
	{
		s_out += id;
		s_out += " ";
		s_out += json;
		s_out += "\n";
	}

	string Ok(const char* text)
	{
		return string("{\"ok\":true,\"result\":") + Esc(text) + "}";
	}

	string Fail(const char* text)
	{
		return string("{\"ok\":false,\"error\":") + Esc(text) + "}";
	}

	string Base64(const std::vector<unsigned char>& data)
	{
		static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		string out;
		out.reserve((data.size() + 2) / 3 * 4);
		for (size_t i = 0; i < data.size(); i += 3)
		{
			const unsigned int v = (data[i] << 16) | ((i + 1 < data.size() ? data[i + 1] : 0) << 8) | (i + 2 < data.size() ? data[i + 2] : 0);
			out += t[(v >> 18) & 63];
			out += t[(v >> 12) & 63];
			out += i + 1 < data.size() ? t[(v >> 6) & 63] : '=';
			out += i + 2 < data.size() ? t[v & 63] : '=';
		}
		return out;
	}

	void UpdateShot()
	{
		if (s_shotId.empty())
			return;
		// a frame drawn since the request (Update comes before the drawing)
		if (++s_shotFrames < 2 && Now() - s_shotSince < 5.0f)
			return;
		IRenderer* r = gEnv->pRenderer;
		const int w = r->GetWidth(), h = r->GetHeight();
		std::vector<unsigned char> jpeg;
		if (w > 0 && h > 0 && w <= 4096 && h <= 4096)
		{
			std::vector<unsigned char> rgb((size_t)w * h * 3);
			r->ReadFrameBuffer(&rgb[0], w, w, h, eRB_BackBuffer, false);
			CoopEncodeJpeg(&rgb[0], w, h, 70, jpeg);
		}
		r->EnableSwapBuffers(true);
		if (!jpeg.empty())
			Reply(s_shotId.c_str(), string("{\"ok\":true,\"mime\":\"image/jpeg\",\"data\":\"") + Base64(jpeg) + "\"}");
		else
			Reply(s_shotId.c_str(), Fail("the screenshot could not be taken"));
		s_shotId.clear();
	}

	void SetOrder(EOrder order)
	{
		s_bot.order = order;
		s_bot.stuck = 0;
		s_bot.farSince = -1;
	}

	void Handle(const string& line)
	{
		// "<id> <command> [arguments]"
		const size_t a = line.find(' ');
		const string id = a == string::npos ? line : line.substr(0, a);
		string rest = a == string::npos ? string() : line.substr(a + 1);
		const size_t b = rest.find(' ');
		string cmd = b == string::npos ? rest : rest.substr(0, b);
		string args = b == string::npos ? string() : rest.substr(b + 1);
		cmd.MakeLower();
		args.Trim();
		CPlayer* pMe = LocalPlayer();
		if (cmd == "observe")
			Reply(id.c_str(), "{\"ok\":true,\"result\":" + Observe() + "}");
		else if (cmd == "follow")
		{
			s_bot.leader = args;
			SetOrder(eO_Follow);
			Reply(id.c_str(), Ok(("following " + (args.empty() ? string("the host") : args)).c_str()));
		}
		else if (cmd == "free")
		{
			s_bot.leader.clear();
			s_bot.target.clear();
			s_bot.fireAtWill = true;
			SetOrder(eO_Free);
			Reply(id.c_str(), Ok("playing on its own: following the host, fighting, reviving"));
		}
		else if (cmd == "hold")
		{
			if (pMe)
			{
				s_bot.spot = pMe->GetEntity()->GetWorldPos();
				s_bot.haveSpot = true;
			}
			SetOrder(eO_Hold);
			Reply(id.c_str(), Ok("holding this position"));
		}
		else if (cmd == "goto")
		{
			Vec3 p(ZERO);
			IActor* pWho = 0;
			IEntity* pThing = 0;
			if (sscanf(args.c_str(), "%f %f %f", &p.x, &p.y, &p.z) == 3 || (pWho = ActorByName(args.c_str())) != 0 || (pThing = gEnv->pEntitySystem->FindEntityByName(args.c_str())) != 0)
			{
				if (pWho)
					p = pWho->GetEntity()->GetWorldPos();
				else if (pThing)
					p = pThing->GetWorldPos();
				s_bot.spot = p;
				s_bot.haveSpot = true;
				SetOrder(eO_Goto);
				Reply(id.c_str(), Ok(("going to " + Vec(p)).c_str()));
			}
			else
				Reply(id.c_str(), Fail("goto: x y z, or the name of a player or an object"));
		}
		else if (cmd == "move")
		{
			// meters in a direction: "forward 10", "north 20", "left 5"
			char dir[32] = "";
			float m = 0;
			if (pMe && sscanf(args.c_str(), "%31s %f", dir, &m) == 2)
			{
				float yaw, pitch;
				ViewAngles(pMe, yaw, pitch);
				float a = yaw;
				string d = dir;
				d.MakeLower();
				if (d == "back" || d == "backward") a += gf_PI;
				else if (d == "left") a += gf_PI * 0.5f;
				else if (d == "right") a -= gf_PI * 0.5f;
				else if (d == "north") a = 0;
				else if (d == "south") a = gf_PI;
				else if (d == "west") a = gf_PI * 0.5f;
				else if (d == "east") a = -gf_PI * 0.5f;
				s_bot.spot = pMe->GetEntity()->GetWorldPos() + Vec3(-sinf(a), cosf(a), 0) * m;
				s_bot.haveSpot = true;
				SetOrder(eO_Goto);
				Reply(id.c_str(), Ok(("going to " + Vec(s_bot.spot)).c_str()));
			}
			else
				Reply(id.c_str(), Fail("move: <forward|back|left|right|north|south|east|west> <meters>"));
		}
		else if (cmd == "attack")
		{
			if (!args.empty() && args != "nearest" && !ActorByName(args.c_str()))
				Reply(id.c_str(), Fail(("no soldier called " + args).c_str()));
			else
			{
				s_bot.target = args == "nearest" ? string() : args;
				SetOrder(eO_Attack);
				Reply(id.c_str(), Ok(("attacking " + (s_bot.target.empty() ? string("the nearest enemy") : s_bot.target)).c_str()));
			}
		}
		else if (cmd == "revive")
		{
			s_bot.target = args;
			SetOrder(eO_Revive);
			Reply(id.c_str(), Ok(("reviving " + (args.empty() ? string("the nearest downed teammate") : args)).c_str()));
		}
		else if (cmd == "fire")
		{
			s_bot.fireAtWill = !(args == "off" || args == "0" || args == "hold");
			Reply(id.c_str(), Ok(s_bot.fireAtWill ? "fires at enemies it sees" : "holds fire (unless ordered to attack)"));
		}
		else if (cmd == "say")
		{
			CGameRules* pRules = g_pGame->GetGameRules();
			if (pRules && pMe && !args.empty())
			{
				pRules->SendChatMessage(eChatToAll, pMe->GetEntityId(), 0, args.c_str());
				Reply(id.c_str(), Ok("said"));
			}
			else
				Reply(id.c_str(), Fail("not in a game, or nothing to say"));
		}
		else if (cmd == "grenade" && pMe)
		{
			const int n = Grenades(pMe);
			if (n <= 0)
				Reply(id.c_str(), Fail("it has no grenades"));
			else
			{
				s_gren.ordered = true;
				s_gren.orderTarget = (args.empty() || args == "best") ? string() : args;
				s_gren.orderAt = Now();
				string text;
				text.Format("throws a grenade %s%s as soon as it has a clear throw (%d left)", s_gren.orderTarget.empty() ? "at the best target" : "at ",
					s_gren.orderTarget.c_str(), n);
				Reply(id.c_str(), Ok(text.c_str()));
			}
		}
		else if (cmd == "use" && pMe)
		{
			Press(pMe, g_pGame->Actions().use, true);
			Press(pMe, g_pGame->Actions().use, false);
			Reply(id.c_str(), Ok("used what is in front"));
		}
		else if (cmd == "weapon" && pMe)
		{
			if (args.empty() || args == "next")
			{
				Press(pMe, g_pGame->Actions().nextitem, true);
				Press(pMe, g_pGame->Actions().nextitem, false);
			}
			else
				pMe->SelectItemByName(args.c_str(), true);
			string name;
			CurrentWeapon(pMe, 0, 0, &name);
			Reply(id.c_str(), Ok(("weapon: " + name).c_str()));
		}
		else if (cmd == "suit" && pMe && pMe->GetNanoSuit())
		{
			string m = args;
			m.MakeLower();
			ENanoMode mode = m == "speed" ? NANOMODE_SPEED : m == "strength" ? NANOMODE_STRENGTH : m == "cloak" ? NANOMODE_CLOAK : NANOMODE_DEFENSE;
			pMe->GetNanoSuit()->SetMode(mode);
			Reply(id.c_str(), Ok(("suit: " + m).c_str()));
		}
		else if (cmd == "screenshot")
		{
			if (!s_shotId.empty())
				Reply(id.c_str(), Fail("a screenshot is being taken"));
			else if (!gEnv->pRenderer || gEnv->pRenderer->GetRenderType() == eRT_Null)
				Reply(id.c_str(), Fail("the companion's game runs without graphics (to stay light and next to a fullscreen game): no picture, observe tells what it sees"));
			else
			{
				gEnv->pRenderer->EnableSwapBuffers(false);
				s_shotId = id;
				s_shotSince = Now();
				s_shotFrames = 0;
			}
		}
		else if (cmd == "ping")
			Reply(id.c_str(), Ok("pong"));
		else
			Reply(id.c_str(), Fail(("unknown command " + cmd).c_str()));
	}

	void CloseClient()
	{
		if (s_client != INVALID_SOCKET)
			closesocket(s_client);
		s_client = INVALID_SOCKET;
		s_in.clear();
		// a screenshot no one waits for any more: the frames are shown again
		if (!s_shotId.empty())
		{
			gEnv->pRenderer->EnableSwapBuffers(true);
			s_shotId.clear();
		}
		s_out.clear();
		if (s_agentConnected)
			CryLogAlways("[CoopAgent] the AI agent left");
		s_agentConnected = false;
	}

	// %LOCALAPPDATA%\CrysisCoop\companion\agent.port: "<port> <process>" of the
	// companion's game, for CrysisCoop.exe -coop_mcp to find it (the system may
	// keep the usual port from this program: a block of ports is reserved)
	// %LOCALAPPDATA%\CrysisCoop\companion\host_ready: the host's process id,
	// there while the host's game is ready for the companion to join (not
	// while it loads a level or a checkpoint). The companion's game starts
	// with the host's level and waits for it, so its own start (half a
	// minute) is done by then.
	// seconds since this game's process started (for the companion's log)
	float Uptime()
	{
		FILETIME created, exited, kernel, user, now;
		if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
			return 0.0f;
		GetSystemTimeAsFileTime(&now);
		const ULONGLONG a = ((ULONGLONG)created.dwHighDateTime << 32) | created.dwLowDateTime;
		const ULONGLONG b = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
		return (float)((b - a) / 10000000.0);
	}

	std::wstring ReadyFile()
	{
		wchar_t local[MAX_PATH] = L"";
		GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
		return std::wstring(local) + L"\\CrysisCoop\\companion\\host_ready";
	}

	std::wstring PortFile()
	{
		wchar_t local[MAX_PATH] = L"";
		GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
		return std::wstring(local) + L"\\CrysisCoop\\companion\\agent.port";
	}

	void UpdateBridge()
	{
		const int port = s_pPort->GetIVal();
		if (port <= 0)
			return;
		if (!s_wsa)
		{
			WSADATA data;
			s_wsa = WSAStartup(MAKEWORD(2, 2), &data) == 0;
			if (!s_wsa)
				return;
		}
		if (s_listen == INVALID_SOCKET)
		{
			if (Now() < s_listenRetry)
				return;
			s_listenRetry = Now() + 5.0f;
			// the usual port, or any free one when the system keeps it
			SOCKET s = INVALID_SOCKET;
			int actual = 0, error = 0;
			for (int attempt = 0; attempt < 2 && s == INVALID_SOCKET; ++attempt)
			{
				s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
				sockaddr_in addr = {};
				addr.sin_family = AF_INET;
				addr.sin_port = htons((u_short)(attempt ? 0 : port));
				addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
				BOOL exclusive = TRUE;
				if (s != INVALID_SOCKET)
					setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&exclusive, sizeof(exclusive));
				u_long nonBlocking = 1;
				int len = sizeof(addr);
				if (s == INVALID_SOCKET || bind(s, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(s, 2) != 0 || ioctlsocket(s, FIONBIO, &nonBlocking) != 0
					|| getsockname(s, (sockaddr*)&addr, &len) != 0)
				{
					error = WSAGetLastError();
					if (s != INVALID_SOCKET)
						closesocket(s);
					s = INVALID_SOCKET;
					continue;
				}
				actual = ntohs(addr.sin_port);
			}
			if (s == INVALID_SOCKET)
			{
				CryLogAlways("[CoopAgent] cannot listen on 127.0.0.1 (error %d): the AI agent cannot connect", error);
				return;
			}
			s_listen = s;
			if (FILE* f = _wfopen(PortFile().c_str(), L"wb"))
			{
				fprintf(f, "%d %u", actual, (unsigned)GetCurrentProcessId());
				fclose(f);
			}
			CryLogAlways("[CoopAgent] AI agents connect to 127.0.0.1:%d (CrysisCoop.exe -coop_mcp)%s", actual, actual != port ? " (the usual port is taken: found through agent.port)" : "");
		}
		SOCKET c = accept(s_listen, 0, 0);
		if (c != INVALID_SOCKET)
		{
			CloseClient();
			u_long nonBlocking = 1;
			ioctlsocket(c, FIONBIO, &nonBlocking);
			s_client = c;
			s_agentConnected = true;
			CryLogAlways("[CoopAgent] an AI agent connected");
		}
		if (s_client == INVALID_SOCKET)
			return;
		char buf[4096];
		for (;;)
		{
			const int got = recv(s_client, buf, sizeof(buf), 0);
			if (got > 0)
			{
				s_in.append(buf, got);
				if (s_in.size() > 65536)
				{
					CloseClient();
					return;
				}
				continue;
			}
			if (got == 0 || WSAGetLastError() != WSAEWOULDBLOCK)
			{
				CloseClient();
				return;
			}
			break;
		}
		for (size_t nl = s_in.find('\n'); nl != string::npos; nl = s_in.find('\n'))
		{
			string line = s_in.substr(0, nl);
			s_in.erase(0, nl + 1);
			line.TrimRight("\r");
			if (!line.empty())
				Handle(line);
		}
		UpdateShot();
		while (!s_out.empty())
		{
			const int sent = send(s_client, s_out.c_str(), (int)(std::min)(s_out.size(), (size_t)65536), 0);
			if (sent > 0)
			{
				s_out.erase(0, sent);
				continue;
			}
			if (sent < 0 && WSAGetLastError() != WSAEWOULDBLOCK)
				CloseClient();
			break;
		}
	}

	// ------------------------------------------------------------------
	// the companion's game: light, silent, joined to the host, gone with it
	HANDLE s_parent = 0;

	// The game's client does not reach a server on the same PC at the
	// server's own port (both are the game's UDP port): nothing came back.
	// A small relay on this PC in between, as the friends' tunnel does: the
	// game talks to a port of its own here, forwarded to the host's.
	struct SLocalProxy
	{
		SOCKET game, host;
		sockaddr_in gameAddr, hostAddr;
		volatile bool haveGame, stop;
		HANDLE thread;
		int port;
		volatile LONG toHost, toGame, loggedHost, loggedGame;
		SLocalProxy(): game(INVALID_SOCKET), host(INVALID_SOCKET), haveGame(false), stop(false), thread(0), port(0), toHost(0), toGame(0), loggedHost(0), loggedGame(0) {}
	} s_proxy;

	DWORD WINAPI ProxyThread(void*)
	{
		static char buf[65536];
		while (!s_proxy.stop)
		{
			fd_set r;
			FD_ZERO(&r);
			FD_SET(s_proxy.game, &r);
			FD_SET(s_proxy.host, &r);
			timeval tv = { 0, 200000 };
			if (select(0, &r, 0, 0, &tv) <= 0)
				continue;
			if (FD_ISSET(s_proxy.game, &r))
			{
				sockaddr_in from;
				int len = sizeof(from);
				const int n = recvfrom(s_proxy.game, buf, sizeof(buf), 0, (sockaddr*)&from, &len);
				if (n > 0)
				{
					s_proxy.gameAddr = from;
					s_proxy.haveGame = true;
					sendto(s_proxy.host, buf, n, 0, (const sockaddr*)&s_proxy.hostAddr, sizeof(s_proxy.hostAddr));
					InterlockedIncrement(&s_proxy.toHost);
				}
			}
			if (FD_ISSET(s_proxy.host, &r))
			{
				const int n = recvfrom(s_proxy.host, buf, sizeof(buf), 0, 0, 0);
				if (n > 0 && s_proxy.haveGame)
				{
					sendto(s_proxy.game, buf, n, 0, (const sockaddr*)&s_proxy.gameAddr, sizeof(s_proxy.gameAddr));
					InterlockedIncrement(&s_proxy.toGame);
				}
			}
		}
		return 0;
	}

	SOCKET LocalUdp()
	{
		SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (s == INVALID_SOCKET)
			return s;
		sockaddr_in addr = {};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (bind(s, (sockaddr*)&addr, sizeof(addr)) != 0)
		{
			closesocket(s);
			return INVALID_SOCKET;
		}
		// a closed port's ICMP answer must not end the socket's reads
		BOOL off = FALSE;
		DWORD got = 0;
		WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12), &off, sizeof(off), 0, 0, &got, 0, 0);    // SIO_UDP_CONNRESET
		return s;
	}

	// the port the game connects to (0: none)
	int ProxyPort(int hostPort)
	{
		if (s_proxy.thread)
			return s_proxy.port;
		if (!s_wsa)
		{
			WSADATA data;
			s_wsa = WSAStartup(MAKEWORD(2, 2), &data) == 0;
		}
		s_proxy.game = LocalUdp();
		s_proxy.host = LocalUdp();
		if (s_proxy.game == INVALID_SOCKET || s_proxy.host == INVALID_SOCKET)
			return 0;
		sockaddr_in me;
		int len = sizeof(me);
		getsockname(s_proxy.game, (sockaddr*)&me, &len);
		s_proxy.port = ntohs(me.sin_port);
		memset(&s_proxy.hostAddr, 0, sizeof(s_proxy.hostAddr));
		s_proxy.hostAddr.sin_family = AF_INET;
		s_proxy.hostAddr.sin_port = htons((u_short)hostPort);
		s_proxy.hostAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		s_proxy.thread = CreateThread(0, 0, ProxyThread, 0, 0, 0);
		CryLogAlways("[CoopAgent] 127.0.0.1:%d leads to the host's game at 127.0.0.1:%d", s_proxy.port, hostPort);
		return s_proxy.thread ? s_proxy.port : 0;
	}
	float s_nextConnect = -1.0f, s_nextParentCheck = 0.0f;

	DWORD WINAPI WatchHost(void*)
	{
		WaitForSingleObject(s_parent, INFINITE);
		TerminateProcess(GetCurrentProcess(), 0);
		return 0;
	}

	// %LOCALAPPDATA%\CrysisCoop\companion\companion.pid: the host ends a
	// companion left over from before (see StopLeftover)
	std::wstring PidFile()
	{
		wchar_t local[MAX_PATH] = L"";
		GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
		return std::wstring(local) + L"\\CrysisCoop\\companion\\companion.pid";
	}

	void WritePidFile()
	{
		if (FILE* f = _wfopen(PidFile().c_str(), L"wb"))
		{
			fprintf(f, "%u", (unsigned)GetCurrentProcessId());
			fclose(f);
		}
	}
	bool s_windowPlaced = false;

	// Out of sight: to the left of all screens. Hidden or minimized, the game
	// all but stopped its frames (it sees no window to draw); there it draws
	// as usual. No taskbar button, never the active window.
	void PutAway(HWND hWnd)
	{
		const LONG style = (GetWindowLongA(hWnd, GWL_EXSTYLE) | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE) & ~WS_EX_APPWINDOW;
		const bool visible = IsWindowVisible(hWnd) != 0;
		if (style != GetWindowLongA(hWnd, GWL_EXSTYLE))
		{
			if (visible)
				ShowWindow(hWnd, SW_HIDE);
			SetWindowLongA(hWnd, GWL_EXSTYLE, style);
		}
		RECT r;
		GetWindowRect(hWnd, &r);
		const int x = GetSystemMetrics(SM_XVIRTUALSCREEN) - (r.right - r.left) - 100;
		SetWindowPos(hWnd, HWND_BOTTOM, x, GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
	}
	LARGE_INTEGER s_frameStart = {};

	BOOL CALLBACK FindGameWindow(HWND hWnd, LPARAM lParam)
	{
		// the game's window: the thread's top-level window with a size
		RECT r;
		if (!GetParent(hWnd) && GetWindowRect(hWnd, &r) && r.right - r.left > 100)
		{
			*(HWND*)lParam = hWnd;
			return FALSE;
		}
		return TRUE;
	}

	// ---- the breaks it makes again while it joins. The host's game keeps
	// every break (those its load made again too) and a game that joins makes
	// each again, one after another; one it cannot make the same is given up
	// after g_breaktimeoutframes frames (the host's 140, synced to this game).
	// The companion's game makes about one frame a second while it joins:
	// the glass of a house a palm tree fell on right after a load (the user's
	// save of 2026-10-14 with coop_load_ids 1) held it more than two minutes
	// and the host told the player it cannot join. A break it can make is
	// made within two frames (35 of 36 in the first, measured): while its
	// level loads one waits JOIN_BREAK_FRAMES frames. (A joining game may
	// change only a cvar the host does not sync; that flag at the connect,
	// while the host's cvars come, cut the connection: "Disconnect ...
	// cause=25". So only once the level loads, both back after)
	// (That one frame a second was CrySystem's background pause, left out
	// while a client joins now: see below)
	const int JOIN_BREAK_FRAMES = 3;
	ICVar* s_pBreakTimeout = 0;
	int s_hostBreakTimeout = 140;      // the host's value
	int s_breakTimeoutFlags = 0;       // the cvar's own flags
	bool s_joinBreakTimeout = false;   // JOIN_BREAK_FRAMES while its level loads

	void UpdateBreakTimeout(bool loading)
	{
		if (!s_pBreakTimeout)
		{
			s_pBreakTimeout = gEnv->pConsole->GetCVar("g_breaktimeoutframes");
			if (!s_pBreakTimeout)
				return;
		}
		if (!loading)
		{
			if (s_joinBreakTimeout)
			{
				s_joinBreakTimeout = false;
				s_pBreakTimeout->Set(s_hostBreakTimeout);
				s_pBreakTimeout->SetFlags(s_breakTimeoutFlags);
				CryLogAlways("[CoopAgent] its level loaded: a break it makes again waits %d frames again (%d now)", s_hostBreakTimeout, s_pBreakTimeout->GetIVal());
			}
			return;
		}
		if (!s_joinBreakTimeout)
		{
			s_joinBreakTimeout = true;
			s_hostBreakTimeout = s_pBreakTimeout->GetIVal();
			s_breakTimeoutFlags = s_pBreakTimeout->GetFlags();
			s_pBreakTimeout->SetFlags(s_breakTimeoutFlags | VF_NOT_NET_SYNCED);
			s_pBreakTimeout->Set(std::min(JOIN_BREAK_FRAMES, s_hostBreakTimeout));
			CryLogAlways("[CoopAgent] its level loads: a break it makes again waits %d frames (now %d; the host's game %d)",
				JOIN_BREAK_FRAMES, s_pBreakTimeout->GetIVal(), s_hostBreakTimeout);
		}
		// (the host's value again: ours)
		else if (s_pBreakTimeout->GetIVal() > JOIN_BREAK_FRAMES)
		{
			s_hostBreakTimeout = s_pBreakTimeout->GetIVal();
			s_pBreakTimeout->Set(JOIN_BREAK_FRAMES);
		}
	}

	// ---- the background pause. CSystem::Update (CrySystem 6156) pauses a game
	// whose window is not the active one while gEnv->bMultiplayer is false
	// (single player's pause: up to 10 x Sleep(100) a frame, so one frame a
	// second). A coop client loads its level with the flag false (CoopAI,
	// coop_sp_world), until it is in: the AI companion (its window is never
	// the active one) and a friend at another window made one frame a second
	// while they joined, and a break of the host's they could not make again
	// held them 140 frames = 140 s each; a friend was not in after 300 s
	// (2026-10-15). A host plays with the flag false only with coop_sp_world 1
	// (the default 2 sets it again once the level has loaded): its game made
	// one frame a second as soon as its window was not the active one, and the
	// friends' world stood still. A network game does not pause: while a
	// client loads, or a server runs its level as single player, the call
	// goes through NoPauseInCoop and is left out
	const unsigned PAUSE_CALL_RVA = 0x4C56D;    // CrySystem 6156, CSystem::Update: "mov ecx,esi; call" (E8) the pause, thiscall, no arguments
	const unsigned PAUSE_RVA = 0x4B890;
	const unsigned char PAUSE_START[13] = { 0x83, 0xEC, 0x24, 0x56, 0x8B, 0xF1, 0x80, 0xBE, 0x3F, 0x05, 0x00, 0x00, 0x00 };  // sub esp,24h; push esi; mov esi,ecx; cmp byte ptr [esi+53Fh],0
	DWORD s_pause = 0;              // CrySystem's own function
	bool s_pauseLeftOut = false;    // logged since the window stopped being the active one
	int s_framesInactive = 0;       // testing (coop_test_framelog): frames whose window was not the active one
	int s_framesPaused = 0;         // ... and that went into CrySystem's pause

	bool __cdecl SkipPause()
	{
		const bool inactive = gEnv->pRenderer && GetActiveWindow() != (HWND)gEnv->pRenderer->GetHWND();
		if (inactive)
			++s_framesInactive;
		const bool joins = CoopAI::ClientLoadsAsSinglePlayer();
		if (!joins && !CoopAI::ServerRunsAsSinglePlayer())
		{
			s_pauseLeftOut = false;
			if (inactive && !gEnv->bMultiplayer)
				++s_framesPaused;
			return false;
		}
		if (!inactive)
			s_pauseLeftOut = false;
		else if (!s_pauseLeftOut)
		{
			s_pauseLeftOut = true;
			CryLogAlways(joins ? "[CoopAgent] this game joins and its window is not the active one: no background pause"
				: "[CoopAgent] this game hosts a coop game and its window is not the active one: no background pause");
		}
		return true;
	}

	__declspec(naked) void NoPauseInCoop()
	{
		__asm
		{
			push ecx
			call SkipPause
			pop ecx
			test al, al
			jnz skip
			jmp dword ptr [s_pause]
		skip:
			ret
		}
	}

	void PatchPause()
	{
		if (s_pause)
			return;
		HMODULE h = GetModuleHandleA("CrySystem.dll");
		unsigned char* p = h ? (unsigned char*)h + PAUSE_CALL_RVA : 0;
		const DWORD own = (DWORD)h + PAUSE_RVA;
		if (!p || p[-2] != 0x8B || p[-1] != 0xCE || p[0] != 0xE8 || (DWORD)(p + 5) + *(int*)(p + 1) != own
			|| memcmp((const void*)own, PAUSE_START, sizeof(PAUSE_START)))
		{
			CryLogAlways("[CoopAgent] the background pause is not left out in coop games (another CrySystem.dll)");
			return;
		}
		DWORD old = 0;
		if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old))
			return;
		s_pause = own;
		*(int*)(p + 1) = (int)((DWORD)&NoPauseInCoop - (DWORD)(p + 5));
		VirtualProtect(p, 5, old, &old);
		FlushInstructionCache(GetCurrentProcess(), p, 5);
		CryLogAlways("[CoopAgent] the background pause is left out while this game joins or hosts a coop game");
	}

	// testing (coop_test_framelog 1): this game's frames a second, also while
	// it loads or joins (the freeze log leaves those out). Counted at the
	// frame's end on the real clock: a joining game's timer is set to the
	// server's
	void LogFrames()
	{
		static CTimeValue s_since;
		static int s_frames = 0;
		if (!s_pFrameLog || !s_pFrameLog->GetIVal())
		{
			s_since.SetValue(0);
			return;
		}
		const CTimeValue now = gEnv->pTimer->GetAsyncTime();
		if (!s_since.GetValue())
		{
			s_since = now;
			s_frames = 0;
			s_framesInactive = s_framesPaused = 0;
			return;
		}
		++s_frames;
		const float sec = (now - s_since).GetSeconds();
		if (sec >= 5.0f)
		{
			CryLogAlways("[CoopFrames] %.1f frames a second (%d in %.1f s)%s; window not the active one in %d, background pause in %d",
				s_frames / sec, s_frames, sec, CoopAI::LevelLoading() ? ", its level loads" : "", s_framesInactive, s_framesPaused);
			s_since = now;
			s_frames = 0;
			s_framesInactive = s_framesPaused = 0;
		}
	}

	void UpdateCompanionGame()
	{
		// the window: named, and out of sight (the host does it too, at once)
		if (!s_windowPlaced)
		{
			HWND hWnd = 0;
			EnumThreadWindows(GetCurrentThreadId(), FindGameWindow, (LPARAM)&hWnd);
			if (hWnd)
			{
				s_windowPlaced = true;
				SetWindowTextA(hWnd, "Crysis Coop - AI companion");
				PutAway(hWnd);
			}
		}
		// the first connect once the network service and the host are ready
		// (both are checked below)
		if (s_nextConnect < 0)
		{
			s_nextConnect = Now();
			CryLogAlways("[CoopAgent] the game is up, %.0f s after its start", Uptime());
		}
		// the host's game is gone: so is this one, at once. A thread waits for
		// it: the game's own loop hung two minutes over the lost connection
		// first, and a second companion found the first still there
		if (s_pParent->GetIVal() && !s_parent)
		{
			s_parent = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)s_pParent->GetIVal());
			if (!s_parent)
			{
				CryLogAlways("[CoopAgent] the host's game is gone: quitting");
				TerminateProcess(GetCurrentProcess(), 0);
			}
			CloseHandle(CreateThread(0, 0, WatchHost, 0, 0, 0));
			WritePidFile();
		}
		// what went through the relay here (the log, now and then)
		static float s_proxyLogAt = 0.0f;
		static int s_proxyLogs = 0;
		if (s_proxy.thread && Now() >= s_proxyLogAt && (s_proxy.toHost != s_proxy.loggedHost || s_proxy.toGame != s_proxy.loggedGame))
		{
			s_proxyLogAt = Now() + (++s_proxyLogs < 10 ? 1.0f : 60.0f);
			s_proxy.loggedHost = s_proxy.toHost;
			s_proxy.loggedGame = s_proxy.toGame;
			CryLogAlways("[CoopAgent] relay here: %d packets to the host, %d back", (int)s_proxy.toHost, (int)s_proxy.toGame);
		}
		// into the host's game (again, after the host loaded a checkpoint)
		const int port = s_pJoin->GetIVal();
		IGameFramework* pFramework = g_pGame->GetIGameFramework();
		// the game's network service (the key check on connecting needs it):
		// the multiplayer menu starts it in a player's game, nothing here does
		static bool s_serviceLogged = false;
		INetworkServicePtr pService = port > 0 ? gEnv->pNetwork->GetService("GameSpy") : INetworkServicePtr();
		const bool serviceReady = !pService || pService->GetState() != eNSS_Initializing;
		if (pService && serviceReady && !s_serviceLogged)
		{
			s_serviceLogged = true;
			CryLogAlways("[CoopAgent] the network service is ready (state %d), %.0f s after the start", (int)pService->GetState(), Uptime());
		}
		// thrown out (the host loaded a checkpoint, its server restarted): in
		// again soon, not after the handshake's long wait
		static bool s_wasIn = false;
		const bool in = pFramework->GetClientChannel() != 0;
		if (s_wasIn && !in)
		{
			CryLogAlways("[CoopAgent] out of the host's game: joining again in 5 s");
			s_nextConnect = Now() + 5.0f;
		}
		s_wasIn = in;
		// joining: from the connect until the level load after it is over
		// (its player is there long before; no load within a minute: in)
		static int s_joinPhase = 0;        // 0 out or in the game, 1 connected, 2 its level loads
		static float s_connectedAt = 0.0f;
		static bool s_wasJoined = false;
		if (!in)
			s_joinPhase = 0;
		else if (!s_wasJoined && s_joinPhase == 0)
		{
			s_joinPhase = 1;
			s_connectedAt = Now();
		}
		if (s_joinPhase == 1 && CoopAI::LevelLoading())
			s_joinPhase = 2;
		if ((s_joinPhase == 2 && !CoopAI::LevelLoading()) || (s_joinPhase == 1 && Now() - s_connectedAt > 60.0f))
			s_joinPhase = 0;
		s_wasJoined = in && s_joinPhase == 0;
		UpdateBreakTimeout(s_joinPhase == 2);
		bool hostReady = true;
		if (const int parent = s_pParent->GetIVal())
		{
			hostReady = false;
			if (FILE* f = _wfopen(ReadyFile().c_str(), L"rb"))
			{
				int pid = 0;
				hostReady = fscanf(f, "%d", &pid) == 1 && pid == parent;
				fclose(f);
			}
			static bool s_waitLogged = false;
			if (!hostReady && !s_waitLogged && serviceReady)
			{
				s_waitLogged = true;
				CryLogAlways("[CoopAgent] ready, %.0f s after the start; waiting for the host's game (its level, a checkpoint)", Uptime());
			}
		}
		if (port > 0 && serviceReady && hostReady && !pFramework->GetClientChannel() && !pFramework->GetClientActor() && Now() >= s_nextConnect)
		{
			// the handshake (the key check) takes a while: a new connect would cut it
			s_nextConnect = Now() + 45.0f;
			const int local = ProxyPort(port);
			string cmd;
			cmd.Format("connect 127.0.0.1 %d", local ? local : port);
			CryLogAlways("[CoopAgent] joining the host: %s (%.0f s after the start)", cmd.c_str(), Uptime());
			gEnv->pConsole->ExecuteString(cmd.c_str());
		}
	}

	// ------------------------------------------------------------------
	// the host: the companion's game, started and ended with the setting
	PROCESS_INFORMATION s_proc = {};
	float s_notWantedSince = -1.0f, s_restartAt = 0.0f, s_startedAt = 0.0f, s_lockUntil = 0.0f;
	int s_starts = 0;
	HANDLE s_hookStop = 0;              // ends the window hook's thread
	volatile LONG s_hookMs = -1;        // when the hook was set, ms after the start (-2: never)
	bool s_hookLogged = false;

	// The engine makes its window before this DLL is loaded into that game.
	// A hook on its main thread has Windows load the DLL at the first window,
	// which is then born a tool window (no taskbar button, never active) and
	// out of sight. The thread takes a hook only once it has made its first
	// call into the window system, so a thread of its own tries until it
	// works: the host's game loop may stand still for seconds right then
	// (its level finishing to load) while the companion makes its window.
	// A hook lives as long as the thread that set it: it waits.
	struct SHookJob { DWORD thread; HANDLE process, stop; };
	DWORD WINAPI HookThread(void* p)
	{
		SHookJob* job = (SHookJob*)p;
		const DWORD start = GetTickCount();
		HHOOK hook = 0;
		while (!hook && GetTickCount() - start < 20000)
		{
			hook = CoopHookWindowsOf(job->thread);
			if (!hook && WaitForSingleObject(job->process, 1) == WAIT_OBJECT_0)
				break;
		}
		s_hookMs = hook ? (LONG)(GetTickCount() - start) : -2;
		if (hook)
		{
			HANDLE wait[2] = { job->process, job->stop };
			WaitForMultipleObjects(2, wait, FALSE, 90000);
			UnhookWindowsHookEx(hook);
		}
		CloseHandle(job->process);
		CloseHandle(job->stop);
		delete job;
		return 0;
	}

	void UnhookWindows()
	{
		if (s_hookStop)
		{
			SetEvent(s_hookStop);
			CloseHandle(s_hookStop);
			s_hookStop = 0;
		}
	}

	bool Running()
	{
		if (!s_proc.hProcess)
			return false;
		if (WaitForSingleObject(s_proc.hProcess, 0) == WAIT_OBJECT_0)
		{
			UnhookWindows();
			CloseHandle(s_proc.hProcess);
			CloseHandle(s_proc.hThread);
			memset(&s_proc, 0, sizeof(s_proc));
			CryLogAlways("[CoopAgent] the AI companion's game ended");
			s_restartAt = Now() + 10.0f;
			return false;
		}
		return true;
	}

	// CPUs the host does not use (up to 4), for the companion
	DWORD_PTR CompanionCpus()
	{
		DWORD_PTR mine = 0, all = 0;
		if (!GetProcessAffinityMask(GetCurrentProcess(), &mine, &all) || !all)
			return 0;
		DWORD_PTR others = all & ~mine;
		if (!others)
			others = all;
		DWORD_PTR mask = 0;
		int n = 0;
		for (int bit = sizeof(DWORD_PTR) * 8 - 1; bit >= 0 && n < 4; --bit)
			if (others & ((DWORD_PTR)1 << bit))
			{
				mask |= (DWORD_PTR)1 << bit;
				++n;
			}
		return mask;
	}

	// The companion's profile keeps its own game.cfg, which the game reads
	// before the command line: a new one (or one written after a fullscreen
	// start) had the game switch the whole screen to 1024x768 and back while
	// it started, and the host's taskbar jumped. It starts in a small window
	// from the first frame.
	void PrepareCompanionConfig(const std::wstring& profile)
	{
		const std::wstring path = profile + L"\\game.cfg";
		std::string kept;
		if (FILE* f = _wfopen(path.c_str(), L"rb"))
		{
			char line[1024];
			while (fgets(line, sizeof(line), f))
			{
				const char* p = line;
				while (*p == ' ' || *p == '\t')
					++p;
				if (!_strnicmp(p, "r_Fullscreen", 12) || !_strnicmp(p, "r_Width", 7) || !_strnicmp(p, "r_Height", 8)
					|| !_strnicmp(p, "s_SoundEnable", 13) || !_strnicmp(p, "s_DummySound", 12) || !_strnicmp(p, "r_Driver", 8))
					continue;
				kept += line;
			}
			fclose(f);
		}
		if (!kept.empty() && kept[kept.size() - 1] != '\n')
			kept += "\n";
		// and no sound system at all: it opened the microphone as it started
		// (Windows' "microphone in use" icon came and went in the taskbar);
		// on the command line it is set too late for that
		// no renderer at all (the dedicated server's): the companion's game
		// crashed making its Direct3D device next to the host's fullscreen
		// game, and a bot needs no picture. Read before the renderer starts
		// (the command line comes after)
		kept += "r_Driver = \"NULL\"\nr_Fullscreen = 0\nr_Width = 800\nr_Height = 450\ns_SoundEnable = 0\ns_DummySound = 1\n";
		if (FILE* f = _wfopen(path.c_str(), L"wb"))
		{
			fwrite(kept.data(), 1, kept.size(), f);
			fclose(f);
		}
	}

	// a companion game left over from before (its host crashed, say) holds
	// the agents' port: it goes first
	void StopLeftover()
	{
		unsigned pid = 0;
		if (FILE* f = _wfopen(PidFile().c_str(), L"rb"))
		{
			if (fscanf(f, "%u", &pid) != 1)
				pid = 0;
			fclose(f);
		}
		if (!pid || pid == GetCurrentProcessId())
			return;
		HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
		if (!h)
			return;
		wchar_t image[MAX_PATH] = L"";
		DWORD len = MAX_PATH;
		QueryFullProcessImageNameW(h, 0, image, &len);
		const size_t n = wcslen(image);
		if (n >= 10 && !_wcsicmp(image + n - 10, L"Crysis.exe"))
		{
			TerminateProcess(h, 0);
			WaitForSingleObject(h, 3000);
			CryLogAlways("[CoopAgent] a companion game left over from before (process %u) ended", pid);
		}
		CloseHandle(h);
	}

	// The companion's memory comes last (low memory priority), set before it
	// runs. (Its disk reads stay normal: at a very low I/O priority its level
	// took 65 s to load instead of 29.)
	void LowerCompanionPriority(HANDLE process)
	{
		typedef BOOL (WINAPI *TSetProcessInformation)(HANDLE, int, LPVOID, DWORD);
		static TSetProcessInformation pInfo = (TSetProcessInformation)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetProcessInformation");
		if (pInfo)
		{
			ULONG memoryPriority = 2;    // ProcessMemoryPriority (0): MEMORY_PRIORITY_LOW
			pInfo(process, 0, &memoryPriority, sizeof(memoryPriority));
		}
	}

	void StartCompanion()
	{
		const DWORD t0 = GetTickCount();
		StopLeftover();
		wchar_t exe[MAX_PATH], dll[MAX_PATH];
		GetModuleFileNameW(NULL, exe, MAX_PATH);
		GetModuleFileNameW((HMODULE)g_hInst, dll, MAX_PATH);
		// Mods\<mod>\Bin32\Coop.dll: the mod's folder name
		std::wstring modPath = dll;
		std::wstring mod = L"Coop";
		size_t bin = modPath.rfind(L'\\');
		if (bin != std::wstring::npos)
		{
			modPath.resize(bin);
			size_t folder = modPath.rfind(L'\\');
			if (folder != std::wstring::npos)
			{
				std::wstring up = modPath.substr(0, folder);
				size_t name = up.rfind(L'\\');
				mod = up.substr(name + 1);
			}
		}
		// the game's folder: Bin32\Crysis.exe
		std::wstring root = exe;
		for (int i = 0; i < 2; ++i)
		{
			size_t slash = root.rfind(L'\\');
			if (slash != std::wstring::npos)
				root.resize(slash);
		}
		wchar_t local[MAX_PATH] = L"";
		GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
		std::wstring profile = std::wstring(local[0] ? local : root.c_str()) + L"\\CrysisCoop\\companion";
		CreateDirectoryW((std::wstring(local[0] ? local : root.c_str()) + L"\\CrysisCoop").c_str(), 0);
		CreateDirectoryW(profile.c_str(), 0);
		PrepareCompanionConfig(profile);
		ICVar* pPort = gEnv->pConsole->GetCVar("sv_port");
		char tail[512];
		_snprintf(tail, sizeof(tail), " -coop_skipintro +coop_agent 1 +coop_agent_join %d +coop_agent_parent %u +coop_agent_port %d +coop_agent_name %s%s",
			pPort ? pPort->GetIVal() : 64087, (unsigned)GetCurrentProcessId(), s_pPort->GetIVal(), s_pName->GetString(), COMPANION_SETTINGS);
		tail[sizeof(tail) - 1] = 0;
		wchar_t wtail[512];
		MultiByteToWideChar(CP_UTF8, 0, tail, -1, wtail, 512);
		// the host's own mode (a server refuses a client whose -devmode differs)
		const bool devmode = wcsstr(GetCommandLineW(), L"-devmode") != 0;
		std::wstring line = L"\"" + std::wstring(exe) + L"\" -mod " + mod + (devmode ? L" -devmode" : L"") + L" -userpath \"" + profile + L"\" -logfile companion.log" + wtail;
		// testing: the host runs the lines of a file as console commands, the
		// companion those of its own
		ICVar* pCmdFile = gEnv->pConsole->GetCVar("coop_test_cmdfile");
		if (pCmdFile && pCmdFile->GetString()[0])
			line += L" +coop_test_cmdfile companion_cmd.txt";
		std::vector<wchar_t> buf(line.begin(), line.end());
		buf.push_back(0);
		STARTUPINFOW si = {};
		si.cb = sizeof(si);
		si.dwFlags = STARTF_USESHOWWINDOW;
		// hidden: the engine makes and shows its window before Coop.dll is
		// there to make it a tool window, and its taskbar button came and went
		// (the taskbar's icons jumped). Coop.dll shows it, out of sight, once
		// it is one (CoopKeepTestWindowBehind; UpdateHost too)
		si.wShowWindow = SW_HIDE;
		// Crysis.exe asks for no elevation when started this way
		SetEnvironmentVariableW(L"__COMPAT_LAYER", L"RunAsInvoker");
		// while its window appears, no program may take the foreground from
		// the host's game (a new window takes it when the player has not
		// touched a key for a while; the host's fullscreen game would go)
		if (LockSetForegroundWindow(LSFW_LOCK))
			s_lockUntil = Now() + 10.0f;
		if (!CreateProcessW(exe, &buf[0], 0, 0, FALSE, CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS, 0, root.c_str(), &si, &s_proc))
		{
			CryLogAlways("[CoopAgent] the AI companion's game could not be started (error %u)", (unsigned)GetLastError());
			memset(&s_proc, 0, sizeof(s_proc));
			s_restartAt = Now() + 30.0f;
			return;
		}
		if (const DWORD_PTR cpus = CompanionCpus())
			SetProcessAffinityMask(s_proc.hProcess, cpus);
		LowerCompanionPriority(s_proc.hProcess);
		UnhookWindows();
		SHookJob* job = new SHookJob();
		job->thread = s_proc.dwThreadId;
		job->stop = CreateEventW(0, TRUE, FALSE, 0);
		s_hookStop = 0;
		if (job->stop && DuplicateHandle(GetCurrentProcess(), s_proc.hProcess, GetCurrentProcess(), &job->process, SYNCHRONIZE, FALSE, 0)
			&& DuplicateHandle(GetCurrentProcess(), job->stop, GetCurrentProcess(), &s_hookStop, 0, FALSE, DUPLICATE_SAME_ACCESS))
		{
			s_hookMs = -1;
			s_hookLogged = false;
			ResumeThread(s_proc.hThread);
			CloseHandle(CreateThread(0, 0, HookThread, job, 0, 0));
		}
		else
		{
			if (job->stop)
				CloseHandle(job->stop);
			delete job;
			ResumeThread(s_proc.hThread);
		}
		++s_starts;
		s_startedAt = Now();
		CryLogAlways("[CoopAgent] AI companion started (process %u, joins 127.0.0.1:%d, agents on 127.0.0.1:%d) in %u ms",
			(unsigned)s_proc.dwProcessId, pPort ? pPort->GetIVal() : 64087, s_pPort->GetIVal(), (unsigned)(GetTickCount() - t0));
	}

	void StopCompanion(const char* why)
	{
		if (!Running())
			return;
		CryLogAlways("[CoopAgent] AI companion stopped (%s)", why);
		TerminateProcess(s_proc.hProcess, 0);
		WaitForSingleObject(s_proc.hProcess, 3000);
		UnhookWindows();
		CloseHandle(s_proc.hProcess);
		CloseHandle(s_proc.hThread);
		memset(&s_proc, 0, sizeof(s_proc));
	}

	// the host's game is in the level for good: no checkpoint being loaded
	// (Continue), no cutscene playing, its player there for a few seconds.
	// (A join during the fullscreen intro crashed the host's game in the
	// engine's file streaming, CCachedFileData::GetData; and the companion
	// would only stand there anyway.) And nothing broke for a while: the
	// host's game keeps every break for the games that join, each when its
	// record ends (120 frames, ~4 s), and a game that joins while that list
	// still grows gets its entities only after it made every break again,
	// one a frame at its one frame a second (the user's save of 2026-10-14
	// with coop_load_ids 1: a palm tree on a house, the house fell in after
	// the load for 30 s; the companion joined 150 s after the load, or never
	// when a break it could not make held it for 140 frames). While things
	// keep breaking (a fight) it waits at most 30 s
	const float BREAK_QUIET_SECONDS = 6.0f;
	const float BREAK_QUIET_WAIT_MAX = 30.0f;

	bool HostWorldReady()
	{
		static float s_since = -1.0f;
		static bool s_ready = false;
		CHUD* pHUD = g_pGame->GetHUD();
		const bool now = !CoopSave::IsBusy() && g_pGame->GetIGameFramework()->GetClientActor() != 0
			&& g_pGame->GetIGameFramework()->GetClientActor()->GetHealth() > 0
			&& !(pHUD && pHUD->IsCutscenePlaying());
		if (!now)
		{
			s_since = -1.0f;
			s_ready = false;
			return false;
		}
		if (s_ready)
			return true;
		if (s_since < 0.0f)
			s_since = Now();
		const float settled = Now() - s_since;
		if (settled <= 3.0f)
			return false;
		const float sinceBreak = CoopAI::SecondsSinceBreak();
		if (sinceBreak < BREAK_QUIET_SECONDS && settled < 3.0f + BREAK_QUIET_WAIT_MAX)
			return false;
		s_ready = true;
		if (settled > 3.5f)
			CryLogAlways("[CoopAgent] the host's game is ready %.0f s after its player is there (%s)", settled,
				sinceBreak >= BREAK_QUIET_SECONDS ? "things stopped breaking" : "things still break: it waited long enough");
		return true;
	}

	float s_readySince = -1.0f;    // when the host's game became ready (-1: not ready)
	float s_lastInGame = -1.0f;    // when the companion's player was last seen in the host's game

	void SetHostReady(bool ready)
	{
		static int s_written = -1;
		if ((int)ready == s_written)
			return;
		s_written = ready;
		s_readySince = ready ? Now() : -1.0f;
		if (ready)
		{
			if (FILE* f = _wfopen(ReadyFile().c_str(), L"wb"))
			{
				fprintf(f, "%u", (unsigned)GetCurrentProcessId());
				fclose(f);
			}
		}
		else
			DeleteFileW(ReadyFile().c_str());
	}

	// the host is told on the screen when the companion cannot join (not
	// only in the co-op menu), once per failure
	void TellFailure()
	{
		static float s_nextCheck = 0.0f;
		static bool s_told = false;
		if (Now() < s_nextCheck)
			return;
		s_nextCheck = Now() + 1.0f;
		const int state = CoopAgent::CompanionState();
		const bool failed = state == 3 || (state == 1 && CoopAgent::CompanionSeconds() >= CoopAgent::JOIN_TIMEOUT);
		if (!failed)
		{
			if (state == 2)
				s_told = false;
			return;
		}
		if (s_told)
			return;
		s_told = true;
		const char* text = state == 3
			? "AI companion cannot join: its game did not start. Esc > Co-op game: turn it off and on"
			: "AI companion cannot join your game. Esc > Co-op game: turn it off and on";
		CryLogAlways("[CoopAgent] told the host: %s", text);
		if (CHUD* pHUD = g_pGame->GetHUD())
			pHUD->DisplayTempFlashText(text, 10.0f, ColorF(1.0f, 0.5f, 0.35f));
	}

	void UpdatePendingEnters();

	void UpdateHost()
	{
		if (gEnv->bServer)
			UpdatePendingEnters();
		// The companion's window is made a tool window out of sight inside its
		// own game (the window hook, GameDll.cpp). Not from here: styling or
		// moving another process's window waits for that process's thread,
		// and the host's game stood still 2 s while the companion loaded.
		if (!s_hookLogged && s_hookMs != -1)
		{
			s_hookLogged = true;
			if (s_hookMs >= 0)
				CryLogAlways("[CoopAgent] window hook on the companion's game %d ms after its start", (int)s_hookMs);
			else
				CryLogAlways("[CoopAgent] no window hook on the companion's game: its window may show for a moment");
		}
		if (s_lockUntil > 0.0f && Now() > s_lockUntil)
		{
			LockSetForegroundWindow(LSFW_UNLOCK);
			s_lockUntil = 0.0f;
		}
		const bool hosting = gEnv->bServer && gEnv->bMultiplayer && CoopAI::IsCoopSession();
		const bool want = s_pCompanion->GetIVal() != 0 && hosting;
		SetHostReady(want && HostWorldReady());
		if (want)
		{
			s_notWantedSince = -1.0f;
			// not before the host's world is ready: a checkpoint loaded by
			// Continue restarts the host's server, and a companion that had
			// just joined was thrown out
			if (!Running() && Now() >= s_restartAt && s_starts < 5)
				StartCompanion();
			TellFailure();
			return;
		}
		if (!Running())
		{
			if (!s_pCompanion->GetIVal())
				s_starts = 0;
			return;
		}
		// turned off: at once; not hosting: after a while (a level change)
		if (!s_pCompanion->GetIVal())
			StopCompanion("turned off in the co-op menu");
		else if (s_notWantedSince < 0)
			s_notWantedSince = Now();
		else if (Now() - s_notWantedSince > 30.0f)
			StopCompanion("the host is not in a co-op game");
	}

	// ------------------------------------------------------------------
	// the server side of a companion's requests
	// the AI navigation's way from one point to another, as the soldiers
	// find theirs: A* over its graph (the outdoor triangles, the indoor
	// waypoints), then straightened where a straight walk is valid
	bool FindPath(const Vec3& from, const Vec3& to, std::vector<Vec3>& out)
	{
		out.clear();
		IAISystem* pAI = gEnv->pAISystem;
		IGraph* pGraph = pAI ? pAI->GetNodeGraph() : 0;
		if (!pGraph)
			return false;
		const IAISystem::tNavCapMask mask = IAISystem::NAV_TRIANGULAR | IAISystem::NAV_WAYPOINT_HUMAN | IAISystem::NAV_ROAD;
		const float radius = 0.4f;
		const unsigned a = pGraph->GetEnclosing(from, mask, radius, 0, 0.0f, 0, true, "coop companion");
		const unsigned b = pGraph->GetEnclosing(to, mask, radius, 0, 0.0f, 0, true, "coop companion");
		GraphNode* pStart = a ? pGraph->GetNode(a) : 0;
		GraphNode* pGoal = b ? pGraph->GetNode(b) : 0;
		if (!pStart || !pGoal)
			return false;
		struct SOpen
		{
			float f;
			GraphNode* pNode;
			bool operator<(const SOpen& o) const { return f > o.f; }
		};
		struct SSeen
		{
			float g;
			GraphNode* pFrom;
			bool closed;
		};
		std::priority_queue<SOpen> open;
		std::map<GraphNode*, SSeen> seen;
		const Vec3 goalPos = pGraph->GetNodePos(pGoal);
		const SSeen s0 = { 0.0f, 0, false };
		seen[pStart] = s0;
		const SOpen o0 = { pGraph->GetNodePos(pStart).GetDistance(goalPos), pStart };
		open.push(o0);
		bool found = false;
		for (int n = 0; !open.empty() && n < 40000; ++n)
		{
			const SOpen cur = open.top();
			open.pop();
			SSeen& sc = seen[cur.pNode];
			if (sc.closed)
				continue;
			sc.closed = true;
			if (cur.pNode == pGoal)
			{
				found = true;
				break;
			}
			const Vec3 p = pGraph->GetNodePos(cur.pNode);
			const unsigned links = pGraph->GetNumNodeLinks(cur.pNode);
			for (unsigned i = 0; i < links; ++i)
			{
				const unsigned link = pGraph->GetGraphLink(cur.pNode, i);
				if (pGraph->GetRadiusFromLink(link) < radius)
					continue;
				GraphNode* pNext = pGraph->GetNextNode(link);
				if (!pNext || !(pGraph->GetNavType(pNext) & mask))
					continue;
				const Vec3 q = pGraph->GetNodePos(pNext);
				const float g = sc.g + p.GetDistance(q);
				std::map<GraphNode*, SSeen>::iterator it = seen.find(pNext);
				if (it != seen.end() && (it->second.closed || it->second.g <= g))
					continue;
				const SSeen sn = { g, cur.pNode, false };
				seen[pNext] = sn;
				const SOpen on = { g + q.GetDistance(goalPos), pNext };
				open.push(on);
			}
		}
		if (!found)
			return false;
		std::vector<Vec3> nodes;
		for (GraphNode* pNode = pGoal; pNode; pNode = seen[pNode].pFrom)
			nodes.push_back(pGraph->GetNodePos(pNode));
		std::reverse(nodes.begin(), nodes.end());
		nodes.push_back(to);
		// straightened: from each point to the farthest one in a straight walk
		Vec3 at = from;
		int sweeps = 40;
		for (size_t i = 0; i < nodes.size(); )
		{
			size_t reach = i;
			for (size_t j = std::min(nodes.size() - 1, i + 12); j > i; --j)
			{
				Vec3 end = nodes[j];
				IAISystem::ENavigationType type = IAISystem::NAV_UNSET;
				// the navigation leaves out fence posts, bins, rocks a little
				// off its edges: the body is swept along too (a few times a
				// way: they are not cheap)
				if (pAI->IsSegmentValid(mask, radius, at, end, type) && (sweeps <= 0 || (--sweeps, ClearWay(0, at, end))))
				{
					reach = j;
					break;
				}
			}
			out.push_back(nodes[reach]);
			at = nodes[reach];
			i = reach + 1;
		}
		return !out.empty();
	}

	// solid between two points (leaves and grass are not)
	bool SolidBetween(const Vec3& a, const Vec3& b, IPhysicalEntity** pSkip, int nSkip)
	{
		ray_hit hit;
		return FirstBlock(a, b - a, ent_static | ent_terrain | ent_rigid | ent_sleeping_rigid, pSkip, nSkip, hit);
	}

	// cover from a threat: of the places around the agent (up to 20 m),
	// the nearest one the threat can not see a crouching head at, not
	// toward the threat, and reachable (the AI navigation's way)
	// the server: an AI that is after the agent (alive, in the world, hostile to it)
	bool HostileTo(IActor* pActor, IActor* pAgent)
	{
		if (!pActor || pActor->IsPlayer() || pActor->GetHealth() <= 0 || pActor->GetEntity()->IsHidden())
			return false;
		IAIObject* pAI = pActor->GetEntity()->GetAI();
		IAIObject* pTarget = pAgent->GetEntity()->GetAI();
		return pAI && pTarget && pAI->IsHostile(pTarget, false);
	}

	// cover from the danger: of the places around the agent (up to 30 m),
	// one that the nearest enemies (the one it fights first) can not see a
	// crouching head at, not toward them, not away from the host, and that
	// it can walk to (the AI navigation's way)
	bool FindCover(IActor* pAgent, IActor* pThreat, std::vector<Vec3>& path, Vec3& cover)
	{
		const Vec3 from = pAgent->GetEntity()->GetWorldPos();
		// the threats: the one named, and the nearest others (up to 4 in all)
		std::vector<std::pair<float, IActor*> > others;
		IActorIteratorPtr it = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = it->Next())
			if (pActor != pThreat && HostileTo(pActor, pAgent))
			{
				const float d = pActor->GetEntity()->GetWorldPos().GetDistance(from);
				if (d < 60.0f)
					others.push_back(std::make_pair(d, pActor));
			}
		std::sort(others.begin(), others.end());
		std::vector<Vec3> eyes;
		eyes.push_back(EyePos(pThreat));
		for (size_t i = 0; i < others.size() && eyes.size() < 4; ++i)
			eyes.push_back(EyePos(others[i].second));
		IActor* pHost = g_pGame->GetIGameFramework()->GetClientActor();
		const Vec3 host = pHost ? pHost->GetEntity()->GetWorldPos() : from;
		IPhysicalEntity* skip[2];
		int nSkip = 0;
		if (IPhysicalEntity* p = pAgent->GetEntity()->GetPhysics())
			skip[nSkip++] = p;
		struct SCandidate
		{
			float score;
			Vec3 pos;
			bool operator<(const SCandidate& o) const { return score < o.score; }
		};
		std::vector<SCandidate> candidates;
		static const float s_radii[] = { 4.0f, 7.0f, 11.0f, 15.0f, 20.0f, 25.0f, 30.0f };
		for (int r = 0; r < 7; ++r)
			for (int k = 0; k < 24; ++k)
			{
				const float a = k * gf_PI2 / 24.0f;
				Vec3 p = from + Vec3(cosf(a), sinf(a), 0) * s_radii[r];
				// the ground there (not on a roof, not down a cliff)
				ray_hit hit;
				if (!gEnv->pPhysicalWorld->RayWorldIntersection(Vec3(p.x, p.y, from.z + 3.0f), Vec3(0, 0, -8.0f),
					ent_static | ent_terrain, rwi_stop_at_pierceable | rwi_colltype_any, &hit, 1, skip, nSkip))
					continue;
				p = hit.pt;
				if (fabsf(p.z - from.z) > 3.0f)
					continue;
				// hidden: solid between each of their eyes and a crouching
				// head there (the first threat's must hide it; the others count)
				int seenBy = 0;
				for (size_t e = 0; e < eyes.size(); ++e)
					if (!SolidBetween(eyes[e], p + Vec3(0, 0, 1.0f), skip, nSkip))
					{
						if (!e)
							seenBy = 100;
						++seenBy;
					}
				if (seenBy >= 100)
					continue;
				float closer = 0.0f;
				for (size_t e = 0; e < eyes.size(); ++e)
					closer += max(0.0f, Dist2D(from, eyes[e]) - Dist2D(p, eyes[e]));
				const SCandidate c = { s_radii[r] + closer * 2.0f + seenBy * 12.0f + max(0.0f, Dist2D(p, host) - 20.0f) * 0.5f, p };
				candidates.push_back(c);
			}
		// no hidden place close by: the one farthest from them it can get to
		// (along the navigation, not into a wall or down a cliff)
		if (candidates.empty())
			for (int k = 0; k < 24; ++k)
			{
				const float a = k * gf_PI2 / 24.0f;
				Vec3 p = from + Vec3(cosf(a), sinf(a), 0) * 22.0f;
				ray_hit hit;
				if (!gEnv->pPhysicalWorld->RayWorldIntersection(Vec3(p.x, p.y, from.z + 3.0f), Vec3(0, 0, -8.0f),
					ent_static | ent_terrain, rwi_stop_at_pierceable | rwi_colltype_any, &hit, 1, skip, nSkip))
					continue;
				p = hit.pt;
				if (fabsf(p.z - from.z) > 3.0f)
					continue;
				float nearest = 1e9f;
				for (size_t e = 0; e < eyes.size(); ++e)
					nearest = min(nearest, Dist2D(p, eyes[e]));
				const SCandidate c = { -nearest + max(0.0f, Dist2D(p, host) - 25.0f) * 0.5f, p };
				candidates.push_back(c);
			}
		std::sort(candidates.begin(), candidates.end());
		for (size_t i = 0; i < candidates.size() && i < 8; ++i)
		{
			if (!FindPath(from, candidates[i].pos, path))
				continue;
			// the way there not much longer than the straight line
			float length = 0.0f;
			Vec3 at = from;
			for (size_t j = 0; j < path.size(); ++j)
			{
				length += Dist2D(at, path[j]);
				at = path[j];
			}
			if (length < Dist2D(from, candidates[i].pos) * 2.0f + 6.0f)
			{
				cover = candidates[i].pos;
				return true;
			}
		}
		path.clear();
		return false;
	}

	void CmdTestAmmo(IConsoleCmdArgs* pArgs)
	{
		IActor* pActor = pArgs->GetArgCount() > 3 && gEnv->bServer ? ActorByName(pArgs->GetArg(1)) : 0;
		IEntityClass* pAmmo = pActor ? gEnv->pEntitySystem->GetClassRegistry()->FindClass(pArgs->GetArg(2)) : 0;
		if (!pAmmo || !pActor->GetInventory())
		{
			CryLogAlways("[CoopTest] coop_test_ammo: no such player or ammo");
			return;
		}
		const int count = atoi(pArgs->GetArg(3));
		pActor->GetInventory()->SetAmmoCount(pAmmo, count);
		CActor* pTarget = static_cast<CActor*>(pActor);
		if (!pActor->IsClient() && pTarget->GetChannelId())
			pTarget->GetGameObject()->InvokeRMI(CActor::ClSetAmmo(), CActor::AmmoParams(pAmmo->GetName(), count), eRMI_ToClientChannel, pTarget->GetChannelId());
		CryLogAlways("[CoopTest] %s carries %d %s now", pActor->GetEntity()->GetName(), count, pAmmo->GetName());
	}

	string EngineStateOf(IVehicle* pVehicle)
	{
		// (every movement of the game's vehicles is one of the game's)
		CVehicleMovementBase* pMovement = static_cast<CVehicleMovementBase*>(pVehicle->GetMovement());
		return pMovement ? pMovement->CoopEngineState() : string("no movement");
	}

	// the vehicles near the host's player (all: everywhere): name, class, distance,
	// seats (who sits there), the engine and its sounds
	void CmdTestVehicles(IConsoleCmdArgs* pArgs)
	{
		IActor* pHost = g_pGame->GetIGameFramework()->GetClientActor();
		IVehicleSystem* pVS = g_pGame->GetIGameFramework()->GetIVehicleSystem();
		if (!pHost || !pVS)
			return;
		const bool all = pArgs->GetArgCount() > 1 && !stricmp(pArgs->GetArg(1), "all");
		const Vec3 me = pHost->GetEntity()->GetWorldPos();
		IVehicleIteratorPtr it = pVS->CreateVehicleIterator();
		while (IVehicle* pVehicle = it->Next())
		{
			IEntity* pEntity = pVehicle->GetEntity();
			const float d = pEntity->GetWorldPos().GetDistance(me);
			if (d > 400.0f && !all)
				continue;
			string seats;
			for (unsigned int i = 1; i <= pVehicle->GetSeatCount(); ++i)
				if (IVehicleSeat* pSeat = pVehicle->GetSeatById((TVehicleSeatId)i))
					seats += string().Format(" %d:%s%s%s=%s", i, pSeat->GetSeatName(), pSeat->IsDriver() ? "(driver)" : "", pSeat->IsGunner() ? "(gunner)" : "",
						pSeat->GetPassenger() ? NameOf(pSeat->GetPassenger()) : "-");
			// its own AI (an AI vehicle moves by it) and its driver's
			IActor* pDriver = pVehicle->GetDriver();
			IAIObject* pAI = pEntity->GetAI();
			IAIObject* pDriverAI = pDriver ? pDriver->GetEntity()->GetAI() : 0;
			string ai = pAI ? string().Format("ai %s", pAI->IsEnabled() ? "on" : "off") : string("no ai");
			if (pDriver && !pDriver->IsPlayer())
				ai += string().Format(", AI driver's ai %s", pDriverAI ? (pDriverAI->IsEnabled() ? "on" : "off") : "none");
			CryLogAlways("[CoopTest] vehicle %s %s at %.0f m (%.0f %.0f %.0f)%s%s; %s; %s", pEntity->GetName(), pEntity->GetClass()->GetName(), d,
				pEntity->GetWorldPos().x, pEntity->GetWorldPos().y, pEntity->GetWorldPos().z, pVehicle->IsDestroyed() ? " destroyed" : pEntity->IsHidden() ? " hidden" : "", seats.c_str(),
				ai.c_str(), EngineStateOf(pVehicle).c_str());
		}
	}

	// coop_test_silencer <player> [0]: a silencer on (off) the weapon in his hands (server)
	void CmdTestSilencer(IConsoleCmdArgs* pArgs)
	{
		IActor* pActor = pArgs->GetArgCount() > 1 ? ActorByName(pArgs->GetArg(1)) : 0;
		CItem* pItem = pActor ? static_cast<CItem*>(pActor->GetCurrentItem()) : 0;
		if (!pItem || !gEnv->bServer)
		{
			CryLogAlways("[CoopTest] coop_test_silencer: no such player or nothing in his hands");
			return;
		}
		const bool on = pArgs->GetArgCount() < 3 || atoi(pArgs->GetArg(2)) != 0;
		const char* name = !stricmp(pItem->GetEntity()->GetClass()->GetName(), "SOCOM") ? "SOCOMSilencer" : "Silencer";
		pItem->AttachAccessory(name, on, true, true);
		CryLogAlways("[CoopTest] %s on %s's %s: %s", name, pActor->GetEntity()->GetName(), pItem->GetEntity()->GetClass()->GetName(),
			pItem->GetAccessory(name) ? "attached" : "not attached");
	}

	// coop_test_deactivate: this game's window stops being the active one of
	// its thread, as when its player went to another window: a hidden, empty
	// window of the thread becomes its active one (the test window's hook
	// lets only that one be active; it stays until the game ends, a destroyed
	// active window would hand activation on). A test game is never the
	// foreground one, so nothing changes for the person at the PC
	// (SetActiveWindow(0) and joining the input of a thread without windows
	// left the game's window active)
	void CmdTestDeactivate(IConsoleCmdArgs*)
	{
		static HWND s_hOther = 0;
		const HWND hGame = gEnv->pRenderer ? (HWND)gEnv->pRenderer->GetHWND() : 0;
		const HWND before = GetActiveWindow();
		if (!s_hOther)
		{
			WNDCLASSA wc = {};
			wc.lpfnWndProc = DefWindowProcA;
			wc.hInstance = GetModuleHandleA(0);
			wc.lpszClassName = COOP_TEST_INACTIVE_CLASS;
			RegisterClassA(&wc);
			s_hOther = CreateWindowExA(WS_EX_TOOLWINDOW, COOP_TEST_INACTIVE_CLASS, "", WS_POPUP, -32000, -32000, 1, 1, 0, 0, wc.hInstance, 0);
		}
		if (s_hOther)
			SetActiveWindow(s_hOther);
		CryLogAlways("[CoopTest] coop_test_deactivate: active window %p -> %p (the game's %p, the empty one %p)", before, GetActiveWindow(),
			hGame, s_hOther);
	}

	// coop_test_onehand <player>: his weapon in the one-hand pose, as a pick-up
	// cut short leaves it (this game only; the left hand's watch takes it back)
	void CmdTestOneHand(IConsoleCmdArgs* pArgs)
	{
		IActor* pActor = pArgs->GetArgCount() > 1 ? ActorByName(pArgs->GetArg(1)) : 0;
		CItem* pItem = pActor ? static_cast<CItem*>(pActor->GetCurrentItem()) : 0;
		if (!pItem)
		{
			CryLogAlways("[CoopTest] coop_test_onehand: no such player or nothing in his hands");
			return;
		}
		// "grab": his body as if the left hand held something up (what holding
		// a grenade or a caught object plays)
		if (pArgs->GetArgCount() > 2 && !stricmp(pArgs->GetArg(2), "grab"))
		{
			static_cast<CActor*>(pActor)->PlayAction("hold_grenade", "ignore", true);
			CryLogAlways("[CoopTest] %s holds his left hand up", pActor->GetEntity()->GetName());
			return;
		}
		pItem->PlayAction(g_pItemStrings->offhand_on);
		pItem->SetActionSuffix("akimbo_");
		CryLogAlways("[CoopTest] %s's %s in one hand", pActor->GetEntity()->GetName(), pItem->GetEntity()->GetClass()->GetName());
	}

	// coop_test_anim <player>: his animation graph as this game plays it (the
	// inputs that are not at their defaults), his weapon's action suffix and
	// his left hand's state
	void CmdTestAnim(IConsoleCmdArgs* pArgs)
	{
		IActor* pActor = pArgs->GetArgCount() > 1 ? ActorByName(pArgs->GetArg(1)) : 0;
		IAnimationGraphState* pState = pActor ? pActor->GetAnimationGraphState() : 0;
		if (!pState)
		{
			CryLogAlways("[CoopTest] coop_test_anim: no such player or no animation graph");
			return;
		}
		// coop_test_anim <player> <input> <value>: sets it first
		if (pArgs->GetArgCount() > 3)
			pState->SetInput(pState->GetInputId(pArgs->GetArg(2)), pArgs->GetArg(3));
		string inputs;
		for (int i = 0; i < 256; ++i)
		{
			const char* name = pState->GetInputName((IAnimationGraphState::InputID)i);
			if (!name)
				break;
			if (pState->IsDefaultInputValue((IAnimationGraphState::InputID)i))
				continue;
			char value[256] = "";
			pState->GetInput((IAnimationGraphState::InputID)i, value);
			inputs += string().Format(" %s=%s", name, value);
		}
		CItem* pItem = static_cast<CItem*>(pActor->GetCurrentItem());
		COffHand* pOffHand = static_cast<COffHand*>(static_cast<CActor*>(pActor)->GetWeaponByClass(CItem::sOffHandClass));
		CryLogAlways("[CoopTest] anim %s: state %s;%s; item %s suffix '%s'; left hand 0x%x", pActor->GetEntity()->GetName(),
			pState->GetCurrentStateName(), inputs.c_str(), pItem ? pItem->GetEntity()->GetClass()->GetName() : "-",
			pItem ? pItem->GetActionSuffix(0) : "", pOffHand ? (unsigned)pOffHand->GetOffHandState() : 0u);
	}

	// coop_test_key <action> <1|0>: this game's player presses (lets go of)
	// a key's action, as the keyboard does (grenade, use, attack1...)
	void CmdTestKey(IConsoleCmdArgs* pArgs)
	{
		CPlayer* pPlayer = static_cast<CPlayer*>(g_pGame->GetIGameFramework()->GetClientActor());
		if (!pPlayer || pArgs->GetArgCount() < 3)
		{
			CryLogAlways("[CoopTest] coop_test_key: no player here, or no action and 1/0");
			return;
		}
		const bool press = atoi(pArgs->GetArg(2)) != 0;
		Press(pPlayer, ActionId(pArgs->GetArg(1)), press);
		CryLogAlways("[CoopTest] %s %s %s", pPlayer->GetEntity()->GetName(), press ? "presses" : "lets go of", pArgs->GetArg(1));
	}

	// coop_test_cam: this game's camera (where, which way), the view it
	// comes from, the player's vehicle seat and its view
	void CmdTestCam(IConsoleCmdArgs*)
	{
		const CCamera& cam = gEnv->pSystem->GetViewCamera();
		const Vec3 p = cam.GetPosition(), d = cam.GetViewdir();
		IViewSystem* pViews = g_pGame->GetIGameFramework()->GetIViewSystem();
		IView* pView = pViews ? pViews->GetActiveView() : 0;
		const SViewParams* pParams = pView ? pView->GetCurrentParams() : 0;
		IActor* pMe = g_pGame->GetIGameFramework()->GetClientActor();
		IVehicle* pVehicle = pMe ? pMe->GetLinkedVehicle() : 0;
		IVehicleSeat* pSeat = pVehicle ? pVehicle->GetSeatForPassenger(pMe->GetEntityId()) : 0;
		IVehicleView* pSeatView = pSeat ? pSeat->GetView(pSeat->GetCurrentView()) : 0;
		const float water = gEnv->p3DEngine->GetWaterLevel(&p);
		CryLogAlways("[CoopTest] water there %.1f (%s)", water, water > WATER_LEVEL_UNKNOWN && p.z < water ? "UNDER WATER" : "above");
		CryLogAlways("[CoopTest] camera (%.1f, %.1f, %.1f) looking (%.2f, %.2f, %.2f) fov %.2f; view of %s (params at %.1f %.1f %.1f); me at %s; vehicle %s at %s seat %s view %d %s",
			p.x, p.y, p.z, d.x, d.y, d.z, cam.GetFov(), pView ? NameOf(pView->GetLinkedId()) : "-",
			pParams ? pParams->position.x : 0.0f, pParams ? pParams->position.y : 0.0f, pParams ? pParams->position.z : 0.0f,
			pMe ? Vec(pMe->GetEntity()->GetWorldPos()).c_str() : "-", pVehicle ? pVehicle->GetEntity()->GetName() : "-",
			pVehicle ? Vec(pVehicle->GetEntity()->GetWorldPos()).c_str() : "-", pSeat ? pSeat->GetSeatName() : "-",
			pSeat ? (int)pSeat->GetCurrentView() : -1, pSeatView ? (pSeatView->IsThirdPerson() ? "third person" : "first person") : "-");
	}

	// coop_test_vehview: this game's player in a vehicle takes the seat's next
	// view (first person / third person), as the view key does
	void CmdTestVehView(IConsoleCmdArgs*)
	{
		IActor* pMe = g_pGame->GetIGameFramework()->GetClientActor();
		IVehicle* pVehicle = pMe ? pMe->GetLinkedVehicle() : 0;
		if (!pVehicle)
		{
			CryLogAlways("[CoopTest] coop_test_vehview: not in a vehicle");
			return;
		}
		IVehicleSeat* pSeat = pVehicle->GetSeatForPassenger(pMe->GetEntityId());
		if (!pSeat)
			return;
		const TVehicleViewId next = pSeat->GetNextView(pSeat->GetCurrentView());
		const bool ok = pSeat->SetView(next);
		CryLogAlways("[CoopTest] view %d in %s's %s: %s", (int)next, pVehicle->GetEntity()->GetName(), pSeat->GetSeatName(), ok ? "set" : "refused");
	}

	// coop_test_grid [x y z] [radius] [x y z]: the squares the companion
	// finds its way round on, around a place (this game's player by
	// default), one log line a row: # the body does not fit, . it does, o
	// a light loose thing to push aside, @ the place, and with a second
	// place the way found there (* its corners)
	void CmdTestGrid(IConsoleCmdArgs* pArgs)
	{
		Vec3 at(ZERO);
		int arg = 1;
		if (pArgs->GetArgCount() >= 4)
		{
			at = Vec3((float)atof(pArgs->GetArg(1)), (float)atof(pArgs->GetArg(2)), (float)atof(pArgs->GetArg(3)));
			arg = 4;
		}
		else if (IActor* pMe = g_pGame->GetIGameFramework()->GetClientActor())
			at = pMe->GetEntity()->GetWorldPos();
		const float radius = clamp_tpl(pArgs->GetArgCount() > arg ? (float)atof(pArgs->GetArg(arg)) : 8.0f, 2.0f, 16.0f);
		GridBegin(0, at, radius);
		std::vector<Vec3> way;
		int looked = 0;
		if (pArgs->GetArgCount() >= arg + 4)
		{
			const Vec3 to((float)atof(pArgs->GetArg(arg + 1)), (float)atof(pArgs->GetArg(arg + 2)), (float)atof(pArgs->GetArg(arg + 3)));
			const bool ok = GridPath(at, to, way, &looked);
			CryLogAlways("[CoopTest] grid way from (%.1f %.1f) to (%.1f %.1f): %s, %d squares looked at", at.x, at.y, to.x, to.y,
				ok ? string().Format("%d corners, %.1f m", (int)way.size(), WayLeftOf(at, way)).c_str() : "none", looked);
			for (size_t i = 0; i < way.size(); ++i)
				CryLogAlways("[CoopTest] grid corner %d: %.1f %.1f %.1f", (int)i, way[i].x, way[i].y, way[i].z);
		}
		const SGrid& g = s_grid;
		CryLogAlways("[CoopTest] grid around (%.1f %.1f %.1f), %d x %d squares of %.1f m, rows north (+y) first, west (-x) left",
			at.x, at.y, at.z, g.n, g.n, GRID_CELL);
		for (int cy = g.n - 1; cy >= 0; --cy)
		{
			string row;
			for (int cx = 0; cx < g.n; ++cx)
			{
				const int i = cy * g.n + cx;
				char c = GridFree(cx, cy) ? (g.cost[i] > 1 ? 'o' : '.') : '#';
				for (size_t k = 0; k < way.size(); ++k)
					if (GridIndex(way[k]) == i)
						c = '*';
				if (GridIndex(at) == i)
					c = '@';
				row += c;
			}
			CryLogAlways("[CoopTest] grid %+5.1f |%s|", g.y0 + (cy + 0.5f) * GRID_CELL - at.y, row.c_str());
		}
	}

	// coop_test_findobj [x y z] [radius] [filter]: the physical things near a
	// place (this game's player by default): what they are (a level brush's
	// model, an entity), where, their parts and how many breakable joints
	// hold them (a building that collapses has them)
	void CmdTestFindObj(IConsoleCmdArgs* pArgs)
	{
		Vec3 at(ZERO);
		int arg = 1;
		if (pArgs->GetArgCount() >= 4)
		{
			at = Vec3((float)atof(pArgs->GetArg(1)), (float)atof(pArgs->GetArg(2)), (float)atof(pArgs->GetArg(3)));
			arg = 4;
		}
		else if (IActor* pMe = g_pGame->GetIGameFramework()->GetClientActor())
			at = pMe->GetEntity()->GetWorldPos();
		const float radius = pArgs->GetArgCount() > arg ? (float)atof(pArgs->GetArg(arg)) : 15.0f;
		const char* filter = pArgs->GetArgCount() > arg + 1 ? pArgs->GetArg(arg + 1) : "";
		IPhysicalEntity** list = 0;
		const int n = gEnv->pPhysicalWorld->GetEntitiesInBox(at - Vec3(radius), at + Vec3(radius), list, ent_static | ent_rigid | ent_sleeping_rigid);
		int shown = 0, withJoints = 0;
		for (int i = 0; i < n && shown < 40; ++i)
		{
			IPhysicalEntity* pPhys = list[i];
			pe_params_foreign_data fd;
			pPhys->GetParams(&fd);
			string what = "?";
			Vec3 pos(ZERO);
			pe_status_pos sp;
			if (pPhys->GetStatus(&sp))
				pos = sp.pos;
			if (fd.iForeignData == PHYS_FOREIGN_ID_STATIC && fd.pForeignData)
			{
				IRenderNode* pNode = (IRenderNode*)fd.pForeignData;
				IStatObj* pObj = pNode->GetEntityStatObj(0);
				what.Format("brush %s", pObj ? pObj->GetFilePath() : pNode->GetName());
			}
			else if (fd.iForeignData == PHYS_FOREIGN_ID_ENTITY && fd.pForeignData)
			{
				IEntity* pEntity = (IEntity*)fd.pForeignData;
				IStatObj* pObj = pEntity->GetStatObj(0);
				what.Format("entity %s (%s) %s", pEntity->GetName(), pEntity->GetClass()->GetName(), pObj ? pObj->GetFilePath() : "");
			}
			else
				what.Format("foreign %d", fd.iForeignData);
			if (*filter && !CryStringUtils::stristr(what.c_str(), filter))
				continue;
			pe_status_nparts np;
			const int parts = pPhys->GetStatus(&np);
			int joints = 0, breakable = 0, broken = 0;
			for (int j = 0; j < 256; ++j)
			{
				pe_params_structural_joint sj;
				sj.idx = j;
				if (!pPhys->GetParams(&sj))
					break;
				++joints;
				if (sj.bBreakable)
					++breakable;
				if (sj.bBroken)
					++broken;
			}
			if (joints)
				++withJoints;
			CryLogAlways("[CoopTest] %s at (%.1f, %.1f, %.1f) %.0f m: type %d, %d parts, %d joints (%d breakable, %d broken)", what.c_str(),
				pos.x, pos.y, pos.z, pos.GetDistance(at), (int)pPhys->GetType(), parts, joints, breakable, broken);
			++shown;
		}
		CryLogAlways("[CoopTest] findobj: %d physical things within %.0f m of (%.0f, %.0f, %.0f), %d shown, %d with joints", n, radius, at.x, at.y, at.z, shown, withJoints);
	}

	// coop_test_physvars [name value]: the physics' settings that decide
	// breaking (and sets one: multiplayer, breakscale, playerscanbreak)
	void CmdTestPhysVars(IConsoleCmdArgs* pArgs)
	{
		PhysicsVars* pVars = gEnv->pPhysicalWorld->GetPhysVars();
		if (pArgs->GetArgCount() >= 3)
		{
			const char* name = pArgs->GetArg(1);
			const float value = (float)atof(pArgs->GetArg(2));
			if (!stricmp(name, "multiplayer"))
				pVars->bMultiplayer = (int)value;
			else if (!stricmp(name, "breakscale"))
				pVars->breakImpulseScale = value;
			else if (!stricmp(name, "playerscanbreak"))
				pVars->bPlayersCanBreak = (int)value;
		}
		CryLogAlways("[CoopTest] physics: multiplayer %d, breakImpulseScale %.3f, playersCanBreak %d, tickBreakable %.3f, breakOnValidation %d, logStructureChanges %d; bMultiplayer %d, bServer %d",
			pVars->bMultiplayer, pVars->breakImpulseScale, pVars->bPlayersCanBreak, pVars->tickBreakable, pVars->bBreakOnValidation, pVars->bLogStructureChanges,
			(int)gEnv->bMultiplayer, (int)gEnv->bServer);
	}

	// coop_test_vehdamage [vehicle]: a vehicle's damage (this game's
	// player's by default): the whole and every component
	void CmdTestVehDamage(IConsoleCmdArgs* pArgs)
	{
		IVehicleSystem* pVehicles = g_pGame->GetIGameFramework()->GetIVehicleSystem();
		IVehicle* pVehicle = 0;
		if (pArgs->GetArgCount() > 1)
		{
			if (IEntity* pEntity = gEnv->pEntitySystem->FindEntityByName(pArgs->GetArg(1)))
				pVehicle = pVehicles->GetVehicle(pEntity->GetId());
		}
		else if (IActor* pMe = g_pGame->GetIGameFramework()->GetClientActor())
			pVehicle = pMe->GetLinkedVehicle();
		if (!pVehicle)
		{
			CryLogAlways("[CoopTest] coop_test_vehdamage: no such vehicle");
			return;
		}
		string parts;
		for (int i = 0; i < pVehicle->GetComponentCount(); ++i)
			if (IVehicleComponent* pComp = pVehicle->GetComponent(i))
				if (pComp->GetDamageRatio() > 0.0f)
				{
					string one;
					one.Format(" %s=%.2f", pComp->GetComponentName(), pComp->GetDamageRatio());
					parts += one;
				}
		CryLogAlways("[CoopTest] vehicle %s damage %.2f (major %.2f), destroyed %d, components hit:%s", pVehicle->GetEntity()->GetName(),
			pVehicle->GetDamageRatio(false), pVehicle->GetDamageRatio(true), (int)pVehicle->IsDestroyed(), parts.empty() ? " none" : parts.c_str());
	}

	// coop_test_levelmodels: the campaign level's own models (Levels/<level>/
	// brush/*.cgf): how many the engine has, how many are its default ball
	void CmdTestLevelModels(IConsoleCmdArgs*)
	{
		ILevel* pLevel = g_pGame->GetIGameFramework()->GetILevelSystem()->GetCurrentLevel();
		const char* name = pLevel ? pLevel->GetLevelInfo()->GetName() : "";
		const char* base = strrchr(name, '/');
		base = base ? base + 1 : name;
		if (strnicmp(base, "coop_", 5) == 0)
			base += 5;
		string mask;
		mask.Format("Levels/%s/brush/*.cgf", base);
		int found = 0, balls = 0;
		string ballNames;
		_finddata_t fd;
		intptr_t h = gEnv->pCryPak->FindFirst(mask.c_str(), &fd);
		if (h != -1)
		{
			do
			{
				string path;
				path.Format("Levels/%s/brush/%s", base, fd.name);
				IStatObj* pObj = gEnv->p3DEngine->LoadStatObj(path.c_str());
				if (!pObj || pObj->IsDefaultObject())
				{
					++balls;
					if (balls <= 5)
						ballNames += string(" ") + fd.name;
				}
				else
					++found;
			} while (gEnv->pCryPak->FindNext(h, &fd) >= 0);
			gEnv->pCryPak->FindClose(h);
		}
		CryLogAlways("[CoopTest] level models of %s: %d found, %d default balls%s", base, found, balls, ballNames.c_str());
	}

	// coop_test_enter <vehicle> <seat id>: the host's player gets in (server)
	void CmdTestEnter(IConsoleCmdArgs* pArgs)
	{
		IActor* pHost = g_pGame->GetIGameFramework()->GetClientActor();
		// coop_test_enter out: the host gets out
		if (pHost && pArgs->GetArgCount() > 1 && !stricmp(pArgs->GetArg(1), "out"))
		{
			IVehicle* pIn = pHost->GetLinkedVehicle();
			IVehicleSeat* pMine = pIn ? pIn->GetSeatForPassenger(pHost->GetEntityId()) : 0;
			const bool ok = pMine && pMine->Exit(false, true);
			CryLogAlways("[CoopTest] the host gets out: %s", ok ? "ok" : "not in a vehicle");
			return;
		}
		IEntity* pEntity = pArgs->GetArgCount() > 2 ? gEnv->pEntitySystem->FindEntityByName(pArgs->GetArg(1)) : 0;
		IVehicle* pVehicle = pEntity ? g_pGame->GetIGameFramework()->GetIVehicleSystem()->GetVehicle(pEntity->GetId()) : 0;
		IVehicleSeat* pSeat = pVehicle ? pVehicle->GetSeatById((TVehicleSeatId)atoi(pArgs->GetArg(2))) : 0;
		if (!pHost || !pSeat || !gEnv->bServer)
		{
			CryLogAlways("[CoopTest] coop_test_enter: no such vehicle or seat");
			return;
		}
		const bool ok = pSeat->Enter(pHost->GetEntityId(), true);
		CryLogAlways("[CoopTest] the host gets into %s seat %s: %s", pEntity->GetName(), pSeat->GetSeatName(), ok ? "ok" : "refused");
	}

	void CmdTestPath(IConsoleCmdArgs* pArgs)
	{
		IEntity* pTo = pArgs->GetArgCount() > 1 ? gEnv->pEntitySystem->FindEntityByName(pArgs->GetArg(1)) : 0;
		IActor* pFrom = g_pGame->GetIGameFramework()->GetClientActor();
		if (!pTo || !pFrom || !gEnv->bServer)
		{
			CryLogAlways("[CoopTest] coop_test_path: no such entity (or not the server)");
			return;
		}
		std::vector<Vec3> path;
		const CTimeValue t0 = gEnv->pTimer->GetAsyncTime();
		const bool ok = FindPath(pFrom->GetEntity()->GetWorldPos(), pTo->GetWorldPos(), path);
		string text;
		for (size_t i = 0; i < path.size(); ++i)
			text += string().Format(" (%.0f %.0f %.0f)", path[i].x, path[i].y, path[i].z);
		CryLogAlways("[CoopTest] way to %s (%.0f m): %s, %d points in %.1f ms:%s", pTo->GetName(),
			pFrom->GetEntity()->GetWorldPos().GetDistance(pTo->GetWorldPos()), ok ? "found" : "NONE", (int)path.size(),
			(gEnv->pTimer->GetAsyncTime() - t0).GetMilliSeconds(), text.c_str());
	}

	// the server: a companion put next to a vehicle gets in a moment later
	struct SPendingEnter
	{
		EntityId agent, vehicle;
		float at;
		SPendingEnter(EntityId a, EntityId v, float t) : agent(a), vehicle(v), at(t) {}
	};
	std::vector<SPendingEnter> s_enters;
	IVehicleSeat* FreeSeat(IVehicle* pVehicle);

	void UpdatePendingEnters()
	{
		for (size_t i = 0; i < s_enters.size(); )
		{
			if (Now() < s_enters[i].at)
			{
				++i;
				continue;
			}
			IActor* pAgent = ActorOf(s_enters[i].agent);
			IVehicle* pVehicle = g_pGame->GetIGameFramework()->GetIVehicleSystem()->GetVehicle(s_enters[i].vehicle);
			if (pAgent && pVehicle && pAgent->GetHealth() > 0 && !pAgent->GetLinkedVehicle() && !pVehicle->IsDestroyed())
				if (IVehicleSeat* pSeat = FreeSeat(pVehicle))
				{
					pSeat->Enter(pAgent->GetEntityId(), false);
					CryLogAlways("[CoopAgent] %s gets into %s", pAgent->GetEntity()->GetName(), pVehicle->GetEntity()->GetName());
				}
			s_enters.erase(s_enters.begin() + i);
		}
	}

	IVehicleSeat* FreeSeat(IVehicle* pVehicle)
	{
		// the gun first (it fights from there), then a passenger's seat, the
		// wheel last
		IVehicleSeat* pPassenger = 0;
		IVehicleSeat* pAny = 0;
		for (unsigned int i = 1; i <= pVehicle->GetSeatCount(); ++i)
		{
			IVehicleSeat* pSeat = pVehicle->GetSeatById((TVehicleSeatId)i);
			if (!pSeat || pSeat->GetPassenger())
				continue;
			if (pSeat->IsGunner() && !pSeat->IsDriver())
				return pSeat;
			if (!pSeat->IsDriver() && !pPassenger)
				pPassenger = pSeat;
			if (!pAny)
				pAny = pSeat;
		}
		return pPassenger ? pPassenger : pAny;
	}
}

string CoopAgent::EngineState(IVehicle* pVehicle)
{
	return pVehicle ? EngineStateOf(pVehicle) : string();
}

void CoopAgent::Init()
{
	s_pCompanion = gEnv->pConsole->RegisterInt("coop_companion", 0, VF_DUMPTODISK,
		"Crysis Coop: 1 = the host's game starts an AI companion, a second player played by the game (or by an AI agent through MCP)");
	s_pAgent = gEnv->pConsole->RegisterInt("coop_agent", 0, 0, "Crysis Coop: 1 = this game is the AI companion");
	s_pPort = gEnv->pConsole->RegisterInt("coop_agent_port", 47810, 0, "Crysis Coop: the AI companion's port for AI agents (127.0.0.1)");
	s_pJoin = gEnv->pConsole->RegisterInt("coop_agent_join", 0, 0, "Crysis Coop: the AI companion joins the host's game at 127.0.0.1:<this port>");
	s_pFps = gEnv->pConsole->RegisterInt("coop_agent_fps", 20, 0, "Crysis Coop: the AI companion's game runs at most this many frames a second");
	s_pParent = gEnv->pConsole->RegisterInt("coop_agent_parent", 0, 0, "Crysis Coop: the host's process; the AI companion's game ends with it");
	s_pName = gEnv->pConsole->RegisterString("coop_agent_name", "Companion", 0, "Crysis Coop: the AI companion's player name");
	PatchPause();
	// this game's own settings: a server sends its console variables to every
	// client that connects, and the companion stopped being one when it
	// joined (the host's coop_agent is 0)
	s_pFrameLog = gEnv->pConsole->RegisterInt("coop_test_framelog", 0, 0,
		"Crysis Coop testing: 1 = this game's frames a second in the log every 5 s, also while it loads or joins a game");
	ICVar* own[] = { s_pCompanion, s_pAgent, s_pPort, s_pJoin, s_pFps, s_pParent, s_pName, s_pFrameLog };
	for (int i = 0; i < (int)(sizeof(own) / sizeof(own[0])); ++i)
		own[i]->SetFlags(own[i]->GetFlags() | VF_NOT_NET_SYNCED);
	// the companion's game never takes the mouse (the host's value is 0)
	if (ICVar* pFree = gEnv->pConsole->GetCVar("coop_test_free_cursor"))
		pFree->SetFlags(pFree->GetFlags() | VF_NOT_NET_SYNCED);
	gEnv->pConsole->AddCommand("coop_test_ammo", CmdTestAmmo, 0,
		"Crysis Coop testing: coop_test_ammo <player> <ammo class> <count> sets what he carries of it (server)");
	gEnv->pConsole->AddCommand("coop_test_vehicles", CmdTestVehicles, 0, "Crysis Coop testing: coop_test_vehicles [all] logs the vehicles near the host's player (all: everywhere), their seats and engines");
	gEnv->pConsole->AddCommand("coop_test_silencer", CmdTestSilencer, 0, "Crysis Coop testing: coop_test_silencer <player> [0]: a silencer on (off) his weapon (server)");
	gEnv->pConsole->AddCommand("coop_test_deactivate", CmdTestDeactivate, 0, "Crysis Coop testing: coop_test_deactivate: this game's window stops being the active one, as when its player goes to another window");
	gEnv->pConsole->AddCommand("coop_test_onehand", CmdTestOneHand, 0, "Crysis Coop testing: coop_test_onehand <player> [grab]: his weapon in the one-hand pose, or (grab) his left hand held up (this game)");
	gEnv->pConsole->AddCommand("coop_test_anim", CmdTestAnim, 0, "Crysis Coop testing: coop_test_anim <player>: logs his animation graph inputs as this game plays them");
	gEnv->pConsole->AddCommand("coop_test_key", CmdTestKey, 0, "Crysis Coop testing: coop_test_key <action> <1|0>: this game's player presses (lets go of) a key's action");
	gEnv->pConsole->AddCommand("coop_test_cam", CmdTestCam, 0, "Crysis Coop testing: coop_test_cam logs this game's camera, its view and the vehicle seat's view");
	gEnv->pConsole->AddCommand("coop_test_vehview", CmdTestVehView, 0, "Crysis Coop testing: coop_test_vehview: the vehicle seat's next view for this game's player");
	gEnv->pConsole->AddCommand("coop_test_grid", CmdTestGrid, 0, "Crysis Coop testing: coop_test_grid [x y z] [radius] [to x y z]: the squares the companion finds its way round on, and the way to a place");
	gEnv->pConsole->AddCommand("coop_test_findobj", CmdTestFindObj, 0, "Crysis Coop testing: coop_test_findobj [x y z] [radius] [filter]: physical things near a place, their model, parts and breakable joints");
	gEnv->pConsole->AddCommand("coop_test_physvars", CmdTestPhysVars, 0, "Crysis Coop testing: coop_test_physvars [multiplayer|breakscale|playerscanbreak value]: the physics settings that decide breaking");
	gEnv->pConsole->AddCommand("coop_test_vehdamage", CmdTestVehDamage, 0, "Crysis Coop testing: coop_test_vehdamage [vehicle]: a vehicle's damage, whole and by component");
	gEnv->pConsole->AddCommand("coop_test_levelmodels", CmdTestLevelModels, 0, "Crysis Coop testing: coop_test_levelmodels: the level's own brush models found / default balls");
	gEnv->pConsole->AddCommand("coop_test_enter", CmdTestEnter, 0, "Crysis Coop testing: coop_test_enter <vehicle> <seat id>: the host's player gets in (server)");
	gEnv->pConsole->AddCommand("coop_test_path", CmdTestPath, 0,
		"Crysis Coop testing: coop_test_path <entity name> logs the AI navigation's way from the host's player to it (server)");
}

bool CoopAgent::IsCompanion()
{
	return s_pAgent && s_pAgent->GetIVal() != 0;
}

void CoopAgent::Update(float frameTime)
{
	if (!s_pAgent || !g_pGame)
		return;
	QueryPerformanceCounter(&s_frameStart);
	if (IsCompanion())
	{
		UpdateCompanionGame();
		UpdateBridge();
		Think(frameTime);
	}
	else
		UpdateHost();
}

void CoopAgent::EndFrame()
{
	LogFrames();
	if (!IsCompanion() || s_pFps->GetIVal() <= 0)
		return;
	// the frames it does not need are slept away (a light background game)
	static LARGE_INTEGER s_last = {};
	LARGE_INTEGER now, freq;
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&freq);
	if (s_last.QuadPart)
	{
		const double spent = double(now.QuadPart - s_last.QuadPart) / double(freq.QuadPart);
		const double frame = 1.0 / s_pFps->GetIVal();
		if (spent < frame)
			Sleep((DWORD)((frame - spent) * 1000.0));
	}
	QueryPerformanceCounter(&s_last);
}

void CoopAgent::SteerInput(CPlayer* pPlayer, Ang3& deltaRotation, Vec3& deltaMovement, uint32& actions)
{
	if (!IsCompanion() || !pPlayer || !pPlayer->IsClient())
		return;
	const float frameTime = gEnv->pTimer->GetFrameTime();
	if (s_ctl.look)
	{
		float yaw, pitch;
		ViewAngles(pPlayer, yaw, pitch);
		const float step = s_ctl.turnRate * (frameTime > 0.001f ? frameTime : 0.001f);
		deltaRotation.z = clamp_tpl(Wrap(s_ctl.yaw - yaw), -step, step);
		deltaRotation.x = clamp_tpl(clamp_tpl(s_ctl.pitch, -1.2f, 1.2f) - pitch, -step, step);
	}
	else
		deltaRotation.Set(0, 0, 0);
	if (s_ctl.moveDir.GetLengthSquared() > 0.01f)
	{
		float yaw, pitch;
		ViewAngles(pPlayer, yaw, pitch);
		const Vec3 forward(-sinf(yaw), cosf(yaw), 0);
		const Vec3 right(cosf(yaw), sinf(yaw), 0);
		deltaMovement.Set(s_ctl.moveDir.Dot(right), s_ctl.moveDir.Dot(forward), 0);
		deltaMovement.NormalizeSafe();
		actions |= ACTION_MOVE;
	}
	else
	{
		deltaMovement.zero();
		actions &= ~ACTION_MOVE;
	}
	if (s_ctl.sprint)
		actions |= ACTION_SPRINT;
	else
		actions &= ~ACTION_SPRINT;
	if (s_ctl.crouch)
		actions |= ACTION_CROUCH;
	else
		actions &= ~ACTION_CROUCH;
	if (s_ctl.jump)
	{
		actions |= ACTION_JUMP;
		s_ctl.jump = false;
	}
	else
		actions &= ~ACTION_JUMP;
}

void CoopAgent::OnChat(EntityId source, const char* text)
{
	if (!IsCompanion() || !text)
		return;
	SLine l = { ++s_chatN, Now(), NameOf(source), text };
	s_chat.push_back(l);
	while (s_chat.size() > 40)
		s_chat.pop_front();
}

void CoopAgent::OnServerRequest(EntityId agent, int op, const char* name)
{
	CGameRules* pRules = g_pGame ? g_pGame->GetGameRules() : 0;
	IActor* pAgent = ActorOf(agent);
	if (!pRules || !pAgent || pAgent->GetHealth() <= 0)
		return;
	IVehicle* pMine = pAgent->GetLinkedVehicle();
	// where the companion is in its own game: far from here twice running,
	// it is put back here (everybody else sees it here, and here the use
	// key, the shots, the damage count)
	if (op == 7)
	{
		static std::map<EntityId, int> s_off, s_seat;
		Vec3 there;
		char vehicle[128] = "-";
		if (!name || sscanf(name, "%f %f %f %127s", &there.x, &there.y, &there.z, vehicle) < 3)
			return;
		const Vec3 here = pAgent->GetEntity()->GetWorldPos();
		// where the downed teammates lie here (a body falls its own way on
		// each computer: the companion goes to the one the revive counts at)
		IActorIteratorPtr it = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = it->Next())
			if (pActor != pAgent && IsDown(pActor))
			{
				const Vec3 at = pActor->GetEntity()->GetWorldPos();
				pRules->CoopSendSync(16, 2, 0, pActor->GetEntity()->GetName(), string().Format("%.2f %.2f %.2f", at.x, at.y, at.z).c_str(), 0, 0.0f,
					static_cast<CActor*>(pAgent)->GetChannelId());
			}
		// seated here, walking in its own game (put into a seat it never saw
		// itself get into: after a load, from afar): out of the seat here, it
		// is where its own game has it
		const bool seatedThere = strcmp(vehicle, "-") != 0;
		int& seatTimes = s_seat[agent];
		if (pMine && !seatedThere)
		{
			if (++seatTimes >= 2)
			{
				seatTimes = 0;
				// (where it is, its own game tells this one as it walks on)
				if (IVehicleSeat* pSeat = pMine->GetSeatForPassenger(agent))
					pSeat->Exit(false, true);
				CryLogAlways("[CoopAgent] %s sat in %s here, walks at %.1f %.1f %.1f in its own game: out of the seat here", pAgent->GetEntity()->GetName(),
					pMine->GetEntity()->GetName(), there.x, there.y, there.z);
			}
			return;
		}
		seatTimes = 0;
		if (pMine || seatedThere)
			return;
		const float off = Dist2D(here, there);
		int& times = s_off[agent];
		if (off < 2.0f && fabsf(here.z - there.z) < 2.5f)
		{
			times = 0;
			return;
		}
		if (++times < 2)
			return;
		times = 0;
		pRules->MovePlayer(static_cast<CActor*>(pAgent), here, pAgent->GetEntity()->GetWorldAngles());
		CryLogAlways("[CoopAgent] %s was %.1f m off in its own game (at %.1f %.1f %.1f, here %.1f %.1f %.1f): put back here",
			pAgent->GetEntity()->GetName(), off, there.x, there.y, there.z, here.x, here.y, here.z);
		return;
	}
	if (op == 2)
	{
		if (IVehicleSeat* pSeat = pMine ? pMine->GetSeatForPassenger(agent) : 0)
		{
			pSeat->Exit(true, true);
			CryLogAlways("[CoopAgent] %s gets out of %s", pAgent->GetEntity()->GetName(), pMine->GetEntity()->GetName());
		}
		return;
	}
	IVehicle* pVehicle = 0;
	IActor* pLeader = 0;
	if (op == 3)
	{
		IEntity* pEntity = name ? gEnv->pEntitySystem->FindEntityByName(name) : 0;
		pVehicle = pEntity ? g_pGame->GetIGameFramework()->GetIVehicleSystem()->GetVehicle(pEntity->GetId()) : 0;
	}
	else if (op == 1)
	{
		pLeader = ActorByName(name);
		pVehicle = pLeader ? pLeader->GetLinkedVehicle() : 0;
	}
	if (pVehicle && pVehicle != pMine && !pVehicle->IsDestroyed())
	{
		if (IVehicleSeat* pSeat = FreeSeat(pVehicle))
		{
			if (pMine)
				if (IVehicleSeat* pOld = pMine->GetSeatForPassenger(agent))
					pOld->Exit(false, true);
			const Vec3 at = pVehicle->GetEntity()->GetWorldPos();
			if (Dist2D(pAgent->GetEntity()->GetWorldPos(), at) > 6.0f)
			{
				// from afar (the vehicle drove off): its body was put into the
				// vehicle's own, which threw the vehicle (the driver's camera
				// stuck or flew off). Next to it first, and in a moment later,
				// and not while it drives fast
				pe_status_dynamics dyn;
				IPhysicalEntity* pPhys = pVehicle->GetEntity()->GetPhysics();
				if (pPhys && pPhys->GetStatus(&dyn) && dyn.v.GetLength() > 3.0f)
					return;
				AABB box;
				pVehicle->GetEntity()->GetLocalBounds(box);
				const Matrix34& tm = pVehicle->GetEntity()->GetWorldTM();
				const Vec3 side = tm.GetColumn0().GetNormalizedSafe(Vec3(1, 0, 0)) * (max(box.max.x, -box.min.x) + 1.5f);
				pRules->MovePlayer(static_cast<CActor*>(pAgent), at + side + Vec3(0, 0, 0.5f), pAgent->GetEntity()->GetWorldAngles());
				s_enters.push_back(SPendingEnter(agent, pVehicle->GetEntityId(), Now() + 0.5f));
				CryLogAlways("[CoopAgent] %s put next to %s, gets in a moment later", pAgent->GetEntity()->GetName(), pVehicle->GetEntity()->GetName());
				return;
			}
			pSeat->Enter(agent, false);
			CryLogAlways("[CoopAgent] %s gets into %s", pAgent->GetEntity()->GetName(), pVehicle->GetEntity()->GetName());
		}
		return;
	}
	// a way for it to walk: around what is in the way (the AI navigation of
	// the host's game; the companion's own game has none), sent back to it
	if (op == 5 && !pMine && name)
	{
		Vec3 goal;
		if (sscanf(name, "%f %f %f", &goal.x, &goal.y, &goal.z) != 3)
			return;
		std::vector<Vec3> path;
		const Vec3 from = pAgent->GetEntity()->GetWorldPos();
		string text;
		if (FindPath(from, goal, path))
		{
			for (size_t i = 0; i < path.size() && i < 20; ++i)
				text += string().Format("%s%.1f %.1f %.1f", i ? ";" : "", path[i].x, path[i].y, path[i].z);
		}
		CryLogAlways("[CoopAgent] a way for %s: %d points over %.0f m%s%s", pAgent->GetEntity()->GetName(), (int)path.size(),
			from.GetDistance(goal), text.empty() ? " (none found: it goes straight)" : ": ", text.c_str());
		pRules->CoopSendSync(16, 0, 0, name, text.c_str(), 0, 0.0f, static_cast<CActor*>(pAgent)->GetChannelId());
		return;
	}
	// cover from an enemy for it: a place close by out of the enemy's sight,
	// with the way there
	if (op == 6 && !pMine && name)
	{
		IActor* pThreat = ActorByName(name);
		std::vector<Vec3> path;
		Vec3 cover(ZERO);
		string text, goal;
		if (pThreat && FindCover(pAgent, pThreat, path, cover))
			for (size_t i = 0; i < path.size() && i < 20; ++i)
				text += string().Format("%s%.1f %.1f %.1f", i ? ";" : "", path[i].x, path[i].y, path[i].z);
		goal.Format("%.1f %.1f %.1f", cover.x, cover.y, cover.z);
		CryLogAlways("[CoopAgent] cover for %s from %s: %s", pAgent->GetEntity()->GetName(), name,
			text.empty() ? "none found" : string().Format("%.0f m away, %d points", cover.GetDistance(pAgent->GetEntity()->GetWorldPos()), (int)path.size()).c_str());
		pRules->CoopSendSync(16, 1, 0, goal.c_str(), text.c_str(), 0, 0.0f, static_cast<CActor*>(pAgent)->GetChannelId());
		return;
	}
	// next to a downed teammate whom it could not revive for long: the host's
	// game had it a few metres off, behind something not there in its own game
	if (op == 4 && !pMine)
	{
		IActor* pDowned = ActorByName(name);
		if (pDowned && pDowned->GetHealth() <= 0)
		{
			const Vec3 body = pDowned->GetEntity()->GetWorldPos();
			Vec3 from = pAgent->GetEntity()->GetWorldPos() - body;
			from.z = 0;
			const Vec3 at = body + from.GetNormalizedSafe(Vec3(1, 0, 0)) * 1.2f + Vec3(0, 0, 0.5f);
			const Vec3 look = body - at;
			pRules->MovePlayer(static_cast<CActor*>(pAgent), at, Ang3(0, 0, atan2f(-look.x, look.y)));
			CryLogAlways("[CoopAgent] %s put next to %s, who is down", pAgent->GetEntity()->GetName(), pDowned->GetEntity()->GetName());
		}
		return;
	}
	if (op == 1 && pLeader && !pMine && pLeader->GetHealth() > 0)
	{
		// next to the leader, a little behind
		const Matrix34& tm = pLeader->GetEntity()->GetWorldTM();
		const Vec3 back = -tm.GetColumn1().GetNormalizedSafe(Vec3(0, 1, 0));
		Vec3 at = tm.GetTranslation() + back * 2.0f + tm.GetColumn0().GetNormalizedSafe(Vec3(1, 0, 0)) * 1.0f;
		at.z = tm.GetTranslation().z + 0.5f;
		pRules->MovePlayer(static_cast<CActor*>(pAgent), at, pLeader->GetEntity()->GetWorldAngles());
		CryLogAlways("[CoopAgent] %s caught up with %s", pAgent->GetEntity()->GetName(), pLeader->GetEntity()->GetName());
	}
}

void CoopAgent::OnPath(int op, const char* goal, const char* text)
{
	if (!IsCompanion())
		return;
	if (op == 2)
	{
		// where a downed teammate lies in the host's game
		SHostBody b;
		if (goal && text && sscanf(text, "%f %f %f", &b.pos.x, &b.pos.y, &b.pos.z) == 3)
		{
			b.at = Now();
			s_hostBodies[goal] = b;
		}
		return;
	}
	std::vector<Vec3>& path = s_bot.path;
	path.clear();
	for (const char* p = text; p && *p; )
	{
		Vec3 v;
		if (sscanf(p, "%f %f %f", &v.x, &v.y, &v.z) == 3)
			path.push_back(v);
		p = strchr(p, ';');
		if (p)
			++p;
	}
	Vec3 g(ZERO);
	if (goal && sscanf(goal, "%f %f %f", &g.x, &g.y, &g.z) == 3)
		s_bot.pathGoal = g;
	s_bot.pathAt = Now();
	if (op == 1)
	{
		// cover: where it falls back to (none found: away, as it can)
		s_bot.haveCover = !path.empty();
		if (s_bot.haveCover)
		{
			s_bot.cover = g;
			s_bot.coverAt = Now();
		}
	}
	if (!path.empty())
		CoopAI::Trace("AGENT way: %d points to %s", (int)path.size(), goal ? goal : "");
}

bool CoopAgent::HostBodyPos(EntityId downed, Vec3& pos)
{
	IEntity* pEntity = IsCompanion() ? gEnv->pEntitySystem->GetEntity(downed) : 0;
	if (!pEntity)
		return false;
	std::map<string, SHostBody>::const_iterator it = s_hostBodies.find(pEntity->GetName());
	if (it == s_hostBodies.end() || Now() - it->second.at > 5.0f)
		return false;
	pos = it->second.pos;
	return true;
}

float CoopAgent::CompanionSeconds()
{
	// since it could join: its start, or the host's game being ready
	// or its last moment in the game (it drops and comes back after a
	// checkpoint load)
	if (!s_proc.hProcess)
		return 0.0f;
	return Now() - std::max(std::max(s_readySince, s_startedAt), s_lastInGame);
}

int CoopAgent::CompanionTries()
{
	return s_starts;
}

int CoopAgent::CompanionState()
{
	if (!s_proc.hProcess)
		return s_starts >= 5 ? 3           // 3: its game would not start (tries used up)
			: s_starts > 0 ? 5 : 0;        // 5: its game closed, started again soon
	if (s_readySince < 0.0f)
		return 4;                          // 4: it waits for the host's game to be ready
	IActorIteratorPtr it = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
	while (IActor* pActor = it->Next())
		if (pActor->IsPlayer() && !stricmp(pActor->GetEntity()->GetName(), s_pName->GetString()))
		{
			s_lastInGame = Now();
			return 2;
		}
	return 1;
}
