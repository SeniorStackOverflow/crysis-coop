"""Runs every detector over a host log (and the companion's) and merges what
they find."""

import traceback

from .context import Context
from .detectors import all_detectors
from .findings import Finding, merge


def judge(host, companion=None, window=None, map_path=None):
    ctx = Context(host, companion, window, map_path)
    found = []
    for det in all_detectors():
        try:
            found.extend(det(ctx) or [])
        except Exception:
            found.append(Finding("D00", "INFRA", "ошибка анализатора: " + det.__name__,
                                 "detector %s raised: %s" % (det.__name__, traceback.format_exc(limit=3).strip().splitlines()[-1]),
                                 key=det.__name__))
    found = [f for f in found if not f.evidence or any(ctx.in_window(l) for l in f.evidence)]
    return ctx, merge(found)
