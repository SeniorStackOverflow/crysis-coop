"""Detectors: each module here has DETECTORS, a list of functions
f(ctx) -> list of Finding. A detector reads only ctx (never files), so the same
code judges a live run, an old run and the user's own Game.log.

ctx.host        Log of the host (the user's Game.log is a host log)
ctx.companion   Log of the AI companion, or None
ctx.ev          Events of the host log
ctx.lines(log)  the lines of log inside the window being judged
ctx.map_path    Coop.map of the DLL that wrote the logs, or None
"""

import importlib
import pkgutil


def all_detectors():
    out = []
    for m in sorted(pkgutil.iter_modules(__path__), key=lambda m: m.name):
        mod = importlib.import_module(__name__ + "." + m.name)
        out.extend(getattr(mod, "DETECTORS", []))
    return out
