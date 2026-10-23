#include "StdAfx.h"
#include "CoopProps.h"
#include "CoopAI.h"
#include "CoopAgent.h"
#include "Game.h"
#include "GameRules.h"
#include "Actor.h"
#include "Player.h"
#include "OffHand.h"
#include "IVehicleSystem.h"
#include "IItemSystem.h"
#include <map>
#include <set>
#include <vector>

// Crysis Coop: things carried in the left hand, for everybody and the same on
// every machine (the user, 2026-10-22: "I cannot pick up ordinary things, a
// pile of crates is in my way"). In a network game the left hand picks up
// weapons only (COffHand::CanPerformPickUp: CheckItemsInProximity), and the
// loose props of a level are not on the network at all: every machine moves
// its own copy. So here:
// - the server decides who holds what (first come; a level prop, in reach,
//   the holder alive and on foot) and tells everybody;
// - the holder's own game holds it as single player does (first person); the
//   others put it at his hands from where he looks (his eyes, and where it is
//   in his hands as his game sent it);
// - a throw or a drop is made in the holder's game (single player's speeds)
//   and sent with the thing's place, speed and spin: everybody starts the
//   same flight, the server sends it on (10 a second) and where it comes to
//   rest; a late joiner gets where the moved things lie.
// Soldiers (grabbing them), rocks of the ground and pieces of broken things
// stay out: none of them is the same on every machine.
namespace
{
	ICVar* s_pOn = 0;
	const int GRAB_ONE_HANDED = 0;    // COffHand's GRAB_TYPE_ONE_HANDED (OffHand.cpp)

	struct SHold
	{
		EntityId holder;      // this game's id of the player
		int channel;          // the server: his channel (0: the host himself)
		uint8 grab;
		Vec3 relPos;          // where it is in his hands (to his eyes)
		Quat relRot;
		uint32 ignoreId;      // the constraint: no collision with him (0 none)
		float since;
		float lostSince;      // the server: the host's own hand no longer has it
	};
	std::map<EntityId, SHold> s_holds;

	// the server: a thrown or dropped thing's flight, sent on till it rests
	struct SFlight
	{
		float since, sentAt, slowSince;
		bool quiet;	// only where it comes to rest (settling after a load, not thrown)
	};
	std::map<EntityId, SFlight> s_flights;

	std::set<EntityId> s_touched;              // the server: moved by a hand (a joiner, a load)
	EntityId s_dropWhenHeld = 0;               // the local hand lets go of it once the pick-up is over
	std::set<EntityId> s_ownFlights;           // a client: its own throws in the air (the server's states of
	                                           // them are a round trip old here: only where they rest)
	struct SLater { EntityId prop; int channel; float at; };
	std::vector<SLater> s_restLater;           // the server: where a refused thing lies, told a moment later
	struct SBase { Vec3 pos; Quat rot; };
	std::map<EntityId, SBase> s_base;          // the server: the level file's place of every thing
	std::map<EntityId, uint16> s_seq;          // the server's count per thing; a client's last one
	uint8 s_epoch = 0;                         // the server's loads; a client's last one
	bool s_haveEpoch = false;

	// the constraints let go of a moment after a throw (not hitting the
	// thrower on the way out)
	struct SUnignore { EntityId prop; uint32 id; float at; };
	std::vector<SUnignore> s_unignore;

	// what the local player holds (his own game)
	struct SLocal
	{
		EntityId prop;
		uint8 grab;
		float since;
		Vec3 relPos;
		Quat relRot;
		float relAt;
		bool relSent;
		bool acked;
		SLocal(): prop(0), grab(0), since(0), relAt(0), relSent(false), acked(false) { relPos.zero(); relRot.SetIdentity(); }
	} s_local;

	float Now() { return gEnv->pTimer->GetAsyncCurTime(); }

	CGameRules* Rules() { return g_pGame ? g_pGame->GetGameRules() : 0; }

	CActor* LocalActor()
	{
		return static_cast<CActor*>(g_pGame->GetIGameFramework()->GetClientActor());
	}

	CActor* ActorOf(EntityId id)
	{
		return id ? static_cast<CActor*>(g_pGame->GetIGameFramework()->GetIActorSystem()->GetActor(id)) : 0;
	}

	const char* NameOf(EntityId id)
	{
		IEntity* p = id ? gEnv->pEntitySystem->GetEntity(id) : 0;
		return p ? p->GetName() : "?";
	}

	// the holder's eyes (his own game: the view; any other: where he looks
	// as the network says)
	Matrix34 EyeFrame(CActor* pActor)
	{
		SMovementState st;
		IMovementController* pMC = pActor->GetMovementController();
		const Matrix34& tm = pActor->GetEntity()->GetWorldTM();
		Vec3 pos = tm.GetTranslation() + Vec3(0, 0, 1.6f);
		Vec3 dir = tm.GetColumn1();
		if (pMC)
		{
			pMC->GetMovementState(st);
			if (st.eyePosition.GetLengthSquared() > 0.01f)
				pos = st.eyePosition;
			if (st.eyeDirection.GetLengthSquared() > 0.01f)
				dir = st.eyeDirection;
		}
		Matrix34 eye(Quat::CreateRotationVDir(dir.GetNormalizedSafe(Vec3(0, 1, 0))));
		eye.SetTranslation(pos);
		return eye;
	}

	// the holder's body (feet, turned his way): where a thing sits in his
	// hands is sent to it - the body is the same in every game, where he
	// looks up or down is not (another game had him look 20 degrees off, the
	// test); his own game sends it again when he looks elsewhere
	Matrix34 BodyFrame(CActor* pActor)
	{
		const Matrix34& tm = pActor->GetEntity()->GetWorldTM();
		Vec3 ahead = tm.GetColumn1();
		ahead.z = 0.0f;
		Matrix34 body(Quat::CreateRotationVDir(ahead.GetNormalizedSafe(Vec3(0, 1, 0))));
		body.SetTranslation(tm.GetTranslation());
		return body;
	}

