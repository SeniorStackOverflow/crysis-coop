"""D01 crashes, and D12's "the mod is not loaded" check.

The crash block is written by C1-Launcher (Crysis.exe), not by the mod:

  ================================ CRASH DETECTED ================================
  C1-Launcher Game v7 32-bit
  Access violation (0xC0000005) at 0x6AD8A761: Read from 0x00000021 failed
  Physical memory = ...                         (4 memory lines)
  Registers:
  EIP: 6AD8A761 ESP: ...                        (RIP/RAX... in a 64-bit game)
  Callstack:
  6AD8A761 Coop.dll: ?? ()
  00000000 ??: ?? ()                            (a call through a null pointer)
  Modules (128):
  6ACF0000 - 6B0F4000 C:\\GOG Games\\Crysis\\Mods\\Coop\\Bin32\\Coop.dll
  Command line:
  "C:\\GOG Games\\Crysis\\Bin32\\Crysis.exe" -mod Coop ...
  ================================================================================

Other threads keep writing while it is written, so lines of the game can sit
inside the block (seen between Registers and Callstack); they are skipped.
Other exceptions seen: "Integer divide by zero (0xC0000094) at 0x02BE4DEF"
(no ": detail"), "... Write to 0x05051284 failed", 16-digit addresses.

Each frame is resolved to module+offset with the Modules list. Frames in the
mod's DLL (Mods\\<mod>\\Bin32\\<mod>.dll: Coop.dll, CoopTest.dll) are named from
ctx.map_path (the MSVC linker map of the DLL that wrote the log): the nearest
public or static symbol at or below the frame. The map fits only its own build,
and an older crash named from a newer map gets wrong names, so: no
"[CoopBuild] version=... built=<__DATE__>_<__TIME__> dll=<path>" line (written
by CoopAgent::Init from CGame::Init, CoopAgent.cpp; from 0.11.35) = no names;
built= later than the map's link time = no names; built= more than
SAME_BUILD_S before the link = names in en only, marked UNSURE (CoopAgent.cpp
may not have been compiled again in a later build). Windows' report carries the
module's PE time stamp, which matches the map's "Timestamp is" exactly.

The finding says what the game was doing: a checkpoint load (the host's
Events), quitting ("System:Quit", "[CoopTest] cmdfile: quit"; not "Nub
destroyed": a Continue ends the old game's network too), the all-down load
("[Coop] everybody is down: the last checkpoint is loaded"), loading a level
("[CoopAI] level loading:", "[Coop] gamerules module loaded", "Game rules
class:" without "*LOADING: Level" after it), just after one, a Continue
("[CoopSave] continuing campaign"), joining the host ("[CoopAgent] joining the
host:", "[CoopRelay] joining coop game"), or starting up (no game DLL yet).
Nothing when the game was playing ([CoopPos]/[CoopAIPos]/[CoopPerf] lines
after the last of those).

Keys are "<role>:<module>+<offset>": the host's and the companion's crash at
one place are two findings.

Windows' own crash report is recognised too, when a log carries it (the old
harness put it in its result files): "Faulting application name: Crysis.exe,
... Faulting module name: Coop.dll ... Exception code: 0xc0000409 ... Fault
offset: 0x002bd38e" (or the same in Russian: "Имя сбойного приложения", "Имя
сбойного модуля", "Код исключения", "Смещение ошибки"), and "has stopped
working" / "Application Error" lines naming Crysis.

A log that just ends (no "System:Quit", no crash block) is NOT a crash: tests
kill games. That is never reported here.

D12 (INFRA): the game initialised its game DLL ("[PlayerProfiles] Login of
user", "[GameProfiles]: Successfully activated", "*LOADING: Level") but no line
shows the mod's DLL ran: "[CoopBuild] ..." or any "[Coop<Tag>] " line (only
the DLL writes those; the mod's Lua writes plain "[Coop]"). Also when a crash
block's Modules list has no Mods\\<mod>\\Bin32\\*.dll although the crash came
after the game DLL started, or the DLL that ran is not the one of the mod
folder in "Added MOD directory".
"""

import bisect
import os
import re
import time

from ..findings import Finding

# ---------------------------------------------------------------- crash block

