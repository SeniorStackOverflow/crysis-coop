"""Performance (D08), sound (D09), small oddities (D10), script errors (D11),
the companion's and the enemy gunners' idleness (D14) and the vehicle
listings of coop_test_vehicles (D15).

D08 perf (CoopAI.cpp LogHitch; no line while a level or a save loads):
  [CoopHitch] a frame took <ms> ms
      host log only, over 1000 ms and not in the settling time (30 s: by time
      stamp, else by the server's 10 s / 5 s periodic lines, else 150 lines)
      after a level load (until "[Coop] InGame" at least), a checkpoint load,
      a save, the companion's start or join (its "delayed pickup" lines),
      or the in-game menu; 10 s after a test command (the test harness's
      command file costs a ~1030 ms frame: gunParN, gunOnTruckBurst): WARN,
      one finding per log (count = such frames, the longest first). In tests
      a 1-1.6 s first frame after every level start is normal (about 650 of
      the corpus's 680 frames over 1 s). A frame of 3 s or more is a freeze
      even in the settling time (fgcomp 4113 ms, 10 s after the companion's
      join; nothing else in the corpus is that long), only the load itself
      (a checkpoint load's lines, a level load until "InGame") is left out.
  [CoopPerf] last minute: <f> fps on average, worst frame <w> ms, <n> frames
      over 100 ms; sound: ... MB[ PAUSED]
      the host under 15 fps: WARN; the AI companion under 10 fps (its game
      is held to coop_agent_fps 20, ~16 fps with Sleep's 15.6 ms steps:
      the corpus's companions run at 15-16 fps); " PAUSED" (the sound system
      paused): WARN.

D09 sound (CoopSound.cpp):
  [CoopSound] no sound came out for <n> s (...): ...                 FAIL
  [CoopSound] still no sound 5 s after the restart (...)             FAIL
  [CoopSound] last minute: ...; <n> came late (0.5 s or more, the latest <x> s); ...
      (older DLLs: "came late anyway"). The latest at 2 s or more, or 20 or
      more late sounds: WARN; the latest at 4 s or more: FAIL. Thresholds
      from the corpus: the normal runs with the unstick fix stay at 0.8 s or
      less and 9 or fewer a minute, its stress runs (sat_on, burst_on) at
      3.3 s at most; every run with the fix off (coop_sound_unstick 0:
      notail_old, heavy_old, heavy_old2, burst_off, quiet_off, sat_off,
      ab_off) reaches 4.3-145 s.

D10 misc, WARN only:
  [CoopNet] NetSerialize failed: <name> (<class>, id <n>) aspect=<a> profile=<p> reading line=<l>
      the 45000 lines of the corpus and the user's logs are all one shape,
      a weapon accessory or ammo (scope, ammo, silencer, LAM, flashlight,
      grenade launcher ...) with aspect=8 line=-2 (the noise baseline, left
      out); any other class, aspect or line is reported.
  [CoopVeh] <who> came out of <vehicle> <d> m from where he got out (...): put back there (...)
      (<vehicle> is "a vehicle" when it is gone)
  [Coop] <vehicle>'s camera in <seat> was <d> m off (view <v>): view <w> now
      (PlayerView.cpp) except right after the test harness moved a seated
      player (a "[CoopTest] cmdfile:" with MovePlayer up to 40 lines before):
      all 16 such lines of goal_runs (gun*, heli2, hdiagA: Civ_Car9 145-408 m
      off) are that; the companion's line is left out when the host's camera
      in the same vehicle was (the companion's log has no command lines).
  [CoopSave] <when>: seat <s> of <vehicle> held a passenger who is not in
      the game (id <n>): NOT FREED
      ("freed" is the fix doing its work before and after every load, 81
      corpus logs: left out)

D11 script (CryScriptSystem.dll's own strings; real: "[Warning] [Lua Error]
  RMI via server (to client) requested but we are not a server" followed by
  "      > ClClientDisconnect, ([C]: -1)" stack lines):
  [Lua Error] <msg>, [Script Error] <msg>, [CScriptSystem::BeginCall] <msg>,
  [Lua] Error <msg>, "script error", [CoopCheck] what=lua ok=0 err=<msg>
      FAIL, key = the message with digits stripped. Not reported: errors
      after the session's end ("Disconnect ...; msg='Nub destroyed'", until
      the next level load; the only real one, 09-29, is that: the game was
      being left). INFRA: the test harness's own Lua command failed (an error
      up to 3 lines after a "[CoopTest] cmdfile: #" line, or one about a
      '[string "...' chunk, which only console commands make).

D14 idle gunner:
  [CoopAgent] the last minute: <b> bursts fired; no shot: <w> frames something
      in the way, <u> enemy not in sight, <t> turning to him; stuck <s> times[, ...]
      (CoopAgent.cpp; the companion's log, which runs at ~16 fps). b = 0 and
      w+u+t >= 150 frames (about 10 s of trying to shoot, never once): WARN.
      The corpus's zero-burst minutes: compgun_base2 777, compgun_fix1 956,
      contnew 882 (the gun-turning bug), the user's 10-15 16:46 353; under
      that fin_convoy 83 (a 5 s fight cut short by the test killing the
      convoy) and revfar_old 41 (on the way to revive the host), the rest 0-7.
      s >= 5: WARN (revfar_new: 6 and 13; all others 0-3).
  [CoopAI] <gunner> at the gun of <vehicle> [(no driver)] sees <target> and
      has not fired for 5 s: ...  (CoopAI.cpp UpdateIdleGunners, host)  WARN

D15 vehicles (coop_test_vehicles, CoopAgent.cpp CmdTestVehicles +
  VehicleMovementBase.cpp CoopEngineState):
  [CoopTest] vehicle <name> <class> at <d> m (<x> <y> <z>)[ destroyed| hidden]
      <seats>; <ai>; engine powered=<p> starting=.. off=.. disabled=..[ <snd>:<state> vol <v> to <r> m]...
  hidden, run:playing (not "playing(virtual)") with vol > 0: FAIL, the hum
      of a hidden helicopter. coop_test_watch's lines carry the same engine
      state and are read the same way:
      [CoopWatch] <name> at (x y z) speed <v> hidden; ai ...; driver <d>; engine powered=..
  visible, an AI driver (", AI driver's ai"), powered=1, the vehicle's own
      AI off, at the same place (5 m) in two listings (50 s or more apart
      when the log has time stamps; otherwise two separate listings): WARN
      stuck. The design's "ai on" is left out on purpose: farm_Jeep_2 and
      the convoy truck stand crewed with their AI on in the fixed builds'
      runs (fgcomp, hfinal), while the bug of 0.11.30 (the AI reset after a
      load) turns the crewed vehicle's AI off (hscopeA: the hovering
      Asian_Helicopter_1 and the frozen Jungle_Convoy_Truck). Listings
      without the ai field (DLLs before 2026-10-13) are not judged.
"""