	// where a thing sits in the hands, to his feet, when nothing better is
	// known (x right, y ahead, z up; the left hand for one)
	Vec3 BodyPlace(int grab)
	{
		return grab == GRAB_ONE_HANDED ? Vec3(-0.3f, 0.5f, 1.0f) : Vec3(0.0f, 0.6f, 0.7f);
	}

	// where a thing sits in the hands when nothing better is known (x right,
	// y ahead, z up of the eyes; the left hand for one)
	Vec3 HandPlace(int grab)
	{
		return grab == GRAB_ONE_HANDED ? Vec3(-0.25f, 0.55f, -0.4f) : Vec3(0.0f, 0.7f, -0.6f);
	}

	uint16 NextSeq(EntityId prop)
	{
		return ++s_seq[prop];
	}

	// newer than the last one of that thing (wraps)
	bool Newer(EntityId prop, uint16 seq)
	{
		std::map<EntityId, uint16>::iterator it = s_seq.find(prop);
		if (it == s_seq.end())
		{
			s_seq[prop] = seq;
			return true;
		}
		if ((int16)(seq - it->second) <= 0)
			return false;
		it->second = seq;
		return true;
	}

	uint32 IgnoreHolder(IEntity* pProp, CActor* pHolder)
	{
		IPhysicalEntity* pPE = pProp ? pProp->GetPhysics() : 0;
		IPhysicalEntity* pBuddy = pHolder ? pHolder->GetEntity()->GetPhysics() : 0;
		if (!pPE || !pBuddy)
			return 0;
		pe_action_add_constraint ic;
		ic.flags = constraint_inactive | constraint_ignore_buddy;
		ic.pBuddy = pBuddy;
		ic.pt[0].Set(0, 0, 0);
		const int id = pPE->Action(&ic);
		return id > 0 ? (uint32)id : 0;
	}

	void Unignore(EntityId prop, uint32 id, float delay)
	{
		if (!id)
			return;
		SUnignore u = { prop, id, Now() + delay };
		s_unignore.push_back(u);
	}

	// a thing put somewhere (and given a speed; at rest: asleep there)
	void Place(IEntity* pEntity, const Vec3& pos, const Quat& rot, const Vec3& v, const Vec3& w, bool rest)
	{
		IPhysicalEntity* pPE = pEntity->GetPhysics();
		Matrix34 tm = Matrix34::Create(Vec3(pEntity->GetScale()), rot, pos);
		pEntity->SetWorldTM(tm, ENTITY_XFORM_USER);
		if (!pPE)
			return;
		pe_params_pos pp;
		pp.pos = pos;
		pp.q = rot;
		pPE->SetParams(&pp);
		pe_action_set_velocity sv;
		sv.v = rest ? Vec3(ZERO) : v;
		sv.w = rest ? Vec3(ZERO) : w;
		pPE->Action(&sv);
		pe_action_awake aw;
		aw.bAwake = rest ? 0 : 1;
		pPE->Action(&aw);
	}

	void Fill(CGameRules::CoopPropParams& p, int op, EntityId prop, IEntity* pEntity)
	{
		p.op = (uint8)op;
		p.prop = prop;
		p.epoch = s_epoch;
		if (pEntity)
		{
			p.pos = pEntity->GetWorldPos();
			p.rot = pEntity->GetWorldRotation();
			if (IPhysicalEntity* pPE = pEntity->GetPhysics())
			{
				pe_status_dynamics dyn;
				if (pPE->GetStatus(&dyn))
				{
					p.v = dyn.v;
					p.w = dyn.w;
				}
			}
		}
	}

	COffHand* OffHandOf(CActor* pActor)
	{
		if (!pActor || pActor->GetActorClass() != CPlayer::GetActorClassType())
			return 0;
		return static_cast<COffHand*>(static_cast<CPlayer*>(pActor)->GetItemByClass(CItem::sOffHandClass));
	}

	// the local player's hand lets go of it (the server said no, or he must;
	// still being picked up: when that is over)
	void DropHere(EntityId prop, const char* why)
	{
		CActor* pMe = LocalActor();
		COffHand* pOffHand = OffHandOf(pMe);
		if (!pOffHand || pOffHand->CoopHeldId() != prop)
			return;
		CryLogAlways("[CoopProp] %s lets go of %s (%s)", pMe->GetEntity()->GetName(), NameOf(prop), why);
		if (pOffHand->CoopHolding())
			pOffHand->CoopForceDrop();
		else
			s_dropWhenHeld = prop;
	}

	// ---- the server -----------------------------------------------------
	void SendAll(const CGameRules::CoopPropParams& p)
	{
		if (CGameRules* pRules = Rules())
			pRules->CoopSendProp(p, 0);
	}

	void BroadcastHeld(EntityId prop, const SHold& h, int channel = 0)
	{
		CGameRules::CoopPropParams p;
		Fill(p, CoopProps::eP_Held, prop, 0);
		p.holder = h.holder;
		p.grab = h.grab;
		p.relPos = h.relPos;
		p.relRot = h.relRot;
		p.seq = NextSeq(prop);
		if (CGameRules* pRules = Rules())
			pRules->CoopSendProp(p, channel);
	}

