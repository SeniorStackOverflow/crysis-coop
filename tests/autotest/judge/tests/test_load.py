"""D05 (load integrity), D06 (load stall) and D13 (all-down).

Excerpts are real lines from the user's Game.log of 2026-10-15 16:45 and the
goal_runs test logs (file:line in the comments). Where a line shape is not in
any log (KeepEnd's "kept entities are gone", "the level did not get ready",
the Lua's "the checkpoint was not loaded" and "fell through the ground", a
load over 120 s) it is written from the format string in CoopSave.cpp /
CoopGameRules.lua, and the comment says so.
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from coopjudge.context import Context  # noqa: E402
from coopjudge.detectors import d05_load as d  # noqa: E402
from coopjudge.findings import merge  # noqa: E402
from coopjudge.logread import Log, from_text  # noqa: E402


def run(det, host_text, companion_text=None):
    host = from_text(host_text, "Game.log", "host")
    comp = from_text(companion_text, "companion.log", "companion") if companion_text else None
    return merge(det(Context(host, comp)))


LOAD_START = "[CoopSave] loading coop_checkpoint_56c79107e0d7a6ff_b.CRYSISJMSF (rescue, checkpoint manual)\n"
LOAD_END = """\
[CoopSave] the saved game took 1.1 s to load
[CoopSave] progress loaded: rescue, checkpoint manual (saved 2026-10-14 12:36:40)
[Coop] checkpoint loaded: 1 players' places kept
"""
INPLACE = """\
[CoopSave] continuing campaign "checking": rescue, checkpoint manual (saved 2026-10-14 12:36:40)
[CoopSave] rescue runs already: the checkpoint is loaded into it (no level load)
"""
POS1 = "[CoopPos] server: Nomad=(1697,1825,194)hp200\n"
POS2 = "[CoopPos] server: Nomad=(1543,2115,234)hp-9 Companion=(1536,2116,234)hp-9\n"
TICK = "[CoopAI] AI shots so far: 704 (wake mode 1)\n"

# Game.log 473-491: the second load; three pieces stay bound, the palm is made twice
USER_LOAD2 = LOAD_START + """\
[CoopSave] 392052 stays: bound 1 (now 1), multiplayer 0
[CoopSave] 392347 stays: bound 1 (now 1), multiplayer 0
[CoopSave] 588657 stays: bound 1 (now 1), multiplayer 0
[CoopSave] the save's 9895 entity ids: 17 slots kept for them, 17 made free (held by things the save does not have): Objects/Library/Architecture/Village/house_small_b.cgf (Default, 194502 for 128966)  (Default, 392052 for 326516)  (Default, 392347 for 326811) Objects/Library/Architecture/Village/house_small_b.cgf (Default, 588657 for 523121)
[CoopSave] the load makes objects/natural/trees/jungle_tree_thin/jungle_tree_thin_a.cgf (Default) again with the id 128955 it had: that slot is kept for a thing of the save now, the piece gets the id 327526
[CoopSave] the load makes Objects/Natural/Trees/Palm_Tree/Palm_Tree_Large_e.cgf (Default) again with the id 784316 it had: that slot is objects/natural/trees/jungle_tree_thin/jungle_tree_thin_a.cgf now, the piece gets the id 327465
[CoopSave] the load spawns DSG11618 (DSG1) with its saved id 128935: objects/natural/trees/jungle_tree_thin/jungle_tree_thin_a.cgf (Default, id 63399) held its slot, removed
[CoopSave] the load spawns Objects/Natural/Trees/Palm_Tree/Palm_Tree_Large_e.cgf (Default) with its saved id 784316: objects/natural/trees/jungle_tree_thin/jungle_tree_thin_a.cgf (Default, id 522172) held its slot, removed
[CoopSave] 588657 stays: bound 1 (now 1), multiplayer 0
[CoopSave] the load spawns GrenadeLauncher1542 (GrenadeLauncher) with its saved id 523121: Objects/Library/Architecture/Village/house_small_b.cgf (Default, id 588657) held its slot, could not be removed
""" + LOAD_END

# lc_inplace3_host.log 20701, 25019-25021: the older "could not be removed"
OLD_STAYS = LOAD_START + """\
[CoopSave] the load spawns SniperScope1617 (SniperScope) with its saved id 128966: Objects/Library/Architecture/Village/house_small_b.cgf (Default, id 194502) held its slot, removed
[Warning] Removing entity during system lock : Objects/Library/Architecture/Village/house_small_b.cgf with id 194502
[CoopSave] 194502 could not be removed
""" + LOAD_END

# Game.log 2524-2539, 2572-2594: everybody down, in-place load, the companion kicked
USER_LOAD3 = """\
[Coop] Companion is down
[Coop] everybody is down: back to the last checkpoint in 6 s
[CoopAI] Companion is down: no target for the soldiers
[CoopPos] server: Nomad=(1543,2115,234)hp-9 Companion=(1536,2116,234)hp-9
[CoopSave] looking for the campaigns (this PC and the cloud)...
[Coop] everybody is down: the last checkpoint is loaded
""" + INPLACE + LOAD_START + """\
[CoopSave] 195034 stays: bound 1 (now 1), multiplayer 0
[net 13:54:55.987] Disconnect host-pc:64822; profid=0; cause=4; msg=''
[CoopSave] the saved game took 1.2 s to load
[CoopSave] progress loaded: rescue, checkpoint manual (saved 2026-10-14 12:36:40)
[Coop] checkpoint loaded: 1 players' places kept
"""

# adk_off2_host.log 405-451, 612-657: entities kept 1419 -> 2492
ADK_OFF2 = (POS2 + "[CoopSave] rescue runs already: the checkpoint is loaded into it (no level load)\n" + LOAD_START
            + "[CoopSave] 1 friend(s) stay in the game through the load (1419 entities kept)\n" + LOAD_END
            + POS2 + "[CoopSave] rescue runs already: the checkpoint is loaded into it (no level load)\n" + LOAD_START
            + "[CoopSave] 1 friend(s) stay in the game through the load (2492 entities kept)\n" + LOAD_END)
# adk_off2_host.log 441: the first of those loads re-armed 205 soldiers
REARMED_205 = ("[CoopSave] after the load 205 soldiers did not have their guns as saved, their equipment again: "
               "Forest_Wave_2 (his AISmokeGrenades belongs to 0; now holstered)\n")
# convoy2_host.log 769-779: 1322 -> 1332, as normal
CONVOY2 = ADK_OFF2.replace("(1419 entities kept)", "(1322 entities kept)").replace("(2492 entities kept)", "(1332 entities kept)")

# brk_off1_host.log 181, 267-291 (time stamps): a load that ends, and its CoopPos lines after
TIMED_START = "<04:25:14> [CoopSave] loading coop_checkpoint_56c79107e0d7a6ff_b.CRYSISJMSF (rescue, checkpoint manual)\n"
TIMED_END = """\
<04:25:17> [CoopSave] the saved game took 2.4 s to load
<04:25:17> [CoopSave] progress loaded: rescue, checkpoint manual (saved 2026-10-14 12:36:40)
<04:25:17> [Coop] checkpoint loaded: 1 players' places kept
"""
TIMED_AFTER = """\
<04:25:22> [CoopPos] server: Nomad=(1697,1825,194)hp200
<04:25:52> [CoopPos] server: Nomad=(1697,1825,194)hp200
<04:26:16> [CoopPerf] last minute: 46 fps on average, worst frame 1088 ms, 4 frames over 100 ms; sound: 13 voices, cpu 0.0, memory 20.5 of 17.6 MB
<04:26:17> [CoopPos] server: Nomad=(1697,1825,194)hp200 Companion=(1698,1827,194)hp200
"""


class D05Leftovers(unittest.TestCase):
    def test_user_log_588657_stays_bound(self):
        fs = run(d.d05_leftovers, USER_LOAD2)
        self.assertEqual([(f.id, f.verdict) for f in fs], [("D05", "FAIL")])
        f = fs[0]
        self.assertIn("588657", f.en)
        self.assertIn("588657 stays: bound", f.evidence[0].text)      # the house piece first
        self.assertIn("could not be removed", " ".join(l.text for l in f.evidence))
        self.assertEqual(f.count, 3)

    def test_older_shape(self):
        fs = run(d.d05_leftovers, OLD_STAYS)
        self.assertEqual(len(fs), 1)
        self.assertIn("194502", fs[0].en)

    def test_removed_is_fine(self):
        text = LOAD_START + USER_LOAD2.splitlines()[5] + "\n" + USER_LOAD2.splitlines()[8] + "\n" + LOAD_END
        self.assertEqual(run(d.d05_leftovers, text), [])


class D05MadeTwice(unittest.TestCase):
    def test_palm_made_twice(self):
        fs = run(d.d05_made_twice, USER_LOAD2)
        self.assertEqual(len(fs), 1)
        self.assertEqual(fs[0].verdict, "FAIL")
        self.assertIn("Palm_Tree_Large_e.cgf", fs[0].en)
        self.assertIn("784316", fs[0].en)
        self.assertIn("again with the id 784316", fs[0].evidence[0].text)

    def test_new_id_only(self):
        # Game.log 478, 480: the tree piece gets a new id; the save spawns a rifle there
        lines = USER_LOAD2.splitlines()
        text = LOAD_START + lines[5] + "\n" + lines[7] + "\n" + LOAD_END
        self.assertEqual(run(d.d05_made_twice, text), [])


class D05KeptGone(unittest.TestCase):
    def test_gone(self):
        # format string of KeepEnd (CoopSave.cpp:1026); no log has it yet
        text = INPLACE + LOAD_START + "[CoopSave] 1 friend(s) stay in the game through the load (1419 entities kept)\n" \
            "[CoopSave] 3 kept entities are gone after the load\n" + LOAD_END
        fs = run(d.d05_kept_gone, text)
        self.assertEqual([(f.verdict, f.key) for f in fs], [("FAIL", "kept_gone")])

    def test_kept_is_fine(self):
        self.assertEqual(run(d.d05_kept_gone, ADK_OFF2), [])


class D05AIAfresh(unittest.TestCase):
    def test_another_aisystem_fails(self):
        # format string (CoopSave.cpp:1925) with its other reason
        text = LOAD_START + "[CoopSave] a checkpoint of a campaign with 2 players: the AI starts afresh " \
            "(the AI's perception could not be checked during the load: another CryAISystem.dll)\n" + LOAD_END
        fs = run(d.d05_ai_afresh, text)
        self.assertEqual([f.verdict for f in fs], ["FAIL"])

    def test_set_or_old_build_warns(self):
        # goal_runs: coop_load_ai_reset 2 runs and the builds before the perception fix
        text = "[CoopSave] a checkpoint of a campaign with 2 players: the AI starts afresh (coop_load_ai_reset 2)\n" \
               "[CoopSave] a checkpoint of a campaign with 2 players: the AI starts afresh (it crashed otherwise)\n"
        self.assertEqual(sorted(f.verdict for f in run(d.d05_ai_afresh, text)), ["WARN", "WARN"])

    def test_goes_on_as_saved(self):
        text = "[CoopSave] the AI goes on as saved: 13 of its perceived targets were not in the game, left out\n"
        self.assertEqual(run(d.d05_ai_afresh, text), [])


class D05WrongIds(unittest.TestCase):
    def test_wrong_id(self):
        # fin_manual_host.log 24556-24559 and its first "Expected" line
        text = LOAD_START + """\
