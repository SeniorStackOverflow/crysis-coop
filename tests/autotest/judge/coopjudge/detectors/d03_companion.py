"""D03 unexpected disconnects, D04 the companion lost from the game, D16 the
companion's join stalls.

What the logs say (CryNetwork, CoopAgent.cpp, CoopSave.cpp):

host
  [net 13:54:55.987] Disconnect host-pc:64822; profid=0; cause=4; msg=''
      cause is EDisconnectionCause (CryCommon/INetwork.h): 0 timeout, 4 server
      full (CryAction's LoadGame ends with SetMaxPlayers(1), which kicks every
      client unless CoopSave's KeepBegin skipped that call), 13 nub destroyed
      (the host's own server went down: a level load or a quit), 14 ICMP
      error (the peer's process is gone), 19 the local player left.
      "[remote]" lines and msg 'Remote disconnected: Remote disconnected: ...'
      are the peer's echo of a disconnect the host logged already.
  [CoopAgent] AI companion started (process N, joins ...) in N ms
  [CoopAgent] AI companion stopped (turned off in the co-op menu)
  [CoopAgent] AI companion stopped (the host is not in a co-op game)    30 s out of a coop game
  [CoopAgent] the AI companion's game ended                             its process ended by itself
  [CoopAgent] the AI companion's game could not be started (error N)
  [CoopAgent] told the host: AI companion cannot join your game. ...    not in for JOIN_TIMEOUT (150 s)
  [CoopAgent] told the host: AI companion cannot join: its game did not start. ...
  Nomad(1)$o renamed to $8Companion      (cut to "...Compani" when the line has a time stamp)
  [CoopPos] server: Nomad=(..)hp200 Companion=(..)hp200                 every 5 s of play
companion
  [CoopAgent] its level loads: a break it makes again waits 3 frames ...   its join load begins
  [CoopAgent] its level loaded: a break it makes again waits 140 frames again ...
      the join load is over; also written right after it was thrown out while loading
  303456E0 times out                     a break of the host's its game could not make again
  [CoopAgent] out of the host's game: joining again in 5 s
  [net 13:57:54.326] connection requested to: 127.0.0.1:61186
  [CoopAgent] relay here: N packets to the host, M back    1 s apart for the first 10, then >= 60 s

A load's cause=4 is a FAIL only for a companion that was in the game (the
last [CoopPos] server line before it names it, or its join is nearer). One
still joining (its level loading) is thrown out and joins again by design:
WARN kick_joining_load; most of goal_runs' kicks are of this kind, in runs
where D04/D16 report the join that never ended.

A normal quit of the host writes, on the host: Disconnect <local>:64087 cause=19
'User left the game', <local>:16 cause=13, <peer> cause=13 'Nub destroyed',
System:Quit; on the companion: Disconnect cause=13 'Remote disconnected: Nub
destroyed', then "out of the host's game". None of that is reported.

Core gap worked around here: events.RE_JOINED wants "renamed to Companion" at
the end of the line, but a line with a time stamp is cut ("renamed to
$8Compani"), so ctx.ev.joins misses those joins; _in_game() also takes the cut
name and [CoopPos] lines that name the companion.
"""

import re

from ..findings import Finding

RE_DISC = re.compile(r"Disconnect (\S+); profid=-?\d+; cause=(\d+); msg='(.*?)'?$")
RE_NET = re.compile(r"^\[net (\d\d):(\d\d):(\d\d)(?:\.(\d+))?\]")
RE_RENAMED = re.compile(r"renamed to (\S+)$")
RE_POS_NAME = re.compile(r" ([^ =]+)=\(")
RE_TIMES_OUT = re.compile(r"^[0-9A-Fa-f]{8} times out$")
RE_AGENT_NAME = re.compile(r"^\s*coop_agent_name = (\S+)")
RE_CUT_START = re.compile(r"^\[CoopCutscene\] (\S+) started \(")
RE_CUT_END = re.compile(r"^\[CoopCutscene\] (\S+) ended at ")
RE_CUT_STOP = re.compile(r"^\[CoopCutscene\] stop (\S+)$")
RE_NO_PACKET = re.compile(r"no packet for ([\d.]+) seconds")
RE_FRIENDS = re.compile(r"^\[CoopRelay\] (\d+) friend\(s\) connected")
RE_STOPPED = re.compile(r"^\[CoopAgent\] AI companion stopped \((.*)\)")
RE_FRAMES = re.compile(r"\(\d+ in ([\d.]+) s\)")

