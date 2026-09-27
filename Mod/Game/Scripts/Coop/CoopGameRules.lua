--------------------------------------------------------------------------
-- Crysis Coop - gamerules overrides for TeamInstantAction.
-- Loaded as the last statement of Scripts/GameRules/TeamInstantAction.lua.
-- ORDER MATTERS: generic overrides first, then DefaultState() re-run,
-- then state-specific functions (DefaultState wipes them).
--------------------------------------------------------------------------
Script.LoadScript("scripts/coop/coopconfig.lua", 1, 1);

local C = CoopConfig;

local function CoopLog(msg)
	if (C.DEBUG) then
		System.LogAlways("[Coop] "..tostring(msg));
	end
end

local function CopyVec(v)
	return { x=v.x, y=v.y, z=v.z };
end

CoopLog("gamerules module loaded, version "..tostring(C.VERSION));

--------------------------------------------------------------------------
-- 0. AI registration in network games.
--    CryAction's AI.RegisterWithAI silently refuses to create AI objects
--    while the game is a network game. Without them soldiers never act,
--    and without the players' own AI objects the AI cannot see players
--    (it only hears gunshots) and has nothing to shoot at. Coop.dll adds
--    "coop_sp_scope 1/0" which makes the engine treat the call as single
--    player; every registration on the server is wrapped in it: soldiers,
--    vehicles, players and reinforcements spawned later by level scripts.
--------------------------------------------------------------------------
local function CoopCallRegister(a, b, c, d, e, f)
	local orig = g_CoopOrigRegisterWithAI;
	if (f ~= nil) then
		return orig(a, b, c, d, e, f);
	elseif (e ~= nil) then
		return orig(a, b, c, d, e);
	elseif (d ~= nil) then
		return orig(a, b, c, d);
	elseif (c ~= nil) then
		return orig(a, b, c);
	end
	return orig(a, b);
end

if (AI and AI.RegisterWithAI and not g_CoopOrigRegisterWithAI) then
	g_CoopOrigRegisterWithAI = AI.RegisterWithAI;
	AI.RegisterWithAI = function(a, b, c, d, e, f)
		if (not CryAction.IsServer()) then
			return CoopCallRegister(a, b, c, d, e, f);
		end
		System.ExecuteCommand("coop_sp_scope 1");
		local ok, res = pcall(CoopCallRegister, a, b, c, d, e, f);
		System.ExecuteCommand("coop_sp_scope 0");
		if (not ok) then
			System.LogAlways("[Coop] AI.RegisterWithAI failed: "..tostring(res));
			return nil;
		end
		return res;
	end;
	CoopLog("AI.RegisterWithAI wrapped (single-player scope on the server)");
end

--------------------------------------------------------------------------
-- 1. static tuning
--------------------------------------------------------------------------
TeamInstantAction.teamName = { C.TEAM_NAME };
TeamInstantAction.START_TIMER = C.START_TIMER;
TeamInstantAction.INVULNERABILITY_TIME = C.SPAWN_INVULNERABILITY;
TeamInstantAction.TEAM_SPAWN_LOCATIONS = false;
TeamInstantAction.COOP_MAPCHANGE_TIMERID = 1090;

--------------------------------------------------------------------------
-- 2. the match never ends by time/score and never waits for players
--------------------------------------------------------------------------
function TeamInstantAction:PlayerCountOk()
	return true;
end

function TeamInstantAction:CheckTimeLimit()
end

function TeamInstantAction:CheckScoreLimit(teamId, score)
end

function TeamInstantAction:CheckPlayerScoreLimit(playerId, score)
end

--------------------------------------------------------------------------
-- 3. AI equipment: InstantAction:EquipActor is an empty MP stub, which
--    would leave every SP enemy unarmed. Use the SP implementation.
--------------------------------------------------------------------------
function TeamInstantAction:EquipActor(actor)
	self.coopEquipCalls = (self.coopEquipCalls or 0) + 1;
	if (self.coopEquipCalls <= 3 or math.floor(self.coopEquipCalls/25)*25 == self.coopEquipCalls) then
		CoopLog(string.format("EquipActor #%d %s pack=%s isPlayer=%s",
			self.coopEquipCalls, tostring(actor:GetName()),
			tostring(actor.Properties and actor.Properties.equip_EquipmentPack),
			tostring(actor.actor and actor.actor:IsPlayer())));
	end
	-- Items spawned while the level is loading are treated by the network
	-- layer as part of the level ("static" objects) and must be spawned in
	-- exactly the same order on the server and on every client. AI
	-- equipment is given on the server only, so during loading it is
	-- queued and handed out once the game has started (Coop.dll calls
	-- CoopFlushDeferredEquip); the items are then ordinary network objects
	-- that clients receive from the server.
	-- only the server hands out equipment; clients receive the items
	if (not CryAction.IsServer()) then
		return;
	end
	if (g_coopLoading) then
		self.coopDeferredEquip = self.coopDeferredEquip or {};
		self.coopDeferredEquipSet = self.coopDeferredEquipSet or {};
		if (not self.coopDeferredEquipSet[actor.id]) then
			self.coopDeferredEquipSet[actor.id] = true;
			table.insert(self.coopDeferredEquip, actor.id);
		end
		return;
	end
	SinglePlayer.EquipActor(self, actor);
end

