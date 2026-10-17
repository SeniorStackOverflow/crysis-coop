# The user's game of 2026-10-16 again: the companion's vehicle gun froze
# (the view and the gun the same to 2 decimals for 6-45 s, 0.3-2.5 rad off the
# enemy) after it had to turn far, while the host drove. Here: the car by the
# Rescue bridge, one soldier at a time 40 m in front of it or 40 m behind it
# (the gun turns half round each time), first the car standing, then the host
# driving; each round the companion has to hit him.
@{
    Name = "companion-gunner-behind"
    Tags = @("companion", "vehicle", "combat")
    SummaryRu = "ИИ-компаньон за пулемётом разворачивает ствол к врагу сзади и спереди (машина стоит и едет), ствол не замирает"
    Session = @{ Fixture = "new:rescue"; Companion = $true }
    Params = @{ Vehicle = "Land.Chinese_4wd1"; Rounds = 6; RoundTime = 14; Awake = 1; MinDriveHits = 4 }
    Body = {
        param($P)
        $v = $P.Vehicle
        Step "stage the car and the soldiers" {
            Send-Game host "coop_god 1"
            # (no vehicle can be destroyed in this test: a wreck takes no seat
            # action, the gun of the car shot to pieces neither turns nor fires)
            Send-Game host "v_goliathMode 1"
            Send-Game companion "coop_test_gun_awake $($P.Awake)"
            # (the states of the car its game gets from the host, and where its copy is)
            # (set on the host: its cvars are sent to the clients)
            Send-Game host "coop_debug_physdump $($v.Split('.')[-1])"
            Pause-Game 5
            Send-Game host ('#local v=System.GetEntityByName("' + $v + '"); local p={x=1737,y=1960,z=0}; p.z=System.GetTerrainElevation(p)+1.2; v:SetWorldPos(p); v:SetAngles({x=0,y=0,z=0}); g_gameRules.game:MovePlayer(g_localActorId,{x=p.x+3,y=p.y,z=p.z+1},{x=0,y=0,z=0}); local c=System.GetEntityByName("Companion"); g_gameRules.game:MovePlayer(c.id,{x=p.x-3,y=p.y,z=p.z+1},{x=0,y=0,z=0}); System.LogAlways("[CHK] car on the road by the bridge")')
            Send-Game host '#BehindKeep={}; BehindList={}; local n=0; for i=1,30 do local e=System.GetEntityByName("Bridge_"..i); if e and e.actor and e.actor:GetHealth()>0 and n<12 then n=n+1; local t={x=1400+n*5,y=1650,z=0}; t.z=System.GetTerrainElevation(t)+0.3; e:SetWorldPos(t); BehindKeep[e.id]=true; table.insert(BehindList,e.id) end end; System.LogAlways("[CHK] soldiers kept away: "..n)'
            Send-Game host '#local k=0; for _,e in ipairs(System.GetEntitiesInSphere({x=1737,y=1960,z=200},400) or {}) do if e.actor and not e.actor:IsPlayer() and e.actor:GetHealth()>0 and not BehindKeep[e.id] then g_gameRules:CreateHit(e.id,g_localActorId,g_localActorId,9000,nil,nil,nil,"bullet"); k=k+1 end end; System.LogAlways("[CHK] others shot: "..k)'
            Pause-Game 3
            $m = New-Mark
            Send-Game host "coop_test_enter $v 1"
            $in = Wait-Log host 'Companion entered vehicle' 90 $m
            Require ($null -ne $in) "the companion got into the car"
            Pause-Game 3
        }
        Step "rounds, the car stands" {
            $hitRounds = 0
            for ($r = 1; $r -le $P.Rounds; $r++) {
                $side = if ($r % 2 -eq 1) { -1 } else { 1 }
                # the next living soldier 40 m in front (1) or behind (-1) of
                # the car, the one before back far away
                Send-Game host ('#local v=System.GetEntityByName("' + $v + '"); local p=v:GetWorldPos(); local d=v:GetDirectionVector(1); BehindNow=nil; for _,id in ipairs(BehindList or {}) do local e=System.GetEntity(id); if e and e.actor:GetHealth()>0 then local t; if not BehindNow then BehindNow=e; t={x=p.x+d.x*40*(' + $side + '),y=p.y+d.y*40*(' + $side + '),z=0} else t={x=1400,y=1650,z=0} end; t.z=System.GetTerrainElevation(t)+0.3; e:SetWorldPos(t) end end; System.LogAlways("[CHK] round ' + $r + ': " .. (BehindNow and BehindNow:GetName() or "nobody") .. " at side ' + $side + '")')
                # (his copy in the companion's game follows the move a
                # moment later: the hits are counted after 3 s)
                Pause-Game 3
                $m = New-Mark
                Pause-Game $P.RoundTime
                $n = Count-Log host '\] HIT Bridge_[0-9]+ by Companion ' $m
                if ($n -gt 0) { $hitRounds++ }
                Check $true "round $r ($(if ($side -lt 0) { 'behind' } else { 'in front' })), the car stands: $n hits"
            }
            Check ($hitRounds -ge $P.Rounds - 2) "the companion hit the soldier in $hitRounds of $($P.Rounds) rounds, the car stands"
        }
        Step "the host drives past soldiers on both sides of the road" {
            # (placed once, before it drives: a soldier moved far while the car
            # is about stayed where he was in the companion's game)
            Send-Game host ('#local spots={ {x=1725,y=1940}, {x=1749,y=1952}, {x=1724,y=1975}, {x=1750,y=1990}, {x=1726,y=2010}, {x=1748,y=2025} }; local k=0; for _,id in ipairs(BehindList or {}) do local e=System.GetEntity(id); if e and e.actor:GetHealth()>0 and k<6 then k=k+1; local t={x=spots[k].x,y=spots[k].y,z=0}; t.z=System.GetTerrainElevation(t)+0.3; e:SetWorldPos(t) end end; System.LogAlways("[CHK] soldiers along the road: "..k)')
            Pause-Game 5
            $m = New-Mark
            for ($r = 1; $r -le $P.Rounds; $r++) {
                Send-Game host "coop_vehtest 4"
                Pause-Game 6
                Send-Game host ('#local v=System.GetEntityByName("' + $v + '"); local p={x=1737,y=1960,z=0}; p.z=System.GetTerrainElevation(p)+1.2; v:SetWorldPos(p); v:SetAngles({x=0,y=0,z=0}); System.LogAlways("[CHK] the car back at the start")')
                Pause-Game 2
            }
            $n = Count-Log host '\] HIT Bridge_[0-9]+ by Companion ' $m
            $killed = Count-Log host '\] HIT Bridge_[0-9]+ by Companion .*KILLED' $m
            Check ($n -ge $P.MinDriveHits) "the companion hit the soldiers along the road $n times while the host drove ($killed killed)"
        }
    }
}