import re

from ..findings import Finding

# --- shared helpers --------------------------------------------------------

# periodic lines of the server (CoopAI.cpp), a clock for logs without time stamps
CLOCKS = [(re.compile(r"^\[CoopAI\] AI shots so far: "), 10),
          (re.compile(r"^\[CoopPos\] server:"), 5)]


def _logs(ctx):
    out = [ctx.host]
    if ctx.companion is not None:
        out.append(ctx.companion)
    return out


def _who(log):
    return "компаньона" if log.role == "companion" else "хоста"


def _who_en(log):
    return "companion" if log.role == "companion" else "host"


def _dt(a, b):
    """Seconds from line a to line b, or None without time stamps."""
    if a.t is None or b.t is None:
        return None
    return (b.t - a.t) % 86400


def _digits(s):
    return re.sub(r"\d+", "N", s)


# --- D08 perf --------------------------------------------------------------

RE_HITCH = re.compile(r"^\[CoopHitch\] a frame took (\d+) ms")
RE_PERF = re.compile(r"^\[CoopPerf\] last minute: ([\d.]+) fps on average, worst frame (\d+) ms, (\d+) frames over 100 ms(.*)$")
# moments after which a long frame is the game settling, not a freeze
RE_SETTLE = re.compile(
    r"\[Coop\] PreGame -> InGame|\[Coop\] InGame: |\[Coop\] checkpoint loaded: "
    r"|\[CoopSave\] loading |\[CoopSave\] progress loaded: |\[CoopSave\] checkpoint .* saved"
    r"|\[CoopAgent\] AI companion started|\[Coop\] revived Companion"
    r"|\[CoopNet\] delayed pickup: |\[CoopTest\] cmdfile: |\[CoopSave\] continuing campaign"
    r"|\[CoopMusic\] (?:the host's )?theme \"menu\"")