[CoopSave] the load spawns LAWScope1614 (LAWScope) with its saved id 194491:  (Default, id 260027) held its slot, could not be removed
[Warning] Entity with id=260027,  (Default) already spawned on this client...override
[Warning] [LoadGame] Spawned Entity ID=194491, LAWScope1614 (LAWScope), with wrong ID=260027
[Warning] [LoadGame] Expected Entity ID=194491, Objects/Natural/Trees/Palm_Tree/Palm_Tree_Large_e.cgf (Default), Loaded ID=784316
""" + LOAD_END
        fs = run(d.d05_wrong_ids, text)
        self.assertEqual(len(fs), 1)
        self.assertEqual(fs[0].verdict, "FAIL")
        self.assertIn("with wrong ID=260027", fs[0].evidence[0].text)

    def test_other_loadgame_lines(self):
        # lc_inplace2_host.log; fin_manual_result.txt
        text = LOAD_START + """\
[Warning] [LoadGame] Missing Entity with ID=194629, probably was removed during Serialization.
[Warning] [CryAction] LoadGame: 'coop_checkpoint_56c79107e0d7a6ff_b.CRYSISJMSF' done. [Duration=5.4184 secs]
""" + LOAD_END
        self.assertEqual(run(d.d05_wrong_ids, text), [])

    def test_class_mismatch_and_failed_spawn(self):
        # CryAction.dll's format strings ("[LoadGame] Entity ID=%d '%s', class
        # mismatch, should be '%s'", "[LoadGame] Failed Spawning Entity ID=%d,
        # %s (%s)"); no log has them yet
        text = LOAD_START + """\