	bool ServerGrab(CActor* pHolder, int channel, EntityId prop, int grab, const Vec3& relPos, const Quat& relRot)
	{
		IEntity* pEntity = gEnv->pEntitySystem->GetEntity(prop);
		const char* why = 0;
		if (!CoopProps::Enabled())
			why = "carrying is off";
		else if (!pEntity || !CoopProps::Eligible(pEntity))
			why = "not a thing to carry here";
		else if (!CoopAI::IsLevelEntity(prop))
			why = "not one of the level's own";
		else if (pHolder->GetHealth() <= 0 || pHolder->GetLinkedVehicle())
			why = "he is down or in a vehicle";
		else if (pEntity->GetWorldPos().GetDistance(pHolder->GetEntity()->GetWorldPos()) > 25.0f)
			why = "too far from him here";
		else
		{
			std::map<EntityId, SHold>::iterator it = s_holds.find(prop);
			if (it != s_holds.end() && it->second.holder != pHolder->GetEntityId())
				why = "someone else holds it";
		}
		if (why)
		{
			CryLogAlways("[CoopProp] %s may not pick up %s (%u): %s", pHolder->GetEntity()->GetName(), pEntity ? pEntity->GetName() : "?", prop, why);
			if (channel)
			{
				CGameRules::CoopPropParams p;
				Fill(p, CoopProps::eP_Deny, prop, 0);
				p.holder = pHolder->GetEntityId();
				if (CGameRules* pRules = Rules())
					pRules->CoopSendProp(p, channel);
				// (his hand moved his copy before the answer came: where it
				// lies, once he let go of it)
				if (pEntity)
				{
					SLater l = { prop, channel, Now() + 2.0f };
					s_restLater.push_back(l);
				}
			}
			else
				DropHere(prop, why);
			return false;
		}
		const bool again = s_holds.count(prop) != 0;
		SHold& h = s_holds[prop];
		if (!again)
		{
			h.holder = pHolder->GetEntityId();
			h.channel = channel;
			h.since = Now();
			h.lostSince = -1.0f;
			// (the host's own hand made its own, single player's)
			h.ignoreId = channel ? IgnoreHolder(pEntity, pHolder) : 0;
			if (channel)
			{
				SEntityEvent ev;
				ev.event = ENTITY_EVENT_PICKUP;
				ev.nParam[0] = 1;
				ev.nParam[1] = pHolder->GetEntityId();
				pEntity->SendEvent(ev);
			}
		}
		h.grab = (uint8)grab;
		h.relPos = relPos;
		h.relRot = relRot;
		s_flights.erase(prop);
		s_touched.insert(prop);
		if (!again)
		{
			pe_status_dynamics dyn;
			const float mass = pEntity->GetPhysics() && pEntity->GetPhysics()->GetStatus(&dyn) ? dyn.mass : 0.0f;
			CryLogAlways("[CoopProp] %s picks up %s (%u, %s, %.0f kg) at %.1f %.1f %.1f", pHolder->GetEntity()->GetName(), pEntity->GetName(), prop,
				grab == GRAB_ONE_HANDED ? "one hand" : "two hands", mass, pEntity->GetWorldPos().x, pEntity->GetWorldPos().y, pEntity->GetWorldPos().z);
		}
		BroadcastHeld(prop, h);
		return true;
	}

	void ServerRelease(EntityId prop, int mode, const Vec3& pos, const Quat& rot, const Vec3& v, const Vec3& w, bool placeHere, const char* why)
	{
		std::map<EntityId, SHold>::iterator it = s_holds.find(prop);
		if (it == s_holds.end())
			return;
		SHold h = it->second;
		s_holds.erase(it);
		Unignore(prop, h.ignoreId, 0.5f);
		IEntity* pEntity = gEnv->pEntitySystem->GetEntity(prop);
		if (pEntity && placeHere)
		{
			Place(pEntity, pos, rot, v, w, false);
			SEntityEvent ev;
			ev.event = ENTITY_EVENT_PICKUP;
			ev.nParam[0] = 0;
			ev.nParam[1] = h.holder;
			pEntity->SendEvent(ev);
			// (single player tells the AI of a throw: the soldiers hear it)
			CActor* pHolder = ActorOf(h.holder);
			if (mode == CoopProps::eR_Throw && pHolder && pHolder->GetEntity()->GetAI())
			{
				SAIEVENT ai;
				ai.targetId = prop;
				pHolder->GetEntity()->GetAI()->Event(AIEVENT_PLAYER_THROW, &ai);
			}
		}
		CGameRules::CoopPropParams p;
		Fill(p, CoopProps::eP_Released, prop, 0);
		p.holder = h.holder;
		p.grab = (uint8)mode;
		p.pos = pos;
		p.rot = rot;
		p.v = v;
		p.w = w;
		p.seq = NextSeq(prop);
		SendAll(p);
		SFlight f = { Now(), Now(), -1.0f, false };
		s_flights[prop] = f;
		s_touched.insert(prop);
		static const char* const s_modes[] = { "throws", "drops", "lets go of" };
		CryLogAlways("[CoopProp] %s %s %s (%u) at %.1f %.1f %.1f, speed %.1f m/s%s%s", NameOf(h.holder), s_modes[mode < 0 || mode > 2 ? 2 : mode], NameOf(prop), prop,
			pos.x, pos.y, pos.z, v.GetLength(), why ? ": " : "", why ? why : "");
	}

	// the server takes it out of his hands (he died, sat in a vehicle, left)
	void ServerForce(EntityId prop, const char* why)
	{
		std::map<EntityId, SHold>::iterator it = s_holds.find(prop);
		if (it == s_holds.end())
			return;
		const SHold h = it->second;
		IEntity* pEntity = gEnv->pEntitySystem->GetEntity(prop);
		CActor* pHolder = ActorOf(h.holder);
		if (h.channel)
		{
			CGameRules::CoopPropParams p;
			Fill(p, CoopProps::eP_Force, prop, 0);
			p.holder = h.holder;
			if (CGameRules* pRules = Rules())
				pRules->CoopSendProp(p, h.channel);
		}
		else if (pHolder && pHolder == LocalActor())
			DropHere(prop, why);
		if (s_holds.count(prop))
			ServerRelease(prop, CoopProps::eR_Forced, pEntity ? pEntity->GetWorldPos() : Vec3(ZERO), pEntity ? pEntity->GetWorldRotation() : Quat(IDENTITY),
				Vec3(ZERO), Vec3(ZERO), pEntity != 0, why);
	}

