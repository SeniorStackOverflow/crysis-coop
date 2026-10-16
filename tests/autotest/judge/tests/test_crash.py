"""D01 (crash blocks, Windows crash reports) and D12 (the mod not loaded).

Excerpts are real lines from goal_runs logs (adk_off_host.log, fin3_manual_host.log,
e1_wait_host.log, soundfile_host.log, quitold_result.txt, quitnew_host.log,
reload3_host.log, keepfriend5_client.log, us1_host.log, before_heli2_host.log),
an older session's runs (sc3_host.log), the game's LogBackups and at_host.log,
shortened. The "[CoopBuild]" lines follow at_host.log's (CoopAgent::Init).
"""

import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from coopjudge.context import Context                 # noqa: E402
from coopjudge.detectors import d01_crash as D        # noqa: E402
from coopjudge.logread import from_text               # noqa: E402

REAL_MAP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "..", "build", "Bin32", "Coop.map")

# adk_off_host.log: the host crashed in Coop.dll right after the level loaded
ADK_OFF = r"""[CoopAI] coop_sp_world=2: level loaded, server back to network mode
-----------------------------------------------------
*LOADING: Level multiplayer/tia/coop_rescue loading time: 29.28 seconds
-----------------------------------------------------
================================ CRASH DETECTED ================================
C1-Launcher Game v7 32-bit
Access violation (0xC0000005) at 0x6AD8A761: Read from 0x00000021 failed
Physical memory = 15693.6 MiB (2259.0 MiB available, 85.6% used)
Virtual memory = 4095.9 MiB (2959.4 MiB available, 27.7% used)
Commit charge = 802.9 MiB (808.9 MiB peak)
Working set = 807.2 MiB (812.5 MiB peak)
Registers:
EIP: 6AD8A761 ESP: 001AF2D0 EBP: 001AF318 EFLAGS: 00010206
EAX: 00000000 EBX: 00000000 ECX: 00000021 EDX: 366E5260
ESI: FFFFFFF3 EDI: 06994910
Callstack:
6AD8A761 Coop.dll: ?? ()
6AD9750A Coop.dll: ?? ()
6AD445BE Coop.dll: ?? ()
6ADB0566 Coop.dll: ?? ()
Modules (128):
00400000 - 00417000 C:\GOG Games\Crysis\Bin32\Crysis.exe
39000000 - 392EF000 C:\GOG Games\Crysis\Bin32\CryGame.dll
6AC60000 - 6ACDC000 C:\Windows\System32\Windows.Internal.Graphics.Display.DisplayColorManagement.dll
6ACF0000 - 6B0F4000 C:\GOG Games\Crysis\Mods\Coop\Bin32\Coop.dll
6B100000 - 6B274000 C:\WINDOWS\SYSTEM32\inputhost.dll
Command line:
"C:\GOG Games\Crysis\Bin32\Crysis.exe" -mod Coop -devmode -logfile host.log +coop_continue checking
================================================================================
"""

# fin3_manual_host.log: a call through a null pointer, Coop.dll below it, during a load
FIN3 = r"""[CoopSave] loading coop_checkpoint_56c79107e0d7a6ff_b.CRYSISJMSF (rescue, checkpoint manual)
[CoopTest] before LoadGame: 30 soldiers not armed
[CoopSave] the load makes objects/natural/trees/jungle_tree_thin/jungle_tree_thin_a.cgf (Default) again with the id 63399 it had: that slot is kept for a thing of the save now, the piece gets the id 705822719
================================ CRASH DETECTED ================================
C1-Launcher Game v7 32-bit
Access violation (0xC0000005) at 0x00000000: Read from 0x00000000 failed
Registers:
EIP: 00000000 ESP: 001ADAF8 EBP: 001ADB90 EFLAGS: 00010246
Callstack:
00000000 ??: ?? ()
3203467E CryEntitySystem.dll: ?? ()
6B796ABC Coop.dll: ?? ()
32033440 CryEntitySystem.dll: ?? ()
Modules (131):
00400000 - 00417000 C:\GOG Games\Crysis\Bin32\Crysis.exe
32000000 - 320E1000 C:\GOG Games\Crysis\Bin32\CryEntitySystem.dll
6B760000 - 6BB50000 C:\GOG Games\Crysis\Mods\Coop\Bin32\Coop.dll
Command line:
"C:\GOG Games\Crysis\Bin32\Crysis.exe" -mod Coop -devmode
================================================================================
"""