STARTED = "[CoopAgent] AI companion started ("
ENDED = "[CoopAgent] the AI companion's game ended"
NOT_STARTED = "[CoopAgent] the AI companion's game could not be started"
TOLD = "[CoopAgent] told the host: AI companion cannot join"
TOLD_NO_GAME = "its game did not start"
READY = "[CoopAgent] the host's game is ready"
POS = "[CoopPos] server:"
KEEP_STAY = "friend(s) stay in the game through the load"
KEEP_NONE = "[CoopSave] no friend in the game to keep through the load"
KEEP_OLD_WAY = "the load disconnects the friends"
C_LOADS = "[CoopAgent] its level loads:"
C_LOADED = "[CoopAgent] its level loaded:"
C_OUT = "[CoopAgent] out of the host's game"
C_STALL = "[CoopAgent] connected "    # ... s and its level never began to load: out, a new relay, in again
C_CONNECT = "connection requested to:"
C_JOINING = "[CoopAgent] joining the host:"
C_RELAY = "[CoopAgent] relay here:"

CAUSE_EN = ["timeout", "protocol error", "resolve failed", "version mismatch", "server full",
            "kicked", "banned", "context corruption", "authentication failed", "not logged in",
            "CD key check failed", "game error", "not DX10 capable", "nub destroyed",
            "ICMP error (unreachable)", "NAT negotiation error", "PunkBuster", "demo finished",
            "demo file not found", "user left", "no controller", "cannot connect", "mod mismatch",
            "map not found", "map version", "unknown"]

JOIN_TIMEOUT = 150      # CoopAgent.h: seconds after which the host says the companion cannot join
POS_SECONDS = 5         # one [CoopPos] server line per 5 s of play (CoopAI.cpp LogPlayerPositions)
STALL_SECONDS = 90      # a join load cut off this long after its connect, with break replays timing out, was stuck


def cause_en(c):
    return CAUSE_EN[c] if 0 <= c < len(CAUSE_EN) else "cause %d" % c


# ---------------------------------------------------------------- helpers

def net_time(line):
    """Seconds since midnight from "[net hh:mm:ss.mmm]" (UTC, unlike the
    <hh:mm:ss> stamp, which is local time), or None."""
    m = RE_NET.match(line.text)
    if m:
        return int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3)) + float("0." + (m.group(4) or "0"))
    return None


def companion_name(ctx):
    if ctx.companion is not None:
        for l in ctx.companion.lines[:400]:
            m = RE_AGENT_NAME.match(l.text)
            if m:
                return m.group(1)
    return "Companion"


def pos_names(line):
    return RE_POS_NAME.findall(line.text[len(POS):])


def in_game_lines(ctx, name):
    """Host lines showing the companion in the game: its join (ctx.ev.joins,
    a cut "renamed to Compani") or a [CoopPos] line naming it."""
    key = "_d03_ingame_" + name
    cached = getattr(ctx, key, None)
    if cached is not None:
        return cached
    out = set(l.no for l in ctx.ev.joins)
    for l in ctx.host.lines:
        s = l.text
        if s.startswith(POS):
            if (" %s=(" % name) in s:
                out.add(l.no)
        elif "renamed to " in s:
            m = RE_RENAMED.search(s)
            if m and len(m.group(1)) >= 5 and name.startswith(m.group(1)):
                out.add(l.no)
    out = sorted(out)
    setattr(ctx, key, out)
    return out


def load_window(ld):
    """First and last line of a load: its start to "the saved game took" or
    its end (the kick is written a frame after LoadGame at the latest)."""
    last = ld.took_line.no if ld.took_line else (ld.end.no if ld.end else ld.start.no + 400)
    if ld.end is not None and last < ld.end.no <= last + 60:
        last = ld.end.no
    return ld.start.no - 2, last + 2


def load_of(ctx, line):
    for i, ld in enumerate(ctx.ev.loads):
        a, b = load_window(ld)
        if a <= line.no <= b:
            return i, ld
    return None, None


def is_level_load(ctx, ld):
    """A load right after a level load (Continue, a level change): the friends
    join again anyway. Old builds do not write "runs already" for an in-place
    load, so a load with no level load in the 150 lines before it counts as
    in place too."""
    if ld.inplace:
        return False
    end = ld.end.no if ld.end else ld.start.no + 400
    return any(ld.start.no - 150 <= lv[0].no <= end for lv in ctx.ev.levels)


def inplace_number(ctx, ld):
    n = 0
    for x in ctx.ev.loads:
        if not is_level_load(ctx, x):
            n += 1
        if x is ld:
            return n
    return n


