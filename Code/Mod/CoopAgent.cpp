// Crysis Coop: the AI companion (see CoopAgent.h).
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")

#include "StdAfx.h"
#include <StringUtils.h>
#include "CoopAgent.h"
#include "project.h"
#include "CoopAI.h"
#include "Game.h"
#include "GameActions.h"
#include "GameRules.h"
#include "CoopSave.h"
#include "CoopRevive.h"
#include "CoopProps.h"
#include "HUD/HUD.h"
#include "OffHand.h"
#include "Item.h"
#include "AmmoPickup.h"
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

	// the mounted gun (a ShiTen on the ground, a level's machine gun nest)
	// one stands behind; 0 for anyone else
	// (the gun he holds; or one right at him: in a joined game a soldier's
	// use of a level's mounted gun is the server's, his current item here
	// may say nothing of it. Looked up once a second per soldier)
	IEntity* MountedGunOf(IActor* pActor)
	{
		if (!pActor)
			return 0;
		IItem* pItem = pActor->GetCurrentItem();
		if (pItem && static_cast<CItem*>(pItem)->IsMounted())
			return pItem->GetEntity();
		struct SNear { EntityId gun; float at; };
		static std::map<EntityId, SNear> s_near;
		const float now = gEnv->pTimer->GetCurrTime();
		SNear& n = s_near[pActor->GetEntityId()];
		if (now - n.at > 1.0f || now < n.at)
		{
			n.at = now;
			n.gun = 0;
			const Vec3 at = pActor->GetEntity()->GetWorldPos();
			IPhysicalEntity** pList = 0;
			const int count = gEnv->pPhysicalWorld->GetEntitiesInBox(at - Vec3(1.6f, 1.6f, 0.5f), at + Vec3(1.6f, 1.6f, 2.2f), pList,
				ent_static | ent_rigid | ent_sleeping_rigid);
			IItemSystem* pItems = g_pGame->GetIGameFramework()->GetIItemSystem();
			for (int i = 0; i < count && !n.gun; ++i)
				if (IEntity* pEntity = gEnv->pEntitySystem->GetEntityFromPhysics(pList[i]))
					if (IItem* pNear = pItems->GetItem(pEntity->GetId()))
					{
						// (his: a gun on the ground, not a vehicle's (those have a
						// parent), not another one's, and he behind it, where its
						// user stands: CItem::CanUse's test)
						CItem* pGun = static_cast<CItem*>(pNear);
						if (!pGun->IsMounted() || pEntity->GetParent())
							continue;
						const EntityId owner = pGun->GetOwnerId();
						if (owner && owner != pActor->GetEntityId())
							continue;
						Vec3 back = at - pEntity->GetWorldPos();
						back.z = 0.0f;
						if (owner == pActor->GetEntityId() || (back.GetLength() < 1.4f && back.Dot(pGun->GetMountedDir()) < 0.0f))
							n.gun = pEntity->GetId();
					}
		}
		return n.gun ? gEnv->pEntitySystem->GetEntity(n.gun) : 0;
	}

	// the head: its bone when it has one inside its box (a soldier's box
	// reaches forward with his arms: the top of the box's middle column was
	// beside his face, the shots passed it), else the top of the box
	Vec3 HeadPoint(IEntity* pEntity)
	{
		AABB box;
		pEntity->GetWorldBounds(box);
		const Vec3 c = box.GetCenter();
		Vec3 head(c.x, c.y, box.max.z - 0.15f);
		ICharacterInstance* pChar = pEntity->GetCharacter(0);
		if (ISkeletonPose* pPose = pChar && !box.IsEmpty() ? pChar->GetISkeletonPose() : 0)
		{
			const int16 h = pPose->GetJointIDByName("Bip01 Head");
			if (h >= 0)
			{
				const Vec3 bone = pEntity->GetSlotWorldTM(0) * pPose->GetAbsJointByID(h).t;
				AABB grown = box;
				grown.Expand(Vec3(0.3f, 0.3f, 0.3f));
				if (grown.IsContainPoint(bone))
					head = bone;
			}
		}
		return head;
	}

	// where to aim at a body: a little above the middle of its box
	Vec3 AimPoint(IEntity* pEntity)
	{
		AABB box;
		pEntity->GetWorldBounds(box);
		Vec3 c = box.GetCenter();
		c.z += (box.max.z - box.min.z) * 0.18f;
		// one at a vehicle's gun: the head, over the side (at his chest the
		// shots went into the truck: 10 of 12 hits, its armor took them, no
		// soldier killed in 20 fights at the bridge); one behind a mounted gun
		// the same (the gun's shield and body stand before his chest)
		IActor* pActor = g_pGame ? g_pGame->GetIGameFramework()->GetIActorSystem()->GetActor(pEntity->GetId()) : 0;
		if (pActor && (pActor->GetLinkedVehicle() || MountedGunOf(pActor)) && !box.IsEmpty())
			return HeadPoint(pEntity);
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

	// what it holds in the left hand (no wall to its eyes: the review)
	IPhysicalEntity* s_heldPhys = 0;

	// what is in this player's left hand now: part of itself for the walking
	// checks (a 100 kg bin in its hands is in front of it)
	IPhysicalEntity* HeldPhysics(CPlayer* pMe)
	{
		COffHand* pOffHand = pMe ? static_cast<COffHand*>(pMe->GetWeaponByClass(CItem::sOffHandClass)) : 0;
		IEntity* pHeld = pOffHand && pOffHand->CoopHeldId() ? gEnv->pEntitySystem->GetEntity(pOffHand->CoopHeldId()) : 0;
		return pHeld ? pHeld->GetPhysics() : 0;
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
			if (hits[i].dist >= 0.0f && hits[i].pCollider && hits[i].pCollider != s_heldPhys && !(Through(hits[i].surface_idx) & mode) && (!found || hits[i].dist < block.dist))
			{
				block = hits[i];
				found = true;
			}
		return found;
	}

	bool ShotPoint(IActor* pFrom, const Vec3& eye, IActor* pTarget, Vec3& out, const Vec3* pMuzzle = 0);

	// a ray's hit is on this entity: its own physics, or another physical
	// entity of it (a soldier's skeleton, what bullets and rays meet, is not
	// his living entity: the line that met the gunner's head was taken for
	// something else, 2026-10-21)
	bool HitIs(const ray_hit& hit, IEntity* pEntity)
	{
		return hit.pCollider && pEntity && (hit.pCollider == pEntity->GetPhysics() || gEnv->pEntitySystem->GetEntityFromPhysics(hit.pCollider) == pEntity);
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
		if (HitIs(hit, pTarget))
			return true;
		// the target's vehicle counts as the target (a gunner behind his gun),
		// so does the mounted gun he stands behind (the ray to his chest ends
		// in its body; AimPoint sends the shots at his head)
		if (IActor* pActor = ActorOf(pTarget->GetId()))
		{
			if (IVehicle* pVehicle = pActor->GetLinkedVehicle())
				if (hit.pCollider && hit.pCollider == pVehicle->GetEntity()->GetPhysics())
					return true;
			// (only when a line reaches some part of him: one wholly behind
			// the gun was picked and never shot at, its fire held for good)
			if (IEntity* pGun = MountedGunOf(pActor))
				if (hit.pCollider && hit.pCollider == pGun->GetPhysics())
				{
					Vec3 reach;
					return ShotPoint(pFrom, eye, pActor, reach);
				}
		}
		return hit.dist > dir.GetLength() - 0.5f;
	}

	// a point of him a ray from the eye reaches first (him, not his gun's
	// shield or body): his head, neck, shoulders, chest, in that order. A
	// gunner behind a mounted gun: the ray to his head met the gun's body,
	// no shot came, he shot it down (the user's game of 2026-10-18)
	bool ShotPoint(IActor* pFrom, const Vec3& eye, IActor* pTarget, Vec3& out, const Vec3* pMuzzle)
	{
		IEntity* pEntity = pTarget->GetEntity();
		IPhysicalEntity* pTargetPhys = pEntity->GetPhysics();
		ICharacterInstance* pChar = pEntity->GetCharacter(0);
		ISkeletonPose* pPose = pChar ? pChar->GetISkeletonPose() : 0;
		if (!pPose || !pTargetPhys)
			return false;
		IPhysicalEntity* skip[2];
		int n = 0;
		if (IPhysicalEntity* p = pFrom->GetEntity()->GetPhysics())
			skip[n++] = p;
		if (IVehicle* pVehicle = pFrom->GetLinkedVehicle())
			if (IPhysicalEntity* p = pVehicle->GetEntity()->GetPhysics())
				skip[n++] = p;
		static const char* const bones[] = { "Bip01 Head", "Bip01 Neck", "Bip01 R Clavicle", "Bip01 L Clavicle", "Bip01 Spine3", "Bip01 R UpperArm", "Bip01 L UpperArm", "Bip01 Spine2" };
		const Matrix34& slot = pEntity->GetSlotWorldTM(0);
		for (int i = 0; i < sizeof(bones) / sizeof(bones[0]); ++i)
		{
			const int16 id = pPose->GetJointIDByName(bones[i]);
			if (id < 0)
				continue;
			const Vec3 at = slot * pPose->GetAbsJointByID(id).t;
			ray_hit hit;
			if (!FirstBlock(eye, (at - eye) * 1.05f, ent_all, skip, n, hit) || !HitIs(hit, pEntity))
				continue;
			// (and from where the bullet leaves: the gun a little lower than
			// the eyes saw his head over the mounted gun's body, its line ran
			// into the body)
			if (pMuzzle && (!FirstBlock(*pMuzzle, (at - *pMuzzle) * 1.05f, ent_all, skip, n, hit) || !HitIs(hit, pEntity)))
				continue;
			out = at;
			return true;
		}
		return false;
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
		string lootClass;        // what it is (gone from the ground once taken)
		float lootSince, useAt, useBlocked;
		int lootUses;            // use pressed at the one it goes for
		float lootNear;          // the closest its eyes came to it
		std::map<EntityId, float> lootFailed;   // given up on, and when (tried again a minute later)
		// a way around what is in the way (from the host's game: its AI
		// navigation), and where to
		std::vector<Vec3> path;
		Vec3 pathGoal;
		float pathAt, pathAskAt;
		float pathNoneAt;       // the host's game found no way there, then, to
		Vec3 pathNoneGoal;
		bool localFound;        // the way round close by gets there (not just nearer)
		bool doorMode;          // along the host's way point by point (its own squares find none)
		Vec3 doorFor;
		float arriveBlockedSince;   // next to the one it follows with something between, since
		float unreachSince, unreachBest, unreachCheckAt;   // the one it follows out of a straight walk since, the nearest it came, next look
		float jumpAt;           // when it last jumped up onto something on its way
		// a climb up onto something (ClimbStep): till then (0: none), toward,
		// from that height, so high, the jumps so far, its phase and since when
		float climbUntil, climbSince, climbFromZ, climbRise, climbPhaseAt, climbSeenAt, climbLookAt, climbGroundAt;
		Vec3 climbDir, climbStart;
		int climbJumps, climbPhase;
		bool climbStrength, climbWasAir, climbPressed;
		Vec3 climbPlace;        // where climbs failed lately, and how often
		float climbPlaceAt;
		int climbFails;
		float waitWayUntil;     // stands till then for the host's way (its own squares only got nearer)
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
		float spreadHeldAt;     // FightStep held a burst back for the spread then
		float smallFarAt;       // FightStep aimed at a small target far off then (a gunner behind his gun's shield)
		float coverAt;          // when the host's game named that cover
		float peekUntil;        // fighting from cover: up shooting / down
		bool peekUp;
		int coverPeek;          // how it fights from the cover: 0 standing up, 1 leaning left, 2 right, 3 no shot
		float peekSince;        // up (or down) since
		float peekSeenAt;       // its enemy last in sight while up
		int peekSide;           // the side it leans out to now (it changes sides)
		float openSince;        // fighting in the open since (-1: not)
		int peeks, peekHits, hiddenReloads;   // for the log, now and then
		int leanPeeks;          // of the peeks, leaning out round the cover's side
		int hitsConfirmed;      // its hits the server marked (the last minute)
		float leanHoldUntil;    // the agent's "lean": held till then, to that side
		int leanHoldSide;
		float zoomAt;           // the sights may go up or down again then
		bool zoomWant;          // FightStep wants the sights up this frame
		bool cloakEscape;       // falling back cloaked (out of their sight)
		float strengthJumpAt;   // a strength jump onto what is in the way: wanted since (-1: none)
		Vec3 strengthJumpDir;
		float heavyHitAt;       // hit by a heavy gun (a vehicle's, a minigun, a rocket) then
		float retreatNowAt;     // shot in cover by a heavy gun then: falls back (Survive, within 1 s)
		Vec3 ownNoneFor;        // its own squares found no way to there, then
		float ownNoneAt;
		Vec3 longWayFor;        // the host's long way round taken for there, then
		float longWayAt;
		Vec3 circleAt;          // where it has been going round in a few metres since, to the one it follows
		float circleSince, circleLast, circleBest;
		Vec3 sjPlace;           // where it tried strength jumps lately, and how often
		float sjPlaceAt;
		int sjTries;
		float meleeAt;          // a blow at one right next to it again then
		float meleeWantUntil;   // strength wanted for a blow till then
		int suitSwitches;
		float suitHoldUntil;    // the agent's own suit mode kept till then (0: SuitStep chooses)
		bool coverOpenAsk;      // the cover was asked for in the open, not shot at (one with no shot is no use then)
		Vec3 pathAskGoal;       // the last way asked for, to where
		Vec3 waitedFor;         // it stood for the host's way to there, then
		float waitedAt;
		float fineGridAt;       // the quarter-metre squares looked at last then
		float fightOpen, fightHidden, fightPeek, fightBack;   // seconds this minute
		// (the user's game of 2026-10-21: it ran 12-28 m off under fire with a
		// cover 4-11 m away, never cloaked, went back to a cover from before a
		// load) where a fall back started, how its cover hides it, since when
		// it is there, when its fight state was last renewed, lying down till
		// when, where it goes cloaked, when a teammate was seen go down
		Vec3 retreatFrom;
		bool coverProne;        // the cover hides it lying down only
		float coverReachedAt;   // in the cover since (-1: not there)
		float freshAt;          // its fight state renewed then (down, up, put elsewhere)
		float proneUntil;       // lying down till then
		Vec3 cloakGoal;         // moving cloaked to there (zero: the cover not known yet)
		float cloakSince;       // cloakEscape set then
		float cloakSeconds, proneSeconds;   // this minute (for the log)
		int cloaks;             // cloaked moves begun this minute
		bool cloakForRevive;    // the cloaked move goes to a downed teammate
		float coverBest, coverBestAt;   // the nearest it came to its cover, when
		float uncloakAt;        // out of the cloak then (no shot for a moment: the server may still have it cloaked)
		float tetherRadiusFor;  // the tether round the downed teammate it goes to (set once for him)
		float cloakBest, cloakBestAt;   // the nearest it came to the cloaked move's goal, when
		std::map<EntityId, float> downAt;   // a teammate seen go down then
		SBrain(): order(eO_Free), spot(ZERO), haveSpot(false), fireAtWill(true), enemy(0), burstUntil(0), pauseUntil(0), reloadAt(0),
			switchAt(0), attackHeld(false), progGoal(ZERO), progBest(1e9f), progAt(0), progSeen(-100), progPath(-100), stuck(0),
			slideDir(ZERO), slideUntil(0), localFor(ZERO), localAt(-100), localStage(0), localPlans(0), nextCheckFor(ZERO), toCheckFor(ZERO),
			nextCheckAt(0), toCheckAt(0), nextClear(true), toClear(true), wayCheckAt(0), wayCheckPath(-100), farSince(-1), askAt(0), reportAt(0), useHeld(false), useFor(0), useSince(0), lastHealth(-1), wasDown(false), named(false),
			weaponAt(0), loot(0), lootSince(0), useAt(0), useBlocked(0), pathGoal(ZERO), pathAt(-100), pathAskAt(0), pathNoneAt(-100), pathNoneGoal(ZERO),
			localFound(false), doorMode(false), doorFor(ZERO), arriveBlockedSince(-1), unreachSince(-1), unreachBest(0), unreachCheckAt(0), jumpAt(-100), climbUntil(0), climbSince(0), climbFromZ(0), climbRise(0), climbPhaseAt(0), climbSeenAt(0), climbLookAt(0), climbGroundAt(-1), climbDir(ZERO), climbStart(ZERO), climbJumps(0),
			climbPhase(0), climbStrength(false), climbWasAir(false), climbPressed(false), climbPlace(ZERO), climbPlaceAt(-100), climbFails(0), waitWayUntil(0), reviveSince(-1), reachFailSince(-1), bodyToldFor(0),
			dodgeUntil(0), dodgeSide(1), retreating(false), hitAt(-100), lastFought(0), bursts(0), heldFire(0), stuckTimes(0), unseen(0), turning(0), statsAt(0), dropAt(0), downedFor(0), downedSince(0), enemySeenAt(-100),
			cover(ZERO), haveCover(false), coverAskAt(0), retreatSince(-100), recoveredAt(-100), spreadHeldAt(-100), smallFarAt(-100),
			coverAt(-100), peekUntil(0), peekUp(true), coverPeek(0), peekSince(0), peekSeenAt(-100), peekSide(1), openSince(-1), peeks(0), leanPeeks(0), hitsConfirmed(0), leanHoldUntil(0), leanHoldSide(0),
			peekHits(0), hiddenReloads(0), zoomAt(0), zoomWant(false), cloakEscape(false), strengthJumpAt(-1), strengthJumpDir(ZERO), sjPlace(ZERO), sjPlaceAt(-100), sjTries(0), heavyHitAt(-100), retreatNowAt(-100), ownNoneFor(ZERO), ownNoneAt(-100), longWayFor(ZERO), longWayAt(-100), circleAt(ZERO), circleSince(-1), circleLast(-100), circleBest(0), meleeAt(0), meleeWantUntil(0),
			suitSwitches(0), suitHoldUntil(0), coverOpenAsk(false), pathAskGoal(ZERO), waitedFor(ZERO), waitedAt(-100), fineGridAt(-100), fightOpen(0), fightHidden(0), fightPeek(0), fightBack(0),
			retreatFrom(ZERO), coverProne(false), coverReachedAt(-1), freshAt(-100), proneUntil(0), cloakGoal(ZERO), cloakSince(-100),
			cloakSeconds(0), proneSeconds(0), cloaks(0), cloakForRevive(false), coverBest(1e9f), coverBestAt(-100), uncloakAt(-100), tetherRadiusFor(12), cloakBest(1e9f), cloakBestAt(-100) {}
	} s_bot;

	// where the downed teammates lie in the host's game, and when it said so
	struct SHostBody
	{
		Vec3 pos;
		float at;
	};
	std::map<string, SHostBody> s_hostBodies;

	// who shot it, lately: when first and last, how often, from where (the
	// server's damage indicator; a hit its suit's armor took left its health
	// as it was, and only a third to two thirds of the hits were noticed)
	struct SThreat
	{
		float first, last;
		int hits;
		Vec3 pos;
		SThreat(): first(0), last(-100), hits(0), pos(ZERO) {}
	};
	std::map<EntityId, SThreat> s_threats;
	std::vector<float> s_hitTimes;          // when it was hit lately

	int HitsWithin(float seconds)
	{
		int n = 0;
		for (size_t i = 0; i < s_hitTimes.size(); ++i)
			if (Now() - s_hitTimes[i] < seconds)
				++n;
		return n;
	}

	// what it was doing in a fight is over: it went down or got up, or it is
	// somewhere else at once (a checkpoint load, the host's game moved it).
	// The cover, the way, the fall back and who shot it were of another place
	// and moment: after a load it walked 35 m back to the cover it had died
	// by, the fall back went on after its death (the user's game of
	// 2026-10-21)
	void ResetFight(const char* why)
	{
		const bool had = s_bot.retreating || s_bot.haveCover || !s_threats.empty() || s_bot.cloakEscape;
		s_bot.retreating = false;
		s_bot.retreatNowAt = -100.0f;
		s_bot.cloakEscape = false;
		s_bot.haveCover = false;
		s_bot.coverAskAt = 0.0f;
		s_bot.coverReachedAt = -1.0f;
		s_bot.coverProne = false;
		s_bot.path.clear();
		s_bot.pathAt = -100.0f;
		s_bot.loot = 0;
		s_bot.openSince = -1.0f;
		s_bot.peekUp = false;
		s_bot.heavyHitAt = -100.0f;
		s_bot.recoveredAt = -100.0f;
		s_bot.hitAt = -100.0f;
		s_bot.proneUntil = 0.0f;
		s_bot.enemy = 0;
		s_bot.cloakForRevive = false;
		s_bot.freshAt = Now();
		s_threats.clear();
		s_hitTimes.clear();
		if (had)
			CryLogAlways("[CoopAgent] the fight so far is over (%s): no cover, fall back or shooters kept", why);
	}

	// the one who shot it last within that long (0: nobody)
	EntityId LastShooter(float within)
	{
		EntityId best = 0;
		float at = Now() - within;
		for (std::map<EntityId, SThreat>::const_iterator it = s_threats.begin(); it != s_threats.end(); ++it)
			if (it->second.last >= at)
			{
				at = it->second.last;
				best = it->first;
			}
		return best;
	}

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
		int lean;               // -1 left, 1 right (out round a corner of its cover)
		bool prone;             // lying down (crouch wins over it)
		SControl(): moveDir(ZERO), look(false), yaw(0), pitch(0), turnRate(6.0f), sprint(false), jump(false), crouch(false), lean(0), prone(false) {}
	} s_ctl;

	void Press(CPlayer* pPlayer, const ActionId& action, bool press)
	{
		if (IPlayerInput* pInput = pPlayer->GetPlayerInput())
			pInput->OnAction(action, press ? eAAM_OnPress : eAAM_OnRelease, press ? 1.0f : 0.0f);
	}

	void Fire(CPlayer* pPlayer, bool on)
	{
		// not with something in both hands, nor while picking up: the fire
		// key would throw it (one in the left hand leaves the rifle free, as
		// in single player)
		if (on)
			if (COffHand* pOffHand = static_cast<COffHand*>(pPlayer->GetWeaponByClass(CItem::sOffHandClass)))
			{
				const int state = pOffHand->GetOffHandState();
				if ((state & (eOHS_HOLDING_NPC | eOHS_PICKING | eOHS_PICKING_ITEM | eOHS_PICKING_ITEM2 | eOHS_THROWING_OBJECT))
					|| ((state & eOHS_HOLDING_OBJECT) && pOffHand->GetGrabType() != 0))
					on = false;
			}
		if (on != s_bot.attackHeld)
		{
			Press(pPlayer, g_pGame->Actions().attack1, on);
			s_bot.attackHeld = on;
		}
	}

	void HoldUse(CPlayer* pPlayer, bool on, EntityId forWhom)
	{
		// (a thing in the left hand: the use key would throw it - at the body
		// it revives; put down first, the key once the hand is free)
		// (only a thing in the hand: in any other state - a grenade just
		// thrown - the revive takes the key as before)
		if (on && !s_bot.useHeld)
			if (COffHand* pOffHand = static_cast<COffHand*>(pPlayer->GetWeaponByClass(CItem::sOffHandClass)))
				if (pOffHand->CoopHeldId())
				{
					if ((pOffHand->GetOffHandState() & eOHS_HOLDING_OBJECT) && Now() >= s_bot.dropAt)
					{
						Press(pPlayer, g_pGame->Actions().zoom, true);
						Press(pPlayer, g_pGame->Actions().zoom, false);
						s_bot.dropAt = Now() + 1.5f;
						Event("puts down what it holds: its hand for the use key");
					}
					return;
				}
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
	// enemies the gun cannot reach for now (seen, but its line of fire runs
	// into a wall, a rock): left alone a while, another one is taken
	std::map<EntityId, float> s_noLineUntil;       // from a vehicle's gun
	std::map<EntityId, float> s_footNoLineUntil;   // on foot (a wall between them for the gun is none on foot)
	EntityId s_footBlockedEnemy = 0;     // on foot: its shots at him blocked, since
	float s_footBlockedSince = -1.0f;
	float s_footBlockedLastAt = -100.0f;

	// its hits on each enemy (the server's hit marker, put on the one it
	// fought then), forgotten 15 s after the last
	struct SMyHits { int n; float last; };
	std::map<EntityId, SMyHits> s_myHits;
	int MyHitsOn(EntityId id, float within)
	{
		std::map<EntityId, SMyHits>::const_iterator it = s_myHits.find(id);
		return it != s_myHits.end() && Now() - it->second.last < within ? it->second.n : 0;
	}

	// in its cover: in sight from where it peeks (standing up, leaning out
	// to either side), one who shot it in the last 20 s
	bool SeenFromPeek(CPlayer* pMe, const Vec3& eye, IActor* pActor, const Vec3& aim)
	{
		if (!s_bot.haveCover || Dist2D(s_bot.cover, pMe->GetEntity()->GetWorldPos()) > 1.5f)
			return false;
		std::map<EntityId, SThreat>::const_iterator threat = s_threats.find(pActor->GetEntityId());
		if (threat == s_threats.end() || Now() - threat->second.last > 20.0f)
			return false;
		// (from where it will be: standing up, from its stance's eye height,
		// not this frame's input, cleared at the top of each think; leaning
		// only the way its cover's peek goes, the way FightFromCover leans)
		const SStanceInfo* pStand = pMe->GetStanceInfo(STANCE_STAND);
		const float standZ = pMe->GetEntity()->GetWorldPos().z + (pStand && pStand->viewOffset.z > 1.0f ? pStand->viewOffset.z : 1.6f);
		const Vec3 upEye(eye.x, eye.y, max(eye.z + 0.15f, standZ));
		const Vec3 right = pMe->GetViewRotation().GetColumn0().GetNormalizedSafe(Vec3(1, 0, 0));
		if (s_bot.coverPeek == 1)
			return Visible(pMe, upEye - right * 0.6f, pActor->GetEntity(), aim);
		if (s_bot.coverPeek == 2)
			return Visible(pMe, upEye + right * 0.6f, pActor->GetEntity(), aim);
		return Visible(pMe, upEye, pActor->GetEntity(), aim);
	}

	EntityId PickEnemy(CPlayer* pMe, const Vec3& eye, float range, float shooterRange = 150.0f)
	{
		if (s_bot.order == eO_Attack && !s_bot.target.empty())
		{
			IActor* pTarget = ActorByName(s_bot.target.c_str());
			if (!pTarget || pTarget->GetHealth() <= 0)
				return 0;
			// (seen: a threat for the suit too)
			if (Visible(pMe, eye, pTarget->GetEntity(), AimPoint(pTarget->GetEntity())))
				s_bot.enemySeenAt = Now();
			return pTarget->GetEntityId();
		}
		// the one it fights stays its target while it is alive and was seen
		// a moment ago (a branch passing between them is no reason to turn
		// to another one and back)
		std::map<EntityId, float>& noLines = pMe->GetLinkedVehicle() ? s_noLineUntil : s_footNoLineUntil;
		std::map<EntityId, float>::iterator noLine = noLines.find(s_bot.enemy);
		const bool currentNoLine = noLine != noLines.end() && Now() < noLine->second;
		if (IActor* pCurrent = s_bot.enemy && !currentNoLine ? ActorOf(s_bot.enemy) : 0)
		{
			const Vec3 aim = AimPoint(pCurrent->GetEntity());
			std::map<EntityId, SThreat>::const_iterator curThreat = s_threats.find(s_bot.enemy);
			const bool curShotMe = curThreat != s_threats.end() && Now() - curThreat->second.last < 10.0f;
			if (Hostile(pCurrent) && aim.GetDistance(eye) < (curShotMe ? max(range, shooterRange) : range))
			{
				if (Visible(pMe, eye, pCurrent->GetEntity(), aim) || SeenFromPeek(pMe, eye, pCurrent, aim))
					s_bot.enemySeenAt = Now();
				const EntityId shooter = LastShooter(1.5f);
				IActor* pShooter = shooter && shooter != s_bot.enemy ? ActorOf(shooter) : 0;
				const bool shooterSeen = pShooter && Hostile(pShooter) && Visible(pMe, eye, pShooter->GetEntity(), AimPoint(pShooter->GetEntity()));
				// (one it has hit twice lately: finished first, in sight, whoever
				// else shoots; it went to each new shooter in turn, the hits spread
				// over all of them, 1 kill in 20 fights. Its hits: the server's hit
				// marker, its health here is only ever 100 or 0)
				const bool wounded = MyHitsOn(s_bot.enemy, 10.0f) >= 2;
				if (Now() - s_bot.enemySeenAt < 2.5f && (!shooterSeen || (wounded && Now() - s_bot.enemySeenAt < 0.5f)))
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
			std::map<EntityId, float>::const_iterator skipIt = noLines.find(pActor->GetEntityId());
			if (skipIt != noLines.end() && Now() < skipIt->second)
				continue;
			const Vec3 aim = AimPoint(pActor->GetEntity());
			const float d = aim.GetDistance(eye);
			// the one it fights already a little preferred; one who shot it a
			// moment ago much more (it shot at the nearest while another one
			// shot it down), and up to 150 m (a mounted gun's nest fires from
			// beyond the 80 m it looks on foot)
			std::map<EntityId, SThreat>::const_iterator threat = s_threats.find(pActor->GetEntityId());
			const bool shotMe = threat != s_threats.end() && Now() - threat->second.last < 4.0f;
			if (d > (shotMe ? max(range, shooterRange) : range))
				continue;
			if (!Visible(pMe, eye, pActor->GetEntity(), aim) && !SeenFromPeek(pMe, eye, pActor, aim))
			{
				// testing (coop_test_shottrace 1): why one in reach is not
				// taken, every 3 s per enemy (what the view meets, what he holds)
				static ICVar* pPickTrace = gEnv->pConsole->GetCVar("coop_test_shottrace");
				static std::map<EntityId, float> s_pickToldAt;
				if (pPickTrace && pPickTrace->GetIVal() && Now() - s_pickToldAt[pActor->GetEntityId()] > 3.0f)
				{
					s_pickToldAt[pActor->GetEntityId()] = Now();
					IPhysicalEntity* skip[2] = { pMe->GetEntity()->GetPhysics(), 0 };
					int ns = 1;
					if (IVehicle* pOwn = pMe->GetLinkedVehicle())
						if ((skip[1] = pOwn->GetEntity()->GetPhysics()) != 0)
							ns = 2;
					ray_hit hit;
					const bool blocked = FirstBlock(eye, aim - eye, ent_all, skip, ns, hit);
					IEntity* pHit = blocked && hit.pCollider ? gEnv->pEntitySystem->GetEntityFromPhysics(hit.pCollider) : 0;
					IItem* pHeld = pActor->GetCurrentItem();
					const bool headSeen = Visible(pMe, eye, pActor->GetEntity(), HeadPoint(pActor->GetEntity()));
					CryLogAlways("[CoopAgent] not picked %s (%.0f m): the view to him blocked at %.1f m by %s; he holds %s%s; his head in sight %d",
						pActor->GetEntity()->GetName(), d, blocked ? hit.dist : -1.0f,
						pHit ? pHit->GetName() : blocked ? (hit.pCollider && hit.pCollider->GetType() == PE_STATIC ? "something static" : "terrain") : "nothing",
						pHeld ? pHeld->GetEntity()->GetClass()->GetName() : "nothing", pHeld && static_cast<CItem*>(pHeld)->IsMounted() ? " (mounted)" : "", (int)headSeen);
				}
				continue;
			}
			// (one at a vehicle's gun, behind its shield and the cab, after
			// the others on foot it can hit; one it hit lately first: 4 m nearer
			// as it were a hit, up to 16, the hits on one, not spread over all)
			const float hurt = (float)min(MyHitsOn(pActor->GetEntityId(), 15.0f), 4);
			const float score = d - (pActor->GetEntityId() == s_bot.enemy ? 8.0f : 0.0f) - (shotMe ? 25.0f : 0.0f) + (pActor->GetLinkedVehicle() ? 20.0f : 0.0f) - hurt * 4.0f;
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
		IPhysicalEntity* skip[3];
		int n = 0;
		if (IPhysicalEntity* p = pMe->GetEntity()->GetPhysics())
			skip[n++] = p;
		if (IPhysicalEntity* p = pBody->GetPhysics())
			skip[n++] = p;
		if (IPhysicalEntity* p = HeldPhysics(pMe))
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
	// the terrain on the way is no slope too steep to walk up: the balls and
	// rays of a walk do not see it. A river bank of ~50 degrees by the bridge
	// looked a straight walk; the host's way round it was dropped for it, its
	// squares went straight up and it jumped at the bank (the user's game of
	// 2026-10-23). Steep for a metre and a half on end (~40 degrees: 1.25 m
	// up), not a hump: half-metre steps of 40 degrees on the camp's rough
	// ground made its ways under fire "blocked", it went the long way and
	// went down (1 fight of 5 won, 3 of 3 before). Down is walked; where
	// something stands on the slope (stairs, a deck) that is the ground
	const float TERRAIN_RISE = 1.25f;	// up over TERRAIN_RUN metres
	const float TERRAIN_RUN = 1.5f;
	bool TerrainWalkable(const Vec3& from, const Vec3& to)
	{
		I3DEngine* p3D = gEnv->p3DEngine;
		if (!p3D)
			return true;
		// (only up to a place that much higher: on the level - a body lying
		// by the camp's rocks, 2 m off - a hump between is walked round or
		// over; "blocked" there it went to and fro and never revived him)
		if (to.z - from.z < 1.0f)
			return true;
		const float z0 = p3D->GetTerrainElevation(from.x, from.y);
		// (not on the terrain there: a floor, a deck)
		if (fabsf(from.z - z0) > 0.6f)
			return true;
		Vec3 dir = to - from;
		dir.z = 0.0f;
		const float len = dir.GetLength();
		if (len < TERRAIN_RUN)
			return true;
		dir /= len;
		const int back = (int)(TERRAIN_RUN / 0.5f);
		float zs[64];
		const int steps = std::min(63, (int)(len / 0.5f));
		zs[0] = z0;
		for (int k = 1; k <= steps; ++k)
		{
			const Vec3 p = from + dir * (0.5f * (float)k);
			zs[k] = p3D->GetTerrainElevation(p.x, p.y);
			if (k >= back && zs[k] - zs[k - back] > TERRAIN_RISE)
			{
				ray_hit hit;
				if (!gEnv->pPhysicalWorld->RayWorldIntersection(Vec3(p.x, p.y, zs[k] + 2.2f), Vec3(0, 0, -2.1f), ent_static, WALK_GEOM, &hit, 1))
					return false;
			}
		}
		return true;
	}

	bool ClearWay(CPlayer* pMe, const Vec3& from, const Vec3& to, float radius = 0.3f)
	{
		const Vec3 dir = to - from;
		if (dir.x * dir.x + dir.y * dir.y < 0.04f)
			return true;
		if (!TerrainWalkable(from, to))
			return false;
		// (itself, and what it carries)
		IPhysicalEntity* skip[2];
		int nSkip = 0;
		if (IPhysicalEntity* p = pMe ? pMe->GetEntity()->GetPhysics() : 0)
			skip[nSkip++] = p;
		if (IPhysicalEntity* p = HeldPhysics(pMe))
			skip[nSkip++] = p;
		static const float s_heights[] = { 0.65f, 1.15f, 1.65f };
		for (int i = 0; i < 3; ++i)
		{
			primitives::sphere ball;
			ball.center = from + Vec3(0, 0, s_heights[i]);
			ball.r = radius;
			geom_contact* pContact = 0;
			const float hit = gEnv->pPhysicalWorld->PrimitiveWorldIntersection(ball.type, &ball, dir, ent_static, &pContact, 0,
				WALK_GEOM, 0, 0, 0, skip, nSkip);
			if (hit > 0.0f && pContact)
				return false;
		}
		Vec3 side(dir.y, -dir.x, 0);
		side.NormalizeSafe(Vec3(1, 0, 0));
		for (int i = 0; i < 6; ++i)
		{
			const Vec3 at = from + Vec3(0, 0, i < 3 ? 0.65f : 1.25f) + side * (radius * (float)(i % 3 - 1));
			ray_hit hit;
			if (gEnv->pPhysicalWorld->RayWorldIntersection(at, dir, ent_rigid | ent_sleeping_rigid, WALK_GEOM, &hit, 1, skip, nSkip)
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
				// (the probe's end on the ground there: a stair is no wall)
				Vec3 end = pos + d * 1.8f;
				ray_hit ground;
				IPhysicalEntity* pSelf = pMe->GetEntity()->GetPhysics();
				if (gEnv->pPhysicalWorld->RayWorldIntersection(end + Vec3(0, 0, 1.5f), Vec3(0, 0, -3.0f), ent_static | ent_terrain, WALK_GEOM, &ground, 1,
					&pSelf, pSelf ? 1 : 0) && ground.pt.z - pos.z < 0.6f)
					end.z = std::max(pos.z, ground.pt.z);
				if (ClearWay(pMe, pos, end))
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
		float cell;                        // the squares' size (0.5 m; 0.25 m to look again close by)
		int n;                             // squares a side
		std::vector<signed char> state;    // -1 not looked at yet, 0 the body does not fit, 1 it does
		std::vector<float> ground;         // the ground's height
		std::vector<unsigned char> cost;   // 1, more where a loose thing is pushed aside
		std::vector<AABB> light;           // loose things pushed aside (walked round if it can)
		std::vector<IPhysicalEntity*> skip;   // not in the way: itself, those loose things, bodies lying there
		std::vector<unsigned char> why;    // why the body does not fit (EGridWhy)
		SGrid(): x0(0), y0(0), z(0), cell(GRID_CELL), n(0) {}
	} s_grid;
	// why a square is not walked on (for the log: what keeps it in)
	enum EGridWhy { eGW_None, eGW_NoGround, eGW_Knee, eGW_Waist, eGW_Chest, eGW_Step };

	// a step up or down from one square to the next the body takes (a stair
	// is no wall: 0.5 m on half-metre squares)
	float GridStep(bool diag)
	{
		const float step = 0.25f + 0.5f * s_grid.cell;
		return diag ? step * 1.4f : step;
	}

	// from one square to the next (both looked at): the step is one the body
	// takes (its own tolerance for a straight step: straightened ways had the
	// diagonal's for every pair, 54 degrees, and a river bank it could not
	// walk up was a way, 2026-10-23)
	bool GridStepOk(int from, int to, bool diag)
	{
		const SGrid& g = s_grid;
		return fabsf(g.ground[to] - g.ground[from]) <= GridStep(diag);
	}

	// the squares around a place (it stands in the middle of one)
	void GridBegin(CPlayer* pMe, const Vec3& center, float radius, float cell = GRID_CELL)
	{
		SGrid& g = s_grid;
		g.cell = cell;
		g.n = 2 * std::max(2, (int)(radius / cell)) + 1;
		g.x0 = center.x - g.n * cell * 0.5f;
		g.y0 = center.y - g.n * cell * 0.5f;
		g.z = center.z;
		g.state.assign(g.n * g.n, -1);
		g.ground.assign(g.n * g.n, center.z);
		g.cost.assign(g.n * g.n, 1);
		g.why.assign(g.n * g.n, eGW_None);
		g.light.clear();
		g.skip.clear();
		if (IPhysicalEntity* p = pMe ? pMe->GetEntity()->GetPhysics() : 0)
			g.skip.push_back(p);
		if (IPhysicalEntity* p = HeldPhysics(pMe))
			g.skip.push_back(p);
		IPhysicalEntity** list = 0;
		const float size = g.n * cell;
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
		const float x = g.x0 + (cx + 0.5f) * g.cell, y = g.y0 + (cy + 0.5f) * g.cell;
		const float top = std::max(g.z, gEnv->p3DEngine->GetTerrainElevation(x, y)) + 2.0f;
		IPhysicalEntity** pSkip = g.skip.empty() ? 0 : &g.skip[0];
		const int nSkip = (int)g.skip.size();
		ray_hit hit;
		if (!gEnv->pPhysicalWorld->RayWorldIntersection(Vec3(x, y, top), Vec3(0, 0, -8.0f), ent_static | ent_terrain, WALK_GEOM, &hit, 1,
			pSkip, nSkip))
		{
			g.why[i] = eGW_NoGround;
			return false;
		}
		g.ground[i] = hit.pt.z;
		for (int k = 0; k < 3; ++k)
		{
			primitives::sphere ball;
			ball.center = Vec3(x, y, hit.pt.z + 0.65f + 0.5f * k);
			ball.r = 0.3f;
			if (gEnv->pPhysicalWorld->PrimitiveWorldIntersection(ball.type, &ball, Vec3(ZERO), ent_static | ent_rigid | ent_sleeping_rigid, 0, 0,
				WALK_GEOM, 0, 0, 0, pSkip, nSkip) > 0.0f)
			{
				g.why[i] = (unsigned char)(eGW_Knee + k);
				return false;
			}
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
		const int cx = (int)floorf((p.x - g.x0) / g.cell), cy = (int)floorf((p.y - g.y0) / g.cell);
		return cx < 0 || cy < 0 || cx >= g.n || cy >= g.n ? -1 : cy * g.n + cx;
	}

	// a straight walk over the squares between two places: each of them
	// free, no step up or down too high
	bool GridLine(const Vec3& a, const Vec3& b)
	{
		SGrid& g = s_grid;
		const int steps = std::max(1, (int)(Dist2D(a, b) / (g.cell * 0.5f)));
		int prev = -1;
		for (int s = 0; s <= steps; ++s)
		{
			const int i = GridIndex(a + (b - a) * ((float)s / (float)steps));
			if (i < 0 || !GridFree(i % g.n, i / g.n))
				return false;
			if (prev >= 0 && prev != i && !GridStepOk(prev, i, prev % g.n != i % g.n && prev / g.n != i / g.n))
				return false;
			prev = i;
		}
		return true;
	}

	// a ball the width of the body swept between two places at the knees,
	// the waist and the chest against what does not move or is heavy (a
	// door at an angle to the squares: both squares beside a diagonal step
	// through it touch its frame, the step itself does not)
	bool GridSweep(const Vec3& a, const Vec3& b, float radius)
	{
		SGrid& g = s_grid;
		IPhysicalEntity** pSkip = g.skip.empty() ? 0 : &g.skip[0];
		const int nSkip = (int)g.skip.size();
		const float base = std::max(a.z, b.z);
		Vec3 dir = b - a;
		dir.z = 0;
		for (int k = 0; k < 3; ++k)
		{
			primitives::sphere ball;
			ball.center = Vec3(a.x, a.y, base + 0.65f + 0.5f * k);
			ball.r = radius;
			geom_contact* pContact = 0;
			if (gEnv->pPhysicalWorld->PrimitiveWorldIntersection(ball.type, &ball, dir, ent_static | ent_rigid | ent_sleeping_rigid, &pContact, 0,
				WALK_GEOM, 0, 0, 0, pSkip, nSkip) > 0.0f && pContact)
				return false;
		}
		return true;
	}

	// what keeps it in, round where it stands (for the log): the squares
	// next to it it does not fit on, and why
	string GridWhyAround(const Vec3& pos)
	{
		const SGrid& g = s_grid;
		const int i = GridIndex(pos);
		if (i < 0)
			return "";
		int counts[6] = { 0, 0, 0, 0, 0, 0 };
		const int cx = i % g.n, cy = i / g.n;
		for (int dy = -1; dy <= 1; ++dy)
			for (int dx = -1; dx <= 1; ++dx)
			{
				const int nx = cx + dx, ny = cy + dy;
				if ((dx || dy) && nx >= 0 && ny >= 0 && nx < g.n && ny < g.n && g.state[ny * g.n + nx] == 0)
					++counts[g.why[ny * g.n + nx]];
			}
		static const char* const s_names[] = { "", "no ground", "at the knees", "at the waist", "at the chest", "a step" };
		string out;
		for (int k = 1; k < 6; ++k)
			if (counts[k])
				out += string().Format("%s%d %s", out.empty() ? "" : ", ", counts[k], s_names[k]);
		return out;
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
		const float cell = g.cell;
		const float edge = cell * 0.5f;
		const Vec3 goal(clamp_tpl(to.x, g.x0 + edge, g.x0 + n * cell - edge), clamp_tpl(to.y, g.y0 + edge, g.y0 + n * cell - edge), to.z);
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
		#define COOP_GRID_H(i) (cell * (float)(std::max(abs((i) % n - tx), abs((i) / n - ty)) + 0.414f * std::min(abs((i) % n - tx), abs((i) / n - ty))))
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
				if (!GridFree(nx, ny))
					continue;
				const int ni = ny * n + nx;
				if (closed[ni] || !GridStepOk(cur.i, ni, diag))
					continue;
				// (a diagonal step past a corner: both squares beside it free,
				// or the body's sweep through it free: a door at an angle)
				if (diag && (!GridFree(nx, cy) || !GridFree(cx, ny))
					&& !GridSweep(Vec3(g.x0 + (cx + 0.5f) * cell, g.y0 + (cy + 0.5f) * cell, g.ground[cur.i]),
						Vec3(g.x0 + (nx + 0.5f) * cell, g.y0 + (ny + 0.5f) * cell, g.ground[ni]), 0.3f))
					continue;
				const float c = cost[cur.i] + (diag ? 1.414f : 1.0f) * cell * (float)g.cost[ni];
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
			cells.push_back(Vec3(g.x0 + (i % n + 0.5f) * cell, g.y0 + (i / n + 0.5f) * cell, g.ground[i]));
		std::reverse(cells.begin(), cells.end());
		// the corners: from each to the farthest one in a straight walk
		Vec3 at = from;
		for (size_t i = 1; i < cells.size(); )
		{
			size_t reach = i;
			for (size_t j = cells.size() - 1; j > i; --j)
				if (GridLine(at, cells[j]) && GridSweep(at, cells[j], 0.36f))
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
		s_bot.pathAskGoal = goal;
		string where;
		where.Format("%.1f %.1f %.1f", goal.x, goal.y, goal.z);
		if (CGameRules* pRules = g_pGame->GetGameRules())
			pRules->CoopSendSyncToServer(15, 5, 0, where.c_str(), "", 0, 0.0f);
	}

	// where a downed teammate lies that it fights for: no cover farther from
	// him than that, no fall back away from him (it fell back 22-26 m from
	// the host it was about to revive and died, everybody down; the user's
	// game of 2026-10-21). Set by the revive each frame
	Vec3 s_tether(ZERO);
	float s_tetherRadius = 0.0f;

	void RecentShooters(float within, std::vector<IActor*>& out);

	// asks the host's game for cover from an enemy: a place close by out of
	// its sight that it can walk to (the answer comes as OnPath, op 1)
	// (targetFirst: a place with a shot at him above all, the host's search
	// takes its peek at the first one named)
	void AskCover(IActor* pThreat, bool open = false, bool targetFirst = false)
	{
		if (Now() < s_bot.coverAskAt || !pThreat)
			return;
		s_bot.coverAskAt = Now() + 4.0f;
		s_bot.coverOpenAsk = open;
		// "A,B|C,D": the cover must hide it from A and B - those who hit it
		// in the last 3 s, the newest first (or the target first: a place
		// with a shot at him) - C and D only count: who hit it before, the
		// one it fights. It hid from one while the one it fought hit it 7
		// times from 25 m, the user's game of 2026-10-21. "@x,y,z,r": none
		// farther from a downed teammate it fights for
		std::vector<IActor*> recent, older, hard, soft;
		RecentShooters(3.0f, recent);
		RecentShooters(6.0f, older);
		if (targetFirst)
			hard.push_back(pThreat);
		for (size_t i = 0; i < recent.size() && hard.size() < 3; ++i)
			if (std::find(hard.begin(), hard.end(), recent[i]) == hard.end())
				hard.push_back(recent[i]);
		for (size_t i = 0; i < older.size() && soft.size() < 2; ++i)
			if (std::find(hard.begin(), hard.end(), older[i]) == hard.end())
				soft.push_back(older[i]);
		if (std::find(hard.begin(), hard.end(), pThreat) == hard.end() && std::find(soft.begin(), soft.end(), pThreat) == soft.end())
			(hard.empty() ? hard : soft).push_back(pThreat);
		string names;
		for (size_t i = 0; i < hard.size(); ++i)
			names += (i ? "," : "") + string(hard[i]->GetEntity()->GetName());
		names += "|";
		for (size_t i = 0; i < soft.size(); ++i)
			names += (i ? "," : "") + string(soft[i]->GetEntity()->GetName());
		if (s_tetherRadius > 0.0f)
			names += string().Format("@%.1f,%.1f,%.1f,%.1f", s_tether.x, s_tether.y, s_tether.z, s_tetherRadius);
		if (CGameRules* pRules = g_pGame->GetGameRules())
			pRules->CoopSendSyncToServer(15, 6, 0, names.c_str(), "", 0, 0.0f);
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
		bool fresh = !path.empty() && Now() - s_bot.pathAt < 12.0f && Dist2D(s_bot.pathGoal, to) < 4.0f;
		// a way many times the straight line, close by: not followed (from
		// behind a hut's wall the host's navigation sent it 800 m round for
		// the 15 m to the door; it ran 60 m off before it turned back)
		if (fresh && dist > 10.0f && dist < 40.0f)
		{
			const float way = WayLeft(pos, to);
			// (once for a goal: its own squares found none since, the long way
			// is the way; else a goto to a walled place never got there)
			static float s_rejectedAt = -100.0f;
			static Vec3 s_rejectedFor(ZERO);
			// (taken once its own squares found no way to this goal since the
			// rejection, and kept for it: a plan toward one of the host's
			// points found and the way was dropped again at the first post.
			// A plan of its own that finds the goal ends that, MoveTo. The
			// plan in the same frame as the rejection counts: 0.5 s slack)
			const bool latched = Dist2D(s_bot.longWayFor, to) < 4.0f && Now() - s_bot.longWayAt < 120.0f;
			const bool triedOwn = latched || (Dist2D(s_rejectedFor, to) < 4.0f && Dist2D(s_bot.ownNoneFor, to) < 4.0f
				&& s_bot.ownNoneAt > s_rejectedAt - 0.5f && Now() - s_bot.ownNoneAt < 30.0f);
			if (way > 4.0f * dist + 60.0f && triedOwn)
			{
				// (moved along with the goal: the one it follows walks on)
				if (!latched)
					s_bot.longWayAt = Now();
				s_bot.longWayFor = to;
				static float s_takenToldAt = -100.0f;
				if (Now() - s_takenToldAt > 20.0f)
				{
					Event("the host's way there is %.0f m for %.0f m straight, its own squares found none: the long way", way, dist);
					s_takenToldAt = Now();
				}
			}
			else if (way > 4.0f * dist + 60.0f)
			{
				if (!(Dist2D(s_rejectedFor, to) < 4.0f && Now() - s_rejectedAt < 30.0f))
				{
					s_rejectedAt = Now();
					s_rejectedFor = to;
				}
				static float s_toldAt = -100.0f;
				static Vec3 s_toldFor(ZERO);
				if (Now() - s_toldAt > 20.0f || Dist2D(s_toldFor, to) > 4.0f)
				{
					Event("the host's way there is %.0f m for %.0f m straight: its own way round instead", way, dist);
					s_toldAt = Now();
					s_toldFor = to;
				}
				path.clear();
				fresh = false;
			}
		}
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
		// a point is passed when it is reached, or when it is behind and the
		// next one is in a straight walk (a door's point dropped a metre
		// before it left the walk to the next one grazing the frame)
		while (!path.empty() && (Dist2D(path[0], pos) < 0.5f
			|| (path.size() > 1 && Dist2D(path[0], pos) < 1.5f && (pos - path[0]).Dot(path[1] - path[0]) > 0.0f && ClearWay(pMe, pos, path[1], 0.38f))))
			path.erase(path.begin());
		// the farthest of the next points the body walks to straight (the
		// corner of a fence is walked round, not cut)
		if (Now() >= s_bot.wayCheckAt || s_bot.wayCheckPath != s_bot.pathAt)
		{
			s_bot.wayCheckAt = Now() + 0.25f;
			s_bot.wayCheckPath = s_bot.pathAt;
			for (size_t k = std::min(path.size(), (size_t)6); k-- > 1; )
				if (Dist2D(path[k], pos) < 25.0f && ClearWay(pMe, pos, path[k], 0.38f))
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

	float LedgeAhead(CPlayer* pMe, const Vec3& pos, const Vec3& dir, bool* pTerrain = 0);

	// room over its head for the body to rise so high (a bridge's deck over a
	// river bank: a strength jump knocked its head on it)
	bool HeadRoom(CPlayer* pMe, const Vec3& pos, float rise)
	{
		IPhysicalEntity* pSkip = pMe->GetEntity()->GetPhysics();
		ray_hit hit;
		return !gEnv->pPhysicalWorld->RayWorldIntersection(pos + Vec3(0, 0, 1.9f), Vec3(0, 0, rise), ent_static | ent_terrain | ent_rigid | ent_sleeping_rigid,
			WALK_GEOM, &hit, 1, &pSkip, pSkip ? 1 : 0);
	}

	// up onto something the way goes onto (a container's floor, a ledge, a
	// bank): as a player does it, not one jump on the spot. A jump lifts the
	// feet 1.0 m, the strength mode's 4.7 m, and the body steps onto what is
	// 0.5 m higher; it gets there only pressing on into it all the time in
	// the air. It jumped once at a bank by the bridge, its way turned along
	// the bank or stopped in the air ("there"), rose 2 m and fell back, twice;
	// the user got up in armor jumping again and again (2026-10-23). Not
	// again where it failed twice lately: a way round then
	bool StartClimb(CPlayer* pMe, const Vec3& pos, const Vec3& dir, float rise, const char* why, bool terrain = false)
	{
		const float now = Now();
		if (s_bot.climbUntil > 0.0f || rise < 0.45f || rise > 2.8f)
			return false;
		const bool samePlace = Dist2D(pos, s_bot.climbPlace) < 3.0f && now - s_bot.climbPlaceAt < 60.0f;
		if (samePlace && s_bot.climbFails >= 2)
			return false;
		CNanoSuit* pSuit = pMe->GetNanoSuit();
		// (a jump lifts the feet 1.0 m, less on a slope; the strength mode's
		// 4.7. Up a steep bank of the terrain a jump is tilted off it: four of
		// them fell back where one strength jump got up, 2026-10-23)
		const bool strength = rise > 1.0f || (terrain && rise > 0.6f);
		// (the energy back first, a few seconds: not jumps that fall short)
		if (strength && !(pSuit && pSuit->GetSuitEnergy() >= 90.0f))
			return false;
		// no room over its head here (a bridge's deck): a step along the face
		// to where there is, then up from there
		Vec3 start = pos;
		if (!HeadRoom(pMe, pos, rise + 0.3f))
		{
			const Vec3 along(dir.y, -dir.x, 0.0f);
			bool found = false;
			for (int k = 0; k < 4 && !found; ++k)
			{
				const Vec3 at = pos + along * ((k & 1) ? -1.0f : 1.0f) * (k < 2 ? 1.2f : 2.4f);
				const float there = LedgeAhead(pMe, at, dir);
				if (there > 0.0f && HeadRoom(pMe, at, there + 0.3f) && ClearWay(pMe, pos, at))
				{
					start = at;
					rise = there;
					found = true;
				}
			}
			if (!found)
			{
				if (!samePlace || s_bot.climbFails == 0)
					Event("no room over its head to climb the %.1f m in the way", rise);
				if (!samePlace)
				{
					s_bot.climbPlace = pos;
					s_bot.climbFails = 0;
				}
				s_bot.climbPlaceAt = now;
				++s_bot.climbFails;
				return false;
			}
		}
		s_bot.climbUntil = now + (strength ? 6.0f : 5.0f);
		s_bot.climbSince = now;
		s_bot.climbDir = dir;
		s_bot.climbDir.z = 0.0f;
		s_bot.climbDir.NormalizeSafe(Vec3(0, 1, 0));
		s_bot.climbFromZ = pos.z;
		s_bot.climbRise = rise;
		s_bot.climbJumps = 0;
		s_bot.climbStart = start;
		s_bot.climbPhase = Dist2D(start, pos) > 0.3f ? 3 : 0;
		s_bot.climbGroundAt = -1.0f;
		s_bot.climbPhaseAt = now;
		s_bot.climbStrength = strength;
		s_bot.climbWasAir = false;
		s_bot.climbSeenAt = now;
		if (strength)
			s_bot.strengthJumpAt = now;
		Event("climbs %.1f m (%s)%s%s", rise, why, strength ? " with the strength" : "", s_bot.climbPhase == 3 ? ", from a step along it (no room over its head here)" : "");
		return true;
	}

	// a climb under way: true while it takes the move (phase 0 a step back for
	// a run, 1 running in, then the jump held till it is off the ground, 2 in
	// the air pressing on). Up: done; fallen back: again, a few times
	bool ClimbStep(CPlayer* pMe, const Vec3& pos)
	{
		if (s_bot.climbUntil <= 0.0f)
			return false;
		const float now = Now();
		s_bot.climbSeenAt = now;
		// (shot at, an enemy seen, falling back: that first)
		if (now - s_bot.hitAt < 6.0f || s_bot.retreating || (s_bot.enemy && now - s_bot.enemySeenAt < 8.0f))
		{
			s_bot.climbUntil = 0.0f;
			s_bot.strengthJumpAt = -1.0f;
			return false;
		}
		const SActorStats* pStats = pMe->GetActorStats();
		const bool inAir = pStats && pStats->inAir > 0.05f;
		if (inAir)
			s_bot.climbGroundAt = -1.0f;
		else if (s_bot.climbGroundAt < 0.0f)
			s_bot.climbGroundAt = now;
		// (up there and standing a moment: not partway up a bank, sliding back)
		const bool up = pos.z >= s_bot.climbFromZ + s_bot.climbRise - 0.3f && s_bot.climbGroundAt >= 0.0f && now - s_bot.climbGroundAt > 0.3f;
		if (up)
		{
			Event("up %.1f m after %d jump%s (%.1f s)", pos.z - s_bot.climbFromZ, s_bot.climbJumps, s_bot.climbJumps == 1 ? "" : "s", now - s_bot.climbSince);
			s_bot.climbUntil = 0.0f;
			s_bot.strengthJumpAt = -1.0f;
			s_bot.progAt = now;
			s_bot.stuck = 0;
			return false;
		}
		CNanoSuit* pSuit = pMe->GetNanoSuit();
		const int most = s_bot.climbStrength ? 2 : 4;
		// (the energy gone for the strength jump: again when it is back, not a
		// failure here - two of those and it never climbed that bank again)
		if (s_bot.climbStrength && s_bot.climbPhase != 2 && !(pSuit && pSuit->GetSuitEnergy() >= 70.0f))
		{
			Event("waits for the suit's energy to climb the %.1f m", s_bot.climbRise);
			s_bot.climbUntil = 0.0f;
			s_bot.strengthJumpAt = -1.0f;
			s_bot.climbLookAt = now + 2.0f;
			return false;
		}
		if ((now >= s_bot.climbUntil || s_bot.climbJumps >= most) && s_bot.climbPhase != 2)
		{
			const bool samePlace = Dist2D(pos, s_bot.climbPlace) < 3.0f && now - s_bot.climbPlaceAt < 60.0f;
			if (!samePlace)
			{
				s_bot.climbPlace = pos;
				s_bot.climbFails = 0;
			}
			s_bot.climbPlaceAt = now;
			++s_bot.climbFails;
			Event("could not climb the %.1f m here (%d jump%s, %.1f s): a way round", s_bot.climbRise, s_bot.climbJumps, s_bot.climbJumps == 1 ? "" : "s",
				now - s_bot.climbSince);
			s_bot.climbUntil = 0.0f;
			s_bot.strengthJumpAt = -1.0f;
			s_bot.pathAt = -100.0f;
			return false;
		}
		if (s_bot.climbStrength)
			s_bot.strengthJumpAt = now;
		// (in the strength mode before the strength jump)
		const bool ready = !s_bot.climbStrength || (pSuit && pSuit->GetMode() == NANOMODE_STRENGTH);
		if (s_bot.climbPhase == 2)
		{
			// landed below: a step back, a run, a jump again (sliding down a
			// steep bank counts as the air: longer than a strength jump's 2 s)
			if ((!inAir && !s_bot.climbWasAir) || now - s_bot.climbPhaseAt > 2.3f)
			{
				s_bot.climbPhase = 0;
				s_bot.climbPhaseAt = now;
			}
		}
		else if (inAir && !s_bot.climbWasAir && s_bot.climbPhase == 1 && s_bot.climbPressed)
		{
			++s_bot.climbJumps;
			s_bot.climbPhase = 2;
			s_bot.climbPhaseAt = now;
			s_bot.climbPressed = false;
		}
		s_bot.climbWasAir = inAir;
		// (a run at it: a take-off pressed against a steep bank has no speed on
		// into it, the air adds little - up to the top's height a metre out,
		// then back down, again and again; a jump with a run got up)
		if (s_bot.climbPhase == 0 && (Dist2D(pos, s_bot.climbStart) > 1.4f || now - s_bot.climbPhaseAt > 1.0f))
		{
			s_bot.climbPhase = 1;
			s_bot.climbPhaseAt = now;
			s_bot.climbPressed = false;
		}
		if (s_bot.climbPhase == 3 && (Dist2D(pos, s_bot.climbStart) < 0.4f || now - s_bot.climbPhaseAt > 2.0f))
		{
			s_bot.climbPhase = 0;
			s_bot.climbPhaseAt = now;
		}
		Vec3 toStart = s_bot.climbStart - pos;
		toStart.z = 0.0f;
		s_ctl.moveDir = s_bot.climbPhase == 3 ? toStart.GetNormalizedSafe(s_bot.climbDir) : s_bot.climbPhase == 0 ? -s_bot.climbDir : s_bot.climbDir;
		s_ctl.sprint = s_bot.climbPhase == 1;
		// (facing it all the time: no slower backwards, the run straight in)
		s_ctl.look = true;
		s_ctl.yaw = Yaw(s_bot.climbDir);
		s_ctl.pitch = 0.0f;
		s_ctl.turnRate = 8.0f;
		const Vec3 view = pMe->GetViewQuatFinal().GetColumn1();
		const bool facing = Vec3(view.x, view.y, 0.0f).GetNormalizedSafe(Vec3(0, 1, 0)).Dot(s_bot.climbDir) > 0.9f;
		// (at the foot: the feet going up the bank, at the place it began, or
		// long enough on the way back to it)
		const bool atFoot = pos.z > s_bot.climbFromZ + 0.2f || Dist2D(pos, s_bot.climbStart) < 0.5f || now - s_bot.climbPhaseAt > 0.8f;
		s_ctl.jump = s_bot.climbPhase == 1 && ready && facing && now - s_bot.climbPhaseAt > 0.15f && atFoot;
		if (s_ctl.jump)
			s_bot.climbPressed = true;
		return true;
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

	// in a doorway, or between two things close on both sides: toward the
	// middle of the gap (the body caught on a door's frame it went through
	// a hand's breadth off the middle)
	Vec3 Centered(CPlayer* pMe, const Vec3& pos, const Vec3& dir)
	{
		IPhysicalEntity* pSkip = pMe->GetEntity()->GetPhysics();
		const Vec3 right(dir.y, -dir.x, 0);
		const Vec3 at = pos + Vec3(0, 0, 1.0f);
		ray_hit hl, hr;
		const int ents = ent_static | ent_rigid | ent_sleeping_rigid;
		const bool l = gEnv->pPhysicalWorld->RayWorldIntersection(at, -right * 0.8f, ents, WALK_GEOM, &hl, 1, &pSkip, pSkip ? 1 : 0) > 0;
		const bool r = gEnv->pPhysicalWorld->RayWorldIntersection(at, right * 0.8f, ents, WALK_GEOM, &hr, 1, &pSkip, pSkip ? 1 : 0) > 0;
		float push = 0.0f;
		if (l && r)
			push = (hr.dist - hl.dist) * 0.5f * 0.6f;
		else if (l && hl.dist < 0.45f)
			push = 0.4f;
		else if (r && hr.dist < 0.45f)
			push = -0.4f;
		return push ? (dir + right * push).GetNormalizedSafe(dir) : dir;
	}

	// walk toward a point; true when there
	// on the way to an item it wants (no strength jumps at it: it jumped at
	// a window for a box); any other way, to a downed teammate say, may
	bool GoingToLoot(const Vec3& goal)
	{
		IEntity* pLoot = s_bot.loot ? gEnv->pEntitySystem->GetEntity(s_bot.loot) : 0;
		return pLoot && Dist2D(goal, pLoot->GetWorldPos()) < 1.5f;
	}

	bool MoveTo(CPlayer* pMe, const Vec3& to, float stopAt, bool mayRun, bool mayCatchUp, const char* toName)
	{
		const Vec3 pos = pMe->GetEntity()->GetWorldPos();
		const float now = Now();
		Vec3 d = to - pos;
		d.z = 0;
		const float dist = d.GetLength();
		// the one it follows out of a straight walk for long and it gets no
		// nearer (a way round would): put next to him, however near he is.
		// After the dome's cutscene the host stood 5-7 m off outside the
		// door it stood behind; its own squares found no way out, the host's
		// navigation none indoors, and the catch-ups below wanted him > 8 m
		// off: it went to and fro at the wall for minutes (the user's game,
		// 2026-10-22). Not in a fight (going round there is on purpose)
		if (mayCatchUp && now >= s_bot.unreachCheckAt)
		{
			s_bot.unreachCheckAt = now + 0.5f;
			const bool inFightNow = now - s_bot.hitAt < 5.0f || (s_bot.enemy && now - s_bot.enemySeenAt < 3.0f);
			const bool clear = dist < 3.5f || fabsf(to.z - pos.z) > 3.0f || ClearWay(pMe, pos, to);
			if (clear || inFightNow || pMe->GetLinkedVehicle())
				s_bot.unreachSince = -1.0f;
			else if (s_bot.unreachSince < 0.0f || dist < s_bot.unreachBest - 1.5f)
			{
				// (out of reach from now, or nearer by a good step: round by a door)
				s_bot.unreachSince = now;
				s_bot.unreachBest = dist;
			}
			else if (now - s_bot.unreachSince > 20.0f)
			{
				Event("%s %.0f m off with something between for %.0f s, no nearer: put next to him", toName && *toName ? toName : "the one it follows",
					dist, now - s_bot.unreachSince);
				Ask(1, toName);
				s_bot.unreachSince = -1.0f;
			}
		}
		else if (!mayCatchUp)
			s_bot.unreachSince = -1.0f;
		// next to the one it follows with a wall between (he inside a hut,
		// it outside by the wall): not there yet, round by the door (for a
		// while: clutter may keep it from a straight walk to him for good)
		// (not in the middle of a climb: it stopped pressing on in the air)
		bool reached = dist <= stopAt && s_bot.climbUntil <= 0.0f;
		// (the one it follows up on a bank, a ledge over it: not there yet, for
		// a while. 4 m below the host it stopped in the middle of a jump up to
		// him, the distance being the one on the map; the user's game,
		// 2026-10-23. Not for ever: under a tower, a roof he is on, it stands
		// as before; not in a fight, it keeps its room there)
		const bool apart = mayCatchUp && to.z - pos.z > 2.0f && !s_bot.enemy && !pMe->GetLinkedVehicle();
		if (reached && mayCatchUp && ((dist > 1.5f && !ClearWay(pMe, pos, to)) || apart))
		{
			if (s_bot.arriveBlockedSince < 0.0f)
				s_bot.arriveBlockedSince = now;
			reached = now - s_bot.arriveBlockedSince > (apart ? 20.0f : 10.0f);
		}
		else
			s_bot.arriveBlockedSince = -1.0f;
		if (reached)
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
		// may lead right through what it does not know: under a truck). Not
		// when the host's way goes round (by a door): its own squares, made
		// to go straight to the goal, found none through the wall and led it
		// into the wall nearest to him (the user's game of 2026-10-17: 20-30
		// s inside the bridge house by the door the host's way went out of)
		const bool hostWay = !s_bot.path.empty() && now - s_bot.pathAt < 12.0f && Dist2D(s_bot.pathGoal, to) < 4.0f;
		const bool navDetour = hostWay && WayLeft(pos, to) > 1.3f * dist + 1.0f;
		const Vec3 target = dist < 14.0f && !navDetour ? to : next;
		const float toTarget = Dist2D(pos, target);
		if (!hostWay || Dist2D(s_bot.doorFor, to) > 4.0f)
			s_bot.doorMode = false;
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
					{
						local.swap(alt);
						found = true;
					}
					looked += more;
				}
				string why = found ? string() : GridWhyAround(pos);
				// not found: once more on quarter-metre squares close by (a
				// narrow door, a gap between crates)
				float cell = GRID_CELL;
				if (!found && toTarget < 6.0f && now - s_bot.fineGridAt > 3.0f)
				{
					s_bot.fineGridAt = now;
					std::vector<Vec3> fine;
					int more = 0;
					bool there = false;
					GridBegin(pMe, pos, 6.0f, 0.25f);
					if (GridPath(pos, target, fine, &more, &there) && there)
					{
						local.swap(fine);
						found = true;
						cell = 0.25f;
					}
					looked += more;
				}
				s_bot.localFound = found;
				// (its own squares found no way to the goal itself: the host's
				// long way round is taken for it then, see Waypoint)
				if (!found && Dist2D(target, to) < 0.5f)
				{
					s_bot.ownNoneFor = to;
					s_bot.ownNoneAt = now;
				}
				else if (found && Dist2D(target, to) < 0.5f)
				{
					// (its own way to the goal: not the host's long way round)
					s_bot.ownNoneAt = -100.0f;
					s_bot.longWayAt = -100.0f;
				}
				if (found)
					s_bot.doorMode = false;
				// only nearer, not there: not walked into the wall between
				// (the square nearest to the goal is that wall's face). Along
				// the host's way point by point instead; with none, it waits
				// for one a moment (its own squares' best when the host's
				// navigation has no way either)
				const char* plan = found ? "found" : local.empty() ? "none" : "partial";
				if (!found && hostWay)
				{
					local.clear();
					if (!s_bot.doorMode || Dist2D(s_bot.doorFor, to) > 4.0f)
						Event("its own squares find no way to %s (%d looked at): along the host's navigation (%d points)", toName && *toName ? toName : "where it goes",
							looked, (int)s_bot.path.size());
					s_bot.doorMode = true;
					s_bot.doorFor = to;
				}
				else if (!found && !(now - s_bot.pathNoneAt < 10.0f && Dist2D(s_bot.pathNoneGoal, to) < 4.0f))
				{
					AskPath(to);
					// (waits only for the answer to an ask for this goal, 2 s at
					// most after it: not again with each plan while the one it
					// follows walks on)
					// (once for a goal: then its own squares' best, as before; it
					// stood by the hut's door, asking and waiting again every 3 s)
					const float askedAt = s_bot.pathAskAt - 3.0f;
					const bool waitedHere = Dist2D(s_bot.waitedFor, to) < 4.0f && now - s_bot.waitedAt < 15.0f;
					if (now - askedAt < 2.0f && s_bot.pathAt < askedAt && Dist2D(s_bot.pathAskGoal, to) < 4.0f && (!waitedHere || now < s_bot.waitWayUntil))
					{
						local.clear();
						s_bot.waitWayUntil = askedAt + 2.0f;
						if (!waitedHere)
						{
							s_bot.waitedFor = to;
							s_bot.waitedAt = now;
						}
					}
				}
				if (moved)
				{
					++s_bot.localPlans;
					CryLogAlways("[CoopAgent] a way round to %s close by: %s%s (%d squares looked at, %.1f ms, %.2f m squares%s%s)", toName && *toName ? toName : "where it goes",
						local.empty() ? "none" : string().Format("%d corners, %.1f m to the point %.1f m away", (int)local.size(),
							WayLeftOf(pos, local), toTarget).c_str(), local.empty() ? "" : string().Format(", %s", plan).c_str(), looked,
						(gEnv->pTimer->GetAsyncCurTime() - t0) * 1000.0f, cell, why.empty() ? "" : "; blocked round it: ", why.c_str());
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
			// (nearer only in the air, on a jump onto a ledge it fell back
			// from: not counted)
			const bool jumping = s_bot.climbUntil > 0.0f || (now - s_bot.jumpAt < 2.0f && Dist2D(pos, s_bot.sjPlace) < 3.0f && s_bot.sjTries >= 2);
			if (s_bot.progBest < 1e8f && !jumping)
			{
				s_bot.progAt = now;
				s_bot.stuck = 0;
			}
			s_bot.progBest = left;
		}
		Vec3 dn = (local.empty() ? next : local[0]) - pos;
		const float rise = (local.empty() ? next.z : local[0].z) - pos.z;
		// (the goal itself is no floor to jump onto: an item on a table, the
		// one it follows on a rock)
		const bool stepIsGoal = Dist2D(local.empty() ? next : local[0], to) < 0.3f;
		dn.z = 0;
		Vec3 dir = dn.GetLength() > 0.1f ? dn.GetNormalized() : d / dist;
		// up onto what the way goes on (a container's floor, a step too high
		// to walk): a jump right before it, not only when it got nowhere
		if (rise > 0.45f && rise < 1.3f && dn.GetLength() < 1.2f && now - s_bot.jumpAt > 1.5f && !stepIsGoal && s_bot.climbUntil <= 0.0f)
		{
			s_ctl.jump = true;
			s_bot.jumpAt = now;
		}
		// a bank or a ledge right ahead toward where it goes, that much higher
		// and no straight walk there: up it, as a player does (twice here at
		// most, then the way round; at the bank by the bridge the way round
		// was 50 m for the 9 m up to the host). (Not "a short way round found"
		// first: its squares' ways round along the bank led nowhere, it went
		// to and fro 60 m without a jump, a test of 2026-10-23)
		if (!s_bot.toClear && s_bot.climbUntil <= 0.0f && to.z - pos.z > 1.0f && dist > 0.5f && dist < 15.0f && now >= s_bot.climbLookAt
			&& now - s_bot.hitAt > 6.0f && !s_bot.retreating && !(s_bot.enemy && now - s_bot.enemySeenAt < 8.0f) && !GoingToLoot(to))
		{
			s_bot.climbLookAt = now + 0.5f;
			// (not to one under a roof: the floor of a room behind a house's
			// wall looks a ledge; that way is the door, the review)
			ray_hit roof;
			const bool indoors = gEnv->pPhysicalWorld->RayWorldIntersection(to + Vec3(0, 0, 2.0f), Vec3(0, 0, 6.0f), ent_static, WALK_GEOM, &roof, 1) > 0;
			bool terrain = false;
			const float ledge = indoors ? 0.0f : LedgeAhead(pMe, pos, d / dist, &terrain);
			if (ledge > 0.0f)
				StartClimb(pMe, pos, d / dist, ledge, "a bank up to where it goes", terrain);
		}
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
			// too high to jump onto as it is (a container's floor, a crate):
			// a strength jump (the suit's mode first: SuitStep)
			// (not for an item on the ground: it jumped at a window for a box)
			else if (s_bot.strengthJumpAt < 0.0f && pMe->GetNanoSuit() && pMe->GetNanoSuit()->GetSuitEnergy() >= 90.0f && now - s_bot.hitAt > 6.0f
				&& !s_bot.retreating && !(s_bot.enemy && now - s_bot.enemySeenAt < 8.0f) && !GoingToLoot(to))
			{
				bool terrain = false;
				const float ledge = LedgeAhead(pMe, pos, dir, &terrain);
				// (twice at the same place and still there: not again. It jumped
				// at a 1.6 m ledge a dozen times on the way to the host, fell back
				// each time, the energy gone each time, and as each jump got it a
				// little nearer it never counted as stuck long enough to be put
				// next to him)
				// (a climb now: straight at it, no step along the face)
				if (ledge > 0.0f && rise > 0.3f && StartClimb(pMe, pos, dir, ledge, "something in the way it got no further at", terrain))
					s_bot.slideUntil = 0.0f;
			}
			Event("no nearer to %s for %.0f s (%.0f m to go): a new way, a way round close by%s", toName && *toName ? toName : "where it goes",
				stalled, dist, low ? ", a jump over something low" : "");
		}
		// (close to him too, long without getting anywhere: inside a hut, it
		// outside by the wall, it pressed against that wall for good)
		if (s_bot.stuck >= 4 && mayCatchUp && (dist > 8.0f || s_bot.stuck >= 5))
			Ask(1, toName);
		// going round within 4 m for 15 s, no nearer to the one it follows:
		// put next to him. (The host's navigation led up a rock 4 m high over
		// 2 m, as the soldiers go; it went to and fro under it a minute, each
		// step a little nearer, so never stuck long enough by the count above)
		// (not in a fight: side steps there are going round on purpose)
		const bool inFight = now - s_bot.hitAt < 5.0f || (s_bot.enemy && now - s_bot.enemySeenAt < 3.0f);
		if (mayCatchUp && dist > 8.0f && !inFight)
		{
			if (s_bot.circleSince < 0.0f || now - s_bot.circleLast > 1.0f || Dist2D(pos, s_bot.circleAt) > 4.0f || dist < s_bot.circleBest - 2.0f)
			{
				s_bot.circleAt = pos;
				s_bot.circleSince = now;
				s_bot.circleBest = dist;
			}
			else if (now - s_bot.circleSince > 15.0f)
			{
				Event("going round within 4 m for %.0f s, %.0f m from %s: put next to him", now - s_bot.circleSince, dist, toName && *toName ? toName : "him");
				Ask(1, toName);
				s_bot.circleSince = -1.0f;
			}
			s_bot.circleLast = now;
		}
		else
			s_bot.circleSince = -1.0f;
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
		// off it (that took it back against the fence). Along the host's way
		// through a door: none either, the middle of the doorway; straight on
		// to a point in a free walk close by neither (a stair looked like a
		// wall ahead). Waiting for the host's way: still
		if (local.empty() && !hostWay && now < s_bot.waitWayUntil)
			s_ctl.moveDir.zero();
		else if (s_bot.doorMode && local.empty())
			s_ctl.moveDir = Centered(pMe, pos, dir);
		else if (local.empty() && !(s_bot.nextClear && toNext < 6.0f))
			s_ctl.moveDir = Steer(pMe, pos, dir);
		else
			s_ctl.moveDir = local.empty() ? dir : Dist2D(local[0], pos) < 1.5f ? Centered(pMe, pos, dir) : dir;
		s_ctl.sprint = mayRun && dist > 15.0f;
		ClimbStep(pMe, pos);
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
	// things to carry (the agent's "grab", "throw", "drop"): the left hand as
	// a player's (CoopProps makes every game see it the same)
	enum ECarry { eC_None, eC_Grab, eC_Carry, eC_Throw, eC_Drop, eC_Thrown };
	struct SCarry
	{
		EntityId prop;
		int mode;
		float since, pressAt;
		Vec3 at;            // where to throw it (zero: where it looks)
		string name;
		SCarry(): prop(0), mode(eC_None), since(0), pressAt(0), at(ZERO) {}
	} s_carry;

	COffHand* OffHand(CPlayer* pMe)
	{
		return pMe ? static_cast<COffHand*>(pMe->GetWeaponByClass(CItem::sOffHandClass)) : 0;
	}

	Vec3 Middle(IEntity* pEntity)
	{
		AABB box;
		pEntity->GetWorldBounds(box);
		return box.IsEmpty() ? pEntity->GetWorldPos() : box.GetCenter();
	}

	// one the left hand takes (Properties.bPickable, as COffHand asks: most
	// loose things of a level are not, the wheels of a trash container too)
	bool Pickable(IEntity* pEntity)
	{
		IScriptTable* pScript = pEntity->GetScriptTable();
		SmartScriptTable props;
		int pickable = 1;
		return !(pScript && pScript->GetValue("Properties", props) && props->GetValue("bPickable", pickable) && !pickable);
	}

	// a thing by its name, its id, or "nearest" (within 8 m)
	IEntity* FindThing(CPlayer* pMe, const string& what)
	{
		if (what.empty() || what == "nearest")
		{
			const Vec3 pos = pMe->GetEntity()->GetWorldPos();
			IPhysicalEntity** list = 0;
			const int n = gEnv->pPhysicalWorld->GetEntitiesInBox(pos - Vec3(8, 8, 4), pos + Vec3(8, 8, 4), list, ent_rigid | ent_sleeping_rigid);
			IEntity* pBest = 0;
			float best = 1e9f;
			for (int i = 0; i < n; ++i)
			{
				IEntity* pEntity = gEnv->pEntitySystem->GetEntityFromPhysics(list[i]);
				if (!pEntity || !CoopProps::Eligible(pEntity) || !Pickable(pEntity) || CoopProps::IsHeld(pEntity->GetId()))
					continue;
				const float d = Middle(pEntity).GetDistance(pos);
				if (d < best)
				{
					best = d;
					pBest = pEntity;
				}
			}
			return pBest;
		}
		const int id = atoi(what.c_str());
		if (id > 0 && what.find_first_not_of("0123456789") == string::npos)
			return gEnv->pEntitySystem->GetEntity((EntityId)id);
		return gEnv->pEntitySystem->FindEntityByName(what.c_str());
	}

	// a step of an order with a thing: true while it takes the frame (walks
	// to it, aims, presses the key)
	bool CarryStep(CPlayer* pMe, const Vec3& pos, const Vec3& eye, bool busy)
	{
		if (s_carry.mode == eC_None)
			return false;
		COffHand* pOffHand = OffHand(pMe);
		const EntityId held = pOffHand ? pOffHand->CoopHeldId() : 0;
		IEntity* pThing = gEnv->pEntitySystem->GetEntity(s_carry.prop);
		const float now = Now();
		switch (s_carry.mode)
		{
		case eC_Grab:
		{
			if (!pThing)
			{
				Event("the thing to pick up is gone");
				s_carry = SCarry();
				return false;
			}
			if (held == s_carry.prop && pOffHand->CoopHolding())
			{
				Event("holds %s", s_carry.name.c_str());
				s_carry.mode = eC_Carry;
				return false;
			}
			if (now - s_carry.since > 25.0f)
			{
				Event("could not pick up %s", s_carry.name.c_str());
				s_carry = SCarry();
				return false;
			}
			// (somebody else picked it up meanwhile)
			if (CoopProps::IsHeld(s_carry.prop) && held != s_carry.prop)
			{
				Event("%s is in somebody else's hands", s_carry.name.c_str());
				s_carry = SCarry();
				return false;
			}
			// (a teammate down, or shot at: that first)
			if (busy)
				return false;
			Fire(pMe, false);
			HoldUse(pMe, false, 0);
			const Vec3 middle = Middle(pThing);
			const float d = Dist2D(middle, pos);
			if (d > 1.3f)
			{
				MoveTo(pMe, middle, 1.2f, d > 5.0f, false, s_carry.name.c_str());
				LookAt(eye, middle, 6.0f);
			}
			else
			{
				LookAt(eye, middle, 8.0f);
				// (aimed at it: the use key, as a player)
				const Vec3 view = pMe->GetViewQuatFinal().GetColumn1();
				const Vec3 to = (middle - eye).GetNormalizedSafe(view);
				if (view.Dot(to) > 0.985f && now >= s_carry.pressAt && pOffHand && pOffHand->GetOffHandState() == eOHS_INIT_STATE)
				{
					Press(pMe, g_pGame->Actions().use, true);
					Press(pMe, g_pGame->Actions().use, false);
					s_carry.pressAt = now + 1.5f;
					Event("reaches for %s (%.1f m)", s_carry.name.c_str(), middle.GetDistance(eye));
				}
			}
			s_bot.doing.Format("picking up %s", s_carry.name.c_str());
			return true;
		}
		case eC_Carry:
			if (held != s_carry.prop)
			{
				Event("no longer holds %s", s_carry.name.c_str());
				s_carry = SCarry();
			}
			return false;
		case eC_Throw:
		case eC_Drop:
		{
			if (held != s_carry.prop || !held)
			{
				s_carry = SCarry();
				return false;
			}
			Fire(pMe, false);
			HoldUse(pMe, false, 0);
			// (not in the middle of a swing)
			if (!pOffHand->CoopHolding())
				return true;
			if (!s_carry.at.IsZero())
				LookAt(eye, s_carry.at, 8.0f);
			// (a moment to turn to where it throws)
			if (now - s_carry.since < (s_carry.at.IsZero() ? 0.1f : 0.6f))
				return true;
			const bool drop = s_carry.mode == eC_Drop;
			const ActionId& key = drop ? g_pGame->Actions().zoom : g_pGame->Actions().use;
			Press(pMe, key, true);
			Press(pMe, key, false);
			Event("%s %s", drop ? "drops" : "throws", s_carry.name.c_str());
			if (drop)
			{
				s_carry = SCarry();
				return true;
			}
			s_carry.mode = eC_Thrown;
			s_carry.since = now;
			return true;
		}
		case eC_Thrown:
			// (the aim kept until it leaves the hand: the throw takes the eyes'
			// direction 0.15-0.4 s after the key)
			if (held == s_carry.prop && now - s_carry.since < 1.0f)
			{
				if (!s_carry.at.IsZero())
					LookAt(eye, s_carry.at, 8.0f);
				return true;
			}
			s_carry = SCarry();
			return false;
		}
		return false;
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
		// not swimming (the fists only there, as in single player: it took
		// out its rifle again and swam holding it out) nor in a vehicle's seat
		// (put away there; a rifle drawn as it got in stayed in its hands at
		// the boat's gun on the host's screen, the user's game of 2026-10-23)
		if (pMe->GetLinkedVehicle() || pMe->IsSwimming() || pMe->ShouldSwim())
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

	// the one it follows under a follow/free order, alive (0: none)
	IActor* FollowedLeader()
	{
		if (s_bot.order != eO_Follow && s_bot.order != eO_Free)
			return 0;
		IActor* pLead = s_bot.leader.empty() ? HostPlayer() : ActorByName(s_bot.leader.c_str());
		return pLead && pLead->GetHealth() > 0 ? pLead : 0;
	}

	// a carried weapon that shoots the ammo in that box, and how much it has
	// for it (-1: none of its weapons does)
	int ShotsForAmmo(CPlayer* pMe, const char* ammo)
	{
		IInventory* pInventory = pMe->GetInventory();
		IItemSystem* pItems = g_pGame->GetIGameFramework()->GetIItemSystem();
		int best = -1;
		for (int i = 0; pInventory && ammo && *ammo && i < pInventory->GetCount(); ++i)
		{
			IItem* pItem = pItems->GetItem(pInventory->GetItem(i));
			IWeapon* pWeapon = pItem && pItem->CanSelect() && WeaponRank(pItem->GetEntity()->GetClass()->GetName()) ? pItem->GetIWeapon() : 0;
			IFireMode* pMode = pWeapon ? pWeapon->GetFireMode(pWeapon->GetCurrentFireMode()) : 0;
			if (pMode && pMode->GetAmmoType() && !stricmp(pMode->GetAmmoType()->GetName(), ammo))
				best = max(best, WeaponShots(pMe, pItem));
		}
		return best;
	}

	// the ammo in a box on the ground (its archetype's properties)
	string BoxAmmo(IEntity* pBox)
	{
		IScriptTable* pScript = pBox->GetScriptTable();
		SmartScriptTable props;
		const char* name = 0;
		if (pScript && pScript->GetValue("Properties", props) && props->GetValue("AmmoName", name) && name)
			return name;
		return "";
	}

	// a weapon or ammo on the ground worth going for: a better weapon than
	// it has, or ammo for one it has when it runs low
	IEntity* FindLoot(CPlayer* pMe, const Vec3& pos, float range)
	{
		int bestRank = 0;
		IItem* pBest = BestWeapon(pMe, &bestRank);
		const int shots = pBest ? WeaponShots(pMe, pBest) : 0;
		const bool low = !pBest || bestRank < 6 || shots < 60;
		// (following: nothing farther than 12 m from him, it would turn back
		// at 14 m and come again for it)
		IActor* pLead = pBest ? FollowedLeader() : 0;
		const Vec3 leadPos = pLead ? pLead->GetEntity()->GetWorldPos() : Vec3(ZERO);
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
			if (pLead && Dist2D(pEntity->GetWorldPos(), leadPos) > 12.0f)
				continue;
			// (a box the server would not give it: full of that ammo, or none
			// of its weapons takes it. It never got one: 0 of 10, use pressed
			// 15 times at each)
			if (ammoBox && !static_cast<CAmmoPickup*>(pItem)->CheckAmmoRestrictions(pMe->GetEntityId()))
				continue;
			// (a box only with ammo for one of its weapons that runs low on it,
			// the empty rifle too: low on the SCAR's it took one FY71 box after
			// another in the hut, the SCAR as low; with a pistol full of
			// rounds it went from box to box)
			if (ammoBox)
			{
				const int forIt = ShotsForAmmo(pMe, BoxAmmo(pEntity).c_str());
				if (forIt < 0 || forIt >= 60)
					continue;
			}
			const float d = Dist2D(pEntity->GetWorldPos(), pos);
			if (d < foundDist && fabsf(pEntity->GetWorldPos().z - pos.z) < 3.0f)
			{
				// (one it sees from where it stands, not behind a wall: a box
				// inside the hut 4 m off through its wall, it strength-jumped at
				// the window 20 s, pressed against the wall)
				AABB box;
				pEntity->GetWorldBounds(box);
				const Vec3 at = box.IsEmpty() ? pEntity->GetWorldPos() : box.GetCenter();
				const Vec3 from = pos + Vec3(0, 0, 1.4f);
				IPhysicalEntity* skip[2] = { pMe->GetEntity()->GetPhysics(), pEntity->GetPhysics() };
				ray_hit wall;
				// (walls only: the ray to an item lying on rolling ground
				// grazed the ground and hid it)
				if (FirstBlock(from, at - from, ent_static, skip, 2, wall, eT_See) && wall.dist < at.GetDistance(from) - 0.5f)
					continue;
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
			// taken (by it, or by someone; an ammo box is gone from the
			// ground once taken: it went unlogged so)
			if (pItem && pItem->GetOwnerId() == pMe->GetEntityId())
				Event("picked up the %s", pLoot->GetClass()->GetName());
			else if ((!pItem || pLoot->IsHidden()) && s_bot.lootUses > 0)
				Event("picked up the %s (gone from the ground)", s_bot.lootClass.c_str());
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
			// (sent somewhere: there, not to a box on the way; it stood 17 s
			// on a bed in the beach hut pressing use at one under a pillow)
			if (s_bot.order == eO_Goto && armed)
				return false;
			// (following one who walks off: with him, not to a box behind)
			if ((s_bot.order == eO_Follow || s_bot.order == eO_Free) && armed)
			{
				IActor* pLead = s_bot.leader.empty() ? HostPlayer() : ActorByName(s_bot.leader.c_str());
				if (pLead && pLead->GetHealth() > 0 && Dist2D(pLead->GetEntity()->GetWorldPos(), pos) > 10.0f)
					return false;
			}
			pLoot = FindLoot(pMe, pos, armed ? 20.0f : 45.0f);
			if (!pLoot)
				return false;
			s_bot.loot = pLoot->GetId();
			s_bot.lootClass = pLoot->GetClass()->GetName();
			s_bot.lootSince = Now();
			s_bot.lootUses = 0;
			s_bot.lootNear = 1000.0f;
			Event("goes to pick up the %s (%.0f m, at %.1f %.1f %.1f)", pLoot->GetClass()->GetName(), Dist2D(pLoot->GetWorldPos(), pos),
				pLoot->GetWorldPos().x, pLoot->GetWorldPos().y, pLoot->GetWorldPos().z);
		}
		// (the one it follows walked off meanwhile: after him, the box stays)
		if ((s_bot.order == eO_Follow || s_bot.order == eO_Free) && BestWeapon(pMe) != 0)
		{
			IActor* pLead = s_bot.leader.empty() ? HostPlayer() : ActorByName(s_bot.leader.c_str());
			if (pLead && pLead->GetHealth() > 0 && Dist2D(pLead->GetEntity()->GetWorldPos(), pos) > 14.0f)
			{
				// (given up a minute: else back for it as soon as it is near him)
				s_bot.lootFailed[pLoot->GetId()] = Now();
				s_bot.loot = 0;
				return false;
			}
		}
		// (in reach, use pressed again and again, not taken: the hand gets
		// something else there)
		// (or no nearer to it for 7 s: the way there is not the straight one)
		if (Now() - s_bot.lootSince > 20.0f || (s_bot.lootUses >= 5 && s_bot.lootNear < 2.2f) || (s_bot.stuck >= 2 && Now() - s_bot.lootSince > 3.0f))
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
	void FightStep(CPlayer* pMe, const Vec3& eye, IActor* pEnemy);

	// those who hit it within that long, alive and hostile, the newest first
	void RecentShooters(float within, std::vector<IActor*>& out)
	{
		std::vector<std::pair<float, EntityId> > list;
		for (std::map<EntityId, SThreat>::const_iterator it = s_threats.begin(); it != s_threats.end(); ++it)
			if (Now() - it->second.last < within)
				if (IActor* p = ActorOf(it->first))
					if (p->GetHealth() > 0 && Hostile(p))
						list.push_back(std::make_pair(-it->second.last, it->first));
		std::sort(list.begin(), list.end());
		out.clear();
		for (size_t i = 0; i < list.size(); ++i)
			out.push_back(ActorOf(list[i].second));
	}

	// a head that high above its feet there: none of them sees it (something
	// solid between it and their eyes)
	bool HiddenFrom(CPlayer* pMe, const Vec3& feet, float head, const std::vector<IActor*>& from)
	{
		const Vec3 p = feet + Vec3(0, 0, head);
		for (size_t i = 0; i < from.size(); ++i)
			if (from[i] && Visible(pMe, p, from[i]->GetEntity(), EyePos(from[i])))
				return false;
		return true;
	}

	// the energy a cloaked walk of that long takes (the suit drains 5 a metre
	// walking cloaked, nothing back while cloaked), with some to spare
	float CloakNeed(float metres)
	{
		return 5.0f * metres + 25.0f;
	}

	// a mode the suit refused a moment ago (SuitStep), till then
	int s_refusedMode = -1;
	float s_refusedUntil = 0.0f;

	// the suit can cloak now: the campaign gave it the mode, not refused
	// lately (else every cloaked move began again each 3 s, unarmed: the
	// way to a downed teammate without a shot back, the review)
	bool CanCloak(CPlayer* pMe)
	{
		CNanoSuit* pSuit = pMe ? pMe->GetNanoSuit() : 0;
		return pSuit && pSuit->IsModeActive(NANOMODE_CLOAK) && !(s_refusedMode == NANOMODE_CLOAK && Now() < s_refusedUntil);
	}

	// to there out of their sight (SuitStep cloaks, FightStep holds its fire,
	// the cloak ends there or when one comes close)
	void StartCloakedMove(CPlayer* pMe, const Vec3& goal, const char* why, float energy)
	{
		// (a burst still held: the next shot from the cloak takes all the
		// energy on the server, the review)
		if (pMe)
			Fire(pMe, false);
		s_bot.cloakBest = 1e9f;
		s_bot.cloakBestAt = Now();
		s_bot.cloakEscape = true;
		s_bot.cloakForRevive = false;
		s_bot.cloakGoal = goal;
		s_bot.cloakSince = Now();
		++s_bot.cloaks;
		Event("cloaks: %s (energy %.0f)", why, energy);
	}

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
			// (one who shot it a moment ago from afar, a machine gun's nest
			// 110 m off: still the danger when the cover hides him; the
			// retreat ended at once, and it walked back into his fire)
			if (!pEnemy)
			{
				float bestFar = 150.0f;
				for (std::map<EntityId, SThreat>::const_iterator t = s_threats.begin(); t != s_threats.end(); ++t)
				{
					IActor* pShooter = Now() - t->second.last < (s_bot.retreating ? 30.0f : 10.0f) ? ActorOf(t->first) : 0;
					if (pShooter && Hostile(pShooter) && pShooter->GetEntity()->GetWorldPos().GetDistance(pos) < bestFar)
					{
						bestFar = pShooter->GetEntity()->GetWorldPos().GetDistance(pos);
						pEnemy = pShooter;
					}
				}
			}
		}
		if (!pEnemy)
		{
			if (s_bot.retreating)
			{
				s_bot.recoveredAt = Now();
				s_bot.cloakEscape = false;
				Event("the fall back is over: no enemy left about (health %d)", pMe->GetHealth());
				if (CNanoSuit* pSuit = pMe->GetNanoSuit())
					if (pSuit->GetMode() == NANOMODE_CLOAK)
						pSuit->SetMode(NANOMODE_DEFENSE);
			}
			s_bot.retreating = false;
			s_bot.retreatNowAt = -100.0f;
			return false;
		}
		// falls back badly hurt (just back from it: only when worse still);
		// back to it only healed and after a while (no running to and fro).
		// Also before the armor is gone: shot again and again, hurt, still
		// some energy, it gets away cloaked (the soldiers lose sight of it,
		// as of the player). Before, it waited for 60 % health, by then the
		// armor had taken all the energy: it never cloaked once in 9 games
		CNanoSuit* pSuit = pMe->GetNanoSuit();
		const float energy = pSuit ? pSuit->GetSuitEnergy() : 0.0f;
		const float limit = Now() - s_bot.recoveredAt < 8.0f ? 0.35f : 0.6f;
		// (a heavy gun takes 70 energy a hit: the armor gone in three, the
		// health still whole; away cloaked while there is energy left)
		// (a heavy gun: away cloaked at its first hit, while there is energy
		// for more than a second of cloak; at its second the energy was 60,
		// the cloak lasted a second)
		// (three hits of it may come before it looks: the energy 200, then 0.
		// Walking cloaked, 40 lasts a few seconds)
		const bool heavy = Now() - s_bot.heavyHitAt < 1.5f && energy >= 40.0f;
		// (a heavy gun again right after it came back: away at once, not 8 s
		// in its fire first)
		const bool pounded = (heavy && Now() - s_bot.recoveredAt > 2.0f)
			|| (HitsWithin(3.0f) >= 2 && energy >= 50.0f && (energy < 150.0f || health < 0.75f) && Now() - s_bot.recoveredAt > 8.0f);
		// (the armor gone, each hit on the health: away before it is half
		// gone, not at 60 % with four soldiers round it; not at 92 % either:
		// 4 of the user's 6 fall backs began there, 2026-10-21. 3 s after it
		// came back, not 8: it stood in fire with no armor so)
		const bool bare = HitsWithin(3.0f) >= 2 && energy < 20.0f && health < 0.8f && Now() - s_bot.recoveredAt > 3.0f;
		const bool retreatNow = Now() - s_bot.retreatNowAt < 1.0f;
		if (!s_bot.retreating && (health < limit || pounded || bare || retreatNow))
		{
			s_bot.retreatNowAt = -100.0f;
			s_bot.retreating = true;
			s_bot.retreatSince = Now();
			s_bot.retreatFrom = pos;
			// the cover it is in, or the place it is at when that hides it from
			// those shooting at it: it gets down there (each fall back was a run
			// of 4 m and more in their fire, 6 hits on the way, the user's game
			// of 2026-10-21); not from a heavy gun that shot it there
			std::vector<IActor*> shooters;
			RecentShooters(3.0f, shooters);
			if (shooters.empty())
				shooters.push_back(pEnemy);
			const bool inCover = s_bot.haveCover && Dist2D(s_bot.cover, pos) < 1.5f;
			const bool hiddenCrouching = HiddenFrom(pMe, pos, 1.0f, shooters);
			const bool hiddenLying = hiddenCrouching || HiddenFrom(pMe, pos, 0.4f, shooters);
			const char* how = "falls back to cover";
			// (its cover only while it still hides it: one flanked is no cover;
			// never where a heavy gun just hit it: its rounds go through planks;
			// never by a vehicle shot half to pieces: the soldiers shoot it till
			// it blows, as FindCover's wrecks, the review)
			bool byWreck = false;
			{
				IVehicleIteratorPtr pVs = g_pGame->GetIGameFramework()->GetIVehicleSystem()->CreateVehicleIterator();
				while (IVehicle* pV = pVs->Next())
					if (!pV->IsDestroyed() && pV->GetDamageRatio() > 0.5f && Dist2D(pV->GetEntity()->GetWorldPos(), pos) < 8.0f)
					{
						byWreck = true;
						break;
					}
			}
			if (!retreatNow && Now() - s_bot.heavyHitAt > 1.5f && hiddenLying && !byWreck)
			{
				s_bot.coverBest = 0.0f;
				s_bot.coverBestAt = Now();
				s_bot.cover = pos;
				s_bot.haveCover = true;
				s_bot.coverAt = Now();
				s_bot.coverPeek = 3;
				s_bot.coverProne = !hiddenCrouching;
				s_bot.coverReachedAt = Now();
				how = inCover ? (s_bot.coverProne ? "lies down in its cover" : "stays down in its cover")
					: s_bot.coverProne ? "lies down where it is (hidden so)" : "gets down where it is (hidden crouching)";
			}
			else
			{
				s_bot.haveCover = false;
				s_bot.coverAskAt = 0.0f;
				// (cloaked to the cover when the energy is enough for the way:
				// the cover about 10 m off until the host's game names it; a
				// heavy gun's next hits come at once, a short way cloaked then)
				const bool closeOne = s_bot.enemy && ActorOf(s_bot.enemy) && Now() - s_bot.enemySeenAt < 1.0f
					&& ActorOf(s_bot.enemy)->GetEntity()->GetWorldPos().GetDistance(pos) < 8.0f;
				if (!s_bot.cloakEscape && !closeOne && energy >= (heavy ? 40.0f : CloakNeed(10.0f)) && CanCloak(pMe))
				{
					string why;
					why.Format("to get away to cover (health %d, %d hits in 3 s)", pMe->GetHealth(), HitsWithin(3.0f));
					StartCloakedMove(pMe, Vec3(ZERO), why.c_str(), energy);
				}
			}
			Event("hurt (health %d, energy %.0f): %s%s", pMe->GetHealth(), energy, how,
				s_bot.cloakEscape ? ", cloaked" : energy < CloakNeed(10.0f) ? " (no energy for the cloak)" : "");
		}
		// (a heavy gun about: back only with the suit charged again, not
		// into its fire with the armor half gone, each hit on the health)
		else if (s_bot.retreating && ((Now() - s_bot.heavyHitAt > 20.0f || energy >= 120.0f) && ((health > 0.95f && Now() - s_bot.retreatSince > 6.0f)
			|| (health > 0.85f && energy > 80.0f && Now() - s_bot.retreatSince > 12.0f)) || Now() - s_bot.retreatSince > 40.0f))
		{
			s_bot.cloakEscape = false;
			s_bot.proneUntil = 0.0f;
			s_bot.retreating = false;
			s_bot.recoveredAt = Now();
			Event("recovered (health %d): back to it", pMe->GetHealth());
		}
		if (!s_bot.retreating)
			return false;
		const Vec3 enemyAim = AimPoint(pEnemy->GetEntity());
		const bool seen = Visible(pMe, eye, pEnemy->GetEntity(), enemyAim);
		// one in sight within 30 m: shot at while it falls back, not run from
		// with its back to him (it fell back past a shotgunner without a shot;
		// riflemen 12-27 m off hit it 8 times in a row with no shot back, the
		// user's game of 2026-10-18). Cloaked, only one right in its way (a
		// shot gives it away)
		// (a cloak on its way counts too: SuitStep switches to it only at the
		// end of this think, and a shot at 20 m took the cloak back at once)
		const bool cloakedNow = pSuit && pSuit->GetMode() == NANOMODE_CLOAK;
		// (a cloak wanted that did not come: SuitStep gives it up after 3 s)
		const float enemyDist = enemyAim.GetDistance(eye);
		const bool engage = seen && enemyDist < (cloakedNow || s_bot.cloakEscape ? 8.0f : 30.0f) && !pEnemy->GetLinkedVehicle();
		// (over 10 m a moving rifle's spread holds every burst: it stops for
		// 0.7 s of every 2 to shoot, then goes on; it moves first, out of the
		// fire that started the retreat, and stops only while the spread
		// holds a burst back)
		const bool standToShoot = engage && enemyDist > 10.0f && Now() - s_bot.spreadHeldAt < 0.3f
			&& fmodf(Now() - s_bot.retreatSince, 2.0f) >= 1.3f;
		if (engage)
			FightStep(pMe, eye, pEnemy);
		else
			Fire(pMe, false);
		s_bot.fightBack += gEnv->pTimer->GetFrameTime();
		if (!s_bot.haveCover)
			AskCover(pEnemy);
		if (s_bot.haveCover)
		{
			// (cloaked it does not sprint: the energy went in two seconds so)
			const bool cloaked = pSuit && pSuit->GetMode() == NANOMODE_CLOAK;
			if (s_bot.cloakEscape && s_bot.cloakGoal.IsZero())
				s_bot.cloakGoal = s_bot.cover;
			// (stuck short of its cover in their fire: down where it is. It
			// stood 2 m before one, blocked at the knees, and took 13 hits of
			// a machine gun; the autotester's companion-crossfire-camp)
			// (along the way to it: round a rock the straight line grows first)
			const float toCover = Dist2D(s_bot.cover, pos) <= 0.8f ? 0.0f : WayLeft(pos, s_bot.cover);
			if (toCover < s_bot.coverBest - 0.3f || toCover <= 0.0f)
			{
				s_bot.coverBest = toCover;
				s_bot.coverBestAt = Now();
			}
			else if (Now() - s_bot.coverBestAt > 2.0f && Now() - s_bot.hitAt < 2.0f)
			{
				Event("cannot get to its cover (%.0f m) under fire: down where it is", toCover);
				s_bot.haveCover = false;
				s_bot.coverAskAt = Now() + 3.0f;
				s_bot.retreatSince = min(s_bot.retreatSince, Now() - 4.0f);
				return true;
			}
			if (!MoveTo(pMe, s_bot.cover, 0.8f, !cloaked && !s_bot.cloakEscape, false, ""))
			{
				s_bot.coverReachedAt = -1.0f;
				if (standToShoot)
				{
					s_ctl.moveDir.zero();
					s_ctl.sprint = false;
				}
				// (shooting one close by: its aim stays on him, it strafes)
				if (!engage)
				{
					s_ctl.look = true;
					s_ctl.yaw = Yaw(s_ctl.moveDir.GetLengthSquared() > 0.01f ? s_ctl.moveDir : s_bot.cover - pos);
					s_ctl.pitch = 0.0f;
					s_ctl.turnRate = 6.0f;
				}
				s_bot.doing.Format("running to cover (health %d, %.0f m)", pMe->GetHealth(), Dist2D(s_bot.cover, pos));
			}
			else
			{
				// in cover: down low (lying down where crouching does not hide
				// it), watching where the danger is (out of their sight the
				// armor again: it heals faster in it; SuitStep ends the cloak)
				if (s_bot.coverReachedAt < 0.0f)
					s_bot.coverReachedAt = Now();
				const bool lying = s_bot.coverProne || Now() < s_bot.proneUntil;
				s_ctl.crouch = !lying;
				s_ctl.prone = lying;
				LookAt(eye, enemyAim, 3.0f);
				s_bot.doing.Format("in cover, recovering (health %d%s)", pMe->GetHealth(), lying ? ", lying down" : "");
				// (a soldier on foot in sight, itself not nearly dead: shot at
				// from down there, not a heavy gun's vehicle; it sat out most of
				// the fight recovering, two hits in two minutes. Not only when
				// nobody hit it for a second: one hitting it once a second kept
				// that gate shut for good)
				if (seen && !pEnemy->GetLinkedVehicle() && health > 0.4f
					&& !(pSuit && pSuit->GetMode() == NANOMODE_CLOAK))
				{
					FightStep(pMe, eye, pEnemy);
					s_bot.doing.Format("in cover, recovering, shooting %s (health %d)", pEnemy->GetEntity()->GetName(), pMe->GetHealth());
				}
				// no cover from those shooting at it there: lying down, if that
				// hides it, else another cover. Only once it is down there (on
				// arrival it still stood, the cover was for a crouching head:
				// it hopped from cover to cover 4 m at a time away from the
				// fight), and only for one who hit it a moment ago and sees its
				// head now, not for the one it aims at
				const float eyeUp = eye.z - pos.z;
				// (a heavy gun's rounds through what hid it: no cover here)
				if (Now() - s_bot.heavyHitAt < 1.0f && Now() - s_bot.coverReachedAt > 0.5f)
				{
					s_bot.haveCover = false;
					s_bot.coverAskAt = 0.0f;
					Event("a heavy gun hits it in its cover: another one");
					return true;
				}
				// (down there: a head still on its way down is not lying yet)
				const bool settled = Now() - s_bot.coverReachedAt > 0.7f && eyeUp < (lying ? 0.7f : 1.3f);
				if (settled && Now() - s_bot.hitAt < 1.5f)
				{
					std::vector<IActor*> shooters;
					RecentShooters(1.5f, shooters);
					if (!shooters.empty() && !HiddenFrom(pMe, pos, eyeUp, shooters))
					{
						if (!lying && HiddenFrom(pMe, pos, 0.4f, shooters))
						{
							s_bot.proneUntil = Now() + 6.0f;
							s_bot.coverReachedAt = Now();
							Event("shot in cover by %s: lies down there", shooters[0]->GetEntity()->GetName());
						}
						else if (Dist2D(pos, s_bot.retreatFrom) > 14.0f)
						{
							// (two moves in this fall back already: down where it is)
							s_bot.proneUntil = Now() + 6.0f;
							s_bot.coverReachedAt = Now();
							Event("shot in cover by %s, %.0f m from where it fell back: stays down there", shooters[0]->GetEntity()->GetName(), Dist2D(pos, s_bot.retreatFrom));
						}
						else
						{
							s_bot.haveCover = false;
							s_bot.coverAskAt = 0.0f;
							Event("shot in cover by %s: another one", shooters[0]->GetEntity()->GetName());
						}
					}
				}
			}
			return true;
		}
		// no cover known yet and far enough from where it fell back, or held
		// to a downed teammate: down where it is, shooting back from there,
		// asking again (it sprinted 18 m into a machine gun's fire toward the
		// host 285 m off, the user's game of 2026-10-21)
		const bool ranEnough = Dist2D(pos, s_bot.retreatFrom) > 8.0f || Now() - s_bot.retreatSince > 3.5f
			|| (s_tetherRadius > 0.0f && Dist2D(pos, s_tether) > max(s_tetherRadius, Dist2D(s_bot.retreatFrom, s_tether) + 1.0f));
		if ((underFire || seen) && ranEnough)
		{
			s_ctl.moveDir.zero();
			s_ctl.sprint = false;
			s_ctl.prone = true;
			// (it stays down: lying it is out of their sight, and up again it
			// was in it; down and up every second, the review)
			s_bot.proneUntil = Now() + 4.0f;
			s_bot.proneSeconds += gEnv->pTimer->GetFrameTime();
			if (!engage)
				LookAt(eye, enemyAim, 3.0f);
			s_bot.doing.Format("no cover close by: lying down, shooting back (health %d)", pMe->GetHealth());
		}
		else if (underFire || seen)
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
			// (not toward one far off: that was a run across the field)
			if (IActor* pLeader = HostPlayer())
			{
				Vec3 toLeader = pLeader->GetEntity()->GetWorldPos() - pos;
				toLeader.z = 0;
				const Vec3 danger = pEnemy->GetEntity()->GetWorldPos();
				if (pLeader->GetHealth() > 0 && toLeader.GetLength() > 6.0f && toLeader.GetLength() < 40.0f
					&& Dist2D(pLeader->GetEntity()->GetWorldPos(), danger) > Dist2D(pos, danger))
					away = (away + toLeader.GetNormalized() * 0.6f).GetNormalizedSafe(away);
			}
			// (held to a downed teammate: sideways round him, not away)
			if (s_tetherRadius > 0.0f)
			{
				Vec3 toBody = s_tether - pos;
				toBody.z = 0;
				if (toBody.GetLength() > s_tetherRadius * 0.6f)
					away = (away + toBody.GetNormalizedSafe(away)).GetNormalizedSafe(away);
			}
			s_ctl.moveDir = standToShoot ? Vec3(ZERO) : Steer(pMe, pos, away);
			// (shooting one close by: it backs off with its aim on him, not
			// running off with its back to him, uncloaked for that shot)
			if (engage)
				s_ctl.sprint = false;
			else
			{
				s_ctl.sprint = !(pSuit && pSuit->GetMode() == NANOMODE_CLOAK);
				s_ctl.look = true;
				s_ctl.yaw = Yaw(s_ctl.moveDir);
				s_ctl.pitch = 0.0f;
				s_ctl.turnRate = 6.0f;
			}
			s_bot.doing.Format("falling back (health %d)", pMe->GetHealth());
		}
		else
		{
			// (lying down where it lay down: out of sight only so)
			s_ctl.prone = Now() < s_bot.proneUntil;
			s_ctl.crouch = !s_ctl.prone;
			LookAt(eye, enemyAim, 3.0f);
			// (out of their sight: the armor again, it recharges and heals in
			// it; cloaked the energy only went down)
			if (Now() - s_bot.hitAt > 2.0f && !s_bot.cloakForRevive)
				s_bot.cloakEscape = false;
			s_bot.doing.Format("out of sight, recovering (health %d%s)", pMe->GetHealth(), s_ctl.prone ? ", lying down" : "");
		}
		return true;
	}

	// the weapon's sights: up while FightStep wants them (up from cover, a
	// target far off) and it does not move this frame; down otherwise (in
	// the sights it walks at a third of its speed). Not up and down faster
	// than every 0.8 s
	void ZoomStep(CPlayer* pMe)
	{
		IItem* pItem = pMe && !pMe->GetLinkedVehicle() ? pMe->GetCurrentItem() : 0;
		IWeapon* pWeapon = pItem ? pItem->GetIWeapon() : 0;
		if (!pWeapon)
			return;
		// (down at once to run: a third of its speed to cover otherwise)
		const bool moves = s_ctl.moveDir.GetLengthSquared() >= 0.01f || s_ctl.sprint;
		if (Now() < s_bot.zoomAt && !moves)
			return;
		static ICVar* pZoom = gEnv->pConsole->GetCVar("coop_agent_zoom");
		const bool want = s_bot.zoomWant && (!pZoom || pZoom->GetIVal()) && !moves && pMe->GetHealth() > 0;
		if (want && !pWeapon->IsZoomed() && !pWeapon->IsZooming())
		{
			pWeapon->StartZoom(pMe->GetEntityId(), 1);
			s_bot.zoomAt = Now() + 0.8f;
		}
		else if (!want && (pWeapon->IsZoomed() || pWeapon->IsZooming()))
		{
			pWeapon->StopZoom(pMe->GetEntityId());
			s_bot.zoomAt = Now() + 0.8f;
		}
	}

	// the suit's mode: armor (the only one that takes hits for it), but for
	// short moves another one and back: cloaked to get away (no shot while
	// cloaked: a shot uncloaks), speed to catch up with the one it follows
	// far off or to a downed teammate with nobody about, strength for a
	// jump onto what it can not step onto and for a blow at one right next
	// to it. Never in a vehicle, never down
	void SuitStep(CPlayer* pMe, const Vec3& pos, IActor* pLeader, IActor* pEnemy)
	{
		CNanoSuit* pSuit = pMe->GetNanoSuit();
		if (!pSuit || pMe->GetHealth() <= 0)
			return;
		static float s_switchAt = 0.0f;
		const float now = Now();
		const float energy = pSuit->GetSuitEnergy();
		const ENanoMode mode = pSuit->GetMode();
		static const char* const s_modes[] = { "speed", "strength", "cloak", "armor" };
		// (changed by something else: the suit out of energy, a fall back
		// ended, the agent; logged too, the cloak's end went unseen so)
		static int s_seenMode = -1;
		if (s_seenMode >= 0 && s_seenMode != (int)mode)
		{
			CryLogAlways("[CoopAgent] suit: %s -> %s (not its choice; energy %.0f, health %d)", (unsigned)s_seenMode < 4 ? s_modes[s_seenMode] : "?",
				(unsigned)mode < 4 ? s_modes[mode] : "?", energy, pMe->GetHealth());
			// (the suit itself let go of the cloak, out of energy below 20:
			// one escape, one cloak; else cloaked again at 25 for a moment, too
			// short for the soldiers' eyes, and again)
			if (s_seenMode == NANOMODE_CLOAK)
				s_bot.cloakEscape = false;
		}
		s_seenMode = (int)mode;
		// (the AI agent chose a mode: kept a while)
		if (now < s_bot.suitHoldUntil)
			return;
		// (as MoveTo arms a strength jump: an enemy seen lately, not one it
		// only remembers; else a jump it armed was never made)
		const bool threatened = s_bot.retreating || now - s_bot.hitAt < 6.0f || (s_bot.enemy && now - s_bot.enemySeenAt < 8.0f);
		// (no speed while a fight goes on round it: it left its cover in
		// speed toward a downed teammate 17 m off, a rifleman 20 m away hit
		// it at once, the armor's energy half gone; the user's game of
		// 2026-10-21. One who shot it in the last 20 s and is still about
		// within 80 m counts, so does an enemy seen in the last 15 s)
		// (and the 5 s after its own revive or a load: hits while it cannot be
		// hurt come with no damage indicator, it ran in speed into them)
		bool inFight = threatened || now - s_bot.hitAt < 10.0f || (s_bot.enemy && now - s_bot.enemySeenAt < 15.0f) || now - s_bot.freshAt < 5.0f;
		for (std::map<EntityId, SThreat>::const_iterator t = s_threats.begin(); t != s_threats.end() && !inFight; ++t)
			if (now - t->second.last < 20.0f)
				if (IActor* pShooter = ActorOf(t->first))
					inFight = pShooter->GetHealth() > 0 && Hostile(pShooter) && pShooter->GetEntity()->GetWorldPos().GetDistance(pos) < 80.0f;
		ENanoMode want = NANOMODE_DEFENSE;
		const char* why = "the default";
		IActor* pDowned = s_bot.order == eO_Hold || s_bot.order == eO_Goto ? 0 : DownedTeammate(pMe, pos, 200.0f);
		// (a teammate down only a moment, or seen down right after its own
		// load or revive, is the old state of his player: twice after an
		// everybody-down load it switched to speed for the host who was up)
		if (pDowned)
		{
			std::map<EntityId, float>::const_iterator d = s_bot.downAt.find(pDowned->GetEntityId());
			if (now - s_bot.freshAt < 3.0f || (d != s_bot.downAt.end() && now - d->second < 1.0f))
				pDowned = 0;
		}
		// the cloaked move: over at its end, or when one comes close it has
		// to shoot (a shot from the cloak takes all the energy), or when the
		// suit did not cloak, or after long
		if (s_bot.cloakEscape)
		{
			const char* over = 0;
			string overWhy;
			IActor* pFoe = ActorOf(s_bot.enemy);
			// (within 6 m a soldier sees through the cloak)
			const bool foeClose = pFoe && pFoe->GetHealth() > 0 && now - s_bot.enemySeenAt < 0.5f
				&& pFoe->GetEntity()->GetWorldPos().GetDistance(pos) < 6.0f;
			// (in whatever cover it is now: the cover may have changed on the
			// way; a revive's cloak ends when nobody is down any more, Think)
			if (!s_bot.cloakForRevive && s_bot.haveCover && Dist2D(s_bot.cover, pos) < 1.5f)
				over = "in its cover";
			else if (!s_bot.cloakForRevive && !s_bot.cloakGoal.IsZero() && Dist2D(s_bot.cloakGoal, pos) < 1.5f)
				over = "there";
			else if (foeClose)
			{
				overWhy.Format("%s %.0f m off: shoots him", pFoe->GetEntity()->GetName(), pFoe->GetEntity()->GetWorldPos().GetDistance(pos));
				over = overWhy.c_str();
			}
			else if (s_bot.cloakGoal.IsZero() && !s_bot.haveCover && now - s_bot.cloakSince > 3.0f)
				over = "no cover named";
			else if (mode != NANOMODE_CLOAK && now - s_bot.cloakSince > 3.0f)
				over = "the suit did not cloak";
			else if (!s_bot.cloakForRevive && !s_bot.cloakGoal.IsZero() && now - s_bot.cloakBestAt > 2.5f)
				over = "it gets no closer to where it went";
			else if (now - s_bot.cloakSince > 15.0f)
				over = "15 s";
			if (over)
			{
				s_bot.cloakEscape = false;
				CryLogAlways("[CoopAgent] the cloaked move is over (%s; energy %.0f)", over, energy);
			}
		}
		if (s_bot.cloakEscape && !s_bot.cloakGoal.IsZero() && Dist2D(s_bot.cloakGoal, pos) < s_bot.cloakBest - 0.3f)
		{
			s_bot.cloakBest = Dist2D(s_bot.cloakGoal, pos);
			s_bot.cloakBestAt = now;
		}
		if (mode == NANOMODE_CLOAK)
			s_bot.cloakSeconds += gEnv->pTimer->GetFrameTime();
		if (pMe->GetLinkedVehicle())
			why = "in a vehicle";
		else if (s_bot.strengthJumpAt >= 0.0f && now - s_bot.strengthJumpAt < 2.5f && energy >= 50.0f && !threatened)
		{
			want = NANOMODE_STRENGTH;
			why = "a jump onto what is in the way";
		}
		else if (now < s_bot.meleeWantUntil && energy >= 40.0f)
		{
			want = NANOMODE_STRENGTH;
			why = "a blow at one right next to it";
		}
		else if (s_bot.cloakEscape && energy > (mode == NANOMODE_CLOAK ? 21.0f : 25.0f))
		{
			want = NANOMODE_CLOAK;
			why = s_bot.cloakForRevive ? "to a downed teammate out of their sight" : s_bot.retreating ? "getting away out of their sight" : "to cover out of their sight";
		}
		else if (!inFight && pDowned && Dist2D(pDowned->GetEntity()->GetWorldPos(), pos) > 15.0f && energy > (mode == NANOMODE_SPEED ? 40.0f : 100.0f))
		{
			want = NANOMODE_SPEED;
			why = "to a downed teammate";
		}
		else if (!inFight && pLeader && (s_bot.order == eO_Free || s_bot.order == eO_Follow) && pLeader->GetHealth() > 0
			&& Dist2D(pLeader->GetEntity()->GetWorldPos(), pos) > (mode == NANOMODE_SPEED ? 12.0f : 30.0f) && energy > (mode == NANOMODE_SPEED ? 50.0f : 120.0f))
		{
			want = NANOMODE_SPEED;
			why = "catching up";
		}
		// (a mode the suit refused a moment ago: armor meanwhile)
		if ((int)want == s_refusedMode && now < s_refusedUntil)
		{
			want = NANOMODE_DEFENSE;
			why = "the default (that mode refused)";
		}
		if (want == mode)
			return;
		// (into the cloak with the trigger let go: a shot already due would
		// be fired from the cloak, the review; next frame)
		if (want == NANOMODE_CLOAK && s_bot.attackHeld)
		{
			Fire(pMe, false);
			return;
		}
		// (not to and fro: a while between switches, but armor at once when
		// shot at)
		if (now < s_switchAt && !(want == NANOMODE_DEFENSE && threatened) && !(want == NANOMODE_STRENGTH && now < s_bot.meleeWantUntil))
			return;
		// (leaving the cloak itself, at 21: that escape is over, one cloak)
		if (mode == NANOMODE_CLOAK && want != NANOMODE_CLOAK && want != NANOMODE_STRENGTH)
			s_bot.cloakEscape = false;
		if (pSuit->SetMode(want))
		{
			if (mode == NANOMODE_CLOAK)
				s_bot.uncloakAt = now;
			s_seenMode = (int)want;
			s_switchAt = now + 1.5f;
			++s_bot.suitSwitches;
			CryLogAlways("[CoopAgent] suit: %s -> %s (%s; energy %.0f, health %d)", (unsigned)mode < 4 ? s_modes[mode] : "?", (unsigned)want < 4 ? s_modes[want] : "?",
				why, energy, pMe->GetHealth());
			if (want == NANOMODE_SPEED)
				s_ctl.sprint = true;
		}
		else
		{
			// (refused: a mode the campaign took away; it never cloaked so)
			s_refusedMode = (int)want;
			s_refusedUntil = now + 10.0f;
			if (want != NANOMODE_DEFENSE && mode != NANOMODE_DEFENSE && threatened && pSuit->SetMode(NANOMODE_DEFENSE))
			{
				s_seenMode = NANOMODE_DEFENSE;
				s_switchAt = now + 1.5f;
				++s_bot.suitSwitches;
				CryLogAlways("[CoopAgent] suit: %s -> armor (%s refused; energy %.0f, health %d)", (unsigned)mode < 4 ? s_modes[mode] : "?",
					(unsigned)want < 4 ? s_modes[want] : "?", energy, pMe->GetHealth());
			}
			static float s_refusedAt = -100.0f;
			if (now - s_refusedAt > 10.0f)
			{
				s_refusedAt = now;
				CryLogAlways("[CoopAgent] suit: %s refused (%s; modes given: speed %d strength %d cloak %d armor %d; energy %.0f)",
					(unsigned)want < 4 ? s_modes[want] : "?", why, (int)pSuit->IsModeActive(NANOMODE_SPEED), (int)pSuit->IsModeActive(NANOMODE_STRENGTH),
					(int)pSuit->IsModeActive(NANOMODE_CLOAK), (int)pSuit->IsModeActive(NANOMODE_DEFENSE), energy);
			}
		}
	}

	// something to jump up onto right ahead, too high to step onto, low
	// enough for a strength jump, room to stand on it: how high (0: none)
	float LedgeAhead(CPlayer* pMe, const Vec3& pos, const Vec3& dir, bool* pTerrain)
	{
		IPhysicalEntity* pSkip = pMe->GetEntity()->GetPhysics();
		const int ents = ent_static | ent_terrain | ent_rigid | ent_sleeping_rigid;
		ray_hit hit;
		if (!gEnv->pPhysicalWorld->RayWorldIntersection(pos + Vec3(0, 0, 0.6f), dir * 1.2f, ents, WALK_GEOM, &hit, 1, &pSkip, pSkip ? 1 : 0))
			return 0.0f;
		// (a slope it walks up is no ledge: up it it jumped and fell back three
		// times, at the top of the bank by the bridge, 2026-10-23)
		if (hit.n.z > 0.77f)
			return 0.0f;
		if (pTerrain)
			*pTerrain = hit.bTerrain != 0;
		const Vec3 over = pos + dir * (hit.dist + 0.6f);
		ray_hit top;
		if (!gEnv->pPhysicalWorld->RayWorldIntersection(over + Vec3(0, 0, 3.6f), Vec3(0, 0, -3.6f), ents, WALK_GEOM, &top, 1, &pSkip, pSkip ? 1 : 0))
			return 0.0f;
		const float h = top.pt.z - pos.z;
		if (h < 0.9f || h > 2.8f)
			return 0.0f;
		ray_hit head;
		if (gEnv->pPhysicalWorld->RayWorldIntersection(top.pt + Vec3(0, 0, 0.1f), Vec3(0, 0, 1.9f), ents, WALK_GEOM, &head, 1, &pSkip, pSkip ? 1 : 0))
			return 0.0f;
		return h;
	}

	// down low it would lose sight of him (a low wall before it: crouched
	// behind it the nest's gunner was out of sight, no shot for a minute;
	// 2026-10-21)
	bool CrouchHidesHim(CPlayer* pMe, IActor* pEnemy)
	{
		const Vec3 eye = EyePos(pMe);
		const float above = eye.z - pMe->GetEntity()->GetWorldPos().z;
		const bool crouched = above < 1.3f;
		const Vec3 standing = crouched ? eye + Vec3(0, 0, 1.6f - above) : eye;
		const Vec3 low = crouched ? eye : eye - Vec3(0, 0, max(0.0f, above - 1.0f));
		const Vec3 aim = AimPoint(pEnemy->GetEntity());
		return Visible(pMe, standing, pEnemy->GetEntity(), aim) && !Visible(pMe, low, pEnemy->GetEntity(), aim);
	}

	// standing its ground in a fight: side steps now and then, crouched in
	// between (a harder target than one standing still)
	void Dodge(const Vec3& pos, IActor* pEnemy)
	{
		if (s_ctl.moveDir.GetLengthSquared() > 0.01f)
			return;    // on its way somewhere anyway
		const float now = Now();
		// (a small target far off: still for the sights, a side step put
		// them down every time; crouched unless that hides him)
		CPlayer* pSelf = LocalPlayer();
		const bool crouchHides = pSelf && CrouchHidesHim(pSelf, pEnemy);
		if (now - s_bot.smallFarAt < 0.5f)
		{
			s_ctl.crouch = !crouchHides;
			return;
		}
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
					s_ctl.crouch = !crouchHides;
					return;
				}
			}
			s_ctl.moveDir = side;
		}
		else
			s_ctl.crouch = !crouchHides;
	}

	// where it aims now, the first thing hit is the enemy (or his vehicle):
	// no shots into a wall, a crate, a fence, a teammate
	// (aim: the point FightStep shoots at, his head when it sees it; the line
	// to his chest ran into a mounted gun's body while the head was free)
	bool LineOfFire(CPlayer* pMe, const Vec3& eye, IActor* pEnemy, const Vec3& aim)
	{
		IPhysicalEntity* skip[2];
		int n = 0;
		if (IPhysicalEntity* p = pMe->GetEntity()->GetPhysics())
			skip[n++] = p;
		if (IItem* pItem = pMe->GetCurrentItem())
			if (IPhysicalEntity* p = pItem->GetEntity()->GetPhysics())
				skip[n++] = p;
		const Vec3 look = pMe->GetViewRotation().GetColumn1();
		const float range = aim.GetDistance(eye) + 2.0f;
		ray_hit hit;
		if (!FirstBlock(eye, look * range, ent_all, skip, n, hit))
			return false;    // aimed at nothing (the enemy is not where it aims)
		IPhysicalEntity* pEnemyPhys = pEnemy->GetEntity()->GetPhysics();
		// where the bullet leaves (the gun, lower than the eyes): its line
		// must be free too. Up from a cover that hid a crouching head the
		// eyes saw him over its top, the gun did not: 16 bursts aimed right
		// at a soldier 21 m off, no hit, all into its own cover
		{
			IItem* pItem = pMe->GetCurrentItem();
			IWeapon* pWeapon = pItem ? pItem->GetIWeapon() : 0;
			IFireMode* pMode = pWeapon ? pWeapon->GetFireMode(pWeapon->GetCurrentFireMode()) : 0;
			if (pMode)
			{
				const Vec3 muzzle = pMode->GetFiringPos(aim);
				const Vec3 toAim = aim - muzzle;
				ray_hit gunHit;
				if (muzzle.GetDistance(eye) < 2.0f && toAim.GetLengthSquared() > 1.0f
					&& FirstBlock(muzzle, toAim, ent_all, skip, n, gunHit) && !HitIs(gunHit, pEnemy->GetEntity())
					&& gunHit.dist < toAim.GetLength() - 0.8f)
				{
					IEntity* pGunHit = gunHit.pCollider ? gEnv->pEntitySystem->GetEntityFromPhysics(gunHit.pCollider) : 0;
					IActor* pGunHitActor = pGunHit ? g_pGame->GetIGameFramework()->GetIActorSystem()->GetActor(pGunHit->GetId()) : 0;
					if (!(pGunHitActor && Hostile(pGunHitActor)))
					{
						static ICVar* pGunTrace = gEnv->pConsole->GetCVar("coop_test_shottrace");
						static float s_gunToldAt = 0.0f;
						if (pGunTrace && pGunTrace->GetIVal() && Now() - s_gunToldAt > 1.0f)
						{
							s_gunToldAt = Now();
							CryLogAlways("[CoopAgent] the gun's line at %s blocked %.1f m off (of %.0f) by %s; the gun %.2f m below the eyes", pEnemy->GetEntity()->GetName(),
								gunHit.dist, toAim.GetLength(), pGunHit ? pGunHit->GetName() : "something static", eye.z - muzzle.z);
						}
						return false;
					}
				}
			}
		}
		// (a vehicle shot to pieces close by in the line: never. It shot the
		// burning jeep 4 m from it, the jeep blew up and took it down, the
		// user's game of 2026-10-21)
		{
			IEntity* pFirst = hit.pCollider ? gEnv->pEntitySystem->GetEntityFromPhysics(hit.pCollider) : 0;
			IVehicle* pWreck = pFirst ? g_pGame->GetIGameFramework()->GetIVehicleSystem()->GetVehicle(pFirst->GetId()) : 0;
			if (pWreck && pWreck != pEnemy->GetLinkedVehicle() && !pWreck->IsDestroyed() && pWreck->GetDamageRatio() > 0.25f
				&& pWreck->GetEntity()->GetWorldPos().GetDistance(eye) < 15.0f)
				return false;
		}
		if (HitIs(hit, pEnemy->GetEntity()))
			return true;
		// (his vehicle is no shot at him: at the truck's gunner 10 of 12 hits
		// went into the truck, its armor took them all. One at a vehicle's
		// gun only when the line reaches him himself: 0.8 m from his head is
		// the gun's shield and the cab; the same behind a mounted gun)
		if (pEnemy->GetLinkedVehicle() || MountedGunOf(pEnemy))
		{
			// testing (coop_test_shottrace 1): what the line meets before him
			static float s_gunnerBlockAt = 0.0f;
			static ICVar* pGunnerTrace = gEnv->pConsole->GetCVar("coop_test_shottrace");
			if (pGunnerTrace && pGunnerTrace->GetIVal() && Now() >= s_gunnerBlockAt)
			{
				s_gunnerBlockAt = Now() + 1.0f;
				IEntity* pMet = hit.pCollider ? gEnv->pEntitySystem->GetEntityFromPhysics(hit.pCollider) : 0;
				CryLogAlways("[CoopAgent] at %s behind his gun: the line meets %s at %.1f m, %.2f m from the aim point", pEnemy->GetEntity()->GetName(),
					pMet ? pMet->GetName() : "something static", hit.dist, hit.pt.GetDistance(aim));
			}
			return false;
		}
		// the hit is right at him (his weapon, a part of him)
		if (hit.pt.GetDistance(aim) < 0.8f)
			return true;
		// another enemy in the way: the shot hits an enemy all the same (in a
		// crowd of soldiers it held its fire for minutes, 2026-10-15)
		IEntity* pHit = hit.pCollider ? gEnv->pEntitySystem->GetEntityFromPhysics(hit.pCollider) : 0;
		IActor* pHitActor = pHit ? g_pGame->GetIGameFramework()->GetIActorSystem()->GetActor(pHit->GetId()) : 0;
		if (pHitActor && Hostile(pHitActor))
			return true;
		// testing (coop_test_shottrace 1): what is in the line of fire
		static float s_footBlockLogAt = 0.0f;
		static ICVar* pShotTrace = gEnv->pConsole->GetCVar("coop_test_shottrace");
		if (pShotTrace && pShotTrace->GetIVal() && Now() >= s_footBlockLogAt)
		{
			s_footBlockLogAt = Now() + 1.0f;
			CryLogAlways("[CoopAgent] on foot at %s, the line of fire blocked %.1f m off (of %.0f) by %s", pEnemy->GetEntity()->GetName(), hit.dist,
				aim.GetDistance(eye), pHit ? pHit->GetName() : hit.pCollider && hit.pCollider->GetType() == PE_STATIC ? "something static" : "terrain or nothing known");
		}
		return false;
	}

	// fighting from cover: to it, then up shooting a while, down a while
	// (FightStep aims and shoots first); false: no cover known
	bool FightFromCover(CPlayer* pMe, const Vec3& pos, IActor* pEnemy)
	{
		if (!s_bot.haveCover)
			return false;
		// (cloaked it walks: a sprint took the energy in two seconds)
		if (!MoveTo(pMe, s_bot.cover, 0.8f, Dist2D(s_bot.cover, pos) > 4.0f && !s_bot.cloakEscape, false, ""))
		{
			s_bot.coverReachedAt = -1.0f;
			s_bot.doing.Format("to cover (%.0f m%s), fighting %s", Dist2D(s_bot.cover, pos), s_bot.cloakEscape ? ", cloaked" : "", pEnemy->GetEntity()->GetName());
			return true;
		}
		if (s_bot.coverReachedAt < 0.0f)
			s_bot.coverReachedAt = Now();
		const float now = Now();
		const Vec3 eye = EyePos(pMe);
		const bool seen = Visible(pMe, eye, pEnemy->GetEntity(), AimPoint(pEnemy->GetEntity()));
		int clip = -1, reserve = -1;
		IWeapon* pWeapon = CurrentWeapon(pMe, &clip, &reserve);
		const int clipSize = pWeapon && pWeapon->GetFireMode(pWeapon->GetCurrentFireMode()) ? pWeapon->GetFireMode(pWeapon->GetCurrentFireMode())->GetClipSize() : 0;
		const bool lowClip = clipSize > 0 && clip >= 0 && clip < clipSize * 3 / 10 && reserve > 0;
		if (s_bot.peekUp)
		{
			// up: standing, or leaning out round its cover's side, firing
			// short bursts at him (FightStep aims and fires); down again when
			// shot, the clip low, him out of sight a moment, or up long
			if (seen)
				s_bot.peekSeenAt = now;
			const bool shot = now - s_bot.hitAt < 0.3f && now - s_bot.peekSince > 0.2f;
			if (shot)
				++s_bot.peekHits;
			if (shot || lowClip || (now - s_bot.peekSeenAt > 0.7f && now - s_bot.peekSince > 0.7f) || now - s_bot.peekSince > 2.5f)
			{
				s_bot.peekUp = false;
				s_bot.peekSince = now;
				Fire(pMe, false);
			}
		}
		else
		{
			// down: crouched out of their sight (lying down where only that
			// hides it), a reload, then up again, a while after the last hit
			// (not up into the same fire)
			const bool lying = s_bot.coverProne || now < s_bot.proneUntil;
			s_ctl.crouch = !lying;
			s_ctl.prone = lying;
			if (lying)
				s_bot.proneSeconds += gEnv->pTimer->GetFrameTime();
			Fire(pMe, false);
			if (lowClip && reserve > 0 && now > s_bot.reloadAt)
			{
				Press(pMe, g_pGame->Actions().reload, true);
				Press(pMe, g_pGame->Actions().reload, false);
				s_bot.reloadAt = now + 2.5f;
				++s_bot.hiddenReloads;
			}
			// shot even down there: no cover from it here (lying down first,
			// when that hides it from him)
			std::vector<IActor*> hitBy;
			if (now - s_bot.hitAt < 0.3f && now - s_bot.peekSince > 1.0f && !lying && now - s_bot.heavyHitAt > 1.0f
				&& (RecentShooters(0.5f, hitBy), !hitBy.empty()) && HiddenFrom(pMe, pos, 0.4f, hitBy))
			{
				s_bot.proneUntil = now + 6.0f;
				s_bot.peekSince = now;
				Event("shot in cover by %s: lies down there", hitBy[0]->GetEntity()->GetName());
			}
			else if (now - s_bot.hitAt < 0.3f && now - s_bot.peekSince > 1.0f)
			{
				s_bot.haveCover = false;
				s_bot.coverAskAt = 0.0f;
				// (a heavy gun through it: away out of its sight, cloaked if it
				// can, not across the open to the next cover in its fire: it
				// went down so at the bridge)
				if (now - s_bot.heavyHitAt < 1.0f)
				{
					s_bot.retreatNowAt = now;
					Event("shot in cover by a heavy gun: falls back out of its sight");
				}
				else
					Event("shot in cover: another one");
			}
			const float down = now - s_bot.peekSince;
			if (down > 1.2f + cry_frand() * 0.6f && (now - s_bot.hitAt > 1.5f || down > 6.0f) && now > s_bot.reloadAt && !lowClip)
			{
				s_bot.peekUp = true;
				s_bot.peekSince = now;
				s_bot.peekSeenAt = now;
				++s_bot.peeks;
				if (s_bot.coverPeek == 1 || s_bot.coverPeek == 2)
					++s_bot.leanPeeks;
			}
		}
		if (s_bot.peekUp)
		{
			// leaning out where the cover's side gives a shot; else standing up
			if (s_bot.coverPeek == 1 || s_bot.coverPeek == 2)
			{
				s_ctl.lean = s_bot.coverPeek == 1 ? -1 : 1;
				s_ctl.crouch = false;
			}
		}
		s_bot.doing.Format("in cover (%s), fighting %s", s_bot.peekUp ? (s_ctl.lean ? (s_ctl.lean < 0 ? "leaning out left" : "leaning out right") : "up") : "down",
			pEnemy->GetEntity()->GetName());
		return true;
	}

	// how the engine updates a vehicle in this game (for the traces): a client
	// updates its seats, gun and engine only while it counts as seen or near
	string VehicleUpdateInfo(IVehicle* pVehicle)
	{
		IGameObject* pGO = pVehicle->GetGameObject();
		IEntity* pEntity = pVehicle->GetEntity();
		string s;
		s.Format("entity active %d hidden %d policy %d, game object update %d visible %d distant %d slots %d %d %d",
			(int)pEntity->IsActive(), (int)pEntity->IsHidden(), (int)pEntity->GetUpdatePolicy(), pGO ? (int)pGO->ShouldUpdate() : -1,
			pGO ? (int)pGO->IsProbablyVisible() : -1, pGO ? (int)pGO->IsProbablyDistant() : -1,
			pGO ? (int)pGO->GetUpdateSlotEnables(pVehicle, 0) : -1, pGO ? (int)pGO->GetUpdateSlotEnables(pVehicle, 1) : -1,
			pGO ? (int)pGO->GetUpdateSlotEnables(pVehicle, 2) : -1);
		return s;
	}

	// the turret's yaw is turned by its rotate actions alone. CryAction keeps
	// a client gunner's turret where it pointed in the world
	// (CVehiclePartBase::UpdateRotation's world-space hold, worldSpace="1" on
	// the LTV's turret), reckoned against its parent's frame; for a car's
	// body that frame stayed in the companion's game where the car first
	// stood: each frame the gun turned on by all the car had turned since.
	// Not seen driving straight; once the host turned, the gun spun ever
	// faster and stayed spinning, 2.5 rad a frame with the car at rest, its
	// own turn 0.14 at most: never on an enemy, never a shot (the user's game
	// of 2026-10-17 on 0.11.39; the autotester's companion-gunner-weave-
	// continue). A yaw joint on the body: its worldSpace (a float at +0xB0 of
	// CVehiclePartBase, build 6156; yawSpeed at +0x88) off, every frame it
	// sits at a gun, fire at will or not; the aim turns against the car's own
	// turn instead. A yaw joint on a turning joint (a tank's machine gun on
	// its turret) keeps the hold: that frame is right, and the hold also
	// covers the main turret's swing (the review). Returns whether the yaw
	// keeps it. Nothing reads worldSpace again: coop_test_turret_hold 1 (as
	// CryAction does, for testing) has to be set before it first takes the gun
	bool TurretHoldOff(IVehicle* pVehicle)
	{
		static ICVar* pTurretHold = gEnv->pConsole->GetCVar("coop_test_turret_hold");
		bool held = false;
		for (int i = 0, n = pVehicle->GetPartCount(); i < n; ++i)
		{
			IVehiclePart* pPart = pVehicle->GetPart((unsigned int)i);
			if (!pPart || pPart->GetType() != IVehiclePart::eVPT_AnimatedJoint || *(float*)((char*)pPart + 0x88) == 0.0f)
				continue;
			IVehiclePart* pParent = pPart->GetParent();
			const bool onJoint = pParent && pParent->GetType() == IVehiclePart::eVPT_AnimatedJoint && *(float*)((char*)pParent + 0x88) != 0.0f;
			if (onJoint || (pTurretHold && pTurretHold->GetIVal()))
				held = held || *(float*)((char*)pPart + 0xB0) > 0.0f;
			else
				*(float*)((char*)pPart + 0xB0) = 0.0f;
		}
		return held;
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
	// nothing else in the way. Which way a rotate action turns it is learnt
	// from how the view moved in the vehicle's own frame after the last one:
	// from the error alone a vehicle the host steered turned the view too, the
	// way was taken for wrong again and again and it never came onto anyone
	// (2026-10-15: "0 bursts fired, 102 turning to him", the error growing)
	void VehicleGunner(CPlayer* pMe, IVehicle* pVehicle)
	{
		static float s_signYaw = -1.0f, s_signPitch = -1.0f;
		static int s_voteYaw = 0, s_votePitch = 0;
		static float s_lastLocalYaw = 0.0f, s_lastLocalPitch = 0.0f, s_lastCmdYaw = 0.0f, s_lastCmdPitch = 0.0f;
		static EntityId s_gunVehicle = 0;
		const CCamera& cam = gEnv->pSystem->GetViewCamera();
		const Vec3 eye = cam.GetPosition();
		const Vec3 look = cam.GetViewdir().GetNormalizedSafe(Vec3(0, 1, 0));
		// the view in the vehicle's frame: what the rotate actions move
		const Vec3 local = Matrix33(pVehicle->GetEntity()->GetWorldTM()).GetInverted() * look;
		const float localYaw = Yaw(local), localPitch = Pitch(local);
		// (the car's own turn since the last frame, from its frame: the gun
		// turns along with it)
		static float s_lastCarYaw = 0.0f;
		const float carYaw = Yaw(pVehicle->GetEntity()->GetWorldTM().GetColumn1());
		float carTurn = Wrap(carYaw - s_lastCarYaw);
		s_lastCarYaw = carYaw;
		// (back at the gun after a while: the car's turn and the last action
		// are of then)
		static float s_gunAt = -100.0f;
		if (Now() - s_gunAt > 0.3f)
		{
			carTurn = 0.0f;
			s_lastCmdYaw = s_lastCmdPitch = 0.0f;
		}
		s_gunAt = Now();
		if (pVehicle->GetEntityId() != s_gunVehicle)
		{
			s_gunVehicle = pVehicle->GetEntityId();
			s_voteYaw = s_votePitch = 0;
			s_lastCmdYaw = s_lastCmdPitch = 0.0f;
			// (CryAction's rotate turret turns against the action's value,
			// both ways: from there, a gun learnt wrong in one car keeps it no
			// longer)
			s_signYaw = s_signPitch = -1.0f;
			carTurn = 0.0f;
		}
		else
		{
			// the last rotate action moved the view this way: that is the way
			// it turns (a few in a row decide, so a bump of the vehicle does not)
			const float dYaw = Wrap(localYaw - s_lastLocalYaw), dPitch = localPitch - s_lastLocalPitch;
			if (fabsf(s_lastCmdYaw) > 1.0f && fabsf(dYaw) > 0.001f)
			{
				s_voteYaw = clamp_tpl(s_voteYaw + (dYaw * s_lastCmdYaw > 0.0f ? 1 : -1), -4, 4);
				if (s_voteYaw >= 3 || s_voteYaw <= -3)
					s_signYaw = s_voteYaw > 0 ? 1.0f : -1.0f;
			}
			if (fabsf(s_lastCmdPitch) > 1.0f && fabsf(dPitch) > 0.001f)
			{
				s_votePitch = clamp_tpl(s_votePitch + (dPitch * s_lastCmdPitch > 0.0f ? 1 : -1), -4, 4);
				if (s_votePitch >= 3 || s_votePitch <= -3)
					s_signPitch = s_votePitch > 0 ? 1.0f : -1.0f;
			}
		}
		// testing (coop_test_guntrace 1): each frame, what it told the turret
		// and how the view moved in the vehicle's frame
		static ICVar* pGunTrace = gEnv->pConsole->GetCVar("coop_test_guntrace");
		if (pGunTrace && pGunTrace->GetIVal())
		{
			pe_status_dynamics dyn;
			IPhysicalEntity* pPhys = pVehicle->GetEntity()->GetPhysics();
			const float wz = pPhys && pPhys->GetStatus(&dyn) ? dyn.w.z : 0.0f;
			CryLogAlways("[CoopGun] cmd %.1f %.1f -> local d %.4f %.4f, local %.3f %.3f, world %.3f, car w %.2f, dt %.3f, votes %d %d",
				s_lastCmdYaw, s_lastCmdPitch, Wrap(localYaw - s_lastLocalYaw), localPitch - s_lastLocalPitch, localYaw, localPitch, Yaw(look), wz,
				gEnv->pTimer->GetFrameTime(), s_voteYaw, s_votePitch);
		}
		s_lastLocalYaw = localYaw;
		s_lastLocalPitch = localPitch;
		s_lastCmdYaw = s_lastCmdPitch = 0.0f;
		// the vehicle updates every frame while it is at the gun: a copy of
		// the car that slept left its turret where it was, whatever the
		// rotate actions said (coop_test_gun_awake 0: not, for testing)
		static ICVar* pGunAwake = gEnv->pConsole->GetCVar("coop_test_gun_awake");
		if (!pGunAwake || pGunAwake->GetIVal())
			pVehicle->NeedsUpdate();
		// (CryAction's world hold of the turret's yaw off where it spins the
		// gun: TurretHoldOff)
		const bool yawHeld = TurretHoldOff(pVehicle);
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
			// (why none: the nearest enemy and what is between, once a second)
			static float s_noneAt = 0.0f;
			static bool s_noneFar = false;
			if (Now() >= s_noneAt)
			{
				s_noneAt = Now() + 5.0f;
				IActor* pNear = 0;
				float nearD = 150.0f;
				int hostile = 0;
				float nearestAny = 1e9f;
				IActorIteratorPtr it = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
				while (IActor* pActor = it->Next())
					if (Hostile(pActor))
					{
						++hostile;
						nearestAny = min(nearestAny, AimPoint(pActor->GetEntity()).GetDistance(eye));
						const float d = AimPoint(pActor->GetEntity()).GetDistance(eye);
						if (d < nearD)
						{
							nearD = d;
							pNear = pActor;
						}
					}
				// (once, and again when the car moved 20 m or 30 s passed: a
				// copy of the car that stayed behind showed in nothing else)
				static Vec3 s_noneCarAt(ZERO);
				static float s_noneSaidAt = -100.0f;
				const Vec3 carAt = pVehicle->GetEntity()->GetWorldPos();
				if (pNear)
					s_noneFar = false;
				else if (!s_noneFar || carAt.GetDistance(s_noneCarAt) > 20.0f || Now() - s_noneSaidAt > 30.0f)
				{
					s_noneFar = true;
					s_noneCarAt = carAt;
					s_noneSaidAt = Now();
					CryLogAlways("[CoopAgent] at the gun of %s, no enemy: %d hostile, the nearest %.0f m off; eye (%.1f %.1f %.1f), car (%.1f %.1f %.1f)",
						pVehicle->GetEntity()->GetName(), hostile, nearestAny, eye.x, eye.y, eye.z, carAt.x, carAt.y, carAt.z);
				}
				if (pNear)
				{
					const Vec3 to = AimPoint(pNear->GetEntity());
					IPhysicalEntity* skip[2];
					int n = 0;
					if (IPhysicalEntity* p = pMe->GetEntity()->GetPhysics())
						skip[n++] = p;
					if (IPhysicalEntity* p = pVehicle->GetEntity()->GetPhysics())
						skip[n++] = p;
					ray_hit hit;
					const bool blocked = FirstBlock(eye, to - eye, ent_all, skip, n, hit);
					IEntity* pHit = blocked && hit.pCollider ? gEnv->pEntitySystem->GetEntityFromPhysics(hit.pCollider) : 0;
					std::map<EntityId, float>::const_iterator noLine = s_noLineUntil.find(pNear->GetEntityId());
					CryLogAlways("[CoopAgent] at the gun of %s, no enemy: the nearest, %s, %.0f m off%s; the view to him %s %.1f m off by %s; eye (%.1f %.1f %.1f), car (%.1f %.1f %.1f)",
						pVehicle->GetEntity()->GetName(), pNear->GetEntity()->GetName(), nearD,
						noLine != s_noLineUntil.end() && Now() < noLine->second ? " (skipped a while: no line of fire)" : "",
						blocked ? "blocked" : "clear,", blocked ? hit.dist : 0.0f,
						pHit ? pHit->GetName() : blocked ? (hit.pCollider && hit.pCollider->GetType() == PE_STATIC ? "something static" : "terrain or nothing known") : "-",
						eye.x, eye.y, eye.z, pVehicle->GetEntity()->GetWorldPos().x, pVehicle->GetEntity()->GetWorldPos().y, pVehicle->GetEntity()->GetWorldPos().z);
				}
			}
			return;
		}
		Vec3 aim = AimPoint(pEnemy->GetEntity());
		if (pEnemy->GetLinkedVehicle() || MountedGunOf(pEnemy))
		{
			Vec3 reach;
			if (ShotPoint(pMe, eye, pEnemy, reach))
				aim = reach;
		}
		const Vec3 want = (aim - eye).GetNormalizedSafe(look);
		const float yawErr = Wrap(Yaw(want) - Yaw(look));
		const float pitchErr = Pitch(want) - Pitch(look);
		static float s_traceAt = 0.0f;
		static int s_traceFrames = 0;
		++s_traceFrames;
		if (Now() >= s_traceAt)
		{
			s_traceAt = Now() + 1.0f;
			// (the gun itself, besides the view: the view comes from the camera)
			Vec3 gun(ZERO);
			if (IEntity* pGun = gEnv->pEntitySystem->GetEntity(pVehicle->GetCurrentWeaponId(pMe->GetEntityId())))
				gun = pGun->GetWorldTM().GetColumn1().GetNormalizedSafe(Vec3(ZERO));
			const Vec3 at = pVehicle->GetEntity()->GetWorldPos();
			IVehicleSeat* pSeat = pVehicle->GetSeatForPassenger(pMe->GetEntityId());
			CryLogAlways("[CoopAgent] gun aim at %s: yaw off %.3f pitch off %.3f (signs %.0f %.0f, votes %d %d), view (%.2f %.2f %.2f), gun (%.2f %.2f %.2f), eye (%.1f %.1f %.1f), car (%.1f %.1f %.1f), %d frames%s, seat state %d",
				pEnemy->GetEntity()->GetName(), yawErr, pitchErr, s_signYaw, s_signPitch, s_voteYaw, s_votePitch, look.x, look.y, look.z,
				gun.x, gun.y, gun.z, eye.x, eye.y, eye.z, at.x, at.y, at.z, s_traceFrames, pVehicle->IsDestroyed() ? ", DESTROYED" : "",
				pSeat ? pSeat->GetCurrentTransition() : -1);
			s_traceFrames = 0;
		}
		// proportional, and what it kept being off added up: while the host
		// steers (the vehicle turning all the time) the turret has to turn
		// all the time too, and the error alone left it a steady 0.09 off at
		// 20 degrees a second, more than it fires at. A new enemy starts it
		// afresh; it shrinks when the error changes side (no overshooting)
		static float s_sumYaw = 0.0f, s_sumPitch = 0.0f;
		static EntityId s_sumEnemy = 0;
		if (s_sumEnemy != enemy)
		{
			s_sumEnemy = enemy;
			s_sumYaw = s_sumPitch = 0.0f;
		}
		// (what a rotate action means, from CryAction.dll: its value is the
		// turn of this one frame, 1 = 0.002 rad, up to the part's yaw/pitch
		// speed (75/70 on an LTV); 25 turned the gun 0.05 rad a frame, 46
		// degrees a second at the companion's 16 frames: behind a car the host
		// drove past the enemy, 0.2-0.3 off all the time, the user's game of
		// 2026-10-16. Now half the error a frame, up to the turret's speed)
		const float dt = clamp_tpl(gEnv->pTimer->GetFrameTime(), 0.0f, 0.2f);
		const float gain = 0.5f / 0.002f, sumGain = 150.0f, maxCmd = 70.0f;
		if (s_sumYaw * yawErr < 0.0f)
			s_sumYaw *= 0.5f;
		if (s_sumPitch * pitchErr < 0.0f)
			s_sumPitch *= 0.5f;
		s_sumYaw = clamp_tpl(s_sumYaw + yawErr * dt, -maxCmd / sumGain, maxCmd / sumGain);
		s_sumPitch = clamp_tpl(s_sumPitch + pitchErr * dt, -maxCmd / sumGain, maxCmd / sumGain);
		// (and against the car's turn: with the turret's world hold off the gun
		// turns with the car, which the host steers; as much again next frame.
		// Not where the yaw keeps its hold: it turns against it already)
		const float carFeed = !yawHeld && fabsf(carTurn) < 0.3f ? carTurn / 0.002f : 0.0f;
		if (fabsf(yawErr) > 0.005f || fabsf(s_sumYaw) > 0.01f || fabsf(carFeed) > 0.25f)
		{
			s_lastCmdYaw = s_signYaw * clamp_tpl(yawErr * gain + s_sumYaw * sumGain - carFeed, -maxCmd, maxCmd);
			pVehicle->OnAction(eVAI_RotateYaw, eAAM_OnPress, s_lastCmdYaw, pMe->GetEntityId());
		}
		if (fabsf(pitchErr) > 0.005f || fabsf(s_sumPitch) > 0.01f)
		{
			s_lastCmdPitch = s_signPitch * clamp_tpl(pitchErr * gain + s_sumPitch * sumGain, -maxCmd, maxCmd);
			pVehicle->OnAction(eVAI_RotatePitch, eAAM_OnPress, s_lastCmdPitch, pMe->GetEntityId());
		}
		// on target, seen, nothing else in the way (its own vehicle aside). The
		// line of fire straight at him: on target the gun points there, and the
		// view a little off (it always is, a few hundredths) ran into the corner
		// of a house half way at 140 m: the shot was always held, the enemy kept
		// (2026-10-15). Still blocked for 2 s: another enemy for a while
		static float s_blockedSince = -1.0f;
		static float s_blockedLastAt = -100.0f;   // the last blocked look at him
		static EntityId s_blockedEnemy = 0;
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
			clear = true;
			if (FirstBlock(eye, (aim - eye).GetNormalizedSafe(look) * (aim.GetDistance(eye) + 2.0f), ent_all, skip, n, hit))
			{
				// (one behind a mounted gun: only a ray that reaches him, the
				// gun's body is within a metre of his head)
				clear = HitIs(hit, pEnemy->GetEntity()) || (hit.pt.GetDistance(aim) < 1.0f && !MountedGunOf(pEnemy));
				if (IVehicle* pTheirs = pEnemy->GetLinkedVehicle())
					clear = clear || hit.pCollider == pTheirs->GetEntity()->GetPhysics();
				// testing (coop_test_shottrace 1): what is in the line of fire
				static float s_blockAt = 0.0f;
				static ICVar* pShotTrace = gEnv->pConsole->GetCVar("coop_test_shottrace");
				if (!clear && pShotTrace && pShotTrace->GetIVal() && Now() >= s_blockAt)
				{
					s_blockAt = Now() + 1.0f;
					IEntity* pHit = hit.pCollider ? gEnv->pEntitySystem->GetEntityFromPhysics(hit.pCollider) : 0;
					CryLogAlways("[CoopAgent] gun on %s, the line of fire blocked %.1f m off (of %.0f) by %s", pEnemy->GetEntity()->GetName(), hit.dist,
						aim.GetDistance(eye), pHit ? pHit->GetName() : hit.pCollider && hit.pCollider->GetType() == PE_STATIC ? "something static" : "terrain or nothing known");
				}
			}
			if (!clear)
			{
				++s_bot.heldFire;
				// (blocked for 2 s while on and about him: the aim leaves him and
				// comes back all the time, a moment off him is no clear shot; a
				// second without a blocked look starts it again)
				if (s_blockedEnemy != enemy || s_blockedSince < 0.0f || Now() - s_blockedLastAt > 1.0f)
				{
					s_blockedEnemy = enemy;
					s_blockedSince = Now();
				}
				else if (Now() - s_blockedSince > 2.0f)
				{
					s_noLineUntil[enemy] = Now() + 8.0f;
					s_blockedSince = -1.0f;
					Event("no line of fire from %s's gun at %s: another enemy", pVehicle->GetEntity()->GetName(), pEnemy->GetEntity()->GetName());
				}
				s_blockedLastAt = Now();
			}
			else
				s_blockedSince = -1.0f;
		}
		else
			++s_bot.turning;
		// a gun that does not come round to him: another enemy for a while
		// (beyond what the turret reaches, -18 degrees down on an LTV; or a
		// turret that does not turn): it hung on one for good before
		static Vec3 s_stallLook(ZERO);
		static float s_stallSince = -1.0f, s_stallCheckAt = 0.0f;
		static EntityId s_stallEnemy = 0;
		if (onTarget || fabsf(yawErr) + fabsf(pitchErr) < 0.3f || s_stallEnemy != enemy)
		{
			s_stallEnemy = enemy;
			s_stallSince = -1.0f;
			s_stallLook = look;
			s_stallCheckAt = Now() + 1.0f;
		}
		else if (Now() >= s_stallCheckAt)
		{
			const bool turned = s_stallLook.Dot(look) < cosf(0.035f);
			s_stallLook = look;
			s_stallCheckAt = Now() + 1.0f;
			if (turned)
				s_stallSince = -1.0f;
			else if (s_stallSince < 0.0f)
				s_stallSince = Now() - 1.0f;
			else if (Now() - s_stallSince >= 3.0f)
			{
				s_noLineUntil[enemy] = Now() + 6.0f;
				s_stallSince = -1.0f;
				Event("the gun of %s does not come round to %s (%.2f off): another enemy", pVehicle->GetEntity()->GetName(),
					pEnemy->GetEntity()->GetName(), fabsf(yawErr) + fabsf(pitchErr));
			}
		}
		// bursts
		const float now = Now();
		if (clear && now >= s_bot.pauseUntil && now < s_bot.burstUntil)
		{
			// testing (coop_test_shottrace 1): at each burst, where its own
			// gun's barrel points against the line to whom it shoots (the
			// server compares its copy of the gun with the shots)
			static float s_gunTraceAt = 0.0f;
			static ICVar* pGunShotTrace = gEnv->pConsole->GetCVar("coop_test_shottrace");
			if (pGunShotTrace && pGunShotTrace->GetIVal() && now >= s_gunTraceAt)
			{
				s_gunTraceAt = now + 1.0f;
				Vec3 barrel(ZERO);
				for (int i = 0, n = pVehicle->GetPartCount(); i < n; ++i)
					if (IVehiclePart* pPart = pVehicle->GetPart((unsigned int)i))
						if (pPart->GetType() == IVehiclePart::eVPT_AnimatedJoint
							&& (*(const float*)((const char*)pPart + 0x88) != 0.0f || *(const float*)((const char*)pPart + 0x98) != 0.0f))
							if (IVehicleSeat* pSeat = *(IVehicleSeat**)((char*)pPart + 0x6c))
								if (pSeat->GetPassenger(true) == pMe->GetEntityId())
									barrel = pPart->GetWorldTM().GetColumn1();
				const Vec3 line = (aim - eye).GetNormalizedSafe(look);
				if (barrel.GetLengthSquared() > 0.5f)
					CryLogAlways("[CoopAgent] gun burst at %s: its barrel %.0f deg off the line to him (barrel yaw %.0f pitch %.0f, line yaw %.0f pitch %.0f, view yaw %.0f pitch %.0f)",
						pEnemy->GetEntity()->GetName(), RAD2DEG(acosf(clamp_tpl(barrel.GetNormalized().Dot(line), -1.0f, 1.0f))),
						RAD2DEG(Yaw(barrel)), RAD2DEG(Pitch(barrel)), RAD2DEG(Yaw(line)), RAD2DEG(Pitch(line)), RAD2DEG(Yaw(look)), RAD2DEG(Pitch(look)));
			}
			VehicleFire(pMe, pVehicle, true);
		}
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
		Vec3 aim = AimPoint(pEnemy->GetEntity());
		// the head, when it sees it, within 35 m or in the sights (no spread
		// there): a body hit takes 52-60 of a soldier's 180, a head hit kills
		// (3900), and a round that goes low still finds the chest. At the
		// chest it hit 5 times a fight and killed one in two fights
		{
			IItem* pSightItem = pMe->GetCurrentItem();
			IWeapon* pSights = pSightItem ? pSightItem->GetIWeapon() : 0;
			const bool inSights = pSights && pSights->IsZoomed() && !pSights->IsZooming();
			if ((inSights || aim.GetDistance(eye) < 35.0f) && !pEnemy->GetLinkedVehicle())
			{
				const Vec3 head = HeadPoint(pEnemy->GetEntity());
				if (head.z > aim.z && Visible(pMe, eye, pEnemy->GetEntity(), head))
					aim = head;
			}
		}
		// (one behind a mounted gun or at a vehicle's gun: the part of him a
		// ray reaches, his head often behind the gun's body)
		if (pEnemy->GetLinkedVehicle() || MountedGunOf(pEnemy))
		{
			Vec3 reach, muzzle(ZERO);
			IItem* pGunItem = pMe->GetCurrentItem();
			IWeapon* pGun = pGunItem ? pGunItem->GetIWeapon() : 0;
			IFireMode* pGunMode = pGun ? pGun->GetFireMode(pGun->GetCurrentFireMode()) : 0;
			if (pGunMode)
				muzzle = pGunMode->GetFiringPos(aim);
			const bool haveMuzzle = pGunMode && muzzle.GetDistance(eye) < 2.0f;
			if (ShotPoint(pMe, eye, pEnemy, reach, haveMuzzle ? &muzzle : 0))
				aim = reach;
			else
			{
				static ICVar* pReachTrace = gEnv->pConsole->GetCVar("coop_test_shottrace");
				static float s_reachToldAt = 0.0f;
				if (pReachTrace && pReachTrace->GetIVal() && Now() >= s_reachToldAt)
				{
					s_reachToldAt = Now() + 1.0f;
					CryLogAlways("[CoopAgent] at %s behind his gun: no part of him a line from the eyes%s reaches", pEnemy->GetEntity()->GetName(),
						haveMuzzle ? " and the gun" : "");
				}
			}
		}
		const bool seen = Visible(pMe, eye, pEnemy->GetEntity(), aim);
		// a small target far off (a gunner behind his mounted gun's shield,
		// one at a vehicle's gun: a head and shoulders): crouched still, in
		// the sights, a shot only with the spread small. At 44 m a rifle's
		// 2 degrees of spread is more than a metre: 3 bursts at the nest's
		// gunner, no hit (2026-10-21)
		const bool smallFar = (pEnemy->GetLinkedVehicle() || MountedGunOf(pEnemy)) && aim.GetDistance(eye) > 20.0f;
		if (smallFar)
			s_bot.smallFarAt = Now();
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
		else if (pMe->GetNanoSuit() && ((pMe->GetNanoSuit()->GetMode() == NANOMODE_CLOAK && s_bot.cloakEscape && aim.GetDistance(eye) > 2.2f)
			|| Now() - s_bot.uncloakAt < 0.35f))
		{
			// cloaked: no shot (a shot from the cloak takes all the energy and
			// shows it); one close enough to have to be shot ends the cloak
			// first (SuitStep: armor, then the shots)
			Fire(pMe, false);
			if (seen && aim.GetDistance(eye) < 6.0f && s_bot.cloakEscape)
			{
				s_bot.cloakEscape = false;
				CryLogAlways("[CoopAgent] the cloaked move is over (%s %.0f m off: shoots him)", pEnemy->GetEntity()->GetName(), aim.GetDistance(eye));
			}
		}
		else if (seen && aim.GetDistance(eye) < 2.2f && Now() >= s_bot.meleeAt)
		{
			// right next to it: a blow (with strength's force when it can; the
			// mode first, the server takes it a moment later)
			Fire(pMe, false);
			CNanoSuit* pSuit = pMe->GetNanoSuit();
			if (pSuit && pSuit->GetMode() != NANOMODE_STRENGTH && pSuit->GetSuitEnergy() >= 40.0f && Now() >= s_bot.meleeWantUntil)
				s_bot.meleeWantUntil = Now() + 1.2f;
			else if (!pSuit || pSuit->GetMode() == NANOMODE_STRENGTH || pSuit->GetSuitEnergy() < 40.0f || Now() - (s_bot.meleeWantUntil - 1.2f) > 0.5f)
			{
				Press(pMe, g_pGame->Actions().special, true);
				Press(pMe, g_pGame->Actions().special, false);
				s_bot.meleeAt = Now() + 1.2f;
				// (strength only for the blow: a shot in strength costs energy)
				s_bot.meleeWantUntil = Now() + 0.4f;
				Event("a blow at %s right next to it", pEnemy->GetEntity()->GetName());
			}
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
		else if (seen && off < 0.06f && clip != 0 && !LineOfFire(pMe, eye, pEnemy, aim))
		{
			// something else in the way (a wall, a crate, a fence): no shot;
			// for 2 s: another enemy (or none: then it does what it would
			// without one, a revive say), this one a while later again
			Fire(pMe, false);
			++s_bot.heldFire;
			// (blocked looks at him without a second's gap: two blocked frames
			// 2 s apart with him out of sight between are no blocked line)
			const EntityId id = pEnemy->GetEntityId();
			if (s_footBlockedEnemy != id || s_footBlockedSince < 0.0f || Now() - s_footBlockedLastAt > 1.0f)
			{
				s_footBlockedEnemy = id;
				s_footBlockedSince = Now();
			}
			else if (Now() - s_footBlockedSince > 2.0f)
			{
				s_footNoLineUntil[id] = Now() + 8.0f;
				s_footBlockedSince = -1.0f;
				Event("no line of fire at %s: another enemy", pEnemy->GetEntity()->GetName());
				// (and a place with a shot at him, round what covers him: behind
				// their truck the soldiers were out of its line the whole fight,
				// it shot once in two minutes)
				if (!s_bot.retreating)
				{
					s_bot.haveCover = false;
					s_bot.coverAskAt = 0.0f;
					AskCover(pEnemy, true, true);
				}
			}
			s_footBlockedLastAt = Now();
		}
		else if (seen && off < 0.06f && clip != 0 && pWeapon->IsZooming())
			Fire(pMe, false);    // (the sights going up or down: no shot then)
		else if (seen && off < 0.06f && clip != 0)
		{
			s_footBlockedSince = -1.0f;
			// short bursts with a pause between (the spread grows each shot
			// and shrinks in the pause: long bursts were sprayed at 25 m);
			// shorter still far off
			const float now = Now();
			const float range = aim.GetDistance(eye);
			// (not a burst while the spread is wide, moving or just landed:
			// 7-9 degrees at 30-60 m, every round wide of him; a moment later)
			IFireMode* pSpreadMode = pWeapon->GetFireMode(pWeapon->GetCurrentFireMode());
			const bool wide = !s_bot.attackHeld && range > 10.0f && pSpreadMode && pSpreadMode->GetSpread() > (smallFar ? 0.8f : 3.0f);
			if (wide && now >= s_bot.pauseUntil && now < s_bot.burstUntil)
			{
				const float len = s_bot.burstUntil - s_bot.pauseUntil;
				s_bot.pauseUntil = now + 0.1f;
				s_bot.burstUntil = s_bot.pauseUntil + len;
				s_bot.spreadHeldAt = now;
				Fire(pMe, false);
			}
			else if (now >= s_bot.pauseUntil && now < s_bot.burstUntil)
			{
				if (!s_bot.attackHeld)
				{
					++s_bot.bursts;
					// testing (coop_test_shottrace 1): at what each burst goes and
					// what the aim meets first (the truck took most of its shots)
					static ICVar* pBurstTrace = gEnv->pConsole->GetCVar("coop_test_shottrace");
					if (pBurstTrace && pBurstTrace->GetIVal())
					{
						IPhysicalEntity* skip[2];
						int n = 0;
						if (IPhysicalEntity* p = pMe->GetEntity()->GetPhysics())
							skip[n++] = p;
						if (IItem* pItem = pMe->GetCurrentItem())
							if (IPhysicalEntity* p = pItem->GetEntity()->GetPhysics())
								skip[n++] = p;
						const Vec3 look = pMe->GetViewRotation().GetColumn1();
						ray_hit hit;
						const bool any = FirstBlock(eye, look * (range + 2.0f), ent_all, skip, n, hit);
						IEntity* pHit = any && hit.pCollider ? gEnv->pEntitySystem->GetEntityFromPhysics(hit.pCollider) : 0;
						IVehicle* pTheirs = pEnemy->GetLinkedVehicle();
						// (where the shot goes: the game casts its ray along the
						// movement state's fire direction, not along the view; the
						// sights bug was there, 27-52 degrees)
						float gunOff = -1.0f;
						IItem* pGunItem = pMe->GetCurrentItem();
						IWeapon* pGun = pGunItem ? pGunItem->GetIWeapon() : 0;
						IFireMode* pGunMode = pGun ? pGun->GetFireMode(pGun->GetCurrentFireMode()) : 0;
						if (IMovementController* pMC = pMe->GetMovementController())
						{
							SMovementState st;
							pMC->GetMovementState(st);
							const Vec3 toAim = (aim - st.weaponPosition).GetNormalizedSafe(look);
							gunOff = RAD2DEG(acos_tpl(clamp_tpl(st.fireDirection.GetNormalizedSafe(toAim).Dot(toAim), -1.0f, 1.0f)));
						}
						CryLogAlways("[CoopAgent] burst at %s (%.0f m%s%s): the aim meets %s at %.1f m, %.2f m from the aim point; the gun %.1f deg off him, spread %.1f",
							pEnemy->GetEntity()->GetName(), range, pTheirs ? ", in " : "", pTheirs ? pTheirs->GetEntity()->GetName() : "",
							pHit ? pHit->GetName() : any ? "something static" : "nothing", any ? hit.dist : -1.0f, any ? hit.pt.GetDistance(aim) : -1.0f,
							gunOff, pGunMode ? pGunMode->GetSpread() : -1.0f);
					}
				}
				Fire(pMe, true);
			}
			else if (now >= s_bot.burstUntil)
			{
				Fire(pMe, false);
				s_bot.pauseUntil = now + (range > 30.0f ? 0.45f : 0.3f) + cry_frand() * 0.25f;
				s_bot.burstUntil = s_bot.pauseUntil + (range > 30.0f ? 0.15f : 0.22f) + cry_frand() * 0.15f;
			}
			else
				Fire(pMe, false);
		}
		else
			Fire(pMe, false);
		// the sights up far off while it stands or crouches still (the
		// spread a tenth), down to move or to change weapons
		// (wanted only up from cover, still: ZoomStep decides once it is
		// known whether it moves this frame; in the open it toggled with each
		// side step and never fired, the autotester's companion-fight-bridge)
		const Vec3 myPos = pMe->GetEntity()->GetWorldPos();
		s_bot.zoomWant = fightsWithIt && seen && aim.GetDistance(eye) > 10.0f && clip != 0
			&& ((s_bot.peekUp && s_bot.haveCover && Dist2D(s_bot.cover, myPos) < 1.5f) || smallFar);
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
		// (a climb MoveTo no longer looks after - there, another order, down: over)
		if (s_bot.climbUntil > 0.0f && Now() - s_bot.climbSeenAt > 0.3f)
		{
			s_bot.climbUntil = 0.0f;
			s_bot.strengthJumpAt = -1.0f;
		}
		s_ctl.crouch = false;
		s_ctl.lean = 0;
		s_ctl.prone = false;
		s_bot.zoomWant = false;
		s_tetherRadius = 0.0f;
		static bool s_wasInGame = false;
		if (!pMe || !InGame(pMe))
		{
			s_wasInGame = false;
			s_heldPhys = 0;
			s_bot.doing = pMe ? "waiting to spawn" : "not in a game";
			return;
		}
		s_heldPhys = HeldPhysics(pMe);
		// (back in the game: joined again after a load, say; what it did
		// before was in another world)
		if (!s_wasInGame)
		{
			s_wasInGame = true;
			ResetFight("in the game again");
		}
		if (Now() >= s_bot.statsAt)
		{
			if (s_bot.bursts || s_bot.heldFire || s_bot.stuckTimes || s_bot.unseen || s_bot.turning || s_bot.localPlans)
				CryLogAlways("[CoopAgent] the last minute: %d bursts fired; no shot: %d frames something in the way, %d enemy not in sight, %d turning to him; stuck %d times, %d ways round found close by",
					s_bot.bursts, s_bot.heldFire, s_bot.unseen, s_bot.turning, s_bot.stuckTimes, s_bot.localPlans);
			s_bot.bursts = s_bot.heldFire = s_bot.stuckTimes = s_bot.unseen = s_bot.turning = s_bot.localPlans = 0;
			if (s_bot.fightOpen + s_bot.fightHidden + s_bot.fightPeek + s_bot.fightBack > 1.0f || s_bot.suitSwitches)
				CryLogAlways("[CoopAgent] the last minute in fights: open %.0f s, hidden %.0f s, up from cover %.0f s, falling back %.0f s; %d peeks (shot while up %d, %d leaning out), %d reloads in cover, %d suit switches; %d hits marked; cloaked %.0f s (%d moves), lying down %.0f s",
					s_bot.fightOpen, s_bot.fightHidden, s_bot.fightPeek, s_bot.fightBack, s_bot.peeks, s_bot.peekHits, s_bot.leanPeeks, s_bot.hiddenReloads, s_bot.suitSwitches, s_bot.hitsConfirmed,
					s_bot.cloakSeconds, s_bot.cloaks, s_bot.proneSeconds);
			s_bot.fightOpen = s_bot.fightHidden = s_bot.fightPeek = s_bot.fightBack = s_bot.cloakSeconds = s_bot.proneSeconds = 0.0f;
			s_bot.peeks = s_bot.peekHits = s_bot.leanPeeks = s_bot.hiddenReloads = s_bot.suitSwitches = s_bot.hitsConfirmed = s_bot.cloaks = 0;
			s_bot.statsAt = Now() + 60.0f;
		}
		IViewSystem* pView = g_pGame->GetIGameFramework()->GetIViewSystem();
		const bool down = pMe->GetHealth() <= 0;
		// what happened to it
		if (down != s_bot.wasDown)
		{
			Event(down ? "you are down: a teammate can revive you" : "you are up again");
			s_bot.wasDown = down;
			ResetFight(down ? "down" : "up again");
		}
		if (!down && s_bot.lastHealth > 0 && pMe->GetHealth() < s_bot.lastHealth - 5)
		{
			Event("you were hit: health %d", pMe->GetHealth());
			s_bot.hitAt = Now();
		}
		s_bot.lastHealth = (float)pMe->GetHealth();
		// (a hit the suit's armor took: the energy, not the health, went
		// down; the damage indicator may come late or not at all)
		{
			static float s_lastEnergy = -1.0f;
			CNanoSuit* pSuit = pMe->GetNanoSuit();
			const float energy = pSuit ? pSuit->GetSuitEnergy() : -1.0f;
			if (!down && pSuit && pSuit->GetMode() == NANOMODE_DEFENSE && s_lastEnergy >= 0.0f && energy < s_lastEnergy - 5.0f)
				s_bot.hitAt = Now();
			s_lastEnergy = energy;
		}
		{
			IActorIteratorPtr it = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
			while (IActor* pActor = it->Next())
			{
				if (!pActor->IsPlayer() || pActor == pMe)
					continue;
				const bool isDown = IsDown(pActor);
				bool& seen = s_bot.downSeen[pActor->GetEntityId()];
				if (isDown != seen)
				{
					Event(isDown ? "%s is down" : "%s is up again", pActor->GetEntity()->GetName());
					if (isDown)
						s_bot.downAt[pActor->GetEntityId()] = Now();
				}
				seen = isDown;
			}
		}
		// something caught in the left hand by the use key (a crate, a bottle
		// next to a weapon it went to pick up): its rifle in one hand, the
		// other at its face; put down again (not thrown: a throw reaches every
		// game now, at whoever stands there). Not what it was told to carry
		if (COffHand* pOffHand = static_cast<COffHand*>(pMe->GetWeaponByClass(CItem::sOffHandClass)))
		{
			const bool ordered = s_carry.prop && pOffHand->CoopHeldId() == s_carry.prop;
			if ((pOffHand->GetOffHandState() & (eOHS_HOLDING_OBJECT | eOHS_HOLDING_NPC)) && !ordered)
			{
				if (s_bot.dropAt <= 0.0f)
					s_bot.dropAt = Now() + 1.0f;
				else if (Now() >= s_bot.dropAt)
				{
					Press(pMe, g_pGame->Actions().zoom, true);
					Press(pMe, g_pGame->Actions().zoom, false);
					s_bot.dropAt = Now() + 1.5f;
					Event("puts down what it caught in the left hand");
				}
			}
			else
				s_bot.dropAt = 0.0f;
		}
		// (a cutscene over: when, for the look below)
		static bool s_inCutscene = false;
		static float s_cutsceneOverAt = -100.0f;
		const bool cutscene = pView && pView->IsPlayingCutScene();
		if (cutscene)
			s_inCutscene = true;
		else if (s_inCutscene)
		{
			s_inCutscene = false;
			s_cutsceneOverAt = Now();
		}
		if (down || cutscene)
		{
			Fire(pMe, false);
			HoldUse(pMe, false, 0);
			s_bot.doing = down ? "down, waiting for a teammate" : "watching a cutscene";
			return;
		}
		// a cutscene puts the host where the story goes on, the companion it
		// leaves where it stood: right after it, far from him or with
		// something between, put next to him (as the cut in single player).
		// The research dome's moved the host 29 m out of the dome; it stayed
		// inside behind a wall (the user's game, 2026-10-22)
		if (Now() - s_cutsceneOverAt > 1.0f && Now() - s_cutsceneOverAt < 6.0f && s_bot.order != eO_Hold && s_bot.order != eO_Goto)
		{
			IActor* pFollow = s_bot.leader.empty() ? HostPlayer() : ActorByName(s_bot.leader.c_str());
			if (!pFollow)
				pFollow = HostPlayer();
			if (pFollow && pFollow->GetHealth() > 0 && !pFollow->GetLinkedVehicle() && !pMe->GetLinkedVehicle())
			{
				const Vec3 me = pMe->GetEntity()->GetWorldPos(), him = pFollow->GetEntity()->GetWorldPos();
				const float apart = Dist2D(me, him);
				if (apart > 25.0f || (apart > 4.0f && !ClearWay(pMe, me, him)))
				{
					Event("the cutscene is over, %s %.0f m off%s: put next to him", pFollow->GetEntity()->GetName(), apart, apart > 25.0f ? "" : " with something between");
					s_bot.askAt = 0.0f;
					Ask(1, pFollow->GetEntity()->GetName());
				}
				s_cutsceneOverAt = -100.0f;
			}
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
		// (somewhere else at once: a load, the host's game moved it)
		{
			static Vec3 s_thinkPos(ZERO);
			static bool s_thinkPosSet = false;
			static bool s_thinkInVehicle = false;
			// (a car at 30 m/s in a long frame goes 20 m: not then)
			if (s_thinkPosSet && !pMyVehicle && !s_thinkInVehicle && Dist2D(pos, s_thinkPos) > 20.0f)
			{
				string why;
				why.Format("moved %.0f m at once", Dist2D(pos, s_thinkPos));
				ResetFight(why.c_str());
			}
			s_thinkPos = pos;
			s_thinkPosSet = true;
			s_thinkInVehicle = pMyVehicle != 0;
		}

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
		// the suit's mode on every way out of here (falling back, reviving,
		// picking up, getting into a vehicle return early: the cloak to get
		// away never came, a speed or strength mode stayed on with no armor)
		struct SSuitAtEnd
		{
			CPlayer* pMe;
			Vec3 pos;
			IActor* pLeader;
			~SSuitAtEnd()
			{
				ZoomStep(pMe);
				SuitStep(pMe, pos, pLeader, ActorOf(s_bot.enemy));
			}
		} suitAtEnd = { pMe, pos, pLeader };

		// an order with a thing: to it, aimed, the key (a teammate down, shot
		// at a moment ago: that first)
		if (!pMyVehicle && s_carry.mode != eC_None)
		{
			const bool busy = Now() - s_bot.hitAt < 3.0f
				|| (s_bot.order != eO_Hold && s_bot.order != eO_Goto && DownedTeammate(pMe, pos, 2000.0f));
			if (CarryStep(pMe, pos, eye, busy))
				return;
		}

		// ---- reviving comes first (unless ordered to stay), however far: the
		// host down 200 m off (it had been put back where it got into a car)
		// and only 80 m looked at, it stood there (2026-10-15)
		IActor* pDowned = (s_bot.order == eO_Hold || s_bot.order == eO_Goto) ? 0 : DownedTeammate(pMe, pos, 2000.0f);
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
				// (the tether: 12 m round him, or as far as it is now - set once,
				// not following it about: a cover asked for was dropped again
				// with each step, the review)
				s_bot.tetherRadiusFor = max(12.0f, d + 2.0f);
			}
			ChooseWeapon(pMe, false);
			// (hit a moment ago: whoever it can see within 80 m, so it shoots
			// back at a rifleman 25 m off instead of keeping down at the body
			// while he hits it, the review, 2026-10-16)
			const bool hitLately = Now() - s_bot.hitAt < 3.0f;
			EntityId enemy = s_bot.fireAtWill ? PickEnemy(pMe, eye, hitLately ? 80.0f : d > 3.0f ? 60.0f : 18.0f, 0.0f) : 0;
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
			// close (not by a vehicle about to blow, below)
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
			// under fire: hit a moment ago (it cannot tell a miss)
			const bool underFire = Now() - s_bot.hitAt < 3.0f;
			const bool atBody = reach || (d <= 1.6f && s_bot.reachFailSince >= 0 && Now() - s_bot.reachFailSince > 8.0f) || (reviving && d <= 2.6f);
			// (the use key stays held under its cloak at the body with nobody
			// within 6 m: let go every frame, the revive never ended, the review)
			const bool foeVeryClose = pEnemy && Dist2D(pEnemy->GetEntity()->GetWorldPos(), pos) < 6.0f;
			if (pEnemy && !(reviving && atBody && !enemyClose) && !(s_bot.cloakEscape && s_bot.cloakForRevive && atBody && !foeVeryClose))
				HoldUse(pMe, false, 0);
			// held to him: no cover, no fall back farther from him than 12 m
			// (or than it is now: one not farther from him is fine)
			s_tether = body;
			s_tetherRadius = s_bot.tetherRadiusFor;
			// (a cover from before far from him: none, the next ask is held to
			// him; it fought from 30 m off, the user's game)
			if (s_bot.haveCover && Dist2D(s_bot.cover, body) > s_tetherRadius + 2.0f)
			{
				s_bot.haveCover = false;
				s_bot.coverAskAt = Now() + 1.0f;
			}
			// (one within 6 m sees through its cloak: the cloak ends, it fights
			// him; else it crouched cloaked at the body, neither shooting nor
			// reviving, the review)
			if (foeVeryClose && s_bot.cloakEscape && s_bot.cloakForRevive)
			{
				s_bot.cloakEscape = false;
				CryLogAlways("[CoopAgent] the cloaked move is over (%s %.0f m off: shoots him)", pEnemy->GetEntity()->GetName(), Dist2D(pEnemy->GetEntity()->GetWorldPos(), pos));
			}
			// (a cloak of the fight going on: over, the revive decides; it sat
			// cloaked at the body neither shooting nor reviving, the review)
			if (s_bot.cloakEscape && !s_bot.cloakForRevive && !s_bot.retreating)
			{
				s_bot.cloakEscape = false;
				CryLogAlways("[CoopAgent] the cloaked move is over (%s is down: to him)", pDowned->GetEntity()->GetName());
			}
			// badly hurt: out of the fire first (dead it revives nobody)
			if (Survive(pMe, pos, eye, pEnemy))
				return;
			// (a vehicle shot almost to pieces next to him, see below)
			IVehicle* pBomb = 0;
			{
				IVehicleIteratorPtr pVs = g_pGame->GetIGameFramework()->GetIVehicleSystem()->CreateVehicleIterator();
				while (IVehicle* pV = pVs->Next())
					if (!pV->IsDestroyed() && pV->GetDamageRatio() > 0.9f && Dist2D(pV->GetEntity()->GetWorldPos(), body) < 7.0f)
					{
						pBomb = pV;
						break;
					}
			}
			// (a revive under way too: the camp's jeep blew up by the host's
			// body while the companion revived him, 108 damage; the review.
			// Not for ever: 40 s after he went down it goes to him anyway, as
			// clearing the way below)
			const bool bombWait = pBomb && Now() - s_bot.downedSince < 40.0f && (underFire || pEnemy || Now() - s_bot.hitAt < 8.0f);
			// in danger on the way: to him cloaked when the energy is enough
			// for the way (5 a metre) and a little more (holding the use key
			// does not end the cloak); it lay "down until it stops" at energy
			// 200 before, or fought its way there in the open
			if (!bombWait)
			{
				CNanoSuit* pReviveSuit = pMe->GetNanoSuit();
				const float reviveEnergy = pReviveSuit ? pReviveSuit->GetSuitEnergy() : 0.0f;
				const bool danger = underFire || pEnemy || Now() - s_bot.hitAt < 8.0f;
				// (one within 6 m sees through the cloak: fought first. At the
				// body hit with energy 200, a soldier 8 m off, it did not cloak:
				// two more hits and the energy was gone, then it was killed)
				const bool enemyVeryClose = pEnemy && Dist2D(pEnemy->GetEntity()->GetWorldPos(), pos) < 6.0f;
				if (danger && !s_bot.cloakEscape && !enemyVeryClose && d < 40.0f && CanCloak(pMe)
					&& (atBody ? underFire && reviveEnergy >= 40.0f : d > 3.0f && reviveEnergy >= CloakNeed(d) + 5.0f))
				{
					string why;
					why.Format("%s %s %.0f m off, %s", atBody ? "to revive" : "to", pDowned->GetEntity()->GetName(), d, underFire ? "under fire" : "enemies about");
					StartCloakedMove(pMe, body, why.c_str(), reviveEnergy);
					s_bot.cloakForRevive = true;
				}
			}
			// a vehicle shot almost to pieces next to him: one more hit and it
			// blows. Not into that while they shoot: the camp's jeep at 0.996
			// blew up by the host's body and took the companion with it (the
			// autotester's companion-crossfire-camp). It waits 10 m off, fights
			{
				if (bombWait)
				{
					const Vec3 bombAt = pBomb->GetEntity()->GetWorldPos();
					static EntityId s_bombTold = 0;
					if (s_bombTold != pBomb->GetEntityId())
					{
						s_bombTold = pBomb->GetEntityId();
						Event("%s lies by %s, shot almost to pieces: keeps away from it while they shoot", pDowned->GetEntity()->GetName(), pBomb->GetEntity()->GetName());
					}
					if (s_bot.cloakEscape && s_bot.cloakForRevive)
						s_bot.cloakEscape = false;
					s_bot.reviveSince = -1;
					HoldUse(pMe, false, 0);
					if (Dist2D(pos, bombAt) < 10.0f)
					{
						Vec3 away = pos - bombAt;
						away.z = 0;
						s_ctl.moveDir = Steer(pMe, pos, away.GetNormalizedSafe(Vec3(0, 1, 0)));
					}
					else
						s_ctl.crouch = true;
					if (pEnemy)
						FightStep(pMe, eye, pEnemy);
					else
						Fire(pMe, false);
					s_bot.doing.Format("waiting 10 m from %s by %s", pBomb->GetEntity()->GetName(), pDowned->GetEntity()->GetName());
					return;
				}
			}
			// (cloaked, or the cloak asked for a moment ago: the suit takes it a
			// frame later)
			const bool cloakedWay = s_bot.cloakEscape && s_bot.cloakForRevive
				&& ((pMe->GetNanoSuit() && pMe->GetNanoSuit()->GetMode() == NANOMODE_CLOAK) || Now() - s_bot.cloakSince < 1.5f);
			if (!atBody && pEnemy && d > 4.0f && Dist2D(pEnemy->GetEntity()->GetWorldPos(), pos) < 40.0f
				&& Now() - s_bot.downedSince < 40.0f && !cloakedWay)
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
			else if (!atBody && !pEnemy && underFire && !cloakedWay)
			{
				// shot at by someone it cannot shoot back at: running to the body
				// now would get it killed too; down, until the shots stop
				s_bot.reviveSince = -1;
				s_ctl.moveDir.zero();
				s_ctl.crouch = true;
				Fire(pMe, false);
				s_bot.doing.Format("under fire on the way to %s (%.0f m): down until it stops", pDowned->GetEntity()->GetName(), d);
			}
			else if (!atBody)
			{
				MoveTo(pMe, body, 0.9f, !pEnemy && !cloakedWay, false, pDowned->GetEntity()->GetName());
				s_bot.reviveSince = -1;
				if (cloakedWay)
				{
					Fire(pMe, false);
					HoldUse(pMe, false, 0);
					LookAt(eye, body + Vec3(0, 0, 1.2f), 6.0f);
					s_bot.doing.Format("to %s cloaked (%.0f m)", pDowned->GetEntity()->GetName(), d);
				}
				else if (pEnemy)
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
			else if (pEnemy && !(reviving && !enemyClose) && !(cloakedWay && !foeVeryClose))
			{
				s_ctl.moveDir.zero();
				s_ctl.crouch = true;
				FightStep(pMe, eye, pEnemy);
				s_bot.doing.Format("at %s, fighting %s first", pDowned->GetEntity()->GetName(), pEnemy->GetEntity()->GetName());
			}
			else if (underFire && !cloakedWay)
			{
				// shot at and nobody it can shoot back at (or they are out of its
				// line of fire): no revive under fire (cloaked it revives) (a revive under way stops
				// too), down by the body until the shots stop. Not shot at: it
				// revives, enemies about or not (those it cannot hit and who do
				// not shoot at it are no reason to wait)
				HoldUse(pMe, false, 0);
				s_ctl.moveDir.zero();
				s_ctl.crouch = true;
				Fire(pMe, false);
				s_bot.reviveSince = -1;
				s_bot.doing.Format("at %s, under fire: down until it stops", pDowned->GetEntity()->GetName());
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
		if (s_bot.cloakEscape && s_bot.cloakForRevive)
		{
			s_bot.cloakEscape = false;
			CryLogAlways("[CoopAgent] the cloaked move is over (nobody down to revive any more)");
		}
		s_bot.cloakForRevive = false;
		if (s_bot.order == eO_Revive)
			s_bot.order = eO_Free;

		// ---- vehicles: along with the one it follows
		const bool follows = s_bot.order == eO_Free || s_bot.order == eO_Follow;
		// (what it makes of its vehicle, once a second while it is in one)
		static float s_vehTraceAt = 0.0f;
		static string s_vehTraceLast;
		IVehicleSeat* pTraceSeat = pMyVehicle ? pMyVehicle->GetSeatForPassenger(pMe->GetEntityId()) : 0;
		IVehicle* pTraceLeaderVehicle = pMyVehicle && pLeader ? pLeader->GetLinkedVehicle() : 0;
		// its vehicle is updated every frame while it rides. A client updates a
		// vehicle (its seats, the gun, the turret, the engine) only while it
		// counts as seen or near, and the companion renders nothing; the seat
		// it sat down in stays "entering" until the seat's update ends it, and
		// till then the vehicle hands it no gun: never one shot from the car's
		// gun in the user's games of 2026-10-16/17 ("seat state 1", "gun (0.00
		// 0.00 0.00)", powered=0; there the car was hidden in its game, see
		// below). Forced as the game's own items do (CItem::RequireUpdate)
		bool forced = false;
		// one its own game keeps hidden by itself (the server shows every
		// vehicle to a joining player; this is in case one is still missed):
		// the engine updates no hidden entity, whatever is forced. Not one the
		// server hid (the story disabled the vehicle it sits in: as for
		// everybody), and the 2 s are its vehicle's
		static float s_hiddenSince = -1.0f;
		static EntityId s_hiddenVehicle = 0;
		if (!pMyVehicle || pMyVehicle->GetEntityId() != s_hiddenVehicle)
		{
			s_hiddenVehicle = pMyVehicle ? pMyVehicle->GetEntityId() : 0;
			s_hiddenSince = -1.0f;
		}
		if (pMyVehicle && !gEnv->bServer && pMyVehicle->GetGameObject())
		{
			if (!pMyVehicle->GetEntity()->IsHidden() || CoopAI::ServerSaysHidden(pMyVehicle->GetEntityId()))
				s_hiddenSince = -1.0f;
			else if (s_hiddenSince < 0.0f)
				s_hiddenSince = Now();
			else if (Now() - s_hiddenSince > 2.0f)
			{
				pMyVehicle->GetEntity()->Hide(false);
				Event("%s was hidden in its own game for 2 s while it rode in it: shown", pMyVehicle->GetEntity()->GetName());
				s_hiddenSince = -1.0f;
			}
			static ICVar* pForce = gEnv->pConsole->GetCVar("coop_test_vehicle_force");
			if (!pForce || pForce->GetIVal())
			{
				IGameObject* pGO = pMyVehicle->GetGameObject();
				pGO->ForceUpdateExtension(pMyVehicle, IVehicle::eVUS_Always);
				pGO->ForceUpdateExtension(pMyVehicle, IVehicle::eVUS_EnginePowered);
				pGO->ForceUpdateExtension(pMyVehicle, IVehicle::eVUS_PassengerIn);
				forced = true;
			}
		}
		// (its seat's transition here, each change: one that stayed "entering"
		// left it without the gun for the whole ride, the user's game of
		// 2026-10-17)
		{
			static int s_seatState = -1;
			static float s_seatStateSince = 0.0f;
			const int seatState = pTraceSeat ? (int)pTraceSeat->GetCurrentTransition() : -1;
			if (seatState != s_seatState)
			{
				if (pTraceSeat)
				{
					CryLogAlways("[CoopAgent] its seat %s of %s: transition %d -> %d (%.1f s after the last change), gun %s; the vehicle %s, %s",
						pTraceSeat->GetSeatName(), pMyVehicle->GetEntity()->GetName(), s_seatState, seatState, Now() - s_seatStateSince,
						pMyVehicle->GetCurrentWeaponId(pMe->GetEntityId()) ? "in hand" : "none", forced ? "updated every frame" : "left to the engine",
						VehicleUpdateInfo(pMyVehicle).c_str());
				}
				else
					CryLogAlways("[CoopAgent] its seat: none any more (transition was %d for %.1f s)", s_seatState, Now() - s_seatStateSince);
				s_seatState = seatState;
				s_seatStateSince = Now();
			}
		}
		string vehTrace;
		if (pMyVehicle)
			vehTrace.Format("%s|%s|%d|%d|%s", pMyVehicle->GetEntity()->GetName(), pTraceSeat ? pTraceSeat->GetSeatName() : "-", (int)s_bot.fireAtWill,
				(int)s_bot.order, pTraceLeaderVehicle ? pTraceLeaderVehicle->GetEntity()->GetName() : "-");
		if (pMyVehicle && (Now() >= s_vehTraceAt || vehTrace != s_vehTraceLast))
		{
			// (every 5 s while its seat is still "entering": the judge's D22)
			s_vehTraceAt = Now() + (pTraceSeat && pTraceSeat->GetCurrentTransition() == IVehicleSeat::eVT_Entering ? 5.0f : 30.0f);
			s_vehTraceLast = vehTrace;
			IVehicleSeat* pSeat = pTraceSeat;
			IVehicle* pLeaderVehicle = pTraceLeaderVehicle;
			CryLogAlways("[CoopAgent] in %s: seat %s (gunner %d, driver %d, transition %d, gun %d), fire at will %d, order %d, the leader %s in %s; %s",
				pMyVehicle->GetEntity()->GetName(), pSeat ? pSeat->GetSeatName() : "-", pSeat ? (int)pSeat->IsGunner() : -1,
				pSeat ? (int)pSeat->IsDriver() : -1, pSeat ? (int)pSeat->GetCurrentTransition() : -1,
				pMyVehicle->GetCurrentWeaponId(pMe->GetEntityId()) ? 1 : 0, (int)s_bot.fireAtWill, (int)s_bot.order,
				pLeader ? pLeader->GetEntity()->GetName() : "-", pLeaderVehicle ? pLeaderVehicle->GetEntity()->GetName() : "nothing",
				VehicleUpdateInfo(pMyVehicle).c_str());
		}
		if (follows && pLeader)
		{
			IVehicle* pLeaderVehicle = pLeader->GetLinkedVehicle();
			if (pLeaderVehicle && pLeaderVehicle->IsDestroyed())
				pLeaderVehicle = 0;    // (a wreck: nothing to get into)
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
		if (pMyVehicle && pMyVehicle->IsDestroyed())
		{
			// a wreck: no seat action works any more (CryAction drops them),
			// its gun neither turns nor fires; it sat in one for good,
			// "turning" to its enemy (the autotester's game, 2026-10-16)
			VehicleFire(pMe, pMyVehicle, false);
			Ask(2, "");
			if (s_bot.doing.find("wreck") == string::npos)
				Event("%s is destroyed: gets out", pMyVehicle->GetEntity()->GetName());
			s_bot.doing = "getting out of the wreck";
			return;
		}
		if (pMyVehicle)
		{
			Fire(pMe, false);
			IVehicleSeat* pSeat = pMyVehicle->GetSeatForPassenger(pMe->GetEntityId());
			// (at a gun with fire held too: the gun spun all the same)
			if (pSeat && pSeat->IsGunner() && !pSeat->IsDriver() && !gEnv->bServer)
				TurretHoldOff(pMyVehicle);
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

		// ---- the one it follows far off for a while (he drove or ran away
		// while it fought or picked something up here): put next to him; the
		// fight here is not his. It stayed on the beach while the host was
		// 350 m off in the autotester's game of 2026-10-16 (it catches up only
		// when it walks after him)
		{
			static float s_leaderFarSince = -1.0f;
			const bool farOff = follows && pLeader && pLeader->GetHealth() > 0 && !pLeader->GetLinkedVehicle()
				&& Dist2D(pLeader->GetEntity()->GetWorldPos(), pos) > 120.0f;
			if (!farOff)
				s_leaderFarSince = -1.0f;
			else if (s_leaderFarSince < 0.0f)
				s_leaderFarSince = Now();
			else if (Now() - s_leaderFarSince > 10.0f)
			{
				Event("%s %.0f m off for 10 s: put next to him", pLeader->GetEntity()->GetName(), Dist2D(pLeader->GetEntity()->GetWorldPos(), pos));
				Ask(1, pLeader->GetEntity()->GetName());
				s_leaderFarSince = -1.0f;
				// what it did there is over (or it ran back for the gun it
				// was picking up, to the cover it fell back to, the review)
				s_bot.loot = 0;
				s_bot.retreating = false;
				s_bot.haveCover = false;
				s_bot.path.clear();
			}
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
			// (the cover was for the one before: still one from him? It sat
			// in a cover asked against another soldier and was shot from the
			// side, the user's game of 2026-10-21)
			IActor* pNew = ActorOf(enemy);
			if (pNew && s_bot.haveCover && !s_bot.retreating && Dist2D(s_bot.cover, pos) < 1.5f)
			{
				std::vector<IActor*> him(1, pNew);
				if (!HiddenFrom(pMe, s_bot.cover, s_bot.coverProne ? 0.4f : 1.0f, him))
				{
					s_bot.haveCover = false;
					s_bot.coverAskAt = 0.0f;
					Event("its cover is no cover from %s: another one", pNew->GetEntity()->GetName());
				}
			}
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
		// (not from its cloaked move: a throw costs it 100 energy or all)
		if (!s_bot.cloakEscape && !(pMe->GetNanoSuit() && pMe->GetNanoSuit()->GetMode() == NANOMODE_CLOAK) && GrenadeStep(pMe, pos, eye, pEnemy))
			return;
		// a cover is good for a while (the enemies move)
		if (s_bot.haveCover && Now() - s_bot.coverAt > 20.0f)
			s_bot.haveCover = false;
		if (pEnemy)
		{
			FightStep(pMe, eye, pEnemy);
			aiming = true;
			// shot at, or fighting in the open a few seconds: it takes cover
			// close by and fights from there (it does not stand in the open
			// till it is hit)
			const bool seenNow = Now() - s_bot.enemySeenAt < 1.0f;
			if (!s_bot.haveCover && seenNow)
			{
				if (s_bot.openSince < 0.0f)
					s_bot.openSince = Now();
			}
			else
				s_bot.openSince = -1.0f;
			const bool hitLately = Now() - s_bot.hitAt < 3.0f;
			if (!s_bot.haveCover && (hitLately || (s_bot.openSince >= 0.0f && Now() - s_bot.openSince > 4.0f)))
				AskCover(pEnemy, !hitLately);
			// hit in the open: to cover out of their sight, cloaked, while the
			// energy is there for the way. The armor takes 2 energy a point of
			// damage: two rifle hits and it is gone, and the cloak never came
			// (the user's game of 2026-10-21: no cloak at all, the energy 0 at
			// 78 of 94 hits). Not with one close by it has to shoot
			const bool inCoverNow = s_bot.haveCover && Dist2D(s_bot.cover, pos) < 1.5f;
			CNanoSuit* pFightSuit = pMe->GetNanoSuit();
			const float fightEnergy = pFightSuit ? pFightSuit->GetSuitEnergy() : 0.0f;
			const float toCover = s_bot.haveCover ? Dist2D(s_bot.cover, pos) : 10.0f;
			// (with a cover known - the answer comes a moment after the hit -
			// not to nowhere for 3 s, the review)
			if (Now() - s_bot.hitAt < 2.0f && s_bot.haveCover && !inCoverNow && !s_bot.cloakEscape && !s_bot.retreating && toCover > 2.0f
				&& Now() - s_bot.cloakSince > 6.0f && pEnemy->GetEntity()->GetWorldPos().GetDistance(pos) > 10.0f && fightEnergy >= CloakNeed(toCover) && CanCloak(pMe))
			{
				string why;
				why.Format("hit in the open by %s, to cover %.0f m off", NameOf(LastShooter(2.0f)), toCover);
				StartCloakedMove(pMe, s_bot.cover, why.c_str(), fightEnergy);
			}
			// spotted in the open (the soldiers' meter on it 85 and more) with
			// no cover yet: to cover cloaked before the first hit takes the
			// energy (the co-op server's suit recharges only 3 s after the
			// last hit, 10 a second: under fire it never comes back)
			float spottedBar = 0.0f, vehicleBar = 0.0f;
			const bool spotted = CoopAI::GetStealth(spottedBar, vehicleBar) && spottedBar >= 85.0f;
			// (a cover first: without one it went cloaked nowhere for 3 s, not
			// shooting, again every 6 s, the review)
			if (spotted && !inCoverNow && !s_bot.cloakEscape && !s_bot.retreating && s_bot.openSince >= 0.0f && Now() - s_bot.openSince > 0.5f)
			{
				if (!s_bot.haveCover)
					AskCover(pEnemy, true);
				else if (Now() - s_bot.cloakSince > 6.0f && toCover > 2.0f && pEnemy->GetEntity()->GetWorldPos().GetDistance(pos) > 10.0f
					&& fightEnergy >= CloakNeed(toCover) && CanCloak(pMe))
				{
					string why;
					why.Format("spotted in the open (meter %.0f), to cover %.0f m off", spottedBar, toCover);
					StartCloakedMove(pMe, s_bot.cover, why.c_str(), fightEnergy);
				}
			}
			if (s_bot.cloakEscape && s_bot.cloakGoal.IsZero() && s_bot.haveCover && !s_bot.retreating)
				s_bot.cloakGoal = s_bot.cover;
		}
		else
		{
			s_bot.openSince = -1.0f;
			Fire(pMe, false);
			if (s_bot.order == eO_Attack)
			{
				Event("the attack is over: back to following");
				s_bot.order = eO_Free;
				s_bot.target.clear();
			}
		}

		// ---- moving
		// (holding a place or going to one: a cover close to it; following:
		// one not far from him, unless he is down)
		const bool holdsNear = (s_bot.order == eO_Hold || s_bot.order == eO_Goto) && s_bot.haveSpot && Dist2D(s_bot.cover, s_bot.spot) < 8.0f;
		const bool fightsFromCover = aiming && pEnemy && s_bot.haveCover
			&& (holdsNear || (s_bot.order != eO_Hold && s_bot.order != eO_Goto
				&& (!pLeader || pLeader->GetHealth() <= 0 || Dist2D(s_bot.cover, pLeader->GetEntity()->GetWorldPos()) < 30.0f)));
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
		// (where it fights from, for the log)
		if (aiming || s_bot.retreating)
		{
			if (s_bot.retreating)
				;    // (counted in Survive)
			else if (fightsFromCover && Dist2D(s_bot.cover, pos) < 1.5f)
				(s_bot.peekUp ? s_bot.fightPeek : s_bot.fightHidden) += frameTime;
			else
				s_bot.fightOpen += frameTime;
		}
		// (SuitStep: by the guard, as on every way out of here)
		// facing the way it walks when it has nobody to aim at (a climb faces
		// what it climbs, also stepping back for a run)
		if (!aiming && s_ctl.moveDir.GetLengthSquared() > 0.01f && s_bot.climbUntil <= 0.0f)
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
		{
			COffHand* pOffHand = OffHand(pMe);
			IEntity* pHeld = pOffHand && pOffHand->GetHeldEntityId() ? gEnv->pEntitySystem->GetEntity(pOffHand->GetHeldEntityId()) : 0;
			j += ",\"held\":" + Esc(pHeld ? pHeld->GetName() : "");
		}
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
		else if (cmd == "lean")
		{
			// lean left|right [seconds] | off: held (5 s by default)
			string side = args, secs;
			const int sp = side.find(' ');
			if (sp > 0)
			{
				secs = side.substr(sp + 1);
				side = side.substr(0, sp);
			}
			side.MakeLower();
			const float held = secs.empty() ? 5.0f : (float)atof(secs.c_str());
			if (side != "left" && side != "right" && side != "off")
				Reply(id.c_str(), Fail("lean left|right [seconds] | off"));
			else if (side != "off" && !(held > 0.0f && held <= 60.0f))
				Reply(id.c_str(), Fail("lean: seconds from 0 to 60"));
			else
			{
				if (side == "off")
					s_bot.leanHoldUntil = 0.0f;
				else
				{
					s_bot.leanHoldSide = side == "left" ? -1 : 1;
					s_bot.leanHoldUntil = Now() + held;
				}
				Reply(id.c_str(), Ok(side == "off" ? "not leaning" : (side == "left" ? "leaning left" : "leaning right")));
			}
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
		else if (cmd == "grab" && pMe)
		{
			// grab <name|id|nearest>: picks it up and carries it (till "throw"
			// or "drop")
			string what = args;
			what.Trim();
			IEntity* pThing = FindThing(pMe, what);
			if (!pThing)
				Reply(id.c_str(), Fail("grab: no such thing (a name, an id, or nearest within 8 m)"));
			else if (!CoopProps::Eligible(pThing) || !Pickable(pThing))
				Reply(id.c_str(), Fail(string().Format("grab: %s is not a thing to carry", pThing->GetName()).c_str()));
			else if (CoopProps::IsHeld(pThing->GetId()) && !CoopProps::HeldHere(pThing->GetId()))
				Reply(id.c_str(), Fail(string().Format("grab: %s is in somebody else's hands", pThing->GetName()).c_str()));
			else
			{
				s_carry = SCarry();
				s_carry.prop = pThing->GetId();
				s_carry.mode = eC_Grab;
				s_carry.since = Now();
				s_carry.name = pThing->GetName();
				CryLogAlways("[CoopAgent] grab: %s (%u) by the agent", pThing->GetName(), pThing->GetId());
				Reply(id.c_str(), Ok(("going to pick up " + s_carry.name).c_str()));
			}
		}
		else if ((cmd == "throw" || cmd == "drop") && pMe)
		{
			// throw [x y z | name]: from the left hand, toward that or where it
			// looks; drop: puts it down
			COffHand* pOffHand = OffHand(pMe);
			const EntityId held = pOffHand && (pOffHand->GetOffHandState() & eOHS_HOLDING_OBJECT) ? pOffHand->GetHeldEntityId() : 0;
			if (!held)
				Reply(id.c_str(), Fail((cmd + ": nothing in the left hand").c_str()));
			else
			{
				Vec3 at(ZERO);
				string where = args;
				where.Trim();
				if (cmd == "throw" && !where.empty())
				{
					float x, y, z;
					if (sscanf(where.c_str(), "%f %f %f", &x, &y, &z) == 3)
						at.Set(x, y, z);
					else if (IEntity* pTo = gEnv->pEntitySystem->FindEntityByName(where.c_str()))
						at = Middle(pTo);
				}
				IEntity* pHeld = gEnv->pEntitySystem->GetEntity(held);
				s_carry.prop = held;
				s_carry.mode = cmd == "throw" ? eC_Throw : eC_Drop;
				s_carry.since = Now();
				s_carry.at = at;
				s_carry.name = pHeld ? pHeld->GetName() : "?";
				CryLogAlways("[CoopAgent] %s: %s by the agent", cmd.c_str(), s_carry.name.c_str());
				Reply(id.c_str(), Ok(((cmd == "throw" ? "throwing " : "dropping ") + s_carry.name).c_str()));
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
			else if (pMe->GetLinkedVehicle() || pMe->IsSwimming() || pMe->ShouldSwim())
			{
				Reply(id.c_str(), Fail(pMe->GetLinkedVehicle() ? "weapon: not in a vehicle's seat" : "weapon: not while swimming (the fists only)"));
				return;
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
			m.Trim();
			m.MakeLower();
			if (m != "speed" && m != "strength" && m != "cloak" && m != "armor" && m != "auto")
			{
				Reply(id.c_str(), Fail("suit: speed, strength, cloak, armor or auto"));
				return;
			}
			ENanoMode mode = m == "speed" ? NANOMODE_SPEED : m == "strength" ? NANOMODE_STRENGTH : m == "cloak" ? NANOMODE_CLOAK : NANOMODE_DEFENSE;
			CNanoSuit* pSuit = pMe->GetNanoSuit();
			if (m == "cloak" && pSuit->GetSuitEnergy() <= 21.0f)
			{
				Reply(id.c_str(), Fail(string().Format("not enough energy to cloak (%.0f)", pSuit->GetSuitEnergy())));
				return;
			}
			if (m == "auto")
				s_bot.suitHoldUntil = 0.0f;
			else if (pSuit->GetMode() != mode && !pSuit->SetMode(mode))
			{
				Reply(id.c_str(), Fail(string().Format("the suit refused %s (modes given: speed %d strength %d cloak %d armor %d)", m.c_str(),
					(int)pSuit->IsModeActive(NANOMODE_SPEED), (int)pSuit->IsModeActive(NANOMODE_STRENGTH), (int)pSuit->IsModeActive(NANOMODE_CLOAK),
					(int)pSuit->IsModeActive(NANOMODE_DEFENSE)).c_str()));
				return;
			}
			// (kept 20 s, the companion's own choice after that; "auto": at once;
			// a refused mode holds nothing)
			if (m != "auto")
				s_bot.suitHoldUntil = Now() + 20.0f;
			CryLogAlways("[CoopAgent] suit: %s by the agent (energy %.0f)", m.c_str(), pSuit->GetSuitEnergy());
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

	// %LOCALAPPDATA%\CrysisCoop: the companion's profile and files; the
	// autotester's games have their own (COOP_AUTOTEST_DIR), so a test never
	// touches the player's companion nor ends it as a leftover
	std::wstring CompanionBase()
	{
		wchar_t dir[MAX_PATH] = L"";
		if (GetEnvironmentVariableW(L"COOP_AUTOTEST_DIR", dir, MAX_PATH) && dir[0])
			return dir;
		GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
		return std::wstring(dir) + L"\\CrysisCoop";
	}

	std::wstring ReadyFile()
	{
		return CompanionBase() + L"\\companion\\host_ready";
	}

	std::wstring PortFile()
	{
		return CompanionBase() + L"\\companion\\agent.port";
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
		volatile LONG dropBig;    // testing: the host's packets over 100 bytes are dropped (a join that stalls)
		SLocalProxy(): game(INVALID_SOCKET), host(INVALID_SOCKET), haveGame(false), stop(false), thread(0), port(0), toHost(0), toGame(0), loggedHost(0), loggedGame(0), dropBig(0) {}
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
				if (n > 100 && s_proxy.dropBig)
					continue;
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

	// the relay ended: the next ProxyPort makes a new one (new ports on both
	// sides: the host may hold on to a stuck channel of the old address)
	void RenewProxy()
	{
		if (s_proxy.thread)
		{
			s_proxy.stop = true;
			WaitForSingleObject(s_proxy.thread, 2000);
			CloseHandle(s_proxy.thread);
		}
		if (s_proxy.game != INVALID_SOCKET)
			closesocket(s_proxy.game);
		if (s_proxy.host != INVALID_SOCKET)
			closesocket(s_proxy.host);
		s_proxy.game = s_proxy.host = INVALID_SOCKET;
		s_proxy.thread = 0;
		s_proxy.stop = false;
		s_proxy.haveGame = false;
		s_proxy.port = 0;
		s_proxy.toHost = s_proxy.toGame = s_proxy.loggedHost = s_proxy.loggedGame = 0;
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
		return CompanionBase() + L"\\companion\\companion.pid";
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
		// connected, and its level never begins to load: a join that stalled
		// in the network's setup (the channel up, both sides sending 44
		// packets a second, nothing more for minutes; no timeout of the
		// engine's ends it). Normal joins load within 9 s (110 of the
		// autotester's): after 45 s out, a new relay, in again; the third
		// time its game quits and the host starts it anew (the autotester,
		// 2026-10-16: "the AI companion cannot join your game")
		static int s_stalls = 0;
		static float s_renewAt = -1.0f;
		static ICVar* pNetLog = gEnv->pConsole->GetCVar("net_log");
		if (s_joinPhase == 1 && !CoopAI::LevelLoading() && !pFramework->GetClientActor() && Now() - s_connectedAt > 45.0f)
		{
			++s_stalls;
			CryLogAlways("[CoopAgent] connected %.0f s and its level never began to load (relay %d to the host, %d back): out, a new relay, in again (%d)",
				Now() - s_connectedAt, (int)s_proxy.toHost, (int)s_proxy.toGame, s_stalls);
			if (pNetLog)
				pNetLog->Set(0);
			if (s_stalls >= 3)
			{
				CryLogAlways("[CoopAgent] the join stalled 3 times: quitting, the host starts it again");
				TerminateProcess(GetCurrentProcess(), 3);
			}
			s_joinPhase = 0;
			gEnv->pConsole->ExecuteString("disconnect");
			s_renewAt = Now() + 1.0f;      // (the disconnect goes out first)
			s_nextConnect = Now() + 3.0f;
			return;
		}
		if (s_renewAt >= 0.0f && Now() >= s_renewAt && !in)
		{
			s_renewAt = -1.0f;
			RenewProxy();
		}
		if (s_joinPhase == 2 || pFramework->GetClientActor())
			s_stalls = 0;
		// (testing: COOP_TEST_JOIN_STALL=1 in its environment: the first try
		// stalls, the relay lets none of the host's big packets through once
		// the channel is up)
		static const bool s_testStall = getenv("COOP_TEST_JOIN_STALL") && atoi(getenv("COOP_TEST_JOIN_STALL")) != 0;
		s_proxy.dropBig = s_testStall && s_stalls == 0 && s_joinPhase == 1 ? 1 : 0;
		// the network's own log while it connects (a stall says where)
		if (pNetLog)
		{
			const int want = s_joinPhase == 1 ? 2 : 0;
			if (pNetLog->GetIVal() != want && (want || pNetLog->GetIVal() == 2))
				pNetLog->Set(want);
		}
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
					|| !_strnicmp(p, "s_SoundEnable", 13) || !_strnicmp(p, "s_DummySound", 12) || !_strnicmp(p, "r_Driver", 8)
					|| !_strnicmp(p, "g_startFirstTime", 16))
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
		// never the first start's machine spec detection: it sets the sound
		// console variables of its spec, which in this game without a sound
		// system point into freed memory - the engine's small-block heap was
		// damaged (3 test boots of 8: the crashes in MSVCR80 as the level
		// loads, 2026-10-22; CoopSound::GuardFailedSoundSystem refuses them
		// now too); a companion its host ends never saves the flag its first
		// start sets
		kept += "g_startFirstTime = 0\n";
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
		// (no LOCALAPPDATA: in the game's folder)
		std::wstring base = CompanionBase();
		if (base == L"\\CrysisCoop")
			base = root + L"\\CrysisCoop";
		const std::wstring profile = base + L"\\companion";
		CreateDirectoryW(base.c_str(), 0);
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
		// the autotester (tests\autotest) sets COOP_AUTOTEST for its games:
		// their companion's log and command file are its own, the player's
		// companion.log stays as his last game left it
		wchar_t autotest[8] = L"";
		const bool isAutotest = GetEnvironmentVariableW(L"COOP_AUTOTEST", autotest, 8) > 0 && autotest[0] != L'0';
		std::wstring line = L"\"" + std::wstring(exe) + L"\" -mod " + mod + (devmode ? L" -devmode" : L"") + L" -userpath \"" + profile + L"\" -logfile "
			+ (isAutotest ? L"at_companion.log" : L"companion.log") + wtail;
		// testing: the host runs the lines of a file as console commands, the
		// companion those of its own
		ICVar* pCmdFile = gEnv->pConsole->GetCVar("coop_test_cmdfile");
		if (pCmdFile && pCmdFile->GetString()[0])
			line += isAutotest ? L" +coop_test_cmdfile at_companion_cmd.txt" : L" +coop_test_cmdfile companion_cmd.txt";
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

	// how it fights from a place: 0 standing up, 1 leaning out to the left,
	// 2 to the right (facing the threat) sees the point; 3 not at all
	int PeekFrom(const Vec3& p, const Vec3& at, IPhysicalEntity** pSkip, int nSkip)
	{
		// (the gun's height: the eyes over the top and the gun below it was
		// no shot, all into the cover)
		if (!SolidBetween(p + Vec3(0, 0, 1.4f), at, pSkip, nSkip))
			return 0;
		Vec3 to = at - p;
		to.z = 0;
		to.NormalizeSafe(Vec3(0, 1, 0));
		const Vec3 left(-to.y, to.x, 0);
		if (!SolidBetween(p + Vec3(0, 0, 1.35f) + left * 0.4f, at, pSkip, nSkip))
			return 1;
		if (!SolidBetween(p + Vec3(0, 0, 1.35f) - left * 0.4f, at, pSkip, nSkip))
			return 2;
		return 3;
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
	// (the first hard of named must not see the place; tetherRadius > 0: none
	// farther from tether; prone: the place hides it only lying down)
	bool FindCover(IActor* pAgent, const std::vector<IActor*>& named, int hard, const Vec3& tether, float tetherRadius,
		std::vector<Vec3>& path, Vec3& cover, int& peek, bool& prone)
	{
		Vec3 from = pAgent->GetEntity()->GetWorldPos();
		prone = false;
		// (its feet on the ground below it: put back 6 m up after "falling
		// through the ground", every place round it was too far down and the
		// answer was "none found", the user's game of 2026-10-21)
		{
			// (only when it is in the air: on a crate, a deck or a boat its feet
			// are where it stands, the review)
			IPhysicalEntity* pSelf = pAgent->GetEntity()->GetPhysics();
			ray_hit floor;
			if (gEnv->pPhysicalWorld->RayWorldIntersection(from + Vec3(0, 0, 0.5f), Vec3(0, 0, -12.0f), ent_static | ent_terrain | ent_rigid | ent_sleeping_rigid,
				rwi_stop_at_pierceable | rwi_colltype_any, &floor, 1, &pSelf, pSelf ? 1 : 0) && from.z - floor.pt.z > 1.5f)
				from.z = floor.pt.z;
		}
		// the threats: those named (who shot it first), every soldier after
		// it (its attention on it: it hid from the one it aimed at while
		// others shot it, 11 of 18 times in the user's games), the nearest
		// others: up to 8 in all
		std::vector<IActor*> threats(named.begin(), named.end());
		std::vector<std::pair<float, IActor*> > others;
		IAIObject* pAgentAI = pAgent->GetEntity()->GetAI();
		IActorIteratorPtr it = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
		while (IActor* pActor = it->Next())
			if (std::find(threats.begin(), threats.end(), pActor) == threats.end() && HostileTo(pActor, pAgent))
			{
				const float d = pActor->GetEntity()->GetWorldPos().GetDistance(from);
				IAIObject* pAI = pActor->GetEntity()->GetAI();
				IPipeUser* pPipe = pAI ? pAI->CastToIPipeUser() : 0;
				const bool after = pPipe && pAgentAI && pPipe->GetAttentionTarget() == pAgentAI;
				if (d < 60.0f || (after && d < 120.0f))
					others.push_back(std::make_pair(after ? d - 1000.0f : d, pActor));
			}
		std::sort(others.begin(), others.end());
		for (size_t i = 0; i < others.size() && threats.size() < 8; ++i)
			threats.push_back(others[i].second);
		std::vector<Vec3> eyes, feet;
		std::vector<IPhysicalEntity*> rides;   // a threat's own vehicle (a truck's gunner: its truck is no wall between)
		for (size_t i = 0; i < threats.size(); ++i)
		{
			eyes.push_back(EyePos(threats[i]));
			feet.push_back(threats[i]->GetEntity()->GetWorldPos());
			IVehicle* pRide = threats[i]->GetLinkedVehicle();
			rides.push_back(pRide ? pRide->GetEntity()->GetPhysics() : 0);
		}
		// burning wrecks to be: no cover next to a vehicle shot to pieces
		// (it hid by a car the soldiers were shooting and died in its
		// explosion, the user's game of 2026-10-17)
		std::vector<Vec3> wrecks;
		IVehicleIteratorPtr pVehicles = g_pGame->GetIGameFramework()->GetIVehicleSystem()->CreateVehicleIterator();
		while (IVehicle* pVehicle = pVehicles->Next())
			if (!pVehicle->IsDestroyed() && pVehicle->GetDamageRatio() > 0.5f && pVehicle->GetEntity()->GetWorldPos().GetDistance(from) < 45.0f)
				wrecks.push_back(pVehicle->GetEntity()->GetWorldPos());
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
			int peek;
			bool lying;     // hidden there only lying down
			bool operator<(const SCandidate& o) const { return score < o.score; }
		};
		std::vector<SCandidate> candidates;
		// (where it stands first, then 2 m round it: the nearest cover is the
		// best, every fall back was a run of 4 m and more under fire)
		static const float s_radii[] = { 0.0f, 2.0f, 4.0f, 7.0f, 11.0f, 15.0f, 20.0f, 25.0f, 30.0f };
		hard = max(1, min(hard, (int)eyes.size()));
		for (int r = 0; r < 9; ++r)
			for (int k = 0; k < (r == 0 ? 1 : r == 1 ? 12 : 24); ++k)
			{
				const float a = k * gf_PI2 / (r == 1 ? 12.0f : 24.0f);
				Vec3 p = from + Vec3(cosf(a), sinf(a), 0) * s_radii[r];
				// (held to a downed teammate: none farther from him)
				if (tetherRadius > 0.0f && Dist2D(p, tether) > tetherRadius)
					continue;
				// the ground there (not on a roof, not down a cliff)
				ray_hit hit;
				if (!gEnv->pPhysicalWorld->RayWorldIntersection(Vec3(p.x, p.y, from.z + 3.0f), Vec3(0, 0, -8.0f),
					ent_static | ent_terrain, rwi_stop_at_pierceable | rwi_colltype_any, &hit, 1, skip, nSkip))
					continue;
				p = hit.pt;
				if (fabsf(p.z - from.z) > 3.0f)
					continue;
				// not next to any of them (it fell back from 13 m to 1 m of a
				// shotgunner), nor next to a wreck to be
				bool close = false;
				for (size_t e = 0; e < feet.size() && !close; ++e)
					close = Dist2D(p, feet[e]) < 8.0f;
				for (size_t w = 0; w < wrecks.size() && !close; ++w)
					close = Dist2D(p, wrecks[w]) < 8.0f;
				if (close)
					continue;
				// hidden: solid between each of their eyes and a crouching
				// head there. Those who hit it a moment ago (the first hard
				// named) must all be hidden from, the others count; seen by
				// one of those, no more rays for this spot. Not hidden so:
				// lying down (a head 0.4 m up), a second choice
				int seenBy = 0;
				bool hardSees = false;
				for (size_t e = 0; e < eyes.size() && !hardSees; ++e)
				{
					IPhysicalEntity* skipE[2];
					int nE = 0;
					if (nSkip)
						skipE[nE++] = skip[0];
					if (rides[e])
						skipE[nE++] = rides[e];
					if (!SolidBetween(eyes[e], p + Vec3(0, 0, 1.0f), skipE, nE))
					{
						hardSees = (int)e < hard;
						++seenBy;
					}
				}
				bool lying = false;
				if (hardSees)
				{
					for (int e = 0; e < hard && !lying; ++e)
					{
						IPhysicalEntity* skipE[2];
						int nE = 0;
						if (nSkip)
							skipE[nE++] = skip[0];
						if (rides[e])
							skipE[nE++] = rides[e];
						if (!SolidBetween(eyes[e], p + Vec3(0, 0, 0.4f), skipE, nE))
							lying = true;    // (seen lying down too: no use)
					}
					if (lying)
						continue;
					lying = true;
				}
				// (nearer to those who shot it costs; the others, soldiers that
				// never fired among them, are not to step away from: each new
				// ask went farther off, 4 m at a time, the user's game)
				float closer = 0.0f;
				for (int e = 0; e < hard; ++e)
					closer += max(0.0f, Dist2D(from, eyes[e]) - Dist2D(p, eyes[e]));
				// (where it fights from there: looked at below, for the best few)
				const SCandidate c = { s_radii[r] + closer * 2.0f + seenBy * 6.0f + max(0.0f, Dist2D(p, host) - 20.0f) * 0.5f + (lying ? 4.0f : 0.0f), p, -1, lying };
				candidates.push_back(c);
			}
		// (no hidden place close by: none. It was the place farthest from them
		// 22 m off; the companion lies down where it is now and shoots back)
		std::sort(candidates.begin(), candidates.end());
		// a place to fight from: standing up, or leaning out to a side, it
		// sees the first threat's chest (a blind peek is no shot), for the
		// 12 best only (3 rays each)
		if (!threats.empty())
		{
			IPhysicalEntity* skip0[2];
			int n0 = 0;
			if (nSkip)
				skip0[n0++] = skip[0];
			if (rides[0])
				skip0[n0++] = rides[0];
			for (size_t i = 0; i < candidates.size() && i < 12; ++i)
				if (candidates[i].peek < 0)
				{
					candidates[i].peek = PeekFrom(candidates[i].pos, eyes[0] - Vec3(0, 0, 0.3f), skip0, n0);
					if (candidates[i].peek == 3)
						candidates[i].score += 6.0f;
				}
			std::sort(candidates.begin(), candidates.begin() + min(candidates.size(), (size_t)12));
		}
		for (size_t i = 0; i < candidates.size() && i < 8; ++i)
		{
			if (candidates[i].peek < 0)
				candidates[i].peek = 3;
			// (where it stands, or a step off with nothing between and level:
			// no way to ask for. Behind a wall 2 m off is not a step, the review)
			const Vec3 c = candidates[i].pos;
			IPhysicalEntity* pSelfPhys = pAgent->GetEntity()->GetPhysics();
			if (Dist2D(from, c) < 0.5f
				|| (Dist2D(from, c) < 2.5f && fabsf(c.z - from.z) < 0.5f
					&& !SolidBetween(from + Vec3(0, 0, 0.5f), c + Vec3(0, 0, 0.5f), &pSelfPhys, pSelfPhys ? 1 : 0)
					&& !SolidBetween(from + Vec3(0, 0, 1.2f), c + Vec3(0, 0, 1.2f), &pSelfPhys, pSelfPhys ? 1 : 0)))
			{
				path.clear();
				path.push_back(candidates[i].pos);
				cover = candidates[i].pos;
				peek = candidates[i].peek;
				prone = candidates[i].lying;
				return true;
			}
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
				peek = candidates[i].peek;
				prone = candidates[i].lying;
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

	// coop_test_sounds [radius]: every sound that plays on an entity near
	// this game's player (its name, the entity, looping or not): a fire loop
	// that never ends (the user's game of 2026-10-17)
	void CmdTestSounds(IConsoleCmdArgs* pArgs)
	{
		IActor* pMe = g_pGame->GetIGameFramework()->GetClientActor();
		const float radius = pArgs->GetArgCount() > 1 ? (float)atof(pArgs->GetArg(1)) : 150.0f;
		const Vec3 at = pMe ? pMe->GetEntity()->GetWorldPos() : Vec3(ZERO);
		int n = 0, loops = 0;
		IEntityItPtr it = gEnv->pEntitySystem->GetEntityIterator();
		it->MoveFirst();
		while (IEntity* pEntity = it->Next())
		{
			if (pEntity->IsGarbage() || (pMe && pEntity->GetWorldPos().GetDistance(at) > radius))
				continue;
			IEntitySoundProxy* pProxy = (IEntitySoundProxy*)pEntity->GetProxy(ENTITY_PROXY_SOUND);
			if (!pProxy)
				continue;
			for (uint32 i = 0; i < 64; ++i)
			{
				ISound* pSound = pProxy->GetSoundByIndex(i);
				if (!pSound)
					break;
				if (!pSound->IsPlaying() && !pSound->IsPlayingVirtual())
					continue;
				const bool loop = (pSound->GetFlags() & FLAG_SOUND_LOOP) != 0;
				++n;
				loops += loop;
				CryLogAlways("[CoopTest] sound %s on %s (%s) %s%s, %.0f m off", pSound->GetName(), pEntity->GetName(), pEntity->GetClass()->GetName(),
					loop ? "LOOP" : "once", pSound->IsPlaying() ? "" : " (virtual)", pEntity->GetWorldPos().GetDistance(at));
			}
		}
		CryLogAlways("[CoopTest] sounds playing within %.0f m: %d (%d looping)", radius, n, loops);
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
			at.x, at.y, at.z, g.n, g.n, g.cell);
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
			CryLogAlways("[CoopTest] grid %+5.1f |%s|", g.y0 + (cy + 0.5f) * g.cell - at.y, row.c_str());
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
		// coop_test_enter out [m/s]: the host gets out (jumps out of it going
		// that fast forward)
		if (pHost && pArgs->GetArgCount() > 1 && !stricmp(pArgs->GetArg(1), "out"))
		{
			IVehicle* pIn = pHost->GetLinkedVehicle();
			IVehicleSeat* pMine = pIn ? pIn->GetSeatForPassenger(pHost->GetEntityId()) : 0;
			IPhysicalEntity* pPhys = pIn ? pIn->GetEntity()->GetPhysics() : 0;
			if (pPhys && pArgs->GetArgCount() > 2)
			{
				pe_action_set_velocity v;
				v.v = pIn->GetEntity()->GetWorldTM().GetColumn1().GetNormalizedSafe(Vec3(0, 1, 0)) * (float)atof(pArgs->GetArg(2));
				pPhys->Action(&v);
				CryLogAlways("[CoopTest] %s set going %.1f m/s", pIn->GetEntity()->GetName(), v.v.GetLength());
			}
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

	// coop_test_vehinfo [m]: how this game updates the vehicles near its
	// player (seen, near, update slots)
	void CmdTestVehInfo(IConsoleCmdArgs* pArgs)
	{
		IActor* pMe = g_pGame->GetIGameFramework()->GetClientActor();
		if (!pMe)
			return;
		const float range = pArgs->GetArgCount() > 1 ? (float)atof(pArgs->GetArg(1)) : 100.0f;
		const Vec3 at = pMe->GetEntity()->GetWorldPos();
		IGameObject* pMine = pMe->GetGameObject();
		CryLogAlways("[CoopTest] vehinfo: me %s at (%.0f %.0f %.0f), my game object visible %d distant %d, channel %d", pMe->GetEntity()->GetName(),
			at.x, at.y, at.z, pMine ? (int)pMine->IsProbablyVisible() : -1, pMine ? (int)pMine->IsProbablyDistant() : -1, pMine ? (int)pMine->GetChannelId() : -1);
		IVehicleIteratorPtr pIt = g_pGame->GetIGameFramework()->GetIVehicleSystem()->CreateVehicleIterator();
		while (IVehicle* pVehicle = pIt->Next())
		{
			const float d = pVehicle->GetEntity()->GetWorldPos().GetDistance(at);
			if (d <= range)
				CryLogAlways("[CoopTest] vehinfo: %s %.0f m: %s", pVehicle->GetEntity()->GetName(), d, VehicleUpdateInfo(pVehicle).c_str());
		}
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
	// which build runs, for whoever reads the log (the autotester checks it
	// is the one it put there)
	{
		char dll[MAX_PATH] = "";
		GetModuleFileNameA((HMODULE)g_hInst, dll, MAX_PATH);
		CryLogAlways("[CoopBuild] version=%s built=%s_%s dll=%s", PROJECT_VERSION, __DATE__, __TIME__, dll);
	}
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
	gEnv->pConsole->AddCommand("coop_test_sounds", CmdTestSounds, 0, "Crysis Coop testing: coop_test_sounds [radius]: the sounds playing on entities near this game's player (looping ones marked)");
	gEnv->pConsole->AddCommand("coop_test_vehicles", CmdTestVehicles, 0, "Crysis Coop testing: coop_test_vehicles [all] logs the vehicles near the host's player (all: everywhere), their seats and engines");
	gEnv->pConsole->AddCommand("coop_test_silencer", CmdTestSilencer, 0, "Crysis Coop testing: coop_test_silencer <player> [0]: a silencer on (off) his weapon (server)");
	gEnv->pConsole->AddCommand("coop_test_deactivate", CmdTestDeactivate, 0, "Crysis Coop testing: coop_test_deactivate: this game's window stops being the active one, as when its player goes to another window");
	gEnv->pConsole->AddCommand("coop_test_onehand", CmdTestOneHand, 0, "Crysis Coop testing: coop_test_onehand <player> [grab]: his weapon in the one-hand pose, or (grab) his left hand held up (this game)");
	gEnv->pConsole->AddCommand("coop_test_anim", CmdTestAnim, 0, "Crysis Coop testing: coop_test_anim <player>: logs his animation graph inputs as this game plays them");
	gEnv->pConsole->RegisterInt("coop_test_gun_awake", 1, VF_NOT_NET_SYNCED, "Crysis Coop testing: 0 = the companion at a vehicle's gun does not tell the vehicle to update every frame (default 1)");
	// (in the sights the game took a shot's direction from the system camera
	// (zoom sway), which the companion's game without a renderer never moves:
	// its shots went 27-52 degrees off; PlayerMovementController uses its own
	// view for it now)
	gEnv->pConsole->RegisterInt("coop_agent_zoom", 1, VF_NOT_NET_SYNCED, "Crysis Coop: 1 = the AI companion puts its weapon's sights up when it fights up from cover at a target over 10 m off (0: never)");
	gEnv->pConsole->RegisterInt("coop_test_turret_hold", 0, VF_NOT_NET_SYNCED, "Crysis Coop testing: 1 = the companion's game keeps CryAction's world hold of a turret's yaw it guns (it spun the gun once the car turned) and no turn against the car's");
	gEnv->pConsole->RegisterInt("coop_test_guntrace", 0, VF_NOT_NET_SYNCED, "Crysis Coop testing: 1 = the companion at a vehicle's gun logs each frame what it told the turret and how the view moved");
	gEnv->pConsole->RegisterInt("coop_test_vehicle_force", 1, VF_NOT_NET_SYNCED, "Crysis Coop testing: 0 = the companion's game leaves the update of the vehicle it rides in to the engine (seen or near only; default 1)");
	gEnv->pConsole->AddCommand("coop_test_key", CmdTestKey, 0, "Crysis Coop testing: coop_test_key <action> <1|0>: this game's player presses (lets go of) a key's action");
	gEnv->pConsole->AddCommand("coop_test_cam", CmdTestCam, 0, "Crysis Coop testing: coop_test_cam logs this game's camera, its view and the vehicle seat's view");
	gEnv->pConsole->AddCommand("coop_test_vehview", CmdTestVehView, 0, "Crysis Coop testing: coop_test_vehview: the vehicle seat's next view for this game's player");
	gEnv->pConsole->AddCommand("coop_test_grid", CmdTestGrid, 0, "Crysis Coop testing: coop_test_grid [x y z] [radius] [to x y z]: the squares the companion finds its way round on, and the way to a place");
	gEnv->pConsole->AddCommand("coop_test_findobj", CmdTestFindObj, 0, "Crysis Coop testing: coop_test_findobj [x y z] [radius] [filter]: physical things near a place, their model, parts and breakable joints");
	gEnv->pConsole->AddCommand("coop_test_physvars", CmdTestPhysVars, 0, "Crysis Coop testing: coop_test_physvars [multiplayer|breakscale|playerscanbreak value]: the physics settings that decide breaking");
	gEnv->pConsole->AddCommand("coop_test_vehdamage", CmdTestVehDamage, 0, "Crysis Coop testing: coop_test_vehdamage [vehicle]: a vehicle's damage, whole and by component");
	gEnv->pConsole->AddCommand("coop_test_levelmodels", CmdTestLevelModels, 0, "Crysis Coop testing: coop_test_levelmodels: the level's own brush models found / default balls");
	gEnv->pConsole->RegisterInt("coop_test_shottrace", 0, 0, "Crysis Coop testing: log the players' shots from vehicle guns (where from, at what, what the line meets)");
	gEnv->pConsole->AddCommand("coop_test_enter", CmdTestEnter, 0, "Crysis Coop testing: coop_test_enter <vehicle> <seat id>: the host's player gets in (server)");
	gEnv->pConsole->AddCommand("coop_test_vehinfo", CmdTestVehInfo, 0, "Crysis Coop testing: coop_test_vehinfo [m]: how this game updates the vehicles near its player");
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
		// (a turn goes to the server as a mouse turn does, CPlayerInput::OnAction
		// marks the input changed: without it the host's copy kept the old look
		// until a key press and its body did not turn to what it shot at)
		static ICVar* pLookNet = gEnv->pConsole->GetCVar("coop_agent_look_net");
		if (fabsf(deltaRotation.z) + fabsf(deltaRotation.x) > 0.0005f && (!pLookNet || pLookNet->GetIVal()))
			pPlayer->GetGameObject()->ChangedNetworkState(IPlayerInput::INPUT_ASPECT);
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
	// (lying down: no cover close by under fire, or the cover hides it only so)
	if (s_ctl.prone && !s_ctl.crouch)
		actions |= ACTION_PRONE;
	else
		actions &= ~ACTION_PRONE;
	if (s_ctl.jump)
	{
		actions |= ACTION_JUMP;
		s_ctl.jump = false;
	}
	else
		actions &= ~ACTION_JUMP;
	// (held every frame: the player input lets go of a lean each frame)
	actions &= ~(ACTION_LEANLEFT | ACTION_LEANRIGHT);
	const int lean = Now() < s_bot.leanHoldUntil ? s_bot.leanHoldSide : s_ctl.lean;
	if (lean < 0)
		actions |= ACTION_LEANLEFT;
	else if (lean > 0)
		actions |= ACTION_LEANRIGHT;
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
		// only sunk here - the copy here fell through the ground, in its own
		// game it stands: the copy back up to where its own game has it, its
		// fall stopped and no landing counted, at once. It trusted this copy
		// once: put the companion 33 m under the graveyard, the stop of the
		// fall killed it, the body sank under the map (the user's game,
		// 2026-10-23)
		const float groundHere = gEnv->p3DEngine ? gEnv->p3DEngine->GetTerrainElevation(here.x, here.y) : here.z;
		if (off < 2.0f && here.z < there.z - 2.5f)
		{
			times = 0;
			CoopAgent::ShieldFall(agent, 2.0f);
			if (IPhysicalEntity* pPhys = pAgent->GetEntity()->GetPhysics())
			{
				pe_action_set_velocity still;
				still.v.zero();
				pPhys->Action(&still);
			}
			Matrix34 tm = pAgent->GetEntity()->GetWorldTM();
			tm.SetTranslation(there);
			pAgent->GetEntity()->SetWorldTM(tm);
			CryLogAlways("[CoopAgent] %s sank %.1f m here (z %.1f, the ground %.1f), stands at z %.1f in its own game: put back up here", pAgent->GetEntity()->GetName(),
				there.z - here.z, here.z, groundHere, there.z);
			return;
		}
		if (++times < 2)
			return;
		times = 0;
		// (never put under the map: under the terrain with nothing to stand on
		// - a mine's tunnel under the terrain has its floor)
		ray_hit floor;
		IPhysicalEntity* pSelf = pAgent->GetEntity()->GetPhysics();
		if (here.z < groundHere - 1.0f && !gEnv->pPhysicalWorld->RayWorldIntersection(here + Vec3(0, 0, 0.5f), Vec3(0, 0, -3.0f), ent_static | ent_terrain,
			rwi_stop_at_pierceable | rwi_colltype_any, &floor, 1, &pSelf, pSelf ? 1 : 0))
		{
			CryLogAlways("[CoopAgent] %s is %.1f m under the ground here (z %.1f): not put back here, at %.1f %.1f %.1f in its own game", pAgent->GetEntity()->GetName(),
				groundHere - here.z, here.z, there.x, there.y, there.z);
			return;
		}
		pRules->MovePlayer(static_cast<CActor*>(pAgent), here, pAgent->GetEntity()->GetWorldAngles());
		CryLogAlways("[CoopAgent] %s was %.1f m off in its own game (%.1f up or down; at %.1f %.1f %.1f, here %.1f %.1f %.1f): put back here",
			pAgent->GetEntity()->GetName(), off, there.z - here.z, there.x, there.y, there.z, here.x, here.y, here.z);
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
		// (the names: "A,B|C,D@x,y,z,r": the cover hides it from A and B,
		// who hit it a moment ago; C and D only count; none farther than r
		// from x y z. No "|": the first one alone, as before)
		std::vector<IActor*> threats;
		string names = name;
		Vec3 tether(ZERO);
		float tetherRadius = 0.0f;
		const size_t atSign = names.find('@');
		if (atSign != string::npos)
		{
			if (sscanf(names.c_str() + atSign + 1, "%f,%f,%f,%f", &tether.x, &tether.y, &tether.z, &tetherRadius) != 4)
				tetherRadius = 0.0f;
			names = names.substr(0, atSign);
		}
		int hard = -1;
		for (int start = 0; start <= (int)names.length(); )
		{
			int end = start;
			while (end < (int)names.length() && names[end] != ',' && names[end] != '|')
				++end;
			if (end > start)
				if (IActor* p = ActorByName(names.substr(start, end - start).c_str()))
					if (std::find(threats.begin(), threats.end(), p) == threats.end())
						threats.push_back(p);
			if (end < (int)names.length() && names[end] == '|')
				hard = (int)threats.size();
			start = end + 1;
		}
		std::vector<Vec3> path;
		Vec3 cover(ZERO);
		int peek = 3;
		bool prone = false;
		string text, goal;
		if (!threats.empty() && FindCover(pAgent, threats, hard < 1 ? 1 : hard, tether, tetherRadius, path, cover, peek, prone))
			for (size_t i = 0; i < path.size() && i < 20; ++i)
				text += string().Format("%s%.1f %.1f %.1f", i ? ";" : "", path[i].x, path[i].y, path[i].z);
		goal.Format("%.1f %.1f %.1f %d %d", cover.x, cover.y, cover.z, peek, prone ? 1 : 0);
		static const char* const s_peeks[] = { "standing up", "leaning left", "leaning right", "no shot from there" };
		CryLogAlways("[CoopAgent] cover for %s from %s: %s", pAgent->GetEntity()->GetName(), name,
			text.empty() ? "none found" : string().Format("%.0f m away, %d points, fights %s%s", cover.GetDistance(pAgent->GetEntity()->GetWorldPos()),
				(int)path.size(), s_peeks[peek], prone ? ", lying down" : "").c_str());
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

void CoopAgent::OnHitIndicator(bool explosion)
{
	if (!IsCompanion() || !s_bot.enemy)
		return;
	SMyHits& h = s_myHits[s_bot.enemy];
	if (Now() - h.last > 15.0f)
		h.n = 0;
	++h.n;
	h.last = Now();
	++s_bot.hitsConfirmed;
}

void CoopAgent::OnDamaged(EntityId shooterId, EntityId weaponId)
{
	if (!IsCompanion())
		return;
	CPlayer* pMe = LocalPlayer();
	IEntity* pShooter = gEnv->pEntitySystem->GetEntity(shooterId);
	if (!pMe || !pShooter || shooterId == pMe->GetEntityId())
		return;
	const float now = Now();
	SThreat& t = s_threats[shooterId];
	if (now - t.last > 20.0f)
	{
		t.first = now;
		t.hits = 0;
	}
	t.last = now;
	++t.hits;
	t.pos = pShooter->GetWorldPos();
	s_bot.hitAt = now;
	s_hitTimes.push_back(now);
	while (!s_hitTimes.empty() && now - s_hitTimes.front() > 8.0f)
		s_hitTimes.erase(s_hitTimes.begin());
	const Vec3 pos = pMe->GetEntity()->GetWorldPos();
	const bool inCover = s_bot.haveCover && Dist2D(s_bot.cover, pos) < 1.5f;
	IEntity* pWeapon = gEnv->pEntitySystem->GetEntity(weaponId);
	// (a truck's machine gun takes 70 energy a hit: the armor gone in three)
	if (pWeapon)
	{
		const char* cls = pWeapon->GetClass()->GetName();
		// (one shooting from a vehicle, or a heavy weapon by name: an energy
		// drop says nothing, one rifle hit takes 49 of it; the helicopter's,
		// the tank's and the APC's guns have names of their own)
		IActor* pShooterActor = ActorOf(shooterId);
		// (a mounted gun on the ground, "ShiTen", is not: fleeing its nest
		// gave up every such fight, it fights it from cover instead)
		if ((pShooterActor && pShooterActor->GetLinkedVehicle()) || !strnicmp(cls, "Vehicle", 7) || strstr(cls, "Cannon")
			|| strstr(cls, "TACGun") || strstr(cls, "AlienMount") || strstr(cls, "Coaxial") || strstr(cls, "50Cal") || strstr(cls, "Hellfire")
			|| strstr(cls, "RocketLauncher") || !stricmp(cls, "Hurricane") || !stricmp(cls, "LAW") || !stricmp(cls, "GaussRifle"))
			s_bot.heavyHitAt = now;
	}
	CryLogAlways("[CoopAgent] shot by %s (%s, %.0f m, %s, health %d, energy %.0f)", pShooter->GetName(), pWeapon ? pWeapon->GetClass()->GetName() : "-",
		pos.GetDistance(t.pos), s_bot.retreating ? "falling back" : inCover ? (s_bot.peekUp ? "peeking" : "hidden") : s_bot.reviveSince >= 0.0f ? "reviving" : "open",
		pMe->GetHealth(), pMe->GetNanoSuit() ? pMe->GetNanoSuit()->GetSuitEnergy() : 0.0f);
	// (forget those long gone quiet)
	for (std::map<EntityId, SThreat>::iterator it = s_threats.begin(); it != s_threats.end(); )
		if (now - it->second.last > 30.0f)
			s_threats.erase(it++);
		else
			++it;
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
	s_bot.waitWayUntil = 0.0f;
	// (no way there in the host's navigation either: its own squares' best)
	if (op == 0 && path.empty())
	{
		s_bot.pathNoneAt = Now();
		s_bot.pathNoneGoal = g;
	}
	if (op == 1)
	{
		// cover: where it falls back to (none found: away, as it can), and
		// how it fights from there
		s_bot.haveCover = !path.empty();
		if (s_bot.haveCover)
		{
			int peek = 3, prone = 0;
			Vec3 at;
			const int got = goal ? sscanf(goal, "%f %f %f %d %d", &at.x, &at.y, &at.z, &peek, &prone) : 0;
			s_bot.coverPeek = got >= 4 ? peek : 0;
			s_bot.coverProne = got >= 5 && prone != 0;
			s_bot.coverReachedAt = -1.0f;
			s_bot.proneUntil = 0.0f;
			s_bot.coverBest = 1e9f;
			s_bot.coverBestAt = Now();
			s_bot.cover = g;
			s_bot.coverAt = Now();
			s_bot.peekUp = false;
			s_bot.peekSince = Now();
			s_bot.peekSide = s_bot.coverPeek == 2 ? 1 : -1;
			// (asked in the open, not shot at: one it can not shoot from takes
			// it out of the fight for nothing)
			if (s_bot.coverOpenAsk && s_bot.coverPeek == 3)
			{
				s_bot.haveCover = false;
				s_bot.coverAskAt = Now() + 6.0f;
			}
		}
	}
	if (!path.empty())
		CoopAI::Trace("AGENT way: %d points to %s", (int)path.size(), goal ? goal : "");
}

// the server: no fall damage for a player for a moment, see CoopAgent.h
namespace
{
	std::map<EntityId, float> s_fallShield;
}

void CoopAgent::ShieldFall(EntityId player, float seconds)
{
	s_fallShield[player] = gEnv->pTimer->GetCurrTime() + seconds;
}

bool CoopAgent::FallShielded(EntityId player)
{
	std::map<EntityId, float>::iterator it = s_fallShield.find(player);
	return it != s_fallShield.end() && gEnv->pTimer->GetCurrTime() < it->second;
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
