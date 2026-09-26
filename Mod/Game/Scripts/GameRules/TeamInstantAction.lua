-- [Crysis Coop] Stock TeamInstantAction.lua with two edits: RequestTeleportToLeader added to
-- Net.Expose ServerMethods, and scripts/coop/coopgamerules.lua loaded at the end of the file.
-- Everything else is byte-identical to the stock GameData.pak version.
--------------------------------------------------------------------------
--	Crytek Source File.
-- 	Copyright (C), Crytek Studios, 2001-2004.
--------------------------------------------------------------------------
--	$Id$
--	$DateTime$
--	Description: GameRules implementation for Death Match
--  
--------------------------------------------------------------------------
--  History:
--  - 22/ 9/2004   16:20 : Created by Mathieu Pinard
--  - 04/10/2004   10:43 : Modified by Craig Tiller
--  - 07/10/2004   16:02 : Modified by Marcio Martins
--
----------------------------------------------------------------------------------------------------
Script.LoadScript("scripts/gamerules/instantaction.lua", 1, 1);
--------------------------------------------------------------------------
TeamInstantAction = new(InstantAction);
TeamInstantAction.States = { "Reset", "PreGame", "InGame", "PostGame", };


TeamInstantAction.MIN_TEAM_LIMIT_WARN_TIMER	= 15; -- team limit warning timer
TeamInstantAction.START_TIMER								= 30; -- starting countdown timer

TeamInstantAction.TEAM_SPAWN_LOCATIONS			= true;
TeamInstantAction.NEUTRAL_SPAWN_LOCATIONS	= true;

TeamInstantAction.TEAMSCORE_TEAM0_KEY 			= 10;
TeamInstantAction.TEAMSCORE_TEAM1_KEY 			= 11;
TeamInstantAction.TEAMSCORE_TEAM2_KEY 			= 12;

----------------------------------------------------------------------------------------------------
TeamInstantAction.teamName={ "tan", "black" };
TeamInstantAction.teamModel=
{
	black	={
						{
							"objects/characters/human/us/nanosuit/nanosuit_us_multiplayer.cdf",
							"objects/weapons/arms_global/arms_nanosuit_us.chr",
							"objects/characters/human/asian/nk_soldier/nk_soldier_frozen_scatter.cgf",
							"objects/characters/human/us/nanosuit/nanosuit_us_fp3p.cdf",
						},
					},
					
	tan		={
						{
							"objects/characters/human/asian/nanosuit/nanosuit_asian_multiplayer.cdf",
							"objects/weapons/arms_global/arms_nanosuit_asian.chr",
							"objects/characters/human/asian/nk_soldier/nk_soldier_frozen_scatter.cgf",
							"objects/characters/human/asian/nanosuit/nanosuit_asian_fp3p.cdf",
						},
					},
}


