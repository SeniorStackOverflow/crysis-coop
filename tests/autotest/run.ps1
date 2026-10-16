<#
.SYNOPSIS
The Crysis Coop autotester: plays scenarios in test games (a window left of
all screens, no focus, no sound to the speakers) from Mods\CoopTest, judges
the logs (judge\triage.py) and writes a report in Russian.

.EXAMPLE
  run.ps1 -Scenario smoke                 one scenario (scenarios\**\smoke.scenario.ps1)
  run.ps1 -Suite load                     every scenario tagged "load"
  run.ps1 -Suite all -Dll old.dll         on another DLL (A/B proof)
  run.ps1 -Capture checking               copy the person's campaign "checking" as a fixture (read only there)
  run.ps1 -Cleanup                        remove Mods\CoopTest and leftovers

Exit: 0 all passed, 1 a product failure, 2 the test went wrong / aborted,
3 a game the runner did not start is running (the person plays).
#>
param(
    [string]$Suite = "",
    [string[]]$Scenario = @(),
    [string]$Dll = "",
    [string]$Capture = "",
    [string]$FixtureName = "",
    [switch]$Cleanup,
    [switch]$List,
    [int]$Repeat = 1
)
$ErrorActionPreference = "Stop"
Import-Module (Join-Path $PSScriptRoot "lib\CoopTest.psm1") -Force -DisableNameChecking
$paths = Get-CoopTestPaths

# another run holds the lock: exit 2 (the test could not run), not 1 (a product failure)
function Enter-LockOrExit {
    try { Enter-CoopTestLock } catch { Write-Host "$_"; exit 2 }
}

if ($Cleanup -or $Capture) {
    # (the person's game may be writing a checkpoint: no copy of it now)
    if (@(Get-ForeignGames).Count) { Write-Host "a game is running: not now"; exit 3 }
    Enter-LockOrExit
    try {
        if ($Cleanup) {
            Remove-TestMod
            foreach ($f in "at_host_cmd.txt", "at_companion_cmd.txt") { Remove-Item -LiteralPath (Join-Path $paths.GameRoot $f) -ErrorAction SilentlyContinue }
            Write-Host "clean"
        } else {
            $name = if ($FixtureName) { $FixtureName } else { $Capture }
            $r = Save-Fixture $Capture $name
            Write-Host "fixture '$name': campaign $($r.Id), $($r.Files) files in $($r.Dir)"
        }
    } finally { Exit-CoopTestLock }
    exit 0
}