# e1_wait_host.log: another thread's lines inside the block (with time stamps)
E1_WAIT = r"""<04:42:35> [brk] still active for 118 is ProceduralBreaking.Recording.JointBreak @ 4C77AAA0
================================ CRASH DETECTED ================================
C1-Launcher Game v7 32-bit
Access violation (0xC0000005) at 0x3650B69C: Read from 0x00000008 failed
Registers:
EIP: 3650B69C ESP: 0491FF08 EBP: 30924B78 EFLAGS: 00010246
<04:42:35> [brk] end frame
<04:42:35> [brk] still active for 83 is ProceduralBreaking.Recording.PlaneBreak @ 4BE480C0
Callstack:
3650B69C CrySystem.dll: ?? ()
36578240 CrySystem.dll: ?? ()
77725D49 KERNEL32.DLL: ?? ()
Modules (141):
36500000 - 36727000 C:\GOG Games\Crysis\Bin32\CrySystem.dll
6ACF0000 - 6B0F4000 C:\GOG Games\Crysis\Mods\Coop\Bin32\Coop.dll
77710000 - 77800000 C:\WINDOWS\System32\KERNEL32.DLL
Command line:
"C:\GOG Games\Crysis\Bin32\Crysis.exe" -mod Coop -devmode
================================================================================
"""

# soundfile_host.log (another session's goal_runs): no ": detail" after the address
DIV0 = r"""[CoopTest] coop_test_sound nanpos: paused 0, 8 voices, output peak -1.000
================================ CRASH DETECTED ================================
C1-Launcher Game v7 32-bit
Integer divide by zero (0xC0000094) at 0x02BE4DEF
Registers:
EIP: 02BE4DEF ESP: 001ADDD4 EBP: 001AE444 EFLAGS: 00210297
Callstack:
02BE4DEF fmodex.dll: ?? ()
Modules (149):
00400000 - 00417000 C:\GOG Games\Crysis\Bin32\Crysis.exe
02BE0000 - 02D82000 C:\GOG Games\Crysis\Bin32\fmodex.dll
6DC20000 - 6DFF1000 C:\GOG Games\Crysis\Mods\Coop\Bin32\Coop.dll
Command line:
"C:\GOG Games\Crysis\Bin32\Crysis.exe" -mod Coop -devmode
================================================================================
"""

# lines that mention crashes but are not one
NOT_A_CRASH = r"""[CoopSave] a checkpoint of a campaign with 2 players: the AI starts afresh (it crashed otherwise)
[Coop] HIT AB1_Crash_Down_VTOL1_Gunner by Nomad: no damage (invulnerable, as in single player)
round 2 places: TIMEOUT (checkpoint loaded: places kept now|CRASH DETECTED)
crash events since the start: 0
[CoopObjState] active: Island.Objective_06
[CoopTest] cmdfile: quit
"""

# quitnew_host.log's start: a normal game with the mod
HEAD_MOD = r"""BackupNameAttachment=" Build(6156) Date(12 Oct 2026) Time(16 32 46)"  -- used by backup system

Log Started at Monday, October 12, 2026 16:32:46
Executable: C:\GOG Games\Crysis\Bin32\Crysis.exe
Added MOD directory <Mods\Coop\> to CryPak
C1-Launcher Game v7 32-bit
Initializing AI System
[CoopText] 123 texts and 56 patterns for Russian (Languages/coop_text_russian.txt)
[CoopMenu] localization reloaded after the mod's texts: 10 tables, 10097 texts (10097 before)
[PlayerProfiles] Login of user 'andre' successful.
[GameProfiles]: Successfully activated profile 'default' for user 'andre'
"""