def play_seconds(ctx, first, last):
    """Seconds of play between host lines first..last: by time stamps when the
    log has them, else 5 s per [CoopPos] server line outside cutscenes."""
    lines = ctx.host.slice(first, last)
    stamped = [l for l in lines if l.t is not None]
    if len(stamped) >= 2:
        return (stamped[-1].t - stamped[0].t) % 86400
    n = 0
    cut = set()
    for l in lines:
        s = l.text
        if s.startswith("[CoopCutscene] "):
            m = RE_CUT_START.match(s)
            if m and m.group(1) != "black_bars":
                cut.add(m.group(1))
                continue
            m = RE_CUT_END.match(s) or RE_CUT_STOP.match(s)
            if m:
                cut.discard(m.group(1))
        elif s.startswith(POS) and not cut:
            n += 1
        elif s.startswith("*LOADING: Level") or s.startswith("[CoopSave] loading "):
            cut.clear()         # a load ends a cutscene that wrote no end
    return n * POS_SECONDS


def who_dropped(ctx, line, name, ld=None):
    """('companion' | 'friend' | 'player', label) for a host Disconnect line:
    who was in the [CoopPos] lines before it and is missing after it."""
    lines = ctx.host.lines
    before = after = None
    for l in reversed(lines[max(0, line.no - 600):line.no - 1]):
        if l.text.startswith(POS):
            before = set(pos_names(l))
            break
    start_after = (load_window(ld)[1] if ld else line.no)
    for l in lines[start_after:start_after + 600]:
        if l.text.startswith(POS):
            after = set(pos_names(l))
            break
    friends = 0
    for l in lines[:line.no]:
        m = RE_FRIENDS.match(l.text)
        if m:
            friends = max(friends, int(m.group(1)))
    if before is not None:
        lost = before - (after or set())
        if name in lost:
            return "companion", name
        lost = sorted(x for x in lost if x != "Nomad")
        if lost:
            return "friend", lost[0]
        if name in before and after is not None and name in after:
            # the companion stayed: somebody who was still joining
            return ("friend", "?") if friends else ("player", None)
    # not in the game yet (it was joining): the companion when one was started
    # and no friend ever came through the relay
    started = any(l.text.startswith(STARTED) for l in lines[:line.no])
    if started and not friends:
        return "companion", name
    return ("friend", "?") if friends else ("player", None)


RU_WHO = {
    # nominative, accusative
    "companion": ("компаньон", "компаньона"),
    "friend": ("друг", "друга"),
    "player": ("второй игрок", "второго игрока"),
}


def back_in_game(ctx, name, after_no):
    for no in in_game_lines(ctx, name):
        if no > after_no:
            return no
    return None


def in_game_at(ctx, name, line):
    """Was the companion in the host's game right before this Disconnect line?
    The nearest of: a [CoopPos] server line (naming it or not), its join, an
    earlier Disconnect of the same peer, its start or a level load. A companion
    that is still joining (its level loading) is not in the game: a load or a
    level load throws it out and it joins again, by design."""
    ing = set(in_game_lines(ctx, name))
    m = RE_DISC.search(line.text)
    peer = m.group(1) if m else None
    for l in reversed(ctx.host.lines[:line.no - 1]):
        s = l.text
        if s.startswith(POS):
            return (" %s=(" % name) in s
        if l.no in ing:
            return True
        if s.startswith(STARTED) or s.startswith("*LOADING: Level"):
            return False
        if peer and "Disconnect " in s and "[remote]" not in s:
            m = RE_DISC.search(s)
            if m and m.group(1) == peer:
                return False
    return False


# ---------------------------------------------------------------- D03

