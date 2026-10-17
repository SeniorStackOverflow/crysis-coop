# The user's games of 2026-10-16/17 (0.11.37/38, the campaign "checking"
# continued): the AI companion at the LTV's gun never fired one shot. Its
# game hid the vehicles by itself while it loaded the level of a continued
# campaign, and the server told it only those hidden or shown otherwise than
# in the level file: this car, shown in the level file and on the server,
# stayed hidden there. The engine updates no hidden entity: the seat stayed
# "entering", the vehicle handed it no gun ("seat state 1", "gun (0.00 0.00
# 0.00)", powered=0). Here the same on a copy of that campaign: the car is
# shown in the companion's game, its seat is taken for real, it hits the
# soldiers round the car while the host drives.
@{
    Name = "companion-gunner-drive-off-continue-nosync"
    Tags = @("companion", "vehicle", "combat")
    SummaryRu = "Без синхронизации видимости машин (как 0.11.38): страховка у компаньона показывает скрытую машину. Как в игре 17.10 на кампании checking (продолжение): хост сразу трогается, компаньон садится до конца и стреляет"
    Session = @{ Fixture = "save:checking"; Companion = $true; Cvars = @{ coop_test_sync_vehicle_hide = 0 } }
    Params = @{ Vehicle = "Non_Driveable_Vehicles.Civ_Car8"; Phase = 60; MinHits = 3; SafetyNet = $true; EnterLines = 4 }
    Body = {
        param($P)
        $v = $P.Vehicle
        Step "stage the car and the soldiers ahead" {
            Send-Game host "coop_god 1"
            Send-Game host "v_goliathMode 1"
            Pause-Game 5
            Send-Game host ('#local v=System.GetEntityByName("' + $v + '"); local p=v:GetWorldPos(); CarAt=p; g_gameRules.game:MovePlayer(g_localActorId,{x=p.x+3,y=p.y,z=p.z+1},{x=0,y=0,z=0}); local c=System.GetEntityByName("Companion"); g_gameRules.game:MovePlayer(c.id,{x=p.x-4,y=p.y+4,z=p.z+1},{x=0,y=0,z=0}); System.LogAlways("[CHK] by the car where the save has it: "..math.floor(p.x).." "..math.floor(p.y))')
            # (whoever lives in the save near the car: the six nearest soldiers stand round it, 40 m off on ground level with the car (the LTV gun goes 18 degrees down at most), whichever way it goes, the rest are shot)
            Send-Game host ('#local p=CarAt or {x=1679,y=1805,z=194}; Spots={}; for j=0,35 do local a=j*0.1745; local t={x=p.x+math.cos(a)*40,y=p.y+math.sin(a)*40,z=0}; t.z=System.GetTerrainElevation(t); if math.abs(t.z-p.z)<4 and (#Spots==0 or j-Spots[#Spots].j>=4) then t.j=j; table.insert(Spots,t) end end; System.LogAlways("[CHK] spots level with the car: "..#Spots)')
            Send-Game host ('#local p=CarAt or {x=1679,y=1805,z=194}; local list={}; for _,e in ipairs(System.GetEntitiesInSphere(p,500) or {}) do if e.actor and e.ai and not e:IsHidden() and not e.actor:IsPlayer() and e.actor:GetHealth()>0 and not string.find(e:GetName(),"Prophet") and not string.find(e:GetName(),"Jester") and not string.find(e:GetName(),"Psycho") and not string.find(e:GetName(),"Aztec") then local d=e:GetWorldPos(); table.insert(list,{e=e,d=(d.x-p.x)^2+(d.y-p.y)^2}) end end; table.sort(list,function(a,b) return a.d<b.d end); local n,k=0,0; for i,it in ipairs(list) do local t=Spots and Spots[i]; if i<=6 and t then it.e:SetWorldPos({x=t.x,y=t.y,z=t.z+0.3}); n=n+1 else g_gameRules:CreateHit(it.e.id,g_localActorId,g_localActorId,9000,nil,nil,nil,"bullet"); k=k+1 end end; System.LogAlways("[CHK] soldiers round the car (40 m, level with it): "..n..", others shot: "..k)')
            Pause-Game 5
        }
        Step "the host drives off as soon as the companion is in" {
            $m = New-Mark
            # (how the companion's game updates the cars round it, before and after)
            Send-Game companion "coop_test_vehinfo 60"
            Send-Game host "coop_test_vehinfo 60"
            Send-Game host "coop_test_enter $v 1"
            $shown = Find-Log companion ('vehinfo: ' + [regex]::Escape($v) + ' \d+ m: entity active \d hidden (\d)') $m
            if ($P.SafetyNet) {
                Check $true "the car in the companion's game before it got in: $(if ($shown) { $shown.Text -replace '^.*vehinfo: ', '' } else { 'not logged' })"
            } else {
                Check ($shown -and $shown.Text -match 'hidden 0') "the car is shown in the companion's game before it gets in ($(if ($shown) { $shown.Text -replace '^.*vehinfo: ', '' } else { 'not logged' }))"
            }
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
            $hits = Count-Log host '\] HIT \S+ by Companion weapon=VehicleShiTenV2' $m
            $states = @(Find-Log companion 'gun aim at .*seat state (\d+)' $m -All)
            $entering = @($states | Where-Object { $_.Text -match 'seat state 1' }).Count
            $noGun = @($states | Where-Object { $_.Text -match 'gun \(0\.00 0\.00 0\.00\)' }).Count
            $seat = @(Find-Log companion '\] its seat \S+ of \S+: transition' $m -All | ForEach-Object { $_.Text -replace '^.*\] its seat ', '' })
            Check $true ("its seat's transitions: " + $(if ($seat.Count) { $seat -join '; ' } else { 'none logged' }))
            Check (-not ($seat | Where-Object { $_ -match '-> 1 ' }) -or ($seat | Where-Object { $_ -match '1 -> 0 \((\d+\.\d) s' -and [double]$Matches[1] -le 3 })) "its seat did not stay 'entering' (more than 3 s)"
            Check ($entering -le $P.EnterLines) "the companion's seat was 'entering' in $entering of $($states.Count) aim lines"
            Check ($noGun -le $P.EnterLines) "the companion had no gun in $noGun of $($states.Count) aim lines"
            Check ($hits -ge $P.MinHits) "the companion hit the soldiers $hits times from the gun (from when it got in)"
            $net = Find-Log companion 'was hidden in its own game for 2 s while it rode in it: shown' $m
            if ($P.SafetyNet) {
                Check ($null -ne $net) "the companion's game showed the car it rode in, hidden there (the safety net)"
            } else {
                Check ($null -eq $net) "no car had to be shown by the safety net (the server showed it)"
            }
        }
    }
}