# (not "[Coop..." lines; looked for only in lines that have the word)
RE_SETTLE_OTHER = re.compile(r"renamed to .*Companion$|Menus_IngameMenu\.gfx")
RE_LEVEL_LOADED = re.compile(r"^\*LOADING: Level ")
RE_INGAME = re.compile(r"^\[Coop\] (?:PreGame -> InGame|InGame: )")
TEST_CMD = "[CoopTest] cmdfile: "
HITCH_MS = 1000
FREEZE_MS = 3000        # a freeze even while the game settles
SETTLE_S = 30
TEST_CMD_S = 10         # a test command's own long frame comes at once
SETTLE_LINES = 150
LOW_FPS = 15
LOW_FPS_COMPANION = 10  # the AI companion's game is held to ~16 fps


def _settle_ranges(ctx, log):
    """(first, last, hard) line ranges in which a long frame is no freeze:
    hard = a checkpoint load or a level load until the game is on ("[Coop]
    InGame", the first frames come before it); soft = 30 s after anything
    heavy (10 s after a test command)."""
    lines = log.lines
    out = []
    run_start = None
    for ld in ctx.ev.loads if log is ctx.host else []:
        out.append((ld.start.no - 5, ld.end.no if ld.end else ld.start.no + 400, True))
    for i, l in enumerate(lines):
        s = l.text
        if not s:
            continue
        c = s[0]
        if c == "*" and RE_LEVEL_LOADED.match(s):
            for x in lines[i + 1:i + 3000]:
                if RE_INGAME.match(x.text):
                    out.append((l.no, x.no, True))
                    break
            out.append((l.no, _settle_end(lines, i, SETTLE_S), False))
        elif (c == "[" and s.startswith("[Coop") and RE_SETTLE.match(s)) or \
                (("Companion" in s or "Menus_IngameMenu" in s) and RE_SETTLE_OTHER.search(s)):
            if i + 1 < len(lines) and s.startswith("[CoopNet] delayed pickup") \
                    and lines[i + 1].text.startswith("[CoopNet] delayed pickup"):
                # a run of the join's pickups: one window, to 30 s after its last line
                if run_start is None:
                    run_start = l.no
                continue
            seconds = TEST_CMD_S if s.startswith(TEST_CMD) else SETTLE_S
            out.append((run_start or l.no, _settle_end(lines, i, seconds), False))
            run_start = None
    return out


def _settle_end(lines, i, seconds):
    """The last line number within `seconds` after lines[i]: by time stamp,
    else by the server's periodic lines, else SETTLE_LINES lines per 30 s."""
    a = lines[i]
    if a.t is not None:
        last = a.no
        for l in lines[i + 1:]:
            if l.t is not None and (l.t - a.t) % 86400 > seconds:
                break
            last = l.no
        return last
    ticks = {id(rx): 0 for rx, _ in CLOCKS}
    for l in lines[i + 1:min(len(lines), i + 1 + 4000)]:
        for rx, period in CLOCKS:
            if rx.match(l.text):
                ticks[id(rx)] += 1
                if ticks[id(rx)] > seconds // period:
                    return l.no
    return a.no + SETTLE_LINES * seconds // SETTLE_S


def d08_perf(ctx):
    out = []
    # long frames: the host's (the player's) game
    log = ctx.host
    ranges = None
    long_frames = []
    for l in ctx.lines(log):
        s = l.text
        if not s.startswith("[CoopHitch]"):
            continue
        m = RE_HITCH.match(s)
        if not m or int(m.group(1)) <= HITCH_MS:
            continue
        ms = int(m.group(1))
        if ranges is None:
            ranges = _settle_ranges(ctx, log)
        if any(a <= l.no <= b and (hard or ms < FREEZE_MS) for a, b, hard in ranges):
            continue
        long_frames.append((ms, l))
    if long_frames:
        long_frames.sort(key=lambda x: -x[0])
        worst = long_frames[0][0]
        f = Finding("D08", "WARN",
                    "игра замирала: кадр шёл дольше секунды (до %.1f с) в обычной игре" % (worst / 1000.0),
                    "%d frame(s) over %d ms outside loads, the longest %d ms" % (len(long_frames), HITCH_MS, worst),
                    [l for _, l in long_frames[:8]], key="hitch:" + log.role)
        f.count = len(long_frames)
        out.append(f)
    # minute lines of both games
    for log in _logs(ctx):
        slow = []
        paused = []
        low = LOW_FPS_COMPANION if log.role == "companion" else LOW_FPS
        for l in ctx.lines(log):
            s = l.text
            if not s.startswith("[CoopPerf] last minute"):
                continue
            m = RE_PERF.match(s)
            if not m:
                continue
            fps = float(m.group(1))
            if fps < low:
                slow.append((fps, l))
            if m.group(4).rstrip().endswith("PAUSED"):
                paused.append(l)
        if slow:
            slow.sort(key=lambda x: x[0])
            f = Finding("D08", "WARN",
                        "игра %s тормозила: %.0f кадров в секунду за минуту" % (_who(log), slow[0][0]),
                        "%s: %d minute(s) under %d fps, the lowest %.0f fps" % (_who_en(log), len(slow), low, slow[0][0]),
                        [l for _, l in slow[:8]], key="lowfps:" + log.role)
            f.count = len(slow)
            out.append(f)
        if paused:
            f = Finding("D08", "WARN",
                        "звук в игре %s стоял на паузе" % _who(log),
                        "%s: the sound system was PAUSED in %d minute line(s)" % (_who_en(log), len(paused)),
                        paused[:8], key="paused:" + log.role)
            f.count = len(paused)
            out.append(f)
    return out


