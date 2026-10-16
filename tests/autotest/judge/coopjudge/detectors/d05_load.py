"""D05 load integrity, D06 load stall, D13 all-down.

The host's lines they read (Code/Mod/CoopSave.cpp, Mod/Game/Scripts/Coop/CoopGameRules.lua):

D05  [CoopSave] <id> stays: bound 1 (now 1), multiplayer 0           RemoveNow could not remove it
     [CoopSave] <id> could not be removed                              (the same, older builds)
     [CoopSave] the load spawns <name> (<class>) with its saved id <n>: <model> (<class>, id <k>) held its slot, could not be removed
     [CoopSave] <n> kept entities are gone after the load              KeepEnd
     [CoopSave] a checkpoint of a campaign with <n> players: the AI starts afresh (<why>)
     [CoopSave] the load makes <m> (<c>) again with the id <n> it had: that slot is <x> now, the piece gets the id <k>
       + [CoopSave] the load spawns <m> (<c>) with its saved id <n>: ...  in the same load: the piece made twice
     [Warning] [LoadGame] Spawned Entity ID=<n>, <name> (<class>), with wrong ID=<k>   CryAction
     [Warning] [LoadGame] Expected Entity ID=<n>, <name> (<class>), Loaded ID=<k>     every one after it
     [Warning] [LoadGame] ... class mismatch ...;  Failed Spawning|Loading|Initializing Entity (WARN)
     an in-place load with a friend in the game and none of
       [CoopSave] <n> friend(s) stay in the game through the load (<m> entities kept)
       [CoopSave] no friend in the game to keep through the load
       [CoopSave] ... the load disconnects the friends (they join again)
     "entities kept" growing from one in-place load to the next (WARN)
     [CoopTest] coop_test_rearm check: <n> soldiers not armed as saved      n > 0 after a load
     [CoopSave] after the load <n> soldiers did not have their guns as saved, their equipment again (WARN)
     [CoopSave] a player or what one carries holds the slot of a saved id: ... (WARN)
     [CoopSave] the load spawns ...: <p>, a player or what one carries, holds its slot and stays (WARN)
     [CoopSave] the entity ids of <save> could not be read / this CryEntitySystem.dll is not the one known (WARN)
     [CoopSave] the player limit call / the AI's perception reading could not be put back (WARN)
     [CoopSave] Friends may not be able to join this game: ...
D06  a load start without its end; [CoopSave] the saved game took <x> s to load (x > 120);
     [CoopSave] the level did not get ready: the saved game is not loaded;
     [CoopSave] The saved game could not be loaded: the level starts from the beginning
D13  [Coop] everybody is down: back to the last checkpoint in <n> s, and in the next 75 s none of
       [CoopSave] loading ... | [Coop] everybody is down, no checkpoint place kept |
       <p> is back at the checkpoint | <p> comes back (...) | revived <p>
       ("everybody is down: the last checkpoint is loaded" only asks for the load;
       "[CoopSave] continuing campaign" without "runs already" after it: the
       checkpoint's level loads first, and the load gets 300 s from there)
     [Coop] the checkpoint was not loaded: the team comes back without it
     [Coop] <p> fell through the ground: put back ... (WARN)

Time: by the lines' time stamps when the log has them (log_IncludeTime);
otherwise by the [CoopPos] server: lines, which the host writes every 5 s of
a network game (a long load writes none: the game does not update).
"""

import re

from ..findings import Finding

# ---- D05
RE_STAYS = re.compile(r"^\[CoopSave\] (\d+) stays: bound (-?\d+) \(now (-?\d+)\), multiplayer (\d+)")
RE_STAYS_OLD = re.compile(r"^\[CoopSave\] (\d+) could not be removed$")
RE_SPAWNS = re.compile(r"^\[CoopSave\] the load spawns (.*?) \(([^()]*)\) with its saved id (\d+): (.*)$")
RE_HOLDER = re.compile(r"^(.*?) ?\(([^(),]*), id (\d+)\) held its slot, could not be removed")
RE_MADE_FREE = re.compile(r"(\S*) \(([^(),]*), (\d+) for (\d+)\)")
RE_KEPT_GONE = re.compile(r"^\[CoopSave\] (\d+) kept entities are gone after the load")
RE_AI_AFRESH = re.compile(r"^\[CoopSave\] a checkpoint of a campaign with (\d+) players: the AI starts afresh \((.*)\)")
RE_MAKES = re.compile(r"^\[CoopSave\] the load makes (.*?) \(([^()]*)\) again with the id (\d+) it had: "
                      r"that slot is (.*) now, the piece gets (?:the id (\d+)|a new id)")
