"""Physics and world damage after checkpoint loads (D07) and the mod's own bug
reports (D02).

D07, from the host log:
  [Coop] DAMAGE player <name> by <who> type=collision hit=<h> taken=<t> hp <a>-><b>
      (CoopGameRules.lua ProcessActorDamage, only with C.DEBUG; the server
      writes it for every player, the companion's own log has none)
      h >= 100, or a hit that kills (a > 0 >= b), from "the saved game took"
      to 30 s after the load's end: FAIL, debris of the load hit the player;
      h >= 100 anywhere else: WARN. coop_god hides the line, and its "HIT player ...
      (god, would be N)" lines never show collisions (none in goal_runs).
  [brk] OnCreatePhysEntityPart entity[<hex id>:<model>] due to joint break ...
      (CryAction's g_breakagelog 1): 20 or more pieces of buildings broken off
      in the 10 s after a load = WARN, the hut collapses after the load.
      "CPE: <model>" (a diagnostic DLL of 2026-10-14/15, not in today's
      source) is the same piece, counted the same way.
  [CoopAgent] the host's game is ready N s after its player is there
      (things still break: it waited long enough)                     WARN

D02, from both logs:
  [CoopBug] after the load <name> (<class>, <id>, <mass> kg) flew at <v> m/s
      at x y z: slowed to 20          (CoopSave.cpp Settle(), at most 20 per load)
  [CoopSave] the 10 s after the load are over: N things were slowed down
      (N counts slow-downs, one body is slowed every 0.05 s while it flies;
      written only after at least one line above)
      Both: ONE FAIL, count = slowed bodies (WARN when every body flew under
      25 m/s, the design's phys_fling speed after a load). Debris of one
      collapse has many names (model paths, empty names: the class
      "Default"), they are listed in the text, not split into a finding each.
  [CoopBug] kind=<k> sev=<n> k=v ...  (the planned watchdog): sev 3 FAIL,
      sev 2 WARN, sev 1 left out; kind=summary (no sev: "suppressed=<n>")
      left out; a kind without sev WARN
  [CoopBug] anything else: FAIL

The user's own Game.log has no time stamps. Without them "30 s after the load"
is counted in the server's periodic lines (CoopAI.cpp): "[CoopAI] AI shots so
far" every 10 s and "[CoopPos] server:" every 5 s of game time. The window ends
at the (seconds/period + 1)th line of either clock (or where the next load
starts), so it covers at least `seconds` and at most one period more, however
many lines a collapse writes. Only when no clock line follows the load at all
does it fall back to Events.window_after's line count (600 lines).
"""

import os
import re

from ..findings import Finding

# --- signals ---------------------------------------------------------------

RE_DAMAGE = re.compile(
    r"^\[Coop\] DAMAGE player (.+?) by (.*?) type=collision hit=([\d.]+)"
    r"(?: taken=(-?[\d.]+))?(?: hp (-?\d+)->(-?\d+))?")
# the server's periodic lines, a clock for logs without time stamps
# (CoopAI.cpp: every 10 s and every 5 s of game time)
CLOCKS = [(re.compile(r"^\[CoopAI\] AI shots so far: "), 10),
          (re.compile(r"^\[CoopPos\] server:"), 5)]
RE_PART = re.compile(r"^\[brk\] OnCreatePhysEntityPart entity\[([0-9a-fA-F]+):([^\]]*)\] due to joint break")
RE_STILL_BREAK = re.compile(
    r"^\[CoopAgent\] the host's game is ready (\d+) s after its player is there \(things still break")
RE_BUG = re.compile(r"^\[CoopBug\] ")
RE_BUG_FLEW = re.compile(
    r"^\[CoopBug\] after the load (.*) \(([^,()]*), (\d+), ([\d.]+) kg\) flew at ([\d.]+) m/s"
    r"(?: at (-?[\d.]+) (-?[\d.]+) (-?[\d.]+))?(?:: slowed to ([\d.]+))?")
