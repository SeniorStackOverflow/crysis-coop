# The user's game of 2026-10-15: the host down ~195 m from the AI companion,
# which stood still (it looked for downed teammates only within 80 m). Here:
# the same places; the companion must come and revive the host.
@{
    Name = "companion-revives-far"
    Tags = @("companion", "revive")
    SummaryRu = "Хост упал в 190 м от ИИ-компаньона: компаньон прибегает и поднимает его"
    Session = @{ Fixture = "new:rescue"; Companion = $true }
    Params = @{ Watch = 150 }
    Body = {
        param($P)
        $where = '#local c=System.GetEntityByName("Companion"); local n=System.GetEntityByName("Nomad"); local function f(e) if not e then return "-" end local p=e:GetWorldPos(); return string.format("%.0f %.0f %.0f hp %d", p.x, p.y, p.z, e.actor:GetHealth()) end; local d=(c and n) and math.floor(c:GetDistance(n.id)) or -1; System.LogAlways("[CHK] TAG: companion "..f(c).." host "..f(n).." apart "..d.." m")'
        Step "place them 190 m apart, the host down" {
            Pause-Game 8
            # the soldiers there dead first (in the user's game they were; here they would kill the companion)
            Send-Game host '#local n=0; for _,e in ipairs(System.GetEntitiesInSphere({x=1710,y=1890,z=200},320) or {}) do if e.actor and not e.actor:IsPlayer() and e.actor:GetHealth()>0 then g_gameRules:CreateHit(e.id,g_localActorId,g_localActorId,9000,nil,nil,nil,"bullet"); n=n+1 end end; System.LogAlways("[CHK] soldiers around shot: "..n)'
            Send-Game host "coop_god 0"
            # placed and the host shot in one go: a host alive and far away has the
            # companion catch up with him (it is put by him), a downed one not
            Send-Game host '#local c=System.GetEntityByName("Companion"); g_gameRules.game:MovePlayer(c.id, {x=1678,y=1805,z=System.GetTerrainElevation({x=1678,y=1805,z=0})+1}, {x=0,y=0,z=0}); g_gameRules.game:MovePlayer(g_localActorId, {x=1737,y=1981,z=System.GetTerrainElevation({x=1737,y=1981,z=0})+1}, {x=0,y=0,z=0}); g_gameRules:CreateHit(g_localActorId,g_localActorId,g_localActorId,9000,nil,nil,nil,"bullet"); System.LogAlways("[CHK] placed as in the user game, the host is down")'
            Pause-Game 1
            Send-Game host $where.Replace("TAG", "host down")
            $h = Find-Log host '^\[CHK\] host down: .* apart (-?\d+) m' -Last
            Require ($h -and [int]$h.Match.Groups[1].Value -ge 150) "the host is down $(if ($h) { $h.Match.Groups[1].Value } else { '?' }) m from the companion"
        }
        Step "the companion comes" {
            $m = New-Mark
            $up = Wait-Log host 'Nomad is up again|revived Nomad|Nomad revived' $P.Watch $m
            Send-Game host $where.Replace("TAG", "at the end")
            Check ($null -ne $up) ("the companion revived the host within $($P.Watch) s" + $(if ($up) { ": $($up.Text)" } else { "" }))
            Check ((Count-Log host 'everybody is down' $m) -eq 0) "no everybody-down while it came"
        }
    }
}