function TeamInstantAction:CoopFlushDeferredEquip()
	local list = self.coopDeferredEquip;
	self.coopDeferredEquip = nil;
	self.coopDeferredEquipSet = nil;
	if (not list) then
		return 0;
	end
	local n = 0;
	for i, id in ipairs(list) do
		local actor = System.GetEntity(id);
		if (actor and actor.actor and not actor:IsDead()) then
			if (actor.actor:IsPlayer() and actor.coopNeedsCoopEquip) then
				actor.coopNeedsCoopEquip = nil;
				self:EquipPlayer(actor);
			else
				SinglePlayer.EquipActor(self, actor);
			end
			n = n + 1;
		end
	end
	CoopLog(string.format("deferred AI equipment handed out: %d of %d actors", n, #list));
	return n;
end

--------------------------------------------------------------------------
-- 4. single coop team
--------------------------------------------------------------------------
function TeamInstantAction:CoopTeamId()
	if (self.teamId and self.teamId[1]) then
		return self.teamId[1];
	end
	return self.game:GetTeamId(C.TEAM_NAME) or 0;
end

function TeamInstantAction:AutoAssignTeam(player, forceTeamId)
	local teamId = self:CoopTeamId();
	if (teamId ~= 0 and self.game:GetTeam(player.id) ~= teamId) then
		self.game:SetTeam(teamId, player.id);
	end
end

--------------------------------------------------------------------------
-- 5. leader and anchor
--------------------------------------------------------------------------
function TeamInstantAction:CoopIsAlive(entity)
	if (not entity or not entity.actor) then
		return false;
	end
	if (entity:IsDead()) then
		return false;
	end
	return entity.actor:GetSpectatorMode() == 0;
end

-- host (listen server local player) if alive, else first alive player
function TeamInstantAction:CoopGetLeader(excludeId)
	if (g_localActorId and g_localActorId ~= excludeId) then
		local host = System.GetEntity(g_localActorId);
		if (self:CoopIsAlive(host)) then
			return host;
		end
	end
	local players = self.game:GetPlayers();
	if (players) then
		for i,p in ipairs(players) do
			if (p.id ~= excludeId and self:CoopIsAlive(p)) then
				return p;
			end
		end
	end
	return nil;
end

-- why the player is not in the playable world (nil: he is): dead or
-- spectating, flying, swimming, in the air, or parked at the world origin
-- where SP logic puts the player before the level intro places him
function TeamInstantAction:CoopNotInWorld(entity)
	if (not self:CoopIsAlive(entity)) then
		return "not alive";
	end
	local okF, flying = pcall(entity.actor.IsFlying, entity.actor);
	if (okF and flying) then
		return "flying";
	end
	-- e.g. on Contact the HALO jump ends in the sea: a floating host is slow
	-- and "not flying", but teammates must not be spawned in open water
	if (STANCE_SWIM) then
		local okS, stance = pcall(entity.actor.GetStance, entity.actor);
		if (okS and stance == STANCE_SWIM) then
			return "swimming";
		end
	end
	-- in the air (the HALO jump on Contact: the script moves the host, the
	-- actor does not report "flying") - teammates would drop like a stone
	if (HUD and HUD.CoopIsAirborne and HUD.CoopIsAirborne(entity.id)) then
		return "in the air";
	end
	local p = entity:GetWorldPos(g_Vectors.temp_v1);
	if (math.abs(p.x) + math.abs(p.y) < 10) then
		return "at the origin";
	end
	return nil;
end

-- "settled" = in the world and (almost) not moving. Guards against spawning
-- teammates next to a host who is still in the intro plane, in free fall, or
-- in a cutscene.
function TeamInstantAction:CoopIsSettled(entity)
	if (self:CoopNotInWorld(entity)) then
		return false;
	end
	-- speed measured by CoopSampleSpeeds() from position deltas between ticks:
	-- actor:IsFlying() is false during the HALO free fall and GetVelocity()
	-- returned nothing usable, so neither can be trusted here
	local speed = self.coopSpeed and self.coopSpeed[entity.id];
	return (speed ~= nil and speed <= C.SETTLE_MAX_SPEED);
end

-- called once per tick (1s): speed of every player from his position delta
function TeamInstantAction:CoopSampleSpeeds()
	self.coopSpeed = self.coopSpeed or {};
	self.coopPrevPos = self.coopPrevPos or {};
	local now = _time;
	local players = self.game:GetPlayers();
	if (players) then
		for i,p in ipairs(players) do
			local pos = CopyVec(p:GetWorldPos(g_Vectors.temp_v1));
			local prev = self.coopPrevPos[p.id];
			if (prev and now > prev.t) then
				local dx = pos.x - prev.pos.x;
				local dy = pos.y - prev.pos.y;
				local dz = pos.z - prev.pos.z;
				self.coopSpeed[p.id] = math.sqrt(dx*dx + dy*dy + dz*dz) / (now - prev.t);
			end
			self.coopPrevPos[p.id] = { pos=pos, t=now };
		end
	end
end

-- world is "ready" once the leader has been settled for SETTLE_SECONDS in a
-- row at least once (i.e. the level intro is over)
-- the leader is in the playable world - speed is not checked
function TeamInstantAction:CoopIsInWorld(entity)
	return self:CoopNotInWorld(entity) == nil;
end

-- a joiner does not wait for ever: a host away from the keyboard can stay
-- "not landed" for the checks above (the physics may still call a host who
-- has not moved since a cutscene "flying"; one floating in the sea swims).
-- Once the joiner has waited JOIN_WAIT_MAX seconds outside cutscenes, he is
-- spawned next to a host who stands still (or is in the world anyway).
function TeamInstantAction:CoopCanJoinIdleHost(leader)
	if (not self:CoopIsAlive(leader)) then
		return false;
	end
	local p = leader:GetWorldPos(g_Vectors.temp_v1);
	if (math.abs(p.x) + math.abs(p.y) < 10) then
		return false;
	end
	local speed = self.coopSpeed and self.coopSpeed[leader.id];
	return (speed ~= nil and speed <= C.SETTLE_MAX_SPEED) or self:CoopIsInWorld(leader);
end

function TeamInstantAction:CoopUpdateSettled()
	local leader = self:CoopGetLeader(nil);
	self.coopLevelTicks = (self.coopLevelTicks or 0) + 1;
	-- fallback: some levels keep the host moving (vehicle, scripted walk)
	-- for a long time; after a while "in the world" is enough
	if ((not self.coopWorldReady) and self.coopLevelTicks >= 40 and leader and self:CoopIsInWorld(leader)) then
		self.coopWorldReady = true;
		self.coopRelaxedSpawn = true;
		CoopLog("world ready (fallback after 40s): leader is in the world");
	end
	local joinersWaiting = (not self.coopWorldReady) and self.coopWaiting and next(self.coopWaiting);
	local cutscene = HUD and HUD.CoopIsPlayingCutscene and HUD.CoopIsPlayingCutscene();
	if (joinersWaiting and math.floor(self.coopLevelTicks/10)*10 == self.coopLevelTicks and leader) then
		local p = leader:GetWorldPos(g_Vectors.temp_v1);
		CoopLog(string.format("waiting joiners: leader %s pos=(%.0f,%.0f,%.0f) notInWorld=%s settled=%s speed=%s cutscene=%s waited=%s",
			tostring(leader:GetName()), p.x, p.y, p.z, tostring(self:CoopNotInWorld(leader)),
			tostring(self:CoopIsSettled(leader)), tostring(self.coopSpeed and self.coopSpeed[leader.id]),
			tostring(cutscene), tostring(self.coopJoinWait)));
	end
	-- see CoopCanJoinIdleHost: the wait outside cutscenes is limited
	if (joinersWaiting and not cutscene) then
		self.coopJoinWait = (self.coopJoinWait or 0) + 1;
		if (self.coopJoinWait >= C.JOIN_WAIT_MAX and leader and self:CoopCanJoinIdleHost(leader)) then
			self.coopWorldReady = true;
			self.coopRelaxedSpawn = true;
			self.coopIdleSpawn = true;
			CoopLog("world ready: a joiner waited "..self.coopJoinWait.."s, the host stays where he is ("
				..(self:CoopNotInWorld(leader) or "in the world")..", speed "..tostring(self.coopSpeed and self.coopSpeed[leader.id])
				.."): teammates spawn next to him");
		end
	end
	if (leader and self:CoopIsSettled(leader)) then
		self.coopSettledTicks = (self.coopSettledTicks or 0) + 1;
	else
		self.coopSettledTicks = 0;
	end
	if ((not self.coopWorldReady) and self.coopSettledTicks >= C.SETTLE_SECONDS) then
		self.coopWorldReady = true;
		CoopLog("world ready: leader settled, teammates can spawn now");
	end
end

function TeamInstantAction:CoopRecordAnchor()
	local leader = self:CoopGetLeader(nil);
	if (leader and self.coopWorldReady and self:CoopIsSettled(leader)) then
		-- g_Vectors.temp_* are shared scratch vectors: always copy out of them
		local p = leader:GetWorldPos(g_Vectors.temp_v1);
		local a = leader:GetWorldAngles(g_Vectors.temp_v2);
		self.coopAnchor = { pos=CopyVec(p), ang=CopyVec(a) };
	end
end

-- returns pos, angles, foundLeaderOrAnchor
function TeamInstantAction:CoopGetSpawnTransform(player)
	local basePos, baseAng;
	local leader = self:CoopGetLeader(player.id);
	if (leader and self.coopWorldReady and (self:CoopIsSettled(leader) or (self.coopRelaxedSpawn and self:CoopIsInWorld(leader))
		or (self.coopIdleSpawn and self:CoopCanJoinIdleHost(leader)))) then
		basePos = CopyVec(leader:GetWorldPos(g_Vectors.temp_v1));
		baseAng = CopyVec(leader:GetWorldAngles(g_Vectors.temp_v2));
	elseif (self.coopWorldReady and self.coopAnchor) then
		basePos = CopyVec(self.coopAnchor.pos);
		baseAng = CopyVec(self.coopAnchor.ang);
	else
		-- nobody to spawn next to: behave like SinglePlayer (0,0,0) and let the
		-- level flowgraph (Game:LocalPlayer) place the host
		return { x=0, y=0, z=0 }, { x=0, y=0, z=0 }, false;
	end

	self.coopSpawnSlot = (self.coopSpawnSlot or 0) + 1;
	if (self.coopSpawnSlot > C.SPAWN_SLOTS) then
		self.coopSpawnSlot = 1;
	end
	local angle = self.coopSpawnSlot * (2 * math.pi / C.SPAWN_SLOTS);
	local pos = {
		x = basePos.x + math.cos(angle) * C.SPAWN_RADIUS,
		y = basePos.y + math.sin(angle) * C.SPAWN_RADIUS,
		z = basePos.z + C.SPAWN_Z_OFFSET,
	};
	return pos, baseAng, true;
end

--------------------------------------------------------------------------
-- 6. revive (replaces InstantAction:RevivePlayer, which needs SpawnPoints
--    that SP levels don't have)
--------------------------------------------------------------------------
function TeamInstantAction:RevivePlayer(channelId, player, keepEquip, atPos, atAng)
	if (not player or not player.actor) then
		return false;
	end
	if (player:IsDead()) then
		keepEquip = false;
	end

	self:AutoAssignTeam(player);
	local teamId = self:CoopTeamId();
	local pos, ang, found = self:CoopGetSpawnTransform(player);
	-- revived by a teammate: where he fell (see section 15)
	if (atPos) then
		pos, ang, found = atPos, atAng or ang, true;
	end
	local isHost = (g_localActorId ~= nil and player.id == g_localActorId);
	if (isHost and not found) then
		if (self.coopHostSpawned and not player:IsDead() and player.actor:GetSpectatorMode() == 0) then
			-- the host is already in the level: his position belongs to the level
			-- scripts (plane, HALO jump...). Re-spawning him at the origin here would
			-- yank him out of the intro and drop him under the map.
			return true;
		end
		if (self.coopHostSpawned and self.coopHostLastPos) then
			-- host died before any anchor existed: bring him back where he last was
			pos = CopyVec(self.coopHostLastPos.pos);
			ang = CopyVec(self.coopHostLastPos.ang);
			found = true;
		elseif (not self.coopHostSpawned) then
			-- first spawn of the level: exactly what SinglePlayer:OnClientConnect
			-- does - the level's first SpawnPoint, and tell it the player spawned
			-- (many levels start their scripts from that output)
			local spawnId = self.game:GetFirstSpawnLocation(0);
			local spawn = spawnId and System.GetEntity(spawnId);
			if (spawn) then
				pos = CopyVec(spawn:GetWorldPos(g_Vectors.temp_v1));
				ang = CopyVec(spawn:GetAngles(g_Vectors.temp_v1));
				found = true;
				self.coopFirstSpawnPoint = spawn;
				CoopLog("host starts at the level spawn point "..tostring(spawn:GetName()));
			end
		end
		self.coopHostSpawned = true;
	end
	if (not found) then
		if (g_localActorId and not isHost) then
			-- intro still running (plane / cutscene / free fall): keep the joiner
			-- spectating and spawn him once the host has landed (see CoopTick)
			self.coopWaiting = self.coopWaiting or {};
			if (not self.coopWaiting[player.id]) then
				self.coopWaiting[player.id] = true;
				CoopLog("holding "..tostring(player:GetName()).." until the host has landed");
				-- watch the host meanwhile instead of looking at the world origin
				if (g_localActorId and player.actor:GetSpectatorMode() ~= 3) then
					pcall(player.actor.SetSpectatorMode, player.actor, 3, g_localActorId);
				end
				self.game:SendTextMessage(TextMessageCenter, "Waiting for the host to finish the level intro...", TextMessageToAll);
			end
			return false;
		end
		CoopLog("no leader/anchor for "..tostring(player:GetName())..", SP-style spawn at origin");
	end
	if (self.coopWaiting) then
		self.coopWaiting[player.id] = nil;
	end

	self.game:RevivePlayer(player.id, pos, ang, teamId, not keepEquip);
	player:UpdateAreas();

	if (self.coopFirstSpawnPoint) then
		local spawn = self.coopFirstSpawnPoint;
		self.coopFirstSpawnPoint = nil;
		if (spawn.Spawned) then
			spawn:Spawned(player);
		end
	end

	if (player.actor:GetSpectatorMode() ~= 0) then
		player.actor:SetSpectatorMode(0, NULL_ENTITY);
	end
	if (not keepEquip) then
		self:EquipPlayer(player);
	end
	-- died in a vehicle (VTOL, tank, boat...): in the campaign the checkpoint
	-- would give it back; here the player is put back into it (or a new one)
	if (player.coopVehicle and player.coopDiedInVehicle) then
		self:CoopQueueVehicleReturn(player);
	elseif (not isHost) then
		-- the leader travels in a vehicle (VTOL, boat, tank): take a seat in
		-- it, or get a vehicle of the same kind next to it
		local leader = self:CoopGetLeader(player.id);
		local vid = leader and leader.actor and leader.actor:GetLinkedVehicleId();
		local lv = vid and System.GetEntity(vid);
		if (lv and lv.vehicle and not lv.vehicle:IsDestroyed()) then
			self.coopVehicleReturns = self.coopVehicleReturns or {};
			self.coopVehicleReturns[player.id] = { join = vid, t = _time + 0.5 };
		end
	end
	player.coopDiedInVehicle = nil;
	player.death_time = nil;
	player.frostShooterId = nil;

	if (self.INVULNERABILITY_TIME and self.INVULNERABILITY_TIME > 0) then
		self.game:SetInvulnerability(player.id, true, self.INVULNERABILITY_TIME);
	end
	CoopLog("revived "..tostring(player:GetName()));
	return true;
end

--------------------------------------------------------------------------
-- 7. player equipment
--------------------------------------------------------------------------
function TeamInstantAction:EquipPlayer(actor, additionalEquip)
	if (self.game:IsDemoMode() ~= 0) then
		return;
	end
	-- see EquipActor: nothing may be spawned by the server alone while the
	-- level is loading
	if (g_coopLoading) then
		actor.coopNeedsCoopEquip = true;
		self.coopDeferredEquip = self.coopDeferredEquip or {};
		self.coopDeferredEquipSet = self.coopDeferredEquipSet or {};
		if (not self.coopDeferredEquipSet[actor.id]) then
			self.coopDeferredEquipSet[actor.id] = true;
			table.insert(self.coopDeferredEquip, actor.id);
		end
		return;
	end
	-- never wipe the inventory: the level scripts hand out and check the
	-- story equipment (Coop.dll keeps a snapshot of every player's inventory)
	-- 1) respawn: the player's own last inventory (like a campaign checkpoint)
	System.ExecuteCommand("coop_inv_restore "..tostring(actor:GetName()));
	if ((tonumber(System.GetCVar("coop_inv_result")) or 0) == 1) then
		return;
	end
	-- 2) a teammate: the same equipment the host has
	local host = g_localActorId and System.GetEntity(g_localActorId);
	if (host and host.id ~= actor.id) then
		System.ExecuteCommand("coop_inv_restore "..tostring(actor:GetName()).." "..tostring(host:GetName()));
		if ((tonumber(System.GetCVar("coop_inv_result")) or 0) == 1) then
			return;
		end
	end
	-- 3) nothing known yet: what the campaign gives a player
	SinglePlayer.EquipActor(self, actor);
end

--------------------------------------------------------------------------
-- 8. joining: skip team/spectator selection, spawn next to the leader
--------------------------------------------------------------------------
local stockOnClientEnteredGame = TeamInstantAction.Server.OnClientEnteredGame;
function TeamInstantAction.Server:OnClientEnteredGame(channelId, player, reset)
	stockOnClientEnteredGame(self, channelId, player, reset);
	self:AutoAssignTeam(player);
	-- RevivePlayer leaves spectator mode itself on success; if the host is
	-- still in the intro the player stays spectating and is spawned later
	self:RevivePlayer(channelId, player);
	if (not reset) then
		self.game:SendTextMessage(TextMessageCenter, tostring(player:GetName()).." joined the co-op", TextMessageToAll);
	end
end

--------------------------------------------------------------------------
-- 9. damage: difficulty multiplier for AI -> player hits (friendly fire
--    between players is handled by stock TIA OnHit via g_friendlyfireratio)
--------------------------------------------------------------------------
local stockOnHit = TeamInstantAction.Server.OnHit;
function TeamInstantAction.Server:OnHit(hit)
	local shooter = hit.shooter;
	local target = hit.target;
	local targetIsPlayer = target and target.actor and target.actor:IsPlayer();
	if (targetIsPlayer and shooter and shooter.actor and (not shooter.actor:IsPlayer())) then
		hit.damage = hit.damage * C.AI_DAMAGE_TO_PLAYER_MULT;
	end
	-- coop_god 1: players take no damage (testing); the damage the hit would
	-- have done is still logged
	local wanted = hit.damage;
	local god = targetIsPlayer and (tonumber(System.GetCVar("coop_god")) or 0) ~= 0;
	if (god) then
		hit.damage = 0;
	end
	local before = target and target.actor and target.actor:GetHealth();
	local r = stockOnHit(self, hit);
	-- debug record of every hit on an actor
	if (C.DEBUG and target and target.actor) then
		local after = target.actor:GetHealth();
		CoopLog(string.format("HIT %s%s by %s weapon=%s type=%s dmg=%.1f%s hp %s->%s%s",
			targetIsPlayer and "player " or "", tostring(target:GetName()), tostring(shooter and shooter:GetName()),
			tostring(hit.weapon and hit.weapon.class), tostring(hit.type), hit.damage or 0,
			god and string.format(" (god, would be %.1f)", wanted or 0) or "",
			tostring(before), tostring(after), (after and after <= 0 and before and before > 0) and " KILLED" or ""));
	end
	return r;
end

-- explosions (grenades, rockets, barrels) hit the actor directly
-- (BasicActor.Server:OnHit -> ProcessActorDamage), not through OnHit above:
-- with coop_god the player keeps his health here too, else the script
-- counts him dead (health <= 0), kills him and the C++ god clamp leaves him
-- lying on the ground, weaponless, with full health and no revive
local stockProcessActorDamage = TeamInstantAction.ProcessActorDamage;
function TeamInstantAction:ProcessActorDamage(hit)
	local target = hit.target;
	if (target and target.actor and target.actor:IsPlayer() and (tonumber(System.GetCVar("coop_god")) or 0) ~= 0) then
		if (C.DEBUG and hit.explosion and (hit.damage or 0) > 0) then
			CoopLog(string.format("HIT player %s by %s explosion type=%s dmg=0.0 (god, would be %.1f) hp %s->%s",
				tostring(target:GetName()), tostring(hit.shooter and hit.shooter:GetName()), tostring(hit.type),
				hit.damage or 0, tostring(target.actor:GetHealth()), tostring(target.actor:GetHealth())));
		end
		return false;
	end
	return stockProcessActorDamage(self, hit);
end

-- a vehicle with enemies inside cannot be entered: its own check asks the AI
-- objects, which a client has not, so there the server's AI state decides
local stockIsUsable = TeamInstantAction.IsUsable;
function TeamInstantAction:IsUsable(srcId, objId)
	if (objId and HUD and HUD.CoopIsVehicleCrewHostile and HUD.CoopIsVehicleCrewHostile(objId)) then
		return 0;
	end
	return stockIsUsable(self, srcId, objId);
end

--------------------------------------------------------------------------
-- 10. timers (map change)
--------------------------------------------------------------------------
local stockOnTimer = TeamInstantAction.Server.OnTimer;
function TeamInstantAction.Server:OnTimer(timerId, msec)
	if (timerId == self.COOP_MAPCHANGE_TIMERID) then
		local target = self.coopPendingMap;
		self.coopPendingMap = nil;
		if (target) then
			CoopLog("changing map to "..target);
			-- every player keeps his weapons for the next level (Coop.dll)
			System.ExecuteCommand("coop_inv_carry "..target);
			System.ExecuteCommand(string.format(C.MAP_COMMAND_FMT, target));
		end
		return;
	end
	stockOnTimer(self, timerId, msec);
end

--------------------------------------------------------------------------
-- 11. level progression (called by the Mission:EndLevelNew flow node)
--------------------------------------------------------------------------
function TeamInstantAction:CoopGetCurrentLevel()
	local map = System.GetCVar("sv_map");
	if (not map or map == "") then
		return nil;
	end
	local short = string.lower(tostring(map));
	short = string.gsub(short, "^.*coop_", "");
	short = string.gsub(short, "^.*[/\\]", "");
	return short;
end

function TeamInstantAction:CoopGetNextLevel(params)
	local cur = self:CoopGetCurrentLevel();
	if (cur) then
		for i,name in ipairs(C.CAMPAIGN) do
			if (name == cur) then
				return C.CAMPAIGN[i+1];   -- nil after the last level = campaign done
			end
		end
	end
	if (params and params.nextlevel and params.nextlevel ~= "") then
		local n = string.lower(tostring(params.nextlevel));
		n = string.gsub(n, "^.*[/\\]", "");
		return n;
	end
	return nil;
end

function TeamInstantAction:EndLevel(params)
	if (not CryAction.IsServer()) then
		return;
	end
	if (self.coopPendingMap) then
		return;   -- the level end graph fired twice
	end
	local nextName = self:CoopGetNextLevel(params);
	CoopLog("EndLevel: current="..tostring(self:CoopGetCurrentLevel())
		.." params.nextlevel="..tostring(params and params.nextlevel)
		.." -> next="..tostring(nextName));
	if (nextName) then
		self.coopPendingMap = C.LEVEL_PREFIX..nextName;
		self.game:SendTextMessage(TextMessageCenter, "Level complete! Next: "..nextName, TextMessageToAll);
		self:SetTimer(self.COOP_MAPCHANGE_TIMERID, C.MAPCHANGE_DELAY_MS);
	else
		self.game:SendTextMessage(TextMessageCenter, "Co-op campaign complete!", TextMessageToAll);
	end
end

--------------------------------------------------------------------------
-- 12. per-second coop logic, teleport RMI, console commands
--------------------------------------------------------------------------
-- In a network game the engine sets ai_SystemUpdate = 0, i.e. the AI system
-- never ticks: soldiers are loaded and equipped but stay frozen, never draw
-- their weapons and never react. Setting it in a cfg before "map" is useless
-- (overwritten on level load), so it is forced here once the level runs.
function TeamInstantAction:CoopEnsureAIRunning()
	if (not CryAction.IsServer()) then
		return;
	end
	local v = tonumber(System.GetCVar("ai_SystemUpdate"));
	if (v ~= 1) then
		System.SetCVar("ai_SystemUpdate", 1);
		CoopLog("ai_SystemUpdate was "..tostring(v)..", forced to 1 (AI would stay frozen otherwise)");
	end
	-- the cutscene code (CViewSystem) sets ai_IgnorePlayer = 1 for the duration
	-- of a cutscene; if that is never restored the AI simply ignores players
	local ip = tonumber(System.GetCVar("ai_IgnorePlayer"));
	if (ip and ip ~= 0) then
		System.SetCVar("ai_IgnorePlayer", 0);
		CoopLog("ai_IgnorePlayer was "..tostring(ip)..", forced to 0 (AI would ignore players otherwise)");
	end
end

function TeamInstantAction:CoopTick()
	self:CoopEnsureAIRunning();
	self:CoopSampleSpeeds();
	self:CoopUpdateSettled();
	self:CoopTrackVehicles();
	self:CoopProcessVehicleReturns();
	self:CoopDebugKillHost();
	self:CoopDebugUse();

	-- last position of the host that is not the pre-intro origin
	if (g_localActorId) then
		local host = System.GetEntity(g_localActorId);
		if (self:CoopIsAlive(host)) then
			local p = host:GetWorldPos(g_Vectors.temp_v1);
			if (math.abs(p.x) + math.abs(p.y) >= 10) then
				self.coopHostLastPos = { pos=CopyVec(p), ang=CopyVec(host:GetWorldAngles(g_Vectors.temp_v2)) };
			end
		end
	end

	if (self.coopWorldReady and self.coopWaiting) then
		for id,waiting in pairs(self.coopWaiting) do
			local p = System.GetEntity(id);
			if (waiting and p and p.actor) then
				self:RevivePlayer(p.actor:GetChannel(), p);
			else
				self.coopWaiting[id] = nil;
			end
		end
	end

	self.coopTickCount = (self.coopTickCount or 0) + 1;
	if (self.coopTickCount >= C.ANCHOR_INTERVAL) then
		self.coopTickCount = 0;
		self:CoopRecordAnchor();
	end

	self:CoopTickFallWatch();
	if (self.coopSpotsPending and self:CoopKeepSpots(true) > 0) then
		CoopLog("checkpoint "..tostring(self.coopSpotsPending)..": places kept now");
		self.coopSpotsPending = nil;
	end
	if (self:CoopReviveMode()) then
		self:CoopTickDowned();
	else
		local players = self.game:GetPlayers();
		if (players) then
			for i,p in ipairs(players) do
				if (p.actor and p:IsDead() and p.death_time and (_time - p.death_time) >= C.RESPAWN_DELAY) then
					self:RevivePlayer(p.actor:GetChannel(), p);
				end
			end
		end
	end

	self:CoopAutotestTick();
end

--------------------------------------------------------------------------
-- 12b. diagnostics: coop_ai_report, and an automatic test that runs when
--      sv_servername contains "AUTOTEST" (never on a normal server)
--------------------------------------------------------------------------
local function Dist2(a, b)
	local dx = a.x - b.x;
	local dy = a.y - b.y;
	local dz = a.z - b.z;
	return dx*dx + dy*dy + dz*dz;
end

function TeamInstantAction:CoopGetGrunts()
	return System.GetEntitiesByClass("Grunt") or {};
end

local COOP_WEAPON_CLASSES = { "FY71", "SCAR", "SOCOM", "Shotgun", "SMG", "DSG1", "LAW", "Hurricane", "GaussRifle" };

-- first weapon class found in the actor's inventory (holstered or not)
local function CoopFindWeapon(g)
	if (not g.inventory) then
		return nil;
	end
	for i,cls in ipairs(COOP_WEAPON_CLASSES) do
		local ok, id = pcall(g.inventory.GetItemByClass, g.inventory, cls);
		if (ok and id) then
			return cls;
		end
	end
	return nil;
end

function TeamInstantAction:CoopAIReport(nearPos, radius)
	local total, alive, withItem, near, armed, moved = 0, 0, 0, 0, 0, 0;
	local aiEnabled, aiObject = 0, 0;
	self.coopGruntPos = self.coopGruntPos or {};
	for i,g in ipairs(self:CoopGetGrunts()) do
		total = total + 1;
		if (g.actor and not g:IsDead()) then
			alive = alive + 1;
			local item = nil;
			if (g.inventory) then
				local ok, res = pcall(g.inventory.GetCurrentItem, g.inventory);
				if (ok) then item = res; end
			end
			if (item) then
				withItem = withItem + 1;
			end
			if (CoopFindWeapon(g)) then
				armed = armed + 1;
			end
			local gp = CopyVec(g:GetWorldPos(g_Vectors.temp_v1));
			local prev = self.coopGruntPos[g.id];
			if (prev and Dist2(prev, gp) > 1) then
				moved = moved + 1;
			end
			self.coopGruntPos[g.id] = gp;
			local okE, enabled = pcall(AI.IsEnabled, g.id);
			if (okE and enabled) then
				aiEnabled = aiEnabled + 1;
			end
			local okO, aiPos = pcall(AI.GetAIObjectPosition, g.id);
			if (okO and aiPos) then
				aiObject = aiObject + 1;
			end
			if (total <= 3) then
				local okT, target = pcall(AI.GetAttentionTargetOf, g.id);
				local okH, hostile = false, nil;
				if (g_localActorId) then
					okH, hostile = pcall(AI.Hostile, g.id, g_localActorId);
				end
				CoopLog(string.format("  sample %s item=%s weapon=%s aiEnabled=%s aiObject=%s hostileToHost=%s attentionTarget=%s",
					tostring(g:GetName()), tostring(item and item.class), tostring(CoopFindWeapon(g)),
					tostring(okE and enabled), tostring(okO and aiPos ~= nil),
					tostring(okH and hostile), tostring(okT and target)));
			end
			if (nearPos) then
				local p = g:GetWorldPos(g_Vectors.temp_v1);
				if (Dist2(p, nearPos) <= radius*radius) then
					near = near + 1;
					local okA, alert = pcall(AI.GetAlertness, g.id);
					CoopLog(string.format("  near %s item=%s pack=%s health=%s alertness=%s",
						tostring(g:GetName()), tostring(item and item.class),
						tostring(g.Properties and g.Properties.equip_EquipmentPack),
						tostring(g.actor:GetHealth()), tostring(okA and alert)));
				end
			end
		end
	end
	CoopLog(string.format("AI report: grunts=%d alive=%d aiObject=%d aiEnabled=%d holdingItem=%d weaponInInventory=%d movedSinceLast=%d near=%d sv_AISystem=%s ai_UpdateAllAlways=%s equipCalls=%s",
		total, alive, aiObject, aiEnabled, withItem, armed, moved, near,
		tostring(System.GetCVar("sv_AISystem")), tostring(System.GetCVar("ai_UpdateAllAlways")), tostring(self.coopEquipCalls)));
end

-- grunts within radius of the host: how many, how many moved since the last
-- call, how many hold a weapon, and the host's health (does anyone shoot him?)
function TeamInstantAction:CoopNearReport(host, radius)
	if (not host) then
		return;
	end
	local hp = CopyVec(host:GetWorldPos(g_Vectors.temp_v2));
	self.coopPrevNear = self.coopPrevNear or {};
	local n, moved, holding = 0, 0, 0;
	for i,g in ipairs(self:CoopGetGrunts()) do
		if (g.actor and not g:IsDead()) then
			local p = CopyVec(g:GetWorldPos(g_Vectors.temp_v1));
			if (Dist2(p, hp) <= radius*radius) then
				n = n + 1;
				local prev = self.coopPrevNear[g.id];
				if (prev and Dist2(prev, p) > 1) then
					moved = moved + 1;
				end
				self.coopPrevNear[g.id] = p;
				local ok, item = pcall(g.inventory.GetCurrentItem, g.inventory);
				if (ok and item) then
					holding = holding + 1;
				end
			end
		end
	end
	System.ExecuteCommand("coop_aistats");
	CoopLog(string.format("NEAR player=(%.0f,%.0f,%.0f) grunts%dm=%d holdingItem=%d movedSinceLast=%d health=%s ai_SystemUpdate=%s ai_IgnorePlayer=%s",
		hp.x, hp.y, hp.z, radius, n, holding, moved, tostring(host.actor:GetHealth()),
		tostring(System.GetCVar("ai_SystemUpdate")), tostring(System.GetCVar("ai_IgnorePlayer"))));
end

function TeamInstantAction:CoopAutotestTick()
	local t = self.coopAutotest;
	if (not t) then
		return;
	end
	t.sec = t.sec + 1;
	local host = nil;
	if (g_localActorId) then
		host = System.GetEntity(g_localActorId);
	end

	-- phase 1: wait for the level intro (cutscenes, plane, HALO fall) to end
	if (not self.coopWorldReady) then
		if (math.floor(t.sec/10)*10 == t.sec) then
			local info = "no host";
			if (host) then
				local p = host:GetWorldPos(g_Vectors.temp_v1);
				local okF, flying = pcall(host.actor.IsFlying, host.actor);
				local okS, stance = pcall(host.actor.GetStance, host.actor);
				info = string.format("pos=(%.0f,%.0f,%.0f) speed=%s flying=%s swimming=%s settledTicks=%s",
					p.x, p.y, p.z, tostring(self.coopSpeed and self.coopSpeed[host.id]), tostring(okF and flying),
					tostring(okS and STANCE_SWIM ~= nil and stance == STANCE_SWIM),
					tostring(self.coopSettledTicks));
			end
			CoopLog("AUTOTEST t="..t.sec.." waiting for intro: "..info);
		end
		-- weapons/AI state can be checked without the host being anywhere near
		if (t.sec == 30 or t.sec == 90) then
			self:CoopAIReport(nil, 0);
			if (host) then
				local okH, hp = pcall(AI.GetAIObjectPosition, host.id);
				CoopLog("  host aiObject="..tostring(okH and hp ~= nil));
			end
		end
		-- same metric as the single-player probe (Mods\CoopProbe) for a 1:1 comparison
		if (math.floor(t.sec/15)*15 == t.sec) then
			self:CoopNearReport(host, 80);
		end
		if (t.sec >= 900) then
			CoopLog("AUTOTEST: host did not reach solid ground within 900s, giving up");
			self.coopAutotest = nil;
		end
		return;
	end

	-- phase 2: the old timeline, relative to the moment the world became ready
	if (not t.readyAt) then
		t.readyAt = t.sec;
		CoopLog("AUTOTEST: intro over after "..t.sec.."s");
	end
	local rel = t.sec - t.readyAt;
	if (rel == 2) then
		self:CoopAIReport(nil, 0);
	elseif (rel >= 5 and rel <= 65 and host and math.floor(rel/5)*5 == rel) then
		-- observe only: the report is centred on wherever the host is now
		local p = CopyVec(host:GetWorldPos(g_Vectors.temp_v1));
		System.ExecuteCommand("coop_aistats");
		CoopLog(string.format("AUTOTEST rel=%d host health=%s dead=%s pos=(%.0f,%.0f,%.0f)",
			rel, tostring(host.actor:GetHealth()), tostring(host:IsDead()), p.x, p.y, p.z));
		self:CoopAIReport(p, C.AUTOTEST_REPORT_RADIUS);
	elseif (rel == 66) then
		CoopLog("AUTOTEST done");
		self.coopAutotest = nil;
	end
end

function TeamInstantAction.Server:RequestTeleportToLeader(playerId)
	local player = System.GetEntity(playerId);
	if (not player or not player.actor) then
		return;
	end
	self.coopLastTeleport = self.coopLastTeleport or {};
	local last = self.coopLastTeleport[playerId];
	if (last and (_time - last) < C.TELEPORT_COOLDOWN) then
		CoopLog("coop_tp on cooldown for "..tostring(player:GetName()));
		return;
	end
	if ((not self:CoopGetLeader(playerId)) and (not self.coopAnchor)) then
		CoopLog("coop_tp: no leader/anchor");
		return;
	end
	self.coopLastTeleport[playerId] = _time;
	self:RevivePlayer(player.actor:GetChannel(), player, true);
end

function TeamInstantAction:CoopClientTeleport()
	if (g_localActorId and self.server and self.server.RequestTeleportToLeader) then
		self.server:RequestTeleportToLeader(g_localActorId);
	end
end

function TeamInstantAction:CoopPrintStatus()
	System.LogAlways("[Coop] version="..tostring(C.VERSION).." level="..tostring(self:CoopGetCurrentLevel())
		.." state="..tostring(self:GetState()));
	local players = self.game:GetPlayers();
	if (players) then
		for i,p in ipairs(players) do
			local pos = p:GetWorldPos(g_Vectors.temp_v1);
			System.LogAlways(string.format("[Coop]  %s alive=%s team=%s pos=(%.1f, %.1f, %.1f)",
				tostring(p:GetName()), tostring(self:CoopIsAlive(p)),
				tostring(self.game:GetTeam(p.id)), pos.x, pos.y, pos.z));
		end
	end
	if (self.coopAnchor) then
		System.LogAlways(string.format("[Coop]  anchor=(%.1f, %.1f, %.1f)",
			self.coopAnchor.pos.x, self.coopAnchor.pos.y, self.coopAnchor.pos.z));
	end
end

if (not g_CoopCommandsRegistered) then
	g_CoopCommandsRegistered = true;
	System.AddCCommand("coop_tp",
		"if (g_gameRules and g_gameRules.CoopClientTeleport) then g_gameRules:CoopClientTeleport(); end",
		"Crysis Coop: respawn next to the co-op leader");
	System.AddCCommand("coop_status",
		"if (g_gameRules and g_gameRules.CoopPrintStatus) then g_gameRules:CoopPrintStatus(); end",
		"Crysis Coop: print co-op state to the console/Game.log");
	System.AddCCommand("coop_ai_report",
		"if (g_gameRules and g_gameRules.CoopAIReport and g_localActor) then g_gameRules:CoopAIReport(g_localActor:GetWorldPos(), 50); end",
		"Crysis Coop: log AI state (weapons/alertness) within 50m");
end

--------------------------------------------------------------------------
-- 13. rebuild state tables so they pick up the overrides above, then
--     re-define the state-specific functions (DefaultState wipes them).
--------------------------------------------------------------------------
TeamInstantAction:DefaultState("Server", "Reset");
TeamInstantAction:DefaultState("Client", "Reset");
TeamInstantAction:DefaultState("Server", "PreGame");
TeamInstantAction:DefaultState("Client", "PreGame");
TeamInstantAction:DefaultState("Server", "InGame");
TeamInstantAction:DefaultState("Client", "InGame");
TeamInstantAction:DefaultState("Server", "PostGame");
TeamInstantAction:DefaultState("Client", "PostGame");

TeamInstantAction.Server.PostGame.OnChangeTeam = nil;
TeamInstantAction.Server.PostGame.OnSpectatorMode = nil;

function TeamInstantAction.Client.PreGame:OnBeginState()
	InstantAction.Client.PreGame.OnBeginState(self);
end

function TeamInstantAction.Server.PreGame:OnBeginState()
	self:ResetTime();
	self:StartTicking();
	self:ResetTeamScores();
	self.starting = false;
	self.warningTimer = 0;
end

function TeamInstantAction.Server.PreGame:OnUpdate(frameTime)
	TeamInstantAction.Server.InGame.OnUpdate(self, frameTime);
end

-- no countdown and NO RestartGame(): RestartGame calls game:ResetEntities(),
-- which would reset the single-player level scripting
function TeamInstantAction.Server.PreGame:OnTick()
	-- no countdown and NO RestartGame()/ResetEntities(): verified that it does
	-- not start the AI system in a network game, it only re-spawns entities
	CoopLog("PreGame -> InGame");
	self:GotoState("InGame");
end

function TeamInstantAction.Client.PreGame:OnTick()
end

function TeamInstantAction.Server.InGame:OnTick()
	InstantAction.Server.InGame.OnTick(self);
	self:CoopTick();
end

function TeamInstantAction.Server.InGame:OnBeginState()
	if (CoopInstallUseForwarding) then CoopInstallUseForwarding(); end
	self:CoopEnsureAIRunning();
	self:ResetTime();
	self:StartTicking();
	self:ResetPlayers();
	self:ResetTeamScores();

	CoopLog("InGame: map="..tostring(System.GetCVar("sv_map")).." sv_AISystem="..tostring(System.GetCVar("sv_AISystem"))
		.." syscfgMarker(sv_maxspectators)="..tostring(System.GetCVar("sv_maxspectators"))
		.." ai_SystemUpdate="..tostring(System.GetCVar("ai_SystemUpdate"))
		.." ai_NoUpdate="..tostring(System.GetCVar("ai_NoUpdate"))
		.." ai_UpdateProxy="..tostring(System.GetCVar("ai_UpdateProxy")));
	local sn = System.GetCVar("sv_servername");
	if (sn and string.find(tostring(sn), "AUTOTEST")) then
		self.coopAutotest = { sec = 0 };
		CoopLog("AUTOTEST armed");
	end
end

function TeamInstantAction.Client.InGame:OnBeginState()
	InstantAction.Client.InGame.OnBeginState(self);
	if (CoopInstallUseForwarding) then CoopInstallUseForwarding(); end
end

function TeamInstantAction.Server.InGame:OnUpdate(frameTime)
	TeamInstantAction.Server.OnUpdate(self, frameTime);
	self:CheckTimeLimit();
	self:CoopUpdateRevives();
end

function TeamInstantAction.Server.PostGame:OnBeginState()
	self:StartTicking();
	self:SetTimer(self.NEXTLEVEL_TIMERID, self.NEXTLEVEL_TIME);
end

function TeamInstantAction.Client.PostGame:OnBeginState()
	InstantAction.Client.PostGame.OnBeginState(self);
end

function TeamInstantAction.Client.PostGame:OnEndState()
	InstantAction.Client.PostGame.OnEndState(self);
end

--------------------------------------------------------------------------
-- 13b. vehicles after death (every level with vehicles: VTOL, tank, boats)
--------------------------------------------------------------------------
function TeamInstantAction:CoopTrackVehicles()
	local players = self.game:GetPlayers();
	if (not players) then
		return;
	end
	for i,p in ipairs(players) do
		if (p.actor) then
			local vid = p.actor:GetLinkedVehicleId();
			local v = vid and System.GetEntity(vid);
			if (v and v.vehicle and not v.vehicle:IsDestroyed() and self:CoopIsAlive(p)) then
				p.coopVehicleHist = p.coopVehicleHist or {};
				table.insert(p.coopVehicleHist, {
					id = vid, class = v.class, seat = v:GetSeatId(p.id) or 1,
					pos = CopyVec(v:GetWorldPos(g_Vectors.temp_v1)),
					ang = CopyVec(v:GetWorldAngles(g_Vectors.temp_v2)) });
				if (table.getn(p.coopVehicleHist) > 4) then
					table.remove(p.coopVehicleHist, 1);
				end
				-- a few seconds old: before the crash that is killing him
				p.coopVehicle = p.coopVehicleHist[1];
				p.coopInVehicle = true;
			elseif (self:CoopIsAlive(p)) then
				-- alive and on foot: he left the vehicle himself
				p.coopVehicle = nil;
				p.coopVehicleHist = nil;
				p.coopInVehicle = false;
			elseif (p.coopInVehicle) then
				p.coopDiedInVehicle = true;
				p.coopInVehicle = false;
			end
		end
	end
end

-- testing: coop_debug_killhost N kills the host N s after he got into a
-- vehicle (coop_debug_destroyveh 1: destroys the vehicle as well)
function TeamInstantAction:CoopDebugKillHost()
	local n = tonumber(System.GetCVar("coop_debug_killhost")) or 0;
	if (n <= 0 or self.coopDebugKilled or not g_localActorId) then
		return;
	end
	local host = System.GetEntity(g_localActorId);
	local vid = host and host.actor and host.actor:GetLinkedVehicleId();
	if (not vid) then
		self.coopDebugInVeh = nil;
		return;
	end
	self.coopDebugInVeh = (self.coopDebugInVeh or 0) + 1;
	if (self.coopDebugInVeh < n) then
		return;
	end
	self.coopDebugKilled = true;
	local v = System.GetEntity(vid);
	CoopLog("DEBUG: killing the host in "..tostring(v and v:GetName()));
	if ((tonumber(System.GetCVar("coop_debug_destroyveh")) or 0) == 1 and v and v.vehicle) then
		v.vehicle:OnHit(v.id, v.id, 100000, v:GetWorldPos(), 5, "normal", true);
	end
	self:KillPlayer(host);
end

-- testing: the host uses entity <coop_debug_use> after <coop_debug_use_at> s
-- (twice: press + release), game token <coop_debug_token> is logged
function TeamInstantAction:CoopDebugUse()
	self.coopDebugTicks = (self.coopDebugTicks or 0) + 1;
	local tok = System.GetCVar("coop_debug_token");
	if (tok and tok ~= "" and GameToken and math.floor(self.coopDebugTicks/5)*5 == self.coopDebugTicks) then
		local hh = g_localActorId and System.GetEntity(g_localActorId);
		CoopLog("DEBUG host spect="..tostring(hh and hh.actor:GetSpectatorMode()));
		CoopLog("DEBUG token "..tok.." = "..tostring(GameToken.GetToken(tok))
			..(self.coopDebugUseEntity and (" entity state="..tostring(self.coopDebugUseEntity:GetState())
			.." bUsable="..tostring(self.coopDebugUseEntity.bUsable).." cool="..tostring(self.coopDebugUseEntity.bCoolDown)
			.." delay="..tostring(self.coopDebugUseEntity.iDelayTimer)) or ""));
	end
	local mj = System.GetCVar("coop_debug_movejoiner");
	local at = tonumber(System.GetCVar("coop_debug_use_at")) or 30;
	if (mj and mj ~= "" and self.coopWorldReady) then
		self.coopDebugJoinTicks = (self.coopDebugJoinTicks or 0) + 1;
		local target = System.GetEntityByName(mj);
		if (target and self.coopDebugJoinTicks >= at - 5 and self.coopDebugJoinTicks <= at + 20) then
			local players = self.game:GetPlayers();
			for i,p in ipairs(players or {}) do
				if (p.id ~= g_localActorId and self:CoopIsAlive(p)) then
					local tp = target:GetWorldPos(g_Vectors.temp_v1);
					if (self.coopDebugJoinTicks == at - 5) then
						self.game:MovePlayer(p.id, { x = tp.x + 1, y = tp.y, z = tp.z + 0.5 }, p:GetWorldAngles(g_Vectors.temp_v2));
						CoopLog("DEBUG moved "..tostring(p:GetName()).." to "..mj);
					end
				end
			end
		end
	end
	local name = System.GetCVar("coop_debug_use");
	if (not name or name == "" or self.coopDebugUsed) then
		return;
	end
	if (self.coopDebugTicks < (tonumber(System.GetCVar("coop_debug_use_at")) or 30)) then
		return;
	end
	self.coopDebugUsed = true;
	local e = System.GetEntityByName(name);
	local host = g_localActorId and System.GetEntity(g_localActorId);
	if (not e or not host) then
		CoopLog("DEBUG use: entity "..tostring(name).." not found");
		return;
	end
	local usable = e.IsUsable and e:IsUsable(host);
	CoopLog("DEBUG use "..tostring(name).." state="..tostring(e.GetState and e:GetState()).." usable="..tostring(usable));
	host:SetWorldPos(e:GetWorldPos(g_Vectors.temp_v1));
	self.coopDebugUseEntity = e;
	CoopLog("DEBUG before: onUsed="..tostring(e.OnUsed ~= nil).." grab="..tostring(host.grabParams and host.grabParams.entityId)
		.." frozen="..tostring(host.actorStats and host.actorStats.isFrozen).." spect="..tostring(host.actor:GetSpectatorMode()));
	host:UseEntity(e.id, 0, true);
	host:UseEntity(e.id, 0, false);
	CoopLog("DEBUG used via player -> state="..tostring(e:GetState()).." cool="..tostring(e.bCoolDown));
	if (e:GetState() == "TurnedOn") then
		e:OnUsed(host, 0);
		CoopLog("DEBUG direct OnUsed -> state="..tostring(e:GetState()).." cool="..tostring(e.bCoolDown));
	end
end

function TeamInstantAction:CoopQueueVehicleReturn(player)
	self.coopVehicleReturns = self.coopVehicleReturns or {};
	self.coopVehicleReturns[player.id] = { rec = player.coopVehicle, t = _time + 0.5 };
end

local function CoopFreeSeat(v, preferred, playerId)
	if (not v or not v.Seats) then
		return nil;
	end
	local function free(seat)
		local pid = seat:GetPassengerId();
		if (pid == nil or pid == NULL_ENTITY or pid == playerId) then
			return true;
		end
		-- a dead body left in the seat does not count
		local p = System.GetEntity(pid);
		return (not p) or (p.actor and p.actor:GetHealth() <= 0);
	end
	if (preferred and v.Seats[preferred] and free(v.Seats[preferred])) then
		return preferred;
	end
	for i,seat in pairs(v.Seats) do
		if (free(seat)) then
			return i;
		end
	end
	return nil;
end

function TeamInstantAction:CoopSpawnVehicleCopy(class, pos, ang, player)
	self.coopVehicleCount = (self.coopVehicleCount or 0) + 1;
	local v = System.SpawnEntity({ class = class, name = class.."_coop_"..self.coopVehicleCount,
		position = { x = pos.x, y = pos.y, z = pos.z + 1 }, orientation = { x=0, y=1, z=0 } });
	if (v) then
		v:SetWorldAngles(ang);
		CoopLog("spawned an extra "..tostring(class).." for "..tostring(player:GetName()));
	end
	return v;
end

function TeamInstantAction:CoopPutIntoVehicle(player, v, seat)
	player:SetWorldPos(v:GetWorldPos(g_Vectors.temp_v1));
	local ok = v.vehicle:EnterVehicle(player.id, seat, false);
	-- a restored or newly spawned vehicle has its engine off: with the driver
	-- in place, "disable + enable" starts it (a VTOL would drop otherwise)
	if (seat == 1) then
		System.ExecuteCommand("coop_vehicle_drive "..tostring(v:GetName()));
	end
	CoopLog(string.format("%s put into %s seat %s (%s)", tostring(player:GetName()),
		tostring(v:GetName()), tostring(seat), tostring(ok)));
end

function TeamInstantAction:CoopProcessVehicleReturns()
	if (not self.coopVehicleReturns) then
		return;
	end
	for id, job in pairs(self.coopVehicleReturns) do
		if (_time >= job.t) then
			self.coopVehicleReturns[id] = nil;
			local player = System.GetEntity(id);
			if (player and self:CoopIsAlive(player)) then
				if (job.join) then
					local v = System.GetEntity(job.join);
					if (v and v.vehicle and not v.vehicle:IsDestroyed()) then
						local seat = CoopFreeSeat(v, nil, player.id);
						if (seat) then
							self:CoopPutIntoVehicle(player, v, seat);
						else
							-- all seats taken (story passengers): an own vehicle next to it
							local p = v:GetWorldPos(g_Vectors.temp_v1);
							local a = v:GetWorldAngles(g_Vectors.temp_v2);
							local c = self:CoopSpawnVehicleCopy(v.class, { x = p.x + 12, y = p.y + 12, z = p.z + 2 }, CopyVec(a), player);
							if (c and c.vehicle) then
								self:CoopPutIntoVehicle(player, c, 1);
							end
						end
					end
				elseif (job.rec) then
					local rec = job.rec;
					local v = System.GetEntity(rec.id);
					if (v and v.vehicle and v.vehicle:IsDestroyed()) then
						-- the level's own vehicle: restore it instead of replacing it,
						-- the story keeps referring to this very entity
						v:SetWorldPos(rec.pos);
						v:SetWorldAngles(rec.ang);
						System.ExecuteCommand("coop_vehicle_restore "..tostring(v:GetName()));
						v:SetWorldPos(rec.pos);
						v:SetWorldAngles(rec.ang);
					end
					if (v and v.vehicle and not v.vehicle:IsDestroyed()) then
						local seat = CoopFreeSeat(v, rec.seat, player.id);
						if (seat) then
							self:CoopPutIntoVehicle(player, v, seat);
						else
							local c = self:CoopSpawnVehicleCopy(rec.class, rec.pos, rec.ang, player);
							if (c and c.vehicle) then
								self:CoopPutIntoVehicle(player, c, 1);
							end
						end
					else
						local c = self:CoopSpawnVehicleCopy(rec.class, rec.pos, rec.ang, player);
						if (c and c.vehicle) then
							self:CoopPutIntoVehicle(player, c, rec.seat or 1);
						end
					end
				end
			end
		end
	end
end

--------------------------------------------------------------------------
-- 14. using level objects (every map, no per-level edits)
--  a) entity scripts of the campaign (InteractiveEntity, BasicEntity,
--     ExplosiveObject, PressurizedObject, ...) refuse to be used when
--     System.IsMultiplayer() is true; in the coop they behave as in the
--     campaign.
--  b) the story runs on the server: when a client player uses an object the
--     request is sent to the server, which performs the use with that
--     player there (level flowgraph, objectives and doors react as in SP).
--------------------------------------------------------------------------
if (System.IsMultiplayer and not g_CoopOrigIsMultiplayer) then
	g_CoopOrigIsMultiplayer = System.IsMultiplayer;
	System.IsMultiplayer = function()
		return false;
	end;
	CoopLog("System.IsMultiplayer reports single player to entity scripts");
