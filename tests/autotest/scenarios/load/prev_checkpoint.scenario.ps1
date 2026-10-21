# The user's game of 2026-10-21 (campaign "checking", copied as fixture
# checking1021: progress.txt Rescue_05 in slot _b, progress_prev.txt manual in
# slot _a). The co-op menu's "previous checkpoint" loaded manual into the
# running level with the AI companion kept; the companion was put next to the
# host's place of the checkpoint before (270 m off); FMOD held 64 nameless
# full-volume channels and nothing came out (the watch restarted the sound
# 3 s later); when everybody went down, the game went back to Rescue_05, not
# to the checkpoint loaded.
# Here: the game starts at Rescue_05; manual is loaded once (a savegame not
# loaded in this game yet: its own list of entities), then Rescue_05 again,
# then - as the user - manual into the running level with the companion kept.
# After each load the companion must stand by the host, the sound must go on,
# and everybody down must load manual again.
@{
    Name = "load-prev-checkpoint"
    Tags = @("load", "sound", "companion", "regress")
    SummaryRu = "Предыдущий чекпоинт на копии кампании checking: он становится текущим (все упали - грузится он же), компаньон сразу рядом с хозяином, звук не пропадает"
    Session = @{ Fixture = "save:checking1021"; Campaign = "56c79107e0d7a6ff"; Companion = $true; Sound = "wav"; VoiceChat = $true; ReadyCommands = @("coop_god 1", "coop_debug_flags 8") }
    Params = @{ Rounds = 2; Near = 15 }
    Body = {
        param($P)
        $state = '#local c=System.GetEntityByName("Companion"); local n=System.GetEntityByName("Nomad"); local function f(e) if not e then return "-" end local p=e:GetWorldPos(); return string.format("%.0f %.0f %.0f hp %d", p.x, p.y, p.z, e.actor:GetHealth()) end; local d=-1; if c and n then local a,b=c:GetWorldPos(),n:GetWorldPos(); d=math.sqrt((a.x-b.x)^2+(a.y-b.y)^2) end; System.LogAlways(string.format("[CHK] TAG: companion %s host %s apart %.0f", f(c), f(n), d))'
        function Get-Channels([string]$Tag) {
            $m = New-Mark
            Send-Game host "coop_test_sound channels"
            Pause-Game 2
            $all = @(Find-Log host '^\[CoopTest\] channel \d+ ' $m -All)
            $nameless = @($all | Where-Object { $_.Text -match '^\[CoopTest\] channel \d+ (\?)?: ' })
            $sum = Find-Log host '^\[CoopTest\] coop_test_sound channels : (\d+); (.*)' $m
            Check $true ("$Tag`: FMOD channels playing $($all.Count), nameless $($nameless.Count); " + $(if ($sum) { $sum.Match.Groups[2].Value } else { '' }))
            return $nameless.Count
        }
        function Get-Silence($Mark) {
            return @(Find-Log host '^\[CoopSound\] (\d+ s without sound|no sound came out|still no sound|the sound.s master group was|the sound.s mix is not|the heard sounds stand|sounds are heard and the mix is empty|FMOD plays \d+ audible sounds)' $Mark -All | ForEach-Object { $_.Text -replace '^\[CoopSound\] ', '' })
        }
        # where both are right after a load (the first state line once the
        # companion is back), and again a while later
        function Test-Together([string]$Tag, $Mark, [double]$After = 3) {
            Pause-Game $After
            Send-Game host $state.Replace("TAG", $Tag)
            Pause-Game 1.5
            $s = Find-Log host ("^\[CHK\] " + [regex]::Escape($Tag) + ": (.*) apart (-?\d+)") $Mark
            Check ($s -and $s.Text -notmatch 'companion -' -and [int]$s.Match.Groups[2].Value -ge 0 -and [int]$s.Match.Groups[2].Value -lt $P.Near) "$Tag`: the companion stands by the host ($(if ($s) { $s.Match.Groups[1].Value + ', apart ' + $s.Match.Groups[2].Value + ' m' } else { 'no state line' }))"
            $far = @(Find-Log host '^\[CoopAI\] joined player (\d+) m from the host' $Mark -All | Where-Object { [int]$_.Match.Groups[1].Value -gt 60 })
            Check ($far.Count -eq 0) "$Tag`: no 'joined player far from the host' ($(if ($far.Count) { $far[0].Text } else { 'none' }))"
        }
        # a load into the running level: which checkpoint, the companion kept
        function Invoke-Load([string]$Cmd, [string]$Expect, [string]$Tag, [bool]$Kept = $true) {
            $m = New-Mark
            Send-Game host $Cmd
            $c = Wait-Log host 'continuing campaign "checking": rescue, checkpoint (\S+)' 60 $m
            Require ($null -ne $c) "$Tag`: a checkpoint is loaded"
            Check ($c.Match.Groups[1].Value -eq $Expect) "$Tag`: the checkpoint loaded is $Expect ($($c.Match.Groups[1].Value))"
            $end = Wait-Log host "^\[Coop\] checkpoint loaded: \d+ players' places kept" 300 $m
            Require ($null -ne $end) "$Tag`: the load ended"
            Send-Game host "coop_god 1"
            if ($Kept) {
                $k = Find-Log host 'friend\(s\) stay in the game through the load' $m
                Check ($null -ne $k) "$Tag`: the companion stayed in the game through the load"
            } else {
                Wait-Companion 240 $m | Out-Null
            }
            return $m
        }
        Step "the campaign continued at its newest checkpoint" {
            $c = Find-Log host 'continuing campaign "checking": rescue, checkpoint (\S+)'
            Require ($null -ne $c) "the campaign was continued"
            Check ($c.Match.Groups[1].Value -eq 'Rescue_05') "it started at the newest checkpoint: $($c.Match.Groups[1].Value)"
            Send-Game host "coop_god 1", "coop_test_sound watch fmod"
            Test-Together "at Rescue_05" (New-Mark) 2
        }
        Step "the previous checkpoint once (not loaded in this game yet: its own list of entities, the companion kept)" {
            $m = Invoke-Load "coop_load prev" "manual" "first manual"
            $list = Find-Log host "was not saved or loaded in this game: its own list of (\d+) entities taken" $m
            Check ($null -ne $list) "the save's own list of entities was taken ($(if ($list) { $list.Match.Groups[1].Value } else { 'no such line' }))"
            $cur = Find-Log host "^\[CoopSave\] checkpoint manual \(saved [^)]*\) is the campaign's current checkpoint now" $m
            Check ($null -ne $cur) "manual is the campaign's current checkpoint now ($(if ($cur) { 'said so' } else { 'no such line' }))"
            Test-Together "after the first manual" $m 8
        }
        Step "Rescue_05 again (the companion kept)" {
            # (the fixed game: Rescue_05 is the 'previous' now, the newer one)
            $swapped = $null -ne (Find-Log host "is the campaign's current checkpoint now")
            $m = Invoke-Load $(if ($swapped) { "coop_load prev" } else { "coop_load" }) "Rescue_05" "back at Rescue_05"
            Test-Together "back at Rescue_05" $m 3
            Pause-Game 20
        }
        Step "as the user: the previous checkpoint into the running level, the companion kept" {
            $m = Invoke-Load "coop_load prev" "manual" "manual kept"
            Test-Together "manual kept" $m 1
            $n3 = Get-Channels "3 s after the load"
            Pause-Game 10
            $n13 = Get-Channels "13 s after the load"
            Pause-Game 15
            Test-Together "manual kept, 30 s on" $m 0
            $silence = @(Get-Silence $m)
            Check ($silence.Count -eq 0) ("no silence after the previous checkpoint: " + $(if ($silence.Count) { ($silence | Select-Object -First 4) -join ' | ' } else { 'none' }))
            Check ($n3 -lt 8 -and $n13 -lt 8) "no pile of nameless FMOD channels after the load ($n3, $n13)"
            $fell = @(Find-Log host 'Companion fell through the ground' $m -All)
            Check ($fell.Count -eq 0) "the companion was not put back as fallen through the ground ($($fell.Count))"
        }
        for ($r = 1; $r -le $P.Rounds; $r++) {
            Step "everybody down #$r`: the checkpoint loaded last again" {
                $m = New-Mark
                Send-Game host "coop_god 0", "coop_test_kill Companion", "coop_test_kill Nomad"
                $down = Wait-Log host 'everybody is down: the last checkpoint is loaded' 60 $m
                Require ($null -ne $down) "round $r`: everybody down -> a checkpoint is loaded"
                $c = Wait-Log host 'continuing campaign "checking": rescue, checkpoint (\S+)' 60 $m
                Check ($c -and $c.Match.Groups[1].Value -eq 'manual') "round $r`: the checkpoint loaded is the one loaded last (manual): $(if ($c) { $c.Match.Groups[1].Value } else { 'none' })"
                $end = Wait-Log host "^\[Coop\] checkpoint loaded: \d+ players' places kept" 300 $m
                Require ($null -ne $end) "round $r`: the load ended"
                Send-Game host "coop_god 1"
                Test-Together "round $r" $m 3
                Pause-Game 20
                $silence = @(Get-Silence $m)
                Check ($silence.Count -eq 0) ("round $r`: no silence after the load: " + $(if ($silence.Count) { ($silence | Select-Object -First 4) -join ' | ' } else { 'none' }))
            }
        }
        Step "the progress files" {
            $prof = Join-Path (Get-Boot).Dir "host\SaveGames\coop\56c79107e0d7a6ff"
            foreach ($f in "progress.txt", "progress_prev.txt") {
                $p = Join-Path $prof $f
                $t = if (Test-Path -LiteralPath $p) { (Get-Content -LiteralPath $p | Where-Object { $_ -match '^(save|checkpoint|time)=' }) -join ' ' } else { 'missing' }
                Check $true "$f`: $t"
            }
            $cur = Join-Path $prof "progress.txt"
            Check ((Test-Path -LiteralPath $cur) -and ((Get-Content -LiteralPath $cur) -contains 'checkpoint=manual')) "progress.txt names manual"
        }
    }
}