# CryAction.dll 6156's own strings (read from the binary):
#   [LoadGame] Spawned Entity ID=%d, %s (%s), with wrong ID=%d
#   [LoadGame] Expected Entity ID=%d, %s (%s), Loaded ID=%d
#   [LoadGame] Entity class mismatch ID=%d %s, should have class '%s'
#   [LoadGame] Entity ID=%d '%s', class mismatch, should be '%s'
#   [LoadGame] Failed Spawning|Loading|Initializing Entity ID=%d, %s (%s)
#   [LoadGame] Failed Spawning Entity ID=%d, Class '%s' not found
#   [LoadGame] Missing Entity ID=%d / Missing Entity ID %d / Missing Entity with ID=%d, probably was removed ...
#     (the Missing ones come with the wrong ids; alone they are not reported)
RE_WRONG_ID = re.compile(r"\[LoadGame\] Spawned Entity ID=(\d+), (.*) \(([^()]*)\), with wrong ID=(\d+)")
RE_EXPECTED = re.compile(r"\[LoadGame\] Expected Entity ID=(\d+), (.*) \(([^()]*)\), Loaded ID=(\d+)")
RE_CLASS_MISMATCH = re.compile(r"\[LoadGame\] (?:Entity class mismatch ID=\d+|Entity ID=\d+ .*, class mismatch, should be )")
RE_ENTITY_FAILED = re.compile(r"\[LoadGame\] Failed (Spawning|Loading|Initializing) Entity ID=(\d+), (.*)")
# (older builds: "(87 entities kept, 5 of the host's things made anew)")
RE_KEEP_YES = re.compile(r"^\[CoopSave\] (\d+) friend\(s\) stay in the game through the load \((\d+) entities kept")
RE_KEEP_NONE = re.compile(r"^\[CoopSave\] no friend in the game to keep through the load")
RE_KEEP_OFF = re.compile(r"^\[CoopSave\] .*the load disconnects the friends \(they join again\)")
RE_DISCONNECT = re.compile(r"Disconnect (\S+); profid=\d+; cause=(\d+)")
# (a player still joining shows at 0,0,0 with hp -1: not counted)
RE_POS_PLAYER = re.compile(r" ([^ =]+)=\((?!0,0,0\))")
RE_TICK_PLAYERS = re.compile(r"^\[CoopAI\] tick: .* players=(\d+)")
RE_REARM_CHECK = re.compile(r"^\[CoopTest\] coop_test_rearm check: (\d+) soldiers not armed as saved")
RE_REARM_LIST = re.compile(r"^\[CoopSave\] (\d+) soldiers do not have their guns as saved:")
RE_REARMED = re.compile(r"^\[CoopSave\] after the load (\d+) soldiers did not have their guns as saved, their equipment again")
RE_SLOT_PROTECTED = re.compile(r"^\[CoopSave\] a player or what one carries holds the slot of a saved id:(.*): this load keeps them")
# (FreeSavedIdSlot: the same, met while the load spawns the save's thing)
RE_SLOT_PROTECTED_SPAWN = re.compile(r"^\[CoopSave\] the load spawns (.*?) \(([^()]*)\) with its saved id (\d+): (.*), "
                                     r"a player or what one carries, holds its slot and stays")
RE_NO_REJOIN = re.compile(r"^\[CoopSave\] Friends may not be able to join this game")
RE_IDS_UNREAD = re.compile(r"^\[CoopSave\] (?:the entity ids of (\S+) could not be read"
                           r"|this CryEntitySystem\.dll is not the one known: the save's entity ids are not looked after)")
# a patch of the load left in place (KeepEnd, LoadNow)
RE_PATCH_LEFT = re.compile(r"^\[CoopSave\] the (player limit call|AI's perception reading) could not be put back")

# ---- D06
RE_NOT_READY = re.compile(r"^\[CoopSave\] the level did not get ready: the saved game is not loaded")
RE_LOAD_FAILED = re.compile(r"^\[CoopSave\] The saved game could not be loaded")
RE_CRASH = re.compile(r"CRASH DETECTED")

# ---- D13
RE_ALLDOWN = re.compile(r"^\[Coop\] everybody is down: back to the last checkpoint in (\d+) s")
# what ends an all-down: the load starts (the Lua's "everybody is down: the
# last checkpoint is loaded" only asks for it), the team comes back without a
# load, somebody is revived, or the Lua gives up ("the checkpoint was not
# loaded", reported on its own)
RE_ALLDOWN_DONE = re.compile(
    r"^\[CoopSave\] loading \S+ \("
    r"|^\[Coop\] (?:everybody is down, no checkpoint place kept"
    r"|.* is back at the checkpoint"
    r"|.* comes back \("
    r"|(?:.* )?revived \S+$"
    r"|the checkpoint was not loaded)")