end

function TeamInstantAction.Server:SvCoopUseEntity(playerId, entityName, slot, press)
	local player = System.GetEntity(playerId);
	-- the entity travels by name: its network id is not always resolvable
	local entity = entityName and System.GetEntityByName(entityName);
	CoopLog("use request: player="..tostring(player and player:GetName()).." entity="..tostring(entity and entity:GetName())
		.." press="..tostring(press).." orig="..tostring(g_CoopOrigUseEntity ~= nil));
	if (not g_CoopOrigUseEntity) then
		CoopInstallUseForwarding();
	end
	if (not player or not entity or not player.actor or not g_CoopOrigUseEntity) then
		return;
	end
	if (player.actor:GetHealth() <= 0) then
		CoopLog("use request ignored: player dead");
		return;
	end
	-- a use needs the player near the object
	local a, b = player:GetWorldPos(), entity:GetWorldPos();
	local dx, dy, dz = a.x-b.x, a.y-b.y, a.z-b.z;
	if (dx*dx + dy*dy + dz*dz > 12*12) then
		CoopLog("use request ignored: too far");
		return;
	end
	CoopLog(string.format("client %s uses %s (press=%s)", tostring(player:GetName()), tostring(entity:GetName()), tostring(press)));
	g_CoopOrigUseEntity(player, entity.id, slot, press);
