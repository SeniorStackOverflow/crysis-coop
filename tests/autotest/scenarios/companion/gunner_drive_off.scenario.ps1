# The user's game of 2026-10-17 (0.11.38): eight rides with the AI companion
# at the LTV's gun, never one shot. The host drove off the moment the
# companion was in; in the companion's game its seat stayed "entering" for the
# whole ride ("seat state 1", "gun (0.00 0.00 0.00)": no weapon), the turret
# turned, nothing fired. Here the same: soldiers ahead, the host drives off
# right as the companion gets in; it has to be seated for real and hit them.
@{
    Name = "companion-gunner-drive-off"
    Tags = @("companion", "vehicle", "combat")
    SummaryRu = "Хост трогается сразу, как компаньон сел за пулемёт (как в игре 17.10): компаньон садится до конца и стреляет"
    Session = @{ Fixture = "new:rescue"; Companion = $true }
    Params = @{ Vehicle = "Non_Driveable_Vehicles.Civ_Car8"; Phase = 60; MinHits = 3 }
    Body = {
        param($P)
        $v = $P.Vehicle
        Step "stage the car and the soldiers ahead" {
            Send-Game host "coop_god 1"
            Send-Game host "v_goliathMode 1"
            Pause-Game 5
            Send-Game host ('#local v=System.GetEntityByName("' + $v + '"); local p={x=1737,y=1940,z=0}; p.z=System.GetTerrainElevation(p)+1.2; v:SetWorldPos(p); v:SetAngles({x=0,y=0,z=0}); g_gameRules.game:MovePlayer(g_localActorId,{x=p.x+3,y=p.y,z=p.z+1},{x=0,y=0,z=0}); local c=System.GetEntityByName("Companion"); g_gameRules.game:MovePlayer(c.id,{x=p.x-5,y=p.y-4,z=p.z+1},{x=0,y=0,z=0}); System.LogAlways("[CHK] car on the road")')
            Send-Game host ('#local p={x=1737,y=1940,z=0}; local keep={}; local n=0; for i=1,30 do local e=System.GetEntityByName("Bridge_"..i); if e and e.actor and e.actor:GetHealth()>0 and n<6 then local t={x=p.x+(n%2==0 and -14 or 14),y=p.y+40+n*12,z=0}; t.z=System.GetTerrainElevation(t)+0.3; e:SetWorldPos(t); keep[e.id]=true; n=n+1 end end; local k=0; for _,e in ipairs(System.GetEntitiesInSphere(p,400) or {}) do if e.actor and not e.actor:IsPlayer() and e.actor:GetHealth()>0 and not keep[e.id] then g_gameRules:CreateHit(e.id,g_localActorId,g_localActorId,9000,nil,nil,nil,"bullet"); k=k+1 end end; System.LogAlways("[CHK] soldiers along the road: "..n..", others shot: "..k)')
            Pause-Game 5
        }
        Step "the host drives off as soon as the companion is in" {
            $m = New-Mark
            # (how the companion's game updates the cars round it, before and after)
            Send-Game companion "coop_test_vehinfo 60"
            Send-Game host "coop_test_enter $v 1"
            $in = Wait-Log host 'Companion entered vehicle' 90 $m
            Require ($null -ne $in) "the companion got into the car"
            # (at once, as the user does)
            Send-Game host "coop_vehtest 3"
            Pause-Game 3
            Send-Game companion "coop_test_vehinfo 60"
            $m2 = New-Mark
            $end = (Get-Date).AddSeconds($P.Phase)
            while ((Get-Date) -lt $end) {
                Pause-Game 8
                Send-Game host "coop_vehtest 2"
            }
            $hits = Count-Log host '\] HIT (Bridge_\d+|vehicle \S+) by Companion weapon=VehicleShiTenV2' $m2
            $states = @(Find-Log companion 'gun aim at .*seat state (\d+)' $m -All)
            $entering = @($states | Where-Object { $_.Text -match 'seat state 1' }).Count
            $noGun = @($states | Where-Object { $_.Text -match 'gun \(0\.00 0\.00 0\.00\)' }).Count
            Check ($entering -le 2) "the companion's seat was 'entering' in $entering of $($states.Count) aim lines"
            Check ($noGun -le 2) "the companion had no gun in $noGun of $($states.Count) aim lines"
            Check ($hits -ge $P.MinHits) "the companion hit the soldiers $hits times from the gun while the host drove"
        }
    }
}