RE_BUG_FLEW_CUT = re.compile(r"^\[CoopBug\] after the load (.*?)(?: \(|$)")
RE_BUG_KIND = re.compile(r"^\[CoopBug\] kind=(\w+)(?: sev=(\d))?(.*)$")
RE_KV = re.compile(r"(\w+)=(\S*)")
RE_SLOWED = re.compile(r"^\[CoopSave\] the 10 s after the load are over: (\d+) things were slowed down")

HIT_MIN = 100.0         # a hit this strong takes half of the 200 health
AFTER_LOAD_S = 30       # D07 window after a load
BREAK_WINDOW_S = 10     # break count window after a load
BREAKS_MIN = 20         # pieces of non-vegetation broken off in that window
FLING_V = 25.0          # Settle() slows from 20 m/s; the design's phys_fling after a load is 25


# --- windows after a load ------------------------------------------------------

def _next_start(ctx, load):
    """Line number where the next load starts (its window ends there)."""
    for ld in ctx.ev.loads:
        if ld.start.no > load.start.no:
            return ld.start.no
    return 1 << 30


def window_after(ctx, load, seconds):
    """Lines from the load's end to `seconds` later (time stamps, else the
    server's periodic lines in CLOCKS, else Events.window_after's line
    count), never into the next load."""
    if load.end is None:
        return []
    stop = _next_start(ctx, load)
    if load.end.t is None:
        # the k-th tick of a clock with period p after the end comes
        # (k-1)*p .. k*p s later: stop at k = seconds/p + 1. No line cap
        # here: a collapse can write 1500 [brk] lines before the first
        # tick (brk_on1_host.log 479-2021, the player died at 948)
        allowed = [seconds // p + 1 for _, p in CLOCKS]
        seen = [0] * len(CLOCKS)
        out = []
        for l in ctx.ev.log.lines[load.end.no:]:
            if l.no >= stop:
                break
            s = l.text
            if s.startswith("[Coop"):
                hit = False
                for i, (rx, _) in enumerate(CLOCKS):
                    if rx.match(s):
                        seen[i] += 1
                        hit = hit or seen[i] >= allowed[i]
                if hit:
                    break
            out.append(l)
        if any(seen):
            return out
        # no clock after this load at all (a DLL without them, or the log
        # ends): the core's line count
    w = ctx.ev.window_after(load, seconds)
    return [l for l in w if l.no < stop]


def _load_ranges(ctx, seconds):
    """[(load, first, last)]: from the end of LoadGame ("the saved game took",
    physics may already run there) to the end of the window."""
    out = []
    for ld in ctx.ev.loads:
        if ld.end is None:
            continue
        w = window_after(ctx, ld, seconds)
        first = ld.took_line.no if ld.took_line is not None else ld.end.no
        last = w[-1].no if w else ld.end.no
        out.append((ld, first, last))
    return out


def _after(load, line):
    """Where the line is from the load's end, for the text."""
    if line.no < load.end.no:
        return "%d lines before" % (load.end.no - line.no)
    if load.end.t is not None and line.t is not None:
        return "%d s after" % ((line.t - load.end.t) % 86400)
    return "%d lines after" % (line.no - load.end.no)


def _fmt(v):
    return ("%.1f" % v).rstrip("0").rstrip(".")


def _short(model):
    return os.path.basename(model.replace("\\", "/")) or model


def _ru_n(n, one, few, many):
    """1 обломок, 2 обломка, 5 обломков."""
    if n % 10 == 1 and n % 100 != 11:
        return "%d %s" % (n, one)
    if 2 <= n % 10 <= 4 and not 12 <= n % 100 <= 14:
        return "%d %s" % (n, few)
    return "%d %s" % (n, many)


# --- D07: collision damage -------------------------------------------------------

def _what_hit(name, by, default):
    """What hit the player: the line says "by <the player>" for debris with
    no owner (the user's hit=485.3), the object's name otherwise
    ("by storage/local/barrels.empty_metal_barrel_d10")."""
    if not by or by in (name, "nil"):
        return default
    return _short(by)


def collision_damage(ctx):
    ranges = _load_ranges(ctx, AFTER_LOAD_S)
    after, other = [], []
    for l in ctx.lines(ctx.host):
        s = l.text
        if not s.startswith("[Coop] DAMAGE player ") or "type=collision" not in s:
            continue
        # (the mod's shield the first seconds after a load: no damage taken)
        if "the load has just ended" in s:
            continue
        m = RE_DAMAGE.match(s)
        if not m:
            continue
        hit = float(m.group(3))
        hp_after = int(m.group(6)) if m.group(6) is not None else None
        hp_before = int(m.group(5)) if m.group(5) is not None else None
        # a smaller hit that kills counts after a load too: fin7_manual 1147-1149:
        # debris took 157 and then the last 31 hp with a hit of 35.4
        lethal = hp_before is not None and hp_after is not None and hp_before > 0 >= hp_after
        if hit < HIT_MIN and not lethal:
            continue
        rec = (l, m.group(1), m.group(2), hit, hp_before, hp_after, lethal)
        load = None
        for ld, first, last in ranges:
            if first <= l.no <= last:
                load = ld
        if load is None and hit < HIT_MIN:
            continue        # a small bump that ends a fight is not a sign of debris
        (after if load is not None else other).append((rec, load))
    out = []
    if after:
        # a hit that killed first (that is what the player saw), the strongest of them
        after.sort(key=lambda x: (not x[0][6], -x[0][3]))
        (l, name, by, hit, a, b, dead), load = after[0]
        players = sorted({r[1] for r, _ in after})
        ru = "%s после загрузки ударили игрока %s: урон %s%s" % (
            _what_hit(name, by, "обломки"), name, _fmt(hit), ", насмерть" if dead else "")
        en = ("collision hit=%s on %s (by %s, hp %s->%s) %s the end of the load at %s; "
              "%d collision hit(s) >= %d or lethal within %d s of a load (%d lethal), players: %s") % (
            _fmt(hit), name, by, a, b, _after(load, l), load.end.where(),
            len(after), HIT_MIN, AFTER_LOAD_S, sum(1 for r, _ in after if r[6]), ", ".join(players))
        ev = [r[0] for r, _ in after]
        loads_seen = []
        for _, ld in after:
            if ld.end not in loads_seen:
                loads_seen.append(ld.end)
        f = Finding("D07", "FAIL", ru, en, ev[:6] + loads_seen[:2], key="collision-after-load")
        f.count = len(after)
        out.append(f)
    if other:
        other.sort(key=lambda x: -x[0][3])
        (l, name, by, hit, a, b, _), _ = other[0]
        ru = "игрока %s сильно ударило: %s, не сразу после загрузки, урон %s" % (
            name, _what_hit(name, by, "предмет"), _fmt(hit))
        en = "collision hit=%s on %s (by %s, hp %s->%s) outside the %d s after a load; %d such hit(s) >= %d" % (
            _fmt(hit), name, by, a, b, AFTER_LOAD_S, len(other), HIT_MIN)
        f = Finding("D07", "WARN", ru, en, [r[0] for r, _ in other], key="collision-outside-load")
        f.count = len(other)
        out.append(f)
    return out


# --- D07: buildings breaking after a load -------------------------------------------

def _vegetation(model):
    m = model.lower()
    return "/natural/" in m or "vegetation" in m or "/trees/" in m


def breaks_after_load(ctx):
    hits = []
    for ld in ctx.ev.loads:
        if ld.end is None or not ctx.in_window(ld.end):
            continue
        parts = []
        for l in window_after(ctx, ld, BREAK_WINDOW_S):
            s = l.text
            if s.startswith("[brk] OnCreatePhysEntityPart entity["):
                m = RE_PART.match(s)
                model = m.group(2) if m else None
            elif s.startswith("CPE: "):
                model = s[5:].strip()
            else:
                continue
            if model and not _vegetation(model):
                parts.append((l, model))
        if len(parts) >= BREAKS_MIN:
            hits.append((ld, parts))
    out = []
    if hits:
        ld, parts = max(hits, key=lambda h: len(h[1]))
        counts = {}
        for _, model in parts:
            counts[model] = counts.get(model, 0) + 1
        top = sorted(counts.items(), key=lambda kv: -kv[1])
        ru = "после загрузки рушатся постройки: %s за %d с (%s)" % (
            _ru_n(len(parts), "обломок", "обломка", "обломков"), BREAK_WINDOW_S,
            ", ".join(_short(k) for k, _ in top[:2]))
        en = "%d pieces broke off (joint breaks, no vegetation) within %d s of the load ending at %s: %s; %d of %d loads like that" % (
            len(parts), BREAK_WINDOW_S, ld.end.where(), ", ".join("%s x%d" % kv for kv in top[:4]), len(hits),
            sum(1 for x in ctx.ev.loads if x.end is not None))
        # a piece of the model that broke most first (the line that shows it),
        # then the load's end
        first = next(p[0] for p in parts if p[1] == top[0][0])
        ev = [first, ld.end] + [p[0] for p in parts if p[0] is not first][:2]
        for other, _ in hits:
            if other is not ld:
                ev.append(other.end)
        f = Finding("D07", "WARN", ru, en, ev, key="breaks-after-load")
        f.count = len(hits)
        out.append(f)
    still = [l for l in ctx.lines(ctx.host) if l.text.startswith("[CoopAgent] the host's game is ready")
             and RE_STILL_BREAK.match(l.text)]
    if still:
        secs = int(RE_STILL_BREAK.match(still[0].text).group(1))
        f = Finding("D07", "WARN", "через %d с после появления игрока хоста всё ещё что-то рушилось" % secs,
                    "things still broke %d s after the host's player appeared (BREAK_QUIET wait ran out)" % secs,
                    still, key="breaks-go-on")
        f.count = len(still)
        out.append(f)
    return out


# --- D02: the mod's own bug reports -------------------------------------------------

KIND_RU = {
    "phys_fling": "после загрузки предмет летел",
    "dup_debris": "обломки задвоились",
    "dup_saved_piece": "предмет сохранения появился дважды",
    "load_leftover": "после загрузки остался лишний предмет",
    "load_breaks": "после загрузки рушатся постройки",
    "collision_hurt": "игрока ударило предметом",
    "friend_dropped_by_load": "друга выкинуло из игры при загрузке",
    "join_stall": "компаньон завис при входе в игру",
    "unarmed_after_rearm": "после загрузки солдаты без оружия",
    "hidden_noise": "слышна невидимая техника",
    "vehicle_stuck": "техника ИИ застряла",
    "actor_out_of_world": "персонаж оказался вне мира",
    "slot_protected": "загрузка чуть не удалила вещь игрока",
}
KEY_FIELDS = ("model", "name", "vehicle", "victim", "id")


def _lines_both(ctx):
    for log in (ctx.host, ctx.companion):
        for l in ctx.lines(log):
            yield l


def _flew_what(name, cls):
    """What the player saw fly, in Russian."""
    if name == "a physical thing":
        return "безымянный физический объект"
    if name:
        return _short(name)
    # a nameless piece of a break is class "Default": the class means
    # nothing to the player (it stays in the English text)
    return "обломки" if cls in ("Default", "?") else "обломки (%s)" % cls


def _flew_finding(flew, slowed):
    """One finding for every body Settle() slowed, whatever its model: the
    debris of one collapse comes with several names (model paths, empty
    names), and one load's pieces are one bug, not one bug per model.
    The "N things were slowed down" summaries only add to it."""
    if flew:
        recs = sorted(flew, key=lambda r: -r[5])
        l, name, cls, eid, mass, v = recs[0]
        whats = {}
        for r in recs:
            w = _flew_what(r[1], r[2])
            whats[w] = whats.get(w, 0) + 1
        top = sorted(whats.items(), key=lambda kv: -kv[1])
        ru = "после загрузки предметы разлетались%s: %s" % (
            ", до %s м/с" % _fmt(v) if v > 0 else "",      # (a cut line has no speed)
            ", ".join(w for w, _ in top[:3]) + (" и др." if len(top) > 3 else ""))
        en = ("after a load the mod slowed %d heavy bod%s flying near a player (%s); fastest '%s' (%s, id %s, %s kg) "
              "at %s m/s") % (len(recs), "y" if len(recs) == 1 else "ies",
                              ", ".join("%s x%d" % kv for kv in top[:4]), name, cls, eid, _fmt(mass), _fmt(v))
        ev = [r[0] for r in recs]
        count = len(recs)
        # only bodies a little over Settle()'s 20 m/s (a crate pushed by a
        # truck, a grenade's debris) and no cut line of unknown speed: WARN
        cut = any(r[5] <= 0 for r in recs)
        verdict = "FAIL" if cut or v >= FLING_V else "WARN"
    else:
        top_n = max(n for _, n in slowed)
        ru = "после загрузки предметы разлетались (мод тормозил их %d раз за 10 с)" % top_n
        en = "no [CoopBug] line, but %d load(s) ended with things slowed down" % len(slowed)
        ev = []
        count = len(slowed)
        verdict = "FAIL"
    if slowed:
        en += "; the 10 s after %d load(s): %s slow-downs (one body is slowed every 0.05 s while it flies)" % (
            len(slowed), ", ".join(str(n) for _, n in slowed))
        ev = ev[:6] + [x for x, _ in slowed]
    f = Finding("D02", verdict, ru, en, ev, key="flew-after-load")
    f.count = count
    return f


def coopbug(ctx):
    flew = []      # [(line, name, class, id, mass, v)]
    slowed = []    # [(line, n)] "the 10 s after the load are over: N things were slowed down"
    kinds = {}     # kind:sub -> [(line, kind, sev, kv)]
    other = {}
    for l in _lines_both(ctx):
        s = l.text
        if s.startswith("[CoopSave] the 10 s after the load"):
            m = RE_SLOWED.match(s)
            if m and int(m.group(1)) > 0:
                slowed.append((l, int(m.group(1))))
            continue
        if not s.startswith("[CoopBug]") or not RE_BUG.match(s):
            continue
        m = RE_BUG_KIND.match(s)
        if m:
            kind = m.group(1)
            kv = dict(RE_KV.findall(m.group(3)))
            sev = m.group(2) or kv.pop("sev", None)
            # "kind=summary suppressed=<n>" (design section 6) counts lines
            # the watchdog held back, it is not a finding of its own
            if kind == "summary":
                continue
            sev = int(sev) if sev and sev.isdigit() else None
            if sev is not None and sev < 2:
                continue
            sub = next((kv[k] for k in KEY_FIELDS if kv.get(k)), "")
            kinds.setdefault(kind + ":" + sub, []).append((l, kind, sev, kv))
            continue
        m = RE_BUG_FLEW.match(s)
        if m:
            flew.append((l, m.group(1).strip(), m.group(2), m.group(3), float(m.group(4)), float(m.group(5))))
            continue
        m = RE_BUG_FLEW_CUT.match(s)
        if m:
            flew.append((l, m.group(1).strip(), "?", "?", 0.0, 0.0))
            continue
        other.setdefault(re.sub(r"-?\d+(\.\d+)?", "N", s), []).append(l)
    out = []
    if flew or slowed:
        out.append(_flew_finding(flew, slowed))
    for key, recs in kinds.items():
        # one finding per kind and thing, as bad as its worst line (a sev 2
        # and a sev 3 line of one key would otherwise be merged under
        # whichever came first)
        recs.sort(key=lambda r: -(r[2] or 0))
        l, kind, sev, kv = recs[0]
        verdict = "FAIL" if sev is not None and sev >= 3 else "WARN"
        sub = key.split(":", 1)[1]
        ru = KIND_RU.get(kind, "мод сообщил об ошибке: " + kind)
        if sub:
            ru += ": " + _short(sub)
        en = "[CoopBug] kind=%s sev=%s %s" % (kind, sev if sev is not None else "?", " ".join("%s=%s" % x for x in kv.items()))
        f = Finding("D02", verdict, ru, en, [r[0] for r in recs], key=key)
        f.count = len(recs)
        out.append(f)
    for key, lines in other.items():
        f = Finding("D02", "FAIL", "мод сообщил об ошибке: " + lines[0].text[len("[CoopBug] "):][:80],
                    lines[0].text[:200], lines, key=key)
        f.count = len(lines)
        out.append(f)
    return out


# Finding.count is what report.py prints as "(xN)", so the Russian text does
# not repeat it: hits (D07), loads with a collapse, slowed bodies (D02).
DETECTORS = [collision_damage, breaks_after_load, coopbug]
