import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from coopjudge.context import Context                 # noqa: E402
from coopjudge.detectors.d20_join import d20_join      # noqa: E402
from coopjudge.logread import from_text                # noqa: E402

HOST = "[CoopPos] server: Nomad=(1,1,1)hp200\n"
STALL = "[CoopAgent] connected 45 s and its level never began to load (relay 1980 to the host, 1990 back): out, a new relay, in again (1)\n"
QUIT = STALL.replace("(1)", "(2)") + STALL.replace("(1)", "(3)") + "[CoopAgent] the join stalled 3 times: quitting, the host starts it again\n"


class TestJoin(unittest.TestCase):
    def run_on(self, text):
        return d20_join(Context(from_text(HOST, "at_host.log"), from_text(text, "at_companion.log")))

    def test_stall_recovered(self):
        f = self.run_on(STALL)
        self.assertEqual([(x.verdict, x.key) for x in f], [("WARN", "stall")])

    def test_quit(self):
        f = self.run_on(QUIT)
        self.assertEqual([(x.verdict, x.key) for x in f], [("FAIL", "quit")])

    def test_clean(self):
        self.assertEqual(self.run_on("[CoopAgent] joining the host: connect 127.0.0.1 51052 (48 s after the start)\n"), [])


if __name__ == "__main__":
    unittest.main()