end

function CoopInstallUseForwarding()
	if (not Player or not Player.UseEntity or g_CoopOrigUseEntity) then
		return;
	end
	g_CoopOrigUseEntity = Player.UseEntity;
	Player.UseEntity = function(self, entityId, slot, press)
		if (CryAction.IsServer() or not g_gameRules or not g_gameRules.server) then
			return g_CoopOrigUseEntity(self, entityId, slot, press);
		end
		local entity = entityId and System.GetEntity(entityId);
		-- items (pick up, mounted guns) keep their own network handling
		if (not entity or entity.item or (self.grabParams and self.grabParams.entityId)) then
			return g_CoopOrigUseEntity(self, entityId, slot, press);
		end
		g_gameRules.server:SvCoopUseEntity(self.id, entity:GetName(), slot or 0, press and true or false);
	end;
	CoopLog("object use is forwarded from clients to the server");
end
CoopInstallUseForwarding();

-- player.lua may be loaded after the gamerules: install again on revive
local stockClientOnRevive = TeamInstantAction.Client.OnRevive;
function TeamInstantAction.Client:OnRevive(playerId, pos, rot, teamId)
	CoopInstallUseForwarding();
	return stockClientOnRevive(self, playerId, pos, rot, teamId);
end

CoopLog("gamerules overrides applied");

