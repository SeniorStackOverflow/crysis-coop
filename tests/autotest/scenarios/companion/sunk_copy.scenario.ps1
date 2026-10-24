# The user's game of 2026-10-23 (campaign "checking", Rescue_10 at the
# graveyard; Game.log 6323-6325): after a revive the host's copy of the AI
# companion fell through the ground while its own game had it standing at
# z 179. Its position report (CoopAgent.cpp OnServerRequest op 7, sent every
# ~2 s) trusted the host's copy: "Companion was 0.0 m off in its own game (at
# 2587.1 1518.5 179.0, here 2587.1 1518.5 145.8): put back here" - put 33 m
# under the map - and the stop of that fall counted as a landing: "DAMAGE
# player Companion by Companion type= hit=531.0 ... hp 200->-172", down, the
# body sank to z 88.
# Here, on open flat ground (found at run time: 5 m round within 0.6 m, no
# water, nothing over it or within 6 m) with nobody alive within 300 m: the
# companion holds still there, mortal (coop_god 2: only the host immortal;
# pl_debugFallDamage 1 logs every landing the server counts); on the host only
# the server's copy of it is put 30 m straight down (SetWorldPos on the
# entity, not MovePlayer: its own game is not told, and a player standing still
# sends no new place); 10 s watched: the copy at 10 Hz on the host, its own
# place at 4 Hz in its own game. Then once more, the copy 30 m down and 3 m
# aside (op 7 takes a copy 2 m or more aside only on its 2nd report: nothing
# but its own game's update can bring it up first), and the companion walks
# off 1 s later: that update brings the copy up, no landing either.
# Old build 0.11.45 (expected FAIL): 2-4 s after the drop "Companion was 0.0 m
# off in its own game (at .. z, here .. z-30 and lower): put back here" - put
# down to the sunk copy; then fall damage by itself ("DAMAGE/HIT player
# Companion by Companion") and "Companion is down", or under the map in its
# own game, and the host's copy far under the ground, never back up. In the
# walking leg the copy's landing as its update brings it up is fall damage
# when it fell faster than the safe 8 m/s by then (the leg's line says how
# fast).
# Fixed build (expected PASS): within ~2 s "Companion sank 30.x m here (z ..,
# the ground ..), stands at z .. in its own game: put back up here"; no fall
# damage, alive in both games, on the ground in its own game, the host's copy
# back within 2 m of the ground where its own game has it; walking: the copy
# comes up with its walk, no landing counted (a z jump up > 1 m in a frame).
@{
    Name = "companion-sunk-copy"
    Tags = @("companion", "sync", "regress")
    SummaryRu = "Копия компаньона на хосте провалилась на 30 м под землю, а в своей игре он стоит (кладбище, игра 23.10): хост поднимает копию к нему, урона от падения нет, компаньон жив и не уходит под карту; то же, когда он сразу идёт"
    Session = @{ Fixture = "new:rescue"; Companion = $true }
    Params = @{
        Drop = 30; Watch = 10; BackWithin = 5
        # the walking leg: 3 m aside (west), the walk 1 s after the drop, 4 m east
        WalkLeg = $true; Aside = 3; WalkAfter = 1.0; Walk = 4
        # where to look for the flat spot besides the host's start: the field
        # south of the Rescue huts (mg_nest's "Back"), the farm, the road by the bridge
        Centers = @(@(1650, 2170), @(994, 2060), @(1737, 1960)); Flat = 0.6; Clear = 300
    }
    Body = {
        param($P)
        $St = @{}
        function Get-D2($a, $c) { [Math]::Sqrt([Math]::Pow($a.X - $c.X, 2) + [Math]::Pow($a.Y - $c.Y, 2)) }
        # where the companion is: the host's copy ("copy") or its own game ("own")
        $tail = ': (-?[\d.]+) (-?[\d.]+) (-?[\d.]+) hp (-?\d+), the ground (-?[\d.]+)$'
        function ConvertTo-Place($l) {
            if (-not $l) { return $null }
            $g = $l.Match.Groups
            [pscustomobject]@{ X = [double]$g[1].Value; Y = [double]$g[2].Value; Z = [double]$g[3].Value; Hp = [int]$g[4].Value; Ground = [double]$g[5].Value; Text = ($l.Text -replace '^\[CHK\] ', '') }
        }
        function Get-State([string]$Tag) {
            $m = New-Mark
            Send-Game host ('#local c=System.GetEntityByName("Companion"); if c and c.actor then local p=c:GetWorldPos(); local x,y,z=p.x,p.y,p.z; System.LogAlways(string.format("[CHK] copy ' + $Tag + ': %.2f %.2f %.2f hp %d, the ground %.2f",x,y,z,c.actor:GetHealth(),System.GetTerrainElevation({x=x,y=y,z=0}))) end')
            Send-Game companion ('#local e=System.GetEntity(g_localActorId); if e and e.actor then local p=e:GetWorldPos(); local x,y,z=p.x,p.y,p.z; System.LogAlways(string.format("[CHK] own ' + $Tag + ': %.2f %.2f %.2f hp %d, the ground %.2f",x,y,z,e.actor:GetHealth(),System.GetTerrainElevation({x=x,y=y,z=0}))) end')
            $h = Wait-Log host ('^\[CHK\] copy ' + [regex]::Escape($Tag) + $tail) 10 $m
            $c = Wait-Log companion ('^\[CHK\] own ' + [regex]::Escape($Tag) + $tail) 10 $m
            return [pscustomobject]@{ Copy = (ConvertTo-Place $h); Own = (ConvertTo-Place $c) }
        }
        # the 10 Hz trace of the copy on the host, the 4 Hz one in its own game
        function Get-STrace($Since) {
            $rx = '^\[CHK\] s t=(-?[\d.]+) pos=(-?[\d.]+),(-?[\d.]+),(-?[\d.]+) hp=(-?\d+) fly=(\d)$'
            @(Find-Log host $rx $Since -All | ForEach-Object { $g = $_.Match.Groups
                [pscustomobject]@{ No = $_.No; T = [double]$g[1].Value; X = [double]$g[2].Value; Y = [double]$g[3].Value; Z = [double]$g[4].Value; Hp = [int]$g[5].Value; Fly = [int]$g[6].Value } })
        }
        function Get-OTrace($Since) {
            $rx = '^\[CHK\] own t=(-?[\d.]+) pos=(-?[\d.]+),(-?[\d.]+),(-?[\d.]+) hp=(-?\d+) ground=(-?[\d.]+)$'
            @(Find-Log companion $rx $Since -All | ForEach-Object { $g = $_.Match.Groups
                [pscustomobject]@{ No = $_.No; T = [double]$g[1].Value; X = [double]$g[2].Value; Y = [double]$g[3].Value; Z = [double]$g[4].Value; Hp = [int]$g[5].Value; Ground = [double]$g[6].Value } })
        }
        # the companion held at the spot, the host 8 m south of it looking at it
        # (the server's MovePlayer does not move a client's own player: it moves
        # itself there in its own game, then a goto there at once - an old hold
        # spot makes it walk back)
        function Set-Spot([string]$Tag) {
            $S = $St.S
            Send-Agent "hold" | Out-Null
            $mP = New-Mark
            Send-Game host ('#local c=System.GetEntityByName("Companion"); local function at(x,y) return {x=x,y=y,z=System.GetTerrainElevation({x=x,y=y,z=0})+0.3} end; g_gameRules.game:MovePlayer(g_localActorId,at(' + $S.X + ',' + ($S.Y - 8) + '),{x=0,y=0,z=0}); g_gameRules.game:MovePlayer(c.id,at(' + $S.X + ',' + $S.Y + '),{x=0,y=0,z=0}); System.LogAlways("[CHK] placed ' + $Tag + '")')
            Require ($null -ne (Wait-Log host '^\[CHK\] placed ' 5 $mP)) "$Tag`: both put there"
            $at = $null
            for ($i = 0; $i -lt 6; $i++) {
                Send-Game companion ('#local t={x=' + $S.X + ',y=' + $S.Y + ',z=0}; t.z=System.GetTerrainElevation(t)+0.3; System.GetEntity(g_localActorId):SetWorldPos(t)')
                Pause-Game 0.3
                $mA = New-Mark
                Send-Agent ("goto " + $S.X + " " + $S.Y + " " + $S.Z) | Out-Null
                [void](Wait-Log companion '^\[CoopAgent\] arrived: holding there' 6 $mA)
                $mC = New-Mark
                Send-Game companion '#local p=System.GetEntity(g_localActorId):GetWorldPos(); System.LogAlways(string.format("[CHK] companion at %.1f %.1f %.1f",p.x,p.y,p.z))'
                $at = Wait-Log companion '^\[CHK\] companion at (-?[\d.]+) (-?[\d.]+)' 3 $mC
                if ($at -and [Math]::Abs([double]$at.Match.Groups[1].Value - $S.X) -lt 1.0 -and [Math]::Abs([double]$at.Match.Groups[2].Value - $S.Y) -lt 1.0) { break }
            }
            Require ($at -and [Math]::Abs([double]$at.Match.Groups[1].Value - $S.X) -lt 1.5 -and [Math]::Abs([double]$at.Match.Groups[2].Value - $S.Y) -lt 1.5) ("$Tag`: the companion holds at the spot in its own game (" + $(if ($at) { $at.Text } else { 'no line' }) + ")")
            # (a host moved by MovePlayer: the companion's game sees him there
            # only once he moves - a jump on the spot)
            Send-Game host "coop_test_key jump 1"
            Pause-Game 0.3
            Send-Game host "coop_test_key jump 0"
            Pause-Game 3
            $mF = New-Mark
            Send-Game host '#local c=System.GetEntityByName("Companion"); local n=0; for _,a in ipairs(System.GetEntitiesByClass("ForbiddenArea") or {}) do if a.warning and a.warning[c.id] and tonumber(a.Properties.bEnabled)~=0 then n=n+1 end end; System.LogAlways("[CHK] forbidden areas counting down for the companion: "..n)'
            $fa = Wait-Log host '^\[CHK\] forbidden areas counting down for the companion: (\d+)$' 5 $mF
            Require ($fa -and $fa.Match.Groups[1].Value -eq '0') ("$Tag`: the companion is in the play area (" + $(if ($fa) { $fa.Text -replace '^\[CHK\] ', '' } else { 'no line' }) + ")")
        }
        # one drop of the server's copy: $Aside m east (negative: west), $Drop m
        # down; $WalkAfter >= 0: the companion walks $Walk m east that many s later
        function Invoke-Drop([string]$Tag, [double]$Aside, [double]$WalkAfter) {
            $walks = $WalkAfter -ge 0
            Set-Spot $Tag
            $b = Get-State "$Tag before"
            Require ($b.Copy -and $b.Own) "$Tag`: both games say where the companion is"
            Require ($b.Copy.Hp -gt 0 -and $b.Own.Hp -gt 0) "$Tag`: the companion is alive (host $($b.Copy.Hp), its own game $($b.Own.Hp))"
            Require ((Get-D2 $b.Own $St.S) -lt 1.5 -and [Math]::Abs($b.Own.Z - $b.Own.Ground) -lt 0.6) "$Tag`: it stands on the ground at the spot in its own game ($($b.Own.Text))"
            Require ((Get-D2 $b.Copy $b.Own) -lt 1.5 -and [Math]::Abs($b.Copy.Z - $b.Own.Z) -lt 1.0) "$Tag`: the host's copy stands where its own game has it ($($b.Copy.Text))"
            Send-Game companion '#AtO=true; function AtOTick() if not AtO then return end local e=System.GetEntity(g_localActorId); if e and e.actor then local p=e:GetWorldPos(); local x,y,z=p.x,p.y,p.z; System.LogAlways(string.format("[CHK] own t=%.2f pos=%.2f,%.2f,%.2f hp=%d ground=%.2f",_time,x,y,z,e.actor:GetHealth(),System.GetTerrainElevation({x=x,y=y,z=0}))) end Script.SetTimer(250,AtOTick) end; AtOTick()'
            Send-Game host '#AtS=true; AtSTick()'
            # mortal from here: only the host immortal
            Send-Game host "coop_god 2"
            Pause-Game 1
            $m = New-Mark
            if (-not $St.FirstDrop) { $St.FirstDrop = $m }
            Send-Game host ('#local c=System.GetEntityByName("Companion"); local p=c:GetWorldPos(); local x,y,z=p.x,p.y,p.z; local g=System.GetTerrainElevation({x=x,y=y,z=0}); c:SetWorldPos({x=x+(' + $Aside + '),y=y,z=z-(' + $P.Drop + ')}); local q=c:GetWorldPos(); System.LogAlways(string.format("[CHK] dropped ' + $Tag + ' at t=%.2f: %.2f %.2f %.2f -> %.2f %.2f %.2f, the ground %.2f, hp %d",_time,x,y,z,q.x,q.y,q.z,g,c.actor:GetHealth()))')
            $dl = Wait-Log host ('^\[CHK\] dropped ' + [regex]::Escape($Tag) + ' at t=(-?[\d.]+): (-?[\d.]+) (-?[\d.]+) (-?[\d.]+) -> (-?[\d.]+) (-?[\d.]+) (-?[\d.]+), the ground (-?[\d.]+), hp (-?\d+)$') 5 $m
            $dropT = if ($dl) { [double]$dl.Match.Groups[1].Value } else { 0 }
            $fromZ = if ($dl) { [double]$dl.Match.Groups[4].Value } else { 0 }
            $toZ = if ($dl) { [double]$dl.Match.Groups[7].Value } else { 0 }
            Require ($dl -and $fromZ - $toZ -ge $P.Drop - 5) ("$Tag`: the server's copy went $($P.Drop) m down (" + $(if ($dl) { $dl.Text -replace '^\[CHK\] ', '' } else { 'no line' }) + ")")
            $mW = $null
            $r = $null
            if ($walks) {
                Pause-Game $WalkAfter
                $mW = New-Mark
                $r = Send-Agent ("move east " + $P.Walk)
                Pause-Game ([Math]::Max(0, $P.Watch - $WalkAfter))
            } else {
                Pause-Game $P.Watch
            }
            Send-Game host '#AtS=false'
            Send-Game companion '#AtO=false'
            $a = Get-State "$Tag after"
            Send-Game host "coop_god 1"

            # the copy on the host after the drop: lowest, back up (within 2 m of
            # where it stood), how fast it fell right before
            $tr = @(Get-STrace $m | Where-Object { $_.No -gt $dl.No })
            $stand = $b.Copy.Z
            $pts = @([pscustomobject]@{ No = $dl.No; T = $dropT; Z = $toZ }) + $tr
            $low = ($pts | Measure-Object -Property Z -Minimum).Minimum
            $k = -1
            for ($i = 1; $i -lt $pts.Count; $i++) { if ($pts[$i].Z -ge $stand - 2.0) { $k = $i; break } }
            $backT = if ($k -ge 1) { $pts[$k].T - $dropT } else { -1 }
            $speed = if ($k -ge 2) { ($pts[$k - 2].Z - $pts[$k - 1].Z) / [Math]::Max(0.01, $pts[$k - 1].T - $pts[$k - 2].T) } else { 0 }
            # its own game after the drop: how far it moved, how low it went
            $own = @(Get-OTrace $m)
            $moved = 0.0
            $ownLow = $b.Own.Z
            foreach ($o in $own) { $moved = [Math]::Max($moved, (Get-D2 $o $b.Own)); $ownLow = [Math]::Min($ownLow, $o.Z) }
            # what op 7 did: put it back up (fixed), put it here (old), left it
            $sank = @(Find-Log host '^\[CoopAgent\] Companion sank ([\d.]+) m here \(z (-?[\d.]+), the ground (-?[\d.]+)\), stands at z (-?[\d.]+) in its own game: put back up here' $m -All)
            $offs = @(Find-Log host '^\[CoopAgent\] Companion was ([\d.]+) m off in its own game \((?:-?[\d.]+ up or down; )?at (-?[\d.]+) (-?[\d.]+) (-?[\d.]+), here (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)\): put back here' $m -All)
            $offsDown = @($offs | Where-Object { [double]$_.Match.Groups[7].Value -lt [double]$_.Match.Groups[4].Value - 2.5 })
            $under = @(Find-Log host '^\[CoopAgent\] Companion is ([\d.]+) m under the ground here' $m -All)
            $op7 = @(@($sank) + @($offs) + @($under) | Sort-Object No)
            $op7T = -1
            if ($op7.Count) { $prev = @($pts | Where-Object { $_.No -lt $op7[0].No }); if ($prev.Count) { $op7T = $prev[-1].T - $dropT } }
            # damage: by itself (the fall), by anybody else; down
            $self = @(Find-Log host '^\[Coop\] (DAMAGE|HIT) player Companion by Companion ' $m -All)
            $others = @(Find-Log host '^\[Coop\] HIT player Companion by (\S+) ' $m -All | Where-Object { $_.Match.Groups[1].Value -ne 'Companion' })
            $down = Find-Log host '^\[Coop\] Companion is down' $m
            $impact = @(Find-Log host '\[Server\]\[Companion\] ImpactVelo=([\d.]+)' $m -All | ForEach-Object { $_.Match.Groups[1].Value })
            $firstOp7 = if ($op7.Count) { "the first ~{0:N1} s after the drop: {1}" -f $op7T, ($op7[0].Text -replace '^\[CoopAgent\] ', '') } else { "no line" }

            Require ($others.Count -eq 0) ("$Tag`: nobody else hit the companion meanwhile" + $(if ($others.Count) { " (" + $others[0].Text + ")" } else { "" }))
            if ($walks) {
                $atWalk = @($tr | Where-Object { $_.No -le $mW["host"] } | Select-Object -Last 1)
                Require ($r -and $r.ok) ("$Tag`: the walk order taken: " + $(if ($r -and $r.ok) { $r.result } elseif ($r) { $r.error } else { 'no answer' }))
                Require ($atWalk.Count -and $atWalk[0].Z -lt $stand - 10) ("$Tag`: the copy was still under the ground when the walk began" + $(if ($atWalk.Count) { " (z {0:N1}, it stood at {1:N1})" -f $atWalk[0].Z, $stand } else { " (no sample)" }))
                Require ($moved -ge 1.0) ("$Tag`: the companion walked in its own game ({0:N1} m)" -f $moved)
            } else {
                Require ($moved -lt 1.0) ("$Tag`: the companion stood still in its own game ({0:N2} m)" -f $moved)
                Require ($op7.Count -ge 1) ("$Tag`: its position report (op 7, every 2 s) saw the sunk copy - it did not come up by itself first (" + $(if ($backT -ge 0) { "back up after {0:N1} s" -f $backT } else { "never back up" }) + ")")
            }
            Check $true ("$Tag`: the copy z {0:N1} -> {1:N1}, lowest {2:N1}; {3}; op 7: sank {4}, was off {5} ({6} of them down to the copy), under the ground {7}, {8}; its own game moved {9:N1} m, lowest z {10:N1}; landings counted: {11}" -f $fromZ, $toZ, $low,
                $(if ($backT -ge 0) { "back up after {0:N1} s, falling at {1:N1} m/s right before (safe: 8)" -f $backT, $speed } else { "never back up" }),
                $sank.Count, $offs.Count, $offsDown.Count, $under.Count, $firstOp7, $moved, $ownLow, $(if ($impact.Count) { ($impact -join ', ') + ' m/s' } else { 'none' }))
            Check ($self.Count -eq 0) ("$Tag`: no fall damage counted for the companion" + $(if ($self.Count) { " ($($self.Count) lines: " + ($self[0].Text -replace '^\[Coop\] ', '') + ")" } else { "" }))
            Check ($offsDown.Count -eq 0) ("$Tag`: its position report never put it down to the sunk copy" + $(if ($offsDown.Count) { " (" + ($offsDown[0].Text -replace '^\[CoopAgent\] ', '') + ")" } else { "" }))
            Check ($a.Copy -and $a.Copy.Hp -gt 0 -and -not $down) ("$Tag`: alive on the host (" + $(if ($a.Copy) { $a.Copy.Text } else { 'no line' }) + $(if ($down) { "; " + $down.Text } else { "" }) + ")")
            Check ($a.Own -and $a.Own.Hp -gt 0) ("$Tag`: alive in its own game (" + $(if ($a.Own) { $a.Own.Text } else { 'no line' }) + ")")
            Check ($a.Own -and [Math]::Abs($a.Own.Z - $a.Own.Ground) -lt 1.0 -and ($walks -or [Math]::Abs($a.Own.Z - $b.Own.Z) -lt 1.0)) ("$Tag`: in its own game it stands on the ground, not under the map (" + $(if ($a.Own) { "z {0:N1}, the ground {1:N1}, before z {2:N1}" -f $a.Own.Z, $a.Own.Ground, $b.Own.Z } else { 'no line' }) + ")")
            Check ($a.Copy -and $a.Own -and [Math]::Abs($a.Copy.Z - $a.Copy.Ground) -lt 2.0 -and (Get-D2 $a.Copy $a.Own) -lt 2.0) ("$Tag`: the host's copy is back on the ground where its own game has it (" + $(if ($a.Copy -and $a.Own) { "z {0:N1}, the ground {1:N1}, {2:N1} m from its own place" -f $a.Copy.Z, $a.Copy.Ground, (Get-D2 $a.Copy $a.Own) } else { 'no line' }) + ")")
            Check ($backT -ge 0 -and $backT -le $P.BackWithin) ("$Tag`: the host's copy back up within $($P.BackWithin) s of the drop" + $(if ($backT -ge 0) { " ({0:N1} s)" -f $backT } else { " (never)" }))
        }

        Step "a flat open spot, nobody alive round it" {
            Send-Game host "coop_god 1"
            # (every landing the server counts, with its speed)
            Send-Game host "pl_debugFallDamage 1"
            Pause-Game 8
            # flat: the terrain 5 m round within $Flat m; open: nothing static or
            # rigid within 6 m at 1.2 m up, nothing over it, no water
            Send-Game host '#function AtSF(x,y) local z0=System.GetTerrainElevation({x=x,y=y,z=0}); local lo,hi=z0,z0; local m=ent_static+ent_rigid+ent_sleeping_rigid; for i=0,7 do local cx,cy=math.cos(i*0.785),math.sin(i*0.785); local z=System.GetTerrainElevation({x=x+5*cx,y=y+5*cy,z=0}); if z<lo then lo=z end if z>hi then hi=z end if Physics.RayWorldIntersection({x=x,y=y,z=z0+1.2},{x=6*cx,y=6*cy,z=0},1,m,g_localActorId,nil,g_HitTable)>0 then return nil end end; if Physics.RayWorldIntersection({x=x,y=y,z=z0+30},{x=0,y=0,z=-30.6},1,m+512,g_localActorId,nil,g_HitTable)>0 then return nil end return z0,hi-lo end'
            # (the flattest within 48 m of a center, the host's start first; one
            # center a command: all of them in one crashed the host in
            # CryScriptSystem, 261023-234609)
            # in the level's play area: a ForbiddenArea (the Rescue start is
            # by its edge) hurts a player outside it 35 a second after 5 s,
            # a test (261023-233536: "type=punish" 22 times, down). The host
            # tries each spot: no area counting down for him there.
            $fs = $null
            $tried = @()
            $Cens = @('h') + @($P.Centers | ForEach-Object { "{" + $_[0] + "," + $_[1] + "}" })
            for ($k = 0; $k -lt $Cens.Count -and -not $fs; $k++) {
                $mF = New-Mark
                $cen = if ($Cens[$k] -eq 'h') { 'local h=System.GetEntity(g_localActorId):GetWorldPos(); local c={h.x,h.y}' } else { 'local c=' + $Cens[$k] }
                Send-Game host ('#' + $cen + '; local best=nil; for dx=-48,48,6 do for dy=-48,48,6 do local x,y=math.floor(c[1]+dx),math.floor(c[2]+dy); local z,r=AtSF(x,y); if z and z>25 and r<' + $P.Flat + ' and (not best or r<best.r) then best={x=x,y=y,z=z,r=r} end end end if best then System.LogAlways(string.format("[CHK] flat spot %d %d %.2f range %.2f near center ' + ($k + 1) + '",best.x,best.y,best.z,best.r)) else System.LogAlways("[CHK] flat spot none near center ' + ($k + 1) + '") end')
                $c = Wait-Log host ('^\[CHK\] flat spot (?:none|(-?\d+) (-?\d+) (-?[\d.]+) range ([\d.]+)) near center (' + ($k + 1) + ')$') 30 $mF
                if (-not $c -or -not $c.Match.Groups[1].Success) { $tried += "center $($k + 1): " + $(if ($c) { 'none' } else { 'no line' }); continue }
                $g = $c.Match.Groups
                $mA = New-Mark
                Send-Game host ('#g_gameRules.game:MovePlayer(g_localActorId,{x=' + $g[1].Value + ',y=' + ([int]$g[2].Value - 8) + ',z=System.GetTerrainElevation({x=' + $g[1].Value + ',y=' + ([int]$g[2].Value - 8) + ',z=0})+0.3},{x=0,y=0,z=0})')
                Pause-Game 2.5
                Send-Game host '#local n=0; for _,a in ipairs(System.GetEntitiesByClass("ForbiddenArea") or {}) do if a.warning and a.warning[g_localActorId] and tonumber(a.Properties.bEnabled)~=0 then n=n+1 end end; System.LogAlways("[CHK] forbidden areas counting down: "..n)'
                $fa = Wait-Log host '^\[CHK\] forbidden areas counting down: (\d+)$' 5 $mA
                $tried += "spot {0} {1} near center {2}: {3}" -f $g[1].Value, $g[2].Value, $g[5].Value, $(if ($fa) { $fa.Text -replace '^\[CHK\] ', '' } else { 'no line' })
                if ($fa -and $fa.Match.Groups[1].Value -eq '0') { $fs = $c }
            }
            Check $true ($tried -join "; ")
            Require ($null -ne $fs) "a flat open spot in the play area"
            $St.S = [pscustomobject]@{ X = [int]$fs.Match.Groups[1].Value; Y = [int]$fs.Match.Groups[2].Value; Z = [double]$fs.Match.Groups[3].Value }
            # (the soldiers there shot: none may hurt the companion while it is mortal)
            $mK = New-Mark
            Send-Game host ('#local p={x=' + $St.S.X + ',y=' + $St.S.Y + ',z=' + $St.S.Z + '}; local k=0; for _,e in ipairs(System.GetEntitiesInSphere(p,' + $P.Clear + ') or {}) do if e.actor and not e.actor:IsPlayer() and e.actor:GetHealth()>0 then g_gameRules:CreateHit(e.id,g_localActorId,g_localActorId,9000,nil,nil,nil,"bullet"); k=k+1 end end; System.LogAlways("[CHK] soldiers around shot: "..k)')
            $k = Wait-Log host '^\[CHK\] soldiers around shot: (\d+)' 10 $mK
            Check $true (($fs.Text -replace '^\[CHK\] ', '') + "; " + $(if ($k) { $k.Text -replace '^\[CHK\] ', '' } else { 'no soldiers line' }))
            # the copy's trace on the host (started and stopped by each drop)
            Send-Game host '#AtS=false; function AtSTick() if not AtS then return end local c=System.GetEntityByName("Companion"); if c and c.actor then local p=c:GetWorldPos(); System.LogAlways(string.format("[CHK] s t=%.2f pos=%.2f,%.2f,%.2f hp=%d fly=%d",_time,p.x,p.y,p.z,c.actor:GetHealth(),c.actor:IsFlying() and 1 or 0)) end Script.SetTimer(100,AtSTick) end'
            Pause-Game 5
        }
        Step "standing still: the server's copy $($P.Drop) m straight down" {
            Invoke-Drop "still" 0 -1
        }
        if ($P.WalkLeg) {
            Step "walking: the copy $($P.Drop) m down, $($P.Aside) m aside, the companion walks off $($P.WalkAfter) s later" {
                Pause-Game 5
                Invoke-Drop "walking" (-$P.Aside) $P.WalkAfter
            }
        }
        Step "the whole test: no fall damage, never down (the moves between the drops too)" {
            if ($St.FirstDrop) {
                $self = @(Find-Log host '^\[Coop\] (DAMAGE|HIT) player Companion by Companion ' $St.FirstDrop -All)
                $down = @(Find-Log host '^\[Coop\] Companion is down' $St.FirstDrop -All)
                Check ($self.Count -eq 0) ("no fall damage for the companion since the first drop" + $(if ($self.Count) { " ($($self.Count) lines: " + ($self[0].Text -replace '^\[Coop\] ', '') + ")" } else { "" }))
                Check ($down.Count -eq 0) "the companion never went down since the first drop ($($down.Count))"
            }
            Send-Game host "coop_god 1"
            Send-Agent "follow" | Out-Null
        }
    }
}