RE_BANNER = re.compile(r"^=+ CRASH DETECTED =+\s*$")
RE_END = re.compile(r"^={40,}\s*$")
RE_LAUNCHER = re.compile(r"^C1-Launcher ")
RE_EXC = re.compile(r"^(?P<what>[A-Za-z][A-Za-z0-9 +/'-]*?) \((?P<code>0x[0-9A-Fa-f]{8})\) at (?P<addr>0x[0-9A-Fa-f]{8,16})(?:: (?P<detail>.*))?\s*$")
RE_FRAME = re.compile(r"^(?P<addr>[0-9A-Fa-f]{8}|[0-9A-Fa-f]{16}) (?P<mod>[^:]+?): (?P<func>.*)$")
RE_MODULES = re.compile(r"^Modules \((\d+)\):\s*$")
RE_MODULE = re.compile(r"^(?P<lo>[0-9A-Fa-f]{8}|[0-9A-Fa-f]{16}) - (?P<hi>[0-9A-Fa-f]{8}|[0-9A-Fa-f]{16}) (?P<path>\S.*?)\s*$")

# ---------------------------------------------------------------- the mod

RE_ADDED_MOD = re.compile(r"^Added MOD directory <Mods[\\/]([^\\/>]+)[\\/]?> to CryPak")
RE_COOPBUILD = re.compile(r"^\[CoopBuild\] version=(\S+) built=([A-Za-z]{3} +\d{1,2} \d{4})_(\d\d:\d\d:\d\d) dll=(.*?)\s*$")
RE_COOP_TAG = re.compile(r"^\[Coop[A-Z][A-Za-z]*\] ")
RE_GAME_INIT = re.compile(r"^\[PlayerProfiles\] Login of user|^\[GameProfiles\]: Successfully|^\*LOADING: Level ")
RE_MOD_DLL = re.compile(r"[\\/]Mods[\\/]([^\\/]+)[\\/]Bin(?:32|64)[\\/]([^\\/]+)\.dll$", re.I)
RE_EDITOR = re.compile(r"^C1-Launcher Editor |^Executable: .*[\\/]Editor\.exe\s*$", re.I)

# ---------------------------------------------------------------- Windows' report

RE_WER_APP = re.compile(r"(?:Faulting application name|Имя сбойного приложения):\s*([^,;]+)")
RE_WER_MOD = re.compile(r"(?:Faulting module name|Имя сбойного модуля):\s*([^,;]+)")
RE_WER_CODE = re.compile(r"(?:Exception code|Код исключения):\s*(0x[0-9A-Fa-f]+)")
RE_WER_OFF = re.compile(r"(?:Fault offset|Смещение ошибки):\s*(0x[0-9A-Fa-f]+)")
# the module's PE time stamp: "Faulting module name: Coop.dll, version: 0.11.22.0, time stamp: 0x6aca28df"
RE_WER_MODSTAMP = re.compile(r"(?:Faulting module name|Имя сбойного модуля):\s*[^,;]+,\s*(?:version|версия):\s*[^,;]*,\s*(?:time stamp|метка времени):\s*(0x[0-9A-Fa-f]+)")
RE_STOPPED = re.compile(r"has stopped working|stopped working and was closed|Прекращена работа программы|перестала работать|\bApplication Error\b|\bAPPCRASH\b", re.I)
# the program's name, not a folder of a path ("C:\GOG Games\Crysis\Bin32")
RE_CRYSIS_APP = re.compile(r"\bCrysis\.exe\b|(?<![\\/\w])Crysis(?![\\/\w.])", re.I)

EXC_RU = {
    0xC0000005: "обращение по неверному адресу",
    0xC0000094: "деление на ноль",
    0xC00000FD: "переполнение стека",
    0xC0000409: "аварийное завершение",
    0xC000001D: "недопустимая инструкция",
    0xE06D7363: "необработанное исключение C++",
    0x80000003: "точка останова",
}

WHO_RU = {"host": "игра хоста", "companion": "игра компаньона"}


def _who(log):
    return WHO_RU.get(log.role, "игра")


def _hex(s):
    return int(s, 16)


# ---------------------------------------------------------------- MSVC map