[Warning] [LoadGame] Entity ID=63399 'DSG11618', class mismatch, should be 'DSG1'
[Warning] [LoadGame] Failed Spawning Entity ID=128935, DSG11618 (DSG1)
""" + LOAD_END
        fs = run(d.d05_wrong_ids, text)
        self.assertEqual(sorted((f.verdict, f.key) for f in fs), [("FAIL", "wrong_ids"), ("WARN", "entity_failed")])
        self.assertIn("1 class mismatches", [f for f in fs if f.key == "wrong_ids"][0].en)


class D05KeepMissing(unittest.TestCase):
    def test_user_companion_not_kept(self):
        fs = run(d.d05_keep_missing, USER_LOAD3)
        self.assertEqual(len(fs), 1)
        f = fs[0]
        self.assertEqual(f.verdict, "FAIL")
        self.assertIn("loading coop_checkpoint", f.evidence[0].text)
        self.assertIn("cause=4", f.en)

    def test_no_friend_but_one_was_there(self):
        # adk_new2_host.log 460-462, with the companion of USER_LOAD3 in the game
        text = POS2 + INPLACE + LOAD_START + "[CoopSave] no friend in the game to keep through the load\n" + LOAD_END
        fs = run(d.d05_keep_missing, text)
        self.assertEqual(len(fs), 1)
        self.assertIn("no friend in the game to keep", fs[0].en)

    def test_kept_or_alone(self):
        self.assertEqual(run(d.d05_keep_missing, ADK_OFF2), [])
        # adk_new2_host.log 453-462: the companion never joined
        text = POS1 + "[Coop] everybody is down: the last checkpoint is loaded\n" + INPLACE + LOAD_START \
            + "[CoopSave] no friend in the game to keep through the load\n" + LOAD_END
        self.assertEqual(run(d.d05_keep_missing, text), [])
        # vdbg1_host.log 782-792: the older "kept" line
        text = POS2 + INPLACE + LOAD_START \
            + "[CoopSave] 1 friend(s) stay in the game through the load (87 entities kept, 5 of the host's things made anew)\n" + LOAD_END
        self.assertEqual(run(d.d05_keep_missing, text), [])
        # a level load (not in place) with the companion in
        self.assertEqual(run(d.d05_keep_missing, POS2 + LOAD_START + LOAD_END), [])

    def test_kept_and_kicked_all_the_same(self):
        # USER_LOAD3 with adk_off2_host.log 413's keep line: no log has both
        text = USER_LOAD3.replace(LOAD_START, LOAD_START
                                  + "[CoopSave] 1 friend(s) stay in the game through the load (1419 entities kept)\n")
        fs = run(d.d05_keep_missing, text)
        self.assertEqual([(f.verdict, f.key) for f in fs], [("FAIL", "keep_broken")])
        self.assertIn("cause=4", fs[0].en)
        self.assertIn("friend(s) stay", fs[0].evidence[0].text)

    def test_joining_player_is_not_in_the_game(self):
        # ab_gate_host.log 340, 363: [CoopPos] shows the joining player at
        # 0,0,0, the AI tick counts him as a player
        text = "[CoopPos] server: Nomad=(1697,1825,194)hp200 Nomad(1)=(0,0,0)hp-1\n" \
            "[CoopAI] tick: scriptedAI=273 withAIObject=273 enabled=3 moving=75 alerted=39 targetingPlayer=12 players=2 playersWithAI=2\n" \
            + INPLACE + LOAD_START + "[CoopSave] no friend in the game to keep through the load\n" + LOAD_END
        self.assertEqual(run(d.d05_keep_missing, text), [])


class D05KeptGrowth(unittest.TestCase):
    def test_adk_off2(self):
        fs = run(d.d05_kept_growth, ADK_OFF2)
        self.assertEqual(len(fs), 1)
        self.assertEqual(fs[0].verdict, "WARN")
        self.assertIn("1419 -> 2492", fs[0].en)

    def test_after_a_rearm_names_it(self):
        # the corpus jumps follow a load that re-armed 205 soldiers: not debris
        text = ADK_OFF2.replace(LOAD_END, REARMED_205 + LOAD_END, 1)
        fs = run(d.d05_kept_growth, text)
        self.assertEqual(len(fs), 1)
        self.assertIn("re-armed 205 soldiers", fs[0].en)
        self.assertNotIn("обломки", fs[0].ru)

    def test_convoy2_normal(self):
        self.assertEqual(run(d.d05_kept_growth, CONVOY2), [])


class D05Rearm(unittest.TestCase):
    # lc_manual_host.log 162-224
    CHECK = """\
