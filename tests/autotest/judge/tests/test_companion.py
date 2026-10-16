"""D03 (unexpected disconnects), D04 (the companion lost from the game) and
D16 (the companion's join stalls).

Excerpts are real lines (file:line in the comments) from the user's Game.log
and companion.log of 2026-10-15 16:45 and the goal_runs test logs, shortened.
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from coopjudge.context import Context  # noqa: E402
from coopjudge.detectors import d03_companion as d  # noqa: E402
from coopjudge.findings import merge  # noqa: E402
from coopjudge.logread import from_text  # noqa: E402


def run(det, host_text, companion_text=None):
    host = from_text(host_text, "Game.log", "host")
    comp = from_text(companion_text, "companion.log", "companion") if companion_text else None
    return merge(det(Context(host, comp)))


def keys(found):
    return sorted((f.id, f.verdict, f.key) for f in found)


# the user's Game.log: the companion is in, everybody is down, the second
# in-place load kicks it (2531, 2538-2539, 2572-2573, 2594, 2619)
KICK_INPLACE = r"""*LOADING: Level multiplayer/tia/coop_rescue loading time: 31.04 seconds
[CoopAgent] AI companion started (process 19064, joins 127.0.0.1:64087, agents on 127.0.0.1:47810) in 78 ms
[CoopPos] server: Nomad=(1543,2115,234)hp-9 Companion=(1536,2116,234)hp-9
[Coop] everybody is down: the last checkpoint is loaded
[CoopSave] rescue runs already: the checkpoint is loaded into it (no level load)
[CoopSave] loading coop_checkpoint_56c79107e0d7a6ff_b.CRYSISJMSF (rescue, checkpoint manual)
[net 13:54:55.987] Disconnect host-pc:64822; profid=0; cause=4; msg=''
[CoopSave] the saved game took 1.2 s to load
[Coop] checkpoint loaded: 1 players' places kept
[CoopPos] server: Nomad=(1696,1826,195)hp198
"""

# musicload_host.log 520, 540, 742, 776, 785: F9 reloaded the level, then
# CryAction's load kicked the companion that had joined in between
KICK_LEVEL = r"""*LOADING: Level multiplayer/tia/coop_rescue loading time: 27.01 seconds
[CoopSave] loading coop_checkpoint_1bc86bbd398076f4_b.CRYSISJMSF (rescue, checkpoint manual)
[net 17:16:49.203] Disconnect host-pc:63228; profid=0; cause=4; msg=''
[CoopSave] the saved game took 1.9 s to load
[Coop] checkpoint loaded: 1 players' places kept
"""

# the user's Game.log 3296-3298, 3328: the host quits
HOST_QUIT = r"""[CoopPos] server: Nomad=(1690,1832,194)hp111
[net 13:59:54.240] Disconnect <local>:64087; profid=0; cause=19; msg='User left the game'
[net 13:59:54.243] Disconnect <local>:16; profid=0; cause=13; msg='Nub destroyed'
[net 13:59:54.246] Disconnect host-pc:61187; profid=0; cause=13; msg='Nub destroyed'
CGameClientChannel::Release
System:Quit
"""

# adk_new2_host.log 1640, 1645: the companion stopped answering, then its echo
TIMEOUT = r"""[CoopAgent] AI companion started (process 7756, joins 127.0.0.1:64087, agents on 127.0.0.1:47810) in 140 ms
[net 15:00:18.253] Disconnect host-pc:63434; profid=0; cause=0; msg='Timeout occurred; no packet for 120.1 seconds'
[CoopHitch] a frame took 391 ms
[net 15:00:19.089] Disconnect host-pc:63434; profid=0; cause=0; msg='Remote disconnected: Remote disconnected: Timeout occurred; no packet for 120.1 seconds'
"""

# fin10_manual_host.log 34860-34861: the companion's log echoed back ([remote])
REMOTE_ECHO = r"""[net 20:29:02.896] Received disconnect packet from host-pc:52182: Remote disconnected: No reason
[net 20:29:02.896]   [remote] [20:29:02.893] Disconnect host-pc:52181; profid=0; cause=4; msg='Remote disconnected: No reason'
"""

# the user's Game.log 2988, 2998-3001: told, turned off and on in the menu
TOLD_AND_RESTART = r"""[CoopAgent] AI companion started (process 19064, joins 127.0.0.1:64087, agents on 127.0.0.1:47810) in 78 ms
[CoopPos] server: Nomad=(1543,2115,234)hp-9 Companion=(1536,2116,234)hp-9
[net 13:54:55.987] Disconnect host-pc:64822; profid=0; cause=4; msg=''
[CoopAgent] told the host: AI companion cannot join your game. Esc > Co-op game: turn it off and on
[CoopMenu] Network > Co-op game: the co-op menu
[CoopAgent] AI companion stopped (turned off in the co-op menu)
[net 13:57:46.302] Disconnect host-pc:64822; profid=0; cause=14; msg='Unreachable address host-pc:64822'
[CoopAgent] AI companion started (process 10564, joins 127.0.0.1:64087, agents on 127.0.0.1:47810) in 157 ms
"""

# fix_on3_host.log 142, 456, 667-668: an old build told after 90 s, the
# companion was in 2 s later (the join line cut by the time stamp)
TOLD_EARLY = r"""[CoopAgent] AI companion started (process 10024, joins 127.0.0.1:64087, agents on 127.0.0.1:47810) in 125 ms
<05:29:11> [CoopAgent] the host's game is ready 16 s after its player is there (things stopped breaking)
<05:30:42> [CoopAgent] told the host: AI companion cannot join your game. Esc > Co-op game: turn it off and on
<05:30:44> $8Nomad(1)$o renamed to $8Compani
"""

# the user's companion.log 144-145, 162-168, 186, 249-250: its join load,
# the break replays time out, the host quits two minutes later
COMP_STALL = r"""[CoopAgent] joining the host: connect 127.0.0.1 61186 (6 s after the start)
[net 13:57:54.326] connection requested to: 127.0.0.1:61186
[CoopAgent] its level loads: a break it makes again waits 3 frames (now 3; the host's game 140)
[CoopTOD] time of day set to the host's 16:56
303456E0 times out
303456E0 times out
303456E0 times out
[net 13:59:54.252] Disconnect host-pc:61186; profid=0; cause=13; msg='Remote disconnected: Nub destroyed'
[CoopAgent] out of the host's game: joining again in 5 s
[CoopAgent] its level loaded: a break it makes again waits 140 frames again (140 now)
"""

# f3_on1_companion_0.log 331-332, 399-402, 477: a break replay timed out,
# yet the join load finished
COMP_JOINED = r"""<05:58:18> [CoopAgent] joining the host: connect 127.0.0.1 61427 (21 s after the start)
<05:58:18> [net 02:58:18.412] connection requested to: 127.0.0.1:61427
<05:58:35> [CoopAgent] its level loads: a break it makes again waits 3 frames (now 3; the host's game 140)
<05:58:50> [Coop] object use is forwarded from clients to the server
<05:59:23> 303568E0 times out
<05:59:33> [CoopAgent] its level loaded: a break it makes again waits 140 frames again (140 now)
<05:59:33> [CoopAgent] picked up the SOCOM
"""

# adk_new2_companion.log 638-642, 666: back in after a kick, its own game
# cut the new connection (cause=25), no line of it on the host
COMP_CAUSE25 = r"""[CoopAgent] joining the host: connect 127.0.0.1 63433 (360 s after the start)
[net 14:58:14.878] connection requested to: 127.0.0.1:63433
[CoopAgent] its level loads: a break it makes again waits 3 frames (now 3; the host's game 140)
[net 14:58:18.145] Disconnect host-pc:63433; profid=0; cause=25; msg='Disconnected'
CGameClientChannel::Release
[CoopAgent] out of the host's game: joining again in 5 s
"""

# final_on2: its log ends while it loads (companion_0.log 344-345, 426-431),
# the host says it cannot join (host.log 142, 745)
FINAL_ON2_COMP = r"""<05:43:12> [CoopAgent] joining the host: connect 127.0.0.1 60642 (22 s after the start)
<05:43:12> [net 02:43:12.211] connection requested to: 127.0.0.1:60642
<05:43:29> [CoopAgent] its level loads: a break it makes again waits about 4.7 s (74 frames here, now 74; the host's game 140)
<05:43:41> [Coop] object use is forwarded from clients to the server
<05:44:35> [CoopAgent] relay here: 3717 packets to the host, 3769 back
<05:44:49> [CoopAgent] relay here: 4416 packets to the host, 4465 back
"""
FINAL_ON2_HOST = r"""[CoopAgent] AI companion started (process 10408, joins 127.0.0.1:64087, agents on 127.0.0.1:47810) in 141 ms
<05:45:41> [CoopAgent] told the host: AI companion cannot join your game. Esc > Co-op game: turn it off and on
"""

START = "[CoopAgent] AI companion started (process 8196, joins 127.0.0.1:64087, agents on 127.0.0.1:47810) in 156 ms\n"
POS_ALONE = "[CoopPos] server: Nomad=(1697,1824,194)hp200\n"       # f3_on1_host.log 438

# fin5_manual_host.log 137, 694, 1081, 1087-1088, 1115-1116, 1142: the
# companion never got in (told), then the test's coop_load threw out the join
# it was making again
KICK_JOINING = r"""[CoopAgent] AI companion started (process 16956, joins 127.0.0.1:64087, agents on 127.0.0.1:47810) in 141 ms
[CoopAgent] told the host: AI companion cannot join your game. Esc > Co-op game: turn it off and on
[CoopPos] server: Nomad=(1697,1825,194)hp200
[CoopSave] rescue runs already: the checkpoint is loaded into it (no level load)
[CoopSave] loading coop_checkpoint_56c79107e0d7a6ff_b.CRYSISJMSF (rescue, checkpoint manual)
[net 19:50:19.717] Disconnect host-pc:55252; profid=0; cause=4; msg=''
[CoopSave] the saved game took 2.1 s to load
[Coop] checkpoint loaded: 1 players' places kept
"""

# adk_new2_host.log 2170, 2174 (companion 749-753): out of a load, the host
# cut the companion's connect with "server full" and wrote only the peer's echo
REFUSED = r"""[CoopAgent] AI companion started (process 7756, joins 127.0.0.1:64087, agents on 127.0.0.1:47810) in 140 ms
[CoopPos] server: Nomad=(1697,1825,194)hp200
[net 15:02:47.836] Disconnect host-pc:63434; profid=0; cause=4; msg='Remote disconnected: Remote disconnected: No reason'
"""

# uw28b_companion.log 474-475, 482-483, 514-516: a join cut by a load, then
# the copy of its log ends 2 lines into the next join; the host's "cannot
# join" (uw28b_host.log 1868) has no time stamp to say which join it was
SNAPSHOT_COMP = r"""[CoopAgent] joining the host: connect 127.0.0.1 51977 (360 s after the start)
[net 10:21:49.183] connection requested to: 127.0.0.1:51977
[CoopAgent] relay here: 6339 packets to the host, 6449 back
[net 10:22:06.111] Disconnect host-pc:51977; profid=0; cause=4; msg='Remote disconnected: No reason'
[CoopAgent] out of the host's game: joining again in 5 s
[CoopAgent] joining the host: connect 127.0.0.1 51977 (403 s after the start)
[net 10:22:31.301] connection requested to: 127.0.0.1:51977
"""
TOLD_LINE = "[CoopAgent] told the host: AI companion cannot join your game. Esc > Co-op game: turn it off and on\n"


class D03Disconnects(unittest.TestCase):
    def test_kick_by_inplace_load(self):
        # a minute of play after it, the companion not in it
        found = run(d.d03_disconnects, KICK_INPLACE + POS_ALONE * 13)
        self.assertEqual(keys(found), [("D03", "FAIL", "kick_inplace_load")])
        f = found[0]
        self.assertIn("cause=4", f.en)
        self.assertIn("in-place load #1", f.en)
        self.assertIn("компаньона выкинуло", f.ru)
        self.assertIn("не вернулся", f.ru)
        self.assertTrue(f.evidence[0].text.startswith("[net 13:54:55.987] Disconnect"))

    def test_kick_with_the_log_ending_at_once_does_not_say_it_never_came_back(self):
        found = run(d.d03_disconnects, KICK_INPLACE)
        self.assertEqual(keys(found), [("D03", "FAIL", "kick_inplace_load")])
        self.assertNotIn("не вернулся", found[0].ru)

    def test_kick_of_a_companion_still_joining_is_only_a_warning(self):
        found = run(d.d03_disconnects, KICK_JOINING + POS_ALONE * 13)
        self.assertEqual(keys(found), [("D03", "WARN", "kick_joining_load")])
        self.assertIn("ещё входил в игру", found[0].ru)
        self.assertIn("still joining", found[0].en)

    def test_kick_after_a_level_load_is_only_a_warning(self):
        self.assertEqual(keys(run(d.d03_disconnects, KICK_LEVEL)), [("D03", "WARN", "kick_level_load")])

    def test_connect_refused_out_of_a_load(self):
        # an echo with no line of the host's own before it is the only trace
        found = run(d.d03_disconnects, REFUSED)
        self.assertEqual(keys(found), [("D03", "FAIL", "server_full")])
        self.assertIn("не пустил", found[0].ru)

    def test_quit_is_not_reported(self):
        self.assertEqual(run(d.d03_disconnects, HOST_QUIT), [])

    def test_timeout_once_not_its_echo(self):
        found = run(d.d03_disconnects, TIMEOUT)
        self.assertEqual(keys(found), [("D03", "FAIL", "timeout")])
        self.assertEqual(found[0].count, 1)
        self.assertIn("120.1", found[0].ru)

    def test_remote_echo_is_not_reported(self):
        self.assertEqual(run(d.d03_disconnects, REMOTE_ECHO), [])

    def test_unreachable_after_a_stop_is_not_reported(self):
        found = run(d.d03_disconnects, TOLD_AND_RESTART)
        # the cause=4 outside a load is; the cause=14 right after the stop is not
        self.assertEqual(keys(found), [("D03", "FAIL", "server_full")])


class D04CompanionLost(unittest.TestCase):
    def test_told_after_it_was_in(self):
        found = run(d.d04_companion_lost, TOLD_AND_RESTART)
        self.assertEqual(keys(found), [("D04", "FAIL", "cannot_join")])
        self.assertIn("не смог вернуться", found[0].ru)
        self.assertTrue(found[0].evidence[0].text.startswith("[CoopAgent] told the host"))

    def test_turned_off_and_on_is_no_unrequested_restart(self):
        found = run(d.d04_companion_lost, TOLD_AND_RESTART)
        self.assertNotIn("restart", [f.key for f in found])

    def test_told_too_early(self):
        self.assertEqual(keys(run(d.d04_companion_lost, TOLD_EARLY)), [("D04", "WARN", "told_then_joined")])

    def test_never_in_the_game(self):
        found = run(d.d04_companion_lost, START + POS_ALONE * 31)
        self.assertEqual(keys(found), [("D04", "FAIL", "cannot_join")])
        self.assertEqual(found[0].ru, "компаньон не смог войти в игру")

    def test_cut_join_line_counts_as_in(self):
        # f3_on1_host.log 646, 653: "renamed to $8Compani" (cut), then [CoopPos] naming it
        text = START + POS_ALONE * 31 + "<05:59:33> $8Nomad(1)$o renamed to $8Compani\n" + \
            "<05:59:36> [CoopPos] server: Nomad=(1697,1824,195)hp200 Companion=(1698,1826,196)hp200\n"
        self.assertEqual(run(d.d04_companion_lost, text), [])

    def test_short_log_is_not_a_failure(self):
        self.assertEqual(run(d.d04_companion_lost, START + POS_ALONE * 5), [])

    # no log has these yet: written from CoopAgent.cpp's format strings
    # (Running(): "the AI companion's game ended", StopCompanion("%s"))
    def test_its_game_ended_by_itself(self):
        text = START + POS_ALONE + "[CoopAgent] the AI companion's game ended\n" + POS_ALONE * 2 + START
        found = run(d.d04_companion_lost, text)
        self.assertEqual([(f.verdict, f.key) for f in found if f.key in ("game_ended", "restart")], [("FAIL", "game_ended")])

    def test_stopped_when_the_host_left_at_the_end(self):
        text = START + POS_ALONE * 2 + "[CoopAgent] AI companion stopped (the host is not in a co-op game)\nSystem:Quit\n"
        self.assertEqual([f for f in run(d.d04_companion_lost, text) if f.key == "stopped_mid_game"], [])

    def test_second_start_without_a_stop(self):
        found = run(d.d04_companion_lost, START + POS_ALONE * 2 + START)
        self.assertIn(("D04", "FAIL", "restart"), keys(found))

    def test_start_after_a_failed_start_is_no_unrequested_restart(self):
        # CoopAgent.cpp StartCompanion: "could not be started (error %u)", then again in 30 s
        text = START + POS_ALONE + "[CoopAgent] the AI companion's game ended\n" + \
            "[CoopAgent] the AI companion's game could not be started (error 5)\n" + POS_ALONE * 6 + START
        found = run(d.d04_companion_lost, text)
        self.assertNotIn("restart", [f.key for f in found])
        self.assertIn(("D04", "FAIL", "no_start"), keys(found))

    def test_companion_cut_its_own_connection(self):
        found = run(d.d04_companion_lost, START, COMP_CAUSE25)
        self.assertEqual(keys(found), [("D04", "FAIL", "out_cause25")])

    def test_out_after_the_host_quit_is_not_reported(self):
        found = run(d.d04_companion_lost, START + POS_ALONE * 3, COMP_STALL)
        self.assertNotIn("out_cause13", [f.key for f in found])
        self.assertEqual([f for f in found if f.verdict == "FAIL"], [])

    def test_out_long_after_its_disconnect_line_is_still_matched(self):
        # the user's companion.log 186-249: 62 lines between the two; here 90
        filler = "[CoopNet] last read #0: Nomad (Player, id 65516) aspect=64 profile=0\n" * 90
        comp = COMP_STALL.replace("[CoopAgent] out of the host's game", filler + "[CoopAgent] out of the host's game")
        found = run(d.d04_companion_lost, START + POS_ALONE * 3, comp)
        self.assertEqual([f for f in found if f.key.startswith("out_")], [])

    def test_out_after_a_kick_the_host_logged_is_d03s(self):
        # adk_new2: host 1322 (14:57:47.996) and companion 583 (14:57:48.003)
        host = KICK_INPLACE.replace("13:54:55.987", "14:57:47.996")
        comp = ("[net 14:57:48.003] Disconnect host-pc:63433; profid=0; cause=4; msg='Remote disconnected: No reason'\n"
                "CGameClientChannel::Release\n"
                "[CoopAgent] out of the host's game: joining again in 5 s\n")
        found = run(d.d04_companion_lost, host, comp)
        self.assertEqual([f for f in found if f.key.startswith("out_")], [])


class D16JoinStall(unittest.TestCase):
    def test_break_replays_and_no_end(self):
        found = run(d.d16_join_stall, START, COMP_STALL)
        self.assertEqual(keys(found), [("D16", "FAIL", "join_stall")])
        f = found[0]
        self.assertIn("3 разрушения", f.ru)
        self.assertIn("loaded for 119 s", f.en)
        self.assertTrue(f.evidence[0].text.startswith("[CoopAgent] its level loads"))

    def test_join_that_finished(self):
        self.assertEqual(run(d.d16_join_stall, START, COMP_JOINED), [])

    def test_join_that_finished_after_the_host_gave_up(self):
        # COMP_JOINED with its load over 165 s after it began
        comp = COMP_JOINED.replace("<05:59:33> [CoopAgent] its level loaded", "<06:01:20> [CoopAgent] its level loaded")
        found = run(d.d16_join_stall, START, comp)
        self.assertEqual(keys(found), [("D16", "FAIL", "join_slow")])
        self.assertIn("165 с", found[0].ru)

    def test_short_join_cut_by_the_hosts_quit_is_not_a_stall(self):
        # COMP_STALL with the host gone 20 s after the connect: break replays
        # time out in joins that finish, too
        comp = COMP_STALL.replace("13:59:54.252", "13:58:14.252")
        self.assertEqual(run(d.d16_join_stall, START, comp), [])

    def test_log_copy_ending_in_a_later_join_is_not_a_stall(self):
        self.assertEqual(run(d.d16_join_stall, START + TOLD_LINE, SNAPSHOT_COMP), [])

    def test_kick_while_loading_is_not_a_stall(self):
        comp = COMP_CAUSE25.replace("cause=25; msg='Disconnected'", "cause=4; msg='Remote disconnected: No reason'")
        self.assertEqual(run(d.d16_join_stall, START, comp), [])

    def test_log_ends_while_loading_and_host_told(self):
        found = run(d.d16_join_stall, FINAL_ON2_HOST, FINAL_ON2_COMP)
        self.assertEqual(keys(found), [("D16", "FAIL", "join_stall")])
        self.assertTrue(found[0].evidence[-1].text.startswith("[CoopAgent] told the host"))

    def test_log_ends_while_loading_but_host_has_it_in(self):
        host = FINAL_ON2_HOST.replace(
            "<05:45:41> [CoopAgent] told the host: AI companion cannot join your game. Esc > Co-op game: turn it off and on",
            "<05:45:41> $8Nomad(1)$o renamed to $8Compani")
        self.assertEqual(run(d.d16_join_stall, host, FINAL_ON2_COMP), [])

    def test_no_companion_log(self):
        self.assertEqual(run(d.d16_join_stall, START), [])


LIVE = r"C:\Users\andre\AppData\Local\Temp\claude\C--GOG-Games-Crysis\76c1b474-7cf3-4595-a9cc-405a5ae0d577\scratchpad\live2"


@unittest.skipUnless(os.path.exists(os.path.join(LIVE, "Game.log")), "the user's Game.log of 2026-10-15 16:45 is not here")
class Golden(unittest.TestCase):
    """The user's game: load 2 (the second in place) kicked the companion
    (cause=4), it could not come back in; the companion started again stalled
    in the break replay."""

    def setUp(self):
        from coopjudge.logread import Log
        comp = os.path.join(LIVE, "companion.log")
        self.ctx = Context(Log(os.path.join(LIVE, "Game.log"), "host"),
                           Log(comp, "companion") if os.path.exists(comp) else None)

    def test_d03_kick_in_the_second_inplace_load(self):
        f = [x for x in merge(d.d03_disconnects(self.ctx)) if x.verdict == "FAIL"]
        self.assertEqual([(x.key, x.evidence[0].no) for x in f], [("kick_inplace_load", 2572)])
        self.assertIn("cause=4", f[0].evidence[0].text)
        self.assertIn("in-place load #2", f[0].en)

    def test_d04_could_not_come_back(self):
        f = [x for x in merge(d.d04_companion_lost(self.ctx)) if x.verdict == "FAIL"]
        self.assertEqual([(x.key, x.evidence[0].no) for x in f], [("cannot_join", 2988)])

    @unittest.skipUnless(os.path.exists(os.path.join(LIVE, "companion.log")), "no companion.log")
    def test_d16_companion_stalled(self):
        f = merge(d.d16_join_stall(self.ctx))
        self.assertEqual([(x.verdict, x.evidence[0].no) for x in f], [("FAIL", 162)])
        self.assertIn("14 '%p times out'", f[0].en)


if __name__ == "__main__":
    unittest.main()


class TestCompanionCombat(unittest.TestCase):
    def test_hits_and_kills(self):
        from coopjudge.report import companion_combat
        log = from_text("""[Coop] HIT Bridge_5 by Companion weapon=VehicleShiTenV2 type=bullet dist=40 x1.00 dmg=30.0 hp 100->70
[Coop] HIT Bridge_5 by Companion weapon=VehicleShiTenV2 type=bullet dist=40 x1.00 dmg=90.0 hp 70->-20 KILLED
[Coop] HIT player Nomad by Companion weapon=SCAR type=bullet dist=4 x1.00 dmg=0.0 hp 200->200
[Coop] HIT Farm_4 by Nomad weapon=FY71 type=bullet dist=9 x1.00 dmg=90.0 hp 50->-40 KILLED
[Coop] HIT Farm_4 by Companion weapon=FY71 type=bullet dist=9 x1.00 dmg=90.0 hp 50->-40 KILLED
""", "at_host.log")
        c = companion_combat(Context(log))
        self.assertEqual((c["hits"], c["kills"]), (3, 2))
        self.assertEqual(c["weapons"], {"VehicleShiTenV2": 1, "FY71": 1})
        self.assertEqual(c["killed"], ["Bridge_5", "Farm_4"])
