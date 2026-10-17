# The user's game of 2026-10-17 on 0.11.39 (the campaign "checking"
# continued): the companion sat down at the LTV's gun for real (gun in hand)
# but never fired: while the host steered, the way its rotate actions turn
# the gun was learnt wrong ("signs 1 -1, votes 4 -4" after 3 s), the gun went
# the wrong way, 0 bursts. Here on a copy of that campaign, everybody and the
# car immortal: the host circles round the soldiers, then weaves left and
# right; the companion has to keep on them and hit them.
@{
    Name = "companion-gunner-weave-continue"
    Tags = @("companion", "vehicle", "combat")
    SummaryRu = "Хост кружит и петляет на джипе вокруг солдат (как в игре 17.10 на 0.11.39), все бессмертны: компаньон держит пулемёт на врагах и попадает"
    Session = @{ Fixture = "save:checking"; Companion = $true }
    Params = @{ Vehicle = "Non_Driveable_Vehicles.Civ_Car8"; GunTrace = $true; MinHits = 5; SafetyNet = $false; EnterLines = 2 }
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
            if ($P.GunTrace) { Send-Game companion "coop_test_guntrace 1" }
            if ($P.CompanionCvars) { foreach ($c in $P.CompanionCvars) { Send-Game companion $c } }
            Send-Game host "coop_test_enter $v 1"
            $shown = Find-Log companion ('vehinfo: ' + [regex]::Escape($v) + ' \d+ m: entity active \d hidden (\d)') $m
            if ($P.SafetyNet) {
                Check $true "the car in the companion's game before it got in: $(if ($shown) { $shown.Text -replace '^.*vehinfo: ', '' } else { 'not logged' })"
            } else {
                Check ($shown -and $shown.Text -match 'hidden 0') "the car is shown in the companion's game before it gets in ($(if ($shown) { $shown.Text -replace '^.*vehinfo: ', '' } else { 'not logged' }))"
            }
            $in = Wait-Log host 'Companion entered vehicle' 90 $m
            Require ($null -ne $in) "the companion got into the car"
            # (at once, as the user does): round and round the soldiers, then
            # left and right in turn
            Send-Game host "coop_vehtest 30 100"
            Pause-Game 3
            Send-Game companion "coop_test_vehinfo 60"
            Pause-Game 28
            $mCircle = New-Mark
            Send-Game host "coop_vehtest 30 1.5"
            Pause-Game 31
            $signs = @(Find-Log companion 'gun aim at .*\(signs (-?1) (-?1), votes' $m -All | ForEach-Object { if ($_.Text -match 'signs (-?1) (-?1)') { $Matches[1] + ' ' + $Matches[2] } })
            $groups = ($signs | Group-Object | Sort-Object Count -Descending | ForEach-Object { "$($_.Name): $($_.Count)" }) -join ', '
            Check $true "the turn signs it used (yaw pitch: aim lines): $groups"
            $bursts = @(Find-Log companion 'the last minute: (\d+) bursts fired' $m -All | ForEach-Object { if ($_.Text -match 'minute: (\d+) bursts fired; no shot: (\d+) frames something in the way, (\d+) enemy not in sight, (\d+) turning') { "$($Matches[1]) bursts (turning $($Matches[4]) frames)" } })
            Check $true "bursts: $($bursts -join '; ')"
            $hitsCircle = Count-Log host '\] HIT \S+ by Companion weapon=VehicleShiTenV2' $m
            Check $true "hits while circling: $($hitsCircle - (Count-Log host '\] HIT \S+ by Companion weapon=VehicleShiTenV2' $mCircle))"
            $hits = Count-Log host '\] HIT \S+ by Companion weapon=VehicleShiTenV2' $m
            $states = @(Find-Log companion 'gun aim at .*seat state (\d+)' $m -All)
            $entering = @($states | Where-Object { $_.Text -match 'seat state 1' }).Count
            $noGun = @($states | Where-Object { $_.Text -match 'gun \(0\.00 0\.00 0\.00\)' }).Count
            $seat = @(Find-Log companion '\] its seat \S+ of \S+: transition' $m -All | ForEach-Object { $_.Text -replace '^.*\] its seat ', '' })
            Check $true ("its seat's transitions: " + $(if ($seat.Count) { $seat -join '; ' } else { 'none logged' }))
            Check (-not ($seat | Where-Object { $_ -match '-> 1 ' }) -or ($seat | Where-Object { $_ -match '1 -> 0 \((\d+\.\d) s' -and [double]$Matches[1] -le 3 })) "its seat did not stay 'entering' (more than 3 s)"
            Check ($entering -le $P.EnterLines) "the companion's seat was 'entering' in $entering of $($states.Count) aim lines"
            Check ($noGun -le $P.EnterLines) "the companion had no gun in $noGun of $($states.Count) aim lines"
            Check ($hits -ge $P.MinHits) "the companion hit the soldiers $hits times from the gun while the host circled and weaved"
            $net = Find-Log companion 'was hidden in its own game for 2 s while it rode in it: shown' $m
            if ($P.SafetyNet) {
                Check ($null -ne $net) "the companion's game showed the car it rode in, hidden there (the safety net)"
            } else {
                Check ($null -eq $net) "no car had to be shown by the safety net (the server showed it)"
            }
        }
    }
}