----------------------------------------------------------------------------------------------------
--maximum 4 groups, 5 messages in each (F5-F8, then 1-5). Change Radio.cpp if more needed.
TeamInstantAction.teamRadio=
{
	black = 
	{
		[1]=
		{
			{"mp_american/us_F5_1_10-4","@mp_radio_Yes",3},
			{"mp_american/us_F5_2_negative","@mp_radio_No",3},
			{"mp_american/us_F5_3_wait","@mp_radio_Wait",3},
			{"mp_american/us_F5_4_follow_me","@mp_radio_FollowMe",3},
			{"mp_american/us_F5_6_thank_you","@mp_radio_Thanks",3},
		},
		[2]=
		{
			{"mp_american/us_F6_1_attack_enemy_base","@mp_radio_TakeBase"},
			{"mp_american/us_F6_2_gather_power_cores","@mp_radio_GatherPower"},
			{"mp_american/us_F6_3_take_prototype_factory","@mp_radio_TakePT"},
			{"mp_american/us_F6_4_take_war_factory","@mp_radio_TakeWar"},
			{"mp_american/us_F6_5_take_airfield","@mp_radio_TakeAir"},
		},
		[3]=
		{
			{"mp_american/us_F7_1_armor_spotted","@mp_radio_ArmorSpotted"},
			{"mp_american/us_F7_2_aircraft_spotted","@mp_radio_AircraftSpotted"},
			{"mp_american/us_F7_3_boat_spotted","@mp_radio_BoatSpotted"},
			{"mp_american/us_F7_4_vehicle_spotted","@mp_radio_LTVSpotted"},
			{"mp_american/us_F7_5_infantry_spotted","@mp_radio_InfantrySpotted"},
		},
		[4]=
		{
			{"mp_american/us_F8_1_request_assistance","@mp_radio_Assistance"},
			{"mp_american/us_F8_2_get_into_vehicle","@mp_radio_GetIn"},
			{"mp_american/us_F8_3_get_out_vehicle","@mp_radio_GetOut"},
			{"mp_american/us_F8_4_mechanical_assistance_needed","@mp_radio_MechAssistance"},
			{"mp_american/us_F8_5_radar_scan","@mp_radio_Radar"},
		},
	},
	tan = 
	{
		[1]=
		{
			{"mp_korean/nk_F5_1_10-4","@mp_radio_Yes",3},
			{"mp_korean/nk_F5_2_negative","@mp_radio_No",3},
			{"mp_korean/nk_F5_3_wait","@mp_radio_Wait",3},
			{"mp_korean/nk_F5_4_follow_me","@mp_radio_FollowMe",3},
			{"mp_korean/nk_F5_6_thank_you","@mp_radio_Thanks",3},
		},
		[2]=
		{
			{"mp_korean/nk_F6_1_attack_enemy_base","@mp_radio_TakeBase"},
			{"mp_korean/nk_F6_2_gather_power_cores","@mp_radio_GatherPower"},
			{"mp_korean/nk_F6_3_take_prototype_factory","@mp_radio_TakePT"},
			{"mp_korean/nk_F6_4_take_war_factory","@mp_radio_TakeWar"},
			{"mp_korean/nk_F6_5_take_airfield","@mp_radio_TakeAir"},
		},
		[3]=
		{
			{"mp_korean/nk_F7_1_armor_spotted","@mp_radio_ArmorSpotted"},
			{"mp_korean/nk_F7_2_aircraft_spotted","@mp_radio_AircraftSpotted"},
			{"mp_korean/nk_F7_3_boat_spotted","@mp_radio_BoatSpotted"},
			{"mp_korean/nk_F7_4_vehicle_spotted","@mp_radio_LTVSpotted"},
			{"mp_korean/nk_F7_5_infantry_spotted","@mp_radio_InfantrySpotted"},
		},
		[4]=
		{
			{"mp_korean/nk_F8_1_request_assistance","@mp_radio_Assistance"},
			{"mp_korean/nk_F8_2_get_into_vehicle","@mp_radio_GetIn"},
			{"mp_korean/nk_F8_3_get_out_vehicle","@mp_radio_GetOut"},
			{"mp_korean/nk_F8_4_mechanical_assistance_needed","@mp_radio_MechAssistance"},
			{"mp_korean/nk_F8_5_radar_scan","@mp_radio_Radar"},
		},
	}
}


----------------------------------------------------------------------------------------------------
----------------------------------------------------------------------------------------------------
Net.Expose {
	Class = TeamInstantAction,
	ClientMethods = {
		ClSetupPlayer					= { RELIABLE_UNORDERED, POST_ATTACH, ENTITYID, },
		ClSetSpawnGroup	 			= { RELIABLE_UNORDERED, POST_ATTACH, ENTITYID, },
		ClSetPlayerSpawnGroup	= { RELIABLE_UNORDERED, POST_ATTACH, ENTITYID, ENTITYID },
		ClSpawnGroupInvalid		= { RELIABLE_UNORDERED, POST_ATTACH, ENTITYID, },
		ClVictory							= { RELIABLE_ORDERED, POST_ATTACH, INT8, INT8 },
		
		ClStartWorking				= { RELIABLE_ORDERED, POST_ATTACH, ENTITYID; STRINGTABLE },
		ClStepWorking					= { RELIABLE_ORDERED, POST_ATTACH, INT8 },
		ClStopWorking					= { RELIABLE_ORDERED, POST_ATTACH, ENTITYID, BOOL },
		ClWorkComplete				= { RELIABLE_ORDERED, POST_ATTACH, ENTITYID, STRINGTABLE },
		
		ClClientConnect				= { RELIABLE_UNORDERED, POST_ATTACH, STRING, BOOL },
		ClClientDisconnect		= { RELIABLE_UNORDERED, POST_ATTACH, STRING, },
		ClClientEnteredGame		= { RELIABLE_UNORDERED, POST_ATTACH, STRING, },	
	},
	ServerMethods = {
		RequestRevive		 = { RELIABLE_UNORDERED, POST_ATTACH, ENTITYID, },
		RequestSpawnGroup			= { RELIABLE_UNORDERED, POST_ATTACH, ENTITYID, ENTITYID },
		RequestTeleportToLeader	= { RELIABLE_UNORDERED, POST_ATTACH, ENTITYID, },
		SvCoopUseEntity		= { RELIABLE_ORDERED, POST_ATTACH, ENTITYID, STRING, INT16, BOOL, },
	},
	ServerProperties = {
	},
};
----------------------------------------------------------------------------------------------------