RE_LOAD_ASKED = re.compile(r"^\[Coop\] everybody is down: the last checkpoint is loaded")
# the host goes to the checkpoint (HostCheckpoint); not followed by "runs
# already" a level load comes first (20-220 s in goal_runs), then "loading"
RE_CONTINUING = re.compile(r"^\[CoopSave\] continuing campaign ")
RE_RUNS_ALREADY = re.compile(r"^\[CoopSave\] \S+ runs already: the checkpoint is loaded into it")
RE_LEVEL_LOAD_DONE = re.compile(r"^\[CoopSave\] (?:loading \S+ \(|the level did not get ready"
                                r"|\S+ is not the saved level \S+: continue cancelled)")
RE_NOT_LOADED = re.compile(r"^\[Coop\] the checkpoint was not loaded: the team comes back without it")
RE_FELL = re.compile(r"^\[Coop\] (.*) fell through the ground: put back (.*)")

LOAD_STALL_S = 300      # a load that has not ended by then is stuck
LOAD_STOPPED_S = 60     # the log ends this soon after the start: the test may have been stopped
TOOK_MAX_S = 120
ALLDOWN_S = 75          # all down: the load starts within this (it waits 6 s; corpus: 10 s at most;
                        # the Lua gives up 60 s after asking)
POS_EVERY_S = 5         # [CoopPos] server: lines, one every 5 s
UNTIMED_CAP = 6000      # lines looked at when neither time stamps nor [CoopPos] tell the time
KEPT_GROWTH = 1.3       # entities kept, from one in-place load to the next
KEPT_GROWTH_MIN = 300
REARMED_WARN = 10       # soldiers re-armed after a load


# ---------------------------------------------------------------- helpers

def _dt(a, b):
    """Seconds from line a to line b by their time stamps, or None."""
    if a is None or b is None or a.t is None or b.t is None:
        return None
    return (b.t - a.t) % 86400


def _is_pos(l):
    return l.text.startswith("[CoopPos] server:")


def _after(log, start, seconds, stop=None):
    """The lines after `start` up to `seconds` later. Returns (hit, elapsed,
    clocked, upto): hit = the first line for which stop(line) is true (None:
    none in time), elapsed = the seconds reached (None: unknown), clocked =
    whether the time ran out before the log did, upto = the last line number
    looked at."""
    timed = start.t is not None
    pos = 0
    last_t = None
    lines = log.lines
    first = start.no          # index of the line after start
    cap = len(lines) if timed else min(len(lines), first + UNTIMED_CAP)
    for k in lines[first:cap]:
        if timed:
            if k.t is not None:
                d = _dt(start, k)
                if d > seconds:
                    return None, d, True, k.no
                last_t = d
        elif _is_pos(k):
            pos += 1
            if pos * POS_EVERY_S > seconds:
                return None, pos * POS_EVERY_S, True, k.no
        if stop is not None and stop(k):
            return k, (last_t if timed else (pos * POS_EVERY_S if pos else None)), False, k.no
    elapsed = last_t if timed else (pos * POS_EVERY_S if pos else None)
    return None, elapsed, cap < len(lines), cap


def _load_span(ctx, i):
    """The host lines of load i: from its start to its end (or the next load's
    start, or the end of the log)."""
    loads = ctx.ev.loads
    ld = loads[i]
    if ld.end is not None:
        last = ld.end.no
    elif i + 1 < len(loads):
        last = loads[i + 1].start.no - 1
    else:
        last = len(ctx.host.lines)
    return ctx.host.slice(ld.start.no, last)


def _loads_in_window(ctx):
    return [(i, ld) for i, ld in enumerate(ctx.ev.loads) if ctx.in_window(ld.start)]


def _model(s):
    s = s.strip()
    return s.rsplit("/", 1)[-1] if s else ""


def _friend_in_game(log, before_no, max_lines=800):
    """The last [CoopPos] server: line before line before_no when it shows
    someone besides the host in the game and nobody disconnected after it;
    else None. ([CoopAI] tick: players= only when no [CoopPos] line is there:
    it counts a player still joining too, [CoopPos] shows him at 0,0,0.)"""
    lo = max(1, before_no - max_lines)
    span = log.slice(lo, before_no - 1)
    tick = None
    pick = None
    for j in range(len(span) - 1, -1, -1):
        s = span[j].text
        if s.startswith("[CoopPos] server:"):
            pick = (j, len(RE_POS_PLAYER.findall(s)))
            break
        if tick is None and s.startswith("[CoopAI] tick: "):
            m = RE_TICK_PLAYERS.match(s)
            if m:
                tick = (j, int(m.group(1)))
    pick = pick or tick
    if pick is None or pick[1] < 2:
        return None
    j = pick[0]
    if any("Disconnect" in k.text and RE_DISCONNECT.search(k.text) for k in span[j + 1:]):
        return None
    return span[j]


