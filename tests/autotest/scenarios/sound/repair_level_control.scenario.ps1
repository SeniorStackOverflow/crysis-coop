# The sound watch sets a paused or muted FMOD master group right, and a
# second fault within 20 s restarts the sound system (sound-master-fault).
# Does the sound come back as loud as it was? (in the WAV file of that test
# the level after the first repair was a tenth of before: a quieter place,
# or the repair?). Here a steady loop 5 m from the host, FMOD's own final mix
# measured before, after a repair and after a restart. Sound to a WAV file.
@{
    Name = "sound-repair-level-control"
    Tags = @("sound")
    SummaryRu = "Контроль к sound-repair-level: те же замеры громкости петли в те же моменты, но без сбоев, починок и перезапуска"
    Session = @{ Fixture = "new:rescue"; Companion = $false; Sound = "wav" }
    Params = @{ Loop = "sounds/weapons:fy71:fire_3rd_single_outdoor"; Control = $true }
    Body = {
        param($P)
        # how loud FMOD plays the loop's channel: its audibility (volume times
        # distance, the groups' volumes...), the same whatever moment of the
        # shot a look meets (FMOD's mix of a shot over and over is not)
        function Get-MixPeaks([int]$N) {
            $vals = @()
            for ($i = 0; $i -lt $N; $i++) {
                $mk = New-Mark
                Send-Game host "coop_test_sound channels fy71"
                $null = Wait-Log host '^\[CoopTest\] coop_test_sound channels fy71: ' 5 $mk
                $best = -1.0
                foreach ($l in @(Find-Log host '^\[CoopTest\] channel \d+ \S*fy71\S*: heard.*audibility ([\d.]+)' $mk -All)) {
                    if ($l.Text -match 'audibility ([\d.]+)' -and [double]$Matches[1] -gt $best) { $best = [double]$Matches[1] }
                }
                if ($best -ge 0) { $vals += $best }
                Pause-Game 0.5
            }
            return $vals
        }
        function Median($v) { $s = @($v | Sort-Object); if ($s.Count) { return $s[[Math]::Floor($s.Count / 2)] } return -1 }
        Step "a steady loop by the host" {
            Send-Game host "coop_god 1"
            Send-Game host "coop_test_sound watch"
            Pause-Game 15
            $m = New-Mark
            Send-Game host "coop_test_sound playloop $($P.Loop)"
            $ok = Wait-Log host '^\[CoopTest\] playloop .*playing' 10 $m
            Require ($null -ne $ok) "the loop plays"
            Pause-Game 4
            $script:before = @(Get-MixPeaks 6)
            Check ($script:before.Count -ge 4) ("the loop's audibility before: " + ($script:before -join ' ') + " (the loop's audibility, median $(Median $script:before))")
        }
        Step "after a repair of a paused master group" {
            $m = New-Mark
            if ($P.Control) { Pause-Game 4 } else {
                Send-Game host "coop_test_sound fmodpause"
                $fix = Wait-Log host '^\[CoopSound\] the sound.s master group was paused behind the game.s back: set right' 6 $m
                Require ($null -ne $fix) "the paused master group was set right"
            }
            Pause-Game 3
            $after = @(Get-MixPeaks 6)
            $b = Median $script:before; $a = Median $after
            Check ($a -ge 0.8 * $b -and $a -gt 0.01) ("as loud after the repair: the loop's audibility $a against $b before (" + ($after -join ' ') + ")")
        }
        Step "after a restart of the sound system" {
            Pause-Game 22
            $m = New-Mark
            if ($P.Control) { Pause-Game 6; Send-Game host "coop_test_sound playloop stop" } else {
                Send-Game host "coop_test_sound fmodpause"
                $fix = Wait-Log host '^\[CoopSound\] the sound.s master group was paused behind the game.s back: set right' 6 $m
                Pause-Game 2
                Send-Game host "coop_test_sound fmodmute"
                $again = Wait-Log host '^\[CoopSound\] the master group was set wrong again within 20 s: the sound system starts again' 6 $m
                Require ($null -ne $again) "the sound system restarted"
            }
            Pause-Game 6
            # (the restart dropped every sound: the loop again)
            $m2 = New-Mark
            Send-Game host "coop_test_sound playloop $($P.Loop)"
            $ok = Wait-Log host '^\[CoopTest\] playloop .*playing' 10 $m2
            Pause-Game 4
            $after = @(Get-MixPeaks 6)
            $b = Median $script:before; $a = Median $after
            # (half: the loop's audibility drifts by itself over the minute, its channel
            # volume 0.67 -> 0.84, the audibility 0.25 -> 0.15-0.17 with no fault and
            # no restart at all; sound-repair-level-control, 2026-10-21)
            Check ($null -ne $ok -and $a -ge 0.5 * $b -and $a -gt 0.01) ("as loud after the restart: the loop's audibility $a against $b before (" + ($after -join ' ') + ")")
            Send-Game host "coop_test_sound playloop stop"
        }
    }
}
