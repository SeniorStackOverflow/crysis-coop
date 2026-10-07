#pragma once

#include <map>
#include <vector>

// Crysis Coop: run the AI system in network games.
//
// In a multiplayer game context CryAction keeps the AI system disabled
// (IAISystem::Enable(false): "not updated, nothing is loaded, no AIObjects
// created for entities") and never does the RESET_ENTER_GAME that single
// player does on level start. That leaves every soldier of a campaign level
// loaded and equipped but without an AI object: no behaviour, weapon never
// drawn, no reaction. These hooks turn it back on, on the server only.
struct IFunctionHandler;
struct IEntity;
struct IVehicle;
struct IActor;

namespace CoopAI
{
	void Init();
	void OnLoadingStart(const char* levelName);
	void OnLoadingComplete();
	void Update(float frameTime);
	void OnGameEnded();

	// true in any network game, also while the coop server runs its level as
	// single player (gEnv->bMultiplayer == false, coop_sp_world 1). Use it
	// where the code must keep network behaviour: no pausing, no reloading
	// the last save on death, remote player handling on the server.
	bool IsNetGame();

	// a network game on one of the coop campaign levels (Levels/Multiplayer/
	// TIA/coop_*): the HUD then works as in the campaign (objectives, PDA
	// map) for every player
	bool IsCoopSession();
	// a co-op campaign level (Levels/Multiplayer/TIA/coop_*)
	bool IsCoopLevelName(const char* levelName);

	// send the objective list to a client again a little later
	void QueueObjectiveResend(int channelId);

	// presentation flow nodes (HUD, screen FX, dialogs, cutscenes...) fired
	// by the level scripts on the server are repeated on every client
	void OnFlowMirror(const char* type, uint32 key, uint32 node, uint32 port, uint32 entity, const char* values);
	void SendFlowHistory(int channelId);
	// voice sounds (dialog lines, AI barks) started on the server, played
	// again on the clients
	void OnVoiceMirror(const char* name, const Vec3& pos, uint32 flags);

	// an AI soldier fired on the server: shown on the clients
	void OnAIShot(EntityId shooterId, IEntity* pWeapon, bool mounted, const Vec3& pos, const Vec3& dir);
	void OnShotMirror(EntityId shooterId, const char* weaponClass, const Vec3& pos, const Vec3& dir, EntityId mountedId, const char* mountedName);

	// map/radar markers: every change of the host's radar is repeated on the
	// clients (op: see ERadarOp in CoopAI.cpp); cutscenes follow the host's
	void OnRadarOp(int op, EntityId id, int type, float f, const char* text);
	// server: a level entity was hidden/shown (story spawns, cutscene props...)
	void OnEntityHidden(IEntity* pEntity, bool hidden);
	// client: the server's view of an AI soldier (the clients have no AI
	// objects): enemy of the players? alertness 0 idle, 1 suspicious, 2 combat.
	// false when unknown (and always on the server)
	bool GetMirroredAI(EntityId id, bool& hostile, int& alertness, bool* pEnabled = 0);
	// the host's player (on a client: as the server said; on the server: its own)
	EntityId HostPlayerId();
	// client: a living enemy sits in the vehicle (the vehicle's own check,
	// which keeps the players out then, asks AI objects a client has not)
	bool IsVehicleCrewHostile(IVehicle* pVehicle);
	// server: a soldier got out of a vehicle (see UpdateVehicleExits)
	void OnAIExitedVehicle(EntityId id);
	// server: NanoSuit:ModeControl ran (it applies to every player); the
	// joined players' own machines get it too (their HUD, their mode choice)
	void OnSuitModeControl(int mode, bool add, bool remove, bool defect, bool repair);
	// a player in the air: free fall / parachute, or nothing to stand on
	// below him (a scripted fall such as the HALO jump moves him by script,
	// so the actor does not report "flying")
	bool IsAirborne(EntityId id);
	// server: the HUD:ProgressBar flow node did something (op 0 hide, 1 show,
	// 2 progress): the clients do the same
	void OnProgressBar(int op, int progress, int posX, int posY, const char* text, bool topText, bool locking);
	void OnSyncMirror(int kind, int op, uint32 entity, const char* name, const char* text, int type, float f, bool fromClient);
	// client: the server's state of a vehicle (see UpdateVehicleMirror)
	void OnVehicleState(uint32 id, uint16 seq, const Vec3& pos, const Quat& rot, const Vec3& vel, const Vec3& w);
	int DebugFlags();

	// is the entity bound to the network (known to clients)? logs once per
	// entity when not (only in network games; true otherwise)
	bool IsNetBound(EntityId id, EntityId id2 = 0);
	// an item picked up before it (or its owner) was bound to the network: the
	// clients are told once it is, else they never see it in his hands
	void QueuePickup(EntityId actorId, EntityId itemId);
	void LogUnbound(const char* what, EntityId a, EntityId b);

	// coop_god: players take no damage
	bool GodMode();

	// a player's equipment by item / ammo class, kept by player name from
	// level to level and in the coop progress save
	struct SInventory
	{
		std::vector<string> items;
		std::vector<std::pair<string, int> > ammo;
		string current;
		string name;            // the player's name when it was taken
	};
	// who a player is from game to game: "id:<his relay player id>" (the host
	// and friends joined through the relay), else "name:<player name>"
	string PlayerKey(IActor* pActor);
	// server: every player's inventory by PlayerKey (a dead player: his last
	// snapshot); the host's own is left out if !includeLocal
	void CollectInventories(std::map<string, SInventory>& out, bool includeLocal);
	// server: hands the items and ammo out (replace: instead of what he has)
	void GiveInventory(IActor* pActor, const SInventory& inv, bool replace);
	// server: given back by player name when the players are equipped on
	// that coop map (see coop_inv_restore)
	void SetInventoryCarry(const char* level, const std::map<string, SInventory>& inventories);
	// server: a saved game was loaded into the running level
	void OnGameLoaded();

	// an actor died (CActor::Kill, server and clients): traced, and his state
	// a moment later (still in the vehicle? ragdoll?) too
	void OnActorKilled(IEntity* pEntity, bool inVehicle);
	void OnActorKilling(IEntity* pEntity);
	// client: this frame the view is the eyes of that player (a player
	// waiting to spawn): his model is not drawn here meanwhile
	void OnEyeView(EntityId playerId);

	// server: an actor within reach of a joined player gets the full update
	// the engine otherwise only gives to what is near the host's camera
	bool KeepFullUpdate(IEntity* pEntity);

	// detailed trace (coop_trace 1): coop_trace_server.log / _client.log
	bool TraceOn();
	void Trace(const char* fmt, ...);
	void TraceInput(const char* action, int mode, float value);
	// HUD calls; a call repeated with the same text (same key) is written once
	void TraceHUD(const char* key, const char* fmt, ...);
	// a Lua HUD.* call with its arguments
	void TraceScriptCall(const char* function, IFunctionHandler* pH);
}

// diagnostics: a game object failed to (de)serialize a network aspect
struct IEntity;
struct IVehicle;
void CoopNetSerFail(IEntity* pEntity, int aspect, int profile, bool reading, int line);
void CoopNetSerTrace(IEntity* pEntity, int aspect, int profile, bool reading);
void CoopNetSerDumpTrace();