class MapInfo:
    """Code symbols of an MSVC linker map ("Publics by Value" and "Static
    symbols"), by preferred VA."""

    RE_PREF = re.compile(r"^\s*Preferred load address is ([0-9A-Fa-f]+)")
    RE_STAMP = re.compile(r"^\s*Timestamp is ([0-9A-Fa-f]+) \((.*)\)")
    RE_SEG = re.compile(r"^\s*([0-9A-Fa-f]{4}):([0-9A-Fa-f]{8}) ([0-9A-Fa-f]{8})H\s+(\S+)\s+(CODE|DATA)\s*$")
    RE_SYM = re.compile(r"^\s*([0-9A-Fa-f]{4}):([0-9A-Fa-f]{8})\s+(\S+)\s+([0-9A-Fa-f]{8})\s+(?:f\s+)?(?:i\s+)?(\S.*?)?\s*$")

    def __init__(self, path):
        self.path = path
        self.pref = None
        self.linked = None      # local time of the link, seconds since the epoch
        self.stamp = None       # the PE time stamp, hex
        seg_len = {}            # section -> length of its segments
        code = set()            # sections with CODE segments
        syms = []
        base = {}
        in_syms = False
        with open(path, "r", encoding="latin-1") as f:
            for s in f:
                if not in_syms:
                    m = self.RE_PREF.match(s)
                    if m:
                        self.pref = _hex(m.group(1))
                        continue
                    m = self.RE_STAMP.match(s)
                    if m:
                        self.stamp = m.group(1).lower()
                        try:
                            self.linked = time.mktime(time.strptime(m.group(2).strip(), "%a %b %d %H:%M:%S %Y"))
                        except ValueError:
                            pass
                        continue
                    m = self.RE_SEG.match(s)
                    if m:
                        sec = _hex(m.group(1))
                        seg_len[sec] = max(seg_len.get(sec, 0), _hex(m.group(2)) + _hex(m.group(3)))
                        if m.group(5) == "CODE":
                            code.add(sec)
                        continue
                    if "Publics by Value" in s:
                        in_syms = True
                    continue
                m = self.RE_SYM.match(s)
                if not m:
                    continue
                sec = _hex(m.group(1))
                if sec not in seg_len:
                    continue
                off, va = _hex(m.group(2)), _hex(m.group(4))
                base.setdefault(sec, va - off)
                if sec in code:
                    syms.append((va, m.group(3), (m.group(5) or "").strip()))
        syms.sort()
        self.vas = [s[0] for s in syms]
        self.syms = syms
        self.code = [(base[s], base[s] + seg_len[s]) for s in code if s in base]
        # the image reaches at least this far (the .reloc section is not in the map)
        ends = [base[s] + n for s, n in seg_len.items() if s in base]
        self.extent = (max(ends) - self.pref) if ends and self.pref is not None else None

    def lookup(self, rva):
        """(name, displacement, object) of the function holding rva, or None."""
        if self.pref is None or not self.vas:
            return None
        va = self.pref + rva
        if not any(lo <= va < hi for lo, hi in self.code):
            return None
        i = bisect.bisect_right(self.vas, va) - 1
        if i < 0:
            return None
        sva, name, obj = self.syms[i]
        return name, va - sva, obj


_MAPS = {}


def load_map(path):
    """MapInfo of path (cached), or None when there is no usable map."""
    if not path or not os.path.isfile(path):
        return None
    try:
        st = os.stat(path)
        key = (os.path.abspath(path), st.st_mtime, st.st_size)
        if key not in _MAPS:
            _MAPS.clear()
            _MAPS[key] = MapInfo(path)
        m = _MAPS[key]
        return m if m.pref is not None and m.vas else None
    except OSError:
        return None


def pretty_symbol(name, obj=""):
    """A readable name for a decorated MSVC symbol, without a real demangler:
    ?Update@CCoopSave@@QAEXXZ -> CCoopSave::Update; an anonymous namespace
    takes the object's name (?UpdateContinue@?A0x1@@YAXXZ in CoopSave.cpp.obj
    -> CoopSave::UpdateContinue); _memcpy -> memcpy; others as they are."""
    if name.startswith("??0") or name.startswith("??1"):
        body = name[3:].split("@@", 1)[0]
        parts = body.split("@")
        if parts and all(p and not p.startswith("?") for p in parts):
            cls = parts[0]
            scope = "::".join(reversed(parts))
            return scope + "::" + ("~" if name[2] == "1" else "") + cls
        return name
    if name.startswith("?") and not name.startswith("??"):
        body = name[1:].split("@@", 1)[0]
        parts = body.split("@")
        if not parts or any(p.startswith("?$") or p.startswith("?") and not p.startswith("?A0x") for p in parts):
            return name
        func, scopes = parts[0], parts[1:]
        anon = any(p.startswith("?A0x") for p in scopes)
        scopes = [p for p in scopes if not p.startswith("?A0x")]
        if anon and not scopes and obj:
            stem = os.path.basename(obj.split(":")[-1])
            stem = re.sub(r"(\.cpp)?\.obj$", "", stem, flags=re.I)
            if stem:
                scopes = [stem]
        return "::".join(list(reversed(scopes)) + [func])
    if name.startswith("_") and not name.startswith("__"):
        return re.sub(r"@\d+$", "", name[1:])
    if name.startswith("@"):
        return re.sub(r"@\d+$", "", name[1:])
    return name


# ---------------------------------------------------------------- parsing