[CoopTest] cmdfile: coop_test_rearm check
[CoopSave] 219 soldiers do not have their guns as saved: Forest_Wave_2 (his AISmokeGrenades belongs to 0) Forest_Fish_5 (his AISmokeGrenades belongs to 0)
[CoopTest] coop_test_rearm check: 219 soldiers not armed as saved
"""

    def test_unarmed_after_load(self):
        fs = run(d.d05_rearm, LOAD_START + LOAD_END + self.CHECK)
        self.assertEqual([(f.verdict, f.key) for f in fs], [("FAIL", "unarmed")])
        self.assertIn("219", fs[0].en)

    def test_armed_or_before_rearm(self):
        # fin_manual_result.txt: 0 after the load; a trace line from inside the load; a check before any load
        text = LOAD_START + "[CoopTest] right after LoadGame: 219 soldiers not armed\n" + LOAD_END \
            + "[CoopTest] coop_test_rearm check: 0 soldiers not armed as saved\n"
        self.assertEqual(run(d.d05_rearm, text), [])
        self.assertEqual(run(d.d05_rearm, self.CHECK), [])

    def test_rearmed_by_the_load_warns(self):
        # Game.log 509
        text = LOAD_START + "[CoopSave] the saved game took 1.1 s to load\n" \
            "[CoopSave] after the load 136 soldiers did not have their guns as saved, their equipment again: " \
            "Forest_Wave_2 (his AISmokeGrenades belongs to 0; now holstered)\n" + LOAD_END
        fs = run(d.d05_rearm, text)
        self.assertEqual([(f.verdict, f.key) for f in fs], [("WARN", "rearmed")])

    def test_ids_not_looked_after_warns(self):
        # adk_new_host.log 411; the others from CoopSave.cpp's format strings
        # (FreeSavedIdSlot, EntitySystemKnown, KeepEnd, LoadNow)
        text = LOAD_START + """\
