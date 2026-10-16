"""D18: a living player the soldiers cannot see.

Every 10 s the host logs each player's AI object (CoopAI.cpp):
  [CoopAI] player Companion ai=1 enabled=0 entity=(1309,2155,237) aiobj=(...) spect=0
and every few seconds the players' health:
  [CoopPos] server: Nomad=(1493,1907,220)hp200 Companion=(1493,1906,220)hp200

An AI object switched off is no target: the soldiers walk past him. That is
right while he is down (CoopAI switches it off) and at the level's start; a
player alive with it off for half a minute is a bug (the checkpoint saved with
the companion's switched off, loaded back: the autotester's monkey game of
2026-10-15).
"""

import re

from ..findings import Finding

RE_AIOBJ = re.compile(r"^\[CoopAI\] player (\S+) ai=1 enabled=(\d) ")
RE_POS = re.compile(r"(\w+)=\((-?\d+),(-?\d+),(-?\d+)\)hp(-?\d+)")

TICKS = 3          # 10 s ticks in a row: off for ~30 s while alive


def d18_invisible(ctx):
    out = []
    hp = {}
    off = {}          # name -> [first line, ticks]
    ready = False     # the level ran a while (its start switches the host's off)
    seen_on = set()
    for l in ctx.lines(ctx.host):
        s = l.text
        if s.startswith("[CoopPos] server:"):
            for m in RE_POS.finditer(s):
                hp[m.group(1)] = int(m.group(5))
            continue
        m = RE_AIOBJ.match(s)
        if not m:
            continue
        name, enabled = m.group(1), m.group(2) == "1"
        if enabled:
            seen_on.add(name)
            off.pop(name, None)
            continue
        if name not in seen_on or hp.get(name, 0) <= 0:
            off.pop(name, None)
            continue
        first, n = off.get(name, (l, 0))
        off[name] = (first, n + 1)
        if n + 1 == TICKS:
            out.append(Finding(
                "D18", "FAIL",
                "солдаты не видят живого игрока %s (его ИИ-объект выключен): не стреляют в него, он для них невидим" % name,
                "%s alive with his AI object off for %d ticks: no target for the soldiers" % (name, TICKS),
                [first, l], key=name))
    return out


DETECTORS = [d18_invisible]
