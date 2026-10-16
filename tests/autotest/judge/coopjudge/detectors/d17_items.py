"""D17: things piling up in the world after loads.

Coop.dll counts the things (items) after every load, before and after it
equips the soldiers again (CoopSave.cpp RearmAfterLoad):

  [CoopSave] things after the load: 1113 held, 32 orphans (22 of them visible),
  905 owned by nobody (690 hidden), 0 held by things; after the soldiers'
  equipment: 1215 held, 32 orphans (22 of them visible), 2113 owned by nobody
  (1899 hidden), 0 held by things

Before the fix of 2026-10-15 the soldiers' old things stayed, hidden and
nobody's, ~1200 more after every load; a load into the running level that
kept the world then kept them too ("orphans", ~1100 more each time).
"""

import re

from ..findings import Finding

_COUNTS = r"(\d+) held, (\d+) orphans \((\d+) of them visible\), (\d+) owned by nobody \((\d+) hidden\), (\d+) held by things"
RE_THINGS = re.compile(r"^\[CoopSave\] things after the load: " + _COUNTS + r"; after the soldiers' equipment: " + _COUNTS)

LEAK = 200        # hidden things nobody owns, more after equipping the soldiers
GROWTH = 200      # orphans more than after the previous load


def d17_items(ctx):
    out = []
    prev = None
    for l in ctx.lines(ctx.host):
        if not l.text.startswith("[CoopSave] things after"):
            continue
        m = RE_THINGS.match(l.text)
        if not m:
            continue
        v = [int(x) for x in m.groups()]
        before, after = v[:6], v[6:]
        leak = after[4] - before[4]
        if leak > LEAK:
            out.append(Finding(
                "D17", "FAIL",
                "после загрузки у солдат остаётся их старое оружие: +%d невидимых ничьих вещей за загрузку" % leak,
                "equipping the soldiers after the load left %d more hidden things owned by nobody (%d -> %d)" % (leak, before[4], after[4]),
                [l], key="rearm_leak"))
        if prev is not None and after[1] - prev[1] > GROWTH:
            out.append(Finding(
                "D17", "FAIL",
                "от загрузки к загрузке вещей-сирот всё больше: %d -> %d" % (prev[1], after[1]),
                "orphan things grew from %d to %d between loads" % (prev[1], after[1]),
                [l], key="orphans_grow"))
        prev = after
    return out


DETECTORS = [d17_items]