----------------------------------------------------------------------------------------------------
function TeamInstantAction:ProcessScores(hit)
	local target=hit.target;
	local shooter=hit.shooter;
	local headshot=self:IsHeadShot(hit);

	local h=0;
	if (headshot) then
		h=1;
	end

	if (target.actor and target.actor:IsPlayer()) then
		self:Award(target, 1, 0, 0);
	end
	
	if (shooter.actor and shooter.actor:IsPlayer()) then
		if (target ~= shooter) then
			local team1=self.game:GetTeam(shooter.id);
			local team2=self.game:GetTeam(target.id);
	
			if (team1~=team2) then
				self:Award(shooter, 0, 1, h);
				
				-- update team score
				self:SetTeamScore(team1, self:GetTeamScore(team1)+1);
			else
				self:Award(shooter, 0, -1, 0);
			end
		else
			self:Award(shooter, 0, -1, 0);
		end
	end
end


----------------------------------------------------------------------------------------------------
function TeamInstantAction:CheckPlayerScoreLimit(playerId, score)
end


----------------------------------------------------------------------------------------------------
function TeamInstantAction:ResetTeamScores()
	for i,teamId in pairs(self.teamId) do
		self.game:SetSynchedGlobalValue(self.TEAMSCORE_TEAM0_KEY+teamId, 0);
	end
end


----------------------------------------------------------------------------------------------------
function TeamInstantAction:SetTeamScore(teamId, score)
	self.game:SetSynchedGlobalValue(self.TEAMSCORE_TEAM0_KEY+teamId, score);

	if (self:GetState() and self:GetState()=="InGame") then
		self:CheckScoreLimit(teamId, score);
	end
end


----------------------------------------------------------------------------------------------------
function TeamInstantAction:GetTeamScore(teamId)
	return self.game:GetSynchedGlobalValue(self.TEAMSCORE_TEAM0_KEY+teamId) or 0;
end


----------------------------------------------------------------------------------------------------
function TeamInstantAction:CheckScoreLimit(teamId, score)
	local state=self:GetState();
	if (state and state~="InGame") then
		return;
	end

	local fraglimit=self.game:GetFragLimit();
	local fraglead=self.game:GetFragLead();
	
	if ((fraglimit > 0) and (score >= fraglimit)) then
		if ((fraglead > 1) and self.teamId) then
			for i,id in pairs(self.teamId) do
				if (self:GetTeamScore(id)+fraglead > score) then
					return;
				end
			end
		end
		
		self:OnGameEnd(teamId, 3);
	end
end


----------------------------------------------------------------------------------------------------
function TeamInstantAction:CheckTimeLimit()	
	if (self.game:IsTimeLimited() and self.game:GetRemainingGameTime()<=0) then
		local state=self:GetState();
		if (state and state~="InGame") then
			return;
		end

		local maxScore=0;
		local maxId=0;
		local draw=false;

		for i,id in pairs(self.teamId) do
			local score=self:GetTeamScore(id);
			if (score>=maxScore) then
				if ((maxId~=0) and (maxScore==score)) then
					draw=true;
				else
					draw=false;
					maxId=id;
					maxScore=score;
				end
			end
		end

		if (not draw) then
			self:OnGameEnd(maxId, 2);
		else
			self:OnGameEnd(nil, 2);
		end
	end
end


