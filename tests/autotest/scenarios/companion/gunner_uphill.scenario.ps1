# The user's game of 2026-10-16: twice the AI companion sat at the gun of the
# LTV the host drove (Rescue, the slope below the bridge) and never fired: the
# soldiers stood 50-60 m off and 12-15 m higher up the hill, and the gun hung
# about 0.3 rad off them ("gun aim at ...: yaw off 0.34 pitch off 0.25", the
# view not moving). Here: the car where the host had it (1724 1939), soldiers
# up the hill where the user's were; phase 1 the car stands, phase 2 a soldier
# right by the car too low for the gun (it goes down to -18 degrees) besides
# the ones up the hill, phase 3 the host drives up and down the slope.
@{
    Name = "companion-gunner-uphill"
    Tags = @("companion", "vehicle", "combat")
    SummaryRu = "ИИ-компаньон за пулемётом джипа на склоне стреляет по врагам выше по холму (как в игре 16.10), не зависает на враге, до которого ствол не достаёт"
    Session = @{ Fixture = "new:rescue"; Companion = $true }
    Params = @{ Vehicle = "Non_Driveable_Vehicles.Civ_Car8"; Phase = 40; MinHits = 3; MinTotal = 10 }
    Body = {
        param($P)
        $v = $P.Vehicle
        $script:UphillHits = 0
        # the soldiers up the hill (where the user's truck crew stood)
        $hill = '{ {x=1671,y=1997}, {x=1691,y=2006}, {x=1687,y=2000}, {x=1680,y=2003} }'
        Step "stage the car on the slope and the soldiers up the hill" {
            Send-Game host "coop_god 1"
            # (no vehicle can be destroyed in this test: a wreck takes no seat
            # action, the gun of the car shot to pieces neither turns nor fires)
            Send-Game host "v_goliathMode 1"
            Send-Game host "coop_test_shottrace 1"
            Pause-Game 5
            Send-Game host ('#local v=System.GetEntityByName("' + $v + '"); local p={x=1724,y=1939,z=0}; p.z=System.GetTerrainElevation(p)+1.2; v:SetWorldPos(p); v:SetAngles({x=0,y=0,z=0}); g_gameRules.game:MovePlayer(g_localActorId,{x=p.x+3,y=p.y,z=p.z+1},{x=0,y=0,z=0}); local c=System.GetEntityByName("Companion"); g_gameRules.game:MovePlayer(c.id,{x=p.x-3,y=p.y,z=p.z+1},{x=0,y=0,z=0}); System.LogAlways("[CHK] car on the slope at "..math.floor(p.x).." "..math.floor(p.y).." "..math.floor(p.z))')
            Send-Game host ('#local spots=' + $hill + '; UphillKeep={}; UphillSoldiers={}; UphillReserve={}; local n=0; for i=1,30 do local e=System.GetEntityByName("Bridge_"..i); if e and e.actor and e.actor:GetHealth()>0 and n<9 then n=n+1; local t; if n<=4 then t={x=spots[n].x,y=spots[n].y,z=0} elseif n==5 then t={x=1700,y=2100,z=0} else t={x=1450+n*4,y=1650,z=0} end; t.z=System.GetTerrainElevation(t)+0.3; e:SetWorldPos(t); UphillKeep[e.id]=true; if n<=5 then table.insert(UphillSoldiers,e.id) else table.insert(UphillReserve,e.id) end end end; local t=System.GetEntity(UphillSoldiers[1]):GetWorldPos(); System.LogAlways("[CHK] soldiers up the hill: "..math.min(n,4)..", in reserve: "..#UphillReserve..", first at height "..math.floor(t.z))')
            Send-Game host '#local k=0; for _,e in ipairs(System.GetEntitiesInSphere({x=1724,y=1939,z=206},400) or {}) do if e.actor and not e.actor:IsPlayer() and e.actor:GetHealth()>0 and not UphillKeep[e.id] then g_gameRules:CreateHit(e.id,g_localActorId,g_localActorId,9000,nil,nil,nil,"bullet"); k=k+1 end end; System.LogAlways("[CHK] others shot: "..k)'
            Pause-Game 3
            $m = New-Mark
            Send-Game host "coop_test_enter $v 1"
            $in = Wait-Log host 'Companion entered vehicle' 90 $m
            Require ($null -ne $in) "the companion got into the car"
        }
        Step "phase 1: the car stands on the slope, the soldiers up the hill" {
            $m = New-Mark
            Pause-Game $P.Phase
            $n = Count-Log host '\] HIT [A-Za-z_]+[0-9]* by Companion ' $m
            $script:UphillHits += $n; Check $true "the companion hit soldiers up the hill from the gun: $n hits"
        }
        Step "phase 2: a soldier right by the car, too low for the gun" {
            # the 5th soldier 3 m in front of the car (the gun goes down to -18
            # degrees: from its height he is out of reach), the living ones of
            # the hill back where they were
            Send-Game host ('#local v=System.GetEntityByName("' + $v + '"); local p=v:GetWorldPos(); local d=v:GetDirectionVector(1); local spots=' + $hill + '; local n=0; for i,id in ipairs(UphillSoldiers or {}) do local e=System.GetEntity(id); if e and e.actor:GetHealth()>0 then local t; if i==5 then t={x=p.x+d.x*3,y=p.y+d.y*3,z=0} else t={x=spots[i].x,y=spots[i].y,z=0} end; t.z=System.GetTerrainElevation(t)+0.3; e:SetWorldPos(t); n=n+1 end end; System.LogAlways("[CHK] phase 2: one soldier by the car, alive in all: "..n)')
            $m = New-Mark
            Pause-Game $P.Phase
            $n = Count-Log host '\] HIT [A-Za-z_]+[0-9]* by Companion ' $m
            $hung = Count-Log companion 'gun aim at ' $m
            $script:UphillHits += $n; Check ($n -ge 1) "the companion kept firing with a soldier out of the gun's reach by the car: $n hits ($hung aim lines)"
        }
        Step "phase 3: the host drives up and down the slope" {
            Send-Game host ('#local v=System.GetEntityByName("' + $v + '"); local spots=' + $hill + '; local n=0; for i,id in ipairs(UphillSoldiers or {}) do local e=System.GetEntity(id); if e and e.actor:GetHealth()>0 and i<=4 then local t={x=spots[i].x,y=spots[i].y,z=0}; t.z=System.GetTerrainElevation(t)+0.3; e:SetWorldPos(t); n=n+1 end end; if n==0 then local k=0; for i=1,30 do local e=System.GetEntityByName("Bridge_"..i); if e and e.actor and e.actor:GetHealth()>0 and k<4 then k=k+1; local t={x=spots[k].x,y=spots[k].y,z=0}; t.z=System.GetTerrainElevation(t)+0.3; e:SetWorldPos(t) end end; n=k end; System.LogAlways("[CHK] phase 3 soldiers up the hill: "..n)')
            # (driven by the host for real: the gas for 4 s up the slope, back
            # to the start, again; the companion's copy of a car the host
            # drives can be moved by the server's states, as in the user's game)
            $m = New-Mark
            $end = (Get-Date).AddSeconds($P.Phase)
            while ((Get-Date) -lt $end) {
                Send-Game host ('#local v=System.GetEntityByName("' + $v + '"); local p={x=1724,y=1939,z=0}; p.z=System.GetTerrainElevation(p)+1.2; v:SetWorldPos(p); v:SetAngles({x=0,y=0,z=0}); System.LogAlways("[CHK] the car back at the start")')
                Pause-Game 1
                Send-Game host "coop_vehtest 4"
                Pause-Game 6
            }
            $n = Count-Log host '\] HIT [A-Za-z_]+[0-9]* by Companion ' $m
            $script:UphillHits += $n; Check $true "the companion hit soldiers up the hill while the host drove: $n hits"
        }
        Step "phase 4: the soldiers shoot at the car, the host drives, the companion can be hurt" {
            # (the user's second try: the car driven fast past them, the
            # companion hit again and again while at the gun)
            Send-Game host "coop_god 2"
            Send-Game host ('#local spots=' + $hill + '; local k=0; for _,id in ipairs(UphillReserve or {}) do local e=System.GetEntity(id); if e and e.actor:GetHealth()>0 and k<4 then k=k+1; local t={x=spots[k].x,y=spots[k].y,z=0}; t.z=System.GetTerrainElevation(t)+0.3; e:SetWorldPos(t) end end; System.LogAlways("[CHK] phase 4 soldiers up the hill: "..k)')
            $m = New-Mark
            $end = (Get-Date).AddSeconds($P.Phase)
            while ((Get-Date) -lt $end) {
                Send-Game host ('#local v=System.GetEntityByName("' + $v + '"); local p={x=1724,y=1939,z=0}; p.z=System.GetTerrainElevation(p)+1.2; v:SetWorldPos(p); v:SetAngles({x=0,y=0,z=0}); System.LogAlways("[CHK] the car back at the start")')
                Pause-Game 1
                Send-Game host "coop_vehtest 5"
                Pause-Game 7
            }
            Send-Game host "coop_god 1"
            $n = Count-Log host '\] HIT [A-Za-z_]+[0-9]* by Companion ' $m
            $hurt = Count-Log host 'HIT player Companion by ' $m
            $script:UphillHits += $n
            Check $true "the companion hit soldiers who shot at the car while the host drove: $n hits (it was hit $hurt times)"
            # (one phase alone says little here: the hill's crest hides the
            # soldiers now and then, the gun rightly holds its fire)
            Check ($script:UphillHits -ge $P.MinTotal) "the companion hit the soldiers $script:UphillHits times in all four phases (at least $($P.MinTotal))"
        }
    }
}