# --- D09 sound -------------------------------------------------------------

RE_SILENT = re.compile(r"^\[CoopSound\] no sound came out for (\d+) s \(([^;)]*)")
RE_RESTART_FAILED = re.compile(r"^\[CoopSound\] still no sound 5 s after the restart(?: \(([^)]*)\))?")
RE_BACK = re.compile(r"^\[CoopSound\] the sound comes out again after the restart")
RE_SND_MINUTE = re.compile(r"^\[CoopSound\] last minute: ")
RE_LATE = re.compile(r"(\d+) came late(?: anyway)? \(0\.5 s or more, the latest ([\d.]+) s\)")
LATE_WARN_S = 2.0
LATE_FAIL_S = 4.0
LATE_WARN_N = 20


def d09_sound(ctx):
    out = []
    for log in _logs(ctx):
        lines = ctx.lines(log)
        late = []
        for i, l in enumerate(lines):
            s = l.text
            if not s.startswith("[CoopSound] "):
                continue
            m = RE_SILENT.match(s)
            if m:
                ev = [l]
                back = None
                for x in lines[i + 1:i + 400]:
                    if RE_BACK.match(x.text) or RE_RESTART_FAILED.match(x.text):
                        back = x
                        break
                if back is not None:
                    ev.append(back)
                tail = " (звук вернулся после перезапуска)" if back is not None and RE_BACK.match(back.text) else ""
                out.append(Finding("D09", "FAIL",
                                   "в игре %s пропал звук на %s с%s" % (_who(log), m.group(1), tail),
                                   "%s: no sound came out for %s s (%s)" % (_who_en(log), m.group(1), m.group(2)),
                                   ev, key="silence:" + log.role))
                continue
            m = RE_RESTART_FAILED.match(s)
            if m:
                out.append(Finding("D09", "FAIL",
                                   "звук в игре %s не вернулся после перезапуска звуковой системы" % _who(log),
                                   "%s: still no sound 5 s after the restart (%s)" % (_who_en(log), m.group(1) or "?"),
                                   [l], key="restart_failed:" + log.role))
                continue
            if RE_SND_MINUTE.match(s):
                m = RE_LATE.search(s)
                if not m:
                    continue
                n, latest = int(m.group(1)), float(m.group(2))
                if latest >= LATE_WARN_S or n >= LATE_WARN_N:
                    late.append((latest, n, l))
        if late:
            late.sort(key=lambda x: (-x[0], -x[1]))
            latest, n, _ = late[0]
            total = sum(x[1] for x in late)
            verdict = "FAIL" if latest >= LATE_FAIL_S else "WARN"
            f = Finding("D09", verdict,
                        "звуки в игре %s запаздывали до %.1f с (%d опоздавших за минуту)" % (_who(log), latest, n),
                        "%s: sounds came late in %d minute(s), the latest %.1f s, %d late in that minute, %d in all"
                        % (_who_en(log), len(late), latest, n, total),
                        [x[2] for x in late[:8]], key="late:%s:%s" % (log.role, verdict))
            f.count = len(late)
            out.append(f)
    return out


# --- D10 misc --------------------------------------------------------------

