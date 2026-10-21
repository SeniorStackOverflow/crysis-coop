# The user's game of 2026-10-18 (0.11.41): in the first ride the companion's
# copy of the jeep stayed where it got in for the whole ride; its gun saw the
# machine gun nest 171 m off (the host's 46 m) and never fired. The host
# numbered every vehicle state from one 16-bit counter that went up for
# every parked vehicle 15-20 times a second: after a minute or two it had
# wrapped half way and the client took every state of a car that had rested
# for "older". Here the same jump of the numbers (coop_test_vehseq_skip
# 40000) just before the host drives off with the companion at the gun: its
# copy of the car must follow the server's.
@{
    Name = "companion-gunner-frozen-copy"
    Tags = @("companion", "vehicle", "sync")
    SummaryRu = "Номера состояний машины прыгнули (как после долгой стоянки): копия машины у компаньона всё равно едет за машиной хоста"
    Session = @{ Fixture = "new:rescue"; Companion = $true }
    Params = @{ Vehicle = "Land.Chinese_4wd1"; Trace = "Chinese_4wd1"; Skip = 40000 }
    Body = {
        param($P)
        $v = $P.Vehicle
        Step "stage the car, the companion at its gun" {
            Send-Game host "coop_god 1"
            Send-Game host "v_goliathMode 1"
            Pause-Game 5
            Send-Game host ('#local v=System.GetEntityByName("' + $v + '"); local p={x=1737,y=1960,z=0}; p.z=System.GetTerrainElevation(p)+1.2; v:SetWorldPos(p); v:SetAngles({x=0,y=0,z=0}); g_gameRules.game:MovePlayer(g_localActorId,{x=p.x+3,y=p.y,z=p.z+1},{x=0,y=0,z=0}); local c=System.GetEntityByName("Companion"); g_gameRules.game:MovePlayer(c.id,{x=p.x-3,y=p.y,z=p.z+1},{x=0,y=0,z=0}); local k=0; for _,e in ipairs(System.GetEntitiesInSphere(p,400) or {}) do if e.actor and not e.actor:IsPlayer() and e.actor:GetHealth()>0 then g_gameRules:CreateHit(e.id,g_localActorId,g_localActorId,9000,nil,nil,nil,"bullet"); k=k+1 end end; System.LogAlways("[CHK] car by the bridge, soldiers shot: "..k)')
            Pause-Game 3
            $m = New-Mark
            Send-Game host "coop_test_enter $v 1"
            $in = Wait-Log host 'Companion entered vehicle' 90 $m
            Require ($null -ne $in) "the companion got into the car"
            # (the car rests a while, as before the user's ride)
            Pause-Game 10
        }
        Step "the numbers jump, the host drives" {
            Send-Game host "coop_debug_physdump $($P.Trace)"
            Send-Game companion "coop_debug_physdump $($P.Trace)"
            $m = New-Mark
            # (the jump while the car drives: states 20 a second, the new ones
            # refused as older for a second, then taken)
            Send-Game host "coop_vehtest 6"
            Pause-Game 2
            Send-Game host "coop_test_vehseq_skip $($P.Skip)"
            Pause-Game 5
            Send-Game host "coop_vehtest 6"
            Pause-Game 8
            Send-Game host "coop_debug_physdump """""
            $skip = Find-Log host '^\[CoopTest\] coop_test_vehseq_skip: ' $m
            $refused = Count-Log companion '^\[CoopAI\] vehicle \S+: state seq \d+ refused as older' $m
            $taken = Find-Log companion '^\[CoopAI\] vehicle \S+: state seq \d+ taken although older' $m
            # (each state the companion's game got: where the server has the car, where its copy is)
            $states = @(Find-Log companion ('^\[CoopVeh<\] \S*' + $P.Trace + ' seq=\d+ \(last \d+\) pos=\(([-\d.]+),([-\d.]+),([-\d.]+)\) here=\(([-\d.]+),([-\d.]+),([-\d.]+)\)') $m -All)
            $offs = @($states | ForEach-Object { if ($_.Text -match 'pos=\(([-\d.]+),([-\d.]+),[-\d.]+\) here=\(([-\d.]+),([-\d.]+),[-\d.]+\)') { [Math]::Sqrt([Math]::Pow([double]$Matches[1] - [double]$Matches[3], 2) + [Math]::Pow([double]$Matches[2] - [double]$Matches[4], 2)) } })
            $close = @($offs | Where-Object { $_ -le 6 }).Count
            $far = if ($offs.Count) { ($offs | Measure-Object -Maximum).Maximum } else { -1 }
            $moved = @(Find-Log host '^\[CoopVeh>\] \S+ seq=\d+ pos=' $m -All).Count
            Check ($null -ne $skip) "the host's numbers jumped ($(if ($skip) { $skip.Text -replace '^.*skip: ', '' } else { 'not logged' }))"
            Check ($moved -ge 10) "the host sent the car's states while it drove ($moved)"
            Check ($null -ne $taken -and $refused -ge 1) "the companion's game refused the 'older' states for a second, then took them again ($refused refused; $(if ($taken) { $taken.Text -replace '^\[CoopAI\] ', '' } else { 'never taken' }))"
            Check ($offs.Count -ge 10 -and $close -ge [Math]::Ceiling(0.85 * $offs.Count)) ("the companion's copy of the car followed the server's: {0} of {1} states within 6 m (the farthest {2:N1} m)" -f $close, $offs.Count, $far)
        }
    }
}
