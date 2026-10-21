# The user's game of 2026-10-18 (0.11.41): all sound gone for 15 s in a
# firefight with 81 channels playing, until the watch restarted the sound
# system; the Windows meter alone could not say why. The watch now also
# looks at FMOD's own picture (the final mix, the master channel group, the
# heard channels): a master group paused or muted behind the game's back is
# set right at once, a second time within 20 s restarts the sound system;
# and in a normal firefight nothing of the kind may fire. Sound to a WAV
# file only.
@{
    Name = "sound-master-fault"
    Tags = @("sound")
    SummaryRu = "Звук: главный канал FMOD поставлен на паузу или заглушён (как при сбое) - сторож сразу возвращает звук; в обычном бою ложных срабатываний нет"
    Session = @{ Fixture = "new:rescue"; Companion = $false; Sound = "wav" }
    Params = @{ Quiet = 60 }
    Body = {
        param($P)
        Step "a firefight round the host, the watch on" {
            Send-Game host "coop_god 1"
            Send-Game host "coop_test_sound watch"
            Pause-Game 15
            Send-Game host '#local p=System.GetEntityByName("Nomad"):GetWorldPos(); local n=0; for i=1,30 do local e=System.GetEntityByName("Bridge_"..i); if e and e.actor and e.actor:GetHealth()>0 and n<3 then local t={x=p.x+20+n*3,y=p.y+15,z=0}; t.z=System.GetTerrainElevation(t)+0.3; e:SetWorldPos(t); n=n+1 end end; System.LogAlways("[CHK] soldiers by the host: "..n)'
            Pause-Game 10
            $m = New-Mark
            Send-Game host "coop_test_sound snapshot"
            $snap = Wait-Log host '^\[CoopTest\] coop_test_sound snapshot: ' 5 $m
            Check ($null -ne $snap -and $snap.Text -match 'mix peak') ("FMOD's picture: " + $(if ($snap) { $snap.Text -replace '^.*snapshot: ', '' } else { 'none' }))
        }
        Step "a normal firefight: no false fault" {
            $m = New-Mark
            Pause-Game $P.Quiet
            $false1 = @(Find-Log host '^\[CoopSound\] (the sound.s master group was|the sound.s mix is not|the heard sounds stand|sounds are heard and the mix is empty|.*starts again)' $m -All | ForEach-Object { $_.Text })
            $shots = Count-Log host '\] HIT player Nomad by Bridge_' $m
            Check $true "the soldiers hit the host $shots times in $($P.Quiet) s"
            Check ($false1.Count -eq 0) ("no fault and no restart in a normal firefight" + $(if ($false1.Count) { ": " + (($false1 | Select-Object -First 3) -join ' | ') } else { "" }))
        }
        Step "FMOD plays, nothing comes out of the game (as the user's 15 s)" {
            $m = New-Mark
            Send-Game host "coop_test_sound blackout 8"
            $restart = Wait-Log host '^\[CoopSound\] FMOD plays \d+ audible sounds and nothing comes out of this game for 4 s: the sound system starts again' 9 $m
            Check ($null -ne $restart) ("heard in FMOD, silent in Windows: the sound system restarted within 9 s" + $(if ($restart) { ": " + (($restart.Text -replace '^\[CoopSound\] ', '').Substring(0, [Math]::Min(160, ($restart.Text -replace '^\[CoopSound\] ', '').Length))) } else { "" }))
            Pause-Game 6
        }
        Step "paused while the window has no focus: left alone (CrySound's own focus pause)" {
            $m = New-Mark
            Send-Game host "coop_test_sound fmodpause unfocused"
            Pause-Game 6
            $early = Find-Log host '^\[CoopSound\] the sound.s master group was paused' $m
            Check ($null -eq $early) ("no repair while the window has no focus" + $(if ($early) { ": " + ($early.Text -replace '^\[CoopSound\] ', '') } else { "" }))
            # (the focus back after 8 s: in devmode CrySound does not unpause it itself, the watch does)
            $back = Wait-Log host '^\[CoopSound\] the sound.s master group was paused behind the game.s back: set right' 8 $m
            Check ($null -ne $back) "with the focus back the paused master group is set right"
        }
        Step "the master group paused, then muted, as by a fault" {
            Pause-Game 22
            $m = New-Mark
            Send-Game host "coop_test_sound fmodpause"
            $fix1 = Wait-Log host '^\[CoopSound\] the sound.s master group was paused behind the game.s back: set right' 6 $m
            Check ($null -ne $fix1) ("a paused master group set right within 6 s" + $(if ($fix1) { ": " + ($fix1.Text -replace '^\[CoopSound\] ', '') } else { "" }))
            # (again within 20 s: the sound system starts again)
            Pause-Game 3
            $m3 = New-Mark
            Send-Game host "coop_test_sound fmodmute"
            $again = Wait-Log host '^\[CoopSound\] the master group was set wrong again within 20 s: the sound system starts again' 6 $m3
            $muted = Find-Log host '^\[CoopSound\] the sound.s master group was muted behind the game.s back' $m3
            Check ($null -ne $muted) ("a muted master group found" + $(if ($muted) { ": " + ($muted.Text -replace '^\[CoopSound\] ', '') } else { "" }))
            Check ($null -ne $again) "set wrong again within 20 s: the sound system restarted"
            Pause-Game 5
            Send-Game host "coop_test_sound snapshot"
            $snap = Wait-Log host '^\[CoopTest\] coop_test_sound snapshot: ' 5 $m3
            Check ($null -ne $snap -and $snap.Text -cnotmatch 'PAUSED|MUTED') ("after it: " + $(if ($snap) { $snap.Text -replace '^.*snapshot: ', '' } else { 'no picture' }))
        }
    }
}
