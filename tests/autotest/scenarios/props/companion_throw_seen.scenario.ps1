# Carrying things (CoopProps), the other way: the AI COMPANION (a client like
# a friend) is told to pick up the farm's barrel ("grab"), follows the host
# with it and throws it ("throw"); the host's game must see it in its hands,
# see it fly the same way and see it come to rest at the same spot.
@{
    Name = "props-companion-throw-seen"
    Tags = @("props", "sync", "companion")
    SummaryRu = "Компаньон по команде берёт бочку, несёт за хостом и бросает: игра хоста видит её в его руках, полёт и место падения там же"
    Session = @{ Fixture = "new:rescue"; Companion = $true }
    Params = @{ Prop = "Storage.rusty_metal_barrel_b35" }
    Body = {
        param($P)
        $n = $P.Prop
        $rx = '^\[CHK\] pw t=([\d.]+) pos=(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)'
        function Get-PW($role, $m) {
            @(Find-Log $role $rx $m -All | ForEach-Object { $g = $_.Match.Groups; [pscustomobject]@{ No = $_.No; X = [double]$g[2].Value; Y = [double]$g[3].Value; Z = [double]$g[4].Value } })
        }
        function Dist($a, $b) { [Math]::Sqrt(($a.X - $b.X) * ($a.X - $b.X) + ($a.Y - $b.Y) * ($a.Y - $b.Y) + ($a.Z - $b.Z) * ($a.Z - $b.Z)) }
        $pw = '#PW={on=true,n="' + $n + '",t=0}; function PWTick() if not PW.on then return end PW.t=PW.t+0.1; local e=System.GetEntityByName(PW.n); if e then local p=e:GetWorldPos(); System.LogAlways(string.format("[CHK] pw t=%.1f pos=%.2f,%.2f,%.2f",PW.t,p.x,p.y,p.z)) end Script.SetTimer(100,PWTick) end; PWTick()'
        $where = '#local e=System.GetEntityByName("' + $n + '"); if e then local p=e:GetWorldPos(); System.LogAlways(string.format("[CHK] thing 1 at %.2f %.2f %.2f", p.x, p.y, p.z)) else System.LogAlways("[CHK] thing none") end'
        Step "the companion goes to pick it up" {
            Send-Game host "coop_god 1"
            Pause-Game 15
            $m = New-Mark
            Send-Game host $where
            $h = Wait-Log host '^\[CHK\] thing (\d+) at (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)' 10 $m
            Require ($null -ne $h) "the thing is in the host's game"
            $hp = [pscustomobject]@{ X = [double]$h.Match.Groups[2].Value; Y = [double]$h.Match.Groups[3].Value; Z = [double]$h.Match.Groups[4].Value }
            $script:thing = $hp
            # the host 6 m north of it (the way it came there is open); the companion 4 m off
            Send-Game host ("#local p={x=$($hp.X),y=$($hp.Y + 6),z=0}; p.z=System.GetTerrainElevation(p)+0.1; g_gameRules.game:MovePlayer(g_localActorId,p,{x=0,y=0,z=3.1416}); local c=System.GetEntityByName(""Companion""); g_gameRules.game:MovePlayer(c.id,{x=$($hp.X - 1),y=$($hp.Y + 4),z=$($hp.Z + 0.5)},{x=0,y=0,z=0})")
            Pause-Game 2
            Send-Game host $pw
            Send-Game companion $pw
            $m = New-Mark
            $r = Send-Agent ("grab " + $n)
            Require ($r.ok) ("the order taken: " + $(if ($r.ok) { $r.result } else { $r.error }))
            $up = Wait-Log host ('^\[CoopProp\] Companion picks up ' + [regex]::Escape($n)) 30 $m
            $reach = @(Find-Log companion '^\[CoopAgent\] reaches for' $m -All).Count
            Require ($null -ne $up) "the host's game: the companion picked it up ($(if ($up) { $up.Text } else { 'no line' }); it reached $reach times)"
            $own = Wait-Log companion '^\[CoopAgent\] holds ' 10 $m
            Check ($null -ne $own) "the companion knows it holds it: $(if ($own) { $own.Text } else { 'no line' })"
            $script:upMark = $m
        }
        Step "it carries it after the host" {
            Send-Agent "free" | Out-Null
            # (the host walks 8 m on: it follows him with the barrel)
            Send-Game host '#g_localActor:SetAngles({x=0,y=0,z=0})'
            Send-Game host "coop_test_key moveforward 1"
            Pause-Game 2.5
            Send-Game host "coop_test_key moveforward 0"
            Pause-Game 5
            $m = $script:upMark
            $mh = Get-PW host $m; $mc = Get-PW companion $m
            Require ($mh.Count -ge 5 -and $mc.Count -ge 5) "the watch ran in both games ($($mh.Count), $($mc.Count))"
            $lh = $mh[-1]; $lc = $mc[-1]
            Check ((Dist $lc $script:thing) -gt 2.0) ("it carried it in its own game ({0:N1} m from where it lay)" -f (Dist $lc $script:thing))
            Check ((Dist $lh $script:thing) -gt 2.0) ("... and the host's game saw it carried ({0:N1} m)" -f (Dist $lh $script:thing))
            Check ((Dist $lh $lc) -lt 0.6) ("held in the same place in both games ({0:N2} m apart)" -f (Dist $lh $lc))
            $still = Find-Log companion '^\[CoopAgent\] no longer holds' $m
            Check ($null -eq $still) "it kept it on the way$(if ($still) { ': ' + $still.Text })"
        }
        Step "it throws it: the same flight, the same rest" {
            $m = New-Mark
            # (toward a point 15 m ahead of it, a little up)
            Send-Game companion '#local c=g_localActor; local p=c:GetWorldPos(); local d=c:GetDirectionVector(1); System.LogAlways(string.format("[CHK] throw to %.1f %.1f %.1f", p.x+d.x*15, p.y+d.y*15, p.z+3))'
            $to = Wait-Log companion '^\[CHK\] throw to (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)' 10 $m
            $r = Send-Agent ("throw " + $(if ($to) { "$($to.Match.Groups[1].Value) $($to.Match.Groups[2].Value) $($to.Match.Groups[3].Value)" } else { "" }))
            Require ($r.ok) ("the order taken: " + $(if ($r.ok) { $r.result } else { $r.error }))
            $thrown = Wait-Log host ('^\[CoopProp\] Companion throws ' + [regex]::Escape($n)) 10 $m
            Require ($null -ne $thrown) "the host's game: it threw it: $(if ($thrown) { $thrown.Text } else { 'no line' })"
            $restH = Wait-Log host ('^\[CoopProp\] ' + [regex]::Escape($n) + ' \(\d+\) rests at') 20 $m
            $restC = Wait-Log companion ('^\[CoopProp\] ' + [regex]::Escape($n) + ' \(\d+\) rests at') 20 $m
            Pause-Game 2
            Send-Game host '#PW.on=false'
            Send-Game companion '#PW.on=false'
            Check ($null -ne $restH) "it came to rest in the host's game: $(if ($restH) { $restH.Text })"
            Check ($null -ne $restC) "the companion's game was told where: $(if ($restC) { $restC.Text })"
            $fh = Get-PW host $m; $fc = Get-PW companion $m
            if ($fh.Count -ge 3 -and $fc.Count -ge 3) {
                $flown = Dist $fc[0] $fc[-1]
                $dev = 0.0
                foreach ($q in $fh) { $best = 1e9; foreach ($p in $fc) { $d = Dist $q $p; if ($d -lt $best) { $best = $d } }; if ($best -gt $dev) { $dev = $best } }
                $gap = Dist $fh[-1] $fc[-1]
                Check ($flown -ge 2.0) ("it flew {0:N1} m in its own game" -f $flown)
                Check ($dev -lt 1.5) ("the host's game saw the same path (at most {0:N2} m off it)" -f $dev)
                Check ($gap -lt 0.3) ("at rest at the same spot in both games ({0:N2} m apart)" -f $gap)
            } else { Check $false "the watch has the flight ($($fh.Count), $($fc.Count))" }
            $grenade = Find-Log host '^\[CoopAnim\] Companion starts a throw from the network' $m
            Check ($null -eq $grenade) "no grenade throw shown for it in the host's game$(if ($grenade) { ': ' + $grenade.Text })"
            $letgo = Find-Log companion 'lets go of the .* instead of throwing it' $m
            Check ($null -eq $letgo) "a real throw, not the old let-go$(if ($letgo) { ': ' + $letgo.Text })"
        }
    }
}
