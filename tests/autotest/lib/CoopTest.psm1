# CoopTest: runs the Crysis Coop mod's games for the autotester and reads what
# they write. The rules it keeps (the person at the computer set them):
#  - a game only in a window, left of all screens, never with the focus or
#    the mouse (G1, G2)
#  - sound never to the speakers: none, or into a WAV file (G3)
#  - the person's saves and installed mod are never written: the tests run
#    from Mods\CoopTest with copies of saves (G4, G7)
#  - no test while the person plays, and one test at a time (G5, G6)
Set-StrictMode -Version 1
$ErrorActionPreference = "Stop"

# <game>\<checkout>\tests\autotest\lib: the checkout this runner is in is the one tested
$script:Repo = Split-Path (Split-Path (Split-Path $PSScriptRoot))
$script:GameRoot = Split-Path $script:Repo
$script:TestMod = "CoopTest"
$script:TestModDir = Join-Path $script:GameRoot "Mods\$($script:TestMod)"
$script:UserModDir = Join-Path $script:GameRoot "Mods\Coop"
# not under LOCALAPPDATA: a packaged app (the Claude desktop app) has its
# AppData redirected, and the games got profile paths of 250+ characters (the
# engine overflowed a path buffer and crashed at its renderer's start)
$script:TestHome = Join-Path $env:USERPROFILE "CrysisCoopTest"
$script:UserSaves = Join-Path ([Environment]::GetFolderPath("MyDocuments")) "My Games\Crysis\SaveGames"

if (-not ("CoopTestNative" -as [type])) { Add-Type -Path (Join-Path $PSScriptRoot "Native.cs") }

function Get-CoopTestPaths {
    [pscustomobject]@{ GameRoot = $script:GameRoot; Repo = $script:Repo; TestModDir = $script:TestModDir; UserModDir = $script:UserModDir; TestHome = $script:TestHome; UserSaves = $script:UserSaves }
}

# ---- guard -----------------------------------------------------------------

# Crysis processes that are not the runner's: the person's game (or another
# test). CrysisCoop.exe -coop_mcp is the AI agents' bridge, not a game.
function Get-ForeignGames {
    $own = @([CoopTestNative]::JobPids())
    $procs = @(Get-Process -Name Crysis, CrysisCoop, CrysisLauncher -ErrorAction SilentlyContinue | Where-Object { $own -notcontains $_.Id })
    if (-not $procs.Count) { return @() }
    $out = @()
    foreach ($p in $procs) {
        if ([CoopTestNative]::InJob($p.Id)) { continue }
        if ($p.ProcessName -eq "CrysisCoop") {
            $cl = (Get-CimInstance Win32_Process -Filter "ProcessId=$($p.Id)" -ErrorAction SilentlyContinue).CommandLine
            if ($cl -and $cl -match '-coop_mcp') { continue }
        }
        $out += $p
    }
    return $out
}

$script:Mutex = $null
function Enter-CoopTestLock {
    $created = $false
    $script:Mutex = New-Object System.Threading.Mutex($false, "Global\CrysisCoopAutotest", [ref]$created)
    if (-not $script:Mutex.WaitOne(0)) { throw "LOCKED: another autotest runs" }
    New-Item -ItemType Directory -Force $script:TestHome | Out-Null
    @{ pid = $PID; since = (Get-Date).ToString("s") } | ConvertTo-Json | Set-Content (Join-Path $script:TestHome "lock.json") -Encoding utf8
}
function Exit-CoopTestLock {
    if ($script:Mutex) { try { $script:Mutex.ReleaseMutex() } catch { }; $script:Mutex.Dispose(); $script:Mutex = $null }
    Remove-Item (Join-Path $script:TestHome "lock.json") -ErrorAction SilentlyContinue
}

