# The user's game of 2026-10-18 (0.11.41): the host drove field_jeep with the
# companion at its gun to the Rescue field huts, got out, the huts' soldiers
# shot him down with shotguns at 4-16 m (hp 101 -> 49 -> 17, down), 81 FMOD
# channels played, the music had just started - and for 15 s nothing at all
# came out, his own rifle neither, till the sound watch restarted the sound
# system. The cause was not in the log. Here the same ride and ambush, the
# host mortal (the companion revives him), sound to a WAV file, the watch
# metering FMOD's own mix (no Windows session to ask): a silence like the
# user's must show with FMOD's picture of it ("N s without sound").
@{
    Name = "sound-hut-ambush"
    Tags = @("sound", "companion", "combat")
    SummaryRu = "Как в игре 18.10: поездка на field_jeep к полевым хижинам, засада с дробовиками, хоста роняют - звук не должен пропадать (в WAV-файл)"
    Session = @{ Fixture = "new:rescue"; Companion = $true; Sound = "wav" }
    Params = @{ Jeep = "field_jeep"; Drop = @(1650, 2290); Fight = 90 }
    Body = {
        param($P)
        Step "the ride to the huts" {
            Send-Game host "coop_god 1"
            Send-Game host "coop_test_sound watch fmod"
            Pause-Game 15
            # (the jeep where the user got in, both of them by it)
            Send-Game host ('#local v=System.GetEntityByName("' + $P.Jeep + '"); local p=v:GetWorldPos(); g_gameRules.game:MovePlayer(g_localActorId,{x=p.x+3,y=p.y,z=p.z+1},{x=0,y=0,z=0}); local c=System.GetEntityByName("Companion"); g_gameRules.game:MovePlayer(c.id,{x=p.x-3,y=p.y+2,z=p.z+1},{x=0,y=0,z=0}); System.LogAlways(string.format("[CHK] by the jeep at %.0f %.0f", p.x, p.y))')
            Pause-Game 2
            $m = New-Mark
            Send-Game host "coop_test_enter $($P.Jeep) 1"
            $in = Wait-Log host 'Companion entered vehicle' 90 $m
            Require ($null -ne $in) "the companion got into the jeep"
            # (the jeep put on the way to the huts, facing them, 40 m before them;
            # driven a little; the huts' soldiers come out when a player is near)
            Send-Game host ('#local v=System.GetEntityByName("' + $P.Jeep + '"); local t={x=' + $P.Drop[0] + ',y=' + $P.Drop[1] + ',z=0}; t.z=System.GetTerrainElevation(t)+1.0; v:SetWorldPos(t); v:SetAngles({x=0,y=0,z=0}); System.LogAlways("[CHK] the jeep before the huts")')
            Pause-Game 2
            Send-Game host "coop_vehtest 2"
            $up = $false
            for ($i = 0; $i -lt 12 -and -not $up; $i++) {
                Pause-Game 3
                $mk = New-Mark
                Send-Game host '#local s=System.GetEntityByName("field_hut_2"); System.LogAlways("[CHK] huts hidden="..tostring(s and s:IsHidden()))'
                $l = Wait-Log host '^\[CHK\] huts hidden=' 5 $mk
                $up = $l -and $l.Text -match 'hidden=false'
            }
            Require $up "the huts' soldiers came out"
        }
        Step "out under the huts' fire" {
            $m = New-Mark
            # (the host out and mortal; the companion follows him out)
            Send-Game host ('#local v=System.GetEntityByName("' + $P.Jeep + '"); if v then v.vehicle:ExitVehicle(g_localActorId) end; System.LogAlways("[CHK] the host out")')
            Pause-Game 2
            Send-Game host "coop_god 0"
            $script:ambushMark = $m
            $end = (Get-Date).AddSeconds($P.Fight)
            while ((Get-Date) -lt $end) { Pause-Game 5 }
            Send-Game host "coop_god 1"
            Pause-Game 2
            $hits = Count-Log host '\] HIT player Nomad by ' $m
            $downs = Count-Log host '^\[Coop\] Nomad is down' $m
            $revived = Count-Log host '^\[Coop\] revived Nomad' $m
            $silent = @(Find-Log host '^\[CoopSound\] (\d+ s without sound|no sound came out|still no sound|the sound.s master group was|the sound.s mix is not|the heard sounds stand|sounds are heard and the mix is empty)' $m -All | ForEach-Object { $_.Text -replace '^\[CoopSound\] ', '' })
            $voices = @(Find-Log host '^\[CoopSound\] last minute: ' $m -All | ForEach-Object { if ($_.Text -match 'FMOD: (\d+) channels playing \((\d+) heard') { "$($Matches[1]) playing, $($Matches[2]) heard" } })
            Check $true "the host was hit $hits times, down $downs times, revived $revived times; FMOD per minute: $($voices -join ' | ')"
            Check ($hits -ge 5) "the huts' soldiers fought the host ($hits hits on him)"
            Check ($silent.Count -eq 0) ("no silence in the ambush" + $(if ($silent.Count) { ": " + (($silent | Select-Object -First 4) -join ' | ') } else { "" }))
        }
    }
}