--------------------------------------------------------------------------
-- 15. downed players and reviving them (coop_auto_respawn 0, the default)
--  A player who dies stays down. A teammate brings him back by holding the
--  use key next to him for REVIVE_TIME (Coop.dll's CoopRevive: the key, the
--  HUD, the network); he gets up where he fell. When everybody is down, the
--  team gets up where it stood at the last checkpoint (the world is not
--  loaded: nobody is disconnected, no loading screen). The downed also come
--  back at a checkpoint the others reach, so a body nobody can get to (the
--  sea, a chasm) is not a dead end.
--------------------------------------------------------------------------
function TeamInstantAction:CoopReviveMode()
	return (tonumber(System.GetCVar("coop_auto_respawn")) or 0) == 0;
end

local function CoopDistance(a, b)
	local p = CopyVec(a:GetWorldPos(g_Vectors.temp_v1));
	local q = b:GetWorldPos(g_Vectors.temp_v2);
	local dx, dy, dz = p.x - q.x, p.y - q.y, p.z - q.z;
	return math.sqrt(dx*dx + dy*dy + dz*dz);
end

-- a player in the game: not a spectator (a joiner waiting for the intro)
local function CoopInGame(p)
	return p and p.actor and p.actor:GetSpectatorMode() == 0;
end

