# The user's game of 2026-10-17: a rifle's gunfire went on and on, for ever,
# after the fight with the bridge truck. Whatever leaves a weapon's shot
# playing (its fire loop never told to stop), the sound watch must end it:
# no weapon fires 15 s without a pause. Here a FY71 shot is played 5 m from
# the host and never stopped.
@{
    Name = "gunfire-loop-watch"
    Tags = @("sound")
    SummaryRu = "Звук очереди, который никто не остановил, сторож звука глушит через 15 с (бесконечная стрельба)"
    Session = @{ Fixture = "new:rescue"; Companion = $false; Sound = "wav" }
    Params = @{ Event = "Sounds/weapons:fy71:fire_3rd_loop" }
    Body = {
        param($P)
        function Get-GunLoops([string]$Tag) {
            $m = New-Mark
            Send-Game host "coop_test_sound channels"
            Pause-Game 1
            $all = @(Find-Log host '^\[CoopTest\] channel \d+ ' $m -All)
            $loops = @($all | Where-Object { $_.Text -match '(?i)\] channel \d+ \S*loop' -and $_.Text -match '(?i)3rd|_cu_|_fp|fire|mg_' })
            Check $true ("$Tag`: gunfire loops: " + $(if ($loops.Count) { (($loops | ForEach-Object { ($_.Text -replace '^\[CoopTest\] ', '') }) -join "; ") } else { "none" }))
            return $loops
        }
        Step "a FY71 shot left playing" {
            Pause-Game 45
            $m = New-Mark
            Send-Game host "coop_test_sound playloop $($P.Event)"
            $ok = Wait-Log host '^\[CoopTest\] playloop .*playing' 10 $m
            Require ($null -ne $ok) "the shot plays"
            Pause-Game 1
            $mc = New-Mark
            Send-Game host "coop_test_sound channels fy71"
            Pause-Game 1
            $ch = @(Find-Log host '^\[CoopTest\] channel \d+ ' $mc -All)
            Check $true ("1 s on, FMOD channels of fy71: " + (($ch | ForEach-Object { ($_.Text -replace '^\[CoopTest\] ', '') }) -join "; "))
            Pause-Game 2
            $l5 = @(Get-GunLoops "5 s on")
            Check ($l5.Count -gt 0) "5 s on its fire loop plays ($($l5.Count))"
            $stopped = Wait-Log host '^\[CoopSound\] a gunfire loop played on' 25 $m
            Check ($null -ne $stopped) "the sound watch stopped it: $(if ($stopped) { $stopped.Text } else { 'no' })"
            Pause-Game 3
            $l25 = @(Get-GunLoops "after the watch")
            Check ($l25.Count -eq 0) "afterwards no fire loop plays ($($l25.Count))"
            Pause-Game 15
            $l40 = @(Get-GunLoops "15 s later")
            Check ($l40.Count -eq 0) "15 s later it has not come back ($($l40.Count))"
            Send-Game host "coop_test_sound playloop stop"
        }
    }
}