def d03_disconnects(ctx):
    host = ctx.host
    name = companion_name(ctx)
    out = []
    seen = {}     # (peer, cause) -> line no of the last one reported
    logged = {}   # peer -> (line no, cause) of the host's last own Disconnect line of it
    lines = ctx.lines(host)
    for l in lines:
        s = l.text
        if "Disconnect " not in s:
            continue
        m = RE_DISC.search(s)
        if not m or "[remote]" in s:
            continue
        peer, cause, msg = m.group(1), int(m.group(2)), m.group(3)
        if peer.startswith("<local>"):
            continue                    # the host's own client and nub: a quit or its server restarting
        prev = logged.get(peer)
        if msg.startswith("Remote disconnected: Remote disconnected"):
            if prev and prev[1] == cause and l.no - prev[0] < 60:
                continue                # the peer's echo of the disconnect logged just before
            # no line of the host's own before it: the host cut a connect
            # while it was made (adk_new2_host.log 2174: refused, cause=4)
        else:
            logged[peer] = (l.no, cause)
        if cause in (13, 19):
            continue                    # the host's server went down (level load, quit); the peer left
        k = (peer, cause)
        if k in seen and l.no - seen[k] < 40:
            continue
        seen[k] = l.no
        if cause == 14 and any(RE_STOPPED.match(x.text) or x.text.startswith(ENDED)
                               for x in host.slice(max(1, l.no - 300), l.no - 1)):
            continue                    # the companion's game was stopped (or it ended: D04 says so)
        i, ld = load_of(ctx, l)
        kind, label = who_dropped(ctx, l, name, ld if cause == 4 else None)
        nom, acc = RU_WHO[kind]
        if kind == "friend" and label != "?":
            nom, acc = "%s %s" % (nom, label), "%s %s" % (acc, label)
        back = back_in_game(ctx, name, l.no) if kind == "companion" else None
        # still joining: the companion not in the game by the host's lines; a
        # friend or player nobody saw go from the [CoopPos] lines
        joining = not in_game_at(ctx, name, l) if kind == "companion" else label in ("?", None)
        who_en = ("the companion (%s), still joining" if joining else "the companion (%s)") % name if kind == "companion" else (
            ("friend %s" % label if label != "?" else "a friend who was still joining") if kind == "friend" else "a player who was still joining")
        back_en = ""
        gone = False        # not back, and the log has a minute of play after it to show that
        if kind == "companion":
            if back:
                back_en = "; in the game at line %d" % back
            else:
                tail_secs = play_seconds(ctx, l.no, len(host.lines))
                gone = tail_secs >= 60
                back_en = "; not in the game in the %d s of play the log has after it" % tail_secs
        base_en = "Disconnect %s cause=%d (%s) msg='%s' at %s: %s" % (peer, cause, cause_en(cause), msg, l.where(), who_en)
        if cause == 4 and ld is not None:
            level = is_level_load(ctx, ld)
            a, b = load_window(ld)
            notes = []
            for x in host.slice(max(1, ld.start.no - 40), b):
                if KEEP_STAY in x.text:
                    notes.append(("keep", x))
                elif x.text.startswith(KEEP_NONE):
                    notes.append(("none", x))
                elif KEEP_OLD_WAY in x.text:
                    notes.append(("old", x))
            ev = [l, ld.start] + [x for _, x in notes[:1]] + ([ld.took_line] if ld.took_line else [])
            later = " (потом вошёл заново)" if back else (" (и больше не вошёл)" if gone else "")
            if level:
                # the level load threw everybody out (cause=13); the
                # companion that came back in during it is thrown out again
                out.append(Finding(
                    "D03", "WARN",
                    "%s выкинуло при загрузке чекпойнта сразу после загрузки уровня%s" % (
                        acc, later if kind == "companion" else ""),
                    "%s, inside the load after a level load (%s at line %d)%s" % (base_en, ld.save, ld.start.no, back_en),
                    ev, key="kick_level_load"))
                continue
            if joining:
                # not in the game yet (its level loading): nothing a load
                # could keep; it joins again (D04/D16 say when it never does)
                out.append(Finding(
                    "D03", "WARN",
                    "загрузка чекпойнта выкинула %s, который ещё входил в игру: вход начался заново%s" % (
                        acc, " (потом вошёл)" if back else (" (так и не вошёл)" if gone else "")),
                    "%s, inside in-place load at lines %d..%d (%s): the load's SetMaxPlayers(1) cut its join%s" % (
                        base_en, ld.start.no, (ld.end.no if ld.end else b), ld.save, back_en),
                    ev, key="kick_joining_load"))
                continue
            n = inplace_number(ctx, ld)
            if notes and notes[0][0] == "keep":
                why = "the load had kept the friends ('%s') and still kicked: SetMaxPlayers was not skipped" % notes[0][1].text[:80]
            elif notes and notes[0][0] == "none":
                why = "the host found no friend to keep ('no friend in the game to keep'), though its [CoopPos] lines had the companion in"
            elif notes and notes[0][0] == "old":
                why = "the host meant to disconnect the friends ('%s')" % notes[0][1].text[:90]
            else:
                why = "no 'friend(s) stay in the game through the load' line: KeepBegin kept nobody, silently (or a build without it)"
            ru = "%s выкинуло из игры при загрузке чекпойнта (загрузка в запущенный уровень №%d)" % (acc, n)
            if gone:
                ru += " и он не вернулся"
            out.append(Finding(
                "D03", "FAIL", ru,
                "%s, inside in-place load #%d (lines %d..%d, %s, checkpoint %s): the load's SetMaxPlayers(1) kicked it; %s%s" % (
                    base_en, n, ld.start.no, (ld.end.no if ld.end else b), ld.save, ld.checkpoint, why, back_en),
                ev, key="kick_inplace_load"))
            continue
        if cause == 4:
            if joining:
                ru = "хост не пустил %s в игру: «сервер заполнен», и не во время загрузки" % acc
            else:
                ru = "%s выкинуло из игры с причиной «сервер заполнен», и не во время загрузки" % acc
            out.append(Finding(
                "D03", "FAIL", ru,
                base_en + " (not inside a checkpoint load)" + back_en,
                [l], key="server_full"))
            continue
        if cause == 0:
            mp = RE_NO_PACKET.search(msg)
            secs = (" %s с без связи" % mp.group(1)) if mp else ""
            side = "the host heard nothing from it" if not msg.startswith("Remote") else "it heard nothing from the host (the host stood still?)"
            out.append(Finding(
                "D03", "FAIL",
                "%s отвалился по тайм-ауту%s (%s)" % (nom, ", пока входил в игру" if joining else "", secs.strip() or "нет связи"),
                "%s: %s%s" % (base_en, side, back_en),
                [l], key="timeout"))
            continue
        if cause == 14:
            ins = {"companion": "компаньоном", "friend": "другом", "player": "вторым игроком"}[kind]
            if kind == "friend" and label != "?":
                ins += " " + label
            out.append(Finding(
                "D03", "FAIL",
                "связь с %s потеряна: его игра недоступна (закрылась?)" % ins,
                base_en + " without an 'AI companion stopped' before it" + back_en,
                [l], key="unreachable"))
            continue
        verdict = "WARN" if cause in (5, 6) else "FAIL"
        out.append(Finding(
            "D03", verdict,
            "%s отключился от игры (%s)" % (nom, cause_en(cause)),
            base_en + back_en,
            [l], key="cause%d" % cause))
    return out


