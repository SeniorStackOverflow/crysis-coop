"""D23: the AI companion's vehicle gun spins round and round.

CryAction keeps a client gunner's turret where it pointed in the world,
reckoned against the car body's frame; in the companion's game that frame
stayed where the car first stood, and each frame the gun turned on by all the
car had turned since: once the host steered, the gun spun ever faster (2.5 rad
a frame), never came onto an enemy, never fired (the user's game of
2026-10-17 on 0.11.39). The companion logs its gun once a second
(CoopAgent.cpp VehicleGunner):
  [CoopAgent] gun aim at X: yaw off 2.416 ... view (...), gun (0.33 0.93 -0.14), ...
A gun seen once a second at a direction far from the last one (0.8 rad and
more: at 2 rad a frame any direction is as likely), far off its enemy, in 4
of 6 lines in a row, spins.
"""

import math
import re

from ..findings import Finding

RE_AIM = re.compile(r"^\[CoopAgent\] gun aim at (\S+): yaw off (-?[\d.]+) pitch off (-?[\d.]+) .*?gun \((-?[\d.]+) (-?[\d.]+) (-?[\d.]+)\)")

TURN = 0.8     # rad between two lines (a second)
OFF = 0.3      # rad off the enemy
WINDOW = 6     # lines
LINES = 4      # such turns among them


def _angle(a, b):
    la = math.sqrt(sum(x * x for x in a))
    lb = math.sqrt(sum(x * x for x in b))
    if la < 1e-6 or lb < 1e-6:
        return 0.0
    d = sum(x * y for x, y in zip(a, b)) / (la * lb)
    return math.acos(max(-1.0, min(1.0, d)))


def d23_gunspin(ctx):
    if ctx.companion is None:
        return []
    window = []     # (line, spun)
    last = None
    for l in ctx.lines(ctx.companion):
        m = RE_AIM.match(l.text)
        if not m:
            continue
        gun = (float(m.group(4)), float(m.group(5)), float(m.group(6)))
        off = abs(float(m.group(2)))
        if last is not None:
            window.append((l, _angle(last, gun) > TURN and off > OFF))
            window = window[-WINDOW:]
            spun = [x for x, s in window if s]
            if len(spun) >= LINES:
                return [Finding(
                    "D23", "FAIL",
                    "пулемёт ИИ-компаньона крутится (%d раз из %d секунд подряд далеко от прежнего направления и мимо врага): он не наводится и не стреляет" % (len(spun), len(window)),
                    "the companion's vehicle gun spins (%d of %d lines in a row far from the last direction, off its enemy)" % (len(spun), len(window)),
                    [spun[0], spun[-1]], key="gunspin")]
        last = gun
    return []


DETECTORS = [d23_gunspin]
