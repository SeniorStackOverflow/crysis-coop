# The user's games of 2026-10-16/17: on foot the companion fought in the open
# till it was hit, hid from the one it aimed at while others shot it, peeked
# blind on a timer, never cloaked, and went down in about 10 s at the bridge.
# Arena: rescue's farm. Farm house 13 (AI waypoint building 13: inner nodes x
# 1170.9..1176.6, y 2091.6..2102.4, floor 244.4) stands between the host (west of
# it) and four soldiers 31 m east of the companion in the open field (x 1214-1216,
# terrain 242-243). The designer's hide spots on the house's west side:
# AIAnchor149 (1169.2,2091.4), AIAnchor4303 (1166.9,2092.9), AIAnchor4302
# (1166.6,2099.5), AIAnchor148 (1168.7,2102.2); south side AIAnchor150/142/143
# (y 2090); north AIAnchor146/147 (y 2104). The companion starts east of the
# house, seen by all four. A 1.2 m wall runs along x 1166 (Jump1m20 smart objects
# SmartObject290-292 at (1165.7-1166.1, 2091.5-2101.0)); the host stands between it and
# the house at (1168.3,2096.5). It has to get behind the house (or another solid
# thing), fight from there (out, shoot, back), keep armor on, cloak when hurt.
@{
    Name = "companion-cover-farm-house"
    Tags = @("companion", "combat", "tactics")
    SummaryRu = "Бой у фермы: компаньон уходит за дом, стреляет из-за укрытия (выглядывает), держит броню, при ранении маскируется"
    Session = @{ Fixture = "new:rescue"; Companion = $true }
    Params = @{ Watch = 150; Foes = @("Farm_3", "Farm_6", "Farm_8", "Farm_10"); MaxExposed = 0.45; MinPeeks = 3; MinKills = 2; CoverWithin = 6 }
    Body = {
        param($P)
        $foeList = ($P.Foes | ForEach-Object { '"' + $_ + '"' }) -join ","
        Step "the arena" {
            Send-Game host "coop_god 1"
            Pause-Game 5
            Send-Game host ('#local keep={}; for _,n in ipairs({' + $foeList + '}) do local e=System.GetEntityByName(n); if e then keep[e.id]=true end end; local k=0; for _,e in ipairs(System.GetEntitiesInSphere({x=1185,y=2097,z=244},400) or {}) do if e.actor and not e.actor:IsPlayer() and e.actor:GetHealth()>0 and not keep[e.id] then g_gameRules:CreateHit(e.id,g_localActorId,g_localActorId,9000,nil,nil,nil,"bullet"); k=k+1 end end; System.LogAlways("[CHK] others shot: "..k)')
            Send-Game host ('#local at={{1214,2088},{1216,2094},{1214,2100},{1216,2106}}; local n=0; for i,nm in ipairs({' + $foeList + '}) do local e=System.GetEntityByName(nm); if e and e.actor:GetHealth()>0 then local t={x=at[i][1],y=at[i][2],z=0}; t.z=System.GetTerrainElevation(t)+0.3; e:SetWorldPos(t); n=n+1 end end; System.LogAlways("[CHK] foes placed: "..n)')
            Send-Game host '#local c=System.GetEntityByName("Companion"); local function g(x,y) return {x=x,y=y,z=System.GetTerrainElevation({x=x,y=y,z=0})+0.5} end; g_gameRules.game:MovePlayer(g_localActorId,g(1168.3,2096.5),{x=0,y=0,z=0}); g_gameRules.game:MovePlayer(c.id,g(1183.5,2096.5),{x=0,y=0,z=0}); System.LogAlways("[CHK] players placed")'
            Send-Agent "free" | Out-Null
            Require ($null -ne (Find-Log host '^\[CHK\] foes placed: [34]' -Last)) "three or four soldiers in the field"
            Start-Watch $P.Foes
            # host only immortal: the companion takes damage (COMPANION_DAMAGE 0.5)
            Send-Game host "coop_god 2"
        }
        Step "the fight" {
            $m = New-Mark
            $deadline = (Get-Date).AddSeconds($P.Watch)
            while ((Get-Date) -lt $deadline) {
                Pause-Game 5
                $kills = Count-Log host ('\] HIT (' + ($P.Foes -join '|') + ') by \S+ .*KILLED') $m
                if ($kills -ge $P.Foes.Count) { break }
            }
            Send-Game host "coop_god 1"
            $w = Get-Watch $m
            $dmg = @(Find-Log host '^\[Coop\] DAMAGE player Companion .* taken=([1-9][\d.]*)' $m -All)
            $kills = Count-Log host ('\] HIT (' + ($P.Foes -join '|') + ') by Companion .*KILLED') $m
            $downs = Count-Log host '^\[Coop\] Companion is down' $m
            $asks = @(Find-Log host '^\[CoopAgent\] cover for Companion from \S+: (.*)' $m -All)
            $none = @($asks | Where-Object { $_.Text -match 'none found' }).Count
            # exposure while it fights (foes alive: some seen line > 0 at all, after its first hit)
            $first = if ($dmg.Count) { $dmg[0].No } else { 0 }
            # (as said: after its first hit, until the last foe's death)
            $lastKill = @(Find-Log host ('\] HIT (' + ($P.Foes -join '|') + ') by \S+ .*KILLED') $m -All)
            $end = if ($lastKill.Count -ge $P.Foes.Count) { $lastKill[-1].No } else { [int]::MaxValue }
            $alive = @($w | Where-Object { $_.Hp -gt 0 -and $_.No -gt $first -and $_.No -le $end })
            $exposed = if ($alive.Count) { @($alive | Where-Object { $_.Seen -gt 0 }).Count / $alive.Count } else { 1 }
            $pk = Get-Peeks $w $m
            $crouch = if ($alive.Count) { @($alive | Where-Object { $_.Head -lt 1.25 }).Count / $alive.Count } else { 0 }
            $suit = @{}; foreach ($x in $alive) { $suit[$x.Suit] = 1 + $(if ($suit.ContainsKey($x.Suit)) { $suit[$x.Suit] } else { 0 }) }
            Check $true ("hits taken {0}, downs {1}, kills {2}, cover asks {3} ({4} none found), crouched {5:P0}, suit ticks {6}" -f $dmg.Count, $downs, $kills, $asks.Count, $none, $crouch, (($suit.Keys | Sort-Object | ForEach-Object { "$_=$($suit[$_])" }) -join ' '))
            # hidden within CoverWithin s of the first hit
            $delay = $null
            if ($dmg.Count) {
                $after = @($w | Where-Object { $_.No -gt $first })
                $h = @($after | Where-Object { $_.Seen -eq 0 -and $_.Hp -gt 0 } | Select-Object -First 1)
                if ($after.Count -and $h.Count) { $delay = $h[0].T - $after[0].T }
            }
            Check ($dmg.Count -eq 0 -or ($null -ne $delay -and $delay -le $P.CoverWithin)) "hidden from every soldier $delay s after its first hit (at most $($P.CoverWithin))"
            Check ($exposed -le $P.MaxExposed) ("seen by a soldier {0:P0} of the fight (at most {1:P0})" -f $exposed, $P.MaxExposed)
            Check ($pk.Peeks -ge $P.MinPeeks) "peeked out and back $($pk.Peeks) times (at least $($P.MinPeeks)); its hits $($pk.HitsByCompanion)"
            Check ($kills -ge $P.MinKills) "killed $kills of $($P.Foes.Count)"
            Check ($downs -eq 0) "never down"
            $shotBy = Count-Log companion '^\[CoopAgent\] shot by ' $m
            $hostHits = @(Find-Log host '^\[Coop\] DAMAGE player Companion ' $m -All).Count
            Check $true "it knew of $shotBy of the $hostHits hits the host counted on it"
            $fightLines = @(Find-Log companion '^\[CoopAgent\] the last minute in fights: (.*)' $m -All | ForEach-Object { $_.Text -replace '^.*fights: ', '' })
            Check $true ("its fights: " + $(if ($fightLines.Count) { $fightLines -join ' | ' } else { 'no line' }))
            $suitLines = @(Find-Log companion '^\[CoopAgent\] suit: (.*)' $m -All | ForEach-Object { $_.Text -replace '^.*suit: ', '' })
            Check $true ("suit: " + $(if ($suitLines.Count) { ($suitLines | Select-Object -First 6) -join ' | ' } else { 'no switch' }))
            Check (-not $suit.ContainsKey(0) -or $suit[0] -lt 0.2 * $alive.Count) "no speed mode standing in a fight"
        }
        Stop-Watch
    }
}
