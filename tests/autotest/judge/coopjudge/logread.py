"""Reads Crysis logs (Game.log, companion.log, test logs) into Line records.

The engine writes the console's colour codes ($0-$9) into the log, cuts lines
it cannot write, and with log_IncludeTime 1 puts "<hh:mm:ss> " in front of
each line. Most of the mod's lines are ASCII; player names and some engine
lines are in the system code page, so a line that is not UTF-8 is read as
cp1251.
"""

import os
import re

_TIME = re.compile(r"^<(\d\d):(\d\d):(\d\d)> ?")
_COLOUR = re.compile(r"\$\d")


class Line:
    __slots__ = ("no", "text", "t", "src")

    def __init__(self, no, text, t, src):
        self.no = no        # 1-based line number in the file
        self.text = text    # without time stamp and colour codes
        self.t = t          # seconds since midnight, or None
        self.src = src      # the Log it came from

    def where(self):
        return "%s:%d" % (self.src.name, self.no)

    def __repr__(self):
        return "<%s %s>" % (self.where(), self.text[:80])


class Log:
    """One log file. role is 'host' or 'companion' (or 'game' when unknown)."""

    def __init__(self, path, role=None, lines=None, name=None):
        self.path = path
        self.name = name or os.path.basename(path)
        self.role = role or guess_role(self.name)
        self.lines = lines if lines is not None else read_lines(path, self)

    def slice(self, first, last=None):
        """Lines with first <= no <= last (1-based, inclusive)."""
        last = last if last is not None else len(self.lines)
        return self.lines[max(0, first - 1):last]


def guess_role(name):
    n = name.lower()
    return "companion" if "companion" in n or "client" in n else "host"


def _decode(raw):
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return raw.decode("cp1251", errors="replace")


def read_lines(path, log=None):
    with open(path, "rb") as f:
        data = f.read()
    out = []
    for i, raw in enumerate(data.split(b"\n"), 1):
        s = _decode(raw.rstrip(b"\r"))
        t = None
        m = _TIME.match(s)
        if m:
            t = int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3))
            s = s[m.end():]
        if "$" in s:
            s = _COLOUR.sub("", s)
        out.append(Line(i, s, t, log))
    if out and not out[-1].text:
        out.pop()
    return out


def from_text(text, name="test.log", role=None):
    """A Log made from a string (for tests)."""
    log = Log.__new__(Log)
    log.path = name
    log.name = name
    log.role = role or guess_role(name)
    log.lines = []
    for i, s in enumerate(text.splitlines(), 1):
        t = None
        m = _TIME.match(s)
        if m:
            t = int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3))
            s = s[m.end():]
        log.lines.append(Line(i, _COLOUR.sub("", s), t, log))
    return log
