# The user's game of 2026-10-15: the AI companion at the gun of the car the
# host drove aimed and never fired. Here: the car by the Rescue bridge, four
# soldiers 50 m ahead, the host drives (seat 1), the companion at the gun;
# phase 1 the car stands, phase 2 it turns all the time.
@{
    Name = "companion-gunner-hits"
    Tags = @("companion", "vehicle", "combat")
    SummaryRu = "ИИ-компаньон за пулемётом машины попадает по врагам (машина стоит и поворачивает)"
    Session = @{ Fixture = "new:rescue"; Companion = $true }
    Params = @{ Vehicle = "Land.Chinese_4wd1"; Phase = 40; Spin = 20; MinHits = 3 }
    Body = {
        param($P)
        $v = $P.Vehicle
        Step "stage the car and the soldiers" {
            Send-Game host "coop_god 1"
            Pause-Game 5
            Send-Game host ('#local v=System.GetEntityByName("' + $v + '"); local p={x=1737,y=1960,z=0}; p.z=System.GetTerrainElevation(p)+1.2; v:SetWorldPos(p); v:SetAngles({x=0,y=0,z=0}); g_gameRules.game:MovePlayer(g_localActorId,{x=p.x+3,y=p.y,z=p.z+1},{x=0,y=0,z=0}); local c=System.GetEntityByName("Companion"); g_gameRules.game:MovePlayer(c.id,{x=p.x-3,y=p.y,z=p.z+1},{x=0,y=0,z=0}); System.LogAlways("[CHK] car on the road by the bridge")')
            Send-Game host ('#local v=System.GetEntityByName("' + $v + '"); local p=v:GetWorldPos(); local d=v:GetDirectionVector(1); local keep={}; local n=0; CompGunSecond={}; for i=1,30 do local e=System.GetEntityByName("Bridge_"..i); if e and e.actor and e.actor:GetHealth()>0 and n<8 then local t; if n<4 then t={x=p.x+d.x*50+(n-1.5)*4*d.y,y=p.y+d.y*50-(n-1.5)*4*d.x,z=0} else t={x=p.x-d.x*260+(n-5.5)*4,y=p.y-d.y*260,z=0}; table.insert(CompGunSecond,e.id) end; t.z=System.GetTerrainElevation(t)+0.3; e:SetWorldPos(t); keep[e.id]=true; n=n+1 end end; local k=0; for _,e in ipairs(System.GetEntitiesInSphere(p,400) or {}) do if e.actor and not e.actor:IsPlayer() and e.actor:GetHealth()>0 and not keep[e.id] then g_gameRules:CreateHit(e.id,g_localActorId,g_localActorId,9000,nil,nil,nil,"bullet"); k=k+1 end end; System.LogAlways("[CHK] soldiers ahead: "..math.min(n,4)..", kept far: "..#CompGunSecond..", others shot: "..k)')
            Pause-Game 3
            $m = New-Mark
            Send-Game host "coop_test_enter $v 1"
            $in = Wait-Log host 'Companion entered vehicle' 90 $m
            Require ($null -ne $in) "the companion got into the car"
        }
        Step "phase 1: the car stands" {
            $m = New-Mark
            Pause-Game $P.Phase
            $n = Count-Log host '\] HIT [A-Za-z_]+[0-9]* by Companion ' $m
            Check ($n -ge $P.MinHits) "the companion hit enemies from the gun of a standing car: $n hits"
        }
        Step "phase 2: the car turns" {
            Send-Game host ('#local v=System.GetEntityByName("' + $v + '"); local p=v:GetWorldPos(); local d=v:GetDirectionVector(1); local n=0; for _,id in ipairs(CompGunSecond or {}) do local e=System.GetEntity(id); if e and e.actor:GetHealth()>0 then local t={x=p.x+d.x*50+(n-1.5)*4*d.y,y=p.y+d.y*50-(n-1.5)*4*d.x,z=0}; t.z=System.GetTerrainElevation(t)+0.3; e:SetWorldPos(t); n=n+1 end end; System.LogAlways("[CHK] phase 2 soldiers ahead: "..n)')
            Send-Game host ('#SpinOn=true; local v=System.GetEntityByName("' + $v + '"); function SpinTick() if not SpinOn then return end local a=v:GetAngles(); v:SetAngles({x=a.x,y=a.y,z=a.z+' + ($P.Spin * 0.1 * [Math]::PI / 180) + '}); Script.SetTimer(100,SpinTick) end; SpinTick(); System.LogAlways("[CHK] the car turns")')
            $m = New-Mark
            Pause-Game $P.Phase
            Send-Game host '#SpinOn=false; System.LogAlways("[CHK] spin off")'
            $n = Count-Log host '\] HIT [A-Za-z_]+[0-9]* by Companion ' $m
            Check ($n -ge $P.MinHits) "the companion hit enemies from the gun of a turning car: $n hits"
        }
    }
}