-- a dead player's click does not bring him back here
local stockRequestRevive = TeamInstantAction.Server.RequestRevive;
function TeamInstantAction.Server:RequestRevive(entityId)
	if (self:CoopReviveMode()) then
		return;
	end
	if (stockRequestRevive) then
		stockRequestRevive(self, entityId);
	end
end

-- Coop.dll: a player's use key on a downed teammate, pressed or let go
function TeamInstantAction:CoopReviveInput(reviverId, targetId, press)
	if (not self:CoopReviveMode()) then
		return;
	end
	self.coopRevives = self.coopRevives or {};
	local reviver, target = System.GetEntity(reviverId), System.GetEntity(targetId);
	if (not reviver or not target) then
		return;
	end
	local running = self.coopRevives[target.id];
	if (not press) then
		if (running and running.by == reviver.id) then
			self.coopRevives[target.id] = nil;
			HUD.CoopReviveState(2, target.id, reviver.id, 0);
		end
		return;
	end
	if (running or not CoopInGame(reviver) or not CoopInGame(target) or reviver:IsDead() or not target:IsDead()) then
		return;
	end
	if (CoopDistance(reviver, target) > C.REVIVE_RANGE) then
		return;
	end
	self.coopRevives[target.id] = { by = reviver.id, t0 = _time };
	CoopLog(tostring(reviver:GetName()).." revives "..tostring(target:GetName()));
	HUD.CoopReviveState(1, target.id, reviver.id, C.REVIVE_TIME);
