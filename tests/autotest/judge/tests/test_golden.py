"""Golden runs: whole real logs with a known verdict.

The logs are private (the user's own game and old test runs on this PC), not
in the repo: every test skips when its files are missing. Other places:
  COOPJUDGE_LIVE2      folder with the user's Game.log + companion.log (2026-10-15 16:45)
  COOPJUDGE_GOAL_RUNS  folder with <name>_host.log / _companion.log / _result.txt

Every expectation below was checked by hand against the log lines first
(file:line in the comments).
"""

import functools
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from coopjudge.judge import judge  # noqa: E402
from coopjudge.logread import Log  # noqa: E402

_TEMP = r"C:\Users\andre\AppData\Local\Temp\claude\C--GOG-Games-Crysis"
LIVE2 = os.environ.get("COOPJUDGE_LIVE2") or os.path.join(
    _TEMP, "76c1b474-7cf3-4595-a9cc-405a5ae0d577", "scratchpad", "live2")
GOAL_RUNS = os.environ.get("COOPJUDGE_GOAL_RUNS") or os.path.join(
    _TEMP, "544db6be-39b7-454f-8c35-8a102411df82", "scratchpad", "coop_test", "goal_runs")

USER_HOST = os.path.join(LIVE2, "Game.log")
USER_COMP = os.path.join(LIVE2, "companion.log")


def run_path(name, kind="host"):
    return os.path.join(GOAL_RUNS, "%s_%s.log" % (name, kind))


def have_runs(*names):
    return all(os.path.exists(run_path(n)) for n in names)


@functools.lru_cache(maxsize=None)
def judged(host_path, comp_path=None):
    host = Log(host_path, "host")
    comp = Log(comp_path, "companion") if comp_path and os.path.exists(comp_path) else None
    return judge(host, comp)


def judged_run(name):
    """A goal run with its companion's log when there is one."""
    comp = run_path(name, "companion")
    return judged(run_path(name), comp if os.path.exists(comp) else None)


def show(findings):
    return "\n".join("  %s %s x%d %s | %s" % (f.id, f.verdict, f.count, f.en[:200],
                                              f.evidence[0].where() if f.evidence else "-")
                     for f in findings) or "  (none)"


def found(findings, id, verdict=None, text=None):
    """The findings with this id (and verdict) that have `text` in an evidence line."""
    out = []
    for f in findings:
        if f.id != id or (verdict and f.verdict != verdict):
            continue
        if text is None or any(text in l.text for l in f.evidence):
            out.append(f)
    return out


class GoldenCase(unittest.TestCase):
    def assertFinding(self, findings, id, verdict, text=None):
        hits = found(findings, id, verdict, text)
        self.assertTrue(hits, "no %s %s%s in:\n%s" % (id, verdict, (" with %r" % text) if text else "", show(findings)))
        return hits[0]

    def assertNoBad(self, findings, bad=("CRASH", "FAIL")):
        badf = [f for f in findings if f.verdict in bad]
        self.assertFalse(badf, "expected no %s, got:\n%s" % ("/".join(bad), show(badf)))

    def assertDetectorsRan(self, findings):
        # D00: a detector raised; it must not hide behind INFRA
        self.assertFalse([f for f in findings if f.id == "D00"], show(findings))


