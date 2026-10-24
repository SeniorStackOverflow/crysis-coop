# The user's games of 2026-10-17..23 (Game.log of 10-23, lines 641-699): the
# AI companion joined the continued campaign (Rescue_08 by the dome) 25 s
# after the load while the host walked about, so the world was not "ready" yet
# (he had not stood still 8 s, the 40 s fallback had not come): the server
# held it as a spectator ("[Coop] holding Nomad(1) until the host has landed",
# CoopGameRules.lua, JOIN_WAIT_MAX 15 s). The server ran that spectator's view
# through the host's eyes and hid the host's own player there
# (IEntity::Invisible, in this engine no physics): he could not move, the
# "[CoopPos] server: Nomad=(1224,2575,244)" lines stayed the same, "waiting
# joiners: ... notInWorld=flying ... speed=0", and the joiner waited the whole
# 15 s ("world ready: a joiner waited 15s, the host stays where he is (flying,
# speed 0)"). The fix (PlayerView.cpp): only the viewer's own game hides the
# player it looks through.
# Here: the host on open ground by the bridge (fixture dome1022; the soldiers
# within 400 m hidden away, none killed: on the fixed build a soldier's death
# reaches the level's flow graph, on the old one not, and a flow graph could
# send more), the companion started late (Companion 'late', then
# coop_companion 1). A test companion's game takes 40 s and more to start; by
# then the rules have the world ready and nobody is held. So until the
# companion is held the host's watch keeps the server's "the host has not
# landed yet" as right after the load (coopWorldReady off, the settle,
# fallback and wait counters at 0); from the hold on the rules run untouched.
# The moment the hold begins, the host's game presses forward for 2 s (from
# its own Lua: no runner delay, the same every run); 4 times a second it logs
# his position, the physics' "flying", the rules' not-in-world reason and
# speed (CoopNotInWorld, coopSpeed: what the hold waits on).
# Old build 0.11.45: expected FAIL - he moves ~0 m in the hold, the samples
# while he presses forward say flying at speed 0, the server's "waiting
# joiners" line (tick 10) says notInWorld=flying speed=0, his [CoopPos] place
# does not change, and the hold ends at the 15 s limit ("a joiner waited 15s").
# Fixed build: expected PASS - he walks ~6 m in the hold, never flying at
# speed 0, the [CoopPos] place moves with him, and the hold ends once he has
# stood 8 s ("world ready: leader settled", 8-12 s after it began).
@{
    Name = "join-host-moves"
    Tags = @("join", "companion", "diag", "regress")
    SummaryRu = "ИИ-компаньон входит в продолженную кампанию, пока хост «не приземлился» (его держат зрителем): хост в это время ходит, а не застывает «в полёте», и ожидание кончается, как только он встал (на 0.11.45 хост замирал на все 15 с)"
    Session = @{ Fixture = "save:dome1022"; Campaign = "56c79107e0d7a6ff"; Companion = "late"; ReadyCommands = @("coop_god 1") }
    Params = @{
        # (the road by the bridge: open ground, companion-gunner-hits' car stands there)
        Spot = @(1737, 1960); Clear = 400; MinFree = 6
        # (pressed 2 s: ~6 m; then 8 s still ends the hold, ~3 s before the 15 s limit)
        Press = 2.0; MinMove = 2.0; JoinWithin = 300; HoldWithin = 45; WaitMax = 15
    }
    Body = {
        param($P)
        # the host's samples: _time, position, the physics' flying, the rules' reason he is not in the world, speed, a joiner held
        $rx = '^\[CHK\] jh t=(-?[\d.]+) pos=(-?[\d.]+),(-?[\d.]+),(-?[\d.]+) fly=(\d) niw=(\S+) spd=(\S+) held=(\d)'
        function Get-JH($Since) {
            @(Find-Log host $rx $Since -All | ForEach-Object { $g = $_.Match.Groups
                [pscustomobject]@{ No = $_.No; T = [double]$g[1].Value; X = [double]$g[2].Value; Y = [double]$g[3].Value; Z = [double]$g[4].Value
                    Fly = [int]$g[5].Value; Niw = $g[6].Value; Spd = $(if ($g[7].Value -eq 'nil') { -1.0 } else { [double]$g[7].Value }); Held = [int]$g[8].Value } })
        }
        function Get-D2($a, $c) { [Math]::Sqrt([Math]::Pow($a.X - $c.X, 2) + [Math]::Pow($a.Y - $c.Y, 2)) }
        function Show-JH($S) { if ($S) { "t {0:N2}: {1:N1} {2:N1} {3:N1} fly={4} niw={5} speed={6}" -f $S.T, $S.X, $S.Y, $S.Z, $S.Fly, $S.Niw, $(if ($S.Spd -lt 0) { 'nil' } else { "{0:N2}" -f $S.Spd }) } else { 'none' } }
        # the host's place in a "[CoopPos] server:" line (whole metres)
        function Get-PosHost($L) {
            if ($L -and $L.Text -match ' Nomad=\((-?\d+),(-?\d+),(-?\d+)\)hp') { return [pscustomobject]@{ No = $L.No; X = [double]$Matches[1]; Y = [double]$Matches[2]; Z = [double]$Matches[3] } }
            return $null
        }

        Step "the host on open ground by the bridge, nobody alive near him, the join watch on" {
            Send-Game host "coop_god 1"
            Pause-Game 8
            $s = $P.Spot
            # (soldiers alive near the spot: hidden, those on foot first taken 650 m off, a crew with its
            # vehicle; none shot - no fight, no grenade pushing the host, no flow graph sending more)
            Send-Game host ('#local p={x=' + $s[0] + ',y=' + $s[1] + ',z=0}; p.z=System.GetTerrainElevation(p); local k,v=0,0; for _,e in ipairs(System.GetEntitiesInSphere(p,' + $P.Clear + ') or {}) do if e.actor and not e.actor:IsPlayer() and e.actor:GetHealth()>0 and not e:IsHidden() then local vi=e.actor:GetLinkedVehicleId(); local ve=vi and System.GetEntity(vi); if ve then ve:Hide(1); v=v+1 else local q={x=1250+k*4,y=1500,z=0}; q.z=System.GetTerrainElevation(q)+0.3; e:SetWorldPos(q) end; e:Hide(1); k=k+1 end end; System.LogAlways("[CHK] soldiers near the spot hidden away (none killed): "..k..", their vehicles "..v)')
            Send-Game host ('#local t={x=' + $s[0] + ',y=' + $s[1] + ',z=0}; t.z=System.GetTerrainElevation(t)+0.3; g_gameRules.game:MovePlayer(g_localActorId,t,{x=0,y=0,z=0}); System.LogAlways("[CHK] the host on the road by the bridge")')
            Pause-Game 2
            # (a jump: put there, he stands on his feet, not "flying" from the move)
            Send-Game host "coop_test_key jump 1"
            Pause-Game 0.3
            Send-Game host "coop_test_key jump 0"
            Pause-Game 3
            # the watch's parts: the rules' state as right after a load (until a joiner is held), the way ahead, one sample
            Send-Game host ('#AtJH={on=true,held=false,n=0,press=' + $P.Press + '}; function AtJHUp() System.ExecuteCommand("coop_test_key moveforward 0") end; function AtJHPre(g) g.coopWorldReady=false; g.coopLevelTicks=0; g.coopSettledTicks=0; g.coopJoinWait=0; g.coopRelaxedSpawn=nil; g.coopIdleSpawn=nil end; System.LogAlways("[CHK] join watch set")')
            Send-Game host '#function AtJHDir(p) local best,yaw=-1,0; for k=0,7 do local a=k*math.pi/4; local dx,dy=-math.sin(a),math.cos(a); local free=12; for _,hz in ipairs({0.5,1.2}) do if Physics.RayWorldIntersection({x=p.x,y=p.y,z=p.z+hz},{x=dx*12,y=dy*12,z=0},1,ent_terrain+ent_static+ent_rigid+ent_sleeping_rigid,g_localActorId,nil,g_HitTable)>0 then local h=g_HitTable[1]; if h and h.pos then free=math.min(free,math.sqrt((h.pos.x-p.x)^2+(h.pos.y-p.y)^2)) end end end; for s=2,12,2 do if math.abs(System.GetTerrainElevation({x=p.x+dx*s,y=p.y+dy*s,z=0})-p.z)>1.5 then free=math.min(free,s-2) end end; if free>best then best,yaw=free,a end end; return best,yaw end'
            Send-Game host '#function AtJHLine(g,h,w) local p=h:GetWorldPos(); local s=g.coopSpeed and g.coopSpeed[h.id]; local niw=g:CoopNotInWorld(h); return string.format("[CHK] jh t=%.2f pos=%.2f,%.2f,%.2f fly=%d niw=%s spd=%s held=%d",_time,p.x,p.y,p.z,h.actor:IsFlying() and 1 or 0,niw and (string.gsub(niw," ","_")) or "nil",s and string.format("%.2f",s) or "nil",w and 1 or 0) end'
            # (the way he will walk: the most open of 8 directions, 12 m of rays at knee and chest height, the ground within 1.5 m)
            $mW = New-Mark
            Send-Game host '#local p=System.GetEntity(g_localActorId):GetWorldPos(); local q={x=p.x,y=p.y,z=p.z}; local f,a=AtJHDir(q); g_localActor:SetAngles({x=0,y=0,z=a}); System.LogAlways(string.format("[CHK] the way: %.1f m free at yaw %.2f from %.1f %.1f %.1f",f,a,q.x,q.y,q.z))'
            $way = Wait-Log host '^\[CHK\] the way: (-?[\d.]+) m free at yaw (-?[\d.]+) from (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)' 5 $mW
            $off = if ($way) { Get-D2 ([pscustomobject]@{ X = [double]$way.Match.Groups[3].Value; Y = [double]$way.Match.Groups[4].Value }) ([pscustomobject]@{ X = $s[0]; Y = $s[1] }) } else { 99 }
            Require ($way -and $off -lt 3.0) ("the host is on the spot (" + $(if ($way) { "{0:N1} m from it" -f $off } else { 'no line' }) + ")")
            Require ([double]$way.Match.Groups[1].Value -ge $P.MinFree) "open ground ahead of him: $($way.Text -replace '^\[CHK\] ', '')"
            Pause-Game 1
            Send-Game host ('#g_localActor:SetAngles({x=0,y=0,z=' + $way.Match.Groups[2].Value + '})')
            # the watch: until a joiner is held the rules' state as after a load; at the hold forward for Press s; samples 1/s, in the hold 4/s
            $script:m0 = New-Mark
            Send-Game host '#function AtJHTick() if not AtJH.on then return end local g=g_gameRules; local h=System.GetEntity(g_localActorId); local w=g.coopWaiting~=nil and next(g.coopWaiting)~=nil; if not AtJH.held then if w then AtJH.held=true; AtJH.t0=_time; System.LogAlways("[CHK] hold on, the rules at tick "..tostring(g.coopLevelTicks)); System.ExecuteCommand("coop_test_key moveforward 1"); Script.SetTimer(math.floor(AtJH.press*1000),AtJHUp) else AtJHPre(g) end elseif not w and not AtJH.off then AtJH.off=_time; System.LogAlways(string.format("[CHK] hold off after %.2f s",_time-AtJH.t0)) end; AtJH.n=AtJH.n+1; if AtJH.n>=4 then AtJH.n=0 end; if h and h.actor and (AtJH.held or AtJH.n==0) then System.LogAlways(AtJHLine(g,h,w)) end; Script.SetTimer(250,AtJHTick) end; AtJHTick()'
            Pause-Game 3
            $pre = @(Get-JH $script:m0)
            Require ($pre.Count -ge 1 -and $null -eq (Find-Log host '^\[CHK\] hold on' $script:m0)) "the watch runs, nobody held yet ($($pre.Count) samples)"
            Require ($pre[-1].Niw -eq 'nil' -and $pre[-1].Fly -eq 0) "the host stands in the world ($(Show-JH $pre[-1]))"
        }
        Step "the companion joins late: held until the host has landed, he walks meanwhile" {
            $script:mJ = New-Mark
            Send-Game host "coop_companion 1"
            $st = Wait-Log host '^\[CoopAgent\] AI companion started' 30 $script:mJ
            Require ($null -ne $st) "the companion's game started"
            $script:hold = Wait-Log host '^\[Coop\] holding (\S+) until the host has landed' $P.JoinWithin $script:mJ
            $came = if (-not $script:hold) { Find-Log host 'renamed to (\$\d)?Companion|revived ' $script:mJ } else { $null }
            Require ($null -ne $script:hold) ("the joining companion was held as a spectator, as in the user's game" + $(if ($came) { " (it came in without a hold: $($came.Text))" } else { "" }))
            $script:on = Wait-Log host '^\[CHK\] hold on' 5 $script:mJ
            Require ($null -ne $script:on) "the host's watch saw the hold begin and pressed forward"
            $script:off = Wait-Log host '^\[CHK\] hold off after ([\d.]+) s' $P.HoldWithin $script:mJ
            $script:in = Wait-Log host 'renamed to (\$\d)?Companion|revived Companion' 120 $script:mJ
            Pause-Game 2
            Send-Game host '#AtJH.on=false; System.LogAlways("[CHK] join watch off")'
            Send-Game host "coop_test_key moveforward 0"
        }
        Step "the host in the hold" {
            $hold = $script:hold
            $all = @(Get-JH $script:m0)
            $before = @($all | Where-Object { $_.No -lt $script:on.No } | Select-Object -Last 1)
            Require ($before.Count -and $before[0].Niw -eq 'nil' -and $before[0].Fly -eq 0) "right before the join the host stood in the world ($(Show-JH $before[0]))"
            $held = @($all | Where-Object { $_.Held -eq 1 })
            Require ($held.Count -ge 4) "the watch sampled the hold ($($held.Count) samples)"
            $s0 = $held[0]
            $press = Find-Log host '^\[CoopTest\] \S+ presses moveforward' $script:mJ
            $rel = Find-Log host '^\[CoopTest\] \S+ lets go of moveforward' $script:mJ
            Require ($null -ne $press) "the host pressed forward in the hold"
            $inPress = @($held | Where-Object { $_.No -gt $press.No -and (-not $rel -or $_.No -lt $rel.No) })
            # how far he got: the farthest sample from where the hold found him; his way while he pressed
            $moved = ($held | ForEach-Object { Get-D2 $_ $s0 } | Measure-Object -Maximum).Maximum
            $walked = 0.0
            for ($i = 1; $i -lt $inPress.Count; $i++) { $walked += Get-D2 $inPress[$i] $inPress[$i - 1] }
            $top = if ($inPress.Count) { ($inPress | Measure-Object -Property Spd -Maximum).Maximum } else { -1 }
            $stuck = @($inPress | Where-Object { $_.Niw -eq 'flying' -and $_.Spd -ge 0 -and $_.Spd -le 0.05 })
            $flyAny = @($held | Where-Object { $_.Fly -eq 1 }).Count
            # how the hold ended: the rules' line, the watch's time
            $ready = @(Find-Log host '^\[Coop\] world ready(.*)$' $script:mJ -All | Where-Object { $_.No -gt $hold.No } | Select-Object -First 1)
            $endNo = if ($ready.Count) { $ready[0].No } elseif ($script:off) { $script:off.No } else { [int]::MaxValue }
            $dur = if ($script:off) { [double]$script:off.Match.Groups[1].Value } else { -1 }
            $waited = if ($ready.Count -and $ready[0].Text -match 'a joiner waited (\d+)s') { [int]$Matches[1] } else { -1 }
            # the server's own report of the hold (every 10th tick of the rules)
            $wj = @(Find-Log host '^\[Coop\] waiting joiners: leader (\S+) pos=\(([^)]*)\) notInWorld=(.*) settled=(\S+) speed=(\S+) cutscene' $script:mJ -All | Where-Object { $_.No -gt $hold.No -and $_.No -lt $endNo })
            $wjStill = @($wj | Where-Object { $sp = 0.0; $_.Match.Groups[3].Value -eq 'flying' -and [double]::TryParse($_.Match.Groups[5].Value, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$sp) -and $sp -le 0.05 })
            # his place in the server's [CoopPos] lines: the last before the hold, the last in it
            $cp = @(Find-Log host '^\[CoopPos\] server:.* Nomad=\(' $script:m0 -All)
            $p0 = Get-PosHost (@($cp | Where-Object { $_.No -lt $hold.No }) | Select-Object -Last 1)
            $p1 = Get-PosHost (@($cp | Where-Object { $_.No -gt $hold.No -and $_.No -lt $endNo }) | Select-Object -Last 1)

            Check $true ("the hold: " + $hold.Text + "; it lasted " + $(if ($dur -ge 0) { "{0:N1} s" -f $dur } else { "more than $($P.HoldWithin) s" }) + "; " + $(if ($ready.Count) { $ready[0].Text } else { 'no world-ready line' }) + "; first sample " + (Show-JH $s0) + "; " + $(if ($wj.Count) { ($wj | ForEach-Object { $_.Text -replace '^\[Coop\] waiting joiners: ', '' }) -join ' | ' } else { 'no waiting-joiners line in it' }))
            Check ($moved -ge $P.MinMove) ("the host moved {0:N1} m in the hold (his own position 4 times a second, at least {1} m): {2:N1} m while he pressed forward ({3} samples, top speed {4:N1} m/s), flying in {5} of {6} samples; last {7}" -f $moved, $P.MinMove, $walked, $inPress.Count, $top, $flyAny, $held.Count, (Show-JH $held[-1]))
            Check ($inPress.Count -ge 2 -and $stuck.Count -eq 0) ("while he pressed forward never 'flying' at speed 0 ({0} of {1} samples{2})" -f $stuck.Count, $inPress.Count, $(if ($stuck.Count) { "; first: " + (Show-JH $stuck[0]) } else { "" }))
            Check ($wjStill.Count -eq 0) ("the server's 'waiting joiners' lines in the hold: none with notInWorld=flying at speed 0 ({0} of {1}{2})" -f $wjStill.Count, $wj.Count, $(if ($wjStill.Count) { ": " + ($wjStill[0].Text -replace '^\[Coop\] ', '') } else { "" }))
            if ($p0 -and $p1) {
                $dp = Get-D2 $p1 $p0
                # (whole metres there: half a metre less)
                Check ($dp -ge $P.MinMove - 0.5) ("his [CoopPos] place on the server moved {0:N1} m in the hold ({1:N0},{2:N0} -> {3:N0},{4:N0}; at least {5} m)" -f $dp, $p0.X, $p0.Y, $p1.X, $p1.Y, ($P.MinMove - 0.5))
            } else {
                Check $true ("no [CoopPos] line " + $(if (-not $p0) { "before the hold" } else { "in the hold" }) + " to compare")
            }
            Check ($dur -ge 0) ("the hold ended within $($P.HoldWithin) s" + $(if ($dur -ge 0) { " ({0:N1} s)" -f $dur } else { "" }))
            Check ($ready.Count -and $ready[0].Text -match 'leader settled' -and $waited -lt 0) ("the hold ended as soon as the host stood settled, not at the joiner's $($P.WaitMax) s limit: " + $(if ($ready.Count) { ($ready[0].Text -replace '^\[Coop\] ', '') } else { 'no world-ready line' }) + $(if ($dur -ge 0) { " ({0:N1} s)" -f $dur } else { "" }))
            Check ($null -ne $script:in) ("the companion came into the game after the hold" + $(if ($script:in) { ": " + $script:in.Text } else { "" }))
        }
    }
}