# LogBackups "Game Build(6156) Date(01 Oct 2026) Time(21 28 51).log": the game without -mod
HEAD_VANILLA = r"""BackupNameAttachment=" Build(6156) Date(01 Oct 2026) Time(21 28 51)"  -- used by backup system

Log Started at Thursday, October 01, 2026 21:28:51
Executable: C:\GOG Games\Crysis\Bin32\Crysis.exe
C1-Launcher Game v7 32-bit
Initializing AI System
[PlayerProfiles] Login of user 'andre' successful.
[GameProfiles]: Successfully activated profile 'default' for user 'andre'
*LOADING: Level mine loading time: 21.53 seconds
"""

# quitold_result.txt: Windows' event 1000 as the old harness copied it
WER_RU = r"""host ended by quit: True
crash events since the start: 1
Имя сбойного приложения: Crysis.exe, версия: 7.0.0.0, метка времени: 0x682078c3 ; Имя сбойного модуля: Coop.dll, версия: 0.11.22.0, метка времени: 0x6aca28df ; Код исключения: 0xc0000409 ; Смещение ошибки: 0x002bd38e ; Идентификатор сбойного процесса: 0xCE8
System:Quit
"""

# the same event on an English Windows (one field per line)
WER_EN = r"""Faulting application name: Crysis.exe, version: 7.0.0.0, time stamp: 0x682078c3
Faulting module name: Coop.dll, version: 0.11.22.0, time stamp: 0x6aca28df
Exception code: 0xc0000409
Fault offset: 0x002bd38e
Faulting process id: 0xce8
"""

# a tiny MSVC map in the linker's format (lines from build\Bin32\Coop.map,
# one symbol moved so that Coop.dll+0x9A761 falls 0x41 into it)
MAP = """ Coop

 Timestamp is 6ad0ef44 (Thu Oct 15 18:20:36 2026)

 Preferred load address is 10000000

 Start         Length     Name                   Class
 0001:00000000 00005c70H .text$di                CODE
 0001:00005c70 002e2860H .text$mn                CODE
 0002:00000000 00000360H .idata$5                DATA

  Address         Publics by Value              Rva+Base       Lib:Object

 0000:00000000       ___AbsoluteZero            00000000     <absolute>
 0001:00005c70       ??$GetParam@M@CProjectile@@IAEMPBDAAM@Z 10006c70 f i AVMine.cpp.obj
 0001:00099500       ?Update@CCoopSave@@QAEXXZ  1009a500 f   CoopSave.cpp.obj
 0002:00000000       __imp__CryptHashData@16    10306000     advapi32:ADVAPI32.dll

 entry point at        0001:002bf379

 Static symbols

 0001:00099720       ?UpdateContinue@?A0xc47c6e26@@YAXXZ 1009a720 f   CoopSave.cpp.obj
 0001:000a0000       _memcpy_helper             100a1000 f   LIBCMT:memcpy.obj
"""

# at_host.log's line, with the times moved next to MAP's link time (18:20:36)
BUILD_OK = "[CoopBuild] version=0.11.35.0 built=Oct 15 2026_18:20:30 dll=C:\\GOG Games\\Crysis\\Mods\\Coop\\Bin32\\Coop.dll\n"
BUILD_NEWER = "[CoopBuild] version=0.11.35.0 built=Oct 16 2026_09:00:00 dll=C:\\GOG Games\\Crysis\\Mods\\Coop\\Bin32\\Coop.dll\n"
BUILD_OLD = "[CoopBuild] version=0.11.35.0 built=Oct 15 2026_12:56:02 dll=C:\\GOG Games\\Crysis\\Mods\\Coop\\Bin32\\Coop.dll\n"
# at_host.log as it is
BUILD_AT_HOST = "[CoopBuild] version=0.11.35.0 built=Oct 15 2026_18:38:44 dll=C:\\GOG Games\\Crysis\\Mods\\CoopTest\\Bin32\\CoopTest.dll\n"