	void ServerUpdate()
	{
		const float now = Now();
		CActor* pHost = LocalActor();
		std::vector<std::pair<EntityId, const char*> > lost;
		for (std::map<EntityId, SHold>::iterator it = s_holds.begin(); it != s_holds.end(); ++it)
		{
			const EntityId prop = it->first;
			SHold& h = it->second;
			IEntity* pEntity = gEnv->pEntitySystem->GetEntity(prop);
			CActor* pHolder = ActorOf(h.holder);
			const char* why = 0;
			if (!pHolder)
				why = "he left";
			else if (pHolder->GetHealth() <= 0)
				why = "he is down";
			else if (pHolder->GetLinkedVehicle())
				why = "he is in a vehicle";
			else if (!pEntity || pEntity->IsHidden() || !pEntity->GetPhysics() || pEntity->GetPhysics()->GetType() != PE_RIGID)
				why = "it is gone or broke";
			else if (h.channel && Rules() && !Rules()->GetActorByChannelId(h.channel))
				why = "he left";
			else if (!h.channel && pHolder == pHost)
			{
				// (the host's own hand let go some way this does not hear of)
				COffHand* pOffHand = OffHandOf(pHolder);
				if (pOffHand && pOffHand->CoopHeldId() == prop)
					h.lostSince = -1.0f;
				else if (h.lostSince < 0.0f)
					h.lostSince = now;
				else if (now - h.lostSince > 0.5f)
					why = "his hand no longer has it";
			}
			if (why)
				lost.push_back(std::make_pair(prop, why));
		}
		for (size_t i = 0; i < lost.size(); ++i)
			ServerForce(lost[i].first, lost[i].second);
		// the flights: 10 a second the first 2 s, 5 after; at rest (or after
		// 12 s) where it lies
		std::vector<EntityId> landed;
		for (std::map<EntityId, SFlight>::iterator it = s_flights.begin(); it != s_flights.end(); ++it)
		{
			IEntity* pEntity = gEnv->pEntitySystem->GetEntity(it->first);
			IPhysicalEntity* pPE = pEntity ? pEntity->GetPhysics() : 0;
			if (!pPE)
			{
				landed.push_back(it->first);
				continue;
			}
			SFlight& f = it->second;
			pe_status_dynamics dyn;
			pPE->GetStatus(&dyn);
			pe_status_awake awake;
			const bool asleep = !pPE->GetStatus(&awake);
			const bool slow = dyn.v.GetLengthSquared() < 0.01f && dyn.w.GetLengthSquared() < 0.04f;
			if (!slow)
				f.slowSince = -1.0f;
			else if (f.slowSince < 0.0f)
				f.slowSince = now;
			// (at rest when it rests: a barrel rolling down a slope or drifting
			// in a river for long is sent on, less often; after a minute its
			// last state, awake, and every game goes on with it)
			const bool rest = asleep || (f.slowSince >= 0.0f && now - f.slowSince > 0.5f);
			const bool tooLong = !rest && now - f.since > 60.0f;
			const float every = now - f.since < 2.0f ? 0.1f : (now - f.since < 12.0f ? 0.2f : 1.0f);
			if (!rest && !tooLong && (f.quiet || now - f.sentAt < every))
				continue;
			f.sentAt = now;
			CGameRules::CoopPropParams p;
			Fill(p, rest ? CoopProps::eP_Rest : CoopProps::eP_State, it->first, pEntity);
			p.seq = NextSeq(it->first);
			// (the last word reliable: a quiet one's only one after its first)
			if (CGameRules* pRules = Rules())
			{
				if (rest || tooLong)
					pRules->CoopSendProp(p, 0);
				else
					pRules->CoopSendPropState(p, 0);
			}
			if (rest)
				CryLogAlways("[CoopProp] %s (%u) rests at %.2f %.2f %.2f (%.1f s after it was let go)", pEntity->GetName(), it->first, p.pos.x, p.pos.y, p.pos.z, now - f.since);
			else if (tooLong)
				CryLogAlways("[CoopProp] %s (%u) still moves a minute after it was let go: its last state sent", pEntity->GetName(), it->first);
			if (rest || tooLong)
				landed.push_back(it->first);
		}
		for (size_t i = 0; i < landed.size(); ++i)
			s_flights.erase(landed[i]);
		for (size_t i = 0; i < s_restLater.size();)
		{
			if (now < s_restLater[i].at)
			{
				++i;
				continue;
			}
			const SLater l = s_restLater[i];
			s_restLater[i] = s_restLater.back();
			s_restLater.pop_back();
			IEntity* pEntity = gEnv->pEntitySystem->GetEntity(l.prop);
			if (!pEntity || s_holds.count(l.prop) || s_flights.count(l.prop))
				continue;
			CGameRules::CoopPropParams p;
			Fill(p, CoopProps::eP_Rest, l.prop, pEntity);
			p.seq = NextSeq(l.prop);
			if (CGameRules* pRules = Rules())
				pRules->CoopSendProp(p, l.channel);
		}
	}

	// where a thing is now differs from the level file's (5 cm, 2 degrees)
	bool MovedFromBase(EntityId prop, IEntity* pEntity)
	{
		std::map<EntityId, SBase>::const_iterator it = s_base.find(prop);
		if (it == s_base.end())
			return false;
		return pEntity->GetWorldPos().GetSquaredDistance(it->second.pos) > 0.0025f || fabsf(pEntity->GetWorldRotation() | it->second.rot) < 0.9998f;
	}