RE_NETSER = re.compile(r"^\[CoopNet\] NetSerialize failed: (.*) \(([^,()]*), id (\d+)\) aspect=(-?\d+) profile=(-?\d+) (\w+) line=(-?\d+)")
# the corpus's only shape: accessories and ammo of weapons, aspect 8, line -2
NETSER_NOISE_CLASS = re.compile(r"(Scope|Ammo|Silencer|LAM\w*|FlashLight|GrenadeLauncher|Reflex|GrenadeShell)$")
RE_PUT_BACK = re.compile(r"^\[CoopVeh\] (.+?) came out of (.+?) (\d+) m from where he got out .*: put back there")
RE_CAMERA = re.compile(r"^\[Coop\] (.+?)'s camera in (\S+) was (\d+) m off")
RE_SEAT = re.compile(r"^\[CoopSave\] ([^:]*): seat (\S+) of (\S+) held a passenger who is not in the game \(id (\d+)\): (.*)$")
TELEPORT_LINES = 40


def _teleported_cameras(lines):
    """Line numbers of camera lines that come right after the test harness
    moved a player ("[CoopTest] cmdfile: ...MovePlayer..."), and the vehicles."""
    nos, vehicles = set(), set()
    moved_at = None
    for l in lines:
        s = l.text
        if s.startswith(TEST_CMD) and "MovePlayer" in s:
            moved_at = l.no
        elif moved_at is not None and l.no - moved_at <= TELEPORT_LINES and s.startswith("[Coop] "):
            m = RE_CAMERA.match(s)
            if m:
                nos.add(l.no)
                vehicles.add(m.group(1))
    return nos, vehicles


def d10_misc(ctx):
    out = []
    test_nos, test_vehicles = _teleported_cameras(ctx.host.lines)
    for log in _logs(ctx):
        for l in ctx.lines(log):
            s = l.text
            if not s.startswith("[Coop"):
                continue
            if s.startswith("[CoopNet] NetSerialize failed"):
                m = RE_NETSER.match(s)
                if not m:
                    continue
                cls, aspect, line = m.group(2), m.group(4), m.group(7)
                if aspect == "8" and line == "-2" and NETSER_NOISE_CLASS.search(cls):
                    continue
                out.append(Finding("D10", "WARN",
                                   "сбой передачи по сети: %s (%s)" % (cls, _who(log)),
                                   "%s: NetSerialize failed for %s %s aspect=%s %s line=%s"
                                   % (_who_en(log), cls, m.group(1), aspect, m.group(6), line),
                                   [l], key="netser:%s:%s:%s" % (_digits(cls), aspect, line)))
                continue
            m = RE_PUT_BACK.match(s)
            if m:
                out.append(Finding("D10", "WARN",
                                   "%s после выхода из машины оказался в %s м от места выхода, его вернули туда" % (m.group(1), m.group(3)),
                                   "%s came out of %s %s m from where he got out: put back" % (m.group(1), m.group(2), m.group(3)),
                                   [l], key="putback:%s:%s" % (m.group(1), m.group(2))))
                continue
            m = RE_CAMERA.match(s)
            if m:
                # the test moved a player sitting in it: the camera stays behind
                if (l.no in test_nos) if log is ctx.host else (m.group(1) in test_vehicles):
                    continue
                out.append(Finding("D10", "WARN",
                                   "камера %s в машине улетела на %s м от машины" % (_who(log), m.group(3)),
                                   "%s: camera in %s of %s was %s m off" % (_who_en(log), m.group(2), m.group(1), m.group(3)),
                                   [l], key="camera:%s:%s" % (_digits(m.group(1)), m.group(2))))
                continue
            m = RE_SEAT.match(s)
            if m and m.group(5).startswith("NOT FREED"):
                out.append(Finding("D10", "WARN",
                                   "место в машине занято игроком, которого нет в игре, и не освобождено",
                                   "%s: seat %s of %s held a passenger not in the game (id %s): NOT FREED"
                                   % (m.group(1), m.group(2), m.group(3), m.group(4)),
                                   [l], key="seat:%s:%s" % (_digits(m.group(3)), m.group(2))))
    return out


# --- D11 script ------------------------------------------------------------

RE_SCRIPT = re.compile(
    r"\[Lua Error\] ?(.*)$|\[Script Error\] ?(.*)$|\[CScriptSystem::BeginCall\] ?(.*)$"
    r"|\[Lua\] Error:? ?(.*)$|\[CoopCheck\] what=lua ok=0(?: err=(.*))?$|(?i:\bscript error\b:? ?(.*))$")