# Windows' report of a Coop.dll crash whose module time stamp is MAP's (6ad0ef44) or not
WER_STAMP = """Faulting application name: Crysis.exe, version: 7.0.0.0, time stamp: 0x682078c3
Faulting module name: Coop.dll, version: 0.11.35.0, time stamp: 0x%s
Exception code: 0xc0000005
Fault offset: 0x0009a761
"""

# reload3_host.log (another session's goal_runs): coop_load, the old game's
# network ends (Nub destroyed, not a quit), the host crashes in Coop.dll
RELOAD3 = r"""[CoopObjState] active: Rescue.Objective_03
[CoopTest] cmdfile: coop_load
[CoopSave] continuing campaign "Reload Test": rescue, checkpoint manual (saved 2026-10-08 13:39:02)
    $3sv_gamerules = $6TeamInstantAction $5[]$4
[net 10:39:09.437] Disconnect <local>:16; profid=0; cause=13; msg='Nub destroyed'
[net 10:39:09.444] Disconnect <local>:64087; profid=0; cause=13; msg='Nub destroyed'
================================ CRASH DETECTED ================================
C1-Launcher Game v7 32-bit
Access violation (0xC0000005) at 0x68FDC46B: Read from 0x6D782E36 failed
Callstack:
68FDC46B Coop.dll: ?? ()
Modules (136):
68DC0000 - 691EF000 C:\GOG Games\Crysis\Mods\Coop\Bin32\Coop.dll
================================================================================
"""

# keepfriend5_client.log: the companion crashed loading the level of the host it joined
KEEPFRIEND5 = r"""[CoopRelay] joining coop game 944702 through wss://crysis.46-225-103-75.sslip.io/ ...
[CoopRelay] tunnel to the host open, connect 127.0.0.1 54253
[net 19:41:47.415] connection requested to: 127.0.0.1:54253
Game rules class: TeamInstantAction
[Coop] gamerules module loaded, version 0.1.0
[CoopAI] coop_sp_world=1: client loads this level in single-player mode
================================ CRASH DETECTED ================================
C1-Launcher Game v7 32-bit
Access violation (0xC0000005) at 0x38133EA4: Read from 0x00000000 failed
Callstack:
38133EA4 CryRenderD3D9.dll: ?? ()
Modules (120):
38000000 - 388EA000 C:\GOG Games\Crysis\Bin32\CryRenderD3D9.dll
================================================================================
"""

# before_heli2_host.log: crashed making the renderer, before the game DLL
HELI2 = r"""BackupNameAttachment=" Build(6156) Date(13 Oct 2026) Time(19 00 45)"  -- used by backup system

Log Started at Tuesday, October 13, 2026 19:00:45
Executable: C:\GOG Games\Crysis\Bin32\Crysis.exe
Physics initialization
MovieSystem initialization
Renderer initialization
================================ CRASH DETECTED ================================
C1-Launcher Game v7 32-bit
Access violation (0xC0000005) at 0x3809A761: Read from 0x00000000 failed
Callstack:
3809A761 CryRenderD3D9.dll: ?? ()
Modules (99):
38000000 - 388EA000 C:\GOG Games\Crysis\Bin32\CryRenderD3D9.dll
================================================================================
"""

# sc3_host.log (an older session's runs): all players down, the checkpoint load starts
SC3 = r"""<15:05:07> [CoopPos] server: Nomad=(1363,1974,229)hp0
<15:05:07> [CoopObjState] active: Rescue.Objective_04
<15:05:09> [Coop] everybody is down: the last checkpoint is loaded
================================ CRASH DETECTED ================================
C1-Launcher Game v7 32-bit
Access violation (0xC0000005) at 0x6A4A64C6: Read from 0xFFFFFFF4 failed
Callstack:
6A4A64C6 Coop.dll: ?? ()
Modules (137):
6A440000 - 6A86F000 C:\GOG Games\Crysis\Mods\Coop\Bin32\Coop.dll
================================================================================
"""

