# The review of the 0.11.39 fix: the companion's game shows the vehicle it
# rides in when its game keeps it hidden by itself (a continued campaign);
# not one the server hid (the story disables the vehicle it sits in, as for
# everybody). Here the host hides the car the companion sits in: hidden in
# the companion's game too, and it stays so (no "shown" by the safety net);
# shown again on the host, shown there again.
@{
    Name = "companion-vehicle-story-hide"
    Tags = @("companion", "vehicle", "sync")
    SummaryRu = "Хост по сюжету скрывает машину, в которой сидит компаньон: у компаньона она тоже скрыта и страховка её не показывает"
    Session = @{ Fixture = "new:rescue"; Companion = $true }
    Params = @{ Vehicle = "Non_Driveable_Vehicles.Civ_Car8" }
    Body = {
        param($P)
        $v = $P.Vehicle
        Step "the companion at the car's gun" {
            Send-Game host "coop_god 1"
            Send-Game host "v_goliathMode 1"
            Pause-Game 5
            Send-Game host ('#local v=System.GetEntityByName("' + $v + '"); local p={x=1737,y=1940,z=0}; p.z=System.GetTerrainElevation(p)+1.2; v:SetWorldPos(p); v:SetAngles({x=0,y=0,z=0}); g_gameRules.game:MovePlayer(g_localActorId,{x=p.x+3,y=p.y,z=p.z+1},{x=0,y=0,z=0}); local c=System.GetEntityByName("Companion"); g_gameRules.game:MovePlayer(c.id,{x=p.x-4,y=p.y-3,z=p.z+1},{x=0,y=0,z=0}); System.LogAlways("[CHK] car on the road")')
            Pause-Game 5
            $m = New-Mark
            Send-Game host "coop_test_enter $v 1"
            $in = Wait-Log companion ('its seat \S+ of ' + [regex]::Escape($v) + ': transition -?\d+ -> 0') 90 $m
            Require ($null -ne $in) "the companion sat down at the car's gun"
        }
        Step "the host hides the car (as the story's Disable does)" {
            $m = New-Mark
            Send-Game host ('#local v=System.GetEntityByName("' + $v + '"); v:Hide(1); System.LogAlways("[CHK] the car hidden on the host")')
            Pause-Game 8
            Send-Game companion "coop_test_vehinfo 10"
            Pause-Game 2
            $info = Find-Log companion ('vehinfo: ' + [regex]::Escape($v) + ' \d+ m: entity active \d hidden (\d)') $m
            Check ($info -and $info.Text -match 'hidden 1') "8 s later the car is hidden in the companion's game too ($(if ($info) { $info.Text -replace '^.*vehinfo: ', '' } else { 'not logged' }))"
            $net = Find-Log companion 'was hidden in its own game for 2 s while it rode in it: shown' $m
            Check ($null -eq $net) "the companion's safety net left the car the server hid alone"
        }
        Step "the host shows the car again" {
            $m = New-Mark
            Send-Game host ('#local v=System.GetEntityByName("' + $v + '"); v:Hide(0); System.LogAlways("[CHK] the car shown on the host")')
            Pause-Game 5
            Send-Game companion "coop_test_vehinfo 10"
            Pause-Game 2
            $info = Find-Log companion ('vehinfo: ' + [regex]::Escape($v) + ' \d+ m: entity active \d hidden (\d)') $m
            Check ($info -and $info.Text -match 'hidden 0') "shown in the companion's game again ($(if ($info) { $info.Text -replace '^.*vehinfo: ', '' } else { 'not logged' }))"
        }
    }
}
