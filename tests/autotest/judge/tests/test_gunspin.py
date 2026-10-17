import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from coopjudge.context import Context                      # noqa: E402
from coopjudge.detectors.d23_gunspin import d23_gunspin     # noqa: E402
from coopjudge.logread import from_text                     # noqa: E402

HOST = "[CoopPos] server: Nomad=(1,1,1)hp200\n"

# the user's companion.log of 2026-10-17 on 0.11.39: the gun spun while he steered
SPIN = """[CoopAgent] gun aim at Bridge_Reinforcement_1: yaw off 0.514 pitch off 0.188 (signs -1 -1, votes 0 0), view (-0.28 0.96 -0.03), gun (-0.28 0.96 -0.03), eye (1740.1 2014.3 214.2), car (1739.6 2015.0 211.5), 1 frames, seat state 0
[CoopAgent] gun aim at Bridge_Reinforcement_1: yaw off -1.656 pitch off 0.109 (signs -1 -1, votes -1 -4), view (0.45 -0.89 -0.08), gun (0.45 -0.89 -0.08), eye (1735.1 2025.3 217.2), car (1734.6 2025.5 214.6), 17 frames, seat state 0
[CoopAgent] gun aim at Bridge_Reinforcement_1: yaw off -2.594 pitch off 0.310 (signs -1 -1, votes 0 -4), view (0.94 -0.15 -0.29), gun (0.95 -0.15 -0.29), eye (1731.7 2023.8 217.8), car (1730.9 2024.2 215.2), 17 frames, seat state 0
[CoopAgent] gun aim at Bridge_Reinforcement_1: yaw off -0.125 pitch off -0.093 (signs 1 -1, votes 4 -4), view (-0.81 -0.59 0.08), gun (-0.81 -0.59 0.08), eye (1724.0 2021.7 218.7), car (1723.7 2021.9 216.1), 17 frames, seat state 0
[CoopAgent] gun aim at Bridge_Reinforcement_1: yaw off 0.813 pitch off 0.009 (signs 1 -1, votes 4 -4), view (-0.98 0.18 -0.06), gun (-0.98 0.18 -0.06), eye (1713.7 2017.7 219.2), car (1713.6 2017.5 216.5), 17 frames, seat state 0
[CoopAgent] gun aim at Bridge_Reinforcement_1: yaw off 2.416 pitch off 0.021 (signs 1 -1, votes 2 -4), view (0.33 0.93 -0.14), gun (0.33 0.93 -0.14), eye (1707.3 2011.4 219.5), car (1707.0 2011.3 216.8), 17 frames, seat state 0
[CoopAgent] gun aim at Bridge_Reinforcement_1: yaw off 2.637 pitch off 0.099 (signs 1 -1, votes 1 -4), view (0.64 0.74 -0.21), gun (0.64 0.74 -0.21), eye (1708.4 2012.2 219.5), car (1708.0 2012.1 216.7), 17 frames, seat state 0
"""
# turning onto him, then on target
TRACK = """[CoopAgent] gun aim at Bridge_1: yaw off 1.427 pitch off -0.025 (signs -1 -1, votes -4 -4), view (-0.42 -0.90 -0.09), gun (-0.42 -0.90 -0.09), seat state 0
[CoopAgent] gun aim at Bridge_1: yaw off -0.050 pitch off -0.021 (signs -1 -1, votes -4 -4), view (0.07 -0.99 -0.10), gun (0.07 -0.99 -0.10), seat state 0
[CoopAgent] gun aim at Bridge_1: yaw off -0.005 pitch off 0.002 (signs -1 -1, votes -4 -3), view (0.03 -0.99 -0.13), gun (0.03 -0.99 -0.13), seat state 0
[CoopAgent] gun aim at Bridge_6: yaw off 2.900 pitch off 0.002 (signs -1 -1, votes -4 -3), view (0.03 -0.99 -0.13), gun (0.03 -0.99 -0.13), seat state 0
[CoopAgent] gun aim at Bridge_6: yaw off 0.900 pitch off 0.002 (signs -1 -1, votes -4 -3), view (0.90 0.40 -0.13), gun (0.90 0.40 -0.13), seat state 0
[CoopAgent] gun aim at Bridge_6: yaw off 0.002 pitch off 0.002 (signs -1 -1, votes -4 -3), view (0.60 0.80 -0.13), gun (0.60 0.80 -0.13), seat state 0
"""


class TestGunSpin(unittest.TestCase):
    def run_on(self, text):
        return [(x.verdict, x.key) for x in d23_gunspin(Context(from_text(HOST, "at_host.log"), from_text(text, "at_companion.log")))]

    def test_spin(self):
        self.assertEqual(self.run_on(SPIN), [("FAIL", "gunspin")])

    def test_tracking(self):
        self.assertEqual(self.run_on(TRACK), [])


if __name__ == "__main__":
    unittest.main()