# us1_host.log: playing (the periodic [CoopPos]/[CoopAIPos] lines) when it crashed
US1 = r"""[CoopAIPos] server: Helicamp_Otherside_4=(1394,2012,226)hp180 Helicamp_Otherside_2=(1395,2017,226)hp180
[CoopAI] AI shots so far: 0 (wake mode 1)
[CoopObjState] active: Rescue.Objective_04
================================ CRASH DETECTED ================================
C1-Launcher Game v7 32-bit
Access violation (0xC0000005) at 0x3959D622: Read from 0x00000000 failed
Callstack:
3959D622 CryNetwork.dll: ?? ()
Modules (130):
39500000 - 39613000 C:\GOG Games\Crysis\Bin32\CryNetwork.dll
================================================================================
"""

# quitnew_host.log's end, and a crash block after it
QUIT = r"""[CoopTest] cmdfile: quit
System:Quit
================================ CRASH DETECTED ================================
C1-Launcher Game v7 32-bit
Access violation (0xC0000005) at 0x3959D622: Read from 0x00000000 failed
Callstack:
3959D622 CryNetwork.dll: ?? ()
Modules (130):
39500000 - 39613000 C:\GOG Games\Crysis\Bin32\CryNetwork.dll
================================================================================
"""


def ctx_of(host_text, companion_text=None, map_path=None, name="x_host.log"):
    host = from_text(host_text, name)
    comp = from_text(companion_text, "x_companion.log") if companion_text is not None else None
    return Context(host, comp, None, map_path)


def crashes(ctx):
    return [f for f in D.d01_crash(ctx) if f.id == "D01"]


