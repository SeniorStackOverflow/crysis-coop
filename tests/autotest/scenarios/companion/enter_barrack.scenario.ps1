# The user (2026-10-17): the companion bumps into a wall entering a house or a
# van. Rescue's "vans" are the Chinese barrack containers on legs at the
# helicopter camp: a floor 1.2 m up (z 236.3-236.4 on ground 235.1), stairs
# up to a door (landings about (1560.4-1561.4, 2129.5-2131.0) and
# (1565.7-1566.5, 2140.0-2142.4); AINavigationModifier37). Here the host
# stands inside one; the companion comes up the stairs after him from two
# sides (follow), and goes in by itself (goto).
@{
    Name = "companion-enter-barrack"
    Tags = @("companion", "navigation")
    SummaryRu = "Компаньон поднимается по ступенькам в барак-контейнер на ножках (фургон) за хостом и сам, не упираясь в стены"
    Session = @{ Fixture = "new:rescue"; Companion = $true }
    Params = @{ Within = 35; MaxBump = 3.0 }
    Body = {
        param($P)
        $inside = @(1562.5, 2136.5, 236.4)
        $starts = @(@{ n = "south-west"; p = @(1556.0, 2127.0) }, @{ n = "north-east"; p = @(1570.0, 2144.0) })
        function Wait-Watch([scriptblock]$Ok, [double]$Within, $Since) {
            $end = (Get-Date).AddSeconds($Within)
            while ((Get-Date) -lt $end) {
                Pause-Game 1
                $w = @(Get-Watch $Since)
                if ($w.Count -and (& $Ok $w[-1])) { return $w }
            }
            return $null
        }
        Step "the camp empty" {
            Send-Game host "coop_god 1"
            Pause-Game 5
            Send-Game host '#local p={x=1562,y=2136,z=236}; local k=0; for _,e in ipairs(System.GetEntitiesInSphere(p,250) or {}) do if e.actor and not e.actor:IsPlayer() and e.actor:GetHealth()>0 then g_gameRules:CreateHit(e.id,g_localActorId,g_localActorId,9000,nil,nil,nil,"bullet"); k=k+1 end end; System.LogAlways("[CHK] soldiers shot: "..k)'
            Start-Watch @() $null
        }
        foreach ($s in $starts) {
            Step "the host inside, the companion after him from the $($s.n)" {
                Send-Agent "follow" | Out-Null
                Send-Game host ('#local c=System.GetEntityByName("Companion"); local function g(x,y) return {x=x,y=y,z=System.GetTerrainElevation({x=x,y=y,z=0})+0.5} end; g_gameRules.game:MovePlayer(g_localActorId,g(' + ($s.p[0] + 2) + ',' + ($s.p[1] + 2) + '),{x=0,y=0,z=0}); g_gameRules.game:MovePlayer(c.id,g(' + $s.p[0] + ',' + $s.p[1] + '),{x=0,y=0,z=0}); System.LogAlways("[CHK] both at the ' + $s.n + '")')
                Pause-Game 3
                $m = New-Mark
                Send-Game host ('#g_gameRules.game:MovePlayer(g_localActorId,{x=' + $inside[0] + ',y=' + $inside[1] + ',z=' + ($inside[2] + 0.3) + '},{x=0,y=0,z=0}); System.LogAlways("[CHK] host in the barrack")')
                $t0 = Get-Date
                $w = Wait-Watch { param($l) $l.Z -gt 236.0 -and $l.Dh -lt 3.5 } $P.Within $m
                $secs = ((Get-Date) - $t0).TotalSeconds
                $all = @(Get-Watch $m)
                $last = if ($all.Count) { $all[-1] } else { $null }
                Check ($null -ne $w) ("$($s.n): up in the barrack by the host within $($P.Within) s" + $(if ($w) { " ({0:N0} s)" -f $secs } elseif ($last) { " (at {0:N1} {1:N1} {2:N1}, {3:N1} m from the host)" -f $last.X, $last.Y, $last.Z, $last.Dh } else { "" }))
                Check ((Get-BumpSeconds $all) -le $P.MaxBump) "$($s.n): pressed into something $(Get-BumpSeconds $all) s"
                Check ((Count-Log host 'Companion (caught up with|put next to)' $m) -eq 0) "$($s.n): no teleport"
            }
            Step "the host outside, the companion goes in by itself from the $($s.n)" {
                Send-Game host ('#local c=System.GetEntityByName("Companion"); local function g(x,y) return {x=x,y=y,z=System.GetTerrainElevation({x=x,y=y,z=0})+0.5} end; g_gameRules.game:MovePlayer(g_localActorId,g(1545,2120),{x=0,y=0,z=0}); g_gameRules.game:MovePlayer(c.id,g(' + $s.p[0] + ',' + $s.p[1] + '),{x=0,y=0,z=0}); System.LogAlways("[CHK] companion at the ' + $s.n + ' again")')
                # (sent there and held there: a 'hold' after the move came a moment
                # late, it had begun to walk to the host or its old spot)
                $mGo = New-Mark
                Send-Agent ("goto " + $s.p[0] + " " + $s.p[1] + " 235") | Out-Null
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
                $arr = Wait-Log companion '^\[CoopAgent\] arrived: holding there' $P.Within $m
                Pause-Game 1
                $all = @(Get-Watch $m)
                $last = if ($all.Count) { $all[-1] } else { $null }
                Check ($null -ne $arr -and $last -and $last.Z -gt 236.0) ("$($s.n): in the barrack by itself within $($P.Within) s" + $(if ($last) { " (at {0:N1} {1:N1} {2:N1})" -f $last.X, $last.Y, $last.Z } else { "" }))
                Check ((Get-BumpSeconds $all) -le $P.MaxBump) "$($s.n): pressed into something $(Get-BumpSeconds $all) s"
                $sj = @(Find-Log companion 'a strength jump onto' $m -All).Count
                Check $true "strength jumps on the way: $sj"
                Send-Agent "follow" | Out-Null
            }
        }
        Stop-Watch
    }
}