# every write of the runner goes through here
function Assert-SafeWrite([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    $bad = @($script:UserSaves, $script:UserModDir, (Join-Path $script:GameRoot "Game.log"), (Join-Path $script:GameRoot "companion.log"),
        (Join-Path $env:LOCALAPPDATA "CrysisCoop"), (Join-Path $script:TestModDir "Game\Levels"))
    foreach ($b in $bad) {
        $bf = [IO.Path]::GetFullPath($b)
        if ($full -eq $bf -or $full.StartsWith($bf.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw "REFUSED: the autotest does not write $full"
        }
    }
}

# what must look the same after a run: the installed mod's DLL and the saves
function Get-UserState {
    $dll = Join-Path $script:UserModDir "Bin32\Coop.dll"
    $saves = if (Test-Path -LiteralPath $script:UserSaves) {
        @(Get-ChildItem -LiteralPath $script:UserSaves -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object { "$($_.FullName)|$($_.Length)|$($_.LastWriteTimeUtc.Ticks)" }) -join "`n"
    } else { "" }
    [pscustomobject]@{
        Dll   = if (Test-Path -LiteralPath $dll) { (Get-FileHash -LiteralPath $dll -Algorithm MD5).Hash } else { "-" }
        Saves = (Get-StringHash $saves)
        SaveCount = @($saves -split "`n" | Where-Object { $_ }).Count
    }
}
function Get-StringHash([string]$s) {
    $md5 = [Security.Cryptography.MD5]::Create()
    return ([BitConverter]::ToString($md5.ComputeHash([Text.Encoding]::UTF8.GetBytes($s)))).Replace("-", "").Substring(0, 12)
}

# ---- the test mod ----------------------------------------------------------

# Mods\CoopTest: the build's DLL (named after the folder, as the engine looks
# for it), the repository's scripts and configs, hard links of the installed
# mod's levels (a hard link removed is only the link; a junction removed by
# PowerShell 5 takes the files it points to with it)
function Install-TestMod([string]$Dll) {
    if (-not $Dll) { $Dll = Join-Path $script:Repo "build\Bin32\Coop.dll" }
    if (-not (Test-Path -LiteralPath $Dll)) { throw "INFRA: no DLL at $Dll" }
    Remove-TestMod
    $bin = Join-Path $script:TestModDir "Bin32"
    Assert-SafeWrite $bin
    New-Item -ItemType Directory -Force $bin | Out-Null
    Copy-Item -LiteralPath $Dll (Join-Path $bin "$($script:TestMod).dll") -Force
    $map = [IO.Path]::ChangeExtension($Dll, ".map")
    if (Test-Path -LiteralPath $map) { Copy-Item -LiteralPath $map (Join-Path $bin "$($script:TestMod).map") -Force }
    $info = Join-Path $script:Repo "build\info.xml"
    if (-not (Test-Path -LiteralPath $info)) { $info = Join-Path $script:UserModDir "info.xml" }
    (Get-Content -LiteralPath $info -Raw) -replace 'name="Coop"', "name=""$($script:TestMod)""" -replace 'description="[^"]*"', 'description="Coop autotest (removed after each test)"' |
        Set-Content (Join-Path $script:TestModDir "info.xml") -Encoding utf8
    # the scripts and configs
    $null = robocopy (Join-Path $script:Repo "Mod") $script:TestModDir /E /NFL /NDL /NJH /NJS /NP
    if ($LASTEXITCODE -ge 8) { throw "INFRA: robocopy of Mod failed ($LASTEXITCODE)" }
    $global:LASTEXITCODE = 0
    # the levels
    $src = Join-Path $script:UserModDir "Game\Levels"
    $n = 0
    foreach ($f in Get-ChildItem -LiteralPath $src -Recurse -File) {
        $rel = $f.FullName.Substring($src.Length)
        $dst = Join-Path (Join-Path $script:TestModDir "Game\Levels") $rel
        New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
        cmd /c mklink /H "`"$dst`"" "`"$($f.FullName)`"" | Out-Null
        if (-not (Test-Path -LiteralPath $dst)) { throw "INFRA: no hard link for $rel" }
        $n++
    }
    return [pscustomobject]@{ Dll = (Get-FileHash -LiteralPath (Join-Path $bin "$($script:TestMod).dll") -Algorithm MD5).Hash; Levels = $n }
}

function Remove-TestMod {
    if (-not (Test-Path -LiteralPath $script:TestModDir)) { return }
    if ((Split-Path $script:TestModDir -Leaf) -ne $script:TestMod) { throw "not the test mod: $($script:TestModDir)" }
    # links first, one by one (never a recursive delete through a link)
    $levels = Join-Path $script:TestModDir "Game\Levels"
    if (Test-Path -LiteralPath $levels) {
        foreach ($f in Get-ChildItem -LiteralPath $levels -Recurse -File -Force) {
            if ($f.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "unexpected reparse point $($f.FullName)" }
            [IO.File]::Delete($f.FullName)
        }
    }
    foreach ($d in Get-ChildItem -LiteralPath $script:TestModDir -Recurse -Directory -Force) {
        if ($d.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "unexpected junction $($d.FullName)" }
    }
    Remove-Item -LiteralPath $script:TestModDir -Recurse -Force
}

# ---- a boot: profile, games, logs ------------------------------------------

$script:Boot = $null

function New-BootDir([string]$RunDir, [string]$Name) {
    $d = Join-Path $RunDir $Name
    # the engine keeps paths in MAX_PATH buffers: the profile's files must fit
    if ($d.Length -gt 90) { throw "INFRA: the boot folder's path is too long for the engine ($($d.Length) characters): $d" }
    New-Item -ItemType Directory -Force (Join-Path $d "host\SaveGames"), (Join-Path $d "lad") | Out-Null
    return $d
}

# the profile's game.cfg is read before the command line: a window from the
# start and no sound (or sound into a WAV file)
# (VoiceChat: the host's voice chat on, as in a real game - its default -
# with the microphone off; the user's silence came from voice-chat streams)
function Write-GameCfg([string]$Prof, [string]$Sound, [bool]$VoiceChat = $false) {
    # (g_startFirstTime 0 as in the person's game.cfg: a profile's first
    # start runs the engine's machine spec detection, and right then the
    # engine's small-block heap was damaged in 3 boots of 8 - the cause of
    # the test games' start crashes, 2026-10-22; the person's games never run it.
    # And the person's spec, 3, which the detection gave this PC: without it the
    # host ran at 4, where the companion could not join a continued campaign
    # until the 10-22 fix (battle dust; scenarios join-spec4, sound-voip-ai-spec4))
    $lines = @("r_Fullscreen = 0", "r_Width = 800", "r_Height = 600", "cl_voice_recording = 0", "net_enable_voice_chat = $(if ($VoiceChat) { 1 } else { 0 })", "g_startFirstTime = 0", "sys_spec = 3")
    if ($Sound -eq "wav") { $lines += @("s_SoundEnable = 1", "s_DummySound = 0", "s_OutputConfig = 2") }
    else { $lines += @("s_SoundEnable = 0", "s_DummySound = 1") }
    Assert-SafeWrite $Prof
    Set-Content -LiteralPath (Join-Path $Prof "game.cfg") -Value $lines -Encoding ascii
}

# a campaign fixture into the profile: <fixture>\coop\<id>\ and the checkpoint files
function Copy-Fixture([string]$Fixture, [string]$Prof) {
    $src = Join-Path $script:TestHome "fixtures\$Fixture"
    if (-not (Test-Path -LiteralPath $src)) { throw "INFRA: no fixture $Fixture (run.ps1 -Capture <campaign> makes one)" }
    $dst = Join-Path $Prof "SaveGames"
    Assert-SafeWrite $dst
    Copy-Item -Path (Join-Path $src "*") -Destination $dst -Recurse -Force
}

# a copy of one of the person's campaigns (read only there)
function Save-Fixture([string]$Campaign, [string]$Name) {
    $coop = Join-Path $script:UserSaves "coop"
    # by its id, or by its exact name (progress.txt "name=..."; "test123" is
    # not "test123456"); two campaigns of that name: refused
    $ids = @()
    foreach ($d in Get-ChildItem -LiteralPath $coop -Directory) {
        $prog = Join-Path $d.FullName "progress.txt"
        # (@() round the if: an if hands one line on as a bare string, [0] was its first letter)
        $nameLine = @(if (Test-Path -LiteralPath $prog) { Get-Content -LiteralPath $prog -Encoding utf8 | Where-Object { $_ -like "name=*" } | Select-Object -First 1 })
        $campName = if ($nameLine.Count) { $nameLine[0].Substring(5) } else { "" }
        if ($d.Name -eq $Campaign -or $campName -eq $Campaign) { $ids += $d.Name }
    }
    if (-not $ids.Count) { throw "no campaign '$Campaign' in $coop" }
    if ($ids.Count -gt 1) { throw "more than one campaign named '$Campaign' ($($ids -join ', ')): give its id" }
    $id = $ids[0]
    $dst = Join-Path $script:TestHome "fixtures\$Name"
    if (Test-Path -LiteralPath $dst) { Remove-Item -LiteralPath $dst -Recurse -Force }
    New-Item -ItemType Directory -Force (Join-Path $dst "coop") | Out-Null
    Copy-Item -LiteralPath (Join-Path $coop $id) (Join-Path $dst "coop") -Recurse
    Get-ChildItem -LiteralPath $script:UserSaves -File -Filter "*coop_checkpoint_${id}_*" | ForEach-Object { Copy-Item -LiteralPath $_.FullName $dst }
    $files = @(Get-ChildItem -LiteralPath $dst -Recurse -File)
    $files | ForEach-Object { "$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash)  $($_.FullName.Substring($dst.Length + 1))" } | Set-Content (Join-Path $dst "manifest.sha256") -Encoding utf8
    return [pscustomobject]@{ Id = $id; Files = $files.Count; Dir = $dst }
}

# ---- logs ------------------------------------------------------------------

# a log read as it grows (the game keeps it open)
class LogTail {
    [string]$Path
    [long]$Offset = 0
    [string]$Partial = ""
    [System.Collections.Generic.List[string]]$Lines = (New-Object 'System.Collections.Generic.List[string]')
    LogTail([string]$p) { $this.Path = $p }
    [int] Read() {
        if (-not [IO.File]::Exists($this.Path)) { return 0 }
        $fs = $null
        try {
            $fs = New-Object IO.FileStream($this.Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
            if ($fs.Length -lt $this.Offset) { $this.Offset = 0; $this.Partial = ""; $this.Lines.Clear() }   # a new file
            if ($fs.Length -eq $this.Offset) { return 0 }
            [void]$fs.Seek($this.Offset, [IO.SeekOrigin]::Begin)
            $buf = New-Object byte[] ($fs.Length - $this.Offset)
            $n = $fs.Read($buf, 0, $buf.Length)
            $this.Offset += $n
            $text = $this.Partial + [Text.Encoding]::Default.GetString($buf, 0, $n)
            $parts = $text -split "`n"
            $this.Partial = $parts[-1]
            $added = 0
            for ($i = 0; $i -lt $parts.Count - 1; $i++) {
                $s = $parts[$i].TrimEnd("`r")
                $s = [regex]::Replace($s, '^<\d\d:\d\d:\d\d> ', '')
                $this.Lines.Add($s); $added++
            }
            return $added
        } catch { return 0 }
        finally { if ($fs) { $fs.Dispose() } }
    }
}

function New-Mark {
    $m = @{}
    foreach ($k in $script:Boot.Tails.Keys) { $m[$k] = $script:Boot.Tails[$k].Lines.Count }
    $m["at"] = Get-Date
    return $m
}

# lines of a role's log after a mark: @{ No; Text }
function Find-Log([string]$Role, [string]$Pattern, $Since = $null, [switch]$All, [switch]$Last) {
    $tail = $script:Boot.Tails[$Role]
    if (-not $tail) { return $null }
    [void]$tail.Read()
    $from = if ($Since -and $Since.ContainsKey($Role)) { $Since[$Role] } else { 0 }
    $hits = @()
    for ($i = $from; $i -lt $tail.Lines.Count; $i++) {
        $m = [regex]::Match($tail.Lines[$i], $Pattern)
        if ($m.Success) {
            $h = [pscustomobject]@{ No = $i + 1; Text = $tail.Lines[$i]; Match = $m }
            if (-not $All -and -not $Last) { return $h }
            $hits += $h
        }
    }
    if ($Last) { if ($hits.Count) { return $hits[-1] } else { return $null } }
    if ($All) { return $hits }
    return $null
}
function Count-Log([string]$Role, [string]$Pattern, $Since = $null) { return @(Find-Log $Role $Pattern $Since -All).Count }
# a role's log as read so far (Lines: every line, without time stamps)
function Get-Tail([string]$Role) { $t = $script:Boot.Tails[$Role]; [void]$t.Read(); return $t }

# ---- the pump: the only wait -------------------------------------------------

$script:Abort = $null
function Invoke-Pump([double]$Seconds = 0.2) {
    $until = (Get-Date).AddSeconds($Seconds)
    do {
        [void][CoopTestNative]::GuardFocus()
        foreach ($i in [CoopTestNative]::TakeIncidents()) { Write-Step "  FOCUS: $i" }
        if ($script:Boot) {
            foreach ($t in $script:Boot.Tails.Values) { [void]$t.Read() }
            if (((Get-Date) - $script:Boot.LastScan).TotalSeconds -ge 2) {
                $script:Boot.LastScan = Get-Date
                $foreign = @(Get-ForeignGames)
                if ($foreign.Count) {
                    $script:Abort = "USER_GAME: a game the runner did not start is running (" + (($foreign | ForEach-Object { "$($_.ProcessName) $($_.Id)" }) -join ", ") + ")"
                    [CoopTestNative]::KillAll()
                    throw $script:Abort
                }
                Test-GameDialog
            }
        }
        Start-Sleep -Milliseconds 200
    } while ((Get-Date) -lt $until)
}

# an engine message box (e.g. "interface not supported" when no 3D device
# could be made): the game waits for a click that never comes
function Test-GameDialog {
    if (-not ("CoopTestDialogs" -as [type])) {
        Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices;
public static class CoopTestDialogs {
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr p);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, EnumProc f, IntPtr p);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    public static string Find(int[] pids) {
        string found = null;
        EnumWindows(delegate(IntPtr h, IntPtr p) {
            uint wp; GetWindowThreadProcessId(h, out wp);
            if (Array.IndexOf(pids, (int)wp) < 0 || !IsWindowVisible(h)) return true;
            var c = new StringBuilder(64); GetClassName(h, c, 64);
            if (c.ToString() != "#32770") return true;
            var t = new StringBuilder(256); GetWindowText(h, t, 256);
            var sb = new StringBuilder(t.ToString() + ":");
            EnumChildWindows(h, delegate(IntPtr k, IntPtr q) { var s = new StringBuilder(512); GetWindowText(k, s, 512); var kc = new StringBuilder(64); GetClassName(k, kc, 64); if (kc.ToString() == "Static" && s.Length > 0) sb.Append(" " + s.ToString().Replace("\r", " ").Replace("\n", " ")); return true; }, IntPtr.Zero);
            found = wp + " " + sb.ToString();
            return false;
        }, IntPtr.Zero);
        return found;
    }
}
"@
    }
    $d = [CoopTestDialogs]::Find([int[]][CoopTestNative]::JobPids())
    if ($d) {
        $script:Abort = "INFRA: a game shows a message box: $d"
        [CoopTestNative]::KillAll()
        throw $script:Abort
    }
}

# waits for a line; ends early when a game crashed or is gone
function Wait-Log([string]$Role, [string]$Pattern, [double]$Within = 60, $Since = $null) {
    $deadline = (Get-Date).AddSeconds($Within)
    while ($true) {
        $h = Find-Log $Role $Pattern $Since
        if ($h) { return $h }
        $crash = Find-Log $Role "CRASH DETECTED" $Since
        if ($crash) { throw "CRASH: $Role crashed (log line $($crash.No))" }
        if ($Role -eq "host" -and -not (Get-Process -Id $script:Boot.HostPid -ErrorAction SilentlyContinue)) { throw "EXITED: the host's game is gone" }
        if ((Get-Date) -gt $deadline) { return $null }
        Invoke-Pump 0.5
    }
}

# ---- talking to a game -------------------------------------------------------

# a batch of console lines: written only when the game took the last one; its
# first line (a Lua comment) comes back in the log as the proof it ran
function Send-Game([string]$Role, [string[]]$Lines, [double]$AckWithin = 20) {
    $file = if ($Role -eq "host") { "at_host_cmd.txt" } else { "at_companion_cmd.txt" }
    $path = Join-Path $script:GameRoot $file
    $t = 0
    while ((Test-Path -LiteralPath $path) -and $t -lt 120) { Invoke-Pump 0.25; $t++ }
    if (Test-Path -LiteralPath $path) { throw "INFRA: $Role did not take its last commands" }
    $script:Boot.Batch++
    $tag = "#--b$($script:Boot.Batch)"
    foreach ($l in $Lines) { if ($l.Length -gt 1000) { throw "a command line over 1000 characters: $($l.Substring(0, 60))..." } }
    $mark = New-Mark
    Set-Content -LiteralPath "$path.tmp" -Value (@($tag) + $Lines) -Encoding ascii
    Move-Item -LiteralPath "$path.tmp" -Destination $path -Force
    $ack = Wait-Log $Role ("^\[CoopTest\] cmdfile: " + [regex]::Escape($tag) + "$") $AckWithin $mark
    if (-not $ack) { throw "INFRA: no ack from $Role for $tag within $AckWithin s" }
    Write-Step "  > $Role`: $($Lines -join ' | ')"
}

function Pause-Game([double]$Seconds) { Invoke-Pump $Seconds }

# ---- starting and stopping ---------------------------------------------------

# Session: @{ Fixture = 'save:<name>' | 'new:rescue'; Companion = $true|$false|'late'; Cvars = @{}; Extra = @(); Sound = $false|'wav'; Campaign = <id>; ReadyCommands = @(); VoiceChat = $false|$true }
function Start-Boot([hashtable]$Session, [string]$RunDir, [string]$Name) {
    # (a boot that fails before its game starts must not leave the last one's as its own)
    $script:Boot = $null
    $dir = New-BootDir $RunDir $Name
    $prof = Join-Path $dir "host"
    Write-GameCfg $prof $Session.Sound ([bool]$Session.VoiceChat)
    # (a profile as after a first game: the engine runs its first-login spec
    # detection otherwise, every test boot; the person's games never do)
    if ($Session.SeedProfile) {
        $seed = Join-Path $script:TestHome "seed_profile\Profiles"
        if (-not (Test-Path -LiteralPath $seed)) { throw "INFRA: no seed profile $seed" }
        Assert-SafeWrite $prof
        Copy-Item -LiteralPath $seed $prof -Recurse -Force
    }
    $a = @("-mod", $script:TestMod, "-dx9", "-devmode", "-userpath", "`"$prof`"", "-logfile", "at_host.log",
        "+coop_test_free_cursor", "1", "+coop_test_cmdfile", "at_host_cmd.txt", "+r_Fullscreen", "0",
        "+cl_voice_recording", "0", "+net_enable_voice_chat", $(if ($Session.VoiceChat) { "1" } else { "0" }), "+coop_relay", "ws://127.0.0.1:9/", "+coop_cloud", "0")
    $fix = [string]$Session.Fixture
    $comp = $Session.Companion
    $a += @("+coop_companion", $(if ($comp -eq $true) { "1" } else { "0" }))
    if ($Session.Cvars) { foreach ($k in $Session.Cvars.Keys) { $a += @("+$k", [string]$Session.Cvars[$k]) } }
    if ($Session.Extra) { $a += @($Session.Extra) }
    if ($fix -like "save:*") {
        $f = $fix.Substring(5)
        Copy-Fixture $f $prof
        $camp = if ($Session.Campaign) { $Session.Campaign } else { $f }
        $a += @("+coop_continue", $camp)
    } elseif ($fix -like "new:*") {
        $a += @("-coop_skipintro", "+coop_host", $fix.Substring(4), "Autotest", $Name)
    } else { throw "unknown fixture $fix" }
    foreach ($f in "at_host.log", "at_companion.log", "at_host_cmd.txt", "at_companion_cmd.txt") {
        Remove-Item -LiteralPath (Join-Path $script:GameRoot $f) -ErrorAction SilentlyContinue
    }
    $script:Boot = [pscustomobject]@{
        Dir = $dir; Name = $Name; Session = $Session; HostPid = 0; Batch = 0; LastScan = Get-Date; Started = Get-Date
        Tails = @{ host = [LogTail]::new((Join-Path $script:GameRoot "at_host.log")); companion = [LogTail]::new((Join-Path $script:GameRoot "at_companion.log")) }
        Marks = @{}
    }
    # the companion's profile and files in the boot's folder (the person's
    # LOCALAPPDATA stays as it is: the graphics driver keeps its caches there)
    $envs = @("COOP_AUTOTEST_DIR=$(Join-Path $dir 'lad')", "COOP_AUTOTEST=1", "__COMPAT_LAYER=RunAsInvoker")
    # a scenario's own (the companion's game gets the host's environment)
    if ($Session.Env) { foreach ($k in $Session.Env.Keys) { $envs += "$k=$($Session.Env[$k])" } }
    [CoopTestNative]::StartOffScreen()
    [CoopTestNative]::StartWatch()
    # G5 right before the start: a game the person started since the last check
    $foreign = @(Get-ForeignGames)
    if ($foreign.Count) { throw ("USER_GAME: a game the runner did not start is running (" + (($foreign | ForEach-Object { "$($_.ProcessName) $($_.Id)" }) -join ", ") + ")") }
    # no window takes the foreground: the game is made suspended, a 32-bit
    # helper hooks its main thread with Coop.dll's window hook (as the host
    # does for its companion: the engine's first window, made before it loads
    # the mod, is never active nor in the taskbar), then it runs
    $locked = [CoopTestNative]::LockForeground($true)
    try {
        $script:Boot.HostPid = [CoopTestNative]::Start((Join-Path $script:GameRoot "Bin32\Crysis.exe"), ($a -join " "), $script:GameRoot, $envs)
        $helper = Start-WindowHook ([CoopTestNative]::LastThreadId) $dir
        if (-not [CoopTestNative]::Resume($script:Boot.HostPid)) { throw "INFRA: the host's game could not be started" }
        $hooked = if ($helper -eq "ready") { Get-WindowHookResult } else { $helper }
        Write-Step "host started (process $($script:Boot.HostPid)), window hook: $hooked, foreground locked: $locked`: $($a -join ' ')"
        # the test DLL is the one that runs
        $b = Wait-Log host '^\[CoopBuild\] ' 90
        Invoke-Pump 5
    } finally { [void][CoopTestNative]::LockForeground($false) }
    if (-not $b) { throw "INFRA: no [CoopBuild] line in 90 s: is $($script:TestMod).dll loaded?" }
    if ($b.Text -notmatch [regex]::Escape("Mods\$($script:TestMod)\Bin32\$($script:TestMod).dll")) { throw "INFRA: another DLL runs: $($b.Text)" }
    $script:Boot.Marks.Build = $b.Text
    $ready = Wait-Log host '^\[CoopSave\] progress loaded: |^\[Coop\] checkpoint loaded: |checkpoint rescue_start|^\[CoopAI\] co-op AI|Co-op code' 420
    if (-not $ready) { throw "INFRA: the level was not ready within 420 s" }
    Write-Step "level ready: $($ready.Text)"
    $script:Boot.Marks.Ready = New-Mark
    # (commands at once, before the companion is waited for: a checkpoint in
    # a fight kills the host in seconds, coop_settings.cfg set coop_god 0)
    if ($Session.ReadyCommands) { Send-Game host @($Session.ReadyCommands) }
    $script:Boot.Marks.Window = [CoopTestNative]::WindowSize($script:Boot.HostPid)
    if ($comp -eq $true) { Wait-Companion 300 | Out-Null }
    return $script:Boot
}

# the 32-bit helper that hooks a suspended game's main thread with Coop.dll's
# no-activate hook (the runner is 64-bit: it cannot load the game's DLL); the
# hook lasts as long as the helper, which ends by itself after a minute (by
# then the game's own Coop.dll keeps its windows from the foreground)
$script:HookHelper = $null
# a line the helper wrote ("" while it is not there or still being written)
function Read-HelperFile([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return "" }
    try { return ([IO.File]::ReadAllText($Path)).Trim() } catch { return "" }
}

function Start-WindowHook([int]$Thread, [string]$Dir) {
    $ps32 = Join-Path $env:WINDIR "SysWOW64\WindowsPowerShell\v1.0\powershell.exe"
    # (one path for every run, outside the test mod: loaded into the test
    # games and the helper, it must not hold the test mod's folder. Windows
    # keeps a session's hook DLLs in a table of 32 by path, and a game that
    # crashed with the hook in it leaves its entry for good: a new path each
    # run - it was the run's folder - filled the table, then every hook
    # failed with 126 till a reboot (2026-10-18 and 10-21). A path in the
    # table is found again. hook_path.txt in the test home names another
    # DLL path to use)
    $dll = Join-Path $script:TestHome "hook\CoopTestHook.dll"
    $pick = Join-Path $script:TestHome "hook_path.txt"
    if (Test-Path -LiteralPath $pick) {
        $named = [string](Get-Content -LiteralPath $pick -TotalCount 1)
        if ($named -and (Test-Path -LiteralPath $named.Trim())) { $dll = $named.Trim() }
    }
    if (-not (Test-Path -LiteralPath $dll)) {
        $built = Join-Path $script:Repo "build\Bin32\CoopTestHook.dll"
        New-Item -ItemType Directory -Force (Split-Path $dll) | Out-Null
        if (Test-Path -LiteralPath $built) { Copy-Item -LiteralPath $built $dll -Force }
    }
    if (-not (Test-Path -LiteralPath $ps32) -or -not (Test-Path -LiteralPath $dll) -or -not $Thread) { return "none" }
    $ready = Join-Path $Dir "hook_ready.txt"
    $script:HookResult = Join-Path $Dir "hook_result.txt"
    Remove-Item -LiteralPath $ready, $script:HookResult -ErrorAction SilentlyContinue
    $args32 = "-NoProfile -ExecutionPolicy Bypass -File `"$(Join-Path $PSScriptRoot 'hook32.ps1')`" -Dll `"$dll`" -Thread $Thread -Ready `"$ready`" -Result `"$($script:HookResult)`" -Seconds 60"
    $script:HookHelper = [CoopTestNative]::StartHelper($ps32, $args32)
    $until = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $until) {
        $t = Read-HelperFile $ready
        if ($t) { return $t }
        if (-not (Get-Process -Id $script:HookHelper -ErrorAction SilentlyContinue)) { break }
        Start-Sleep -Milliseconds 20
    }
    return "helper not ready"
}

# how the hook went once the game ran a moment
function Get-WindowHookResult {
    if (-not $script:HookResult) { return "none" }
    $until = (Get-Date).AddSeconds(25)
    while ((Get-Date) -lt $until) {
        $t = Read-HelperFile $script:HookResult
        if ($t) { return $t }
        Invoke-Pump 0.2
    }
    return "no result"
}

function Wait-Companion([double]$Within = 300, $Since = $null) {
    $j = Wait-Log host 'renamed to (\$\d)?Companion|revived Companion' $Within $Since
    if (-not $j) { throw "FAIL: the AI companion did not come into the game within $Within s" }
    Write-Step "companion in the game: $($j.Text)"
    return $j
}

function Stop-Boot {
    if (-not $script:Boot) { return }
    try {
        if (Get-Process -Id $script:Boot.HostPid -ErrorAction SilentlyContinue) {
            $path = Join-Path $script:GameRoot "at_host_cmd.txt"
            Set-Content -LiteralPath $path -Value "quit" -Encoding ascii -ErrorAction SilentlyContinue
            $t = 0
            while ($t -lt 40 -and @([CoopTestNative]::JobPids()).Count) { Start-Sleep -Milliseconds 250; [void][CoopTestNative]::GuardFocus(); $t++ }
        }
    } catch { }
    [CoopTestNative]::KillAll()
    Start-Sleep -Milliseconds 500
    foreach ($t in $script:Boot.Tails.Values) { [void]$t.Read() }
    $src = Join-Path $script:GameRoot "at_host.log"
    if (Test-Path -LiteralPath $src) { Copy-Item -LiteralPath $src (Join-Path $script:Boot.Dir "at_host.log") -Force }
    # the companion's log: a companion that ended (crashed) and was started
    # again left its log in LogBackups (the engine moves it there); all of
    # this boot's, oldest first, in one file for the judge
    $parts = @(Get-ChildItem -LiteralPath (Join-Path $script:GameRoot "LogBackups") -Filter "at_companion*.log" -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -ge $script:Boot.Started } | Sort-Object LastWriteTime)
    $cur = Join-Path $script:GameRoot "at_companion.log"
    if (Test-Path -LiteralPath $cur) { $parts += Get-Item -LiteralPath $cur }
    if ($parts.Count) {
        $dst = Join-Path $script:Boot.Dir "at_companion.log"
        $out = New-Object IO.FileStream($dst, [IO.FileMode]::Create)
        try {
            foreach ($p in $parts) { $bytes = [IO.File]::ReadAllBytes($p.FullName); $out.Write($bytes, 0, $bytes.Length) }
        } finally { $out.Dispose() }
        if ($parts.Count -gt 1) { Write-Step "  the companion's game ran $($parts.Count) times in this boot: its logs joined" }
    }
    foreach ($f in "at_host_cmd.txt", "at_companion_cmd.txt") { Remove-Item -LiteralPath (Join-Path $script:GameRoot $f) -ErrorAction SilentlyContinue }
}

# ---- steps, checks, verdicts -------------------------------------------------

$script:Checks = New-Object System.Collections.ArrayList
$script:LogFile = $null
function Write-Step([string]$Text) {
    $line = (Get-Date).ToString("HH:mm:ss") + " " + $Text
    Write-Host $line
    if ($script:LogFile) {
        # (a reader may hold the file a moment)
        for ($k = 0; $k -lt 20; $k++) {
            try { [IO.File]::AppendAllText($script:LogFile, $line + "`r`n", (New-Object Text.UTF8Encoding($false))); break }
            catch { Start-Sleep -Milliseconds 50 }
        }
    }
}
function Set-ProgressLog([string]$Path) { $script:LogFile = $Path }
function Reset-Checks { $script:Checks.Clear() }
function Get-Checks { return @($script:Checks) }

# a soft check: recorded, the scenario goes on
function Check([bool]$Ok, [string]$Text, [string]$Ru = "") {
    [void]$script:Checks.Add([pscustomobject]@{ Kind = "check"; Ok = $Ok; Text = $Text; Ru = $Ru; Mark = (New-Mark) })
    Write-Step ("  " + $(if ($Ok) { "ok  " } else { "FAIL" }) + " $Text")
}
# a precondition: false = the scenario proves nothing (INCONCLUSIVE), never a pass
function Require([bool]$Ok, [string]$Text) {
    [void]$script:Checks.Add([pscustomobject]@{ Kind = "require"; Ok = $Ok; Text = $Text; Ru = ""; Mark = (New-Mark) })
    Write-Step ("  " + $(if ($Ok) { "pre ok" } else { "PRECONDITION NOT MET" }) + " $Text")
    if (-not $Ok) { throw "INCONCLUSIVE: $Text" }
}
function Step([string]$Name, [scriptblock]$Body) {
    Write-Step "step: $Name"
    & $Body
}

function Get-Boot { return $script:Boot }

# ---- the companion's moves and fights: orders over its bridge, a watch of it on the host ----

# ---- orders to the bot without C++ changes: the companion's agent bridge -----
# The companion listens on 127.0.0.1:<port> (CoopAgent.cpp Handle(): "<id> <cmd> [args]"
# -> "<id> <json>\n"); in a test its port file is <boot>\lad\companion\agent.port
# ("port pid", CompanionBase() = COOP_AUTOTEST_DIR). One client at a time.
# Commands: observe | follow [name] | free | hold | goto <x y z | name> | move <dir> <m>
#           attack <name|nearest> | revive [name] | fire | grenade | use | weapon <cls> | suit <speed|strength|cloak|armor>
function Send-Agent([string]$Line, [double]$Within = 10) {
    $pf = Join-Path $script:Boot.Dir "lad\companion\agent.port"
    if (-not (Test-Path -LiteralPath $pf)) { throw "INFRA: no agent.port in $pf" }
    $port = [int](((Get-Content -LiteralPath $pf -Raw).Trim() -split '\s+')[0])
    $c = New-Object Net.Sockets.TcpClient
    try {
        $c.Connect("127.0.0.1", $port)
        $s = $c.GetStream(); $s.ReadTimeout = [int]($Within * 1000)
        $w = New-Object IO.StreamWriter($s); $w.AutoFlush = $true
        $r = New-Object IO.StreamReader($s)
        $script:Boot.Batch++
        $id = "at$($script:Boot.Batch)"
        $w.WriteLine("$id $Line")
        $reply = $null
        $until = (Get-Date).AddSeconds($Within)
        while ((Get-Date) -lt $until) {
            # (the bridge is served from the companion's game loop: ~16 fps)
            $l = $r.ReadLine()
            if ($l -and $l.StartsWith("$id ")) { $reply = $l.Substring($id.Length + 1); break }
        }
    } finally { $c.Close() }
    if (-not $reply) { throw "INFRA: no answer from the companion's bridge to '$Line'" }
    Write-Step "  > agent: $Line -> $($reply.Substring(0, [Math]::Min(140, $reply.Length)))"
    return ($reply | ConvertFrom-Json)
}

# ---- an oracle on the host: every 0.5 s the companion as the server sees it ----
# One "[CHK] w ..." line per tick: position, moved since the last tick, a static/
# rigid thing within 0.7 m in front at waist height, head height above the feet
# (crouch < 1.25), suit mode (0 speed 1 strength 2 cloak 3 armor) and energy,
# health, how many of the watched foes see its head (ray eye->head, terrain +
# static + rigid), distance to the host and to the goal.
# Each line < 1000 chars (Send-Game's limit); functions in separate lines.
function Start-Watch([string[]]$FoeNames = @(), $Goal = $null) {
    $foes = ($FoeNames | ForEach-Object { '"' + $_ + '"' }) -join ","
    $g = if ($Goal) { "{x=$($Goal[0]),y=$($Goal[1]),z=$($Goal[2])}" } else { "nil" }
    Send-Game host ("#AtW={on=true,t=0,name=""Companion"",goal=$g,foes={}}; for _,n in ipairs({$foes}) do local e=System.GetEntityByName(n); if e then table.insert(AtW.foes,e.id) end end; System.LogAlways(""[CHK] watch on, foes ""..#AtW.foes)")
    Send-Game host '#function AtWHead(e) local h=e:GetBonePos("Bip01 Head"); if h and h.z then return {x=h.x,y=h.y,z=h.z} end local p=e:GetWorldPos(); return {x=p.x,y=p.y,z=p.z+1.6} end'
    Send-Game host '#function AtWSeen(c) local n=0; local hd=AtWHead(c); for _,id in ipairs(AtW.foes) do local e=System.GetEntity(id); if e and e.actor and e.actor:GetHealth()>0 then local s=AtWHead(e); if Physics.RayWorldIntersection(s,{x=hd.x-s.x,y=hd.y-s.y,z=hd.z-s.z},1,ent_terrain+ent_static+ent_rigid+ent_sleeping_rigid,e.id,c.id,g_HitTable)==0 then n=n+1 end end end return n end'
    Send-Game host '#function AtWWall(c,p) local f=c:GetDirectionVector(1); return Physics.RayWorldIntersection({x=p.x,y=p.y,z=p.z+1.0},{x=f.x*0.7,y=f.y*0.7,z=0},1,ent_static+ent_rigid+ent_sleeping_rigid,c.id,nil,g_HitTable)>0 and 1 or 0 end'
    Send-Game host '#function AtWLine(c) local p=c:GetWorldPos(); local lp=AtW.last or p; local mv=math.sqrt((p.x-lp.x)^2+(p.y-lp.y)^2); AtW.last={x=p.x,y=p.y,z=p.z}; local dg=-1; if AtW.goal then dg=math.sqrt((p.x-AtW.goal.x)^2+(p.y-AtW.goal.y)^2) end; return string.format("[CHK] w t=%.1f pos=%.1f,%.1f,%.1f mv=%.2f wall=%d head=%.2f suit=%d en=%.0f hp=%d seen=%d dh=%.1f dg=%.1f",AtW.t,p.x,p.y,p.z,mv,AtWWall(c,p),AtWHead(c).z-p.z,c.actor:GetNanoSuitMode(),c.actor:GetNanoSuitEnergy(),c.actor:GetHealth(),AtWSeen(c),c:GetDistance(g_localActorId),dg) end'
    Send-Game host '#function AtWTick() if not AtW.on then return end AtW.t=AtW.t+0.5; local c=System.GetEntityByName(AtW.name); if c and c.actor then System.LogAlways(AtWLine(c)) end Script.SetTimer(500,AtWTick) end; AtWTick()'
}
function Stop-Watch { Send-Game host '#AtW.on=false; System.LogAlways("[CHK] watch off")' }
function Set-WatchGoal($Goal) { Send-Game host "#AtW.goal={x=$($Goal[0]),y=$($Goal[1]),z=$($Goal[2])}; AtW.last=nil; System.LogAlways(""[CHK] watch goal $($Goal -join ' ')"")" }

# the watch lines after a mark, as objects
function Get-Watch($Since) {
    $rx = '^\[CHK\] w t=([\d.]+) pos=(-?[\d.]+),(-?[\d.]+),(-?[\d.]+) mv=([\d.]+) wall=(\d) head=(-?[\d.]+) suit=(-?\d+) en=(-?[\d.]+) hp=(-?\d+) seen=(\d+) dh=(-?[\d.]+) dg=(-?[\d.]+)'
    @(Find-Log host $rx $Since -All | ForEach-Object {
        $g = $_.Match.Groups
        [pscustomobject]@{ No = $_.No; T = [double]$g[1].Value; X = [double]$g[2].Value; Y = [double]$g[3].Value; Z = [double]$g[4].Value; Mv = [double]$g[5].Value
            Wall = [int]$g[6].Value; Head = [double]$g[7].Value; Suit = [int]$g[8].Value; En = [double]$g[9].Value; Hp = [int]$g[10].Value
            Seen = [int]$g[11].Value; Dh = [double]$g[12].Value; Dg = [double]$g[13].Value }
    })
}

# seconds pressed into something: not moving (< 0.1 m in 0.5 s), a solid thing
# within 0.7 m in front, alive, and not where it should be (goal > 2.5 m off, or
# with no goal the host > 4.5 m off)
function Get-BumpSeconds($W) {
    0.5 * @($W | Where-Object { $_.Hp -gt 0 -and $_.Mv -lt 0.1 -and $_.Wall -eq 1 -and $(if ($_.Dg -ge 0) { $_.Dg -gt 2.5 } else { $_.Dh -gt 4.5 }) }).Count
}
# inside an axis-aligned box (x0 x1 y0 y1, the building's waypoint box) at floor height
function Test-Inside($S, $Box, [double]$Floor) {
    $S.X -ge $Box[0] - 0.3 -and $S.X -le $Box[1] + 0.3 -and $S.Y -ge $Box[2] - 0.3 -and $S.Y -le $Box[3] + 0.3 -and [Math]::Abs($S.Z - $Floor) -lt 1.2
}
# peeks: exposed stretches (seen > 0) of 0.5-4 s between hidden ones, with a hit by the companion in them
function Get-Peeks($W, $Since) {
    $hits = @(Find-Log host '\] HIT \S+ by Companion ' $Since -All)
    $n = 0; $exposedFrom = $null
    for ($i = 1; $i -lt $W.Count; $i++) {
        if ($W[$i].Hp -le 0) { $exposedFrom = $null; continue }
        if ($W[$i].Seen -gt 0 -and $W[$i - 1].Seen -eq 0) { $exposedFrom = $i }
        elseif ($W[$i].Seen -eq 0 -and $W[$i - 1].Seen -gt 0 -and $null -ne $exposedFrom) {
            $len = 0.5 * ($i - $exposedFrom)
            # (a peek: short, standing still, and a hit by it in that stretch;
            # any short exposure passing by counted before)
            $from = $W[[Math]::Max(0, $exposedFrom - 1)].No; $to = $W[$i].No
            $still = @($W[$exposedFrom..($i - 1)] | Where-Object { $_.Mv -ge 0.1 }).Count -eq 0
            $shot = @($hits | Where-Object { $_.No -gt $from -and $_.No -le $to }).Count -gt 0
            if ($len -le 4.0 -and $still -and $shot) { $n++ }
            $exposedFrom = $null
        }
    }
    return [pscustomobject]@{ Peeks = $n; HitsByCompanion = $hits.Count }
}

Export-ModuleMember -Function * -Variable Boot