class CrashBlock(unittest.TestCase):
    def test_coop_dll_top_frame(self):
        fs = crashes(ctx_of(ADK_OFF))
        self.assertEqual(len(fs), 1)
        f = fs[0]
        self.assertEqual(f.verdict, "CRASH")
        self.assertEqual(f.key, "host:coop.dll+0x9a761")
        self.assertIn("игра хоста упала в Coop.dll+0x9A761", f.ru)
        self.assertIn("обращение по неверному адресу", f.ru)
        self.assertIn("Read from 0x00000021 failed", f.en)
        self.assertIn("Coop.dll+0x9A761 < Coop.dll+0xA750A", f.en)
        self.assertTrue(f.evidence[0].text.startswith("Access violation"))
        self.assertTrue(any(l.text.startswith("*LOADING: Level") for l in f.evidence))

    def test_bad_eip_with_coop_below_during_load(self):
        f = crashes(ctx_of(FIN3))[0]
        self.assertEqual(f.key, "host:badeip<cryentitysystem.dll+0x3467e")
        self.assertIn("в CryEntitySystem.dll+0x3467E", f.ru)
        self.assertIn("вызов по неверному адресу 0x0", f.ru)
        self.assertIn("в стеке Coop.dll+0x36ABC", f.ru)
        self.assertIn("во время загрузки чекпойнта", f.ru)

    def test_other_threads_lines_inside_the_block(self):
        f = crashes(ctx_of(E1_WAIT))[0]
        self.assertEqual(f.key, "host:crysystem.dll+0xb69c")
        # an excerpt without the log's start: not "while starting up"
        self.assertNotIn("при запуске", f.ru)
        self.assertIn("CrySystem.dll+0x78240", f.en)
        self.assertIn("KERNEL32.DLL+0x15D49", f.en)

    def test_divide_by_zero_in_companion(self):
        ctx = ctx_of(HEAD_MOD, DIV0)
        fs = crashes(ctx)
        self.assertEqual(len(fs), 1)
        f = fs[0]
        self.assertEqual(f.key, "companion:fmodex.dll+0x4def")
        self.assertIn("игра компаньона упала в fmodex.dll+0x4DEF: деление на ноль", f.ru)
        self.assertEqual(f.evidence[0].src.name, "x_companion.log")

    def test_cut_block(self):
        f = crashes(ctx_of(ADK_OFF.split("Registers:")[0]))[0]
        self.assertEqual(f.verdict, "CRASH")
        self.assertIn("no Modules list", f.en)
        cut = crashes(ctx_of("[CoopTest] cmdfile: quit\n================================ CRASH DETECTED ================================\n"))[0]
        self.assertIn("подробностей в логе нет", cut.ru)

    def test_two_blocks(self):
        fs = crashes(ctx_of(ADK_OFF + E1_WAIT))
        self.assertEqual(sorted(f.key for f in fs), ["host:coop.dll+0x9a761", "host:crysystem.dll+0xb69c"])

    def test_host_and_companion_at_one_place_stay_two(self):
        fs = crashes(ctx_of(DIV0, DIV0))
        self.assertEqual(sorted(f.key for f in fs), ["companion:fmodex.dll+0x4def", "host:fmodex.dll+0x4def"])

    def test_phase_continue_after_level_load(self):
        # adk_off: Continue, the level loads, the crash
        text = '[CoopSave] continuing campaign "checking": rescue, checkpoint manual (saved 2026-10-14 12:36:40)\n' + \
               "[Coop] gamerules module loaded, version 0.1.0\n[CoopAI] level loading: enabling AI system on the server\n" + ADK_OFF
        f = crashes(ctx_of(text))[0]
        self.assertIn("сразу после загрузки уровня при продолжении кампании", f.ru)
        self.assertTrue(any(l.text.startswith("*LOADING: Level") for l in f.evidence))

    def test_phase_continue_is_not_a_quit(self):
        f = crashes(ctx_of(RELOAD3))[0]
        self.assertIn("при продолжении кампании", f.ru)
        self.assertNotIn("выход", f.ru)

    def test_phase_companion_joining(self):
        f = crashes(ctx_of(HEAD_MOD, KEEPFRIEND5))[0]
        self.assertIn("игра компаньона упала в CryRenderD3D9.dll+0x133EA4", f.ru)
        self.assertIn("во время загрузки уровня при входе в игру хоста", f.ru)

    def test_phase_start(self):
        f = crashes(ctx_of(HELI2))[0]
        self.assertIn("при запуске игры", f.ru)

    def test_phase_all_down(self):
        f = crashes(ctx_of(SC3))[0]
        self.assertIn("при загрузке чекпойнта после гибели всех игроков", f.ru)

    def test_phase_quit(self):
        f = crashes(ctx_of(QUIT))[0]
        self.assertIn("при выходе из игры", f.ru)

    def test_phase_playing_says_nothing(self):
        f = crashes(ctx_of(US1))[0]
        self.assertEqual(f.ru, "игра хоста упала в CryNetwork.dll+0x9D622: обращение по неверному адресу")

    def test_not_a_crash(self):
        # crash words, a test result's regex, and a log that just ends (tests kill games)
        self.assertEqual(D.d01_crash(ctx_of(NOT_A_CRASH)), [])
        self.assertEqual(D.d01_crash(ctx_of(HEAD_MOD + NOT_A_CRASH)), [])