class Frame:
    __slots__ = ("line", "addr", "mod", "path", "off", "func", "sym", "fake")

    def __init__(self, line, addr, mod, func, fake=False):
        self.fake = fake        # made from the exception line (the block had no Callstack)
        self.line = line
        self.addr = addr
        self.mod = mod          # module name: from the Modules list, else the frame's own
        self.path = None        # full path when the Modules list has it
        self.off = None         # addr - module base
        self.func = func        # the launcher's function text ("?? ()" without symbols)
        self.sym = None         # (pretty, displacement, raw) from the map

    def resolved(self):
        return self.off is not None

    def known(self):
        """In a module: by the Modules list, or by the launcher's own name."""
        return self.mod is not None

    def place(self):
        if self.off is None:
            return "%s@0x%X" % (self.mod or "??", self.addr)
        return "%s+0x%X" % (self.mod, self.off)

    def named(self, names=True):
        """Coop.dll (CoopSave::UpdateContinue+0x41) / CryNetwork.dll+0x9D622.
        names=False leaves out a name that came from the map."""
        if self.sym and names:
            return "%s (%s+0x%X)" % (self.mod, self.sym[0], self.sym[1])
        f = self.func.strip()
        if f and not f.startswith("??"):
            return "%s (%s)" % (self.place(), f)
        return self.place()


class Crash:
    def __init__(self, log, banner):
        self.log = log
        self.banner = banner
        self.exc = None         # the exception line
        self.what = None
        self.code = None
        self.addr = None
        self.detail = None
        self.frames = []
        self.modules = []       # (lo, hi, path)
        self.modules_line = None
        self.cmdline = None
        self.end = None


def parse_crash(log, i):
    """The crash block whose banner is log.lines[i], and the index of the
    first line after it."""
    lines = log.lines
    c = Crash(log, lines[i])
    section = "head"
    junk = 0
    j = i + 1
    while j < len(lines):
        l = lines[j]
        s = l.text.strip()
        if RE_BANNER.match(s):
            break           # another crash block: the caller parses it
        j += 1
        if RE_END.match(s):
            c.end = l
            break
        if not s:
            continue
        if section == "head":
            if c.exc is None and not RE_LAUNCHER.match(s):
                m = RE_EXC.match(s)
                if m:
                    c.exc = l
                    c.what, c.code, c.addr, c.detail = m.group("what"), _hex(m.group("code")), _hex(m.group("addr")), m.group("detail")
                    continue
        if s == "Registers:":
            section = "regs"
            continue
        if s == "Callstack:":
            section = "stack"
            continue
        m = RE_MODULES.match(s)
        if m:
            section = "modules"
            c.modules_line = l
            continue
        if s == "Command line:":
            section = "cmd"
            continue
        if section == "stack":
            m = RE_FRAME.match(s)
            if m:
                c.frames.append(Frame(l, _hex(m.group("addr")), m.group("mod").strip(), m.group("func")))
                junk = 0
                continue
        elif section == "modules":
            m = RE_MODULE.match(s)
            if m:
                c.modules.append((_hex(m.group("lo")), _hex(m.group("hi")), m.group("path")))
                junk = 0
                continue
        elif section == "cmd":
            if c.cmdline is None:
                c.cmdline = l
            continue
        elif section in ("head", "regs"):
            continue
        junk += 1
        if junk > 40:       # the block was cut: the game went on writing
            break
    _resolve(c)
    return c, j


def _resolve(c):
    mods = sorted(c.modules)
    los = [m[0] for m in mods]
    for f in c.frames:
        k = bisect.bisect_right(los, f.addr) - 1
        if k >= 0 and mods[k][0] <= f.addr < mods[k][1]:
            lo, _, path = mods[k]
            f.path = path
            f.mod = re.split(r"[\\/]", path)[-1]
            f.off = f.addr - lo
        elif f.mod == "??" or not f.mod:
            f.mod = None


def is_mod_module(name_or_path):
    if not name_or_path:
        return False
    if RE_MOD_DLL.search(name_or_path):
        return True
    return os.path.basename(name_or_path.replace("\\", "/")).lower() in ("coop.dll", "cooptest.dll")


def _coopbuild(log):
    """(version, built time or None, dll path, line) of the first [CoopBuild] line."""
    for l in log.lines:
        if l.text.startswith("[CoopBuild] "):
            m = RE_COOPBUILD.match(l.text)
            if m:
                built = None
                try:
                    built = time.mktime(time.strptime(re.sub(" +", " ", m.group(2)) + " " + m.group(3), "%b %d %Y %H:%M:%S"))
                except ValueError:
                    pass
                return m.group(1), built, m.group(4), l
    return None


def _cut(s, n):
    return s if len(s) <= n else s[:n - 3] + "..."


def _when(t):
    return time.strftime("%Y-%m-%d %H:%M", time.localtime(t)) if t is not None else "?"


