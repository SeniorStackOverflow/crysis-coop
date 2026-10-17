import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from coopjudge.context import Context                 # noqa: E402
from coopjudge.detectors.d19_gun import d19_gun        # noqa: E402
from coopjudge.logread import from_text                # noqa: E402

HOST = "[CoopPos] server: Nomad=(1,1,1)hp200\n"

# the user's companion.log of 2026-10-16 (lines 904-918): the gun hung
HUNG = """[CoopAgent] gun aim at Bridge_Reinforcement_1: yaw off 0.298 pitch off 0.281 (signs -1 -1, votes -1 -2), view (-0.65 -0.65 -0.38)
[CoopAgent] gun aim at Bridge_Reinforcement_1: yaw off 0.332 pitch off 0.250 (signs -1 -1, votes -1 -1), view (-0.66 -0.66 -0.37)
[CoopAgent] gun aim at Bridge_Reinforcement_1: yaw off 0.340 pitch off 0.243 (signs -1 -1, votes -1 -1), view (-0.66 -0.66 -0.36)
[CoopAgent] gun aim at Bridge_Reinforcement_1: yaw off 0.350 pitch off 0.249 (signs -1 -1, votes -1 -2), view (-0.66 -0.66 -0.36)
[CoopAgent] gun aim at Bridge_Reinforcement_1: yaw off 0.356 pitch off 0.255 (signs -1 -1, votes -1 -2), view (-0.66 -0.66 -0.36)
[CoopAgent] gun aim at Bridge_Reinforcement_1: yaw off 0.363 pitch off 0.273 (signs -1 -1, votes -1 -1), view (-0.66 -0.66 -0.36)
"""
# turning onto him, then on target (the autotester's companion-gunner-behind)
TRACKING = """[CoopAgent] gun aim at Bridge_1: yaw off 0.427 pitch off -0.025 (signs -1 -1, votes -4 -4), view (-0.42 -0.90 -0.09), gun (-0.42 -0.90 -0.09)
[CoopAgent] gun aim at Bridge_1: yaw off -0.050 pitch off -0.021 (signs -1 -1, votes -4 -4), view (0.07 -0.99 -0.10), gun (0.07 -0.99 -0.10)
[CoopAgent] gun aim at Bridge_1: yaw off -0.005 pitch off 0.002 (signs -1 -1, votes -4 -3), view (0.03 -0.99 -0.13), gun (0.03 -0.99 -0.13)
[CoopAgent] gun aim at Bridge_1: yaw off -0.002 pitch off 0.002 (signs -1 -1, votes -4 -4), view (0.02 -0.99 -0.12), gun (0.02 -0.99 -0.12)
[CoopAgent] gun aim at Bridge_1: yaw off -0.002 pitch off 0.003 (signs -1 -1, votes -4 -4), view (0.01 -0.99 -0.12), gun (0.01 -0.99 -0.12)
[CoopAgent] gun aim at Bridge_1: yaw off -0.000 pitch off -0.006 (signs -1 -1, votes -4 -4), view (0.00 -0.99 -0.10), gun (0.00 -0.99 -0.10)
"""


class TestGun(unittest.TestCase):
    def run_on(self, text):
        return d19_gun(Context(from_text(HOST, "at_host.log"), from_text(text, "at_companion.log")))

    def test_hung(self):
        f = self.run_on(HUNG)
        self.assertEqual([(x.verdict, x.key) for x in f], [("FAIL", "Bridge_Reinforcement_1")])

    def test_tracking(self):
        self.assertEqual(self.run_on(TRACKING), [])

    def test_no_companion(self):
        self.assertEqual(d19_gun(Context(from_text(HOST, "at_host.log"))), [])


if __name__ == "__main__":
    unittest.main()
