# The user's game of 2026-10-23 (campaign "checking" continued from Rescue_08;
# Game.log 84 and 4291-4328, companion.log 3840-3911). On the river bank by the
# bridge the host stood on top (1699,1856,194), the companion below
# (1708,1854,190), a bank LedgeAhead took for 1.3 m between. Its squares went
# straight at it ("1 corners, 8.5 m ... found"; the host's navigation had a way
# up round by 1704.3 1850.8 193.3); it strength-jumped twice, rose to z 192 and
# fell back each time, then "going round within 4 m for 15 s, 9 m from Nomad:
# put next to him". The user: jump and keep going forward; in armor he got up
# jumping again and again. Here: following, it gets up by itself before any
# catch-up, few jumps falling back, forward in the air, no going round on the
# spot; then by goto (no catch-up at all), and with the host right above the
# bank (4 m off, as at its first jump that day).
@{
    Name = "companion-ledge-bridge"
    Tags = @("companion", "navigation")
    SummaryRu = "Компаньон под уступом ~1.3 м на берегу у моста (игра 23.10): сам выбирается к хосту — прыгает вперёд, а не на месте, не кружит, и его не ставят рядом"
    Session = @{ Fixture = "save:dome1022"; Campaign = "56c79107e0d7a6ff"; Companion = $true; ReadyCommands = @("coop_god 1") }
    Params = @{
        Bottom = @(1708.0, 1854.0, 190.0); Top = @(1699.0, 1856.0, 194.0); Near = @(1704.5, 1854.0, 193.0)
        Within = 30; UpBy = 20; GotoWithin = 40; MaxFallBacks = 3; MinForward = 0.4; MaxRound = 10
        # (2 strength jumps per place in 60 s: s_bot.sjPlace; 0 once an agent "forget" exists)
        Forget = 62; GotoLeg = $true; NearLeg = $true
        # (coop_test_movetrace does not exist yet)
        MoveTrace = $false
    }
    Body = {
        param($P)
        $b = $P.Bottom
        $suits = @("speed", "strength", "cloak", "armor")
        # the ground under x y near z: a ray from 2.5 m above it 8 m down (tree tops stay out), else the terrain
        $G = 'local function G(x,y,z) if Physics.RayWorldIntersection({x=x,y=y,z=z+2.5},{x=0,y=0,z=-8},1,ent_terrain+ent_static,g_localActorId,nil,g_HitTable)>0 then local h=g_HitTable[1]; if h and h.pos then return h.pos.z end end return System.GetTerrainElevation({x=x,y=y,z=0}) end; '
        function Get-D2($a, $c) { [Math]::Sqrt([Math]::Pow($a.X - $c.X, 2) + [Math]::Pow($a.Y - $c.Y, 2)) }
        function Get-Pt($v) { [pscustomobject]@{ X = $v[0]; Y = $v[1] } }
        # up by the goal: within 4.5 m and not more than 1 m below it
        function Test-Up($S, $Goal) { $S.Z -ge $Goal[2] - 1.0 -and $S.Dg -ge 0 -and $S.Dg -le 4.5 }
        # the 10 Hz trace on the host
        function Get-JTrace($Since) {
            $rx = '^\[CHK\] j t=([\d.]+) pos=(-?[\d.]+),(-?[\d.]+),(-?[\d.]+) fly=(\d) suit=(-?\d+) en=(-?[\d.]+)'
            @(Find-Log host $rx $Since -All | ForEach-Object { $g = $_.Match.Groups
                [pscustomobject]@{ No = $_.No; T = [double]$g[1].Value; X = [double]$g[2].Value; Y = [double]$g[3].Value; Z = [double]$g[4].Value; Fly = [int]$g[5].Value; Suit = [int]$g[6].Value; En = [double]$g[7].Value } })
        }
        # jumps: off the ground and >= 0.4 m up; run-up speed, height, where it came down
        # (0.3 s after touch-down, against the take-off), toward the goal / sideways in the air
        function Get-Jumps($J, $Goal) {
            $out = @(); $i = 1
            while ($i -lt $J.Count) {
                if ($J[$i].Fly -eq 1 -and $J[$i - 1].Fly -eq 0) {
                    $s = $J[$i - 1]; $peak = $s.Z; $k = $i; $top = $s
                    while ($k -lt $J.Count - 1 -and $J[$k].Fly -eq 1) { if ($J[$k].Z -gt $peak) { $peak = $J[$k].Z; $top = $J[$k] }; $k++ }
                    $l = $J[$k]; $e = $J[[Math]::Min($k + 3, $J.Count - 1)]
                    $p0 = $J[[Math]::Max(0, $i - 4)]
                    $ux = $Goal[0] - $s.X; $uy = $Goal[1] - $s.Y; $ul = [Math]::Max(0.01, [Math]::Sqrt($ux * $ux + $uy * $uy)); $ux /= $ul; $uy /= $ul
                    # (toward the goal up to the jump's top: a jump on the spot or on into it; where it
                    # came down says little on a 50 degree bank, a jump that fell short slides back)
                    $dx = $top.X - $s.X; $dy = $top.Y - $s.Y
                    if ($peak - $s.Z -ge 0.4) {
                        $out += [pscustomobject]@{ T = $s.T; No = $s.No; Up = $peak - $s.Z; Landed = $e.Z - $s.Z; Forward = $dx * $ux + $dy * $uy
                            Side = [Math]::Abs($dx * $uy - $dy * $ux); Air = $l.T - $s.T; RunUp = $(if ($s.T -gt $p0.T) { (Get-D2 $s $p0) / ($s.T - $p0.T) } else { 0 }); Suit = $s.Suit; En = $s.En }
                    }
                    $i = $k + 1
                } else { $i++ }
            }
            return $out
        }
        # the longest stretch going round (or standing) within 4 m without getting 2 m nearer
        function Get-RoundSeconds($W) {
            $best = 0.0
            for ($a = 0; $a -lt $W.Count; $a++) {
                $k = $a
                while ($k + 1 -lt $W.Count -and (Get-D2 $W[$k + 1] $W[$a]) -le 4.0 -and $W[$k + 1].Dg -gt $W[$a].Dg - 2.0) { $k++ }
                $best = [Math]::Max($best, $W[$k].T - $W[$a].T)
            }
            return $best
        }
        # the host at $HostAt, the companion at the bottom held there, suits full
        # (held before the move: a following companion put far from the host
        # - the fixture starts at the dome - catches up with him at once)
        function Set-Places($HostAt, [string]$Tag) {
            Send-Agent "hold" | Out-Null
            $mP = New-Mark
            Send-Game host ('#' + $G + 'local c=System.GetEntityByName("Companion"); g_gameRules.game:MovePlayer(g_localActorId,{x=' + $HostAt[0] + ',y=' + $HostAt[1] + ',z=G(' + ($HostAt -join ',') + ')+0.3},{x=0,y=0,z=0}); g_gameRules.game:MovePlayer(c.id,{x=' + $b[0] + ',y=' + $b[1] + ',z=G(' + ($b -join ',') + ')+0.3},{x=0,y=0,z=0}); pcall(function() c.actor:SetNanoSuitEnergy(200) end); System.LogAlways("[CHK] placed ' + $Tag + '")')
            Require ($null -ne (Wait-Log host '^\[CHK\] placed ' 5 $mP)) "$Tag`: both put there"
            # (it may not take the move at once and walk from where it was: once
            # more then, and longer to get there)
            for ($try = 0; $try -lt 2; $try++) {
                # (the server's MovePlayer does not move a client's own player: it
                # moves itself, then walks the last bit to the spot)
                Send-Game companion ('#' + $G + 'System.GetEntity(g_localActorId):SetWorldPos({x=' + $b[0] + ',y=' + $b[1] + ',z=G(' + ($b -join ',') + ')+0.3})')
                Pause-Game 0.5
                $mA = New-Mark
                Send-Agent ("goto " + ($b -join " ")) | Out-Null
                if (Wait-Log companion '^\[CoopAgent\] arrived: holding there' 25 $mA) { break }
                Send-Agent "hold" | Out-Null
                Send-Game host ('#' + $G + 'local c=System.GetEntityByName("Companion"); g_gameRules.game:MovePlayer(c.id,{x=' + $b[0] + ',y=' + $b[1] + ',z=G(' + ($b -join ',') + ')+0.3},{x=0,y=0,z=0})')
                Pause-Game 2
            }
            Send-Game companion '#pcall(function() System.GetEntity(g_localActorId).actor:SetNanoSuitEnergy(200) end)'
            # (the companion's game takes the host's new place only once he moves:
            # it kept him at the dome, 900 m off, and ran there; a jump on the spot)
            Send-Game host "coop_test_key jump 1"
            Pause-Game 0.3
            Send-Game host "coop_test_key jump 0"
            Pause-Game 3
            $mC = New-Mark
            Send-Game companion '#local n=System.GetEntityByName("Nomad"); if n then local p=n:GetWorldPos(); System.LogAlways(string.format("[CHK] sees the host at %.1f %.1f %.1f",p.x,p.y,p.z)) end'
            $sees = Wait-Log companion '^\[CHK\] sees the host at (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)' 5 $mC
            $seenOff = if ($sees) { [Math]::Sqrt([Math]::Pow([double]$sees.Match.Groups[1].Value - $HostAt[0], 2) + [Math]::Pow([double]$sees.Match.Groups[2].Value - $HostAt[1], 2)) } else { 99 }
            Require ($seenOff -lt 3.0) ("$Tag`: the companion's game sees the host there (" + $(if ($sees) { $sees.Text } else { 'no line' }) + ")")
            $mS = New-Mark
            Send-Game host '#local h=System.GetEntity(g_localActorId):GetWorldPos(); System.LogAlways(string.format("[CHK] host at %.1f %.1f %.1f",h.x,h.y,h.z))'
            Pause-Game 1.5
            $hz = Find-Log host '^\[CHK\] host at (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)' $mS -Last
            $w = @(Get-Watch $mS)
            $c = if ($w.Count) { $w[-1] } else { $null }
            $off = if ($c) { Get-D2 $c (Get-Pt $b) } else { 99 }
            Require ($hz -and [double]$hz.Match.Groups[3].Value -ge $HostAt[2] - 0.8) ("$Tag`: the host on top (" + $(if ($hz) { $hz.Text } else { 'no line' }) + ")")
            Require ($c -and $off -lt 2.0 -and $c.Z -le $b[2] + 0.8) ("$Tag`: the companion below the bank ({0:N1} m from its spot, z {1:N1})" -f $off, $(if ($c) { $c.Z } else { 0 }))
        }
        # one try: the 10 Hz trace, the order, until it is up, caught up, or the time is over
        function Start-Leg([string]$Order, $Goal, [double]$Within) {
            Set-WatchGoal $Goal
            if ($P.MoveTrace) { Send-Game companion "coop_test_movetrace 1" }
            Send-Game host '#AtJ=true; function AtJTick() if not AtJ then return end local c=System.GetEntityByName("Companion"); if c and c.actor then local p=c:GetWorldPos(); System.LogAlways(string.format("[CHK] j t=%.2f pos=%.2f,%.2f,%.2f fly=%d suit=%d en=%.0f",_time,p.x,p.y,p.z,c.actor:IsFlying() and 1 or 0,c.actor:GetNanoSuitMode(),c.actor:GetNanoSuitEnergy())) end Script.SetTimer(100,AtJTick) end; AtJTick()'
            $m = New-Mark
            Send-Agent $Order | Out-Null
            $end = (Get-Date).AddSeconds($Within)
            while ((Get-Date) -lt $end) {
                Pause-Game 1
                $w = @(Get-Watch $m)
                if ($w.Count -and (Test-Up $w[-1] $Goal)) { Pause-Game 1; break }
                if (Find-Log host '^\[CoopAgent\] Companion caught up with ' $m) { Pause-Game 2; break }
            }
            Send-Game host '#AtJ=false'
            if ($P.MoveTrace) { Send-Game companion "coop_test_movetrace 0" }
            Send-Agent "hold" | Out-Null
            return $m
        }
        function Measure-Leg([string]$Name, $m, $Goal) {
            $w = @(Get-Watch $m)
            Require ($w.Count -ge 4) "$Name`: the watch ran ($($w.Count) samples)"
            $tp = Find-Log host '^\[CoopAgent\] Companion caught up with ' $m
            $pre = @($w | Where-Object { -not $tp -or $_.No -lt $tp.No })
            $s0 = $w[0]
            $up = @($pre | Where-Object { Test-Up $_ $Goal } | Select-Object -First 1)
            $upT = if ($up.Count) { $up[0].T - $s0.T } else { -1 }
            $on = -1
            for ($i = 0; $i -lt $pre.Count - 1; $i++) { if ($pre[$i].Z -ge $s0.Z + 1.1 -and $pre[$i + 1].Z -ge $s0.Z + 1.1) { $on = $pre[$i].T - $s0.T; break } }
            $till = if ($up.Count) { @($pre | Where-Object { $_.No -le $up[0].No }) } else { $pre }
            $walked = ($till | Measure-Object -Property Mv -Sum).Sum
            $round = Get-RoundSeconds $till
            $j = @(Get-JTrace $m | Where-Object { (-not $tp -or $_.No -lt $tp.No) -and (-not $up.Count -or $_.No -le $up[0].No) })
            $jumps = @(Get-Jumps $j $Goal)
            $back = @($jumps | Where-Object { $_.Landed -lt 0.5 })
            $fwd = if ($back.Count) { ($back | Measure-Object -Property Forward -Average).Average } else { 0 }
            $sj = @(Find-Log companion '^\[CoopAgent\] a strength jump onto something ([\d.]+) m high' $m -All)
            $why = Find-Log companion '(put next to him|no nearer to Nomad for (18|25|33|42|52) s)' $m
            $tpT = if ($tp) { $a = @($w | Where-Object { $_.No -gt $tp.No } | Select-Object -First 1); if ($a.Count) { $a[0].T - $s0.T } else { -1 } } else { -1 }
            Check $true ("$Name`: start {0:N1} m off, {1:N1} m below; on the bank {2}; by the goal {3}; walked {4:N1} m; ways round planned {5}, host ways {6}, stalls {7}, to strength {8}; strength jump lines {9} ({10} m); 10 Hz samples in the air {11}" -f $s0.Dg, ($Goal[2] - $s0.Z),
                $(if ($on -ge 0) { "after {0:N1} s" -f $on } else { "never" }), $(if ($upT -ge 0) { "after {0:N1} s" -f $upT } else { "never by itself" }), $walked,
                (Count-Log companion '^\[CoopAgent\] a way round to Nomad close by: ' $m), (Count-Log host '^\[CoopAgent\] a way for Companion: ' $m),
                (Count-Log companion '^\[CoopAgent\] no nearer to Nomad for ' $m), (Count-Log companion '^\[CoopAgent\] suit: \S+ -> strength' $m),
                $sj.Count, (($sj | ForEach-Object { $_.Match.Groups[1].Value }) -join ','), @($j | Where-Object { $_.Fly -eq 1 }).Count)
            Check $true ("$Name`: jumps " + $(if ($jumps.Count) { ($jumps | ForEach-Object { "{0:N1} s: run-up {1:N1} m/s, up {2:N1} m, down at {3:N1} m, {4:N2} m toward, {5:N2} m sideways, {6:N1} s in the air, {7} {8:N0}" -f ($_.T - $j[0].T), $_.RunUp, $_.Up, $_.Landed, $_.Forward, $_.Side, $_.Air, $suits[[Math]::Max(0, $_.Suit)], $_.En }) -join ' | ' } else { "none seen" }))
            Check (-not $tp) ("$Name`: never put next to the host" + $(if ($tp) { " (caught up {0:N0} s in: {1})" -f $tpT, $(if ($why) { $why.Text -replace '^\[CoopAgent\] ', '' } else { 'no reason logged (the silent catch-up after the 4th stall)' }) } else { "" }))
            Check ($upT -ge 0 -and $upT -le $P.UpBy) ("$Name`: up by the goal on its own within $($P.UpBy) s" + $(if ($upT -ge 0) { " ({0:N1} s)" -f $upT } else { "" }))
            Check ($back.Count -le $P.MaxFallBacks) "$Name`: $($back.Count) of $($jumps.Count) jumps fell back (at most $($P.MaxFallBacks))"
            Check ($back.Count -eq 0 -or $fwd -ge $P.MinForward) ("$Name`: the jumps that fell back went forward up to their top ({0:N2} m toward the goal on average, at least {1}): not straight up" -f $fwd, $P.MinForward)
            Check ($round -le $P.MaxRound) ("$Name`: going round or standing within 4 m without getting 2 m nearer {0:N1} s (at most {1})" -f $round, $P.MaxRound)
            if ($P.MoveTrace) {
                $air = @(Find-Log companion '^\[CoopAgent\] mv .* fly=1 .* toward=(-?[\d.]+)' $m -All | ForEach-Object { [double]$_.Match.Groups[1].Value })
                $ahead = @($air | Where-Object { $_ -ge 0.7 }).Count
                Check ($air.Count -eq 0 -or $ahead -ge 0.8 * $air.Count) "$Name`: in the air it pressed toward the goal in $ahead of $($air.Count) frames"
            }
        }

        Step "the bank by the bridge: nobody alive near it, the watch, the places, the bank's shape" {
            Send-Game host "coop_god 1"
            Pause-Game 8
            # (soldiers still alive within 300 m: taken 250 m west and shot; the dead stay where they lie, as in the user's game)
            Send-Game host '#local p={x=1704,y=1855,z=192}; local k=0; for _,e in ipairs(System.GetEntitiesInSphere(p,300) or {}) do if e.actor and not e.actor:IsPlayer() and e.actor:GetHealth()>0 then local q={x=1450+k*4,y=1650,z=0}; q.z=System.GetTerrainElevation(q)+0.3; e:SetWorldPos(q); g_gameRules:CreateHit(e.id,g_localActorId,g_localActorId,9000,nil,nil,nil,"bullet"); k=k+1 end end; System.LogAlways("[CHK] soldiers taken off and shot: "..k)'
            Start-Watch @() $P.Top
            Set-Places $P.Top "the user's places"
            $t = $P.Top
            $mp = New-Mark
            # (the terrain's height: rays from above hit the bridge's deck, 195.6, over the companion's spot)
            Send-Game host ('#local s=""; for i=0,20 do s=s..string.format(" %.2f",System.GetTerrainElevation({x=' + $b[0] + '+(' + ($t[0] - $b[0]) + ')*i/20,y=' + $b[1] + '+(' + ($t[1] - $b[1]) + ')*i/20,z=0})) end; System.LogAlways("[CHK] profile"..s)')
            $pr = Wait-Log host '^\[CHK\] profile((?: -?[\d.]+)+)$' 5 $mp
            $zs = if ($pr) { @($pr.Match.Groups[1].Value.Trim() -split ' ' | ForEach-Object { [double]$_ }) } else { @() }
            $step = 0.0; $at = -1
            for ($i = 0; $i + 2 -lt $zs.Count; $i++) { if ($zs[$i + 2] - $zs[$i] -gt $step) { $step = $zs[$i + 2] - $zs[$i]; $at = $i } }
            $len = Get-D2 (Get-Pt $b) (Get-Pt $t)
            Require ($zs.Count -eq 21 -and $step -ge 0.9) ("the bank is there: {0:N1} m up within 1 m, {1:N1} m from the companion's spot (ground: {2})" -f $step, ($at * $len / 20), ($zs -join ' '))
            # what its squares, the host's navigation and the physics make of it (logs only)
            Send-Game companion ("coop_test_grid " + ($b -join " ") + " 10 " + ($t -join " "))
            Send-Game host "coop_test_path Companion"
            Send-Game host ("coop_test_findobj " + ((($b[0] + $t[0]) / 2), (($b[1] + $t[1]) / 2), ($b[2] + 1.5) -join " ") + " 4")
            Pause-Game 1
            $gw = Find-Log companion '^\[CoopTest\] grid way from ' -Last
            $hp = Find-Log host '^\[CoopTest\] way to Companion ' -Last
            Check $true ("its squares: " + $(if ($gw) { $gw.Text -replace '^\[CoopTest\] ', '' } else { 'no line' }) + "; the host's navigation: " + $(if ($hp) { $hp.Text -replace '^\[CoopTest\] ', '' } else { 'no line' }))
        }
        Step "following: the host on top 9 m off (the user's places)" {
            $m = Start-Leg "follow" $P.Top $P.Within
            Measure-Leg "following" $m $P.Top
        }
        if ($P.GotoLeg) {
            Step "goto the host's spot (no catch-up there): up by itself" {
                Pause-Game $P.Forget
                Set-Places $P.Top "goto"
                $m = Start-Leg ("goto " + ($P.Top -join " ")) $P.Top $P.GotoWithin
                Measure-Leg "goto" $m $P.Top
            }
        }
        if ($P.NearLeg) {
            Step "following: the host right above the bank, 4 m off (its first jump that day)" {
                Pause-Game $P.Forget
                Set-Places $P.Near "near"
                $m = Start-Leg "follow" $P.Near $P.Within
                Measure-Leg "near" $m $P.Near
            }
        }
        Stop-Watch
        Send-Agent "follow" | Out-Null
    }
}