end

-- every frame: a revive goes on while the reviver stays next to him
function TeamInstantAction:CoopUpdateRevives()
	if (not self.coopRevives) then
		return;
	end
	for targetId, r in pairs(self.coopRevives) do
		local target, reviver = System.GetEntity(targetId), System.GetEntity(r.by);
		if (not target or not reviver or not CoopInGame(target) or not target:IsDead() or reviver:IsDead()
			or CoopDistance(reviver, target) > C.REVIVE_RANGE + 1) then
			self.coopRevives[targetId] = nil;
			HUD.CoopReviveState(2, targetId, r.by, 0);
		elseif (_time - r.t0 >= C.REVIVE_TIME) then
			self.coopRevives[targetId] = nil;
			-- up where the body lies, looking the way the teammate does. A body
			-- sinks a little into rock: a player put back 0.3 m above it fell
			-- through the cliff, so a metre (and CoopWatchFall)
			local pos = CopyVec(target:GetWorldPos(g_Vectors.temp_v1));
			pos.z = pos.z + 1.0;
			local ang = CopyVec(reviver:GetWorldAngles(g_Vectors.temp_v2));
			if (self:RevivePlayer(target.actor:GetChannel(), target, false, pos, ang)) then
				self:CoopWatchFall(target, pos, reviver);
				target.actor:SetHealth(math.max(1, target.actor:GetMaxHealth() * C.REVIVE_HEALTH));
				CoopLog(tostring(reviver:GetName()).." revived "..tostring(target:GetName()));
				self.game:SendTextMessage(TextMessageInfo, tostring(reviver:GetName()).." revived "..tostring(target:GetName()), TextMessageToAll);
			end
			HUD.CoopReviveState(3, targetId, r.by, 0);
		end
	end