	// a thing's place to one game or all: asleep where it lies, or flying on
	// (the number of games it reached)
	int SendWhere(EntityId prop, IEntity* pEntity, int channel)
	{
		IPhysicalEntity* pPE = pEntity->GetPhysics();
		pe_status_awake awake;
		const bool moving = s_flights.count(prop) || (pPE && pPE->GetStatus(&awake));
		CGameRules::CoopPropParams p;
		Fill(p, moving ? CoopProps::eP_State : CoopProps::eP_Rest, prop, pEntity);
		p.seq = NextSeq(prop);
		CGameRules* pRules = Rules();
		if (!pRules)
			return 0;
		int reached = 0;
		if (moving)
		{
			// (its fall mirrored from here on)
			reached = pRules->CoopSendProp(p, channel);
			// (where it comes to rest, then: after a load a hundred things
			// settle at once - their falls at 10 a second, to every game, were
			// a thousand messages a second)
			if (!s_flights.count(prop))
			{
				SFlight f = { Now(), Now(), -1.0f, true };
				s_flights[prop] = f;
			}
		}
		else
			reached = pRules->CoopSendProp(p, channel);
		return reached;
	}

	// ---- the holder's own game ------------------------------------------
	void LocalUpdate()
	{
		CActor* pMe = LocalActor();
		COffHand* pOffHand = OffHandOf(pMe);
		// (the server said no while it was still being picked up)
		if (s_dropWhenHeld)
		{
			if (!pOffHand || pOffHand->CoopHeldId() != s_dropWhenHeld)
				s_dropWhenHeld = 0;
			else if (pOffHand->CoopHolding())
			{
				s_dropWhenHeld = 0;
				pOffHand->CoopForceDrop();
			}
		}
		if (!s_local.prop)
			return;
		IEntity* pEntity = gEnv->pEntitySystem->GetEntity(s_local.prop);
		if (!pMe || !pOffHand || !pEntity || pOffHand->CoopHeldId() != s_local.prop)
		{
			// (let go some way the throw does not report: as a drop where it is)
			const EntityId prop = s_local.prop;
			if (pMe && pEntity)
			{
				CryLogAlways("[CoopProp] %s's hand no longer has %s: let go of where it is", pMe->GetEntity()->GetName(), pEntity->GetName());
				pe_status_dynamics dyn;
				if (pEntity->GetPhysics())
					pEntity->GetPhysics()->GetStatus(&dyn);
				CoopProps::OnLocalRelease(pMe, prop, CoopProps::eR_Forced, dyn.v, dyn.w);
			}
			s_local = SLocal();
			return;
		}
		// where it sits in the hands, to the eyes: sent when it moved 5 cm
		const float now = Now();
		if (now - s_local.relAt < 0.25f || now - s_local.since < 0.3f)
			return;
		s_local.relAt = now;
		Matrix34 rel = BodyFrame(pMe).GetInverted() * pEntity->GetWorldTM();
		Vec3 relPos = rel.GetTranslation();
		Matrix33 m(rel);
		m.OrthonormalizeFast();
		Quat relRot(m);
		relRot.Normalize();
		// (out of any sane place round him: where a hand would hold it - a
		// barrel's foot is 1.4 m below his eyes, the test)
		if (relPos.y < -0.5f || relPos.y > 2.5f || fabsf(relPos.x) > 1.5f || relPos.z < -0.5f || relPos.z > 3.0f)
		{
			relPos = BodyPlace(s_local.grab);
			relRot.SetIdentity();
		}
		if (s_local.relSent && relPos.GetDistance(s_local.relPos) < 0.05f && fabsf(relRot | s_local.relRot) > 0.996f)
			return;
		s_local.relPos = relPos;
		s_local.relRot = relRot;
		s_local.relSent = true;
		// (now and then in the log: where in his hands)
		static float s_relLogAt = 0.0f;
		if (now >= s_relLogAt)
		{
			s_relLogAt = now + 2.0f;
			const Vec3 f = pMe->GetEntity()->GetWorldPos();
			CryLogAlways("[CoopProp] %s holds %s %.2f %.2f %.2f from his feet (at %.2f %.2f %.2f; it at %.2f %.2f %.2f)",
				pMe->GetEntity()->GetName(), pEntity->GetName(), relPos.x, relPos.y, relPos.z, f.x, f.y, f.z,
				pEntity->GetWorldPos().x, pEntity->GetWorldPos().y, pEntity->GetWorldPos().z);
		}
		if (gEnv->bServer)
		{
			std::map<EntityId, SHold>::iterator it = s_holds.find(s_local.prop);
			if (it != s_holds.end() && it->second.holder == pMe->GetEntityId())
			{
				it->second.relPos = relPos;
				it->second.relRot = relRot;
				BroadcastHeld(s_local.prop, it->second);
			}
			else
			{
				// (not his: somebody else's hold, or none)
				const EntityId prop = s_local.prop;
				s_local = SLocal();
				DropHere(prop, "not his to hold");
			}
		}
		else if (CGameRules* pRules = Rules())
		{
			CGameRules::CoopPropParams p;
			Fill(p, CoopProps::eP_Rel, s_local.prop, 0);
			p.grab = s_local.grab;
			p.relPos = relPos;
			p.relRot = relRot;
			pRules->CoopSendPropToServer(p);
		}
	}
}

// ---- the interface ---------------------------------------------------------

bool CoopProps::Enabled()
{
	if (!CoopAI::IsCoopSession() || !gEnv->bMultiplayer)
		return false;
	if (!s_pOn && gEnv->pConsole)
	{
		s_pOn = gEnv->pConsole->GetCVar("coop_props");
		if (!s_pOn)
			s_pOn = gEnv->pConsole->RegisterInt("coop_props", 1, 0,
				"Crysis Coop: 1 = everybody may pick up, carry and throw loose things, and every game sees them the same (0 = weapons only, as a network game)");
	}
	return !s_pOn || s_pOn->GetIVal() != 0;
}

