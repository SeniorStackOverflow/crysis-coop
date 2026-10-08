--------------------------------------------------------------------------
-- Crysis Coop - tunables. Safe to edit; restart the level after changes.
--------------------------------------------------------------------------
CoopConfig =
{
	VERSION = "0.1.0",
	DEBUG = true,                       -- "[Coop] ..." lines in Game.log

	TEAM_NAME = "black",                -- US nanosuit models (stock TIA team)
	START_TIMER = 3,

	RESPAWN_DELAY = 8,                  -- seconds dead before auto-respawn (coop_auto_respawn 1)
	REVIVE_TIME = 3,                    -- seconds a teammate holds the use key next to a downed player
	REVIVE_RANGE = 4.5,                 -- m between them (Coop.dll's use key: 4)
	REVIVE_RANGE_SERVER = 10,           -- m the server allows: a friend's game may have him a few metres off
	COMPANION_DAMAGE = 0.5,             -- the AI companion takes this share of the enemies' damage (a buddy, as in other games)
	-- a bullet's damage by the distance it flew (players' and soldiers' alike):
	-- full up to "near" metres, then less and less, down to "min" times at "far"
	-- (on top of the single player weapons' own small drop after 50 m)
	DAMAGE_FALLOFF = {
		default    = { near = 25, far = 120, min = 0.6 },
		FY71       = { near = 30, far = 150, min = 0.6 },
		SCAR       = { near = 30, far = 150, min = 0.6 },
		SMG        = { near = 15, far = 70,  min = 0.5 },
		SOCOM      = { near = 15, far = 60,  min = 0.45 },
		Shotgun    = { near = 6,  far = 35,  min = 0.2 },
		Hurricane  = { near = 20, far = 100, min = 0.5 },
		DSG1       = false,             -- a sniper rifle: the same at any distance
		GaussRifle = false,
	},
	SILENCER_DAMAGE = 0.8,              -- a silenced weapon's bullets do this share (quiet costs power)
	-- how much ammo of each kind a player carries: the campaign's numbers
	-- (Scripts/Entities/actor/player.lua), not the much smaller multiplayer ones
	AMMO_CAPACITY = {
		bullet=40*7, fybullet=30*10, lightbullet=20*10, smgbullet=50*7,
		explosivegrenade=10, flashbang=10, smokegrenade=10, empgrenade=10, scargrenade=10,
		rocket=3, sniperbullet=10*3, tacbullet=4*5, tagbullet=10, gaussbullet=4*5,
		hurricanebullet=500, incendiarybullet=30*10, shotgunshell=8*5,
		avexplosive=3, c4explosive=4, claymoreexplosive=3, rubberbullet=30*20, tacgunprojectile=5,
	},
	REVIVE_HEALTH = 0.5,                -- part of his health a revived player gets back
	ALL_DOWN_DELAY = 6,                 -- seconds everybody is down before the team is back at the last checkpoint
	BACK_FADE_TIME = 1.5,               -- seconds the screen takes to come out of black there
	SPAWN_INVULNERABILITY = 5,          -- seconds of invulnerability after (re)spawn
	SPAWN_RADIUS = 2.5,                 -- meters from the leader
	SPAWN_Z_OFFSET = 0.5,
	SPAWN_SLOTS = 6,                    -- positions on the ring around the leader
	ANCHOR_INTERVAL = 3,                -- ticks (seconds) between leader position snapshots
	TELEPORT_COOLDOWN = 20,
	SETTLE_SECONDS = 8,                 -- host must stand still this long (intro over) before teammates spawn
	SETTLE_MAX_SPEED = 3,               -- m/s; faster than this = still in plane / falling / driving
	JOIN_WAIT_MAX = 15,                 -- seconds a joiner waits (not counting cutscenes) before he is
	                                    -- spawned next to a host who stays still anyway (away from the keyboard)

	-- autotest only (sv_servername contains "AUTOTEST"): observe-only AI report
	-- radius around the host once he stands on solid ground
	AUTOTEST_REPORT_RADIUS = 60,             -- seconds between coop_tp uses per player

	AI_DAMAGE_TO_PLAYER_MULT = 1.0,     -- difficulty knob: <1 easier, >1 harder

	EQUIP_PACK = "DefaultPlayer",       -- same pack SinglePlayer gives the SP player
	EXTRA_ITEMS = { "OffHand", "Fists", "Binoculars", "SOCOM" },
	SELECT_ITEM = "SCAR",               -- given last and selected

	LEVEL_PREFIX = "multiplayer/tia/coop_",
	MAP_COMMAND_FMT = "coop_change_map %s",   -- keeps the players connected
	MAPCHANGE_DELAY_MS = 8000,
	CAMPAIGN = { "island", "village", "rescue", "harbor", "tank", "mine",
	             "core", "ice", "sphere", "ascension", "fleet" },
};