[CoopSave] a player or what one carries holds the slot of a saved id: Companion (129479, the save's 63943): this load keeps them and does not get the save's ids right (the soldiers are armed again after it)
[CoopSave] the load spawns SOCOM1353 (SOCOM) with its saved id 63548: Companion, a player or what one carries, holds its slot and stays
[CoopSave] this CryEntitySystem.dll is not the one known: the save's entity ids are not looked after
[CoopSave] the player limit call could not be put back
""" + LOAD_END
        fs = run(d.d05_rearm, text)
        self.assertEqual(sorted((f.verdict, f.key, f.count) for f in fs),
                         [("WARN", "ids_unread", 1), ("WARN", "patch_left", 1), ("WARN", "slot_protected", 2)])

    def test_few_rearmed_or_ids_kept(self):
        # convoy_fix2_host.log 1760 (6 soldiers); fin_convoy_prev_host.log 1590's "made free" line
        text = LOAD_START + "[CoopSave] after the load 6 soldiers did not have their guns as saved, their equipment again: " \
            "Jungle_Convoy_7 (his AISmokeGrenades belongs to 0; now holstered)\n" \
            "[CoopSave] the save's 9814 entity ids: 29 slots kept for them, 1 made free (held by things the save does not have): " \
            "Jungle_Convoy_Truck_DetachedPart_door1 (VehiclePartDetached, 129502 for 63966)\n" + LOAD_END
        self.assertEqual(run(d.d05_rearm, text), [])


class D06Stall(unittest.TestCase):
    def test_no_end_timed(self):
        fs = run(d.d06_stall, TIMED_START + TIMED_AFTER)
        self.assertEqual([(f.verdict, f.key) for f in fs], [("FAIL", "no_end")])

    def test_no_end_next_load(self):
        fs = run(d.d06_stall, LOAD_START + POS1 + LOAD_START + LOAD_END)
        self.assertEqual([(f.verdict, f.key) for f in fs], [("FAIL", "no_end")])

    def test_log_ends_soon(self):
        # fin4_manual_host.log 163-167: the log ends 4 lines into the load
        text = LOAD_START + """\
[CoopSave] the save's 9895 entity ids: 206 slots kept for them, 107 made free (held by things the save does not have): BirdBoid (Boid, 63534 for 129070)
[CoopSave] the load makes objects/natural/trees/jungle_tree_thin/jungle_tree_thin_a.cgf (Default) again with the id 63399 it had: that slot is kept for a thing of the save now, the piece gets the id 196454
"""
        fs = run(d.d06_stall, text)
        self.assertEqual([(f.verdict, f.key) for f in fs], [("WARN", "log_ends")])

    def test_ended(self):
        self.assertEqual(run(d.d06_stall, TIMED_START + TIMED_END + TIMED_AFTER), [])
        self.assertEqual(run(d.d06_stall, USER_LOAD2), [])

    def test_crash_is_d01(self):
        text = LOAD_START + "================================ CRASH DETECTED ================================\n"
        self.assertEqual(run(d.d06_stall, text), [])


class D06TookAndFailed(unittest.TestCase):
    def test_took_over_120(self):
        # Game.log 491's line with another number
        text = LOAD_START + LOAD_END.replace("took 1.1 s", "took 130.4 s")
        fs = run(d.d06_took, text)
        self.assertEqual([(f.verdict, f.key) for f in fs], [("FAIL", "took")])
        self.assertEqual(run(d.d06_took, LOAD_START + LOAD_END.replace("took 1.1 s", "took 6.5 s")), [])

    def test_not_ready(self):
        # format string (CoopSave.cpp:2360)
        fs = run(d.d06_failed, "[CoopSave] the level did not get ready: the saved game is not loaded\n")
        self.assertEqual([(f.verdict, f.key) for f in fs], [("FAIL", "not_ready")])
        self.assertEqual(run(d.d06_failed, INPLACE + LOAD_START + LOAD_END), [])


class D13AllDown(unittest.TestCase):
    ALLDOWN = "[Coop] everybody is down: back to the last checkpoint in 6 s\n"

    def test_loaded(self):
        self.assertEqual(run(d.d13_alldown, USER_LOAD3), [])

    def test_no_load_untimed(self):
        # Game.log 2525-2536 without the load: 16 [CoopPos] lines = 80 s
        text = self.ALLDOWN + "[Coop] everybody is down: the last checkpoint is loaded\n" + (POS2 + TICK) * 16
        fs = run(d.d13_alldown, text)
        self.assertEqual([(f.verdict, f.key) for f in fs], [("FAIL", "alldown_no_load")])
        self.assertIn("asked", fs[0].en)

    def test_no_load_timed(self):
        text = "<04:25:14> " + self.ALLDOWN + TIMED_AFTER \
            + "<04:26:37> [CoopPos] server: Nomad=(1697,1825,194)hp200 Companion=(1698,1827,194)hp200\n"
        fs = run(d.d13_alldown, text)
        self.assertEqual([(f.verdict, f.key) for f in fs], [("FAIL", "alldown_no_load")])

    def test_log_ends(self):
        fs = run(d.d13_alldown, self.ALLDOWN + POS2 + TICK)
        self.assertEqual([(f.verdict, f.key) for f in fs], [("WARN", "alldown_log_ends")])

    def test_revived(self):
        # goal_runs: "[Coop] Companion revived Nomad" ends it without a load
        text = self.ALLDOWN + "[Coop] Companion revived Nomad\n" + (POS2 + TICK) * 16
        self.assertEqual(run(d.d13_alldown, text), [])

    # alldown_host.log 525-634 with time stamps (log_IncludeTime): the
    # checkpoint's level loads first (goal_runs level loads: 20-220 s)
    LEVEL_LOAD = """\
<10:00:00> [Coop] everybody is down: back to the last checkpoint in 6 s
<10:00:06> [Coop] everybody is down: the last checkpoint is loaded
<10:00:06> [CoopSave] continuing campaign "AllDown Test": rescue, checkpoint rescue_start (saved 2026-10-12 19:19:35)
<10:00:07> [net 16:22:21.138] Disconnect host-pc:58282; profid=0; cause=13; msg='Nub destroyed'
<10:00:08> [CoopAI] level loading: enabling AI system on the server
<10:01:50> [CoopVeh] River_Helicopter_1_Gunner entered vehicle River_Helicopter_1 seat gunner (driver=0)
"""

    def test_level_load_first(self):
        text = self.LEVEL_LOAD + "<10:01:55> " + LOAD_START
        self.assertEqual(run(d.d13_alldown, text), [])

    def test_level_load_never_loads(self):
        text = self.LEVEL_LOAD + "<10:05:50> [CoopPos] server: Nomad=(1697,1825,194)hp200\n" \
            "<10:06:20> [CoopPos] server: Nomad=(1697,1825,194)hp200\n"
        fs = run(d.d13_alldown, text)
        self.assertEqual([(f.verdict, f.key) for f in fs], [("FAIL", "alldown_no_load")])
        self.assertIn("level load began", fs[0].en)

    def test_not_loaded_and_fell(self):
        # format strings (CoopGameRules.lua 1737, 1837)
        text = "[Coop] the checkpoint was not loaded: the team comes back without it\n" \
               "[Coop] Companion fell through the ground: put back next to Nomad\n"
        fs = run(d.d13_alldown, text)
        self.assertEqual(sorted((f.verdict, f.key) for f in fs), [("FAIL", "not_loaded"), ("WARN", "fell")])
        # Game.log: a player back at the loaded checkpoint is not a fall
        self.assertEqual(run(d.d13_alldown, "[Coop] Companion is back at the loaded checkpoint (next to the host)\n"), [])


LIVE = r"C:\Users\andre\AppData\Local\Temp\claude\C--GOG-Games-Crysis\76c1b474-7cf3-4595-a9cc-405a5ae0d577\scratchpad\live2\Game.log"


@unittest.skipUnless(os.path.exists(LIVE), "the user's Game.log of 2026-10-15 16:45 is not here")
class Golden(unittest.TestCase):
    def test_user_game_log(self):
        ctx = Context(Log(LIVE, "host"))
        fs = merge([f for det in d.DETECTORS for f in det(ctx)])
        keys = {(f.id, f.verdict, f.key.split(":")[0]) for f in fs}
        self.assertIn(("D05", "FAIL", "leftover"), keys)
        self.assertIn(("D05", "FAIL", "made_twice"), keys)
        self.assertIn(("D05", "FAIL", "keep_missing"), keys)
        left = [f for f in fs if f.key == "leftover"][0]
        self.assertIn("588657", left.en)
        self.assertFalse([f for f in fs if f.id in ("D06", "D13")])


if __name__ == "__main__":
    unittest.main()