def _crash_after(log, no):
    for l in log.lines[no:]:
        if "CRASH" in l.text and RE_CRASH.search(l.text):
            return l
    return None


def _ids_text(ids, models, n=6):
    out = []
    for i in ids[:n]:
        m = models.get(i)
        out.append("%s %s" % (i, m) if m else i)
    return ", ".join(out) + (" ..." if len(ids) > n else "")


# ---------------------------------------------------------------- D05

def d05_leftovers(ctx):
    """An entity the load had to remove (it held the slot of a saved id)
    stayed: the save's thing got another id, the piece stays in the world."""
    stays = {}      # (load start, id) -> line
    held = {}       # (load start, id) -> line
    models = {}
    for l in ctx.lines(ctx.host):
        s = l.text
        if not s.startswith("[CoopSave] "):
            continue
        m = RE_STAYS.match(s) or RE_STAYS_OLD.match(s)
        if m:
            ld = ctx.ev.load_at(l.no)
            stays.setdefault((ld.start.no if ld else 0, m.group(1)), l)
            continue
        if s.startswith("[CoopSave] the save's ") and " made free " in s:
            for mm in RE_MADE_FREE.finditer(s):
                if mm.group(1):
                    models.setdefault(mm.group(3), _model(mm.group(1)))
            continue
        m = RE_SPAWNS.match(s)
        if m and s.endswith("could not be removed"):
            h = RE_HOLDER.match(m.group(4))
            if h:
                ld = ctx.ev.load_at(l.no)
                held.setdefault((ld.start.no if ld else 0, h.group(3)), l)
                if h.group(1).strip():
                    models.setdefault(h.group(3), _model(h.group(1)))
    keys = sorted(set(stays) | set(held), key=lambda k: (stays.get(k) or held.get(k)).no)
    if not keys:
        return []
    ids = []
    for k in keys:
        if k[1] not in ids:
            ids.append(k[1])
    # the pieces of something (a model is known) first: they are what piles up
    ids.sort(key=lambda i: 0 if models.get(i) else 1)
    first = {}
    for k in keys:
        first.setdefault(k[1], stays.get(k) or held.get(k))
    ev = [first[i] for i in ids]
    ev += [held[k] for k in keys if k in held and held[k] not in ev]
    loads = len({k[0] for k in keys})
    f = Finding("D05", "FAIL",
                "загрузка не смогла убрать обломки, занявшие места вещей сохранения: они остаются в мире и копятся (%s)"
                % _ids_text(ids, models, 3),
                "%d entities the load had to remove stayed (%d load(s)): %s" % (len(ids), loads, _ids_text(ids, models)),
                ev[:8], key="leftover")
    f.count = len(keys)
    return [f]


def d05_kept_gone(ctx):
    out = []
    for l in ctx.lines(ctx.host):
        m = RE_KEPT_GONE.match(l.text)
        if m and int(m.group(1)) > 0:
            n = int(m.group(1))
            out.append(Finding("D05", "FAIL",
                               "после загрузки пропали вещи, которые она должна была оставить (%d)" % n,
                               "%d entities kept through the in-place load are gone after it" % n,
                               [l], key="kept_gone"))
    return out


def d05_ai_afresh(ctx):
    out = []
    for l in ctx.lines(ctx.host):
        m = RE_AI_AFRESH.match(l.text)
        if not m:
            continue
        why = m.group(2)
        if why.startswith("coop_load_ai_reset"):
            out.append(Finding("D05", "WARN",
                               "ИИ после загрузки начал заново (так задано: %s): солдаты забыли тревогу, машины ИИ стоят" % why,
                               "the AI started afresh after the load (%s, set on purpose)" % why, [l], key="ai_afresh_set"))
        elif "crashed otherwise" in why:
            out.append(Finding("D05", "WARN",
                               "ИИ после загрузки начал заново (так делала старая сборка): солдаты забыли тревогу, машины ИИ стоят",
                               "the AI started afresh after the load (an old build: %s)" % why, [l], key="ai_afresh_old"))
        else:
            out.append(Finding("D05", "FAIL",
                               "ИИ после загрузки начал заново: солдаты забыли тревогу, вертолёты и колонны ИИ стоят",
                               "the AI started afresh after the load: %s" % why, [l], key="ai_afresh"))
    return out


