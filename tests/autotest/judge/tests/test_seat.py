import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from coopjudge.context import Context                 # noqa: E402
from coopjudge.detectors.d22_seat import d22_seat      # noqa: E402
from coopjudge.logread import from_text                # noqa: E402

HOST = "[CoopPos] server: Nomad=(1,1,1)hp200\n"

# the user's companion.log of 2026-10-17 (0.11.38): "seat state 1" all along
OLD_STUCK = "".join("[CoopAgent] gun aim at Helicamp_MG_9: yaw off -1.277 pitch off 0.120 (signs 1 -1, votes 1 -4), view (-0.97 -0.26 0.00), gun (0.00 0.00 0.00), eye (1691.6 2003.8 218.8), car (1694.7 2006.1 217.1), 16 frames, seat state 1\n" for _ in range(6))
OLD_FINE = "".join("[CoopAgent] gun aim at Bridge_6: yaw off -0.009 pitch off -0.004 (signs -1 -1, votes -4 -3), view (0.13 0.99 0.10), gun (0.13 0.99 0.10), eye (1737.3 1939.5 205.1), car (1737.0 1939.8 202.5), 17 frames, seat state 0\n" for _ in range(6))

SIT = "[CoopAgent] its seat gunner of Non_Driveable_Vehicles.Civ_Car8: transition -1 -> 1 (30.7 s after the last change), gun none; the vehicle updated every frame, entity active 1 hidden %d policy 0, game object update 1 visible 0 distant %d slots 1 2 2\n"
SAT = "[CoopAgent] its seat gunner of Non_Driveable_Vehicles.Civ_Car8: transition 1 -> 0 (%.1f s after the last change), gun in hand; the vehicle updated every frame, entity active 1 hidden 0 policy 0, game object update 1 visible 0 distant 0 slots 1 2 2\n"
IN1 = "[CoopAgent] in Non_Driveable_Vehicles.Civ_Car8: seat gunner (gunner 1, driver 0, transition 1, gun 0), fire at will 1, order 0, the leader Nomad in Non_Driveable_Vehicles.Civ_Car8; entity active 1 hidden 1\n"
GONE = "[CoopAgent] its seat: none any more (transition was %d for %.1f s)\n"
NET = "[CoopAgent] Non_Driveable_Vehicles.Civ_Car8 was hidden in its own game for 2 s while it rode in it: shown\n"


class TestSeat(unittest.TestCase):
    def run_on(self, text):
        return [(x.verdict, x.key) for x in d22_seat(Context(from_text(HOST, "at_host.log"), from_text(text, "at_companion.log")))]

    def test_old_stuck(self):
        self.assertEqual(self.run_on(OLD_STUCK), [("FAIL", "seat")])

    def test_old_fine(self):
        self.assertEqual(self.run_on(OLD_FINE), [])

    def test_sat_down(self):
        self.assertEqual(self.run_on(SIT % (0, 0) + SAT % 0.0 + GONE % (0, 60.0)), [])

    def test_stuck_whole_ride(self):
        self.assertEqual(self.run_on(SIT % (1, 1) + IN1 + GONE % (1, 41.0)), [("FAIL", "Non_Driveable_Vehicles.Civ_Car8")])

    def test_stuck_periodic(self):
        self.assertEqual(self.run_on(SIT % (1, 1) + IN1 + IN1), [("FAIL", "Non_Driveable_Vehicles.Civ_Car8")])

    def test_late_sit(self):
        self.assertEqual(self.run_on(SIT % (1, 1) + SAT % 35.0), [("FAIL", "Non_Driveable_Vehicles.Civ_Car8")])

    def test_safety_net(self):
        self.assertEqual(self.run_on(SIT % (1, 1) + IN1 + NET + SAT % 2.1), [("WARN", "hidden")])


if __name__ == "__main__":
    unittest.main()
