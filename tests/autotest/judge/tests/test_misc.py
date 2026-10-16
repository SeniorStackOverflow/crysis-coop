"""D08 perf, D09 sound, D10 misc, D11 script, D14 idle gunner, D15 vehicles.

Excerpts are real lines from the goal_runs test logs, the user's Game.log of
2026-10-15 16:45 and the user's LogBackups (Game.log of 2026-09-29, 10-10,
10-11, 10-13, companion of 10-14). Where a shape exists only in the source
(the sound restart that failed, PAUSED, NOT FREED, an audible hidden
vehicle, a NetSerialize failure outside the baseline, a put back from "a
vehicle", a test command's Lua error) a real line is changed as little as
possible and the comment says so.
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from coopjudge.context import Context  # noqa: E402
from coopjudge.detectors import d08_misc as d  # noqa: E402
from coopjudge.findings import merge  # noqa: E402
from coopjudge.logread import from_text  # noqa: E402


def run(det, host_text, companion_text=None):
    host = from_text(host_text, "Game.log", "host")
    comp = from_text(companion_text, "companion.log", "companion") if companion_text else None
    return merge(det(Context(host, comp)))


# ---------------------------------------------------------------- D08 perf

# hdiagB_host.log 1700-1705: a 2.7 s frame in the middle of the game
HITCH_MIDGAME = """\
[CoopWatch] Pilot_3 at (1766 1881 308) speed 0.0; ai enabled=1 proxy=1 alert=0 character=Cover2 pipe=0 target=- behaviour=InVehicle; health 999 in Asian_Helicopter_1
[CoopStealth] seen: Nomad 31/11
[CoopVoice] languages/dialog/ai_korean_soldier_1/alertheargroup_06.mp2
[CoopHitch] a frame took 2715 ms
[CoopHitch] a frame took 342 ms
"""
# alldown_host.log 131-148: the first frame after the level load
HITCH_LEVEL_START = """\
*LOADING: Level multiplayer/tia/coop_rescue loading time: 30.08 seconds
[CoopAgent] AI companion started (process 19632, joins 127.0.0.1:64087, agents on 127.0.0.1:47810) in 172 ms
[Error] <Flash> Error: CallMethod - 'gotoAndPlay' on invalid object. [Libs/UI/HUD_RadarCompassStealth.gfx]
[CoopTOD] the network game set speed 0.0000: the level's 0.0010 kept
[CoopHitch] a frame took 1357 ms
"""
# gunParN2_host.log 5390-5391: a test command made it
HITCH_TEST_CMD = """\
[CoopTest] cmdfile: coop_sp_scope 1
[CoopHitch] a frame took 1037 ms
"""
# fin10_manual_host.log 10103, 10440-10448, 10466: before "InGame", 340 lines after the load
HITCH_BEFORE_INGAME = ("*LOADING: Level multiplayer/tia/coop_rescue loading time: 35.10 seconds\n"
                       + "File open: textures/terrain/detail/plains_pepples_a_ddn.dds\n" * 300
                       + "[CoopObj] broadcast Rescue.Objective_03 -> 3\n"
                       "[CoopHitch] a frame took 1220 ms\n"
                       "[Coop] PreGame -> InGame\n")
# fgcomp_host.log 953-955 and 1046-1047: 4.1 s, some 10 s after the companion's join
HITCH_AFTER_JOIN = """\
Nomad(1)$o renamed to $8Companion
[Coop] Companion put into Non_Driveable_Vehicles.Civ_Car9 seat 2 (false)
[CoopPos] server: Nomad=(1364,1972,231)hp200 Companion=(1363,1971,231)hp200
[CoopHitch] a frame took 4113 ms
[CoopHitch] a frame took 790 ms
"""
# fix1_host.log 399; the user's Game.log 1557 (17 fps: no)
PERF_8FPS = "[CoopPerf] last minute: 8 fps on average, worst frame 207 ms, 460 frames over 100 ms; sound: 0 voices, cpu 0.0, memory 0.0 of 0.0 MB\n"
PERF_17FPS = "[CoopPerf] last minute: 17 fps on average, worst frame 300 ms, 39 frames over 100 ms; sound: 38 voices, cpu 0.0, memory 54.2 of 52.3 MB\n"
# Game.log of 2026-10-10 21:04, line 2011; before_hscopeA_companion.log 1057
# (the AI companion's game is held to ~16 fps; 7 such 15 fps minutes in goal_runs)
PERF_14FPS = "[CoopPerf] last minute: 14 fps on average, worst frame 162 ms, 36 frames over 100 ms; sound: 52 voices, cpu 0.0, memory 49.7 of 44.2 MB\n"
PERF_15FPS_COMPANION = "[CoopPerf] last minute: 15 fps on average, worst frame 1040 ms, 7 frames over 100 ms; sound: 0 voices, cpu 0.0, memory 0.0 of 0.0 MB\n"
# the user's Game.log 2611 with " PAUSED" added (CoopAI.cpp's format: "...MB%s", " PAUSED")
PERF_PAUSED = "[CoopPerf] last minute: 31 fps on average, worst frame 107 ms, 1 frames over 100 ms; sound: 54 voices, cpu 0.0, memory 58.3 of 56.5 MB PAUSED\n"


class TestD08Perf(unittest.TestCase):
    def test_midgame_hitch(self):
        f = run(d.d08_perf, HITCH_MIDGAME)
        self.assertEqual(len(f), 1)
        self.assertEqual((f[0].id, f[0].verdict), ("D08", "WARN"))
        self.assertIn("2715", f[0].en)
        self.assertIn("2.7", f[0].ru)
        self.assertEqual(f[0].evidence[0].no, 4)

    def test_hitches_counted_longest_first(self):
        f = run(d.d08_perf, "[CoopHitch] a frame took 1044 ms\n" + HITCH_MIDGAME)
        self.assertEqual(f[0].count, 2)
        self.assertIn("2715", f[0].evidence[0].text)

    def test_level_start_and_test_command_are_no_freeze(self):
        self.assertEqual(run(d.d08_perf, HITCH_LEVEL_START), [])
        self.assertEqual(run(d.d08_perf, HITCH_TEST_CMD), [])
        self.assertEqual(run(d.d08_perf, HITCH_BEFORE_INGAME), [])

    def test_short_hitch_is_nothing(self):
        # the user's Game.log 147
        self.assertEqual(run(d.d08_perf, "[CoopHitch] a frame took 617 ms\n"), [])

    def test_hitch_in_checkpoint_load_window(self):
        # the user's Game.log 408-411, then a long frame right after the load
        text = ("[CoopSave] loading coop_checkpoint_56c79107e0d7a6ff_b.CRYSISJMSF (rescue, checkpoint manual)\n"
                "[CoopSave] the saved game took 1.5 s to load\n"
                "[CoopSave] progress loaded: rescue, checkpoint manual (saved 2026-10-14 12:36:40)\n"
                "[Coop] checkpoint loaded: 1 players' places kept\n"
                "[CoopHitch] a frame took 1500 ms\n")
        self.assertEqual(run(d.d08_perf, text), [])

    def test_settle_by_time_stamps(self):
        early = "<16:00:00> [CoopTest] cmdfile: coop_sp_scope 1\n<16:00:10> [CoopHitch] a frame took 1037 ms\n"
        late = "<16:00:00> [CoopTest] cmdfile: coop_sp_scope 1\n<16:00:45> [CoopHitch] a frame took 1037 ms\n"
        self.assertEqual(run(d.d08_perf, early), [])
        self.assertEqual(len(run(d.d08_perf, late)), 1)

    def test_test_command_settles_for_10_s_only(self):
        # a test command's own long frame comes at once; 20 s later it is a freeze
        after = "<16:00:00> [CoopTest] cmdfile: coop_sp_scope 1\n<16:00:20> [CoopHitch] a frame took 1037 ms\n"
        self.assertEqual(len(run(d.d08_perf, after)), 1)
        # ... while a join settles for 30 s
        join = "<16:00:00> Nomad(1)$o renamed to $8Companion\n<16:00:20> [CoopHitch] a frame took 1037 ms\n"
        self.assertEqual(run(d.d08_perf, join), [])

    def test_long_freeze_while_settling(self):
        f = run(d.d08_perf, HITCH_AFTER_JOIN)
        self.assertEqual(len(f), 1)
        self.assertIn("4113", f[0].en)
        self.assertEqual(f[0].evidence[0].no, 4)
        # a 1.5 s frame there is the join settling
        self.assertEqual(run(d.d08_perf, HITCH_AFTER_JOIN.replace("4113", "1500")), [])

    def test_long_frame_inside_a_load_is_no_freeze(self):
        self.assertEqual(run(d.d08_perf, HITCH_BEFORE_INGAME.replace("1220", "4113")), [])
        text = ("[CoopSave] loading coop_checkpoint_56c79107e0d7a6ff_b.CRYSISJMSF (rescue, checkpoint manual)\n"
                "[CoopSave] the saved game took 1.5 s to load\n"
                "[CoopHitch] a frame took 4113 ms\n"
                "[CoopSave] progress loaded: rescue, checkpoint manual (saved 2026-10-14 12:36:40)\n"
                "[Coop] checkpoint loaded: 1 players' places kept\n")
        self.assertEqual(run(d.d08_perf, text), [])

    def test_low_fps(self):
        f = run(d.d08_perf, PERF_17FPS + PERF_8FPS + PERF_8FPS)
        self.assertEqual(len(f), 1)
        self.assertEqual(f[0].verdict, "WARN")
        self.assertEqual(f[0].count, 2)
        self.assertIn("8", f[0].ru)

    def test_17_fps_is_fine(self):
        self.assertEqual(run(d.d08_perf, PERF_17FPS), [])

    def test_paused_sound(self):
        f = run(d.d08_perf, PERF_PAUSED)
        self.assertEqual(len(f), 1)
        self.assertIn("PAUSED", f[0].en)

    def test_companion_fps(self):
        f = run(d.d08_perf, PERF_17FPS, PERF_8FPS)
        self.assertEqual(len(f), 1)
        self.assertIn("компаньона", f[0].ru)

    def test_companion_frame_limit_is_no_slowness(self):
        # the AI companion runs at ~16 fps by design: 14-15 is not slow there
        self.assertEqual(run(d.d08_perf, PERF_17FPS, PERF_15FPS_COMPANION + PERF_14FPS), [])
        f = run(d.d08_perf, PERF_14FPS)
        self.assertEqual([(x.verdict, x.key) for x in f], [("WARN", "lowfps:host")])


# --------------------------------------------------------------- D09 sound

# the user's Game.log of 2026-10-10 19:39, lines 3686 and 3694
SILENCE = """\
[CoopSound] no sound came out for 15 s (session playing silence; sound system running, 98 voices, cpu 0.0, memory 66635752 of 66645568): the sound system starts again
[CoopStealth] seen: nobody
[CoopSound] the sound comes out again after the restart (peak 0.223)
"""
# CoopSound.cpp's format; no log has it
RESTART_FAILED = "[CoopSound] still no sound 5 s after the restart (session playing silence)\n"
# sat_off_host.log 672 (latest 69.4 s), heavy_old2_host.log 539 (3.6 s),
# ab_off_host.log 1373 (the older "came late anyway", 144.9 s)
LATE_69 = ("[CoopSound] last minute: 1253 sound parts stuck at their start (virtual), 0 stopped; 83 came late (0.5 s or more, the latest 69.4 s); "
           "audible sounds lost their voice midway 183 times; one-shots heard at once 651 of 2040; FMOD: 128 channels playing (58 heard, 70 virtual) "
           "of 256, 64 mixed at most; cpu 28.6% (dsp 18.2, stream 8.1, update 2.3)\n")
LATE_3 = ("[CoopSound] last minute: 584 sound parts stuck at their start (virtual), 0 stopped; 17 came late (0.5 s or more, the latest 3.6 s); "
          "audible sounds lost their voice midway 38 times (audibility under 0.02: 6, under 0.1: 23, louder: 9); one-shots heard at once 1333 of 2715; "
          "FMOD: 70 channels playing (31 heard, 39 virtual) of 256, 64 mixed at most; cpu 14.1% (dsp 11.8, stream 1.8, update 0.5)\n")
LATE_OLD = ("[CoopSound] last minute: 58 sound parts could not start (FMOD's 64 voices all taken), 0 stopped; 108 came late anyway (0.5 s or more, "
            "the latest 144.9 s); heard sounds lost their voice 13 times; FMOD: 14 channels playing (5 heard, 9 virtual) of 256, 64 mixed at most; "
            "cpu 5.9% (dsp 4.0, stream 1.7, update 0.2)\n")
# heavy_old_host.log 539: the fix off (coop_sound_unstick 0), the latest 4.9 s;
# sat_on_host.log 817: the fix on under a saturating stress test, 3.3 s
LATE_FIX_OFF = ("[CoopSound] last minute: 448 sound parts stuck at their start (virtual), 0 stopped; 19 came late (0.5 s or more, the latest 4.9 s); "
                "audible sounds lost their voice midway 37 times; one-shots heard at once 1290 of 2528; FMOD: 64 channels playing (27 heard, 37 virtual) "
                "of 256, 64 mixed at most; cpu 14.3% (dsp 11.6, stream 2.1, update 0.6)\n")
LATE_FIX_STRESSED = ("[CoopSound] last minute: 996 sound parts stuck at their start (virtual), 862 stopped: fy71_med_single_ext_04_e708.wa "
                     "fy71_dist_single_ext_0_e6ff.wa sniper_indoor_3rd_dist_03_left fy71_dist_single_ext_0_e6ff.wa; 17 came late (0.5 s or more, "
                     "the latest 3.3 s); audible sounds lost their voice midway 128 times; one-shots heard at once 528 of 1641; FMOD: 7 channels "
                     "playing (4 heard, 3 virtual) of 256, 64 mixed at most; cpu 5.3% (dsp 3.8, stream 1.3, update 0.1)\n")
# the user's Game.log 646: a normal minute with the fix (6 late, the latest 0.6 s)
LATE_OK = ("[CoopSound] last minute: 195 sound parts stuck at their start (virtual), 59 stopped: glass_thick_small_glass_01.wav@a "
           "metal_thick_small_metal_thick_ mat_plastic_medium_mat_plastic wood_hollow_small_wood_hollow_; 6 came late (0.5 s or more, the latest 0.6 s); "
           "audible sounds lost their voice midway 96 times (audibility under 0.02: 2, under 0.1: 86, louder: 8: wood_hollow_medium_wood_hollow(0.11,454 ms)); "
           "one-shots heard at once 611 of 940; FMOD: 11 channels playing (10 heard, 1 virtual) of 256, 64 mixed at most; cpu 6.1% (dsp 4.7, stream 1.3, update 0.1)\n")


class TestD09Sound(unittest.TestCase):
    def test_silence(self):
        f = run(d.d09_sound, SILENCE)
        self.assertEqual(len(f), 1)
        self.assertEqual((f[0].id, f[0].verdict), ("D09", "FAIL"))
        self.assertIn("15 с", f[0].ru)
        self.assertIn("вернулся", f[0].ru)
        self.assertEqual([l.no for l in f[0].evidence], [1, 3])

    def test_restart_failed(self):
        f = run(d.d09_sound, RESTART_FAILED)
        self.assertEqual([x.verdict for x in f], ["FAIL"])
        self.assertIn("не вернулся", f[0].ru)

    def test_restart_that_worked_alone_is_nothing(self):
        self.assertEqual(run(d.d09_sound, "[CoopSound] the sound comes out again after the restart (peak 0.223)\n"), [])

    def test_late_fail_and_warn(self):
        f = run(d.d09_sound, LATE_3 + LATE_69)
        self.assertEqual(len(f), 1)
        self.assertEqual(f[0].verdict, "FAIL")
        self.assertIn("69.4", f[0].en)
        self.assertEqual(f[0].evidence[0].no, 2)
        f = run(d.d09_sound, LATE_3)
        self.assertEqual([x.verdict for x in f], ["WARN"])

    def test_late_4_s_is_the_bug(self):
        # every run with the fix off reaches 4.3 s or more, the fix's stress runs 3.3 s at most
        self.assertEqual([x.verdict for x in run(d.d09_sound, LATE_FIX_OFF)], ["FAIL"])
        self.assertEqual([x.verdict for x in run(d.d09_sound, LATE_FIX_STRESSED)], ["WARN"])

    def test_late_old_format(self):
        f = run(d.d09_sound, LATE_OLD)
        self.assertEqual([x.verdict for x in f], ["FAIL"])
        self.assertIn("144.9", f[0].en)

    def test_normal_minute_is_nothing(self):
        self.assertEqual(run(d.d09_sound, LATE_OK), [])
        # the minute line's other "sound limits" line
        self.assertEqual(run(d.d09_sound, "[CoopSound] sound limits: s_PriorityThreshold 45, s_MaxChannels 80 (less important new sounds "
                                          "dropped above 64 voices), s_MaxActiveSounds 512\n"), [])


# ---------------------------------------------------------------- D10 misc

# the user's Game.log 169 and Game.log of 2026-10-11 12:22: the noise baseline
NETSER_NOISE = """\
[CoopNet] NetSerialize failed: FY711474_GrenadeLauncher (GrenadeLauncher, id 1112081) aspect=8 profile=255 reading line=-2
[CoopNet] NetSerialize failed: SOCOM265_LAMFlashLight (LAMFlashLight, id 65173) aspect=8 profile=255 reading line=-2
"""
# the same shape for a soldier, aspect 4 (CoopAI.cpp CoopNetSerFail's format, no log has it)
NETSER_ACTOR = "[CoopNet] NetSerialize failed: Bridge_2 (Grunt, id 64521) aspect=4 profile=0 reading line=12\n"
# the user's Game.log 1196
PUT_BACK = ("[CoopVeh] Companion came out of Non_Driveable_Vehicles.Civ_Car8 330 m from where he got out (at 1678 1805 194): "
            "put back there (1537 2107 236)\n")
# goal_runs: "put back here" of the companion's own position (not a vehicle exit)
PUT_BACK_OTHER = ("<11:22:20> [CoopAgent] Companion was 2.7 m off in its own game (at 1696.1 1824.4 194.2, here 1698.0 1826.4 194.7): "
                  "put back here\n")
# PlayerView.cpp's "a vehicle" (the vehicle gone) in the user's Game.log 1196
PUT_BACK_NO_VEHICLE = PUT_BACK.replace("Non_Driveable_Vehicles.Civ_Car8", "a vehicle")
# gunNew2_host.log 1001; Game.log of 2026-10-10 3146 and gunNew2_host.log (other "m off" lines)
CAMERA = "[Coop] Non_Driveable_Vehicles.Civ_Car9's camera in driver was 189 m off (view 1): view 1 now\n"
# gunNew2_host.log 999-1001 and gunNew2_companion.log 867-869: the test moved
# both players while they sat in Civ_Car9; the cameras stayed behind
CAMERA_TELEPORT_HOST = """\
[CoopTest] cmdfile: #g_gameRules.game:MovePlayer(g_localActorId,{x=1543,y=1914,z=227},{x=0,y=0,z=0}); local c=System.GetEntityByName("Companion"); if c then g_gameRules.game:MovePlayer(c.id,{x=1541,y=1912,z=227},{x=0,y=0,z=0}) end; System.LogAlways("[CHK] in the Bridge trigger")
[CHK] in the Bridge trigger
[Coop] Non_Driveable_Vehicles.Civ_Car9's camera in driver was 189 m off (view 1): view 1 now
"""
CAMERA_TELEPORT_COMPANION = """\
[CoopInput] vehicle Non_Driveable_Vehicles.Civ_Car9 powered=1 processing=1 damage=0.00 destroyed=0
[CoopInput] Companion filters: | maps vehicle=1 player=0 | vehicle=Non_Driveable_Vehicles.Civ_Car9 paused=0
[Coop] Non_Driveable_Vehicles.Civ_Car9's camera in passenger was 188 m off (view 1): view 1 now
"""
# companion of 2026-10-14 12:27, line 1496: the host drove fast, no test (real)
CAMERA_COMPANION_REAL = "[Coop] Non_Driveable_Vehicles.Civ_Car9's camera in passenger was 52 m off (view 1): view 1 now\n"
CAMERA_OTHER = """\
[Coop] Non_Driveable_Vehicles.Civ_Car8's third person camera in driver was far (2473 m off): updated by hand (1 of 147 frames), follows again
[CoopAI] vehicle River_Helicopter_1 put where the server has it (8 m off)
"""
# a real "freed" line, and the same with CoopSave.cpp's other ending
SEAT_FREED = ("[CoopSave] after the load: seat passenger of Non_Driveable_Vehicles.Civ_Car9 held a passenger who is not in the game "
              "(id 129064): freed\n")
SEAT_NOT_FREED = SEAT_FREED.replace(": freed", ": NOT FREED")


class TestD10Misc(unittest.TestCase):
    def test_netserialize_baseline_is_nothing(self):
        self.assertEqual(run(d.d10_misc, NETSER_NOISE), [])

    def test_netserialize_other(self):
        f = run(d.d10_misc, NETSER_NOISE + NETSER_ACTOR + NETSER_ACTOR.replace("64521", "64522"))
        self.assertEqual(len(f), 1)
        self.assertEqual((f[0].id, f[0].verdict, f[0].count), ("D10", "WARN", 2))
        self.assertIn("Grunt", f[0].en)

    def test_put_back(self):
        f = run(d.d10_misc, PUT_BACK)
        self.assertEqual([x.verdict for x in f], ["WARN"])
        self.assertIn("330", f[0].ru)
        self.assertEqual(run(d.d10_misc, PUT_BACK_OTHER), [])

    def test_put_back_of_a_vehicle_gone(self):
        f = run(d.d10_misc, PUT_BACK_NO_VEHICLE)
        self.assertEqual([x.key for x in f], ["putback:Companion:a vehicle"])

    def test_camera(self):
        f = run(d.d10_misc, CAMERA, CAMERA.replace("driver was 189", "driver was 188"))
        self.assertEqual(len(f), 1)
        self.assertEqual(f[0].count, 2)
        self.assertEqual(run(d.d10_misc, CAMERA_OTHER), [])

    def test_camera_after_a_test_teleport_is_nothing(self):
        self.assertEqual(run(d.d10_misc, CAMERA_TELEPORT_HOST, CAMERA_TELEPORT_COMPANION), [])
        # the companion's camera in a car the test did not move the host's camera from
        f = run(d.d10_misc, CAMERA_TELEPORT_HOST, CAMERA_COMPANION_REAL.replace("Civ_Car9", "Civ_Car8"))
        self.assertEqual([x.en for x in f], ["companion: camera in passenger of Non_Driveable_Vehicles.Civ_Car8 was 52 m off"])
        # the same camera line long after the teleport
        far = CAMERA_TELEPORT_HOST.replace("[CHK] in the Bridge trigger\n", "[CHK] in the Bridge trigger\n" + "[CoopStealth] seen: Nomad 30/8\n" * 45)
        self.assertEqual(len(run(d.d10_misc, far)), 1)

    def test_camera_of_the_companion_alone(self):
        f = run(d.d10_misc, "", CAMERA_COMPANION_REAL)
        self.assertEqual([x.verdict for x in f], ["WARN"])
        self.assertIn("компаньона", f[0].ru)

    def test_seat(self):
        self.assertEqual(run(d.d10_misc, SEAT_FREED), [])
        f = run(d.d10_misc, SEAT_NOT_FREED)
        self.assertEqual([x.verdict for x in f], ["WARN"])
        self.assertIn("NOT FREED", f[0].en)


# -------------------------------------------------------------- D11 script

# the user's Game.log of 2026-09-29 19:48, lines 32467-32471: a Lua error and
# its stack (no log has one in the middle of a game: these lines without the
# session's end before them stand for one)
LUA_ERROR = """\
CGameStats::RemovePlayer 00007777
[Warning] InvokeRMI: RMI via server (to client) requested but we are not a server
[Warning] [Lua Error] RMI via server (to client) requested but we are not a server
      > ClClientDisconnect, ([C]: -1)
    > (null) (scripts/gamerules/instantaction.lua: 399)
