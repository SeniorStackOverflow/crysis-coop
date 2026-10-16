"""Turns a host log into the things detectors ask about: loads, level loads,
the companion's joins.

A checkpoint load in the host's log:
  [CoopSave] <level> runs already: the checkpoint is loaded into it (no level load)   (only in place)
  [CoopSave] loading <save> (<level>, checkpoint <c>)                                    start
  [CoopSave] N friend(s) stay in the game through the load (M entities kept)            (in place, friends kept)
  [CoopSave] the saved game took X s to load
  [CoopSave] progress loaded: <level>, checkpoint <c> (saved ...)
  [Coop] checkpoint loaded: N players' places kept                                       end
"""

import re

RE_INPLACE = re.compile(r"^\[CoopSave\] (\S+) runs already: the checkpoint is loaded into it")
RE_LOAD_START = re.compile(r"^\[CoopSave\] loading (\S+) \((\S+), checkpoint ([^)]*)\)")
RE_TOOK = re.compile(r"^\[CoopSave\] the saved game took ([\d.]+) s to load")
RE_PROGRESS = re.compile(r"^\[CoopSave\] progress loaded: ")
RE_LOAD_END = re.compile(r"^\[Coop\] checkpoint loaded: (\d+) players' places kept")
RE_KEPT = re.compile(r"^\[CoopSave\] (\d+) friend\(s\) stay in the game through the load \((\d+) entities kept\)")
RE_LEVEL = re.compile(r"^\*LOADING: Level (\S+) loading time: ([\d.]+) seconds")
RE_JOINED = re.compile(r"renamed to (Companion|.*Companion)$")   # (a revive is no join)
RE_ALLDOWN = re.compile(r"^\[Coop\] everybody is down: back to the last checkpoint in")


class Load:
    """One checkpoint load. Line numbers are 1-based; None when not in the log."""

    def __init__(self, start):
        self.start = start          # the "loading" line
        self.inplace = False        # loaded into the running level
        self.level = None
        self.save = None
        self.checkpoint = None
        self.took = None            # seconds, from "the saved game took"
        self.took_line = None
        self.end = None             # "checkpoint loaded" (or "progress loaded") line
        self.friends_kept = None    # from "N friend(s) stay"
        self.entities_kept = None
        self.alldown = None         # the "everybody is down" line that led to it, if any

    def __repr__(self):
        return "<Load %s %s@%d..%s>" % ("inplace" if self.inplace else "level", self.save, self.start.no, self.end.no if self.end else "?")


class Events:
    def __init__(self, log):
        self.log = log
        self.loads = []
        self.levels = []    # (line, level, seconds)
        self.joins = []     # lines where the companion came into the game
        self.alldowns = []
        self._scan()

    def _scan(self):
        cur = None
        pending_inplace = None
        pending_alldown = None
        for l in self.log.lines:
            s = l.text
            if not s:
                continue
            c = s[0]
            if c == "[":
                if s.startswith("[CoopSave] "):
                    m = RE_INPLACE.match(s)
                    if m:
                        pending_inplace = l
                        continue
                    m = RE_LOAD_START.match(s)
                    if m:
                        cur = Load(l)
                        cur.save, cur.level, cur.checkpoint = m.group(1), m.group(2), m.group(3)
                        cur.inplace = pending_inplace is not None and l.no - pending_inplace.no < 40
                        cur.alldown = pending_alldown
                        pending_inplace = None
                        pending_alldown = None
                        self.loads.append(cur)
                        continue
                    if cur is not None and cur.end is None:
                        m = RE_KEPT.match(s)
                        if m:
                            cur.friends_kept = int(m.group(1))
                            cur.entities_kept = int(m.group(2))
                            continue
                        m = RE_TOOK.match(s)
                        if m:
                            cur.took = float(m.group(1))
                            cur.took_line = l
                            continue
                        if RE_PROGRESS.match(s) and cur.end is None:
                            cur.end = l
                            continue
                elif s.startswith("[Coop] "):
                    m = RE_LOAD_END.match(s)
                    if m and cur is not None and (cur.end is None or cur.end.no >= l.no - 5):
                        cur.end = l
                        continue
                    if RE_ALLDOWN.match(s):
                        pending_alldown = l
                        self.alldowns.append(l)
                        continue
            elif c == "*":
                m = RE_LEVEL.match(s)
                if m:
                    self.levels.append((l, m.group(1), float(m.group(2))))
                    continue
            if "Companion" in s and RE_JOINED.search(s):
                self.joins.append(l)

    def load_at(self, no):
        """The load whose start..end+30 lines contain line no, or None."""
        for ld in self.loads:
            end = ld.end.no if ld.end else ld.start.no + 400
            if ld.start.no - 10 <= no <= end + 30:
                return ld
        return None

    def window_after(self, load, seconds=30, lines=600):
        """Lines from the load's end to `seconds` later (by time stamp when the
        log has them, else the next `lines` lines)."""
        if load.end is None:
            return []
        log = self.log
        first = load.end.no + 1
        if load.end.t is not None:
            t0 = load.end.t
            out = []
            for l in log.lines[first - 1:]:
                if l.t is not None and (l.t - t0) % 86400 > seconds:
                    break
                out.append(l)
            return out
        return log.slice(first, first + lines - 1)