class Symbols(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.map = os.path.join(self.dir, "Coop.map")
        with open(self.map, "w", encoding="latin-1") as f:
            f.write(MAP)

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def test_named_when_the_build_fits(self):
        f = crashes(ctx_of(BUILD_OK + ADK_OFF, map_path=self.map))[0]
        self.assertIn("Coop.dll (CoopSave::UpdateContinue+0x41)", f.ru)
        self.assertIn("?UpdateContinue@?A0xc47c6e26@@YAXXZ", f.en)
        self.assertEqual(f.key, "host:coop.dll+0x9a761")

    def test_no_coopbuild_no_names(self):
        # a log of an older DLL (before [CoopBuild]) named from today's map
        # gets wrong names: none at all
        f = crashes(ctx_of(ADK_OFF, map_path=self.map))[0]
        self.assertIn("Coop.dll+0x9A761", f.ru)
        self.assertNotIn("UpdateContinue", f.ru + f.en)
        self.assertIn("no [CoopBuild] line", f.en)

    def test_coopbuild_long_before_the_link_names_en_only(self):
        # CoopAgent.cpp compiled hours before the map's link: the map may be
        # of a later build than the DLL that crashed
        f = crashes(ctx_of(BUILD_OLD + ADK_OFF, map_path=self.map))[0]
        self.assertNotIn("UpdateContinue", f.ru)
        self.assertIn("CoopSave::UpdateContinue+0x41", f.en)
        self.assertIn("UNSURE", f.en)

    def test_map_older_than_the_dll(self):
        f = crashes(ctx_of(BUILD_NEWER + ADK_OFF, map_path=self.map))[0]
        self.assertNotIn("UpdateContinue", f.ru + f.en)
        self.assertIn("older than the DLL", f.en)

    def test_lookup_and_pretty(self):
        m = D.load_map(self.map)
        self.assertEqual(m.lookup(0x9A761)[:2], ("?UpdateContinue@?A0xc47c6e26@@YAXXZ", 0x41))
        self.assertEqual(m.lookup(0x9A510)[:2], ("?Update@CCoopSave@@QAEXXZ", 0x10))
        self.assertIsNone(m.lookup(0x306010))          # data, not code
        self.assertEqual(D.pretty_symbol("?Update@CCoopSave@@QAEXXZ"), "CCoopSave::Update")
        self.assertEqual(D.pretty_symbol("?UpdateContinue@?A0xc47c6e26@@YAXXZ", "CoopSave.cpp.obj"), "CoopSave::UpdateContinue")
        self.assertEqual(D.pretty_symbol("_memcpy_helper"), "memcpy_helper")
        self.assertEqual(D.pretty_symbol("??$GetParam@M@CProjectile@@IAEMPBDAAM@Z"), "??$GetParam@M@CProjectile@@IAEMPBDAAM@Z")

    def test_windows_report_time_stamp_matches_the_map(self):
        f = crashes(ctx_of(WER_STAMP % "6ad0ef44", map_path=self.map))[0]
        self.assertIn("CoopSave::UpdateContinue+0x41", f.ru)
        other = crashes(ctx_of(WER_STAMP % "6aca28df", map_path=self.map))[0]
        self.assertNotIn("UpdateContinue", other.ru + other.en)
        self.assertIn("not of the DLL that crashed", other.en)

    def test_no_map_no_names(self):
        f = crashes(ctx_of(BUILD_OK + ADK_OFF, map_path=os.path.join(self.dir, "missing.map")))[0]
        self.assertNotIn("UpdateContinue", f.ru + f.en)

    @unittest.skipUnless(os.path.isfile(REAL_MAP), "no build\\Bin32\\Coop.map")
    def test_real_map_reads(self):
        m = D.load_map(REAL_MAP)
        self.assertEqual(m.pref, 0x10000000)
        # every code symbol finds itself
        va, name, obj = m.syms[len(m.syms) // 2]
        self.assertEqual(m.lookup(va - m.pref)[1], 0)


class WindowsReport(unittest.TestCase):
    def test_event_ru(self):
        fs = crashes(ctx_of(WER_RU, name="quitold_result.txt"))
        self.assertEqual(len(fs), 1)
        f = fs[0]
        self.assertEqual(f.key, "host:coop.dll+0x2bd38e")
        self.assertIn("Coop.dll+0x2BD38E", f.ru)
        self.assertIn("0xC0000409", f.ru)

    def test_event_en_lines(self):
        f = crashes(ctx_of(WER_EN))[0]
        self.assertEqual(f.key, "host:coop.dll+0x2bd38e")
        self.assertIn("exception 0xC0000409", f.en)

    def test_stopped_working_names_the_program_not_a_folder(self):
        self.assertEqual(len(crashes(ctx_of("Crysis has stopped working\n"))), 1)
        self.assertEqual(len(crashes(ctx_of("Application Error: Crysis.exe, Coop.dll\n"))), 1)
        # a path with the game's folder in it is not the program
        text = "[Warning] Sound - the device has stopped working (C:\\GOG Games\\Crysis\\Bin32\\fmodex.dll)\n"
        self.assertEqual(crashes(ctx_of(text)), [])

    def test_other_programs_and_counts(self):
        text = ("crash events since the start: 0\n"
                "Faulting application name: explorer.exe, version: 10.0.26100.1, time stamp: 0x12345678\n"
                "Faulting module name: ntdll.dll, version: 10.0.26100.1\n")
        self.assertEqual(crashes(ctx_of(text)), [])


class ModLoaded(unittest.TestCase):
    def test_vanilla_game(self):
        fs = D.d12_mod_loaded(ctx_of(HEAD_VANILLA))
        self.assertEqual(len(fs), 1)
        self.assertEqual((fs[0].id, fs[0].verdict, fs[0].key), ("D12", "INFRA", "nomod"))
        self.assertIn("без -mod", fs[0].ru)

    def test_mod_folder_but_no_dll_lines(self):
        # LogBackups 22 Sep: -mod Coop, but nothing from Coop.dll
        text = HEAD_VANILLA.replace("C1-Launcher Game v7 32-bit", "Added MOD directory <Mods\\Coop\\> to CryPak\nC1-Launcher Game v7 32-bit")
        fs = D.d12_mod_loaded(ctx_of(text))
        self.assertEqual(len(fs), 1)
        self.assertNotIn("без -mod", fs[0].ru)

    def test_wrong_dll(self):
        text = HEAD_MOD.replace("Mods\\Coop\\", "Mods\\CoopTest\\") + BUILD_OK
        fs = D.d12_mod_loaded(ctx_of(text))
        self.assertEqual([f.key for f in fs], ["wrongdll:" + BUILD_OK.split("dll=")[1].strip().lower()])

    def test_crash_modules_with_another_mods_dll(self):
        text = HEAD_MOD.replace("Mods\\Coop\\", "Mods\\CoopTest\\") + ADK_OFF
        fs = D.d12_mod_loaded(ctx_of(text))
        self.assertEqual(len(fs), 1)
        self.assertIn("mods\\coop\\bin32\\coop.dll", fs[0].key)

    def test_crash_modules_without_the_mod(self):
        text = HEAD_MOD + ADK_OFF.replace("6ACF0000 - 6B0F4000 C:\\GOG Games\\Crysis\\Mods\\Coop\\Bin32\\Coop.dll\n", "")
        self.assertNotIn("Coop.dll\n", text)
        fs = D.d12_mod_loaded(ctx_of(text))
        self.assertEqual([(f.key, f.verdict) for f in fs], [("nomod", "INFRA")])
        self.assertTrue(fs[0].evidence[0].text.startswith("Modules ("))

    def test_mod_loaded(self):
        self.assertEqual(D.d12_mod_loaded(ctx_of(HEAD_MOD + ADK_OFF)), [])
        self.assertEqual(D.d12_mod_loaded(ctx_of(HEAD_MOD.replace("Mods\\Coop\\", "Mods\\CoopTest\\") + BUILD_AT_HOST)), [])

    def test_crash_before_the_game_dll(self):
        # before_heli2_host.log: crashed at "Renderer initialization", no Coop.dll yet
        text = HEAD_MOD.split("Initializing AI System")[0] + "Renderer initialization\n" + DIV0.replace(
            "6DC20000 - 6DFF1000 C:\\GOG Games\\Crysis\\Mods\\Coop\\Bin32\\Coop.dll\n", "")
        self.assertEqual(D.d12_mod_loaded(ctx_of(text)), [])
        self.assertEqual(len(crashes(ctx_of(text))), 1)

    def test_log_without_its_start(self):
        # a tail of a log: a level load but no start-up lines
        self.assertEqual(D.d12_mod_loaded(ctx_of("*LOADING: Level multiplayer/tia/coop_rescue loading time: 29.28 seconds\n")), [])


if __name__ == "__main__":
    unittest.main()
