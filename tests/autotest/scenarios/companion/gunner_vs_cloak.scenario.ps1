# The autotester's soak and camp fights of 2026-10-21/22: cloaked, the
# companion was lost by the soldiers on foot ("lost sight of Companion:
# cloaked") but a soldier at a vehicle's gun - the bridge helicopter's .50,
# the camp LTV's ShiTen - kept hitting it. Here the bridge reinforcement
# truck's gunner: once he shoots, the host goes 120 m away (out of his sight),
# the companion (immortal) stands still in the open, holding its fire: 12 s
# in armor, then 12 s cloaked. His hits on it and whether it is his target are
# counted in each; cloaked he must lose it as the soldiers on foot do.
@{
    Name = "companion-gunner-vs-cloak"
    Tags = @("companion", "combat", "suit", "vehicle")
    SummaryRu = "Невидимость против стрелка на технике: пулемётчик грузовика у моста теряет компаньона под невидимостью, как пехота, и перестаёт в него попадать"
    Session = @{ Fixture = "new:rescue"; Companion = $true }
    Params = @{ Truck = "Bridge_reinforcement_Truck_1"; Gunner = "Bridge_Reinforcement_2"; Phase = 12 }
    Body = {
        param($P)
        $g = $P.Gunner
        function Count-Phase($m) {
            # (his target: "Companion" when he sees it, "Perception of Companion ..." where he last knew it)
            $att = @(Find-Log host '^\[CHK\] gunner att=(.+) suit=(-?\d+)$' $m -All)
            $onIt = @($att | Where-Object { $_.Match.Groups[1].Value -eq 'Companion' }).Count
            $hits = Count-Log host ('^\[Coop\] HIT player Companion by ' + [regex]::Escape($g) + ' ') $m
            $others = Count-Log host '^\[Coop\] HIT player Companion by ' $m
            return @{ Hits = $hits; Others = $others - $hits; OnIt = $onIt; Samples = $att.Count }
        }
        Step "the truck's gunner shoots at the players by the bridge" {
            Send-Game host "coop_god 1"
            Pause-Game 30
            $m = New-Mark
            Send-Game host '#local p={x=1737,y=1960,z=0}; p.z=System.GetTerrainElevation(p)+1; g_gameRules.game:MovePlayer(g_localActorId,p,{x=0,y=0,z=0}); local c=System.GetEntityByName("Companion"); if c then g_gameRules.game:MovePlayer(c.id,{x=p.x-4,y=p.y-3,z=p.z},{x=0,y=0,z=0}) end; System.LogAlways("[CHK] the players by the bridge")'
            Send-Agent "hold" | Out-Null
            $in = Wait-Log host ("\] " + $g + " entered vehicle " + $P.Truck + " seat gunner") 120 $m
            Require ($null -ne $in) "the truck's gunner $g got to its gun"
            $fire = Wait-Log host ("HIT (player \S+|vehicle \S+) by " + $g + " weapon=") 120 $m
            Require ($null -ne $fire) "the gunner shoots"
            # (the host out of his sight: only the companion to shoot at)
            Send-Game host '#local h={x=1737,y=1840,z=0}; h.z=System.GetTerrainElevation(h)+1; g_gameRules.game:MovePlayer(g_localActorId,h,{x=0,y=0,z=0}); System.LogAlways("[CHK] the host away")'
            Send-Agent "fire off" | Out-Null
            Send-Game host ('#GunOn=true; function GunTick() if not GunOn then return end local c=System.GetEntityByName("Companion"); local e=System.GetEntityByName("' + $g + '"); local t="-"; if e then local ok,a=pcall(AI.GetAttentionTargetOf,e.id); if ok and a then t=tostring(a) end end; System.LogAlways(string.format("[CHK] gunner att=%s suit=%d", t, c.actor:GetNanoSuitMode())); Script.SetTimer(1000,GunTick) end; GunTick()')
            Pause-Game 8
        }
        Step "in armor, then cloaked" {
            Send-Agent "suit armor" | Out-Null
            $m = New-Mark
            Pause-Game $P.Phase
            $a = Count-Phase $m
            Check $true ("in armor: the gunner hit it {0} times (others {1}), it was his target {2} of {3} s" -f $a.Hits, $a.Others, $a.OnIt, $a.Samples)
            Send-Game host '#local c=System.GetEntityByName("Companion"); pcall(function() c.actor:SetNanoSuitEnergy(200) end)'
            $cloakMark = New-Mark
            $r = Send-Agent "suit cloak"
            Check ($r.ok) ("its suit took cloak: " + $(if ($r.ok) { $r.result } else { $r.error }))
            Pause-Game 2
            $m = New-Mark
            Pause-Game $P.Phase
            $c = Count-Phase $m
            Send-Game host '#GunOn=false'
            Check $true ("cloaked: the gunner hit it {0} times (others {1}), it was his target {2} of {3} s" -f $c.Hits, $c.Others, $c.OnIt, $c.Samples)
            $lost = Find-Log host ('^\[CoopAI\] ' + [regex]::Escape($g) + ' lost sight of Companion: cloaked') $m
            Check $true ("the gunner lost sight of it: " + $(if ($lost) { $lost.Text } else { 'no line' }))
            Check ($a.Hits -ge 2) "in armor the gunner did hit it ($($a.Hits)): a fair test"
            Check ($c.Hits -le 1 -and $c.OnIt -le 3 -and $c.Samples -ge 8) "cloaked, the gunner lost it: $($c.Hits) hits, it was what he saw $($c.OnIt) of $($c.Samples) s"
            $left = Find-Log host ('^\[CoopVeh\] ' + [regex]::Escape($g) + ' left vehicle') $cloakMark
            Check $true ("the gunner " + $(if ($left) { "left his gun: " + $left.Text } else { "stayed at his gun" }))
        }
    }
}