bool CoopProps::Eligible(IEntity* pEntity)
{
	if (!pEntity || pEntity->IsHidden() || pEntity->GetParent())
		return false;
	IPhysicalEntity* pPE = pEntity->GetPhysics();
	if (!pPE || pPE->GetType() != PE_RIGID)
		return false;
	IGameFramework* pFw = g_pGame->GetIGameFramework();
	if (pFw->GetIActorSystem()->GetActor(pEntity->GetId()) || pFw->GetIItemSystem()->GetItem(pEntity->GetId())
		|| pFw->GetIVehicleSystem()->GetVehicle(pEntity->GetId()))
		return false;
	// (the level's props; not "Default" - a piece of something broken, its
	// own on every machine - nor "rock", one made of the ground in this game)
	static IEntityClass* s_pBasic = gEnv->pEntitySystem->GetClassRegistry()->FindClass("BasicEntity");
	static IEntityClass* s_pRigid = gEnv->pEntitySystem->GetClassRegistry()->FindClass("RigidBodyEx");
	IEntityClass* pClass = pEntity->GetClass();
	return pClass && (pClass == s_pBasic || pClass == s_pRigid);
}

Matrix34 CoopProps::HoldMatrix(CActor* pHolder, IEntity* pEntity, int grabType, const Matrix34* pHoldOffset)
{
	// (the hand before the eyes; the thing by its grab point there, as single
	// player holds it by its helper - a crate's middle, not its foot, at the
	// hand: not up before the eyes)
	Matrix34 tm = EyeFrame(pHolder) * Matrix34::CreateTranslationMat(HandPlace(grabType));
	if (pEntity)
		tm = tm * Matrix34::CreateScale(Vec3(pEntity->GetScale()));
	if (pHoldOffset)
		tm = tm * *pHoldOffset;
	return tm;
}

void CoopProps::OnLocalGrab(CActor* pHolder, EntityId prop, int grabType)
{
	if (!Enabled() || !pHolder || pHolder != LocalActor())
		return;
	IEntity* pEntity = gEnv->pEntitySystem->GetEntity(prop);
	if (!pEntity || !Eligible(pEntity))
		return;
	s_local = SLocal();
	s_local.prop = prop;
	s_local.grab = (uint8)grabType;
	s_local.since = Now();
	s_local.relPos = BodyPlace(grabType);
	if (gEnv->bServer)
	{
		s_local.acked = ServerGrab(pHolder, 0, prop, grabType, s_local.relPos, s_local.relRot);
		if (!s_local.acked)
			s_local = SLocal();
		return;
	}
	CryLogAlways("[CoopProp] %s picks up %s (%u): asks the server", pHolder->GetEntity()->GetName(), pEntity->GetName(), prop);
	CGameRules::CoopPropParams p;
	Fill(p, eP_Grab, prop, pEntity);
	p.grab = (uint8)grabType;
	p.relPos = s_local.relPos;
	p.relRot = s_local.relRot;
	if (CGameRules* pRules = Rules())
		pRules->CoopSendPropToServer(p);
}

void CoopProps::OnLocalRelease(CActor* pHolder, EntityId prop, int mode, const Vec3& v, const Vec3& w)
{
	if (!Enabled() || !pHolder || pHolder != LocalActor())
		return;
	IEntity* pEntity = gEnv->pEntitySystem->GetEntity(prop);
	if (s_local.prop == prop)
		s_local = SLocal();
	if (!pEntity)
		return;
	if (gEnv->bServer)
	{
		std::map<EntityId, SHold>::iterator it = s_holds.find(prop);
		if (it != s_holds.end() && it->second.holder == pHolder->GetEntityId())
			ServerRelease(prop, mode, pEntity->GetWorldPos(), pEntity->GetWorldRotation(), v, w, false, 0);
		return;
	}
	if (mode != eR_Forced)
		s_ownFlights.insert(prop);
	CGameRules::CoopPropParams p;
	Fill(p, eP_Release, prop, pEntity);
	p.grab = (uint8)mode;
	p.v = v;
	p.w = w;
	if (CGameRules* pRules = Rules())
		pRules->CoopSendPropToServer(p);
	static const char* const s_modes[] = { "throws", "drops", "lets go of" };
	CryLogAlways("[CoopProp] %s %s %s (%u) at %.1f %.1f %.1f, speed %.1f m/s (sent to the server)", pHolder->GetEntity()->GetName(),
		s_modes[mode < 0 || mode > 2 ? 2 : mode], pEntity->GetName(), prop, p.pos.x, p.pos.y, p.pos.z, v.GetLength());
}

bool CoopProps::IsHeld(EntityId prop)
{
	return prop && s_holds.count(prop) != 0;
}

bool CoopProps::IsBusy(EntityId prop)
{
	return prop && (s_holds.count(prop) != 0 || s_flights.count(prop) != 0);
}

bool CoopProps::HeldHere(EntityId prop)
{
	return prop && s_local.prop == prop;
}

void CoopProps::OnServerMessage(const CGameRules::CoopPropParams& p, CActor* pFrom, int channelId)
{
	if (!gEnv->bServer || !pFrom)
		return;
	switch (p.op)
	{
	case eP_Grab:
		ServerGrab(pFrom, channelId, p.prop, p.grab, p.relPos, p.relRot);
		break;
	case eP_Release:
	{
		std::map<EntityId, SHold>::iterator it = s_holds.find(p.prop);
		if (it == s_holds.end() || it->second.holder != pFrom->GetEntityId())
		{
			// (refused before, or let go of twice: where it is in his game is
			// no news; the server's own flight is sent on)
			break;
		}
		ServerRelease(p.prop, p.grab, p.pos, p.rot, p.v, p.w, true, 0);
		break;
	}
	case eP_Rel:
	{
		std::map<EntityId, SHold>::iterator it = s_holds.find(p.prop);
		if (it != s_holds.end() && it->second.holder == pFrom->GetEntityId())
		{
			it->second.relPos = p.relPos;
			it->second.relRot = p.relRot;
			BroadcastHeld(p.prop, it->second);
		}
		break;
	}
	}
}

