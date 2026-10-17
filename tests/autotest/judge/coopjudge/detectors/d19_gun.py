"""D19: the AI companion's vehicle gun hangs.

At a vehicle's gun the companion logs once a second (CoopAgent.cpp
VehicleGunner):
  [CoopAgent] gun aim at Bridge_Reinforcement_1: yaw off 0.340 pitch off 0.243
    (signs -1 -1, votes -1 -1), view (-0.66 -0.66 -0.36)[, gun (...), ...]
The view is the gun's (its camera sits on the gun). A gun far off its enemy
that does not turn for seconds never fires: the user's game of 2026-10-16
("view (-0.66 -0.66 -0.36)" for 6 s, 0.6 off; twice he put the companion at
an LTV's gun and it never fired), the autotester's companion-gunner-uphill
(15-45 s, 2.4 off).
"""

import math
import re

from ..findings import Finding

RE_AIM = re.compile(r"^\[CoopAgent\] gun aim at (\S+): yaw off (-?[\d.]+) pitch off (-?[\d.]+) .*?view \((-?[\d.]+) (-?[\d.]+) (-?[\d.]+)\)")

OFF = 0.3          # rad (yaw + pitch) off the enemy
STILL = 0.035      # rad the gun turned at most from one line to the next
LINES = 6          # lines in a row (one a second): ~5 s


def _angle(a, b):
    la = math.sqrt(sum(x * x for x in a))
    lb = math.sqrt(sum(x * x for x in b))
    if la < 1e-6 or lb < 1e-6:
        return math.pi
    d = sum(x * y for x, y in zip(a, b)) / (la * lb)
    return math.acos(max(-1.0, min(1.0, d)))


def d19_gun(ctx):
    if ctx.companion is None:
        return []
    out = []
    run = []          # [(line, enemy, view, off)]
    told = set()

    def close():
        if len(run) >= LINES:
            enemy = run[0][1]
            if enemy not in told:
                told.add(enemy)
                out.append(Finding(
                    "D19", "FAIL",
                    "пулемёт ИИ-компаньона замер, не поворачиваясь к врагу %s (%d с, %.2f рад мимо): он не стреляет" % (enemy, len(run), run[0][3]),
                    "the companion's vehicle gun hung %d s %.2f rad off %s" % (len(run), run[0][3], enemy),
                    [run[0][0], run[-1][0]], key=enemy))

    for l in ctx.lines(ctx.companion):
        m = RE_AIM.match(l.text)
        if not m:
            continue
        enemy = m.group(1)
        off = abs(float(m.group(2))) + abs(float(m.group(3)))
        view = (float(m.group(4)), float(m.group(5)), float(m.group(6)))
        if run and run[-1][1] == enemy and off > OFF and _angle(run[-1][2], view) < STILL:
            run.append((l, enemy, view, off))
            continue
        close()
        run = [(l, enemy, view, off)] if off > OFF else []
    close()
    return out


DETECTORS = [d19_gun]