# [CoopBuild]'s built= is when CoopAgent.cpp was compiled; the map is written
# when the DLL is linked, a few seconds to minutes later in the same build. A
# longer gap: CoopAgent.cpp was not compiled again, so the map may be of a
# later build than the DLL that crashed.
SAME_BUILD_S = 900


def _symbolize(c, mapinfo, build, pe_stamp=None):
    """Names the mod's frames from the map. Returns (note for en, names_ok):
    names_ok is True only when the map is known to be of the DLL that
    crashed (names go to ru); when that is likely but not sure the names go to
    en only, marked; when the map cannot be matched to the DLL (no [CoopBuild]
    line, a map older than the DLL, a PE time stamp that differs) no frame is
    named: an older build's crash named from a newer map gets wrong names.
    pe_stamp: the crashed module's PE time stamp (Windows' report has it)."""
    mod_frames = [f for f in c.frames if f.resolved() and is_mod_module(f.path or f.mod)]
    if mapinfo is None or not mod_frames:
        return "", False
    mapname = os.path.basename(mapinfo.path)
    linked = _when(mapinfo.linked)
    exact = False
    if pe_stamp is not None and mapinfo.stamp:
        if int(mapinfo.stamp, 16) != pe_stamp:
            return "; %s (time stamp %s) is not of the DLL that crashed (time stamp %08x): frames not named" % (
                mapname, mapinfo.stamp, pe_stamp), False
        exact = True
    elif build is None:
        return "; frames not named: the log has no [CoopBuild] line, so %s cannot be matched to the DLL that crashed" % mapname, False
    if not exact and build[1] is not None and mapinfo.linked is not None and build[1] > mapinfo.linked + 300:
        return "; %s is older than the DLL in the log (map linked %s, DLL built %s): frames not named" % (
            mapname, linked, _when(build[1])), False
    sizes = [hi - lo for lo, hi, p in c.modules if is_mod_module(p)]
    if mapinfo.extent and sizes and max(sizes) < mapinfo.extent:
        return "; %s (linked %s) does not fit the DLL in the log (image 0x%X < 0x%X): frames not named" % (
            mapname, linked, max(sizes), mapinfo.extent), False
    for f in mod_frames:
        hit = mapinfo.lookup(f.off)
        if hit:
            f.sym = (_cut(pretty_symbol(hit[0], hit[2]), 100), hit[1], _cut(hit[0], 160))
    if exact:
        return "; names from %s (time stamp %s, the crashed DLL's)" % (mapname, mapinfo.stamp), True
    if build[1] is None or mapinfo.linked is None or mapinfo.linked - build[1] > SAME_BUILD_S:
        return "; names from %s linked %s, UNSURE: the DLL's [CoopBuild] time is %s, so the map may be of a later build" % (
            mapname, linked, _when(build[1])), False
    return "; names from %s linked %s (DLL %s built %s)" % (mapname, linked, build[0], _when(build[1])), True


def _exc_ru(code, what):
    if code in EXC_RU:
        return EXC_RU[code]
    return (what or "исключение").lower()