----------------------------------------------------------------------------------------------------
function TeamInstantAction:PlayerCountOk()
	local preGame=self:GetState()=="PreGame";
	
	for i,teamName in ipairs(self.teamName) do
		local teamId = self.game:GetTeamId(teamName);
		if (self.game:GetTeamChannelCount(teamId, preGame) < self.game:GetMinTeamLimit()) then
			return false;
		end
	end
	
	return true;
end


----------------------------------------------------------------------------------------------------
function TeamInstantAction:OnGameEnd(winningTeamId, type)
	if (winningTeamId and winningTeamId~=0) then
		local teamName=self.game:GetTeamName(winningTeamId);
		self.game:SendTextMessage(TextMessageCenter, "@mp_GameOverWinner", TextMessageToAll, nil, "@mp_team_"..teamName);
	else
		self.game:SendTextMessage(TextMessageCenter, "@mp_GameOverNoWinner", TextMessageToAll);
	end
	
	self.allClients:ClVictory(winningTeamId or 0, type or 0);
	
	self.game:EndGame();

	self:GotoState("PostGame");	
end


----------------------------------------------------------------------------------------------------
function TeamInstantAction.Server:OnInit()
	self.teamId={};
	for i,v in ipairs(self.teamName) do
		table.insert(self.teamId, self.game:CreateTeam(v));
	end
	
	InstantAction.Server.OnInit(self);
end


----------------------------------------------------------------------------------------------------
function TeamInstantAction.Client:OnInit()
	InstantAction.Client.OnInit(self);

	-- if we are a server, it will be done by the server-side
	if (not self.isServer) then
		self.teamId={};
		for i,v in ipairs(self.teamName) do
			table.insert(self.teamId, self.game:CreateTeam(v));
		end
	end
end

----------------------------------------------------------------------------------------------------
function TeamInstantAction.Server:OnClientConnect(channelId, reset, name)
	local player = InstantAction.Server.OnClientConnect(self, channelId, reset, name);

	return player;
end

----------------------------------------------------------------------------------------------------
function TeamInstantAction.Server:OnClientEnteredGame(channelId, player, reset)
	InstantAction.Server.OnClientEnteredGame(self, channelId, player, reset);
end

----------------------------------------------------------------------------------------------------
function TeamInstantAction.Server:OnChangeTeam(playerId, teamId)
	if (teamId ~= self.game:GetTeam(playerId)) then
		local player=System.GetEntity(playerId);
		if (player) then
			if (player.actor:GetHealth()>0 and player.actor:GetSpectatorMode()==0) then
				self:KillPlayer(player);
			end

			self.game:SetTeam(teamId, playerId);
			
			self.game:ChangeSpectatorMode(playerId, 0, NULL_ENTITY);
			self:RevivePlayer(player.actor:GetChannel(), player);
		end
	end
end


----------------------------------------------------------------------------------------------------
function TeamInstantAction.Client:OnRevive(playerId, pos, rot, teamId)
	local teamId = teamId;
	local player = System.GetEntity(playerId);
	
	if (player and teamId and teamId~=0) then
		local teamName=self.game:GetTeamName(teamId);
		local models=self.teamModel[teamName];
		if (models and table.getn(models)>0) then
			local model=models[1];
			player:SetModel(model[1], model[2], model[3], model[4]);
		end
	end
end


----------------------------------------------------------------------------------------------------
function TeamInstantAction.Client:OnReviveInVehicle(playerId, vehicleId, seatId, teamId)
	local teamId = teamId;
	local player = System.GetEntity(playerId);
	
	if (player and teamId and teamId~=0) then
		local teamName=self.game:GetTeamName(teamId);
		local models=self.teamModel[teamName];
		if (models and table.getn(models)>0) then
			local model=models[1];
			player:SetModel(model[1], model[2], model[3], model[4]);
		end
	end
end


----------------------------------------------------------------------------------------------------
function TeamInstantAction:AutoAssignTeam(player, forceTeamId)
	if (forcedTeamId and forcedTeamId~=0) then
		self.game:SetTeam(forcedTeamId, player.id);
	else
		teamId=self.teamId[1];
		local teamIdCount=self.game:GetTeamChannelCount(teamId);
		for i,v in pairs(self.teamId) do
			if (teamId ~= v) then
				local c1=self.game:GetTeamChannelCount(v);
				if (c1<teamIdCount) then
					teamId=v;
					teamIdCount=c1;
				end
			end
		end
		
		if (teamId and teamId~=0) then
			self.game:SetTeam(teamId, player.id);
		end
	end
