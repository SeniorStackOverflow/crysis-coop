"""D07 (debris and breaks after loads) and D02 ([CoopBug] reports).

Excerpts are real lines from the user's Game.log of 2026-10-15 16:45 and the
goal_runs test logs; where a line shape exists only in the source (Settle's
summary, the planned "kind=" watchdog, "things still break") it is written
from the format string, and the comment says so.
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from coopjudge.context import Context  # noqa: E402
from coopjudge.detectors import d07_physics as d  # noqa: E402
from coopjudge.logread import Log, from_text  # noqa: E402


def run(det, host_text, companion_text=None):
    host = from_text(host_text, "Game.log", "host")
    comp = from_text(companion_text, "companion.log", "companion") if companion_text else None
    return det(Context(host, comp))


# the user's Game.log 162, 408, 417-419 (the first load) and 423 (the hut's debris kills Nomad)
USER_LOAD = """\
[CoopSave] loading coop_checkpoint_56c79107e0d7a6ff_b.CRYSISJMSF (rescue, checkpoint manual)
[CoopSave] the saved game took 1.5 s to load
[CoopSave] progress loaded: rescue, checkpoint manual (saved 2026-10-14 12:36:40)
[Coop] checkpoint loaded: 1 players' places kept
[CoopStealth] seen: Nomad 0/9
"""
USER_HIT = "[Coop] DAMAGE player Nomad by Nomad type=collision hit=485.3 taken=485.3 hp 200->-286\n"
# Game.log 2971: a barrel, minutes after the third load
BARREL = ("[Coop] DAMAGE player Nomad by storage/local/barrels.empty_metal_barrel_d10 type=collision "
          "hit=125.5 taken=125.5 hp 200->74\n")
TICK = "[CoopAI] AI shots so far: 708 (wake mode 1)\n"
POS = "[CoopPos] server: Nomad=(1696,1826,195)hp198\n"

# fr_before1_host.log 179-481, time stamped
TS_LOAD = """\
<10:18:43> [CoopSave] loading coop_checkpoint_56c79107e0d7a6ff_b.CRYSISJMSF (rescue, checkpoint manual)
<10:18:45> [CoopSave] the saved game took 2.7 s to load
<10:18:46> [CoopSave] progress loaded: rescue, checkpoint manual (saved 2026-10-14 12:36:40)
<10:18:46> [Coop] checkpoint loaded: 1 players' places kept
"""
HOUSE_PART = ("[brk] OnCreatePhysEntityPart entity[0008fc10:Objects/Library/Architecture/Village/house_small_b.cgf] "
              "due to joint break part[src=%d,new=0]")
SHELF_PART = ("[brk] OnCreatePhysEntityPart entity[000002a7:furniture/shelves.shelf_wooden_150_150_a1] "
              "due to joint break part[src=%d,new=0]")
TREE_PART = ("[brk] OnCreatePhysEntityPart statobj[pos=(1522.14587402,2429.17553711,217.47969055),"
             "center=(1522.62731934,2430.69628906,218.82331848),vol=0.00000000 due to joint break part[src=%d,new=0")


class CollisionAfterLoad(unittest.TestCase):
    def test_user_log_debris_kills_after_load(self):
        f = run(d.collision_damage, USER_LOAD + USER_HIT)
        self.assertEqual([(x.id, x.verdict) for x in f], [("D07", "FAIL")])
        self.assertIn("485.3", f[0].ru)
        self.assertIn("насмерть", f[0].ru)
        self.assertTrue(f[0].ru.startswith("обломки"))      # "by Nomad": debris with no owner
        self.assertIn("hit=485.3", f[0].en)
        self.assertEqual(f[0].evidence[0].no, 6)        # the hit first
        self.assertEqual(f[0].evidence[1].no, 4)        # then the load's end

    def test_small_hits_and_bullets_do_not_fire(self):
        # Game.log 513-514 (debris, small) and a heavy bullet hit right after a load
        text = USER_LOAD + (
            "[Coop] DAMAGE player Nomad by Nomad type=collision hit=4.5 taken=4.5 hp 200->195\n"
            "[Coop] DAMAGE player Nomad by Nomad type=collision hit=11.1 taken=11.1 hp 195->183\n"
            "[Coop] DAMAGE player Nomad by Farm_13 type=bullet hit=150.0 taken=150.0 hp 200->50\n"
            "[Coop] DAMAGE player Nomad by Nomad type= hit=2.0 taken=2.0 hp 200->198\n")
        self.assertEqual(run(d.collision_damage, text), [])

    def test_strong_hit_without_load_is_warn(self):
        f = run(d.collision_damage, BARREL)
        self.assertEqual([(x.id, x.verdict) for x in f], [("D07", "WARN")])
        self.assertIn("125.5", f[0].ru)
        self.assertIn("barrels.empty_metal_barrel_d10", f[0].ru)   # what hit him, when the line names it

    def test_no_time_stamps_ai_ticks_end_the_window(self):
        # 2 ticks: at most 20 s after the end, inside the 30 s window
        f = run(d.collision_damage, USER_LOAD + TICK * 2 + BARREL)
        self.assertEqual([x.verdict for x in f], ["FAIL"])
        # 4 ticks: 30 s or more after the end (the user's barrel came later still)
        f = run(d.collision_damage, USER_LOAD + TICK * 4 + BARREL)
        self.assertEqual([x.verdict for x in f], ["WARN"])

    def test_no_time_stamps_pos_clock_ends_the_window(self):
        f = run(d.collision_damage, USER_LOAD + POS * 5 + BARREL)
        self.assertEqual([x.verdict for x in f], ["FAIL"])
        f = run(d.collision_damage, USER_LOAD + POS * 7 + BARREL)
        self.assertEqual([x.verdict for x in f], ["WARN"])

    def test_no_time_stamps_a_collapse_has_no_line_cap(self):
        # brk_on1_host.log: the load ends at 479, 469 [brk] lines and two hitches later
        # the player dies (948), the first clock line comes only at 2021
        brk = "".join(HOUSE_PART % i + "\n" for i in range(700))
        f = run(d.collision_damage, USER_LOAD + brk + USER_HIT + POS)
        self.assertEqual([x.verdict for x in f], ["FAIL"])
        # no clock line after the load at all (an old DLL, the log ends): 600 lines
        f = run(d.collision_damage, USER_LOAD + brk + USER_HIT)
        self.assertEqual([x.verdict for x in f], ["WARN"])

    def test_time_stamps(self):
        # the user's hit line, stamped 2 s and 44 s after fr_before1's load end
        f = run(d.collision_damage, TS_LOAD + "<10:18:48> " + USER_HIT)
        self.assertEqual([x.verdict for x in f], ["FAIL"])
        self.assertIn("2 s after", f[0].en)
        f = run(d.collision_damage, TS_LOAD + "<10:19:30> " + USER_HIT)
        self.assertEqual([x.verdict for x in f], ["WARN"])

    def test_smaller_hit_that_kills_after_load(self):
        # fin7_manual_host.log 1146-1149 (after the load ending at 1143): 157 does not kill, 35.4 does
        text = USER_LOAD + (
            "[Coop] DAMAGE player Nomad by Nomad type=collision hit=4.7 taken=4.7 hp 200->195\n"
            "[Coop] DAMAGE player Nomad by Nomad type=collision hit=157.0 taken=157.0 hp 195->37\n"
            "[Coop] DAMAGE player Nomad by Nomad type=collision hit=9.2 taken=9.2 hp 41->31\n"
            "[Coop] DAMAGE player Nomad by Nomad type=collision hit=35.4 taken=35.4 hp 31->-5\n")
        f = run(d.collision_damage, text)
        self.assertEqual([(x.verdict, x.count) for x in f], [("FAIL", 2)])
        self.assertIn("35.4", f[0].evidence[0].text)    # the hit that killed first
        self.assertIn("насмерть", f[0].ru)
        # alone, without the 157: still a FAIL
        f = run(d.collision_damage, USER_LOAD + "[Coop] DAMAGE player Nomad by Nomad type=collision "
                                                "hit=35.4 taken=35.4 hp 31->-5\n")
        self.assertEqual([(x.verdict, x.count) for x in f], [("FAIL", 1)])

    def test_smaller_hit_that_kills_outside_loads_does_not_fire(self):
        # the same 35.4 with no load before it, and one on a body already dead
        self.assertEqual(run(d.collision_damage, "[Coop] DAMAGE player Nomad by Nomad type=collision "
                                                 "hit=35.4 taken=35.4 hp 31->-5\n"), [])
        self.assertEqual(run(d.collision_damage, USER_LOAD + "[Coop] DAMAGE player Nomad by Nomad type=collision "
                                                             "hit=35.4 taken=35.4 hp -5->-40\n"), [])

    def test_many_hits_one_finding(self):
        # adk_old_host.log 739 and 763 after one load: one finding, the strongest first
        text = USER_LOAD + (
            "[Coop] DAMAGE player Companion by Companion type=collision hit=244.7 taken=171.3 hp 200->28\n"
            "[Coop] DAMAGE player Nomad by Nomad type=collision hit=344.8 taken=344.8 hp 200->-145\n")
        f = run(d.collision_damage, text)
        self.assertEqual(len(f), 1)
        self.assertEqual(f[0].count, 2)
        self.assertIn("344.8", f[0].evidence[0].text)
        self.assertIn("Companion, Nomad", f[0].en)


class BreaksAfterLoad(unittest.TestCase):
    def collapse(self, stamp, n, part=HOUSE_PART):
        return "".join("%s %s\n" % (stamp, part % i) for i in range(n))

    def test_hut_collapses_after_load(self):
        text = TS_LOAD + self.collapse("<10:18:47>", 12, SHELF_PART) + self.collapse("<10:18:48>", 20)
        f = run(d.breaks_after_load, text)
        self.assertEqual([(x.id, x.verdict) for x in f], [("D07", "WARN")])
        self.assertIn("32 обломка", f[0].ru)
        self.assertIn("house_small_b.cgf", f[0].ru)

    def test_few_late_or_vegetation_breaks_do_not_fire(self):
        self.assertEqual(run(d.breaks_after_load, TS_LOAD + self.collapse("<10:18:48>", 10)), [])
        self.assertEqual(run(d.breaks_after_load, TS_LOAD + self.collapse("<10:19:06>", 40)), [])
        self.assertEqual(run(d.breaks_after_load, TS_LOAD + self.collapse("<10:18:48>", 40, TREE_PART)), [])
        # breaks without a load (the level's own start) are not about loads
        self.assertEqual(run(d.breaks_after_load, self.collapse("<10:18:48>", 40)), [])

    def test_old_diagnostic_cpe_lines(self):
        # lcv_slot_host.log 15061-15074 and 15174..: no time stamps, a diagnostic DLL's "CPE:" lines
        load = USER_LOAD.replace("1.5 s", "4.8 s")
        house = "CPE: Objects/Library/Architecture/Village/house_small_b.cgf\n"
        shelf = "CPE: furniture/shelves.shelf_wooden_100_150_a13\n"
        f = run(d.breaks_after_load, load + shelf * 8 + house * 14)
        self.assertEqual([x.verdict for x in f], ["WARN"])
        self.assertIn("22 обломка", f[0].ru)
        brush = "CPE: Vegetation/Brush Object\n"
        self.assertEqual(run(d.breaks_after_load, load + brush * 30), [])
        # after the AI clock's 2nd tick (10 s or more): not counted
        self.assertEqual(run(d.breaks_after_load, load + TICK * 2 + house * 30), [])

    def test_things_still_break(self):
        # written from the format in CoopAgent.cpp (HostWorldReady); the corpus only has "stopped breaking"
        f = run(d.breaks_after_load, "[CoopAgent] the host's game is ready 34 s after its player is there "
                                     "(things still break: it waited long enough)\n")
        self.assertEqual([x.verdict for x in f], ["WARN"])
        self.assertEqual(run(d.breaks_after_load, "<11:50:19> [CoopAgent] the host's game is ready 15 s after "
                                                  "its player is there (things stopped breaking)\n"), [])


# adk_new2_host.log 428-431 and 488
FLEW = """\
[CoopBug] after the load  (Default, 194642, 50 kg) flew at 64 m/s at 1697 1824 197: slowed to 20
[CoopBug] after the load  (Default, 194641, 50 kg) flew at 48 m/s at 1697 1825 195: slowed to 20
[CoopBug] after the load  (Default, 129087, 50 kg) flew at 25 m/s at 1698 1824 194: slowed to 20
[CoopBug] after the load  (Default, 129088, 50 kg) flew at 100 m/s at 1699 1823 193: slowed to 20
[CoopBug] after the load a physical thing (-, 0, 35 kg) flew at 100 m/s at 1695 1821 194: slowed to 20
"""


class CoopBug(unittest.TestCase):
    def test_settle_one_finding_for_all_bodies(self):
        # nameless debris and "a physical thing" of one collapse: one bug, not one per name
        f = run(d.coopbug, FLEW)
        self.assertEqual([(x.id, x.verdict, x.key, x.count) for x in f], [("D02", "FAIL", "flew-after-load", 5)])
        self.assertIn("100 м/с", f[0].ru)
        self.assertIn("обломки", f[0].ru)
        self.assertNotIn("Default", f[0].ru)             # the class means nothing to the player
        self.assertIn("Default", f[0].en)
        self.assertIn("безымянный физический объект", f[0].ru)
        self.assertIn("129088", f[0].evidence[0].text)   # the fastest first

    def test_named_models_and_companion_log(self):
        # the engine names some pieces of a break by their model, others not at all (Game.log 477:
        # "house_small_b.cgf (Default, 194502 for 128966)"), so one collapse gives Settle lines with
        # several names: still one finding. (Settle runs on the server; the companion's log is read too.)
        lines = ("[CoopBug] after the load Objects/Library/Architecture/Village/house_small_b.cgf "
                 "(Default, 194502, 1200 kg) flew at 31 m/s at 1695 1820 196: slowed to 20\n"
                 "[CoopBug] after the load furniture/shelves.shelf_wooden_150_150_a1 "
                 "(Default, 194558, 40 kg) flew at 25 m/s at 1695 1821 196: slowed to 20\n")
        f = run(d.coopbug, FLEW[:FLEW.index("\n") + 1], lines)
        self.assertEqual([(x.key, x.verdict, x.count) for x in f], [("flew-after-load", "FAIL", 3)])
        self.assertIn("house_small_b.cgf", f[0].ru)
        self.assertIn("shelves.shelf_wooden_150_150_a1", f[0].ru)

    def test_settle_just_over_its_limit_is_warn(self):
        # written from CoopSave.cpp's format (the corpus has no body under 25 m/s): one body at
        # 22 m/s, a little over Settle()'s 20, is not yet the design's phys_fling (25 m/s after a load)
        line = "[CoopBug] after the load  (Default, 194642, 50 kg) flew at 22 m/s at 1697 1824 197: slowed to 20" + chr(10)
        self.assertEqual([(x.verdict, x.count) for x in run(d.coopbug, line)], [("WARN", 1)])
        # with one real fast piece among them: FAIL
        f = run(d.coopbug, line + FLEW.splitlines()[0] + chr(10))
        self.assertEqual([(x.verdict, x.count) for x in f], [("FAIL", 2)])

    def test_slowed_summary_joins_the_finding(self):
        # Settle()'s summary (CoopSave.cpp, written from the format: the corpus has none) comes
        # only after its [CoopBug] lines: it adds to that finding, it is not a second one
        f = run(d.coopbug, FLEW + "[CoopSave] the 10 s after the load are over: 57 things were slowed down\n")
        self.assertEqual([(x.key, x.count) for x in f], [("flew-after-load", 5)])
        self.assertIn("57 slow-downs", f[0].en)
        self.assertIn("57 things", f[0].evidence[-1].text)
        # alone (its [CoopBug] lines outside the judged lines) it still reports
        f = run(d.coopbug, "[CoopSave] the 10 s after the load are over: 57 things were slowed down\n")
        self.assertEqual([(x.id, x.verdict) for x in f], [("D02", "FAIL")])
        self.assertIn("57", f[0].ru)

    def test_nothing_slowed(self):
        self.assertEqual(run(d.coopbug, "[CoopSave] the 10 s after the load are over: 0 things were slowed down\n"
                                        "[CoopSave] the saved game took 1.5 s to load\n"), [])

    def test_watchdog_kinds(self):
        # the planned "[CoopBug] kind=<k> sev=<n>" format (design section 6)
        text = (
            "[CoopBug] kind=phys_fling sev=3 id=194642 model=house_small_b.cgf mass=50 v=64 rise=9 pos=1697,1824,197 sinceLoad=1.2\n"
            "[CoopBug] kind=phys_fling sev=3 id=194641 model=house_small_b.cgf mass=50 v=48 rise=8 pos=1697,1825,195 sinceLoad=1.4\n"
            "[CoopBug] kind=hidden_noise sev=2 vehicle=River_Helicopter_1 sound=run dist=80\n"
            "[CoopBug] kind=vehicle_stuck sev=1 vehicle=Jungle_Truck_1 still=12\n")
        f = sorted(run(d.coopbug, text), key=lambda x: x.verdict)
        self.assertEqual([(x.verdict, x.key, x.count) for x in f],
                         [("FAIL", "phys_fling:house_small_b.cgf", 2), ("WARN", "hidden_noise:River_Helicopter_1", 1)])

    def test_watchdog_one_key_takes_its_worst_line(self):
        # a sev 2 line first, then a sev 3 line of the same kind and model: one FAIL, not a WARN
        text = ("[CoopBug] kind=phys_fling sev=2 id=194642 model=house_small_b.cgf mass=50 v=30 sinceLoad=0.5\n"
                "[CoopBug] kind=phys_fling sev=3 id=194641 model=house_small_b.cgf mass=50 v=64 sinceLoad=1.4\n")
        f = run(d.coopbug, text)
        self.assertEqual([(x.verdict, x.key, x.count) for x in f], [("FAIL", "phys_fling:house_small_b.cgf", 2)])
        self.assertIn("sev=3", f[0].en)
        self.assertEqual(f[0].evidence[0].no, 2)

    def test_watchdog_summary_is_not_a_bug(self):
        # design section 6: "the rest are counted in kind=summary suppressed=<n>" (no sev)
        self.assertEqual(run(d.coopbug, "[CoopBug] kind=summary suppressed=12\n"
                                        "[CoopBug] kind=summary sev=1 suppressed=3\n"), [])

    def test_lookalikes_do_not_fire(self):
        text = (
            # a test result echoing the host's line (not at the start)
            "host: [CoopBug] after the load  (Default, 194642, 50 kg) flew at 64 m/s at 1697 1824 197: slowed to 20\n"
            # Game.log 164: a load line about the same debris
            "[CoopSave] the load makes objects/natural/trees/jungle_tree_thin/jungle_tree_thin_a.cgf (Default) again "
            "with the id 63399 it had: that slot is kept for a thing of the save now, the piece gets the id 196454\n")
        self.assertEqual(run(d.coopbug, text), [])

    def test_unknown_bug_line_is_fail(self):
        f = run(d.coopbug, "[CoopBug] something new 12 happened\n[CoopBug] something new 13 happened\n")
        self.assertEqual([(x.verdict, x.count) for x in f], [("FAIL", 2)])


USER_GAME_LOG = os.environ.get("COOP_USER_GAME_LOG", os.path.join(
    os.environ.get("TEMP", ""), "claude", "C--GOG-Games-Crysis", "76c1b474-7cf3-4595-a9cc-405a5ae0d577",
    "scratchpad", "live2", "Game.log"))


@unittest.skipUnless(os.path.exists(USER_GAME_LOG), "the user's Game.log of 2026-10-15 16:45 is not here")
class Golden(unittest.TestCase):
    def test_user_game_log(self):
        ctx = Context(Log(USER_GAME_LOG, "host"))
        f = d.collision_damage(ctx)
        fail = [x for x in f if x.verdict == "FAIL"]
        self.assertEqual(len(fail), 1)
        self.assertIn("hit=485.3", fail[0].evidence[0].text)
        self.assertEqual(fail[0].evidence[0].no, 423)
        warn = [x for x in f if x.verdict == "WARN"]
        self.assertEqual([x.evidence[0].no for x in warn], [2971])    # the barrel, minutes later
        # no Settle() lines and no break logging in that build: nothing else of this module
        self.assertEqual(d.coopbug(ctx), [])
        self.assertEqual(d.breaks_after_load(ctx), [])


if __name__ == "__main__":
    unittest.main()
