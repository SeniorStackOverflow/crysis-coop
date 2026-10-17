import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from coopjudge.context import Context                      # noqa: E402
from coopjudge.detectors.d21_gunloop import d21_gunloop     # noqa: E402
from coopjudge.logread import from_text                     # noqa: E402


class TestGunLoop(unittest.TestCase):
    def test_stopped_loop(self):
        log = from_text("[CoopSound] a gunfire loop played on with nobody firing that long: stopped (3 so far: fy71_cu_loop_071006_med_mono.w(15 s) fy71_3rd_loop_close_71_a_short(15 s))\n", "at_host.log")
        f = d21_gunloop(Context(log))
        self.assertEqual([(x.verdict, x.key) for x in f], [("WARN", "gunloop")])
        self.assertIn("fy71_3rd_loop_close_71_a_short", f[0].en)

    def test_clean(self):
        self.assertEqual(d21_gunloop(Context(from_text("[CoopSound] sound limits: s_PriorityThreshold 45\n", "at_host.log"))), [])


if __name__ == "__main__":
    unittest.main()
