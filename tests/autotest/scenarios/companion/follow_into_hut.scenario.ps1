# The user's games of 2026-10-16/17: following the host into the beach hut
# by the bridge (and out of it) the companion bumped into its walls: inside
# by the north-west door it walked to the wall nearest the host outside
# ("a way round to Nomad close by: ... 1.1 m to the point 14.6 m away", 20-30
# s), outside by the north door it stood "none (1 squares looked at)". Here
# it follows (no goto): the host steps into the hut, the companion comes from
# five sides; then the host steps out to the beach and it comes out after
# him. The hut (rescue's AI waypoint building, AINavigationModifier10): inner
# box x 1691.2-1698.3, y 1819.2-1826.6, floor z 194.4, doors north-west
# (1692.9,1826.6) and south (1695.0,1818.3), walls at 38/128 degrees.
@{
    Name = "companion-follow-into-hut"
    Tags = @("companion", "navigation")
    SummaryRu = "Компаньон идёт за хостом в хижину у моста с пяти сторон и выходит за ним обратно, не упираясь в стены"
    Session = @{ Fixture = "new:rescue"; Companion = $true }
    Params = @{ In = 25; Out = 20; MaxBump = 3.0 }
    Body = {
        param($P)
        $box = @(1691.2, 1698.3, 1819.2, 1826.6); $floor = 194.4
        $inside = @(1695.0, 1822.8, 194.4)
        $beach = @(1680.0, 1812.0)
        $starts = @(
            @{ n = "east wall"; p = @(1702.0, 1823.0) }, @{ n = "west wall"; p = @(1687.5, 1823.0) },
            @{ n = "north door, user spot"; p = @(1688.0, 1830.0) }, @{ n = "south door"; p = @(1695.2, 1814.5) },
            @{ n = "behind the truck"; p = @(1697.0, 1843.0) })
        function Wait-Watch([scriptblock]$Ok, [double]$Within, $Since) {
            $end = (Get-Date).AddSeconds($Within)
            while ((Get-Date) -lt $end) {
                Pause-Game 1
                $w = @(Get-Watch $Since)
                if ($w.Count -and (& $Ok $w[-1])) { return $w }
            }
            return $null
        }
        Step "the beach empty" {
            Send-Game host "coop_god 1"
            Pause-Game 5
            Send-Game host '#local p={x=1694,y=1823,z=194}; local k=0; for _,e in ipairs(System.GetEntitiesInSphere(p,250) or {}) do if e.actor and not e.actor:IsPlayer() and e.actor:GetHealth()>0 then g_gameRules:CreateHit(e.id,g_localActorId,g_localActorId,9000,nil,nil,nil,"bullet"); k=k+1 end end; System.LogAlways("[CHK] soldiers shot: "..k)'
            Start-Watch @() $null
        }
        foreach ($s in $starts) {
            Step "the host steps in, the companion from the $($s.n)" {
                # (the companion put at its start and held there: following, it
                # ran to the host on the beach in the 3 s before, and the legs
                # did not start from the sides they are named after)
                Send-Game host ('#g_gameRules.game:MovePlayer(g_localActorId,{x=' + $beach[0] + ',y=' + $beach[1] + ',z=System.GetTerrainElevation({x=' + $beach[0] + ',y=' + $beach[1] + ',z=0})+0.5},{x=0,y=0,z=0}); local c=System.GetEntityByName("Companion"); g_gameRules.game:MovePlayer(c.id,{x=' + $s.p[0] + ',y=' + $s.p[1] + ',z=System.GetTerrainElevation({x=' + $s.p[0] + ',y=' + $s.p[1] + ',z=0})+0.5},{x=0,y=0,z=0}); System.LogAlways("[CHK] companion at ' + $s.n + '")')
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
                $m = New-Mark
                Send-Game host ('#g_gameRules.game:MovePlayer(g_localActorId,{x=' + $inside[0] + ',y=' + $inside[1] + ',z=' + ($inside[2] + 0.3) + '},{x=0,y=0,z=0}); System.LogAlways("[CHK] host in the hut")')
                Send-Agent "follow" | Out-Null
                $t0 = Get-Date
                $w = Wait-Watch { param($l) Test-Inside $l $box $floor } $P.In $m
                $secs = ((Get-Date) - $t0).TotalSeconds
                $all = @(Get-Watch $m)
                $last = if ($all.Count) { $all[-1] } else { $null }
                Check ($null -ne $w) ("$($s.n): inside the hut after the host within $($P.In) s" + $(if ($w) { " ({0:N0} s)" -f $secs } elseif ($last) { " (at {0:N1} {1:N1} {2:N1}, {3:N1} m from the host)" -f $last.X, $last.Y, $last.Z, $last.Dh } else { "" }))
                $bump = Get-BumpSeconds $all
                Check ($bump -le $P.MaxBump) "$($s.n): pressed into something $bump s on the way in (at most $($P.MaxBump))"
                Check ((Count-Log host 'Companion (caught up with|put next to)' $m) -eq 0) "$($s.n): no teleport on the way in"
                Check ((Count-Log companion 'close by: none \(1 squares' $m) -lt 3) "$($s.n): never boxed in on its own square"
            }
            Step "the host steps out to the beach, the companion after him" {
                Pause-Game 2
                $m = New-Mark
                Send-Game host ('#g_gameRules.game:MovePlayer(g_localActorId,{x=' + $beach[0] + ',y=' + $beach[1] + ',z=System.GetTerrainElevation({x=' + $beach[0] + ',y=' + $beach[1] + ',z=0})+0.5},{x=0,y=0,z=0}); System.LogAlways("[CHK] host on the beach")')
                $t0 = Get-Date
                $w = Wait-Watch { param($l) -not (Test-Inside $l $box $floor) -and $l.Dh -lt 8.0 } $P.Out $m
                $secs = ((Get-Date) - $t0).TotalSeconds
                $all = @(Get-Watch $m)
                $last = if ($all.Count) { $all[-1] } else { $null }
                Check ($null -ne $w) ("$($s.n): out of the hut and by the host within $($P.Out) s" + $(if ($w) { " ({0:N0} s)" -f $secs } elseif ($last) { " (at {0:N1} {1:N1} {2:N1}, {3:N1} m from the host)" -f $last.X, $last.Y, $last.Z, $last.Dh } else { "" }))
                Check ((Get-BumpSeconds $all) -le $P.MaxBump) "$($s.n): pressed into something $(Get-BumpSeconds $all) s on the way out"
                Check ((Count-Log host 'Companion (caught up with|put next to)' $m) -eq 0) "$($s.n): no teleport on the way out"
            }
        }
        Stop-Watch
    }
}