def d05_made_twice(ctx):
    """A broken piece the load makes again under a new id (its old id's slot
    was taken), then spawns again from the save with that id: two of it (the
    palm trunks over the user's hut, 2026-10-15)."""
    out = []
    for i, ld in _loads_in_window(ctx):
        makes = {}
        for l in _load_span(ctx, i):
            s = l.text
            if not s.startswith("[CoopSave] the load "):
                continue
            m = RE_MAKES.match(s)
            if m:
                makes.setdefault(m.group(3), (m, l))
                continue
            m = RE_SPAWNS.match(s)
            if m and m.group(3) in makes:
                mk, ml = makes.pop(m.group(3))
                if mk.group(1).lower() != m.group(1).lower():
                    continue
                model = _model(m.group(1)) or m.group(2)
                # (older builds: "the piece gets a new id", no number)
                again = ("again as %s" % mk.group(5)) if mk.group(5) else "again under a new id"
                out.append(Finding("D05", "FAIL",
                                   "загрузка сделала одну вещь дважды: %s (два одинаковых обломка в одном месте)" % model,
                                   "the load made %s twice: %s (its id %s was taken), then from the save with id %s"
                                   % (model, again, m.group(3), m.group(3)),
                                   [ml, l], key="made_twice:" + model.lower()))
    return out


def d05_wrong_ids(ctx):
    """CryAction spawned a saved thing with another id; from there on it
    matches the save's entities with the next one's data (the soldiers'
    equipment of the whole level not theirs)."""
    out = []
    for i, ld in _loads_in_window(ctx):
        wrong, expected, mismatch, failed = [], [], [], []
        for l in _load_span(ctx, i):
            s = l.text
            if "[LoadGame]" not in s:
                continue
            if RE_WRONG_ID.search(s):
                wrong.append(l)
            elif RE_EXPECTED.search(s):
                expected.append(l)
            elif RE_CLASS_MISMATCH.search(s):
                mismatch.append(l)
            elif RE_ENTITY_FAILED.search(s):
                failed.append(l)
        if failed:
            # (no goal_runs log has one: a thing of the save is not in the world)
            f = Finding("D05", "WARN",
                        "загрузка не смогла создать %d вещей сохранения: их нет в мире" % len(failed),
                        "the load at line %d failed to spawn, load or initialize %d saved entities (%s)"
                        % (ld.start.no, len(failed), RE_ENTITY_FAILED.search(failed[0].text).group(3)[:80]),
                        failed[:8], key="entity_failed")
            f.count = len(failed)
            out.append(f)
        if not (wrong or expected or mismatch):
            continue
        ev = wrong[:4] + mismatch[:2] + expected[:2]
        f = Finding("D05", "FAIL",
                    "загрузка перепутала номера вещей: вещи сохранения получили чужие данные "
                    "(у солдат чужое оружие или никакого)",
                    "the load at line %d spawned %d saved entities with a wrong id; %d more were matched with another's data%s"
                    % (ld.start.no, len(wrong), len(expected), ", %d class mismatches" % len(mismatch) if mismatch else ""),
                    ev, key="wrong_ids")
        f.count = len(wrong) + len(mismatch) or 1
        out.append(f)
    return out


def d05_keep_missing(ctx):
    """An in-place load with a friend in the game that did not keep him: no
    "friend(s) stay" line and no reason (KeepBegin returned false silently),
    or "no friend in the game to keep" while [CoopPos] had one a moment
    before (the load removed his player first): the load kicks him. Also a
    load that says it keeps the friends and kicks one (cause=4) all the same."""
    out = []
    loads = ctx.ev.loads

    def kicks(first, last):
        return [l for l in ctx.host.slice(first, last)
                if "Disconnect" in l.text and RE_DISCONNECT.search(l.text) and RE_DISCONNECT.search(l.text).group(2) == "4"]

    for i, ld in _loads_in_window(ctx):
        if not ld.inplace:
            continue
        span = _load_span(ctx, i)
        keep = next((l for l in span if RE_KEEP_YES.match(l.text)), None)
        if keep is not None:
            # kept, and kicked by the load all the same: the keeping broke
            # (no goal_runs log has it; D03 reports the kick too)
            kicked = kicks(ld.start.no, span[-1].no)
            if kicked:
                out.append(Finding("D05", "FAIL",
                                   "загрузка должна была оставить друга в игре, но его всё равно выкинуло",
                                   "in-place load at line %d kept its friends (line %d: %s), yet %s"
                                   % (ld.start.no, keep.no, keep.text[len("[CoopSave] "):], kicked[0].text.strip()),
                                   [keep, kicked[0], ld.start], key="keep_broken"))
            continue
        if any(RE_KEEP_OFF.match(l.text) for l in span):
            continue
        none = next((l for l in span if RE_KEEP_NONE.match(l.text)), None)
        pos = _friend_in_game(ctx.host, ld.start.no)
        if pos is None:
            continue
        # who the load disconnected (up to the next load)
        last = loads[i + 1].start.no - 1 if i + 1 < len(loads) else len(ctx.host.lines)
        last = min(last, (ld.end.no if ld.end else ld.start.no) + 60)
        kicked = kicks(ld.start.no, last)
        # (D03 reports the kick itself; this one says why the load did it)
        if none is not None:
            why = "it says no friend in the game to keep (line %d), but line %d had one" % (none.no, pos.no)
            ru = "загрузка в запущенный уровень решила, что друга в игре нет, и не стала его оставлять"
        else:
            why = ("no \"friend(s) stay in the game through the load\" and no reason given "
                   "(keeping friends through a load is off: coop_load_keep 0, or a build without it)")
            ru = "загрузка в запущенный уровень не стала оставлять друга в игре (это выключено или сборка старая)"
        out.append(Finding("D05", "FAIL",
                           ru + ("; поэтому его выкинуло" if kicked else ""),
                           "in-place load at line %d with a friend in the game: %s%s"
                           % (ld.start.no, why, "; then %s" % kicked[0].text.strip() if kicked else ""),
                           [ld.start] + ([none] if none else []) + kicked[:1] + [pos], key="keep_missing"))
    return out