# (powershell -File passes "a,b" as one string)
$Scenario = @($Scenario | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
# the scenarios
$all = @()
foreach ($f in Get-ChildItem (Join-Path $PSScriptRoot "scenarios") -Recurse -Filter "*.scenario.ps1") {
    # read as UTF-8 whatever its BOM (PowerShell 5 reads a file without one in
    # the ANSI code page and the Russian text breaks the parse)
    try { $sc = & ([scriptblock]::Create([IO.File]::ReadAllText($f.FullName, [Text.Encoding]::UTF8))) }
    catch { Write-Host "scenario file $($f.Name) does not parse: $_"; continue }
    $sc.File = $f.FullName
    if (-not $sc.ContainsKey("Tags")) { $sc.Tags = @() }
    $all += $sc
}
if ($List) {
    $all | ForEach-Object { "{0,-34} {1,-28} {2}" -f $_.Name, ($_.Tags -join ","), $_.SummaryRu }
    exit 0
}
$pick = @($all | Where-Object { ($Scenario -contains $_.Name) -or ($Suite -and ($Suite -eq "all" -or $_.Tags -contains $Suite)) })
if (-not $pick.Count) { Write-Host "no scenario picked (-List shows them)"; exit 2 }

# G5: never while the person plays
$foreign = @(Get-ForeignGames)
if ($foreign.Count) {
    Write-Host ("a game the runner did not start is running: " + (($foreign | ForEach-Object { "$($_.ProcessName) $($_.Id)" }) -join ", ") + ": no test now")
    exit 3
}

$stamp = (Get-Date).ToString("yyMMdd-HHmmss")
$runDir = Join-Path $paths.TestHome "runs\$stamp"
New-Item -ItemType Directory -Force $runDir | Out-Null
Set-ProgressLog (Join-Path $runDir "progress.log")
$results = @()
$retried = @{}
$exit = 0
Enter-LockOrExit
$before = Get-UserState
$build = $null
try {
    $build = Install-TestMod $Dll
    Copy-Item (Join-Path $paths.TestModDir "Bin32\CoopTest.map") $runDir -ErrorAction SilentlyContinue
    Write-Step "test mod: CoopTest.dll $($build.Dll), $($build.Levels) level files linked; run folder $runDir"
    foreach ($sc in $pick) {
        for ($rep = 1; $rep -le [Math]::Max(1, $Repeat); $rep++) {
            $name = $sc.Name + $(if ($Repeat -gt 1) { "#$rep" } else { "" })
            Reset-Checks
            $status = "PASS"; $why = ""
            $t0 = Get-Date
            Write-Step "=== $name"
            $bodyStarted = $false
            try {
                $boot = Start-Boot $sc.Session $runDir ("b" + ($results.Count + 1))
                $bodyStarted = $true
                $P = if ($sc.ContainsKey("Params")) { $sc.Params } else { @{} }
                & $sc.Body $P
            } catch {
                $why = "$_"
                $status = switch -Regex ($why) {
                    '^CRASH' { "CRASH" }
                    # the host's game gone without a crash report (a stack
                    # overflow, a fast fail): a crash all the same
                    '^EXITED' { "CRASH" }
                    '^(INFRA|LOCKED)' { "INFRA" }
                    '^INCONCLUSIVE' { "INCONCLUSIVE" }
                    '^USER_GAME' { "ABORTED" }
                    '^FAIL' { "FAIL" }
                    default { "INFRA" }
                }
                # anything else is the test script's own error, not the mod's
                if ($status -eq "INFRA" -and $why -notmatch '^(INFRA|LOCKED)') { $why = "test script error: $why (" + ($_.InvocationInfo.PositionMessage -split "`n")[0] + ")" }
                Write-Step "  $status`: $why"
            } finally {
                Stop-Boot
            }
            $boot = Get-Boot
            # the judge: everything the logs say (its own failure is the test's, not the mod's)
            $judge = $null
            $jexit = 0
            if ($boot -and (Test-Path (Join-Path $boot.Dir "at_host.log"))) {
                try {
                    $comp = Join-Path $boot.Dir "at_companion.log"
                    $args2 = @((Join-Path $PSScriptRoot "judge\triage.py"), (Join-Path $boot.Dir "at_host.log"))
                    if (Test-Path $comp) { $args2 += $comp }
                    $args2 += @("--json", (Join-Path $boot.Dir "findings.json"), "--ru", (Join-Path $boot.Dir "findings.ru.md"), "--quiet")
                    $map = Join-Path $runDir "CoopTest.map"
                    if (Test-Path $map) { $args2 += @("--map", $map) }
                    Remove-Item (Join-Path $boot.Dir "findings.json") -ErrorAction SilentlyContinue
                    & python @args2
                    $jexit = $LASTEXITCODE
                    $judge = Get-Content (Join-Path $boot.Dir "findings.json") -Raw -Encoding utf8 | ConvertFrom-Json
                } catch {
                    $judge = $null
                    $jexit = 4
                    Write-Step "  the judge failed: $_"
                }
            }
            # the worst of: what the scenario ran into, its checks, the judge
            # (a product failure counts above a test that went wrong)
            $rank = @{ PASS = 0; INCONCLUSIVE = 1; INFRA = 2; FAIL = 3; CRASH = 4; ABORTED = 5 }
            $raise = { param($to) if ($rank[$to] -gt $rank[$script:statusNow]) { $script:statusNow = $to } }
            $script:statusNow = $status
            if ($status -ne "ABORTED") {
                if (@(Get-Checks | Where-Object { -not $_.Ok -and $_.Kind -eq "check" }).Count) { & $raise "FAIL" }
                switch ($jexit) { 2 { & $raise "CRASH" } 1 { & $raise "FAIL" } 3 { & $raise "INFRA" } 4 { & $raise "INFRA" } }
                if (-not $judge -and $boot) { & $raise "INFRA" }
            }
            $status = $script:statusNow
            if ($sc.ContainsKey("KnownBug") -and $sc.KnownBug) {
                if ($status -eq "FAIL") { $status = "XFAIL" } elseif ($status -eq "PASS") { $status = "XPASS" }
            }
            $r = [pscustomobject]@{
                Name = $name; Status = $status; Why = $why; SummaryRu = $sc.SummaryRu; Minutes = [Math]::Round(((Get-Date) - $t0).TotalMinutes, 1)
                Checks = @(Get-Checks | ForEach-Object { [pscustomobject]@{ Ok = $_.Ok; Kind = $_.Kind; Text = $_.Text } })
                Findings = if ($judge) { @($judge.findings) } else { @() }
                Dir = if ($boot) { $boot.Dir } else { "" }
                Build = if ($boot) { $boot.Marks.Build } else { "" }
                Window = if ($boot) { $boot.Marks.Window } else { "" }
            }
            # a test game that crashed in the graphics driver as it started
            # (d3d9.dll, before the scenario did anything: about 1 in 10
            # test starts on this PC, never seen in a played game, cause not
            # found): reported, and the scenario once more
            # (only that crash: d3d9.dll+0x12359C, read from 0x25; any other
            # crash in the driver may be the mod's and counts, the review)
            $startCrash = $false
            if ($status -eq "CRASH" -and -not $bodyStarted -and $boot) {
                $hostLog = Join-Path $boot.Dir "at_host.log"
                $lines = @(Get-Content -LiteralPath $hostLog -ErrorAction SilentlyContinue)
                $frame = @($lines | Select-String -Pattern '^([0-9A-F]{8}) (\S+): ' | Select-Object -First 1)
                $base = @($lines | Select-String -Pattern '^([0-9A-F]{8}) - [0-9A-F]{8} .*\\d3d9\.dll$' | Select-Object -First 1)
                if ($frame.Count -and $base.Count -and $frame[0].Matches[0].Groups[2].Value -eq "d3d9.dll") {
                    $off = [Convert]::ToInt64($frame[0].Matches[0].Groups[1].Value, 16) - [Convert]::ToInt64($base[0].Matches[0].Groups[1].Value, 16)
                    $startCrash = $off -eq 0x12359C -and ($lines -match 'Read from 0x00000025 failed').Count -gt 0
                }
            }
            if ($startCrash -and -not $retried.ContainsKey($name)) {
                $retried[$name] = $true
                $r.Status = "INFRA"
                $r.Why = "the test game crashed in the graphics driver as it started (d3d9.dll; known on this PC): the scenario once more"
                $results += $r
                Write-Step "=== $name`: crashed at its start in d3d9.dll: once more"
                $rep--
                continue
            }
            $results += $r
            Write-Step "=== $name`: $status ($($r.Minutes) min)"
            if ($status -eq "ABORTED") { break }
        }
        if ($results.Count -and $results[-1].Status -eq "ABORTED") { break }
    }
} catch {
    Write-Step "RUN ERROR: $_"
    $exit = 2
} finally {
    try { Remove-TestMod } catch { Write-Step "could not remove Mods\CoopTest: $_" }
    Exit-CoopTestLock
}
$after = Get-UserState

# ---- the report ---------------------------------------------------------------
$ru = @{ PASS = "прошёл"; FAIL = "ОШИБКА"; CRASH = "ПАДЕНИЕ"; INFRA = "сбой теста"; INCONCLUSIVE = "не проверено"; ABORTED = "прерван"; XFAIL = "известная ошибка"; XPASS = "известная ошибка больше не видна" }
$lines = @()
$lines += "Автотест $((Get-Date).ToString('dd.MM.yyyy HH:mm')), сборка $(if ($build) { $build.Dll.Substring(0, 8) } else { '-' }), сценариев: $($results.Count)"
$n = @{}; foreach ($r in $results) { $n[$r.Status] = 1 + $(if ($n.ContainsKey($r.Status)) { $n[$r.Status] } else { 0 }) }
$lines += "ИТОГ: " + (($n.Keys | Sort-Object | ForEach-Object { "$($ru[$_]): $($n[$_])" }) -join ", ")
$g = @()
$g += "окно " + $(if ($results.Count -and $results[0].Window) { $results[0].Window } else { "?" })
$g += "фокус у тестовой игры: $([CoopTestNative]::FocusIncidents) раз" + $(if ([CoopTestNative]::FocusPolls) { " (~$([Math]::Round([CoopTestNative]::FocusPolls * 0.2, 1)) с, подробности в progress.log: FOCUS)" } else { "" })
$g += "звук: только в файл или выключен"
$g += $(if ($before.Saves -eq $after.Saves) { "сохранения не тронуты ($($after.SaveCount) файлов)" } else { "СОХРАНЕНИЯ ИЗМЕНИЛИСЬ (было $($before.SaveCount), стало $($after.SaveCount) файлов: игрок играл или ошибка теста)" })
$g += $(if ($before.Dll -eq $after.Dll) { "установленный мод не тронут" } else { "УСТАНОВЛЕННЫЙ Coop.dll ИЗМЕНИЛСЯ" })
$lines += "Правила: " + ($g -join "; ")
$i = 0
foreach ($r in $results) {
    $i++
    $lines += ""
    $lines += "$i. [$($ru[$r.Status])] $($r.Name): $($r.SummaryRu) ($($r.Minutes) мин)"
    if ($r.Why) { $lines += "   причина: $($r.Why)" }
    foreach ($c in $r.Checks) { $lines += "   " + $(if ($c.Ok) { "+" } else { "-" }) + " $($c.Text)" }
    foreach ($f in $r.Findings) {
        if ($f.verdict -eq "WARN") { continue }
        $lines += "   ! [$($f.id) $($f.verdict)] $($f.ru)" + $(if ($f.count -gt 1) { " (x$($f.count))" } else { "" })
        foreach ($e in @($f.evidence | Select-Object -First 2)) { $lines += "       $($e.where)  $($e.text.Substring(0, [Math]::Min(150, $e.text.Length)))" }
    }
    $w = @($r.Findings | Where-Object { $_.verdict -eq "WARN" })
    if ($w.Count) { $lines += "   заметки: " + (($w | ForEach-Object { $_.ru }) -join "; ") }
    $lines += "   логи: $($r.Dir)"
}
$text = $lines -join "`r`n"
[IO.File]::WriteAllText((Join-Path $runDir "summary.ru.md"), $text, (New-Object Text.UTF8Encoding($true)))
$results | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $runDir "results.json") -Encoding utf8
Write-Host ""
Write-Host $text
Write-Step "DONE $runDir"
if ($exit -eq 0) {
    if ($results | Where-Object { $_.Status -eq "ABORTED" }) { $exit = 3 }
    elseif ($results | Where-Object { $_.Status -in @("FAIL", "CRASH", "XPASS") }) { $exit = 1 }
    elseif ($results | Where-Object { $_.Status -in @("INFRA", "INCONCLUSIVE") -and $_.Why -notlike "the test game crashed in the graphics driver as it started*" }) { $exit = 2 }
}
Add-Content (Join-Path $runDir "progress.log") "DONE exit=$exit"
exit $exit