@unittest.skipUnless(os.path.exists(USER_HOST) and os.path.exists(USER_COMP), "the user's Game.log of 2026-10-15 is not here")
class UserGame(GoldenCase):
    """The user's own game, 2026-10-15 16:45, rescue: three checkpoint loads
    (one level load, two in place); the AI companion is kicked by the second
    in-place load, cannot come back, and its restarted game hangs joining."""

    @classmethod
    def setUpClass(cls):
        cls.ctx, cls.f = judged(USER_HOST, USER_COMP)

    def test_detectors_ran(self):
        self.assertDetectorsRan(self.f)
        self.assertFalse(found(self.f, "D01"), show(self.f))

    def test_d03_kicked_by_inplace_load(self):
        # Game.log 2539 loading (in place, after 2538 "runs already"),
        # 2572 Disconnect host-pc:64822; cause=4 (server full),
        # 2573 the saved game took 1.2 s: the kick is inside the load
        f = self.assertFinding(self.f, "D03", "FAIL", "cause=4")
        kick = f.evidence[0]
        self.assertIn("cause=4", kick.text)
        ld = [l for l in self.ctx.ev.loads
              if (l.took_line or l.end) is not None and l.start.no < kick.no <= (l.took_line or l.end).no]
        self.assertEqual(len(ld), 1, "cause=4 at %s is not inside one load" % kick.where())
        self.assertTrue(ld[0].inplace)
        self.assertIsNone(ld[0].friends_kept)

    def test_d04_companion_lost(self):
        # Game.log 2988 [CoopAgent] told the host: AI companion cannot join your game
        self.assertFinding(self.f, "D04", "FAIL", "AI companion cannot join")

    def test_d05_588657_stays_bound(self):
        # Game.log 476 [CoopSave] 588657 stays: bound 1 (now 1), multiplayer 0
        f = self.assertFinding(self.f, "D05", "FAIL", "588657 stays: bound")
        self.assertIn("588657", f.ru)

    def test_d05_palm_made_twice(self):
        # Game.log 479 + 481: the palm is made again as 327465 and spawned from the save as 784316
        f = self.assertFinding(self.f, "D05", "FAIL", "Palm_Tree_Large_e.cgf")
        self.assertIn("Palm_Tree_Large_e", f.ru)

    def test_d07_debris_after_load(self):
        # Game.log 418 checkpoint loaded, 423 DAMAGE player Nomad by Nomad type=collision hit=485.3 ... hp 200->-286
        f = self.assertFinding(self.f, "D07", "FAIL", "hit=485.3")
        self.assertIn("485.3", f.ru)
        ends = [l.end.no for l in self.ctx.ev.loads if l.end]
        self.assertIn(418, ends)

    def test_d16_restarted_companion_hangs_joining(self):
        # companion.log (the companion restarted at Game.log 3001): 162 its level
        # loads, 14 "303456E0 times out", no "its level loaded" before the host
        # quit at 13:59:54 (cause=13), about 119 s
        f = self.assertFinding(self.f, "D16", "FAIL", "its level loads")
        self.assertEqual(f.evidence[0].src.name, "companion.log")

    def test_barrel_hit_away_from_load_is_only_a_note(self):
        # Game.log 2971 hit=125.5 by a barrel, minutes after the last load: WARN, not FAIL
        self.assertFalse(found(self.f, "D07", "FAIL", "hit=125.5"), show(self.f))
        self.assertFinding(self.f, "D07", "WARN", "hit=125.5")


@unittest.skipUnless(have_runs("adk_off"), "goal_runs/adk_off is not here")
class AdkOff(GoldenCase):
    def test_d01_crash_in_coop_dll(self):
        # adk_off_host.log 136 CRASH DETECTED, 138 Access violation at 0x6AD8A761,
        # callstack 6AD8A761 Coop.dll; the result says the companion EXITED
        ctx, f = judged_run("adk_off")
        hit = self.assertFinding(f, "D01", "CRASH")
        self.assertIn("Coop.dll", hit.en)
        self.assertTrue(any("Access violation" in l.text for l in hit.evidence), show(f))


@unittest.skipUnless(have_runs("adk_off2") and os.path.exists(run_path("adk_off2", "companion")),
                     "goal_runs/adk_off2 is not here")
