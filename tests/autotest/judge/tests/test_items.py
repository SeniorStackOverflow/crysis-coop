import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from coopjudge.context import Context          # noqa: E402
from coopjudge.detectors.d17_items import d17_items  # noqa: E402
from coopjudge.logread import from_text        # noqa: E402

# from the autotester's run 261015-204848 (b1: rearm on, the leak; b2: rearm off)
LEAKY = """[CoopSave] things after the load: 1113 held, 32 orphans (22 of them visible), 905 owned by nobody (690 hidden), 0 held by things; after the soldiers' equipment: 1215 held, 32 orphans (22 of them visible), 2113 owned by nobody (1899 hidden), 0 held by things
[CoopSave] things after the load: 1234 held, 1122 orphans (28 of them visible), 912 owned by nobody (692 hidden), 0 held by things; after the soldiers' equipment: 1236 held, 1122 orphans (28 of them visible), 2114 owned by nobody (1895 hidden), 0 held by things
"""
CLEAN = """[CoopSave] things after the load: 1113 held, 32 orphans (22 of them visible), 905 owned by nobody (690 hidden), 0 held by things; after the soldiers' equipment: 1113 held, 32 orphans (22 of them visible), 905 owned by nobody (690 hidden), 0 held by things
[CoopSave] things after the load: 1134 held, 32 orphans (22 of them visible), 910 owned by nobody (692 hidden), 0 held by things; after the soldiers' equipment: 1134 held, 32 orphans (22 of them visible), 910 owned by nobody (692 hidden), 0 held by things
"""


class TestItems(unittest.TestCase):
    def run_on(self, text):
        return d17_items(Context(from_text(text, "at_host.log")))

    def test_leak_and_growth(self):
        f = self.run_on(LEAKY)
        keys = sorted(x.key for x in f)
        self.assertEqual(keys, ["orphans_grow", "rearm_leak", "rearm_leak"])
        self.assertTrue(all(x.verdict == "FAIL" for x in f))

    def test_clean(self):
        self.assertEqual(self.run_on(CLEAN), [])


class TestShield(unittest.TestCase):
    def test_shielded_hit_is_no_finding(self):
        from coopjudge.detectors.d07_physics import DETECTORS as D07
        log = from_text("""[CoopSave] loading coop_checkpoint_x_b.CRYSISJMSF (rescue, checkpoint manual)
[CoopSave] the saved game took 1.2 s to load
[CoopSave] progress loaded: rescue, checkpoint manual (saved 2026-10-14 12:36:40)
[Coop] checkpoint loaded: 1 players' places kept
[Coop] DAMAGE player Nomad by Nomad type=collision hit=4070.6: none, the load has just ended
""", "at_host.log")
        ctx = Context(log)
        found = [f for d in D07 for f in (d(ctx) or [])]
        self.assertEqual([f for f in found if f.verdict == "FAIL"], [])


if __name__ == "__main__":
    unittest.main()