def _crash_finding(ctx, c, mapinfo, build):
    log = c.log
    if not c.frames and c.addr is not None:
        # no Callstack (a cut block): the faulting address is the top frame
        c.frames = [Frame(c.exc, c.addr, None, "", fake=True)]
        _resolve(c)
    note, names_ok = _symbolize(c, mapinfo, build)
    frames = c.frames
    top = frames[0] if frames else None
    place = next((f for f in frames if f.known()), None)
    # the game ran into an address outside every module (a call through a
    # null or stale pointer): neither the launcher nor the Modules list knows it
    bad_eip = top is not None and not top.fake and not top.known()
    modf = next((f for f in frames if f.known() and is_mod_module(f.path or f.mod)), None)

    # key: whose game, and module+offset of the top frame (of the first frame
    # in a module when the game jumped to a bad address). The host's and the
    # companion's crash at one place stay two findings: "the host's game
    # crashed (x2)" would hide that the companion's crashed.
    if place is not None:
        key = place.place().lower()
        if bad_eip:
            key = "badeip<" + key
    elif c.code is not None:
        key = "0x%08x@0x%x" % (c.code, c.addr or 0)
    else:
        key = "crash:unknown"
    key = "%s:%s" % (log.role, key)

    who = _who(log)
    ru = who + " упала"
    if place is not None:
        ru += " в " + place.named(names_ok)
    if bad_eip:
        ru += ": вызов по неверному адресу 0x%X" % top.addr
    elif c.code is not None:
        ru += ": " + _exc_ru(c.code, c.what)
    if modf is not None and modf is not place:
        ru += " (в стеке %s)" % modf.named(names_ok)
    # what the game was doing: quitting; a checkpoint load (the host's
    # Events); else a level load, a Continue, the start
    phase, phase_line, cause = _phase(log, c.banner)
    load = ctx.ev.load_at(c.banner.no) if log is ctx.host and phase != "quit" else None
    if load is not None:
        if load.end is None or c.banner.no <= load.end.no:
            ru += ", во время загрузки чекпойнта" + (" в запущенный уровень" if load.inplace else "")
        elif c.banner.no > load.start.no:
            ru += ", сразу после загрузки чекпойнта" + (" в запущенный уровень" if load.inplace else "")
        else:
            load = None
    if load is None and phase is not None:
        ru += ", " + PHASE_RU[phase]
        if cause and cause != phase:
            ru += " " + PHASE_RU[cause].split(" (")[0]
    else:
        phase = None
    if c.exc is None:
        ru += " (подробностей в логе нет)"

    # en: the facts, with numbers
    en = "%s crashed" % ({"host": "the host's game", "companion": "the companion's game"}.get(log.role, "the game"))
    if c.exc is not None:
        en += ": %s (0x%08X) at 0x%X" % (c.what, c.code, c.addr)
        if c.detail:
            en += " (%s)" % c.detail
    if place is not None:
        en += " in %s" % place.named()
        if bad_eip:
            en += " (bad EIP 0x%X, first frame in a module)" % top.addr
        if place.sym:
            en += " [%s]" % place.sym[2]
    if modf is not None and modf is not place:
        en += "; mod frame: %s" % modf.named() + (" [%s]" % modf.sym[2] if modf.sym else "")
    if frames and not frames[0].fake:
        en += "; stack: " + " < ".join(f.named() for f in frames[:8]) + (" ..." if len(frames) > 8 else "")
    if load is not None:
        en += "; %s the checkpoint load at line %d%s" % (
            "during" if load.end is None or c.banner.no <= load.end.no else "%d lines after" % (c.banner.no - load.end.no),
            load.start.no, " (in place)" if load.inplace else "")
    if phase is not None:
        en += "; " + PHASE_EN[phase] + (" (line %d)" % phase_line.no if phase_line is not None else "") + \
            (", " + PHASE_EN[cause] if cause and cause != phase else "")
    if not c.modules:
        en += "; no Modules list in the block"
    en += note

    # the exception, the frames, then the line that says what the game was
    # doing (the report shows the first three)
    if load is not None:
        context = load.start if load.end is None or c.banner.no <= load.end.no else load.end
    else:
        context = phase_line
    ev = []
    for l in (c.exc, top.line if top else None, place.line if place is not None and place is not top else None,
              modf.line if modf is not None else None, context, _before(log, c.banner), c.banner):
        if l is not None and l not in ev:
            ev.append(l)
    return Finding("D01", "CRASH", ru, en, ev, key=key)


RE_QUIT = re.compile(r"^System:Quit\b|^\[CoopTest\] cmdfile: quit\s*$")
RE_LEVEL_START = re.compile(r"^\[CoopAI\] level loading: |^\[Coop\] gamerules module loaded|^Game rules class: ")
RE_LEVEL_END = re.compile(r"^\*LOADING: Level ")
# lines a running game writes every few seconds (not while a level loads)
RE_INGAME = re.compile(r"^\[Coop(?:Pos|AIPos|Perf)\] ")
RE_CONTINUE = re.compile(r"^\[CoopSave\] continuing campaign ")
# a game joining another's: the AI companion ("[CoopAgent] joining the host:
# connect ...") or a player through the relay (CoopRelay.cpp)
RE_JOIN = re.compile(r"^\[CoopAgent\] joining the host: |^\[CoopRelay\] joining coop game ")
# all players down: the last checkpoint is loaded (CoopGameRules.lua), before
# the "[CoopSave] loading" line the host's Events start the load at
RE_ALLDOWN_LOAD = re.compile(r"^\[Coop\] everybody is down: the last checkpoint is loaded")

PHASE_RU = {
    "quit": "при выходе из игры",
    "start": "при запуске игры",
    "level": "во время загрузки уровня",
    "after_level": "сразу после загрузки уровня",
    "continue": "при продолжении кампании (загрузка сохранения)",
    "join": "при входе в игру хоста",
    "alldown": "при загрузке чекпойнта после гибели всех игроков",
}
PHASE_EN = {
    "quit": "while quitting",
    "start": "while starting up",
    "level": "while loading a level",
    "after_level": "right after a level loaded",
    "continue": "while continuing a campaign (loading its save)",
    "join": "while joining the host's game",
    "alldown": "while loading the last checkpoint after everybody was down",
}