RE_STACK = re.compile(r"^\s+> ")
SESSION_END = "msg='Nub destroyed'"     # [net ...] Disconnect <local>:..; cause=13; msg='Nub destroyed'
TEST_LUA_LINES = 3


def d11_script(ctx):
    out = []
    for log in _logs(ctx):
        lines = ctx.lines(log)
        ended = False       # the session is over: the game is being left
        test_lua = None     # the harness's last Lua command
        for i, l in enumerate(lines):
            s = l.text
            if not s:
                continue
            if s[0] == "*" and RE_LEVEL_LOADED.match(s):
                ended = False
                continue
            if s.startswith(TEST_CMD):
                if s.startswith(TEST_CMD + "#"):
                    test_lua = l
                continue
            if SESSION_END in s:
                ended = True
                continue
            if "rror" not in s and "BeginCall" not in s and "what=lua" not in s:
                continue
            if s.startswith("[Error] <Flash>") or s.startswith("[CHK]"):
                continue
            m = RE_SCRIPT.search(s)
            if not m or ended:
                continue
            msg = next((g for g in m.groups() if g), "").strip() or s.strip()
            ev = [l]
            for x in lines[i + 1:i + 6]:
                if not RE_STACK.match(x.text):
                    break
                ev.append(x)
            if (test_lua is not None and l.no - test_lua.no <= TEST_LUA_LINES) or '[string "' in msg:
                if test_lua is not None and l.no - test_lua.no <= TEST_LUA_LINES:
                    ev.append(test_lua)
                out.append(Finding("D11", "INFRA",
                                   "команда теста на Lua с ошибкой (%s): %s" % (_who(log), msg[:100]),
                                   "%s: the test's own Lua command failed: %s" % (_who_en(log), msg[:200]),
                                   ev, key="test_lua:" + _digits(msg)[:200]))
                continue
            out.append(Finding("D11", "FAIL",
                               "ошибка в скрипте игры (%s): %s" % (_who(log), msg[:100]),
                               "%s: script error: %s" % (_who_en(log), msg[:200]),
                               ev, key="script:" + _digits(msg)[:200]))
    return out


# --- D14 idle gunner -------------------------------------------------------

RE_AGENT_MINUTE = re.compile(
    r"^\[CoopAgent\] the last minute: (\d+) bursts fired; no shot: (\d+) frames something in the way, "
    r"(\d+) enemy not in sight, (\d+) turning to him; stuck (\d+) times")
RE_FIGHTING = re.compile(r"^\[CoopAgent\] (?:.* )?fighting \S")
RE_ENEMY_IDLE = re.compile(r"^\[CoopAI\] (\S+) at the gun of (\S+) (?:\(no driver\) )?sees (.+?) and has not fired for 5 s")
NO_SHOT_FRAMES = 150
STUCK_TIMES = 5


def d14_idle_gunner(ctx):
    out = []
    for log in _logs(ctx):
        lines = ctx.lines(log)
        prev = 0
        for i, l in enumerate(lines):
            s = l.text
            if s.startswith("[CoopAgent] the last minute"):
                m = RE_AGENT_MINUTE.match(s)
                if not m:
                    continue
                bursts, way, unseen, turning, stuck = (int(g) for g in m.groups())
                lo = max(prev, i - 400)
                prev = i + 1
                noshot = way + unseen + turning
                if bursts == 0 and noshot >= NO_SHOT_FRAMES:
                    fight = [x for x in lines[lo:i] if x.text.startswith("[CoopAgent]") and RE_FIGHTING.match(x.text)]
                    out.append(Finding("D14", "WARN",
                                       "компаньон целую минуту не стрелял, хотя дрался (%d кадров без выстрела)" % noshot,
                                       "companion: 0 bursts in a minute, %d frames without a shot (%d in the way, %d not in sight, %d turning)"
                                       % (noshot, way, unseen, turning),
                                       [l] + fight[-1:], key="zero_bursts"))
                if stuck >= STUCK_TIMES:
                    out.append(Finding("D14", "WARN",
                                       "компаньон застревал %d раз за минуту" % stuck,
                                       "companion: stuck %d times in a minute" % stuck,
                                       [l], key="stuck"))
                continue
            if s.startswith("[CoopAI] ") and "has not fired for 5 s" in s:
                m = RE_ENEMY_IDLE.match(s)
                if not m:
                    continue
                out.append(Finding("D14", "WARN",
                                   "враг у пулемёта %s видел игрока %s и не стрелял 5 с" % (m.group(2), m.group(3)),
                                   "%s at the gun of %s saw %s and did not fire for 5 s" % (m.group(1), m.group(2), m.group(3)),
                                   [l], key="enemy_gunner:%s:%s" % (m.group(1), m.group(2))))
    return out