end


----------------------------------------------------------------------------------------------------
function TeamInstantAction.Server:OnHit(hit)
	local shooter = hit.shooter;
	local target = hit.target;
	if (shooter and target and shooter.actor and shooter.actor:IsPlayer()) then
		local team1=self.game:GetTeam(shooter.id);
		local team2=self.game:GetTeam(target.id);
		if(team1 == team2 and team1~=0 and shooter.id~=target.id and (hit.type~="repair")) then
			hit.damage = hit.damage*self.game:GetFriendlyFireRatio();
		end
	end
	return InstantAction.Server.OnHit(self, hit);
end


----------------------------------------------------------------------------------------------------
function TeamInstantAction:CalcExplosionDamage(entity, explosion, obstruction)
	local dmg=InstantAction.CalcExplosionDamage(self, entity, explosion, obstruction);
	
	local shooter = explosion.shooter;
	if (shooter and entity and shooter.actor and shooter.actor:IsPlayer()) then
		local team1=self.game:GetTeam(shooter.id);
		local team2=self.game:GetTeam(entity.id);
		if(team1 == team2 and team1~=0 and shooter.id~=entity.id) then
			dmg = dmg*self.game:GetFriendlyFireRatio();
		end
	end
	
	return dmg;	
end


----------------------------------------------------------------------------------------------------
function TeamInstantAction.Server:OnFreeze(targetId, shooterId, weaponId, value)
	local target=System.GetEntity(targetId);
	if ((self.game:GetFriendlyFireRatio()>0) or (self.game:GetTeam(targetId)~=self.game:GetTeam(shooterId))) then
		if (target.OnFreeze and not target:OnFreeze(shooterId, weaponId, value)) then
			return false;
		end
		
		if (target.actor or target.vehicle) then
			target.frostShooterId=shooterId;
		end
		return true;
	end

	return false;
end


----------------------------------------------------------------------------------------------------
function TeamInstantAction:DefaultState(cs, state)
	local default=self[cs];
	self[cs][state]={
		OnClientConnect = default.OnClientConnect,
		OnClientDisconnect = default.OnClientDisconnect,
		OnClientEnteredGame = default.OnClientEnteredGame,
		OnDisconnect = default.OnDisconnect, -- client only
		OnActorAction = default.OnActorAction, -- client only
		OnStartLevel = default.OnStartLevel,
		OnStartGame = default.OnStartGame,

		OnKill = default.OnKill,
		OnHit = default.OnHit,
		OnFreeze = default.OnFreeze,
		OnExplosion = default.OnExplosion,
		OnChangeTeam = default.OnChangeTeam,
		OnChangeSpectatorMode = default.OnChangeSpectatorMode,
		OnSetTeam = default.OnSetTeam,
		OnItemPickedUp = default.OnItemPickedUp,
		OnItemDropped = default.OnItemDropped,

		OnTimer = default.OnTimer,
		OnUpdate = default.OnUpdate,	
	}
end

----------------------------------------------------------------------------------------------------
TeamInstantAction:DefaultState("Server", "Reset");
TeamInstantAction:DefaultState("Client", "Reset");

----------------------------------------------------------------------------------------------------
TeamInstantAction:DefaultState("Server", "PreGame");
TeamInstantAction:DefaultState("Client", "PreGame");

----------------------------------------------------------------------------------------------------
TeamInstantAction:DefaultState("Server", "InGame");
TeamInstantAction:DefaultState("Client", "InGame");

----------------------------------------------------------------------------------------------------
TeamInstantAction:DefaultState("Server", "PostGame");
TeamInstantAction:DefaultState("Client", "PostGame");

----------------------------------------------------------------------------------------------------
TeamInstantAction.Server.PostGame.OnChangeTeam = nil;
TeamInstantAction.Server.PostGame.OnSpectatorMode = nil;


----------------------------------------------------------------------------------------------------
function TeamInstantAction.Client.PreGame:OnBeginState()
	InstantAction.Client.PreGame.OnBeginState(self);
end

----------------------------------------------------------------------------------------------------
function TeamInstantAction.Server.PreGame:OnBeginState()
	self:ResetTime();	
	self:StartTicking();
	self:ResetPlayers();
	self:ResetTeamScores();
		
	self.starting=false;
	self.warningTimer=0;