def _phase(log, banner):
    """What the game was doing, from the lines before the crash block:
    (phase, line, cause) with phase one of PHASE_RU or None (playing, or not
    known), the line that tells it, and for a level load what started it
    ("continue": "[CoopSave] continuing campaign", "join": joining the host)
    or None. Nub-destroyed disconnects are not a quit: a Continue or a level
    load ends the old game the same way."""
    lines = log.lines
    k = banner.no - 2
    n = 0
    while k >= 0:
        l = lines[k]
        s = l.text.strip()
        if s and not set(s) <= set("-="):
            n += 1
        if n <= 60 and RE_QUIT.match(s):
            return "quit", l, None
        if n <= 60 and RE_ALLDOWN_LOAD.match(s):
            return "alldown", l, None
        if RE_INGAME.match(s):
            return None, None, None
        if RE_CONTINUE.match(s):
            return "continue", l, None
        if RE_JOIN.match(s):
            return "join", l, None
        if RE_LEVEL_END.match(s):
            if n > 25:
                return None, None, None
            return "after_level", l, _level_cause(lines, k)
        if RE_LEVEL_START.match(s):
            return "level", l, _level_cause(lines, k)
        if RE_GAME_INIT.match(s):
            return None, None, None
        k -= 1
    # the top of the log: only a whole log shows the game was starting
    if any(l.text.startswith("Log Started at") for l in lines[:40]):
        return "start", None, None
    return None, None, None


def _level_cause(lines, k):
    """"continue" or "join" when a Continue or a join started the level load
    whose line is lines[k], else None."""
    for l in reversed(lines[max(0, k - 400):k]):
        s = l.text
        if RE_CONTINUE.match(s):
            return "continue"
        if RE_JOIN.match(s):
            return "join"
        if RE_LEVEL_END.match(s) or RE_INGAME.match(s):
            return None
    return None


def _before(log, banner):
    """The last line the game wrote before the crash block (what it was doing)."""
    k = banner.no - 2
    while k >= 0:
        l = log.lines[k]
        s = l.text.strip()
        if s and not set(s) <= set("-="):
            return l
        k -= 1
    return None


# ---------------------------------------------------------------- Windows' report

def _wer_findings(ctx, log, mapinfo, build):
    out = []
    lines = ctx.lines(log)
    used = set()
    for idx, l in enumerate(lines):
        s = l.text
        if l.no in used:
            continue
        m = RE_WER_APP.search(s)
        if m:
            app = m.group(1).strip()
            if not app.lower().startswith("crysis"):
                continue
            # the rest may be on the same line ("; " joined) or on the next ones
            chunk = [l] + [x for x in lines[idx + 1: idx + 8]]
            mod = code = off = stamp = None
            ev = [l]
            for x in chunk:
                if mod is None:
                    mm = RE_WER_MOD.search(x.text)
                    if mm:
                        mod = mm.group(1).strip()
                        ms = RE_WER_MODSTAMP.search(x.text)
                        stamp = _hex(ms.group(1)) if ms else None
                        if x not in ev:
                            ev.append(x)
                if code is None:
                    mm = RE_WER_CODE.search(x.text)
                    if mm:
                        code = _hex(mm.group(1))
                if off is None:
                    mm = RE_WER_OFF.search(x.text)
                    if mm:
                        off = _hex(mm.group(1))
                        if x not in ev:
                            ev.append(x)
                used.add(x.no)
                if mod and code is not None and off is not None:
                    break
            note, names_ok = "", False
            f = None
            if mod and off is not None:
                f = Frame(l, off, mod, "")
                f.off = off
                c = Crash(log, l)
                c.frames = [f]
                note, names_ok = _symbolize(c, mapinfo, build, stamp)
                key = "%s:%s" % (log.role, f.place().lower())    # as a crash block's key
                where_ru, where_en = f.named(names_ok), f.named()
            else:
                key = "wer:" + app.lower()
                where_ru = where_en = mod or ""
            ru = "игра упала (отчёт Windows)" + (" в " + where_ru if where_ru else "")
            if code is not None:
                ru += ": " + _exc_ru(code, "исключение") + " (0x%08X)" % code
            en = "Windows reports a crash of %s" % app + (" in %s" % where_en if where_en else "") + \
                 (", exception 0x%08X" % code if code is not None else "") + \
                 (" [%s]" % f.sym[2] if f is not None and f.sym else "") + note
            out.append(Finding("D01", "CRASH", ru, en, ev, key=key))
            continue
        if RE_STOPPED.search(s) and RE_CRYSIS_APP.search(s):
            used.add(l.no)
            out.append(Finding("D01", "CRASH", "игра упала (сообщение Windows: \"%s\")" % s.strip()[:80],
                               "Windows says the game stopped: %s" % s.strip()[:200], [l],
                               key="wer:stopped"))
    return out


