# The user's games of 2026-10-16/17 (companion.log 782-992 of user_logs_1016b,
# 918-946 of user_logs_1017b): by the beach hut the companion logged "a way round
# to Nomad close by: none (1 squares looked at)" 8 times in a row at (1688,1830)
# / (1694,1831), i.e. pressed into the hut's north doorway / the clutter between
# the hut and Bridge_Truck_2. The hut (AI waypoint building 1 of rescue's
# netmission0.bai): inner nodes x 1691.2..1698.3, y 1819.2..1826.6, floor z 194.4;
# doorways S (1695.0,1818.3) facing south, NW (1692.9,1826.6) facing north-west;
# the east and west sides are walls.
@{
    Name = "companion-enter-beach-hut"
    Tags = @("companion", "navigation")
    SummaryRu = "Компаньон заходит в хижину у пляжа с любой стороны (со стороны стены обходит к двери), не упирается в стены"
    Session = @{ Fixture = "new:rescue"; Companion = $true }
    Params = @{ Leg = 45; MaxBump = 2.0 }
    Body = {
        param($P)
        $box = @(1691.2, 1698.3, 1819.2, 1826.6); $floor = 194.4
        $inside = @(1694.7, 1823.0, 194.4)
        # starts: the east wall side, the west wall side, the user's spot at the
        # north door, south of the south door, behind Bridge_Truck_2
        $starts = @(
            @{ n = "east wall"; p = @(1702.0, 1823.0) }, @{ n = "west wall"; p = @(1687.5, 1823.0) },
            @{ n = "north door, user spot"; p = @(1688.0, 1830.0) }, @{ n = "south door"; p = @(1695.2, 1814.5) },
            @{ n = "behind the truck"; p = @(1697.0, 1843.0) })
        Step "the beach empty, the host on the beach" {
            Send-Game host "coop_god 1"
            Pause-Game 5
            Send-Game host '#local p={x=1694,y=1823,z=194}; local k=0; for _,e in ipairs(System.GetEntitiesInSphere(p,200) or {}) do if e.actor and not e.actor:IsPlayer() and e.actor:GetHealth()>0 then g_gameRules:CreateHit(e.id,g_localActorId,g_localActorId,9000,nil,nil,nil,"bullet"); k=k+1 end end; System.LogAlways("[CHK] soldiers shot: "..k)'
            Send-Game host '#g_gameRules.game:MovePlayer(g_localActorId,{x=1680,y=1812,z=System.GetTerrainElevation({x=1680,y=1812,z=0})+1},{x=0,y=0,z=0}); System.LogAlways("[CHK] host on the beach")'
            Start-Watch @() $inside
        }
        foreach ($s in $starts) {
            Step "from the $($s.n) into the hut" {
                # (put there first, then held: a hold before the move walked it
                # back toward its old spot, the leg did not start where named)
                Send-Game host ('#local c=System.GetEntityByName("Companion"); g_gameRules.game:MovePlayer(c.id,{x=' + $s.p[0] + ',y=' + $s.p[1] + ',z=System.GetTerrainElevation({x=' + $s.p[0] + ',y=' + $s.p[1] + ',z=0})+0.5},{x=0,y=0,z=0}); System.LogAlways("[CHK] companion at ' + $s.n + '")')
                # (sent there and held there: a 'hold' after the move came a moment
                # late, it had begun to walk to the host or its old spot)
                $mGo = New-Mark
                Send-Agent ("goto " + $s.p[0] + " " + $s.p[1] + " 194") | Out-Null
                [void](Wait-Log companion '^\[CoopAgent\] arrived: holding there' 15 $mGo)
                $mPre = New-Mark
                Pause-Game 2
                $pre = @(Get-Watch $mPre)
                $at = if ($pre.Count) { $pre[-1] } else { $null }
                $off = if ($at) { [Math]::Sqrt([Math]::Pow($at.X - $s.p[0], 2) + [Math]::Pow($at.Y - $s.p[1], 2)) } else { 99 }
                Require ($off -lt 2.5) ("the companion starts at the $($s.n) ({0:N1} m from it)" -f $off)
                Set-WatchGoal $inside
                $m = New-Mark
                Send-Agent ("goto " + ($inside -join " ")) | Out-Null
                $arr = Wait-Log companion '^\[CoopAgent\] arrived: holding there' $P.Leg $m
                Pause-Game 1
                $w = Get-Watch $m
                $last = if ($w.Count) { $w[-1] } else { $null }
                $bump = Get-BumpSeconds $w
                Check ($null -ne $arr -and $last -and (Test-Inside $last $box $floor)) ("$($s.n): inside the hut within $($P.Leg) s" + $(if ($last) { " (at {0:N1} {1:N1} {2:N1})" -f $last.X, $last.Y, $last.Z } else { "" }))
                Check ($bump -le $P.MaxBump) "$($s.n): pressed into something $bump s (at most $($P.MaxBump))"
                Check ((Count-Log host 'Companion (caught up with|put next to)' $m) -eq 0) "$($s.n): no teleport on the way"
                Check ((Count-Log companion 'close by: none \(1 squares' $m) -lt 3) "$($s.n): never stuck with its own square blocked"
            }
            Step "out through a door and back to the $($s.n)" {
                Set-WatchGoal @($s.p[0], $s.p[1], 194)
                $m = New-Mark
                Send-Agent ("goto " + $s.p[0] + " " + $s.p[1] + " 194") | Out-Null
                $arr = Wait-Log companion '^\[CoopAgent\] arrived: holding there' $P.Leg $m
                $w = Get-Watch $m
                Check ($null -ne $arr) "$($s.n): back outside within $($P.Leg) s"
                Check ((Get-BumpSeconds $w) -le $P.MaxBump) "$($s.n): on the way out pressed into something $(Get-BumpSeconds $w) s"
            }
        }
        Stop-Watch
        Send-Agent "follow" | Out-Null
    }
}