# ---------------------------------------------------------------- D04

def _host_disconnects(ctx):
    """[(net seconds, cause, line)] of the host's own Disconnect lines."""
    out = []
    for l in ctx.host.lines:
        if "Disconnect " in l.text and "[remote]" not in l.text:
            m = RE_DISC.search(l.text)
            t = net_time(l)
            if m and t is not None:
                out.append((t, int(m.group(2)), l))
    return out


def d04_companion_lost(ctx):
    host = ctx.host
    name = companion_name(ctx)
    hl = host.lines
    out = []
    lines = ctx.lines(host)
    starts = [l for l in hl if l.text.startswith(STARTED)]
    joined = in_game_lines(ctx, name)

    def window_of(no):
        """The start of the companion's game this host line belongs to, and the next start."""
        prev = nxt = None
        for s in starts:
            if s.no <= no:
                prev = s
            elif nxt is None:
                nxt = s
        return prev, nxt

    def joins_between(a, b):
        return [n for n in joined if a < n < b]

    told_windows = set()
    for l in lines:
        s = l.text
        if s.startswith(TOLD):
            start, nxt = window_of(l.no)
            end = nxt.no if nxt else len(hl) + 1
            if TOLD_NO_GAME in s:
                out.append(Finding(
                    "D04", "FAIL", "игра компаньона не запустилась (хост показал «AI companion cannot join: its game did not start»)",
                    "%s: '%s' (5 starts used up)" % (l.where(), s[:120]),
                    [l] + ([start] if start else []), key="no_start"))
                continue
            told_windows.add(start.no if start else 0)
            later = joins_between(l.no, end)
            earlier = joins_between(start.no if start else 0, l.no)
            if later:
                j = host.lines[later[0] - 1]
                secs = play_seconds(ctx, l.no, j.no)
                if secs <= 30:
                    # an older build told after 90 s; the companion came in right after
                    out.append(Finding(
                        "D04", "WARN",
                        "хост показал «компаньон не может войти», а тот вошёл через %d с" % secs,
                        "%s: '%s', yet the companion is in the game at %s (%d s of play later): told too early" % (
                            l.where(), s[:70], j.where(), secs),
                        [l, j], key="told_then_joined"))
                else:
                    out.append(Finding(
                        "D04", "FAIL",
                        "компаньон долго не мог войти в игру: хост показал «AI companion cannot join», вошёл только через %d с после этого" % secs,
                        "%s: '%s'; the companion came in only at %s, %d s of play later" % (
                            l.where(), s[:70], j.where(), secs),
                        [l, j] + ([start] if start else []), key="join_late"))
            elif earlier:
                last_in = host.lines[earlier[-1] - 1]
                out.append(Finding(
                    "D04", "FAIL",
                    "компаньон выпал из игры и не смог вернуться (хост показал «AI companion cannot join»)",
                    "%s: '%s'; the companion was last in the game at %s and did not come back%s" % (
                        l.where(), s[:70], last_in.where(), (" until its game was started again at %s" % nxt.where()) if nxt else ""),
                    [l, last_in] + ([start] if start else []), key="cannot_join"))
            else:
                out.append(Finding(
                    "D04", "FAIL",
                    "компаньон не смог войти в игру (хост показал «AI companion cannot join»)",
                    "%s: '%s'; never in the game since its start at %s" % (
                        l.where(), s[:70], start.where() if start else "?"),
                    [l] + ([start] if start else []), key="cannot_join"))
        elif s.startswith(NOT_STARTED):
            out.append(Finding(
                "D04", "FAIL", "игра компаньона не запустилась",
                "%s: '%s'" % (l.where(), s[:120]), [l], key="no_start"))
        elif s.startswith(ENDED):
            start, nxt = window_of(l.no)
            if nxt is not None:
                out.append(Finding(
                    "D04", "FAIL", "игра компаньона закрылась сама (упала?) и была запущена заново",
                    "%s: the companion's process ended by itself; started again at %s" % (l.where(), nxt.where()),
                    [l, nxt], key="game_ended"))
            elif play_seconds(ctx, l.no, len(hl)) >= 30:
                out.append(Finding(
                    "D04", "FAIL", "игра компаньона закрылась сама (упала?)",
                    "%s: the companion's process ended by itself, %d s of play before the log ends, not started again" % (
                        l.where(), play_seconds(ctx, l.no, len(hl))),
                    [l], key="game_ended"))
        else:
            m = RE_STOPPED.match(s)
            if not m:
                continue
            why = m.group(1)
            if why.startswith("turned off"):
                continue            # the player turned it off
            start, nxt = window_of(l.no)
            later_play = any(x.text.startswith(POS) for x in hl[l.no:])
            if nxt is None and not later_play:
                continue            # the end of the game: the host left the coop game and quit
            out.append(Finding(
                "D04", "WARN",
                "компаньона остановили: хост больше 30 с был не в кооп-игре" + (" (потом его запустили заново)" if nxt else ""),
                "%s: 'AI companion stopped (%s)' in the middle of the game%s" % (
                    l.where(), why, (", started again at %s" % nxt.where()) if nxt else ""),
                [l] + ([nxt] if nxt else []), key="stopped_mid_game"))

    # a start the player did not ask for: no stop, no ended line before it
    # (StartCompanion runs only when its game is not running: stopped, ended
    # or never started, each with its line)
    for i, st in enumerate(starts[1:], 1):
        if not ctx.in_window(st):
            continue
        between = hl[starts[i - 1].no:st.no - 1]
        if any(RE_STOPPED.match(x.text) or x.text.startswith(ENDED) or x.text.startswith(NOT_STARTED) for x in between):
            continue
        out.append(Finding(
            "D04", "FAIL", "компаньона запустили заново без запроса",
            "%s: a second 'AI companion started' with no 'stopped'/'game ended' after the one at %s" % (
                st.where(), starts[i - 1].where()),
            [st, starts[i - 1]], key="restart"))

    # started, never in the game, nothing told (or an old build)
    for i, st in enumerate(starts):
        if not ctx.in_window(st) or st.no in told_windows:
            continue
        end = starts[i + 1].no if i + 1 < len(starts) else len(hl) + 1
        js = joins_between(st.no, end)
        until = js[0] if js else end - 1
        # the companion may join only once the host's game is ready (no load, no cutscene)
        from_no = st.no
        for x in hl[st.no:until - 1]:
            if x.text.startswith(READY):
                from_no = x.no
        for ld in ctx.ev.loads:
            if st.no < ld.start.no < until and ld.end is not None:
                from_no = max(from_no, ld.end.no)
        secs = play_seconds(ctx, from_no, until)
        if js:
            if secs >= JOIN_TIMEOUT + 30:
                j = hl[js[0] - 1]
                out.append(Finding(
                    "D04", "FAIL", "компаньон долго не мог войти в игру: вошёл только через %d с" % secs,
                    "AI companion started at %s, first in the game at %s, %d s of play after line %d (the host gives up after %d s)" % (
                        st.where(), j.where(), secs, from_no, JOIN_TIMEOUT),
                    [j, st], key="join_late"))
            continue
        if secs >= JOIN_TIMEOUT:
            out.append(Finding(
                "D04", "FAIL", "компаньон не смог войти в игру",
                "AI companion started at %s, never in the game (no join, no [CoopPos] naming %s) in %d s of play after line %d" % (
                    st.where(), name, secs, from_no),
                [st], key="cannot_join"))
        elif secs >= 60 and not (i == len(starts) - 1 and ctx.companion is not None and d16_join_stall(ctx)):
            # (the companion's log is of its last start: D16 says why it is not in)
            out.append(Finding(
                "D04", "WARN", "компаньон не успел войти в игру до конца лога (%d с)" % secs,
                "AI companion started at %s, not in the game in the %d s of play the log has after line %d" % (
                    st.where(), secs, from_no),
                [st], key="not_in_yet"))

    # the companion's own log: thrown out of the host's game
    comp = ctx.companion
    if comp is not None:
        host_disc = _host_disconnects(ctx)
        cl = comp.lines
        for o in ctx.lines(comp):
            if not o.text.startswith(C_OUT):
                continue
            # its Disconnect line: after its last connect (63 lines before the
            # "out" in the user's companion.log, 249)
            d = None
            for x in reversed(cl[max(0, o.no - 1000):o.no - 1]):
                if "Disconnect " in x.text and "[remote]" not in x.text and RE_DISC.search(x.text):
                    d = x
                    break
                if C_CONNECT in x.text or x.text.startswith(C_OUT):
                    break
            cause = int(RE_DISC.search(d.text).group(2)) if d else None
            if cause == 13:
                continue            # the host's server went down: a level load or its quit
            # it left by itself: its join had stalled, it joins again on a
            # new relay (CoopAgent.cpp; D20 notes the stall)
            if any(x.text.startswith(C_STALL) and "never began to load" in x.text for x in cl[max(0, o.no - 60):o.no - 1]):
                continue
            t = net_time(d) if d else None
            if t is not None and any(abs(((ht - t + 43200) % 86400) - 43200) <= 5 and hc not in (13, 19)
                                     for ht, hc, _ in host_disc):
                continue            # the host logged it: D03
            msg = RE_DISC.search(d.text).group(3) if d else ""
            out.append(Finding(
                "D04", "FAIL",
                "компаньон выпал из игры хоста (%s)" % (cause_en(cause) if cause is not None else "без сообщения о разрыве"),
                "%s: 'out of the host's game'%s" % (
                    o.where(), (" after %s: Disconnect cause=%d (%s) msg='%s'" % (d.where(), cause, cause_en(cause), msg)) if d else " with no Disconnect line before it"),
                ([d, o] if d else [o]), key="out_cause%s" % cause))
    return out