end

-- every second: who went down, and everybody down -> the last checkpoint
function TeamInstantAction:CoopTickDowned()
	self.coopDownSeen = self.coopDownSeen or {};
	local alive, down = 0, 0;
	for i,p in ipairs(self.game:GetPlayers() or {}) do
		if (CoopInGame(p)) then
			if (p:IsDead()) then
				down = down + 1;
				if (not self.coopDownSeen[p.id]) then
					self.coopDownSeen[p.id] = true;
					CoopLog(tostring(p:GetName()).." is down");
					self.game:SendTextMessage(TextMessageInfo, tostring(p:GetName()).." is down", TextMessageToAll);
				end
			else
				alive = alive + 1;
				self.coopDownSeen[p.id] = nil;
			end
		end
	end
	if (down == 0 or alive > 0) then
		if (self.coopAllDownAt) then
			self.coopAllDownAt = nil;
			HUD.CoopReviveState(5, NULL_ENTITY, NULL_ENTITY, 0);
		end
		return;
	end
	if (not self.coopAllDownAt) then
		self.coopAllDownAt = _time + C.ALL_DOWN_DELAY;
		CoopLog("everybody is down: back to the last checkpoint in "..C.ALL_DOWN_DELAY.." s");
		HUD.CoopReviveState(4, NULL_ENTITY, NULL_ENTITY, C.ALL_DOWN_DELAY);
	elseif (_time >= self.coopAllDownAt) then
		self.coopAllDownAt = nil;
		self:CoopBackToCheckpoint();
	end
end

-- Coop.dll: a checkpoint was reached (a story one, "manual" = Save now) or
-- the progress was loaded ("loaded"). Everybody's place is kept: when the
-- whole team is down, it comes back there (CoopBackToCheckpoint). At a
-- checkpoint of the story the downed come back next to the others.
function TeamInstantAction:CoopOnCheckpoint(name)
	-- the places before a load are not in the world that was loaded
	if (name == "loaded" or not self.coopSpots) then
		self.coopSpots = {};
	end
	local kept = self:CoopKeepSpots();
	CoopLog("checkpoint "..tostring(name)..": "..kept.." players' places kept");
	-- nobody in the world yet (the level's first checkpoint comes before the
	-- host's intro): the places are kept once somebody is (CoopTick)
	self.coopSpotsPending = (kept == 0) and name or nil;
	if (name ~= "manual" and name ~= "loaded" and self:CoopReviveMode()) then
		self:CoopReviveAllDowned("checkpoint");
	end
end

-- the place of every player standing in the world: not in a vehicle seat
-- (the vehicle goes on without him), not lying on the ground, not in an
-- intro or a cutscene
function TeamInstantAction:CoopKeepSpots(quiet)
	local kept = 0;
	for i,p in ipairs(self.game:GetPlayers() or {}) do
		-- "flying" is not trusted here: the server's physics says it of a
		-- friend who stands (the friend's machine moves the player); one really in
		-- the air has no ground below ("in the air")
		local why = CoopInGame(p) and not p:IsDead() and self:CoopNotInWorld(p);
		-- ... but one who falls is "flying" too
		if (why == "flying" and HUD and HUD.CoopIsAirborne and HUD.CoopIsAirborne(p.id)) then
			why = "in the air";
		end
		if (CoopInGame(p) and not p:IsDead() and not p.actor:GetLinkedVehicleId() and (not why or why == "flying")) then
			self.coopSpots[p:GetName()] = {
				pos = CopyVec(p:GetWorldPos(g_Vectors.temp_v1)),
				ang = CopyVec(p:GetWorldAngles(g_Vectors.temp_v2)),
				host = (p.id == g_localActorId),
			};
			kept = kept + 1;
		elseif (not quiet and CoopInGame(p) and not p:IsDead()) then
			CoopLog("place of "..tostring(p:GetName()).." not kept: "..(p.actor:GetLinkedVehicleId() and "in a vehicle" or tostring(why)));
		end
	end
	return kept;
end

-- A player put back on their feet (revived, back at the checkpoint) who
-- falls through the ground goes next to the teammate who revived them, or
-- back to the same place higher up; watched for a few seconds (CoopTick)
function TeamInstantAction:CoopWatchFall(p, pos, reviver)
	self.coopFallWatch = self.coopFallWatch or {};
	self.coopFallWatch[p.id] = { pos = CopyVec(pos), by = reviver and reviver.id, t = _time, tries = 0 };
end

function TeamInstantAction:CoopTickFallWatch()
	if (not self.coopFallWatch) then
		return;
	end
	for id, w in pairs(self.coopFallWatch) do
		local p = System.GetEntity(id);
		if (not p or not p.actor or p:IsDead() or _time - w.t > 8) then
			self.coopFallWatch[id] = nil;
		else
			local now = p:GetWorldPos(g_Vectors.temp_v1);
			if (now.z < w.pos.z - 8) then
				local by = w.by and System.GetEntity(w.by);
				local to, ang;
				if (by and by.actor and not by:IsDead()) then
					to = CopyVec(by:GetWorldPos(g_Vectors.temp_v2));
					ang = CopyVec(by:GetWorldAngles(g_Vectors.temp_v2));
					to.z = to.z + 0.5;
				else
					to = CopyVec(w.pos);
					to.z = to.z + 2 + 2 * w.tries;
					ang = CopyVec(p:GetWorldAngles(g_Vectors.temp_v2));
				end
				CoopLog(tostring(p:GetName()).." fell through the ground: put back "..(by and ("next to "..tostring(by:GetName())) or "higher up"));
				self.game:MovePlayer(id, to, ang);
				w.pos = CopyVec(to);
				w.t = _time;
				w.tries = w.tries + 1;
				if (w.tries > 3) then
					self.coopFallWatch[id] = nil;
				end
			end
		end
	end
end

-- a player's place at the last checkpoint; one who has none (joined or
-- lay down since) stands next to the host's, or anybody's
function TeamInstantAction:CoopSpotOf(p, n)
	local spots = self.coopSpots or {};
	local own = spots[p:GetName()];
	if (own) then
		return CopyVec(own.pos), CopyVec(own.ang);
	end
	local base;
	for name, s in pairs(spots) do
		if (s.host or not base) then
			base = s;
		end
	end
	if (not base) then
		return nil;
	end
	local pos, ang = CopyVec(base.pos), CopyVec(base.ang);
	-- side by side, 1.5 m apart, to the right of where the host looks
	local right = { x = math.cos(ang.z), y = math.sin(ang.z), z = 0 };
	pos.x = pos.x + right.x * 1.5 * n;
	pos.y = pos.y + right.y * 1.5 * n;
	return pos, ang;
end

-- everybody is down: the whole team comes back where it was at the last
-- checkpoint, with what it carries now; nothing is loaded, so nobody is
-- disconnected and nobody sees a loading screen (the explicit "Back to the
-- last checkpoint" of the menu still loads it)
function TeamInstantAction:CoopBackToCheckpoint()
	local list, others = {}, 0;
	for i,p in ipairs(self.game:GetPlayers() or {}) do
		if (CoopInGame(p) and p:IsDead()) then
			if (p.id == g_localActorId) then
				table.insert(list, 1, p);
			else
				table.insert(list, p);
			end
		end
	end
	if (not self.coopSpots or not next(self.coopSpots)) then
		CoopLog("everybody is down, no checkpoint place kept: everybody comes back");
		self:CoopReviveAllDowned("no checkpoint");
		HUD.CoopReviveState(6, NULL_ENTITY, NULL_ENTITY, C.BACK_FADE_TIME);
		return;
	end
	for i,p in ipairs(list) do
		if (self.coopRevives) then
			self.coopRevives[p.id] = nil;
		end
		local own = self.coopSpots[p:GetName()] ~= nil;
		local pos, ang = self:CoopSpotOf(p, own and 0 or i);
		if (pos) then
			pos.z = pos.z + 0.2;
		end
		if (self:RevivePlayer(p.actor:GetChannel(), p, false, pos, ang)) then
			if (pos) then
				self:CoopWatchFall(p, pos, nil);
			end
			CoopLog(tostring(p:GetName()).." is back at the checkpoint"..(own and "" or " (next to the others)"));
		end
	end
	HUD.CoopReviveState(6, NULL_ENTITY, NULL_ENTITY, C.BACK_FADE_TIME);
	self.game:SendTextMessage(TextMessageInfo, "Back to the last checkpoint", TextMessageToAll);
end

-- the downed come back next to the others (a checkpoint of the story was
-- reached); the host first, the friends then join him
function TeamInstantAction:CoopReviveAllDowned(reason)
	local list = {};
	for i,p in ipairs(self.game:GetPlayers() or {}) do
		if (CoopInGame(p) and p:IsDead()) then
			if (p.id == g_localActorId) then
				table.insert(list, 1, p);
			else
				table.insert(list, p);
			end
		end
	end
	for i,p in ipairs(list) do
		CoopLog(tostring(p:GetName()).." comes back ("..tostring(reason)..")");
		if (self.coopRevives) then
			self.coopRevives[p.id] = nil;
		end
		self:RevivePlayer(p.actor:GetChannel(), p);
	end
end
