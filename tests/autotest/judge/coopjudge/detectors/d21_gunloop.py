"""D21: a gunfire loop that played on and on.

A weapon's rapid-fire sound (Sounds/weapons:fy71:fire_3rd_loop ...) plays
while it fires and is stopped when it stops; once it was never stopped and the
gunfire went on for ever (the user's game of 2026-10-17, the fight with the
bridge truck). Since then the sound watch stops a fire loop that has played
15 s without a pause (no rifle fires that long), CoopSound.cpp:
  [CoopSound] a gunfire loop played on with nobody firing that long: stopped (3 so far: fy71_3rd_loop_close_71_a_short(15 s) ...)
Each such line is a fire sound something forgot to stop: a note, with its
names, so the cause can be found.
"""

import re

from ..findings import Finding

RE_LOOP = re.compile(r"^\[CoopSound\] a gunfire loop played on with nobody firing that long: stopped \((\d+) so far:(.*)\)$")


def d21_gunloop(ctx):
    hits = [l for l in ctx.lines(ctx.host) if RE_LOOP.match(l.text)]
    if not hits:
        return []
    names = sorted(set(n.split("(")[0] for l in hits for n in RE_LOOP.match(l.text).group(2).split()))
    f = Finding(
        "D21", "WARN",
        "звук очереди играл без остановки (никто так долго не стреляет): сторож звука заглушил его через 15 с (%s)" % ", ".join(names[:4]),
        "a gunfire loop nobody stopped played 15 s: the sound watch stopped it (%s)" % ", ".join(names),
        hits[:3], key="gunloop")
    f.count = len(hits)
    return [f]


DETECTORS = [d21_gunloop]
