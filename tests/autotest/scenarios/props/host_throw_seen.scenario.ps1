# Carrying things (CoopProps, the user's wish of 2026-10-22: "everybody may
# carry ordinary things, the same for all"): the HOST picks up a thing by the
# farm east of the start, carries it a few metres and throws it; the
# companion's game must see it in his hands, see it fly the same way and see
# it come to rest at the same spot (within 0.3 m). Before CoopProps nobody could
# pick such things up in a coop game, and nothing a host moved reached a
# client at all.
@{
    Name = "props-host-throw-seen"
    Tags = @("props", "sync")
    SummaryRu = "Хост поднимает бочку, несёт и бросает: игра компаньона видит её в его руках, полёт и место падения там же"
    Session = @{ Fixture = "new:rescue"; Companion = $true }
    # (the farm's barrel east of the start; carried and thrown back the way
    # he came, up: the farm's walls are on the other sides)
    Params = @{ Prop = "Storage.rusty_metal_barrel_b35"; Mid = 0.45; CarryYaw = $null; ThrowPitch = 0.45 }
    Body = {
        param($P)
        $n = $P.Prop
        $rx = '^\[CHK\] pw t=([\d.]+) pos=(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)'
        function Get-PW($role, $m) {
            @(Find-Log $role $rx $m -All | ForEach-Object { $g = $_.Match.Groups; [pscustomobject]@{ No = $_.No; X = [double]$g[2].Value; Y = [double]$g[3].Value; Z = [double]$g[4].Value } })
        }
        function Dist($a, $b) { [Math]::Sqrt(($a.X - $b.X) * ($a.X - $b.X) + ($a.Y - $b.Y) * ($a.Y - $b.Y) + ($a.Z - $b.Z) * ($a.Z - $b.Z)) }
        $pw = '#PW={on=true,n="' + $n + '",t=0}; function PWTick() if not PW.on then return end PW.t=PW.t+0.1; local e=System.GetEntityByName(PW.n); if e then local p=e:GetWorldPos(); System.LogAlways(string.format("[CHK] pw t=%.1f pos=%.2f,%.2f,%.2f",PW.t,p.x,p.y,p.z)) end Script.SetTimer(100,PWTick) end; PWTick()'
        Step "the thing in both games, the host before it" {
            Send-Game host "coop_god 1"
            Pause-Game 15
            $where = '#local e=System.GetEntityByName("' + $n + '"); if e then local p=e:GetWorldPos(); System.LogAlways(string.format("[CHK] thing 1 at %.2f %.2f %.2f", p.x, p.y, p.z)) else System.LogAlways("[CHK] thing none") end'
            $m = New-Mark
            Send-Game host $where
            Send-Game companion $where
            $h = Wait-Log host '^\[CHK\] thing (\d+) at (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)' 10 $m
            $c = Wait-Log companion '^\[CHK\] thing (\d+) at (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)' 10 $m
            Require ($h -and $c) "the thing is in both games: host $(if ($h) { $h.Text }), companion $(if ($c) { $c.Text })"
            $hp = [pscustomobject]@{ X = [double]$h.Match.Groups[2].Value; Y = [double]$h.Match.Groups[3].Value; Z = [double]$h.Match.Groups[4].Value }
            $cp = [pscustomobject]@{ X = [double]$c.Match.Groups[2].Value; Y = [double]$c.Match.Groups[3].Value; Z = [double]$c.Match.Groups[4].Value }
            Check ((Dist $hp $cp) -lt 0.5) ("at the same place before ({0:N2} m apart)" -f (Dist $hp $cp))
            # (its physics in both games: rigid (2) to be carried)
            $m2 = New-Mark
            Send-Game host ("coop_test_findobj $($hp.X) $($hp.Y) $($hp.Z + 0.4) 2 barrel")
            Send-Game companion ("coop_test_findobj $($hp.X) $($hp.Y) $($hp.Z + 0.4) 2 barrel")
            Pause-Game 1
            $fh = @(Find-Log host ([regex]::Escape($n)) $m2 -All | Select-Object -Last 1)
            $fc = @(Find-Log companion ([regex]::Escape($n)) $m2 -All | Select-Object -Last 1)
            Check $true ("its physics: host " + $(if ($fh.Count) { $fh[0].Text } else { "none" }) + " | companion " + $(if ($fc.Count) { $fc[0].Text } else { "none" }))
            $script:thing = $hp
            # the companion 7 m off, holding there; the host 1.3 m south of it, looking at its middle
            Send-Game host ("#local c=System.GetEntityByName(""Companion""); g_gameRules.game:MovePlayer(c.id,{x=$($hp.X + 6),y=$($hp.Y - 4),z=$($hp.Z + 0.5)},{x=0,y=0,z=0})")
            Pause-Game 2
            Send-Agent "hold" | Out-Null
            # (from the side the hand reaches it: a fence or a wall may stand on
            # another; each side tried with the use key's answer, then let go)
            $script:side = $null
            foreach ($sd in @(@(0, -1), @(0, 1), @(1, 0), @(-1, 0))) {
                $px = $hp.X - $sd[0] * 1.3; $py = $hp.Y - $sd[1] * 1.3
                $yaw = [Math]::Atan2(-$sd[0], $sd[1])
                $pitch = [Math]::Atan2($P.Mid - 1.6, 1.3)
                Send-Game host ("#local p={x=$px,y=$py,z=0}; p.z=System.GetTerrainElevation(p)+0.1; g_gameRules.game:MovePlayer(g_localActorId,p,{x=$pitch,y=0,z=$yaw})")
                Pause-Game 1
                Send-Game host ("#g_localActor:SetAngles({x=$pitch,y=0,z=$yaw})")
                Pause-Game 1
                $m = New-Mark
                Send-Game host "coop_test_key use 1"
                Pause-Game 0.2
                Send-Game host "coop_test_key use 0"
                $ans = Wait-Log host '^\[CoopProp\] Nomad.s left hand: (.*)' 5 $m
                Check $true ("from side $($sd -join ','): " + $(if ($ans) { $ans.Match.Groups[1].Value } else { "no answer" }))
                if ($ans -and $ans.Match.Groups[1].Value -like 'a thing*') {
                    $script:side = $sd
                    $script:upMark = $m
                    break
                }
            }
            Require ($null -ne $script:side) "a side the host's hand reaches it from"
            Send-Game host $pw
            Send-Game companion $pw
        }
        Step "he picks it up and carries it 3 m" {
            $m = $script:upMark
            $up = Wait-Log host ('^\[CoopProp\] Nomad picks up ' + [regex]::Escape($n)) 8 $m
            Require ($null -ne $up) "the host picked it up: $(if ($up) { $up.Text } else { 'no line' })"
            $seen = Wait-Log companion ('^\[CoopProp\] Nomad holds ' + [regex]::Escape($n)) 8 $m
            Check ($null -ne $seen) "the companion's game knows he holds it: $(if ($seen) { $seen.Text } else { 'no line' })"
            Pause-Game 1
            # (turned round, the way he came, to carry it)
            $sd = $script:side
            $script:carryYaw = if ($null -ne $P.CarryYaw) { $P.CarryYaw } else { [Math]::Atan2($sd[0], -$sd[1]) }
            Send-Game host ("#g_localActor:SetAngles({x=0,y=0,z=$($script:carryYaw)})")
            Pause-Game 1
            Send-Game host "coop_test_key moveforward 1"
            Pause-Game 1.2
            Send-Game host "coop_test_key moveforward 0"
            Pause-Game 1.5
            $mh = Get-PW host $m; $mc = Get-PW companion $m
            Require ($mh.Count -ge 5 -and $mc.Count -ge 5) "the watch ran in both games ($($mh.Count), $($mc.Count))"
            $lh = $mh[-1]; $lc = $mc[-1]
            Check ((Dist $lh $script:thing) -gt 1.5) ("it moved with him in the host's game ({0:N1} m from where it lay)" -f (Dist $lh $script:thing))
            Check ((Dist $lc $script:thing) -gt 1.5) ("... and in the companion's game ({0:N1} m)" -f (Dist $lc $script:thing))
            Check ((Dist $lh $lc) -lt 0.6) ("held in the same place in both games ({0:N2} m apart)" -f (Dist $lh $lc))
        }
        Step "he throws it: the same flight, the same rest" {
            # (looking a little up, over the ground: the throw goes where he looks)
            Send-Game host ("#g_localActor:SetAngles({x=$($P.ThrowPitch),y=0,z=$($script:carryYaw)})")
            Pause-Game 1
            $m = New-Mark
            Send-Game host "coop_test_key use 1"
            Pause-Game 0.15
            Send-Game host "coop_test_key use 0"
            $thrown = Wait-Log host ('^\[CoopProp\] Nomad throws ' + [regex]::Escape($n)) 8 $m
            Require ($null -ne $thrown) "the host threw it: $(if ($thrown) { $thrown.Text } else { 'no line' })"
            $restH = Wait-Log host ('^\[CoopProp\] ' + [regex]::Escape($n) + ' \(\d+\) rests at (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)') 15 $m
            $restC = Wait-Log companion ('^\[CoopProp\] ' + [regex]::Escape($n) + ' \(\d+\) rests at (-?[\d.]+) (-?[\d.]+) (-?[\d.]+) \(here it was ([\d.]+) m off\)') 15 $m
            Pause-Game 2
            Send-Game host '#PW.on=false'
            Send-Game companion '#PW.on=false'
            Check ($null -ne $restH) "it came to rest in the host's game: $(if ($restH) { $restH.Text })"
            Check ($null -ne $restC) "the companion's game was told where: $(if ($restC) { $restC.Text })"
            $fh = Get-PW host $m; $fc = Get-PW companion $m
            if ($fh.Count -ge 3 -and $fc.Count -ge 3) {
                $flown = Dist $fh[0] $fh[-1]
                $dev = 0.0
                foreach ($q in $fc) { $best = 1e9; foreach ($p in $fh) { $d = Dist $q $p; if ($d -lt $best) { $best = $d } }; if ($best -gt $dev) { $dev = $best } }
                $gap = Dist $fh[-1] $fc[-1]
                Check ($flown -ge 2.0) ("it flew {0:N1} m in the host's game" -f $flown)
                Check ($dev -lt 1.0) ("the companion's game saw the same path (at most {0:N2} m off it)" -f $dev)
                Check ($gap -lt 0.3) ("at rest at the same spot in both games ({0:N2} m apart)" -f $gap)
            } else { Check $false "the watch has the flight ($($fh.Count), $($fc.Count))" }
            $grenade = Find-Log companion '^\[CoopAnim\] Nomad starts a throw from the network' $m
            Check ($null -eq $grenade) "no grenade throw shown for it in the companion's game$(if ($grenade) { ': ' + $grenade.Text })"
        }
    }
}