class AdkOff2(GoldenCase):
    def test_companion_kept_through_two_inplace_loads(self):
        # adk_off2_host.log 413, 618: 1 friend(s) stay in the game through the load
        # (1419 / 2492 entities kept); the result shows 0 disconnects in both rounds
        ctx, f = judged_run("adk_off2")
        self.assertDetectorsRan(f)
        inplace = [l for l in ctx.ev.loads if l.inplace]
        self.assertEqual(len(inplace), 2)
        self.assertEqual([l.friends_kept for l in inplace], [1, 1])
        self.assertNoBad(f)
        self.assertFalse(found(f, "D03") + found(f, "D04") + found(f, "D16"), show(f))


# runs whose result shows the thing working and no crash
CLEAN = [
    # vehicle: the companion gets in and out of the 4wd twice, the host jumps out
    ("vehexit_new", "vehicle"),
    # revive: the host down 181 m from the companion, all down, in-place load
    # keeps the companion (1 friend, 1392 entities), revived, 3 m apart after
    ("revfar2_new", "revive"),
    # sound: 98 loops + a burst with the unstick fix on: nothing came late
    ("quiet_on", "sound"),
    # music: riverbattle drums -> incidental -> middle -> action at the camp
    ("musiccoop2", "music"),
    # join: the companion joins after Continue (join1_host.log 356), 283 AI hits
    ("join1", "join"),
]


@unittest.skipUnless(have_runs(*[n for n, _ in CLEAN]), "goal_runs clean runs are not here")
class CleanRuns(GoldenCase):
    def test_no_fail_or_crash(self):
        for name, kind in CLEAN:
            with self.subTest(run=name, kind=kind):
                ctx, f = judged_run(name)
                self.assertDetectorsRan(f)
                self.assertNoBad(f)
                self.assertTrue(ctx.ev.joins, "%s: the companion never came in" % name)

    def test_revive_load_kept_the_companion(self):
        ctx, f = judged_run("revfar2_new")
        self.assertEqual([(l.inplace, l.friends_kept) for l in ctx.ev.loads], [(True, 1)])


# runs with a known real bug and the detector that must name it
@unittest.skipUnless(have_runs("inplace1", "fin2_manual", "quiet_off", "join2"), "goal_runs bug runs are not here")
class BugRuns(GoldenCase):
    def test_inplace1_companion_kicked_by_load(self):
        # before friends were kept through a load: inplace1_host.log 409 loading
        # (in place), 612 Disconnect ...; cause=4, 613 took 2.0 s; the result:
        # "companion back: ... renamed to Companion" (it had to join again)
        ctx, f = judged_run("inplace1")
        hit = self.assertFinding(f, "D03", "FAIL", "cause=4")
        self.assertEqual(hit.evidence[0].no, 612)

    def test_fin2_manual_debris_kills_after_load(self):
        # fin2_manual_host.log 424 checkpoint loaded, 432 DAMAGE player Nomad
        # by Nomad type=collision hit=983.8 ... hp 200->-784
        ctx, f = judged_run("fin2_manual")
        hit = self.assertFinding(f, "D07", "FAIL", "hit=983.8")
        self.assertEqual(hit.evidence[0].no, 432)

    def test_quiet_off_sounds_late(self):
        # the sound fix turned off: quiet_off_host.log 667 "9 came late (0.5 s
        # or more, the latest 16.4 s)"; quiet_on (fix on) has none (CleanRuns)
        ctx, f = judged_run("quiet_off")
        self.assertFinding(f, "D09", "FAIL", "came late")
        ctx, f = judged_run("quiet_on")
        self.assertFalse(found(f, "D09"), show(f))

    def test_join2_companion_never_got_in(self):
        # join2_result.txt "companion joined: TIMEOUT"; join2_host.log 412 cannot
        # join; join2_companion.log 185 its level loads, then 300 s without
        # its player in the game
        ctx, f = judged_run("join2")
        self.assertFinding(f, "D04", "FAIL", "AI companion cannot join")
        hit = self.assertFinding(f, "D16", "FAIL")
        self.assertEqual(hit.evidence[0].where(), "join2_companion.log:185")


if __name__ == "__main__":
    unittest.main()
