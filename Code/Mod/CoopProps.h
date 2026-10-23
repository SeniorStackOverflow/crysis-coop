#pragma once

#include "GameRules.h"

class CActor;
struct IEntity;

// Crysis Coop: things carried in the left hand (crates, barrels, spades...)
// for everybody, the same on every machine. The server (host) decides who
// holds what; the holder's own game shows it in his hands as single player
// does, every other game puts it at his hands from where he looks; a throw or
// a drop is sent with the thing's place and speed as the holder made it, and
// the server sends its flight and where it comes to rest.
namespace CoopProps
{
	enum EOp
	{
		// client -> server
		eP_Grab = 1,        // picked it up (grab, rel)
		eP_Release = 2,     // let go of it (grab = ERelease; pos, rot, v, w)
		eP_Rel = 3,         // where it is in the hands now (rel)
		// server -> clients
		eP_Held = 10,       // holder holds it
		eP_Released = 11,   // let go of: from pos/rot with v/w
		eP_Deny = 12,       // the holder may not have it: drop it
		eP_Force = 13,      // the holder must let go of it (died, in a vehicle...)
		eP_Rest = 14,       // it lies here
		eP_State = 15,      // its flight now
	};
	enum ERelease { eR_Throw = 0, eR_Drop = 1, eR_Forced = 2 };

	// a coop game with carrying on (coop_props)
	bool Enabled();
	// a thing anybody may pick up in a coop game: a loose physics prop of the
	// level (not a soldier, not a piece of something broken, not a rock of the
	// ground)
	bool Eligible(IEntity* pEntity);
	// the hold place in front of the eyes (one or two hands): for the
	// companion's own game (no first-person view) and for every other game
	Matrix34 HoldMatrix(CActor* pHolder, IEntity* pEntity, int grabType, const Matrix34* pHoldOffset = 0);

	// the holder's own game: it picked the thing up / let go of it (after the
	// speed was set: v and w as given)
	void OnLocalGrab(CActor* pHolder, EntityId prop, int grabType);
	void OnLocalRelease(CActor* pHolder, EntityId prop, int mode, const Vec3& v, const Vec3& w);
	// held (anyone, as this game knows) / held or still flying (the server)
	bool IsHeld(EntityId prop);
	bool IsBusy(EntityId prop);
	// the local player holds it in this game with the server's leave
	bool HeldHere(EntityId prop);

	void OnServerMessage(const CGameRules::CoopPropParams& params, CActor* pFrom, int channelId);
	void OnClientMessage(const CGameRules::CoopPropParams& params, bool state);

	void Update(float frameTime);     // before the framework's update
	void AfterFrameworkUpdate();      // others' things at their hands
	void Reset();                     // a level starts or ends
	void CaptureBase();               // the server, the level loaded: where its things lie in the level file
	void BeforeLoad();                // the host loads a checkpoint
	void OnGameLoaded();              // ... loaded
	void SendHistory(int channelId);  // a client joins: what was moved, who holds what
}
