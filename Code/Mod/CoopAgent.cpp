// Crysis Coop: the AI companion (see CoopAgent.h).
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")

#include "StdAfx.h"
#include "CoopAgent.h"
#include "CoopAI.h"
#include "Game.h"
#include "GameActions.h"
#include "GameRules.h"
#include "IPlayerInput.h"
#include "Player.h"
#include "NanoSuit.h"
#include "IVehicleSystem.h"
#include "IViewSystem.h"
#include "IItemSystem.h"
#include "IWeapon.h"
#include "ILevelSystem.h"
#include "INetworkService.h"

#include <algorithm>
#include <deque>
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

	// nothing solid between the eye and the target
	bool Visible(IActor* pFrom, const Vec3& eye, IEntity* pTarget, const Vec3& to)
	{
		IPhysicalEntity* skip[2];
		int n = 0;
		if (IPhysicalEntity* p = pFrom->GetEntity()->GetPhysics())
			skip[n++] = p;
		if (IVehicle* pVehicle = pFrom->GetLinkedVehicle())
			if (IPhysicalEntity* p = pVehicle->GetEntity()->GetPhysics())
				skip[n++] = p;
		ray_hit hit;
		const Vec3 dir = to - eye;
		const int hits = gEnv->pPhysicalWorld->RayWorldIntersection(eye, dir, ent_all, rwi_stop_at_pierceable | rwi_colltype_any, &hit, 1, skip, n);
		if (!hits)
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
		// moving
		Vec3 checkPos;
		float checkAt;
		int stuck;
		float strafeUntil, strafeSide;
		bool jump;
		float farSince, askAt;
		// reviving
		bool useHeld;
		EntityId useFor;
		float useSince;
		// what was seen before (events)
		std::map<EntityId, bool> downSeen;
		float lastHealth;
		bool wasDown;
		bool named;
		SBrain(): order(eO_Free), spot(ZERO), haveSpot(false), fireAtWill(true), enemy(0), burstUntil(0), pauseUntil(0), reloadAt(0),
			switchAt(0), attackHeld(false), checkPos(ZERO), checkAt(0), stuck(0), strafeUntil(0), strafeSide(1), jump(false),
			farSince(-1), askAt(0), useHeld(false), useFor(0), useSince(0), lastHealth(-1), wasDown(false), named(false) {}
	} s_bot;

	// what SteerInput hands the player input
	struct SControl
	{
		Vec3 moveDir;           // world, horizontal (zero: stand)
		bool look;              // turn to yaw/pitch
		float yaw, pitch;
		float turnRate;         // radians a second
		bool sprint;
		bool jump;
		SControl(): moveDir(ZERO), look(false), yaw(0), pitch(0), turnRate(6.0f), sprint(false), jump(false) {}
	} s_ctl;

	void Press(CPlayer* pPlayer, const ActionId& action, bool press)
	{
		if (IPlayerInput* pInput = pPlayer->GetPlayerInput())
			pInput->OnAction(action, press ? eAAM_OnPress : eAAM_OnRelease, press ? 1.0f : 0.0f);
	}

	void Fire(CPlayer* pPlayer, bool on)
	{
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

	// walk toward a point; true when there
	bool MoveTo(CPlayer* pMe, const Vec3& to, float stopAt, bool mayRun, bool mayCatchUp, const char* toName)
	{
		const Vec3 pos = pMe->GetEntity()->GetWorldPos();
		Vec3 d = to - pos;
		d.z = 0;
		const float dist = d.GetLength();
		if (dist <= stopAt)
		{
			s_ctl.moveDir.zero();
			s_bot.stuck = 0;
			s_bot.checkAt = Now() + 1.0f;
			s_bot.checkPos = pos;
			s_bot.farSince = -1;
			return true;
		}
		Vec3 dir = d / dist;
		// stuck: jump, then step aside, then ask to be put next to the one it follows
		if (Now() >= s_bot.checkAt)
		{
			const float moved = (pos - s_bot.checkPos).GetLength2D();
			s_bot.stuck = moved < 0.5f ? s_bot.stuck + 1 : 0;
			s_bot.checkPos = pos;
			s_bot.checkAt = Now() + 1.0f;
			if (s_bot.stuck == 1 || s_bot.stuck == 3)
				s_ctl.jump = true;
			if (s_bot.stuck == 2 || s_bot.stuck == 4)
			{
				s_bot.strafeUntil = Now() + 1.2f;
				s_bot.strafeSide = -s_bot.strafeSide;
			}
			if (s_bot.stuck >= 6 && mayCatchUp && dist > 8.0f)
			{
				Ask(1, toName);
				s_bot.stuck = 0;
			}
		}
		if (Now() < s_bot.strafeUntil)
		{
			const Vec3 side(dir.y * s_bot.strafeSide, -dir.x * s_bot.strafeSide, 0);
			dir = (dir * 0.3f + side).GetNormalized();
		}
		// far behind for a while: put next to the one it follows
		if (mayCatchUp && dist > 60.0f)
		{
			if (s_bot.farSince < 0)
				s_bot.farSince = Now();
			else if (Now() - s_bot.farSince > 4.0f)
			{
				Ask(1, toName);
				s_bot.farSince = -1;
			}
		}
		else
			s_bot.farSince = -1;
		s_ctl.moveDir = dir;
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

	void Think(float frameTime)
	{
		CPlayer* pMe = LocalPlayer();
		s_ctl.moveDir.zero();
		s_ctl.look = false;
		s_ctl.sprint = false;
		s_ctl.jump = false;
		if (!pMe || !InGame(pMe))
		{
			s_bot.doing = pMe ? "waiting to spawn" : "not in a game";
			return;
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
			Event("you were hit: health %d", pMe->GetHealth());
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
		if (down || (pView && pView->IsPlayingCutScene()))
		{
			Fire(pMe, false);
			HoldUse(pMe, false, 0);
			s_bot.doing = down ? "down, waiting for a teammate" : "watching a cutscene";
			return;
		}
		// the name the host sees
		if (!s_bot.named && s_pName->GetString()[0] && strcmp(pMe->GetEntity()->GetName(), s_pName->GetString()))
		{
			s_bot.named = true;
			string cmd;
			cmd.Format("name %s", s_pName->GetString());
			gEnv->pConsole->ExecuteString(cmd.c_str());
		}

		const Vec3 pos = pMe->GetEntity()->GetWorldPos();
		const Vec3 eye = EyePos(pMe);
		IVehicle* pMyVehicle = pMe->GetLinkedVehicle();

		// the one it follows
		IActor* pLeader = s_bot.leader.empty() ? HostPlayer() : ActorByName(s_bot.leader.c_str());
		if (!pLeader)
			pLeader = HostPlayer();

		// ---- reviving comes first (unless ordered to stay)
		IActor* pDowned = (s_bot.order == eO_Hold || s_bot.order == eO_Goto) ? 0 : DownedTeammate(pMe, pos, 80.0f);
		if (pDowned && !pMyVehicle)
		{
			Fire(pMe, false);
			const Vec3 body = pDowned->GetEntity()->GetWorldPos();
			const float d = Dist2D(body, pos);
			if (!MoveTo(pMe, body, 1.6f, true, false, pDowned->GetEntity()->GetName()))
			{
				HoldUse(pMe, false, 0);
				LookAt(eye, body + Vec3(0, 0, 1.2f), 6.0f);
				s_bot.doing.Format("running to revive %s (%.0f m)", pDowned->GetEntity()->GetName(), d);
			}
			else
			{
				LookAt(eye, body, 6.0f);
				// the use key goes to the nearest downed teammate within reach
				if (!s_bot.useHeld || s_bot.useFor != pDowned->GetEntityId())
				{
					HoldUse(pMe, false, 0);
					HoldUse(pMe, true, pDowned->GetEntityId());
				}
				else if (Now() - s_bot.useSince > 6.0f)
					HoldUse(pMe, false, 0);    // pressed again next frame
				s_bot.doing.Format("reviving %s", pDowned->GetEntity()->GetName());
			}
			return;
		}
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
					Ask(3, pLeaderVehicle->GetEntity()->GetName());
				s_bot.doing.Format("getting into %s's vehicle", pLeader->GetEntity()->GetName());
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
			if (enemy)
				Event("fighting %s", NameOf(enemy));
			s_bot.enemy = enemy;
		}
		IActor* pEnemy = ActorOf(enemy);
		bool aiming = false;
		if (pEnemy)
		{
			const Vec3 aim = AimPoint(pEnemy->GetEntity());
			const bool seen = Visible(pMe, eye, pEnemy->GetEntity(), aim);
			LookAt(eye, aim, 5.0f);
			aiming = true;
			float yaw, pitch;
			ViewAngles(pMe, yaw, pitch);
			const float off = fabsf(Wrap(s_ctl.yaw - yaw)) + fabsf(s_ctl.pitch - pitch);
			int clip = -1, reserve = -1;
			IWeapon* pWeapon = CurrentWeapon(pMe, &clip, &reserve);
			if (!pWeapon && Now() > s_bot.switchAt)
			{
				// fists or nothing: the next weapon
				Press(pMe, g_pGame->Actions().nextitem, true);
				Press(pMe, g_pGame->Actions().nextitem, false);
				s_bot.switchAt = Now() + 1.5f;
			}
			else if (pWeapon && clip == 0 && Now() > s_bot.reloadAt)
			{
				Fire(pMe, false);
				if (reserve > 0)
				{
					Press(pMe, g_pGame->Actions().reload, true);
					Press(pMe, g_pGame->Actions().reload, false);
				}
				else
				{
					Press(pMe, g_pGame->Actions().nextitem, true);
					Press(pMe, g_pGame->Actions().nextitem, false);
				}
				s_bot.reloadAt = Now() + 2.5f;
			}
			else if (pWeapon && seen && off < 0.06f)
			{
				// bursts: held a while, then let go (single shot weapons fire again)
				const float now = Now();
				if (now >= s_bot.pauseUntil && now < s_bot.burstUntil)
					Fire(pMe, true);
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
		s.Format(",\"me\":{\"name\":%s,\"health\":%d,\"maxHealth\":%d,\"down\":%s,\"suit\":%s,\"energy\":%.0f,\"weapon\":%s,\"clip\":%d,\"reserve\":%d,\"position\":%s,\"facing\":%s,\"vehicle\":%s}",
			Esc(pMe->GetEntity()->GetName()).c_str(), pMe->GetHealth(), pMe->GetMaxHealth(), pMe->GetHealth() <= 0 ? "true" : "false",
			Esc(pSuit && pSuit->GetMode() >= 0 && pSuit->GetMode() < 4 ? modes[pSuit->GetMode()] : "none").c_str(), pSuit ? pSuit->GetSuitEnergy() : 0.0f,
			Esc(weapon.c_str()).c_str(), clip, reserve, Vec(pos).c_str(), Esc(Compass(pMe->GetViewRotation().GetColumn1())).c_str(),
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
		// the first connect a while after the start (the game's network is
		// not ready at once)
		if (s_nextConnect < 0)
			s_nextConnect = Now() + 15.0f;
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
			CryLogAlways("[CoopAgent] the network service is ready (state %d)", (int)pService->GetState());
		}
		if (port > 0 && serviceReady && !pFramework->GetClientChannel() && !pFramework->GetClientActor() && Now() >= s_nextConnect)
		{
			// the handshake (the key check) takes a while: a new connect would cut it
			s_nextConnect = Now() + 45.0f;
			const int local = ProxyPort(port);
			string cmd;
			cmd.Format("connect 127.0.0.1 %d", local ? local : port);
			CryLogAlways("[CoopAgent] joining the host: %s", cmd.c_str());
			gEnv->pConsole->ExecuteString(cmd.c_str());
		}
	}

	// ------------------------------------------------------------------
	// the host: the companion's game, started and ended with the setting
	PROCESS_INFORMATION s_proc = {};
	float s_notWantedSince = -1.0f, s_restartAt = 0.0f, s_startedAt = 0.0f, s_lockUntil = 0.0f;
	int s_starts = 0;
	bool s_windowDone = false;
	HANDLE s_hookStop = 0;              // ends the window hook's thread
	volatile LONG s_hookMs = -1;        // when the hook was set, ms after the start (-2: never)
	bool s_hookLogged = false;

	struct SFind { DWORD pid; HWND hWnd; };
	BOOL CALLBACK FindProcessWindow(HWND hWnd, LPARAM lParam)
	{
		SFind* f = (SFind*)lParam;
		DWORD pid = 0;
		GetWindowThreadProcessId(hWnd, &pid);
		RECT r;
		if (pid == f->pid && !GetParent(hWnd) && GetWindowRect(hWnd, &r) && r.right - r.left > 100)
		{
			f->hWnd = hWnd;
			return FALSE;
		}
		return TRUE;
	}

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
					|| !_strnicmp(p, "s_SoundEnable", 13) || !_strnicmp(p, "s_DummySound", 12))
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
		kept += "r_Fullscreen = 0\nr_Width = 480\nr_Height = 270\ns_SoundEnable = 0\ns_DummySound = 1\n";
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

	void StartCompanion()
	{
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
		std::wstring line = L"\"" + std::wstring(exe) + L"\" -mod " + mod + L" -dx9" + (devmode ? L" -devmode" : L"") + L" -userpath \"" + profile + L"\" -logfile companion.log" + wtail;
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
			s_lockUntil = Now() + 20.0f;
		if (!CreateProcessW(exe, &buf[0], 0, 0, FALSE, CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS, 0, root.c_str(), &si, &s_proc))
		{
			CryLogAlways("[CoopAgent] the AI companion's game could not be started (error %u)", (unsigned)GetLastError());
			memset(&s_proc, 0, sizeof(s_proc));
			s_restartAt = Now() + 30.0f;
			return;
		}
		if (const DWORD_PTR cpus = CompanionCpus())
			SetProcessAffinityMask(s_proc.hProcess, cpus);
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
		s_windowDone = false;
		CryLogAlways("[CoopAgent] AI companion started (process %u, joins 127.0.0.1:%d, agents on 127.0.0.1:%d)",
			(unsigned)s_proc.dwProcessId, pPort ? pPort->GetIVal() : 64087, s_pPort->GetIVal());
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

	void UpdateHost()
	{
		// the companion's window, the moment it is there: no taskbar button
		// (a button that came and went made the taskbar's icons jump), out of
		// sight
		if (s_proc.hProcess && !s_windowDone && Now() - s_startedAt < 60.0f)
		{
			SFind f = { s_proc.dwProcessId, 0 };
			EnumWindows(FindProcessWindow, (LPARAM)&f);
			if (f.hWnd)
			{
				PutAway(f.hWnd);
				s_windowDone = true;
			}
		}
		if (!s_hookLogged && s_hookMs != -1)
		{
			s_hookLogged = true;
			if (s_hookMs >= 0)
				CryLogAlways("[CoopAgent] window hook on the companion's game %d ms after its start", (int)s_hookMs);
			else
				CryLogAlways("[CoopAgent] no window hook on the companion's game: its window may show for a moment");
		}
		if (s_lockUntil > 0.0f && (s_windowDone || Now() > s_lockUntil))
		{
			LockSetForegroundWindow(LSFW_UNLOCK);
			s_lockUntil = 0.0f;
		}
		const bool hosting = gEnv->bServer && gEnv->bMultiplayer && CoopAI::IsCoopSession();
		const bool want = s_pCompanion->GetIVal() != 0 && hosting;
		if (want)
		{
			s_notWantedSince = -1.0f;
			if (!Running() && Now() >= s_restartAt && s_starts < 5)
				StartCompanion();
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
	IVehicleSeat* FreeSeat(IVehicle* pVehicle)
	{
		IVehicleSeat* pAny = 0;
		for (unsigned int i = 1; i <= pVehicle->GetSeatCount(); ++i)
		{
			IVehicleSeat* pSeat = pVehicle->GetSeatById((TVehicleSeatId)i);
			if (!pSeat || pSeat->GetPassenger())
				continue;
			// a passenger's seat rather than the wheel
			if (!pSeat->IsDriver())
				return pSeat;
			if (!pAny)
				pAny = pSeat;
		}
		return pAny;
	}
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
	// this game's own settings: a server sends its console variables to every
	// client that connects, and the companion stopped being one when it
	// joined (the host's coop_agent is 0)
	ICVar* own[] = { s_pCompanion, s_pAgent, s_pPort, s_pJoin, s_pFps, s_pParent, s_pName };
	for (int i = 0; i < (int)(sizeof(own) / sizeof(own[0])); ++i)
		own[i]->SetFlags(own[i]->GetFlags() | VF_NOT_NET_SYNCED);
	// the companion's game never takes the mouse (the host's value is 0)
	if (ICVar* pFree = gEnv->pConsole->GetCVar("coop_test_free_cursor"))
		pFree->SetFlags(pFree->GetFlags() | VF_NOT_NET_SYNCED);
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
			pSeat->Enter(agent, false);
			CryLogAlways("[CoopAgent] %s gets into %s", pAgent->GetEntity()->GetName(), pVehicle->GetEntity()->GetName());
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

int CoopAgent::CompanionState()
{
	if (!s_proc.hProcess)
		return 0;
	IActorIteratorPtr it = g_pGame->GetIGameFramework()->GetIActorSystem()->CreateActorIterator();
	while (IActor* pActor = it->Next())
		if (pActor->IsPlayer() && !stricmp(pActor->GetEntity()->GetName(), s_pName->GetString()))
			return 2;
	return 1;
}
