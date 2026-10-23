# The autotester's soak of 2026-10-22 (261022-001238): the bridge
# helicopter's door gunner (InVehicleControlledGunner, 5 s FIREMODE_FORCED
# bursts) hit the cloaked companion 4 times and put ~25 more rounds into the
# truck by it - he fires at the memory of a target, and the in-vehicle
# behaviours ignore the cloak. coop_cloak_mute 1 has soldiers at a vehicle's gun
# hold their fire then, as the foot soldiers' own cloak reaction does.
# The helicopter comes when one of the bridge squad gets to the radio
# (AIReinforcementSpot1 "Called" in the Bridge flow graph), lands its troops by
# the hut and its gunner fires for a few seconds only, then it flies off: one
# measurement a game. The companion (immortal, holding its fire, in armor)
# stands in the open by the hut, the host far away; at the gunner's first hit
# on it, it cloaks at once and his hits are timed for 8 s by a 0.25 s ticker
# on the host, from the server's cloak. Run with -Repeat: the odd runs are
# coop_cloak_mute -1 (log only: as before), the even ones 1 (held).
@{
    Name = "companion-heli-gunner-cloak"
    Tags = @("companion", "combat", "suit", "vehicle")
    SummaryRu = "Невидимость против вертолётного стрелка: с coop_cloak_mute 1 стрелок на технике перестаёт стрелять по невидимому компаньону (запуски по очереди: как раньше / с правилом)"
    Session = @{ Fixture = "new:rescue"; Companion = $true }
    Params = @{ Gunner = "Bridge_gunner"; Measure = 8 }
    Body = {
        param($P)
        $g = $P.Gunner
        $global:CoopHeliCloakRun = 1 + [int]$global:CoopHeliCloakRun
        $mode = if ($global:CoopHeliCloakRun % 2) { -1 } else { 1 }
        $hitRx = '^\[Coop\] HIT player Companion by ' + [regex]::Escape($g) + ' '
        Step "the radio calls the helicopter (coop_cloak_mute $mode)" {
            Send-Game host "coop_god 1"
            Send-Game host "coop_cloak_mute $mode"
            Pause-Game 30
            # (the bridge squad is woken at the bridge: at the hut alone no
            # soldier was enabled, the first run)
            $m = New-Mark
            Send-Game host '#local p={x=1737,y=1960,z=0}; p.z=System.GetTerrainElevation(p)+1; g_gameRules.game:MovePlayer(g_localActorId,p,{x=0,y=0,z=0}); local c=System.GetEntityByName("Companion"); if c then g_gameRules.game:MovePlayer(c.id,{x=p.x-4,y=p.y-3,z=p.z},{x=0,y=0,z=0}) end; System.LogAlways("[CHK] the players by the bridge")'
            $in = Wait-Log host "\] Bridge_\d entered vehicle Bridge_Truck_1 " 120 $m
            Require ($null -ne $in) "the bridge squad woke: $(if ($in) { $in.Text })"
            Pause-Game 25
            Send-Game host '#local p={x=1695,y=1825,z=0}; p.z=System.GetTerrainElevation(p)+1; g_gameRules.game:MovePlayer(g_localActorId,p,{x=0,y=0,z=0}); local c=System.GetEntityByName("Companion"); if c then local q={x=1699,y=1852,z=0}; q.z=System.GetTerrainElevation(q)+1; g_gameRules.game:MovePlayer(c.id,q,{x=0,y=0,z=0}) end; System.LogAlways("[CHK] the players by the hut")'
            Pause-Game 10
            # (the companion shot the squad before one got to the radio, the
            # second run: the spot called as the soldier does)
            if (-not (Find-Log host ('\] ' + [regex]::Escape($g) + ' alert=') $m)) {
                Send-Game host '#local s=System.GetEntityByName("AIReinforcementSpot1"); if s then s:Alarm(); System.LogAlways("[CHK] the radio called") else System.LogAlways("[CHK] no radio") end'
            }
            # (the host far away: only the companion for him to shoot at)
            Send-Game host '#local h={x=942,y=2110,z=0}; h.z=System.GetTerrainElevation(h)+1; g_gameRules.game:MovePlayer(g_localActorId,h,{x=0,y=0,z=0}); System.LogAlways("[CHK] the host away")'
            Send-Agent "hold" | Out-Null
            Send-Agent "fire off" | Out-Null
        }
        Step "at his first hit it cloaks: his hits timed for $($P.Measure) s" {
            # (armor held and the energy full till he hits it: the suit's hold
            # is 20 s, renewed)
            $m = New-Mark
            $hit = $null
            for ($t = 0; $t -lt 18 -and -not $hit; $t++) {
                Send-Game host '#local c=System.GetEntityByName("Companion"); pcall(function() c.actor:SetNanoSuitEnergy(200) end)'
                Send-Agent "suit armor" | Out-Null
                $hit = Wait-Log host $hitRx 10 $m
            }
            Require ($null -ne $hit) "the helicopter's door gunner $g hits the companion: $(if ($hit) { $hit.Text })"
            $m0 = New-Mark
            $r = Send-Agent "suit cloak"
            Send-Game host ('#GunT0=_time; GunOn=true; function GunTick() if not GunOn then return end local c=System.GetEntityByName("Companion"); local e=System.GetEntityByName("' + $g + '"); local t="-"; if e then local ok,a=pcall(AI.GetAttentionTargetOf,e.id); if ok and a then t=tostring(a) end end; System.LogAlways(string.format("[CHK] gt=%.2f att=%s suit=%d", _time-GunT0, t, c.actor:GetNanoSuitMode())); Script.SetTimer(250,GunTick) end; GunTick()')
            Pause-Game ($P.Measure + 1.5)
            Send-Game host '#GunOn=false'
            # the hits timed by the ticks: a hit takes the time of the tick after it
            $tail = Get-Tail host
            $cloakAt = $null; $pending = @(); $times = @(); $lastT = 0.0; $att = @(); $before = 0
            for ($n = $m0["host"]; $n -lt $tail.Lines.Count; $n++) {
                $l = $tail.Lines[$n]
                if ($null -eq $cloakAt -and $l -match "^\[CoopAI\] Companion's cloak for the soldiers' own eyes: [\d.]+ \(now [\d.]+; the suit cloaked\)") { $pending += 'C' }
                elseif ($l -match $hitRx) { $pending += 'H' }
                elseif ($l -match '^\[CHK\] gt=(-?[\d.]+) att=(.+) suit=(-?\d+)$') {
                    $lastT = [double]$Matches[1]
                    $att += [pscustomobject]@{ T = $lastT; Att = $Matches[2]; Suit = [int]$Matches[3] }
                    foreach ($k in $pending) { if ($k -eq 'C') { $cloakAt = $lastT } elseif ($null -ne $cloakAt) { $times += $lastT - $cloakAt } else { $before++ } }
                    $pending = @()
                }
            }
            $held = Find-Log host ('^\[CoopAI\] ' + [regex]::Escape($g) + ' \([^)]+\) (holds|would hold) fire at Companion: cloaked') $m0
            $again = Find-Log host ('^\[CoopAI\] ' + [regex]::Escape($g) + ' may fire at Companion again') $m0
            $early = @($times | Where-Object { $_ -le 1.0 }).Count
            $late = @($times | Where-Object { $_ -gt 1.0 -and $_ -le $P.Measure }).Count
            $cl = @($att | Where-Object { $_.Suit -eq 2 })
            $onIt = @($cl | Where-Object { $_.Att -match 'Companion' }).Count
            $other = @(Find-Log host '^\[Coop\] HIT player Companion by ' $m0 -All).Count - $times.Count - $before
            Check ($r.ok -and $null -ne $cloakAt) ("cloak " + $(if ($r.ok) { "taken" } else { "refused: " + $r.error }) + $(if ($null -ne $cloakAt) { ", the server's at $cloakAt s" } else { ", no server cloak line" }))
            Check $true ("RESULT coop_cloak_mute {0}: cloaked, the gunner hit it {1} times in 0-1 s and {2} in 1-{3} s ({4} before the server's cloak, others {5}); his target was it (or its memory) {6} of {7} cloaked ticks; {8}{9}" -f $mode,
                $early, $late, $P.Measure, $before, $other, $onIt, $cl.Count, $(if ($held) { $held.Text } else { "no hold line" }), $(if ($again) { "; " + $again.Text } else { "" }))
            if ($mode -gt 0) {
                Check ($null -ne $held -and $held.Text -match ' holds fire') "held: the gunner holds his fire"
                Check ($late -eq 0) "held: no hit on the cloaked companion after the first second ($late)"
                Check (($early + $late) -le 1) "held: at most one hit in all ($($early + $late))"
            }
        }
    }
}