def _keep_line(ctx, i):
    """The "N friend(s) stay in the game through the load (M entities kept"
    line of load i, or None."""
    for l in _load_span(ctx, i):
        if RE_KEEP_YES.match(l.text):
            return l
    return None


def d05_kept_growth(ctx):
    """'entities kept' growing from one in-place load to the next: something
    the load does not remove piles up. Normal runs stay within a few per cent
    (1322 -> 1338). The three corpus jumps (adk_new 1392 -> 2475, adk_off2
    1419 -> 2492, final_convoy 1428 -> 2519) each follow a load that re-armed
    205 soldiers ("1132 things picked up again"): their new equipment, not
    debris, so the text names the re-arm when there was one."""
    out = []
    prev = prev_line = None
    for i, ld in enumerate(ctx.ev.loads):
        if not ld.inplace:
            prev = None
            continue
        kl = _keep_line(ctx, i)
        if kl is None:
            continue
        kept = int(RE_KEEP_YES.match(kl.text).group(2))
        if prev is not None and ctx.in_window(ld.start):
            a, b = prev, kept
            if b - a >= KEPT_GROWTH_MIN and b >= a * KEPT_GROWTH:
                rearm = None
                for k in ctx.host.slice(prev_line.no, kl.no):
                    m = RE_REARMED.match(k.text)
                    if m and int(m.group(1)) >= REARMED_WARN:
                        rearm = (int(m.group(1)), k)
                        break
                if rearm:
                    ru = ("от загрузки к загрузке вещей в мире всё больше (%d -> %d): прошлая загрузка выдала "
                          "снаряжение заново %d солдатам, похоже, копится оно" % (a, b, rearm[0]))
                    en = ("entities kept through in-place loads grew %d -> %d (lines %d, %d); the load before re-armed "
                          "%d soldiers (line %d): their equipment may be what piles up" % (a, b, prev_line.no, kl.no, rearm[0], rearm[1].no))
                else:
                    ru = "от загрузки к загрузке вещей в мире всё больше (%d -> %d): похоже, копятся обломки" % (a, b)
                    en = "entities kept through in-place loads grew %d -> %d (lines %d, %d)" % (a, b, prev_line.no, kl.no)
                out.append(Finding("D05", "WARN", ru, en,
                                   [kl, prev_line] + ([rearm[1]] if rearm else []), key="kept_growth"))
        prev, prev_line = kept, kl
    return out