[CoopStealth] seen: nobody
"""
# the same, lines 32465-32471 as they are: the host was leaving the game
LUA_ERROR_LEAVING = ("[net 16:49:36.244] Disconnect <local>:16; profid=0; cause=13; msg='Nub destroyed'\n"
                     "CGameClientChannel::Release\n" + LUA_ERROR)
# gunNew2_host.log 999 (a test's Lua command) followed by a Lua error about it
# (the error line is made up: no test command failed in any log)
LUA_TEST_CMD = """\
[CoopTest] cmdfile: #g_gameRules.game:MovePlayer(g_localActorId,{x=1543,y=1914,z=227},{x=0,y=0,z=0}); local c=System.GetEntityByName("Companion"); if c then g_gameRules.game:MovePlayer(c.id,{x=1541,y=1912,z=227},{x=0,y=0,z=0}) end; System.LogAlways("[CHK] in the Bridge trigger")
[Warning] [Lua Error] [string "g_gameRules.game:MovePlayer(g_localActorId,{..."]:1: attempt to index a nil value
"""
# goal_runs: Flash's errors and the level's Lua lines are no script errors
NOT_SCRIPT = """\
[Error] <Flash> Error: CallMethod - 'gotoAndPlay' on invalid object. [Libs/UI/HUD_GrenadeDetect.gfx]
<Lua> AIAnchor[VTOL_LANDING2]:  undefined type [_wrong_type_] Cant register with [AISYSTEM]
<Lua> MissionObjective Rescue.Objective_03 Activated
[Error] Debug: CTextureCache::InitPool: GetRenderer()->DownLoadToVideoMemory returned 0
"""
# CryScriptSystem.dll's "[Lua Error] Failed to load script file %s" (no log has it)
LUA_LOAD = "[Error] [Lua Error] Failed to load script file Scripts/Coop/CoopTestLib.lua\n"


class TestD11Script(unittest.TestCase):
    def test_lua_error_with_stack(self):
        f = run(d.d11_script, LUA_ERROR)
        self.assertEqual(len(f), 1)
        self.assertEqual((f[0].id, f[0].verdict), ("D11", "FAIL"))
        self.assertEqual([l.no for l in f[0].evidence], [3, 4, 5])
        self.assertIn("RMI via server", f[0].en)

    def test_not_script_errors(self):
        self.assertEqual(run(d.d11_script, NOT_SCRIPT), [])

    def test_error_while_leaving_the_game_is_nothing(self):
        self.assertEqual(run(d.d11_script, LUA_ERROR_LEAVING), [])
        # a new level: errors count again
        again = LUA_ERROR_LEAVING + "*LOADING: Level multiplayer/tia/coop_rescue loading time: 24.98 seconds\n" + LUA_ERROR
        self.assertEqual([x.verdict for x in run(d.d11_script, again)], ["FAIL"])

    def test_test_command_error_is_infra(self):
        f = run(d.d11_script, LUA_TEST_CMD)
        self.assertEqual([(x.verdict, [l.no for l in x.evidence]) for x in f], [("INFRA", [2, 1])])

    def test_key_without_digits(self):
        # the same error in two files that differ only in a number: one finding
        one = LUA_LOAD.replace("CoopTestLib", "CoopTestLib1")
        two = LUA_LOAD.replace("CoopTestLib", "CoopTestLib22")
        f = run(d.d11_script, one + two + LUA_LOAD)
        self.assertEqual(sorted(x.count for x in f), [1, 2])


# --------------------------------------------------------- D14 idle gunner

# compgun_base2_companion.log 1333 and 1450: at the car's gun, 0 bursts
ZERO_BURSTS = """\
[CoopAgent] fighting Bridge_7 from Land.Chinese_4wd1's gun
[CoopAgent] gun aim at Bridge_7: yaw off 0.030 pitch off 0.010 (signs -1 -1), view (-0.38 -0.92 -0.10)
[CoopAgent] the last minute: 0 bursts fired; no shot: 698 frames something in the way, 0 enemy not in sight, 79 turning to him; stuck 0 times, 1 ways round found close by
"""
# hfixE_companion.log 963-1057: fought on the way, 7 frames without a shot;
# gunBurst_companion.log 955: 0 bursts, no fight at all
ZERO_BURSTS_FINE = """\
[CoopAgent] fighting Jungle_Convoy_4 on the way to Nomad
[CoopAgent] the last minute: 0 bursts fired; no shot: 1 frames something in the way, 0 enemy not in sight, 6 turning to him; stuck 0 times, 1 ways round found close by
[CoopAgent] the last minute: 0 bursts fired; no shot: 0 frames something in the way, 0 enemy not in sight, 0 turning to him; stuck 0 times, 1 ways round found close by
"""
# fin_convoy_companion.log 1018 and 1053: a 5 s fight the test cut short (it
# killed the convoy): 83 frames at ~16 fps is no idle gunner
ZERO_BURSTS_SHORT_FIGHT = """\
[CoopAgent] fighting Jungle_Convoy_3
[CoopAgent] the last minute: 0 bursts fired; no shot: 37 frames something in the way, 39 enemy not in sight, 7 turning to him; stuck 0 times, 2 ways round found close by
"""
# revfar_new_companion.log 1401 (6 times) and adk_old_companion.log 1283 (3: fine)
STUCK = "[CoopAgent] the last minute: 0 bursts fired; no shot: 0 frames something in the way, 0 enemy not in sight, 0 turning to him; stuck 6 times, 16 ways round found close by\n"
STUCK_FINE = "[CoopAgent] the last minute: 0 bursts fired; no shot: 0 frames something in the way, 0 enemy not in sight, 0 turning to him; stuck 3 times, 8 ways round found close by\n"
# gfix32_host.log 6432; the harness's own line about the gunner (not the mod's)
ENEMY_GUNNER = "[CoopAI] Bridge_2 at the gun of Bridge_Truck_1 (no driver) sees Companion and has not fired for 5 s: sub-pipe 27 cancelled\n"
ENEMY_GUNNER_FINE = "[CHK] gunner: a burst, turn -57.2958\n"


class TestD14IdleGunner(unittest.TestCase):
    def test_zero_bursts(self):
        f = run(d.d14_idle_gunner, "", ZERO_BURSTS)
        self.assertEqual(len(f), 1)
        self.assertEqual((f[0].id, f[0].verdict), ("D14", "WARN"))
        self.assertIn("777", f[0].en)
        self.assertEqual([l.no for l in f[0].evidence], [3, 1])

    def test_zero_bursts_fine(self):
        self.assertEqual(run(d.d14_idle_gunner, "", ZERO_BURSTS_FINE), [])
        self.assertEqual(run(d.d14_idle_gunner, "", ZERO_BURSTS_SHORT_FIGHT), [])

    def test_stuck(self):
        f = run(d.d14_idle_gunner, "", STUCK)
        self.assertEqual([x.key for x in f], ["stuck"])
        self.assertIn("6 раз", f[0].ru)
        self.assertEqual(run(d.d14_idle_gunner, "", STUCK_FINE), [])

    def test_enemy_gunner(self):
        f = run(d.d14_idle_gunner, ENEMY_GUNNER + ENEMY_GUNNER)
        self.assertEqual(len(f), 1)
        self.assertEqual(f[0].count, 2)
        self.assertIn("Bridge_Truck_1", f[0].ru)
        self.assertEqual(run(d.d14_idle_gunner, ENEMY_GUNNER_FINE), [])


# ------------------------------------------------------------ D15 vehicles

# goal_runs (56 such lines): hidden, its run sound virtual: not heard
HIDDEN_VIRTUAL = ("[CoopTest] vehicle Reinforcement_Heli_Fish Asian_helicopter at 642 m (1963 2109 278) hidden "
                  "1:pilot(driver)(gunner)=fish_pilot 2:gunner(gunner)=fish_gunner 3:backseat1=fish_helitroop_1; ai on, AI driver's ai off; "
                  "engine powered=1 starting=0 off=0 disabled=0 run:playing(virtual) vol 1.00 to 750 m\n")
# the same heard (CoopEngineState's "playing" without "(virtual)"): the bug
HIDDEN_HEARD = HIDDEN_VIRTUAL.replace("run:playing(virtual)", "run:playing")
# goal_runs [CoopWatch] (45 such lines) and the same heard
WATCH_VIRTUAL = ("[CoopWatch] Asian_Helicopter_1 at (2450 1523 274) speed 0.0 hidden; ai enabled=0 proxy=0 alert=0 character=HeliAggressive "
                 "pipe=0 target=- behaviour=HeliAggressiveIdle; driver Pilot_3; engine powered=1 starting=0 off=0 disabled=0 "
                 "run:playing(virtual) vol 1.00 to 750 m\n")
WATCH_HEARD = WATCH_VIRTUAL.replace("run:playing(virtual)", "run:playing")
# fgcomp_host.log 1019: a visible helicopter heard flying: fine
VISIBLE_HEARD = ("[CoopTest] vehicle Asian_Helicopter_1 Asian_helicopter at 606 m (1939 1801 308) 1:pilot(driver)(gunner)=Pilot_3 "
                 "2:gunner(gunner)=- 3:backseat1=-; ai on, AI driver's ai on; engine powered=1 starting=0 off=0 disabled=0 run:playing vol 1.00 to 750 m\n")
# hscopeA_host.log 934 and 1038: the hovering helicopter of 0.11.30, crewed, its AI off
HELI_HOVER = ("[CoopTest] vehicle Asian_Helicopter_1 Asian_helicopter at 342 m (1279 1645 281) 1:pilot(driver)(gunner)=Pilot_3 2:gunner(gunner)=- "
              "3:backseat1=- 4:backseat2=- 5:backseat3=- 6:backseat4=- 7:backseat5=- 8:backseat6=-; ai off, AI driver's ai on; "
              "engine powered=1 starting=0 off=0 disabled=0 run:playing(virtual) vol 1.00 to 750 m\n")
# fgcomp_host.log 1026 (and again in the next listing): farm_Jeep_2 parked crewed, its AI on: fine
JEEP_PARKED = ("[CoopTest] vehicle farm_Jeep_2 Asian_ltv at 194 m (1308 2158 237) 1:driver(driver)=Farm_13 2:gunner(gunner)=Farm_14 "
               "3:passenger=Farm_15 4:passenger_left_rear=Farm_16 5:passenger_right_rear=-; ai on, AI driver's ai on; "
               "engine powered=1 starting=0 off=0 disabled=0 run:playing(virtual) vol 1.00 to 200 m\n")
BETWEEN = "[CoopStealth] seen: Nomad 31/11\n" * 8


class TestD15Vehicles(unittest.TestCase):
    def test_hidden_heard(self):
        f = run(d.d15_vehicles, HIDDEN_HEARD)
        self.assertEqual(len(f), 1)
        self.assertEqual((f[0].id, f[0].verdict), ("D15", "FAIL"))
        self.assertEqual(f[0].ru, "слышен гул спрятанного вертолёта Reinforcement_Heli_Fish")

    def test_hidden_virtual_and_visible_are_fine(self):
        self.assertEqual(run(d.d15_vehicles, HIDDEN_VIRTUAL + VISIBLE_HEARD + WATCH_VIRTUAL), [])

    def test_watch_line_heard(self):
        f = run(d.d15_vehicles, WATCH_HEARD + WATCH_HEARD)
        self.assertEqual([(x.verdict, x.count) for x in f], [("FAIL", 2)])

    def test_stuck_helicopter(self):
        f = run(d.d15_vehicles, HELI_HOVER + BETWEEN + HELI_HOVER)
        self.assertEqual(len(f), 1)
        self.assertEqual((f[0].verdict, f[0].key), ("WARN", "stuck:Asian_Helicopter_1"))
        self.assertEqual([l.no for l in f[0].evidence], [10, 1])

    def test_one_listing_or_parked_with_ai_on_is_fine(self):
        self.assertEqual(run(d.d15_vehicles, HELI_HOVER), [])
        self.assertEqual(run(d.d15_vehicles, JEEP_PARKED + BETWEEN + JEEP_PARKED), [])

    def test_listings_too_close_in_time(self):
        a = "<12:00:00> " + HELI_HOVER
        b = "<12:00:20> " + HELI_HOVER
        c = "<12:01:00> " + HELI_HOVER
        self.assertEqual(run(d.d15_vehicles, a + BETWEEN + b), [])
        self.assertEqual(len(run(d.d15_vehicles, a + BETWEEN + b + BETWEEN + c)), 1)

    def test_moved_is_fine(self):
        moved = HELI_HOVER.replace("(1279 1645 281)", "(1939 1801 308)")
        self.assertEqual(run(d.d15_vehicles, HELI_HOVER + BETWEEN + moved), [])


if __name__ == "__main__":
    unittest.main()
