# The user's games: a long helicopter hum though no helicopter flies. The
# vehicles the level hides until the story needs them must make no sound.
# Here, on a copy of the campaign "checking": Continue and an in-place load,
# then every vehicle's engine sound listed.
@{
    Name = "hidden-vehicles-silent"
    Tags = @("sound", "load", "vehicle")
    SummaryRu = "Спрятанные вертолёты и машины не гудят (после продолжения и после загрузки)"
    Session = @{ Fixture = "save:checking"; Companion = $false; Sound = "wav" }
    Body = {
        param($P)
        function Test-Hum([string]$When) {
            $m = New-Mark
            Send-Game host "coop_test_vehicles all"
            Pause-Game 2
            $lines = @(Find-Log host '^\[CoopTest\] vehicle ' $m -All)
            $loud = @($lines | Where-Object { $_.Text -match ' hidden[;]' -and $_.Text -match 'run:playing vol' })
            Check ($lines.Count -gt 0) "$When`: $($lines.Count) vehicles listed"
            Check ($loud.Count -eq 0) ("$When`: hidden vehicles with an audible engine: $($loud.Count)" + $(if ($loud.Count) { " (" + (($loud | ForEach-Object { ($_.Text -split ' ')[2] }) -join ", ") + ")" } else { "" }))
        }
        Step "after Continue" { Pause-Game 20; Test-Hum "after Continue" }
        Step "after an in-place load" {
            Send-Game host "coop_god 1"
            $m = New-Mark
            Send-Game host "coop_load"
            Check ($null -ne (Wait-Log host "^\[Coop\] checkpoint loaded: |^\[CoopSave\] progress loaded: " 300 $m)) "the load ended"
            Pause-Game 30
            Test-Hum "30 s after the load"
        }
        Step "no helicopter hums in one place" {
            # a visible helicopter whose engine sound plays and that has not moved
            # in a minute: a hum that goes on and on (an AI helicopter stuck in the air)
            function Get-Helis {
                $m = New-Mark
                Send-Game host "coop_test_vehicles all"
                Pause-Game 2
                $out = @{}
                foreach ($l in @(Find-Log host '^\[CoopTest\] vehicle (\S+) (Asian_helicopter|US_vtol)\S* at \d+ m \((-?\d+) (-?\d+) (-?\d+)\)(?! hidden)(?! destroyed)' $m -All)) {
                    $g = $l.Match.Groups
                    $out[$g[1].Value] = @{ Pos = "$($g[3].Value) $($g[4].Value) $($g[5].Value)"; Loud = ($l.Text -match 'run:playing vol') }
                }
                return $out
            }
            $a = Get-Helis
            Pause-Game 60
            $b = Get-Helis
            $stuck = @($b.Keys | Where-Object { $a.ContainsKey($_) -and $a[$_].Pos -eq $b[$_].Pos -and $b[$_].Loud -and [int](($b[$_].Pos -split ' ')[2]) -gt 0 })
            Check $true ("visible helicopters: " + (($b.Keys | ForEach-Object { "$_ at $($b[$_].Pos)" + $(if ($b[$_].Loud) { " (engine heard)" } else { "" }) }) -join "; "))
            Check ($stuck.Count -eq 0) ("helicopters humming in one place for a minute: $($stuck.Count)" + $(if ($stuck.Count) { " (" + ($stuck -join ", ") + ")" } else { "" }))
        }
    }
}
