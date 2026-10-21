# The user's game of 2026-10-18 (0.11.41): on the host the companion's
# vehicle gun and its body never turned to where it fired; the bullets went
# out sideways from a gun fixed to the car. CryAction carries a client
# gunner's turret turn only through a seat serializer entity the coop
# server's level (loaded in single player mode) never gets. Here: the car by
# the Rescue bridge turns all the time, soldiers 50 m ahead, the companion at
# its gun; every hit of the companion's on the server is compared with where
# the server's copy of the gun points ([CoopShotDir], coop_test_shottrace),
# and the server's gun part with what the gunner sent ([CoopTurret]).
@{
    Name = "companion-gunner-turret-seen-off"
    Tags = @("companion", "vehicle", "combat", "sync")
    SummaryRu = "То же без синхронизации поворота (coop_turret_apply 0): тест должен видеть ошибку - ствол на хосте не смотрит туда, куда летят пули"
    Session = @{ Fixture = "new:rescue"; Companion = $true }
    Params = @{ Vehicle = "Land.Chinese_4wd1"; Phase = 50; Spin = 20; MinHits = 3; Apply = 0; Expect = "fixed" }
    Body = {
        param($P)
        $v = $P.Vehicle
        Step "stage the car and the soldiers" {
            Send-Game host "coop_god 1"
            Send-Game host "v_goliathMode 1"
            Send-Game host "coop_test_shottrace 1"
            Send-Game host "coop_turret_apply $($P.Apply)"
            Pause-Game 5
            Send-Game host ('#local v=System.GetEntityByName("' + $v + '"); local p={x=1737,y=1960,z=0}; p.z=System.GetTerrainElevation(p)+1.2; v:SetWorldPos(p); v:SetAngles({x=0,y=0,z=0}); g_gameRules.game:MovePlayer(g_localActorId,{x=p.x+3,y=p.y,z=p.z+1},{x=0,y=0,z=0}); local c=System.GetEntityByName("Companion"); g_gameRules.game:MovePlayer(c.id,{x=p.x-3,y=p.y,z=p.z+1},{x=0,y=0,z=0}); System.LogAlways("[CHK] car on the road by the bridge")')
            Send-Game host ('#local v=System.GetEntityByName("' + $v + '"); local p=v:GetWorldPos(); local d=v:GetDirectionVector(1); local keep={}; local n=0; for i=1,30 do local e=System.GetEntityByName("Bridge_"..i); if e and e.actor and e.actor:GetHealth()>0 and n<6 then local t={x=p.x+d.x*50+(n-2.5)*4*d.y,y=p.y+d.y*50-(n-2.5)*4*d.x,z=0}; t.z=System.GetTerrainElevation(t)+0.3; e:SetWorldPos(t); keep[e.id]=true; n=n+1 end end; local k=0; for _,e in ipairs(System.GetEntitiesInSphere(p,400) or {}) do if e.actor and not e.actor:IsPlayer() and e.actor:GetHealth()>0 and not keep[e.id] then g_gameRules:CreateHit(e.id,g_localActorId,g_localActorId,9000,nil,nil,nil,"bullet"); k=k+1 end end; System.LogAlways("[CHK] soldiers ahead: "..n..", others shot: "..k)')
            Pause-Game 3
            $m = New-Mark
            Send-Game host "coop_test_enter $v 1"
            $in = Wait-Log host 'Companion entered vehicle' 90 $m
            Require ($null -ne $in) "the companion got into the car"
            Pause-Game 3
            Send-Game host "coop_test_parts $v"
            Send-Game companion "coop_test_parts $v"
        }
        Step "the car turns, the companion fires" {
            Send-Game host ('#SpinOn=true; local v=System.GetEntityByName("' + $v + '"); function SpinTick() if not SpinOn then return end local a=v:GetAngles(); v:SetAngles({x=a.x,y=a.y,z=a.z+' + ($P.Spin * 0.1 * [Math]::PI / 180) + '}); Script.SetTimer(100,SpinTick) end; SpinTick(); System.LogAlways("[CHK] the car turns")')
            $m = New-Mark
            Pause-Game ($P.Phase / 2)
            Send-Game host "coop_test_parts $v"
            Send-Game companion "coop_test_parts $v"
            Pause-Game ($P.Phase / 2)
            Send-Game host '#SpinOn=false; System.LogAlways("[CHK] spin off")'
            $hits = Count-Log host '\] HIT [A-Za-z_]+[0-9]* by Companion ' $m
            $dirs = @(Find-Log host '^\[CoopShotDir\] Companion hit \S+: the shot (\d+) deg off the server copy''s gun part' $m -All | ForEach-Object { if ($_.Text -match 'the shot (\d+) deg off') { [int]$Matches[1] } })
            $sorted = @($dirs | Sort-Object)
            $median = if ($sorted.Count) { $sorted[[Math]::Floor($sorted.Count / 2)] } else { -1 }
            $near = @($dirs | Where-Object { $_ -le 15 }).Count
            $turret = @(Find-Log host '^\[CoopTurret\] ' $m -All | ForEach-Object { $_.Text })
            $agree = 0
            foreach ($t in $turret) {
                if ($t -match 'server yaw (-?\d+) pitch (-?\d+), the gunner sent yaw (-?\d+) pitch (-?\d+)') {
                    $dy = [Math]::Abs([int]$Matches[1] - [int]$Matches[3]); if ($dy -gt 180) { $dy = 360 - $dy }
                    if ($dy -le 5 -and [Math]::Abs([int]$Matches[2] - [int]$Matches[4]) -le 5) { $agree++ }
                }
            }
            # (logged at the first one, when it sat down)
            $recv = Count-Log host '^\[CoopAI\] gun turns received: '
            Check ($hits -ge $P.MinHits) "the companion hit enemies from the gun of a turning car: $hits hits"
            Check $true ("the shots against the server's gun: " + $(if ($dirs.Count) { "median $median deg, $near of $($dirs.Count) within 15 deg; " + (($dirs | Select-Object -First 12) -join ' ') } else { 'no line' }))
            Check $true ("the server's gun part against the gunner's: $agree of $($turret.Count) within 5 deg" + $(if ($turret.Count) { "; e.g. " + (($turret | Select-Object -First 2) -replace '^\[CoopTurret\] ', '') -join ' | ' } else { "" }))
            if ($P.Expect -eq "follows") {
                Check ($recv -ge 1) "the server got the gunner's turns ($recv log lines)"
                Check ($dirs.Count -ge 3 -and $near -ge [Math]::Ceiling(0.8 * $dirs.Count)) "on the host the gun points where its shots go ($near of $($dirs.Count) hits within 15 deg, median $median)"
                Check ($turret.Count -ge 2 -and $agree -ge [Math]::Ceiling(0.6 * $turret.Count)) "the server's gun part follows the gunner's ($agree of $($turret.Count) within 5 deg)"
            } else {
                Check ($dirs.Count -ge 3 -and $median -gt 20) "without the sync the host's gun stays with the car (median $median deg off the shots; the measurement sees the fault)"
            }
        }
    }
}