def d05_rearm(ctx):
    """Soldiers not armed as saved after a load (coop_test_rearm check), the
    load's own re-arming of many soldiers, and the other things a load says
    it could not get right."""
    out = []
    loads = ctx.ev.loads
    unarmed, rearmed = [], []       # (n, load, evidence)
    for l in ctx.lines(ctx.host):
        s = l.text
        if s.startswith("[CoopTest] coop_test_rearm check"):
            m = RE_REARM_CHECK.match(s)
            if not m or int(m.group(1)) <= 0:
                continue
            last = None     # the last load started before it: it must have ended before it
            for ld in loads:
                if ld.start.no < l.no:
                    last = ld
            if last is None or last.end is None or last.end.no > l.no:
                continue
            ev = [l]
            prev = ctx.host.lines[l.no - 2] if l.no >= 2 else None
            if prev is not None and RE_REARM_LIST.match(prev.text):
                ev.append(prev)
            unarmed.append((int(m.group(1)), last, ev))
            continue
        if not s.startswith("[CoopSave] "):
            continue
        m = RE_REARMED.match(s)
        if m and int(m.group(1)) >= REARMED_WARN:
            rearmed.append((int(m.group(1)), ctx.ev.load_at(l.no), [l]))
            continue
        m = RE_SLOT_PROTECTED.match(s)
        if m:
            out.append(Finding("D05", "WARN",
                               "игрок занял место вещи сохранения: загрузка не расставила номера вещей как в сохранении",
                               "a player or what one carries held a saved id's slot:%s: the load did not get the save's ids right"
                               % m.group(1)[:200], [l], key="slot_protected"))
            continue
        m = RE_SLOT_PROTECTED_SPAWN.match(s)
        if m:
            out.append(Finding("D05", "WARN",
                               "игрок занял место вещи сохранения: загрузка не расставила номера вещей как в сохранении",
                               "%s held the slot of the saved %s (%s, id %s) while the load spawned it: it gets another id"
                               % (m.group(4)[:80], _model(m.group(1)) or "?", m.group(2), m.group(3)), [l], key="slot_protected"))
            continue
        m = RE_PATCH_LEFT.match(s)
        if m:
            out.append(Finding("D05", "WARN",
                               "загрузка не вернула на место свою правку игры (%s)"
                               % ("лимит игроков" if m.group(1).startswith("player") else "чтение восприятия ИИ"),
                               "the load could not put back %s" % m.group(1), [l], key="patch_left"))
            continue
        if RE_NO_REJOIN.match(s):
            out.append(Finding("D05", "FAIL",
                               "после загрузки друзья, возможно, не смогут войти в игру",
                               "after the load the player limit was not put back: friends may not be able to join",
                               [l], key="no_rejoin"))
            continue
        m = RE_IDS_UNREAD.match(s)
        if m:
            out.append(Finding("D05", "WARN",
                               "загрузка не следит за номерами вещей сохранения: они могут перепутаться",
                               ("the entity ids of %s could not be read" % m.group(1) if m.group(1)
                                else "another CryEntitySystem.dll") + ": the save's ids are not looked after",
                               [l], key="ids_unread"))

    def nums(items):
        return ", ".join(str(n) for n, _, _ in items[:6]) + (" ..." if len(items) > 6 else "")

    if unarmed:
        f = Finding("D05", "FAIL",
                    "после загрузки солдаты без своего оружия (%s)" % nums(unarmed),
                    "soldiers not armed as saved after a load (coop_test_rearm check): %s"
                    % ", ".join("%d after the load at line %d" % (n, ld.start.no) for n, ld, _ in unarmed[:6]),
                    [e for _, _, ev in unarmed for e in ev][:8], key="unarmed")
        f.count = len(unarmed)
        out.append(f)
    if rearmed:
        f = Finding("D05", "WARN",
                    "загрузка оставляла солдат без своего оружия (%s): им выдали снаряжение заново" % nums(rearmed),
                    "loads left soldiers without their guns as saved, re-armed: %s"
                    % ", ".join("%d (load at line %s)" % (n, ld.start.no if ld else "?") for n, ld, _ in rearmed[:6]),
                    [ev[0] for _, _, ev in rearmed][:8], key="rearmed")
        f.count = len(rearmed)
        out.append(f)
    return out


# ---------------------------------------------------------------- D06

def d06_stall(ctx):
    out = []
    loads = ctx.ev.loads
    host = ctx.host
    for i, ld in _loads_in_window(ctx):
        if ld.end is not None:
            d = _dt(ld.start, ld.end)
            if d is None:
                d = POS_EVERY_S * sum(1 for l in host.slice(ld.start.no, ld.end.no) if _is_pos(l))
            if d > LOAD_STALL_S:
                out.append(Finding("D06", "FAIL",
                                   "загрузка чекпойнта шла %d с" % d,
                                   "checkpoint load took %d s (line %d to %d)" % (d, ld.start.no, ld.end.no),
                                   [ld.start, ld.end], key="slow"))
            continue
        if _crash_after(host, ld.start.no):
            continue        # D01 reports the crash
        span = _load_span(ctx, i)
        if any(RE_LOAD_FAILED.match(l.text) for l in span):
            continue        # d06_failed says it
        nxt = loads[i + 1] if i + 1 < len(loads) else None
        if nxt is not None:
            out.append(Finding("D06", "FAIL",
                               "загрузка чекпойнта не закончилась (следующая началась на строке %d)" % nxt.start.no,
                               "checkpoint load at line %d never ended; the next one started at line %d" % (ld.start.no, nxt.start.no),
                               [ld.start, nxt.start], key="no_end"))
            continue
        _, d, clocked, _ = _after(host, ld.start, LOAD_STALL_S)
        last = host.lines[-1]
        if clocked or (d is not None and d >= LOAD_STOPPED_S):
            out.append(Finding("D06", "FAIL",
                               "загрузка чекпойнта зависла: за %s с не закончилась" % ("%d" % d if d is not None else "много"),
                               "checkpoint load at line %d did not end in the %s s the log goes on"
                               % (ld.start.no, "%d" % d if d is not None else "?"),
                               [ld.start, last], key="no_end"))
        else:
            out.append(Finding("D06", "WARN",
                               "лог кончается посреди загрузки чекпойнта (игру остановили или она зависла)",
                               "the log ends %s after the checkpoint load at line %d started, before it ended"
                               % ("%d s" % d if d is not None else "%d lines" % (len(host.lines) - ld.start.no), ld.start.no),
                               [ld.start, last], key="log_ends"))
    return out