# ---------------------------------------------------------------- D16

def _ru_breaks(n):
    if n % 10 == 1 and n % 100 != 11:
        return "%d разрушение" % n
    if 2 <= n % 10 <= 4 and not 12 <= n % 100 <= 14:
        return "%d разрушения" % n
    return "%d разрушений" % n


def d16_join_stall(ctx):
    comp = ctx.companion
    if comp is None:
        return []
    cl = comp.lines
    out = []
    name = companion_name(ctx)
    in_game = " %s=(" % name
    # the index of each "relay here" line: after the 10th, the next comes >= 60 s later
    relay_idx = {}
    n = 0
    for l in cl:
        if l.text.startswith(C_RELAY):
            n += 1
            relay_idx[l.no] = n
    # an old build wrote no "its level loads": its join begins at "joining the host"
    begin = C_LOADS if any(l.text.startswith(C_LOADS) for l in cl) else C_JOINING
    hl = ctx.host.lines
    host_starts = [l for l in hl if l.text.startswith(STARTED)]
    last_start = host_starts[-1].no if host_starts else 0
    host_in_after = [j for j in in_game_lines(ctx, name) if j > last_start]
    host_told = [l for l in hl if l.text.startswith(TOLD) and l.no > last_start]
    n_joins = sum(1 for l in cl if l.text.startswith(begin))
    window = ctx.lines(comp)
    first = window[0].no if window else 1
    last = window[-1].no if window else 0
    for L in cl[first - 1:last]:
        if not L.text.startswith(begin):
            continue
        timeouts, disc, loaded, outl, relay, frames_s = [], None, None, None, [], 0.0
        last_seen = L
        again = False
        for x in cl[L.no:]:
            s = x.text
            if s.startswith(C_LOADED):
                loaded = x
                break
            if s.startswith(begin):
                again = True
                break
            last_seen = x
            if s.startswith(C_OUT):
                outl = x
                break
            if RE_TIMES_OUT.match(s):
                timeouts.append(x)
            elif s.startswith(C_RELAY):
                relay.append(x)
            elif s.startswith("[CoopPos] client:") and in_game in s:
                loaded = x      # its player is there (an old build wrote no "its level loaded")
                break
            elif s.startswith("[CoopFrames] "):
                m = RE_FRAMES.search(s)
                if m:
                    frames_s += float(m.group(1))
            elif "Disconnect " in s and "[remote]" not in s and RE_DISC.search(s):
                disc = x
        late = [r for r in relay if relay_idx.get(r.no, 0) >= 11]
        if loaded is not None:
            # in, but later than the host waits (none in goal_runs: the
            # longest took 131 s, final_on1)
            durs = [frames_s] if frames_s else []
            if L.t is not None and loaded.t is not None:
                durs.append((loaded.t - L.t) % 86400)
            if len(late) >= 2:
                durs.append(60 * (len(late) - 1))
            dur = int(max(durs)) if durs else 0
            if dur >= JOIN_TIMEOUT:
                out.append(Finding(
                    "D16", "FAIL",
                    "компаньон входил в игру слишком долго: загрузка заняла %d с%s" % (
                        dur, (", его игра не смогла повторить %s хоста" % _ru_breaks(len(timeouts))) if timeouts else ""),
                    "join at %s finished only at %s, %d s or more (the host gives up after %d s); %d '%%p times out' lines" % (
                        L.where(), loaded.where(), dur, JOIN_TIMEOUT, len(timeouts)),
                    [L, loaded] + timeouts[:1], key="join_slow"))
            continue
        cause = int(RE_DISC.search(disc.text).group(2)) if disc else None
        # how long it loaded: time stamps; its connect to the disconnect (net
        # stamps); the test frame log; at least 60 s for each gap between late
        # "relay here" lines
        conn = None
        if begin == C_JOINING:
            for x in cl[L.no:L.no + 5]:     # right after "joining the host"
                if C_CONNECT in x.text:
                    conn = x
                    break
        else:
            for x in reversed(cl[max(0, L.no - 150):L.no - 1]):
                if C_CONNECT in x.text:
                    conn = x
                    break
        durs = []
        end_line = disc or outl or last_seen
        if L.t is not None and end_line.t is not None:
            durs.append((end_line.t - L.t) % 86400)
        t0 = net_time(conn) if conn else None
        t1 = net_time(disc) if disc else None
        if t0 is not None and t1 is not None:
            durs.append((t1 - t0) % 86400)
        if frames_s:
            durs.append(frames_s)
        if len(late) >= 2:
            durs.append(60 * (len(late) - 1))
        dur = int(max(durs)) if durs else None
        long_load = dur is not None and dur >= JOIN_TIMEOUT
        # break replays that time out are normal in a join that finishes (18
        # of 53 in goal_runs, up to 25 lines, joins up to 131 s): they count
        # only in a join that has gone on for STALL_SECONDS
        slow_replays = bool(timeouts) and dur is not None and dur >= STALL_SECONDS
        tail = []
        if disc is not None or outl is not None:
            # thrown out while loading: by a load (cause=4, D03), the host's
            # level load or quit (13); stuck only when it had loaded that long
            if not (long_load or slow_replays):
                continue
        elif again:
            if not long_load:
                continue        # (old build) it connected again: that try is judged on its own
        else:
            # its log ends while it loads: a copy taken early, or stuck. Stuck
            # when the host's log never has it in the game after its start
            # and the host said it cannot join after this join began (by the
            # time stamps; without them only when this is its one join)
            if host_in_after:
                continue
            told_after = [t for t in host_told
                          if (t.t is not None and L.t is not None and (t.t - L.t) % 86400 < 43200)
                          or (t.t is None or L.t is None) and n_joins == 1]
            if not (long_load or slow_replays or told_after):
                continue
            tail = told_after[:1]
        if long_load:
            reason = "it loaded for %d s or more (the host gives up after %d s)" % (dur, JOIN_TIMEOUT)
        elif slow_replays:
            reason = "break replays timed out and it had not finished after %d s" % dur
        else:
            reason = "its log ends while loading and the host said it cannot join after this join began"
        ended = ("a disconnect, cause=%d (%s)" % (cause, cause_en(cause))) if disc else (
            "'out of the host's game'" if outl else "the end of its log")
        ev = [L] + timeouts[:1] + (timeouts[-1:] if len(timeouts) > 1 else []) + \
            ([disc] if disc else []) + ([outl] if outl else []) + tail
        took = (" (%d с)" % dur) if dur else ""
        if timeouts:
            ru = "компаньон завис при входе в игру: его игра не смогла повторить %s хоста, загрузка%s не закончилась" % (
                _ru_breaks(len(timeouts)), took)
        else:
            ru = "компаньон завис при входе в игру: загрузка уровня%s не закончилась" % took
        out.append(Finding(
            "D16", "FAIL", ru,
            "join at %s did not finish ('%s' never came before %s): %s; %d '%%p times out' lines; %s%s" % (
                L.where(), "its level loaded" if begin == C_LOADS else "[CoopPos] client naming " + name, ended, reason,
                len(timeouts), ("loaded for %d s" % dur) if dur is not None else "time loading unknown",
                (", connect at %s" % conn.where()) if conn else ""),
            ev, key="join_stall"))
    return out


DETECTORS = [d03_disconnects, d04_companion_lost, d16_join_stall]