void CoopProps::OnClientMessage(const CGameRules::CoopPropParams& p, bool state)
{
	if (gEnv->bServer || !Enabled())
		return;
	// (a load on the host: what came before it is old)
	if (!s_haveEpoch || (int8)(p.epoch - s_epoch) > 0)
	{
		if (s_haveEpoch)
		{
			for (std::map<EntityId, SHold>::iterator it = s_holds.begin(); it != s_holds.end(); ++it)
				Unignore(it->first, it->second.ignoreId, 0.0f);
			s_holds.clear();
			s_seq.clear();
			s_ownFlights.clear();
		}
		s_haveEpoch = true;
		s_epoch = p.epoch;
	}
	else if (p.epoch != s_epoch)
		return;
	IEntity* pEntity = gEnv->pEntitySystem->GetEntity(p.prop);
	CActor* pMe = LocalActor();
	const EntityId me = pMe ? pMe->GetEntityId() : 0;
	// (the newest of each thing only: a flight's states come unordered)
	if ((p.op == eP_State || p.op == eP_Rest || p.op == eP_Held || p.op == eP_Released) && !Newer(p.prop, p.seq) && p.op == eP_State)
		return;
	switch (p.op)
	{
	case eP_Held:
	{
		const bool again = s_holds.count(p.prop) != 0 && s_holds[p.prop].holder == p.holder;
		SHold& h = s_holds[p.prop];
		if (!again)
		{
			Unignore(p.prop, h.ignoreId, 0.0f);
			h.holder = p.holder;
			h.channel = 0;
			h.since = Now();
			h.lostSince = -1.0f;
			h.ignoreId = p.holder != me ? IgnoreHolder(pEntity, ActorOf(p.holder)) : 0;
			CryLogAlways("[CoopProp] %s holds %s (%u)%s", NameOf(p.holder), pEntity ? pEntity->GetName() : "?", p.prop,
				p.holder == me ? ": the server says yes" : "");
			// (a new hold only: the server's echo of this game's last hold
			// position comes after its throw too, and the throw is its own)
			s_ownFlights.erase(p.prop);
		}
		h.grab = p.grab;
		h.relPos = p.relPos;
		h.relRot = p.relRot;
		if (p.holder == me && s_local.prop == p.prop)
			s_local.acked = true;
		break;
	}
	case eP_Released:
	{
		std::map<EntityId, SHold>::iterator it = s_holds.find(p.prop);
		if (it != s_holds.end())
		{
			Unignore(p.prop, it->second.ignoreId, 0.5f);
			s_holds.erase(it);
		}
		if (pEntity && p.holder != me)
			Place(pEntity, p.pos, p.rot, p.v, p.w, false);
		CryLogAlways("[CoopProp] %s let go of %s (%u) at %.1f %.1f %.1f, speed %.1f m/s", NameOf(p.holder), pEntity ? pEntity->GetName() : "?", p.prop,
			p.pos.x, p.pos.y, p.pos.z, p.v.GetLength());
		break;
	}
	case eP_State:
	case eP_Rest:
	{
		if (!pEntity || s_holds.count(p.prop) || s_local.prop == p.prop)
			break;
		const bool rest = p.op == eP_Rest;
		// (its own throw flies here from the moment of the throw; the
		// server's states of it are a round trip late: moved on by that much
		// and taken only when this copy went another way - a barrel that
		// bounced and rolled elsewhere, 4.6 m off at the end, the test)
		if (s_ownFlights.count(p.prop))
		{
			if (!rest)
			{
				INetChannel* pChannel = g_pGame->GetIGameFramework()->GetClientChannel();
				const float rtt = pChannel ? CLAMP(pChannel->GetPing(true), 0.0f, 0.5f) : 0.1f;
				const Vec3 g(0.0f, 0.0f, -9.81f);
				const Vec3 ahead = p.pos + p.v * rtt + g * (0.5f * rtt * rtt);
				if (pEntity->GetWorldPos().GetDistance(ahead) > 1.0f)
				{
					Quat rot = p.rot;
					if (p.w.GetLengthSquared() > 0.0001f)
						rot = Quat::CreateRotationAA(p.w.GetLength() * rtt, p.w.GetNormalized()) * p.rot;
					Place(pEntity, ahead, rot.GetNormalized(), p.v + g * rtt, p.w, false);
				}
				break;
			}
			s_ownFlights.erase(p.prop);
		}
		const Vec3 here = pEntity->GetWorldPos();
		const float off = here.GetDistance(p.pos);
		const float turn = fabsf(pEntity->GetWorldRotation() | p.rot);
		if (rest || off > 0.3f || turn < 0.985f)
			Place(pEntity, p.pos, p.rot, p.v, p.w, rest);
		else if (IPhysicalEntity* pPE = pEntity->GetPhysics())
		{
			pe_action_set_velocity sv;
			sv.v = p.v;
			sv.w = p.w;
			pPE->Action(&sv);
		}
		if (rest)
			CryLogAlways("[CoopProp] %s (%u) rests at %.2f %.2f %.2f (here it was %.2f m off)", pEntity->GetName(), p.prop, p.pos.x, p.pos.y, p.pos.z, off);
		break;
	}
	case eP_Deny:
		if (s_local.prop == p.prop)
		{
			DropHere(p.prop, "the server says no");
			s_local = SLocal();
		}
		break;
	case eP_Force:
		if (s_local.prop == p.prop)
			DropHere(p.prop, "the server takes it");
		break;
	}
}

void CoopProps::Update(float frameTime)
{
	if (!Enabled())
		return;
	const float now = Now();
	for (size_t i = 0; i < s_unignore.size();)
	{
		if (now < s_unignore[i].at)
		{
			++i;
			continue;
		}
		IEntity* pEntity = gEnv->pEntitySystem->GetEntity(s_unignore[i].prop);
		if (pEntity && pEntity->GetPhysics())
		{
			pe_action_update_constraint up;
			up.bRemove = true;
			up.idConstraint = s_unignore[i].id;
			pEntity->GetPhysics()->Action(&up);
		}
		s_unignore[i] = s_unignore.back();
		s_unignore.pop_back();
	}
	LocalUpdate();
	if (gEnv->bServer)
		ServerUpdate();
}