def d06_took(ctx):
    out = []
    for i, ld in _loads_in_window(ctx):
        if ld.took is not None and ld.took > TOOK_MAX_S:
            out.append(Finding("D06", "FAIL",
                               "сохранённая игра грузилась %.0f с" % ld.took,
                               "the saved game took %.1f s to load (line %d)" % (ld.took, ld.took_line.no),
                               [ld.took_line, ld.start], key="took"))
    return out


def d06_failed(ctx):
    out = []
    for l in ctx.lines(ctx.host):
        s = l.text
        if not s.startswith("[CoopSave] "):
            continue
        if RE_NOT_READY.match(s):
            out.append(Finding("D06", "FAIL",
                               "уровень так и не стал готов: чекпойнт не загрузился",
                               "the level did not get ready: the saved game was not loaded", [l], key="not_ready"))
        elif RE_LOAD_FAILED.match(s):
            out.append(Finding("D06", "FAIL",
                               "чекпойнт не загрузился: уровень начался сначала",
                               "the saved game could not be loaded: the level starts from the beginning", [l], key="load_failed"))
    return out


# ---------------------------------------------------------------- D13

def d13_alldown(ctx):
    out = []
    host = ctx.host
    for l in ctx.lines(host):
        s = l.text
        if not s.startswith("[Coop] "):
            continue
        if RE_ALLDOWN.match(s):
            done, d, clocked, upto = _after(host, l, ALLDOWN_S, lambda k: bool(RE_ALLDOWN_DONE.match(k.text)))
            if done is not None:
                continue
            asked = next((k for k in host.slice(l.no + 1, upto) if RE_LOAD_ASKED.match(k.text)), None)
            # the checkpoint is of a level that does not run (or in-place
            # loads are off): its level loads first, which takes up to minutes
            cont = next((k for k in host.slice(l.no + 1, upto) if RE_CONTINUING.match(k.text)), None)
            level_load = cont is not None and not any(RE_RUNS_ALREADY.match(k.text) for k in host.slice(cont.no + 1, cont.no + 5))
            limit = ALLDOWN_S
            if level_load:
                done, d, clocked, upto = _after(host, cont, LOAD_STALL_S, lambda k: bool(RE_LEVEL_LOAD_DONE.match(k.text)))
                if done is not None:
                    continue    # loading, or D06's "did not get ready"
                limit = LOAD_STALL_S
            crash = _crash_after(host, l.no)
            if crash is not None and crash.no <= upto + 1:
                continue        # D01
            if not clocked:
                out.append(Finding("D13", "WARN",
                                   "все погибли, а лог кончается раньше, чем начал грузиться чекпойнт",
                                   "everybody went down at line %d; the log ends %s later, before the checkpoint load started"
                                   % (l.no, "%d s" % d if d is not None else "%d lines" % (len(host.lines) - l.no)),
                                   [l, host.lines[-1]], key="alldown_log_ends"))
                continue
            if level_load:
                en = ("everybody went down at line %d; the checkpoint's level load began (line %d) and no checkpoint load "
                      "started in the next %d s" % (l.no, cont.no, limit))
            else:
                en = ("everybody went down at line %d; %sno checkpoint load started in the next %d s"
                      % (l.no, "the load was asked for (line %d), " % asked.no if asked else "", limit))
            out.append(Finding("D13", "FAIL",
                               "все погибли, но игра не вернулась к последнему чекпойнту"
                               + (" (загрузку попросили, она не началась)" if asked else ""),
                               en, [l] + ([asked] if asked else []) + ([cont] if cont else []), key="alldown_no_load"))
            continue
        if RE_NOT_LOADED.match(s):
            out.append(Finding("D13", "FAIL",
                               "все погибли, а чекпойнт не загрузился: команда встала без него",
                               "the checkpoint was not loaded a minute after everybody went down: the team comes back without it",
                               [l], key="not_loaded"))
            continue
        m = RE_FELL.match(s)
        if m:
            out.append(Finding("D13", "WARN",
                               "%s провалился сквозь землю (его поставили обратно)" % m.group(1),
                               "%s fell through the ground: put back %s" % (m.group(1), m.group(2)),
                               [l], key="fell"))
    return out


DETECTORS = [
    d05_leftovers, d05_kept_gone, d05_ai_afresh, d05_made_twice, d05_wrong_ids,
    d05_keep_missing, d05_kept_growth, d05_rearm,
    d06_stall, d06_took, d06_failed,
    d13_alldown,
]