# --- D15 vehicles ----------------------------------------------------------

RE_VEHICLE = re.compile(r"^\[CoopTest\] vehicle (\S+) (\S+) at (\d+) m \((-?\d+) (-?\d+) (-?\d+)\)( destroyed| hidden)?(.*)$")
# coop_test_watch's line of a vehicle (CoopAI.cpp, once a second while it changes)
RE_WATCH_HIDDEN = re.compile(r"^\[CoopWatch\] (\S+) at \((-?\d+) (-?\d+) (-?\d+)\) speed [\d.]+ hidden[ ;].*; driver ")
RE_RUN = re.compile(r" run:playing vol ([\d.]+)")
STUCK_M = 5.0
STUCK_S = 50


def _listings(lines):
    """Vehicle lines grouped into listings (one coop_test_vehicles each)."""
    out = []
    last = -100
    for l in lines:
        if not l.text.startswith("[CoopTest] vehicle "):
            continue
        m = RE_VEHICLE.match(l.text)
        if not m:
            continue
        if l.no - last > 5:
            out.append([])
        last = l.no
        out[-1].append((l, m))
    return out


def _hum(name, cls, vol, l, where):
    heli = any(w in (cls or name).lower() for w in ("helicopter", "heli", "vtol"))
    return Finding("D15", "FAIL",
                   ("слышен гул спрятанного вертолёта %s" if heli else "слышен мотор спрятанной машины %s") % name,
                   "%s%s is hidden but its run sound plays audibly (vol %s%s)" % (name, " " + cls if cls else "", vol, where),
                   [l], key="hum:" + name)


def d15_vehicles(ctx):
    out = []
    for log in _logs(ctx):
        lines = ctx.lines(log)
        for l in lines:
            s = l.text
            if s.startswith("[CoopWatch] ") and " hidden" in s:
                m = RE_WATCH_HIDDEN.match(s)
                r = RE_RUN.search(s) if m else None
                if r and float(r.group(1)) > 0.0:
                    out.append(_hum(m.group(1), None, r.group(1), l, ""))
        listings = _listings(lines)
        if not listings:
            continue
        # a hidden vehicle heard
        for li in listings:
            for l, m in li:
                if m.group(7) != " hidden":
                    continue
                r = RE_RUN.search(m.group(8))
                if not r or float(r.group(1)) <= 0.0:
                    continue
                out.append(_hum(m.group(1), m.group(2), r.group(1), l, ", %s m away" % m.group(3)))
        # a crewed AI vehicle standing still with its AI off
        last_seen = {}
        for li in listings:
            for l, m in li:
                rest = m.group(8)
                name = m.group(1)
                ok = (m.group(7) is None and "AI driver's ai" in rest and "; ai off," in rest
                      and "engine powered=1" in rest)
                if not ok:
                    last_seen.pop(name, None)
                    continue
                pos = (int(m.group(4)), int(m.group(5)), int(m.group(6)))
                before = last_seen.get(name)
                if before is None:
                    last_seen[name] = (l, pos)
                    continue
                l0, p0 = before
                dist = sum((a - b) ** 2 for a, b in zip(pos, p0)) ** 0.5
                dt = _dt(l0, l)
                if dist > STUCK_M:
                    last_seen[name] = (l, pos)
                    continue
                if dt is not None and dt < STUCK_S:
                    continue
                out.append(Finding("D15", "WARN",
                                   "%s с водителем-ИИ и заведённым мотором стоит на месте (ИИ машины выключен)" % name,
                                   "%s %s: AI driver, engine on, vehicle AI off, still at (%d %d %d) in two listings%s"
                                   % (name, m.group(2), pos[0], pos[1], pos[2], " %d s apart" % dt if dt is not None else ""),
                                   [l, l0], key="stuck:" + name))
                last_seen[name] = (l, pos)
    return out


DETECTORS = [d08_perf, d09_sound, d10_misc, d11_script, d14_idle_gunner, d15_vehicles]