void CoopProps::AfterFrameworkUpdate()
{
	if (s_holds.empty() || !Enabled())
		return;
	CActor* pMe = LocalActor();
	const EntityId me = pMe ? pMe->GetEntityId() : 0;
	for (std::map<EntityId, SHold>::iterator it = s_holds.begin(); it != s_holds.end(); ++it)
	{
		if (it->second.holder == me)
			continue;
		CActor* pHolder = ActorOf(it->second.holder);
		IEntity* pEntity = gEnv->pEntitySystem->GetEntity(it->first);
		if (!pHolder || !pEntity)
			continue;
		const Matrix34 body = BodyFrame(pHolder);
		const Matrix34 tm = body * Matrix34::Create(Vec3(pEntity->GetScale()), it->second.relRot, it->second.relPos);
		{
			static float s_placeLogAt = 0.0f;
			if (Now() >= s_placeLogAt)
			{
				s_placeLogAt = Now() + 2.0f;
				const Vec3 f = body.GetTranslation(), at = tm.GetTranslation();
				CryLogAlways("[CoopProp] %s at %s's hands here: %.2f %.2f %.2f from his feet (at %.2f %.2f %.2f; it at %.2f %.2f %.2f)",
					pEntity->GetName(), pHolder->GetEntity()->GetName(), it->second.relPos.x, it->second.relPos.y, it->second.relPos.z,
					f.x, f.y, f.z, at.x, at.y, at.z);
			}
		}
		if (IPhysicalEntity* pPE = pEntity->GetPhysics())
		{
			// (awake, as single player's held thing: breakable joints)
			pe_action_set_velocity sv;
			sv.v = Vec3(0.01f, 0.01f, 0.01f);
			pPE->Action(&sv);
		}
		pEntity->SetWorldTM(tm, ENTITY_XFORM_USER);
	}
}

void CoopProps::Reset()
{
	s_holds.clear();
	s_flights.clear();
	s_touched.clear();
	s_seq.clear();
	s_unignore.clear();
	s_local = SLocal();
	s_haveEpoch = false;
	s_dropWhenHeld = 0;
	s_ownFlights.clear();
	s_restLater.clear();
	s_base.clear();
}

void CoopProps::CaptureBase()
{
	s_base.clear();
	if (!gEnv->bServer || !gEnv->pEntitySystem)
		return;
	IEntityItPtr pIt = gEnv->pEntitySystem->GetEntityIterator();
	pIt->MoveFirst();
	while (IEntity* pEntity = pIt->Next())
		if (Eligible(pEntity))
		{
			SBase b = { pEntity->GetWorldPos(), pEntity->GetWorldRotation() };
			s_base[pEntity->GetId()] = b;
		}
	CryLogAlways("[CoopProp] %d things to carry in this level", (int)s_base.size());
}

void CoopProps::BeforeLoad()
{
	if (!gEnv->bServer)
		return;
	// (the load puts everything back where it was saved; the clients' hands
	// let go of what they hold)
	for (std::map<EntityId, SHold>::iterator it = s_holds.begin(); it != s_holds.end(); ++it)
		if (it->second.channel)
		{
			CGameRules::CoopPropParams p;
			Fill(p, eP_Force, it->first, 0);
			p.holder = it->second.holder;
			if (CGameRules* pRules = Rules())
				pRules->CoopSendProp(p, it->second.channel);
		}
	if (!s_holds.empty())
		CryLogAlways("[CoopProp] a load: %d things out of the players' hands", (int)s_holds.size());
	s_holds.clear();
	s_flights.clear();
	s_unignore.clear();
	s_restLater.clear();
	s_local = SLocal();
}

void CoopProps::OnGameLoaded()
{
	if (!gEnv->bServer || !Enabled())
		return;
	++s_epoch;
	s_seq.clear();
	// (every thing a hand moved since the level began, and every thing the
	// load put elsewhere than the level file has it - a campaign continued
	// with a barrel carried to a door: where the load put it; one still
	// falling is mirrored as a flight)
	int sent = 0, messages = 0;
	for (std::map<EntityId, SBase>::iterator it = s_base.begin(); it != s_base.end(); ++it)
	{
		IEntity* pEntity = gEnv->pEntitySystem->GetEntity(it->first);
		if (!pEntity || s_holds.count(it->first) || !(s_touched.count(it->first) || MovedFromBase(it->first, pEntity)))
			continue;
		messages += SendWhere(it->first, pEntity, 0);
		++sent;
	}
	CryLogAlways("[CoopProp] after the load: %d moved things sent where they lie (epoch %d; %d messages to the games in the game, one still coming in is told as it is in)",
		sent, (int)s_epoch, messages);
}

void CoopProps::SendHistory(int channelId)
{
	if (!gEnv->bServer || !channelId || !Enabled())
		return;
	CGameRules* pRules = Rules();
	if (!pRules)
		return;
	int rest = 0, reached = 0;
	for (std::map<EntityId, SBase>::iterator it = s_base.begin(); it != s_base.end(); ++it)
	{
		IEntity* pEntity = gEnv->pEntitySystem->GetEntity(it->first);
		if (!pEntity || s_holds.count(it->first) || !(s_touched.count(it->first) || MovedFromBase(it->first, pEntity)))
			continue;
		reached += SendWhere(it->first, pEntity, channelId);
		++rest;
	}
	for (std::map<EntityId, SHold>::iterator it = s_holds.begin(); it != s_holds.end(); ++it)
		BroadcastHeld(it->first, it->second, channelId);
	if (rest || !s_holds.empty())
	{
		if (reached || !rest)
			CryLogAlways("[CoopProp] a joining game is told of %d moved things and %d held", rest, (int)s_holds.size());
		else
			CryLogAlways("[CoopProp] channel %d is not in the game yet: its %d moved things come when it is", channelId, rest);
	}
}
