"""D20: the AI companion's join stalled.

The companion's game connects, its channel comes up, and its level begins to
load within ~9 s ("Game rules class: ..."). Once in ~110 joins the setup
stalled: both sides kept sending, no level load, minutes long (the
autotester's game of 2026-10-16). Since then the companion leaves after 45 s,
takes a new relay and joins again (CoopAgent.cpp):
  [CoopAgent] connected 45 s and its level never began to load (relay 1980 to the host, 1990 back): out, a new relay, in again (1)
  [CoopAgent] the join stalled 3 times: quitting, the host starts it again
A stall it came back from is a note; three in a row (its game quits) a fault.
"""

import re

from ..findings import Finding

RE_STALL = re.compile(r"^\[CoopAgent\] connected (\d+) s and its level never began to load .*\((\d+)\)$")
RE_QUIT = re.compile(r"^\[CoopAgent\] the join stalled 3 times")


def d20_join(ctx):
    if ctx.companion is None:
        return []
    stalls, quits = [], []
    for l in ctx.lines(ctx.companion):
        s = l.text
        if RE_STALL.match(s):
            stalls.append(l)
        elif RE_QUIT.match(s):
            quits.append(l)
    out = []
    if quits:
        out.append(Finding(
            "D20", "FAIL",
            "вход ИИ-компаньона застрял 3 раза подряд (канал есть, уровень не грузится): его игра закрылась, хост запускает её заново",
            "the companion's join stalled 3 times in a row: its game quit", quits + stalls[:2], key="quit"))
    elif stalls:
        f = Finding(
            "D20", "WARN",
            "вход ИИ-компаньона застрял (канал есть, уровень не грузится): он вышел и зашёл заново",
            "the companion's join stalled; it left and joined again", stalls[:3], key="stall")
        f.count = len(stalls)
        out.append(f)
    return out


DETECTORS = [d20_join]
