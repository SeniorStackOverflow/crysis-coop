# The user, 2026-10-15: "it can't kill, and under fire there is no point
# running to revive when you can be killed - tell the two apart". Here: the
# host goes down by the bridge with four of its squad 25 m off, alive and
# shooting; nobody is god. The companion must not start a revive while it is
# being hit (it fights or keeps down first), and must revive the host once
# the shooting has stopped (the soldiers dead or it is no longer hit).
@{
    Name = "companion-revive-under-fire"
    Tags = @("companion", "revive", "combat")
    SummaryRu = "Хост упал под огнём: компаньон не бежит поднимать, пока в него попадают, и поднимает, когда стрельба стихла"
    Session = @{ Fixture = "new:rescue"; Companion = $true }
    Params = @{ Watch = 150 }
    Body = {
        param($P)
        Step "the host down by the bridge, four soldiers 25 m off" {
            Pause-Game 5
            Send-Game host "coop_god 1"
            # every other soldier around dead; four of the bridge squad 25 m north of the host
            Send-Game host '#local p={x=1737,y=1960,z=0}; p.z=System.GetTerrainElevation(p)+1; local keep={}; local n=0; for i=1,30 do local e=System.GetEntityByName("Bridge_"..i); if e and e.actor and e.actor:GetHealth()>0 and n<4 then local t={x=p.x+(n-1.5)*5,y=p.y+25,z=0}; t.z=System.GetTerrainElevation(t)+0.3; e:SetWorldPos(t); keep[e.id]=true; n=n+1 end end; local k=0; for _,e in ipairs(System.GetEntitiesInSphere(p,400) or {}) do if e.actor and not e.actor:IsPlayer() and e.actor:GetHealth()>0 and not keep[e.id] then g_gameRules:CreateHit(e.id,g_localActorId,g_localActorId,9000,nil,nil,nil,"bullet"); k=k+1 end end; System.LogAlways("[CHK] soldiers placed: "..n..", others shot: "..k)'
            Send-Game host '#local p={x=1737,y=1960,z=0}; p.z=System.GetTerrainElevation(p)+1; local c=System.GetEntityByName("Companion"); g_gameRules.game:MovePlayer(c.id,{x=p.x-6,y=p.y-4,z=p.z},{x=0,y=0,z=0}); g_gameRules.game:MovePlayer(g_localActorId,p,{x=0,y=0,z=0}); System.LogAlways("[CHK] players placed")'
            Pause-Game 2
            Send-Game host "coop_god 0"
            Send-Game host '#g_gameRules:CreateHit(g_localActorId,g_localActorId,g_localActorId,9000,nil,nil,nil,"bullet"); System.LogAlways("[CHK] the host is down")'
        }
        Step "the companion fights first, then revives" {
            $m = New-Mark
            $end = Wait-Log host 'Companion revived Nomad|revived Nomad$|everybody is down' $P.Watch $m
            Send-Game host "coop_god 1"
            # every revive the companion started: was it hit just before (the 25 lines before, about 1 s)?
            $tail = Get-Tail host
            $starts = @(Find-Log host '^\[Coop\] Companion revives Nomad' $m -All)
            $underFire = 0
            foreach ($s in $starts) {
                $from = [Math]::Max(0, $s.No - 26)
                for ($i = $from; $i -lt $s.No - 1; $i++) {
                    if ($tail.Lines[$i] -match '^\[Coop\] DAMAGE player Companion .* taken=([\d.]+)' -and [double]$Matches[1] -gt 0) { $underFire++; break }
                }
            }
            $hits = Count-Log host '^\[Coop\] DAMAGE player Companion .* taken=[1-9]' $m
            $kills = Count-Log host '\] HIT Bridge_\d+ by Companion .*KILLED' $m
            Check $true "the companion was hit $hits times and killed $kills soldiers; it started $($starts.Count) revives"
            Check ($underFire -eq 0) "revives started while it was being hit: $underFire"
            Check ($null -ne $end -and $end.Text -notmatch 'everybody') ("the host revived within $($P.Watch) s" + $(if ($end) { ": $($end.Text)" } else { "" }))
        }
    }
}