# ---------------------------------------------------------------- D12: the mod

def _mod_dir(log):
    for l in log.lines[:200]:
        m = RE_ADDED_MOD.match(l.text)
        if m:
            return m.group(1), l
    return None, None


def _mod_findings(ctx, log, crashes):
    lines = log.lines
    head = lines[:40]
    # only a whole log can show the mod is missing (a log cut at the front,
    # e.g. a tail of a running game, has no start-up lines)
    if not any(l.text.startswith("Log Started at") for l in head) or any(RE_EDITOR.match(l.text) for l in head):
        return []
    init = next((l for l in lines if RE_GAME_INIT.match(l.text)), None)
    mod, mod_line = _mod_dir(log)
    build = _coopbuild(log)
    proof = build[3] if build else next((l for l in lines if RE_COOP_TAG.match(l.text)), None)
    out = []
    who = _who(log)
    if init is not None and proof is None:
        ru = who + ": мод не загрузился, игра шла без Coop.dll" + ("" if mod else " (запущена без -mod)")
        en = "%s log: the game DLL started (line %d) but no line of the mod's DLL ([CoopBuild] or [Coop<Tag>]); mod folder: %s" % (
            log.role, init.no, mod or "none (no -mod)")
        out.append(Finding("D12", "INFRA", ru, en, [x for x in (init, mod_line) if x is not None], key="nomod"))
    elif build is not None and mod:
        m = RE_MOD_DLL.search(build[2])
        if m is None or m.group(1).lower() != mod.lower():
            ru = who + ": работала не та DLL мода (%s, а мод %s)" % (build[2], mod)
            en = "%s log: [CoopBuild] dll=%s is not Mods\\%s\\Bin32\\%s.dll (version %s)" % (log.role, build[2], mod, mod, build[0])
            out.append(Finding("D12", "INFRA", ru, en, [build[3]] + ([mod_line] if mod_line else []), key="wrongdll:" + build[2].lower()))
    # the Modules list of a crash that came after the game DLL started
    for c in crashes:
        if not c.modules or init is None or init.no > c.banner.no:
            continue
        dlls = [p for _, _, p in c.modules if RE_MOD_DLL.search(p)]
        if not dlls:
            if not any(f.key == "nomod" for f in out):
                out.append(Finding("D12", "INFRA", who + ": в списке модулей при падении нет Coop.dll (мод не загружен)",
                                   "%s log: the crash's Modules list (%d modules, line %d) has no Mods\\<mod>\\Bin32\\*.dll" % (
                                       log.role, len(c.modules), c.modules_line.no if c.modules_line else c.banner.no),
                                   [x for x in (c.modules_line, c.banner, mod_line) if x is not None], key="nomod"))
        elif mod:
            for p in dlls:
                m = RE_MOD_DLL.search(p)
                if m.group(1).lower() != mod.lower():
                    key = "wrongdll:" + p.lower()
                    if not any(f.key == key for f in out):
                        out.append(Finding("D12", "INFRA", who + ": загружена чужая DLL мода (%s, а мод %s)" % (p, mod),
                                           "%s log: the crash's Modules list has %s, the mod folder is %s" % (log.role, p, mod),
                                           [x for x in (c.modules_line, mod_line) if x is not None], key=key))
    return out


# ---------------------------------------------------------------- detectors

def _crashes(ctx, log):
    out = []
    lines = log.lines
    i = 0
    n = len(lines)
    while i < n:
        if "CRASH DETECTED" in lines[i].text and RE_BANNER.match(lines[i].text.strip()):
            c, j = parse_crash(log, i)
            out.append(c)
            i = max(j, i + 1)
        else:
            i += 1
    return out


def d01_crash(ctx):
    found = []
    mapinfo = load_map(ctx.map_path)
    for log in (ctx.host, ctx.companion):
        if log is None:
            continue
        build = _coopbuild(log)
        keys = {}
        for c in _crashes(ctx, log):
            if not ctx.in_window(c.banner):
                continue
            f = _crash_finding(ctx, c, mapinfo, build)
            keys[f.key] = f
            found.append(f)
        for f in _wer_findings(ctx, log, mapinfo, build):
            g = keys.get(f.key)
            if g is not None:   # the same crash seen by Windows: one finding
                g.evidence.extend(x for x in f.evidence if x not in g.evidence)
                continue
            found.append(f)
    return found


def d12_mod_loaded(ctx):
    found = []
    for log in (ctx.host, ctx.companion):
        if log is None:
            continue
        found.extend(_mod_findings(ctx, log, _crashes(ctx, log)))
    return found


DETECTORS = [d01_crash, d12_mod_loaded]
