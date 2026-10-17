"""D22: the AI companion's vehicle seat stays "entering": no gun, no shot.

A client's own player sits down in a seat as "entering" (CryAction's SitDown
does it for a player with a network channel); the seat's update makes it
"none" the next frame, and only then does the vehicle hand him its gun. The
engine updates no hidden vehicle, and the companion's game kept hidden the
vehicles of a continued campaign the server showed as in the level file: the
seat stayed "entering" for the whole ride and the companion never fired the
car's gun (the user's games of 2026-10-16/17, "seat state 1", "gun (0.00 0.00
0.00)"). The companion logs each change of its seat (CoopAgent.cpp):
  [CoopAgent] its seat gunner of <car>: transition -1 -> 1 (...), gun none; ...
  [CoopAgent] its seat gunner of <car>: transition 1 -> 0 (0.0 s after the last change), gun in hand; ...
  [CoopAgent] its seat: none any more (transition was 1 for 41.0 s)
  [CoopAgent] in <car>: seat gunner (gunner 1, driver 0, transition 1, gun 0), ... (every 5 s while so)
older builds only the state at the end of each gun aim line ("seat state 1").
Its game showing a vehicle it rode in, hidden there, is a note: the server's
sync missed it.
"""

import re

from ..findings import Finding

RE_SEAT = re.compile(r"^\[CoopAgent\] its seat \S+ of (\S+): transition (-?\d+) -> (-?\d+) \(([\d.]+) s after the last change\)")
RE_GONE = re.compile(r"^\[CoopAgent\] its seat: none any more \(transition was (-?\d+) for ([\d.]+) s\)")
RE_AIM = re.compile(r"^\[CoopAgent\] gun aim at \S+: .*, seat state (-?\d+)$")
RE_IN = re.compile(r"^\[CoopAgent\] in (\S+): seat \S+ \(gunner -?\d+, driver -?\d+, transition 1,")
RE_NET = re.compile(r"^\[CoopAgent\] (\S+) was hidden in its own game for 2 s while it rode in it: shown")

STUCK = 3.0        # s "entering"
AIM_LINES = 5      # gun aim lines in a row with "seat state 1" (one a second)


def d22_seat(ctx):
    if ctx.companion is None:
        return []
    out = []
    told = set()

    def stuck(vehicle, secs, secs_en, lines):
        key = vehicle or "seat"
        if key in told:
            return
        told.add(key)
        out.append(Finding(
            "D22", "FAIL",
            "место ИИ-компаньона в %s осталось «садится» %s: машина не дала ему пулемёт, он не стреляет" % (vehicle or "машине", secs),
            "the companion's seat in %s stayed 'entering' %s: no gun from the vehicle, no shot" % (vehicle or "a vehicle", secs_en),
            lines, key=key))

    entering = None     # (line, vehicle)
    ins = 0             # its vehicle traces (on a change, then every 5 s while "entering") still "entering"
    aims = []
    nets = []
    last = None
    for l in ctx.lines(ctx.companion):
        t = l.text
        last = l
        m = RE_SEAT.match(t)
        if m:
            vehicle, old, new, secs = m.group(1), int(m.group(2)), int(m.group(3)), float(m.group(4))
            if old == 1 and entering and secs > STUCK:
                stuck(vehicle, "%.0f с" % secs, "%.0f s" % secs, [entering[0], l])
            entering = (l, vehicle) if new == 1 else None
            ins = 0
            continue
        m = RE_GONE.match(t)
        if m:
            if int(m.group(1)) == 1 and float(m.group(2)) > STUCK:
                stuck(entering[1] if entering else None, "%.0f с" % float(m.group(2)), "%.0f s" % float(m.group(2)),
                      [entering[0], l] if entering else [l])
            entering = None
            continue
        m = RE_AIM.match(t)
        if m:
            if m.group(1) == "1":
                aims.append(l)
                if len(aims) == AIM_LINES:
                    stuck(entering[1] if entering else None, "(%d строк прицела подряд)" % AIM_LINES, "(%d aim lines in a row)" % AIM_LINES, [aims[0], l])
            else:
                aims = []
            continue
        m = RE_IN.match(t)
        if m and entering:
            ins += 1
            if ins == 2:
                stuck(entering[1], "(5 с и больше)", "(5 s or more)", [entering[0], l])
            continue
        m = RE_NET.match(t)
        if m:
            nets.append(l)
    # (the log ends with it still "entering")
    if entering and last is not None and entering[0].t is not None and last.t is not None and last.t - entering[0].t > 10.0:
        stuck(entering[1], "до конца лога (%.0f с)" % (last.t - entering[0].t), "to the end of the log (%.0f s)" % (last.t - entering[0].t), [entering[0], last])
    if nets:
        f = Finding(
            "D22", "WARN",
            "в игре компаньона машина, в которой он ехал, была скрыта (сервер не сообщил, что она видна): компаньон показал её сам",
            "a vehicle the companion rode in was hidden in its game (the server's sync missed it): shown by the companion",
            nets[:3], key="hidden")
        f.count = len(nets)
        out.append(f)
    return out


DETECTORS = [d22_seat]