end


----------------------------------------------------------------------------------------------------
function TeamInstantAction.Server.PreGame:OnUpdate(frameTime)
	TeamInstantAction.Server.InGame.OnUpdate(self, frameTime);
end


----------------------------------------------------------------------------------------------------
function TeamInstantAction.Server.PreGame:OnTick()
	if (self:PlayerCountOk()) then
		if (not self.starting) then
			self.starting=true;
			self.game:ResetGameStartTimer(self.START_TIMER);
		end
	elseif (self.starting) then
		self.starting=false;
		self.warningTimer=0;
		
		self.game:ResetGameStartTimer(-1);
	end
	
	if (self.starting) then
		if (self.game:GetRemainingStartTimer()<=0) then
			self.starting=false;
			
			self:RestartGame(true);
		end
	else
		self.warningTimer = self.warningTimer-1;
		if (self.warningTimer<=0) then
			self.game:SendTextMessage(TextMessageCenter, "@mp_MinTeamWarning", TextMessageToAll, nil, self.game:GetMinTeamLimit());
			self.warningTimer=self.MIN_TEAM_LIMIT_WARN_TIMER;
		end
	end
end


----------------------------------------------------------------------------------------------------
function TeamInstantAction.Client.PreGame:OnTick()
	InstantAction.Client.PreGame.OnTick(self);
end

----------------------------------------------------------------------------------------------------
function TeamInstantAction.Server.InGame:OnTick()
	InstantAction.Server.InGame.OnTick(self);
end


----------------------------------------------------------------------------------------------------
function TeamInstantAction.Server.InGame:OnBeginState()
	self:ResetTime();
	self:StartTicking();
	self:ResetPlayers();
	self:ResetTeamScores();
end

----------------------------------------------------------------------------------------------------
function TeamInstantAction.Client.InGame:OnBeginState()
	InstantAction.Client.InGame.OnBeginState(self);
end


----------------------------------------------------------------------------------------------------
function TeamInstantAction.Server.InGame:OnUpdate(frameTime)
	TeamInstantAction.Server.OnUpdate(self, frameTime);

	self:CheckTimeLimit();
end


----------------------------------------------------------------------------------------------------
function TeamInstantAction.Server.PostGame:OnBeginState()
	self:StartTicking();
	self:SetTimer(self.NEXTLEVEL_TIMERID, self.NEXTLEVEL_TIME);
end


----------------------------------------------------------------------------------------------------
function TeamInstantAction.Client.PostGame:OnBeginState()
	
	InstantAction.Client.PostGame.OnBeginState(self);
end


----------------------------------------------------------------------------------------------------
function TeamInstantAction.Client.PostGame:OnEndState()
	InstantAction.Client.PostGame.OnEndState(self);
end

----------------------------------------------------------------------------------------------------
function TeamName(p)
	if (type(p)=="table") then
		p=p.id;
	elseif (type(p)=="string") then
		p=EntityNamed(p) and EntityNamed(p).id;
	end
	return g_gameRules.game:GetTeamName(g_gameRules.game:GetTeam(p));
end

function Team(p)
	if (type(p)=="table") then
		p=p.id;
	elseif (type(p)=="string") then
		p=EntityNamed(p) and EntityNamed(p).id;
	end
	return g_gameRules.game:GetTeam(p);
end


----------------------------------------------------------------------------------------------------
function TeamInstantAction.Client:ClVictory(teamId, type)
	if (teamId and teamId~=0) then
		local ownTeamId=self.game:GetTeam(g_localActorId);
		if(ownTeamId == teamId) then
			--self:PlaySoundAlert("win", ownTeamId);
			--self:PlayRadioAlert("win", ownTeamId);
			self.game:GameOver(1);
		else
			--self:PlaySoundAlert("lose", ownTeamId);
			--self:PlayRadioAlert("lose", ownTeamId);
			self.game:GameOver(-1);
		end
	else
		self.game:GameOver(0);
	end
end
----------------------------------------------------------------------------------------------------
-- [Crysis Coop] coop overrides (must stay the LAST statement of this file)
Script.LoadScript("scripts/coop/coopgamerules.lua", 1, 1);
