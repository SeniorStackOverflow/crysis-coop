# The user's games of 2026-10-16/17: at the north end of the bridge the
# reinforcement truck's soldiers came; the companion fought in the open,
# hid from the one it aimed at while others shot it, peeked blind, never
# cloaked, went down in about 10 s, and everybody was down in 3 of 4 games.
# Here the same fight on foot: the host (immortal, standing back) calls the
# truck at the bridge; the companion (not immortal) fights its soldiers for
# 120 s. It should know who shoots it, take cover from them, fight from
# there (down, up, short bursts), get away cloaked when pounded, live, kill.
@{
    Name = "companion-fight-bridge-sound"
    Tags = @("companion", "combat", "tactics")
    SummaryRu = "Тот же бой у моста со звуком (только в WAV-файл) и сторожем звука: в бою ни ложных сбоев, ни перезапусков звука"
    Session = @{ Fixture = "new:rescue"; Companion = $true; Sound = "wav" }
    Params = @{ Fight = 120; Truck = "Bridge_reinforcement_Truck_1"; HostCvars = @("coop_test_sound watch"); LookNet = 1 }
    Body = {
        param($P)
        Step "the host at the bridge calls the truck" {
            Send-Game host "coop_god 1"
            Pause-Game 30
            # (the soldiers elsewhere along the road out of it: this fight only)
            Send-Game host '#local p={x=1737,y=1960,z=0}; local k=0; for _,e in ipairs(System.GetEntitiesInSphere(p,400) or {}) do if e.actor and not e.actor:IsPlayer() and e.actor:GetHealth()>0 and not string.find(e:GetName(),"Bridge_Reinforcement") then g_gameRules:CreateHit(e.id,g_localActorId,g_localActorId,9000,nil,nil,nil,"bullet"); k=k+1 end end; System.LogAlways("[CHK] others shot: "..k)'
            $m = New-Mark
            Send-Game host '#local p={x=1737,y=1960,z=0}; p.z=System.GetTerrainElevation(p)+1; g_gameRules.game:MovePlayer(g_localActorId,p,{x=0,y=0,z=0}); local c=System.GetEntityByName("Companion"); g_gameRules.game:MovePlayer(c.id,{x=p.x-4,y=p.y-3,z=p.z},{x=0,y=0,z=0}); System.LogAlways("[CHK] the players by the bridge")'
            $truck = Wait-Log host ('\] Bridge_Reinforcement_\d entered vehicle ' + $P.Truck) 90 $m
            Require ($null -ne $truck) "the truck came with its soldiers"
            # the host steps back out of the fight (immortal, he only watches);
            # the companion is not immortal from now on
            Send-Game host '#local p={x=1731,y=1930,z=0}; p.z=System.GetTerrainElevation(p)+1; g_gameRules.game:MovePlayer(g_localActorId,p,{x=0,y=0,z=0}); System.LogAlways("[CHK] the host back from the fight")'
            Send-Game host "coop_god 2"
            # (each burst: at whom, what the aim meets first; a cvar the server
            # syncs to its clients: set on the host, the companion's own set
            # was ignored)
            Send-Game host "coop_test_shottrace 1"
            # (its turns sent at once as a mouse turn is, or only with its key
            # presses as before, coop_agent_look_net 0: the host's copy kept
            # looking the old way, its body did not turn to its fire)
            Send-Game host "coop_agent_look_net $($P.LookNet)"
            foreach ($c in $P.HostCvars) { Send-Game host $c }
            Start-Watch @("Bridge_Reinforcement_1", "Bridge_Reinforcement_2", "Bridge_Reinforcement_3", "Bridge_Reinforcement_4", "Bridge_Reinforcement_5")
        }
        Step "the fight" {
            $m = New-Mark
            $end = (Get-Date).AddSeconds($P.Fight)
            while ((Get-Date) -lt $end) {
                Pause-Game 5
                if ((Count-Log host '\] HIT Bridge_Reinforcement_\d by \S+ .*KILLED' $m) -ge 5) { break }
                if ((Count-Log host '^\[Coop\] Companion is down' $m) -ge 1) { break }
            }
            Send-Game host "coop_god 1"
            Pause-Game 2
            $w = @(Get-Watch $m)
            $alive = @($w | Where-Object { $_.Hp -gt 0 })
            $dmg = @(Find-Log host '^\[Coop\] DAMAGE player Companion ' $m -All)
            $hurt = @($dmg | Where-Object { $_.Text -match 'taken=([1-9][\d.]*)' }).Count
            $shotBy = Count-Log companion '^\[CoopAgent\] shot by ' $m
            $kills = Count-Log host '\] HIT Bridge_Reinforcement_\d by Companion .*KILLED' $m
            $hits = Count-Log host '\] HIT (Bridge_Reinforcement_\d|vehicle \S+) by Companion ' $m
            # (into the truck's armor or into a soldier: at the gunner's chest it
            # was nearly all the truck)
            $onSoldiers = Count-Log host '\] HIT Bridge_Reinforcement_\d by Companion ' $m
            $onTruck = Count-Log host '\] HIT vehicle \S+ by Companion ' $m
            # (a head hit: thousands of damage, a body hit 52-60 of 180)
            $heads = @(Find-Log host '\] HIT Bridge_Reinforcement_\d by Companion .* dmg=([\d.]+)' $m -All | Where-Object { $_.Text -match 'dmg=([\d.]+)' -and [double]$Matches[1] -ge 1000 }).Count
            $downs = Count-Log host '^\[Coop\] Companion is down' $m
            $asks = @(Find-Log host '^\[CoopAgent\] cover for Companion from (.*)' $m -All | ForEach-Object { $_.Text -replace '^.*cover for Companion from ', '' })
            $exposed = if ($alive.Count) { @($alive | Where-Object { $_.Seen -gt 0 }).Count / $alive.Count } else { 1 }
            $fights = @(Find-Log companion '^\[CoopAgent\] the last minute in fights: (.*)' $m -All | ForEach-Object { $_.Text -replace '^.*fights: ', '' })
            $suit = @(Find-Log companion '^\[CoopAgent\] suit: (.*)' $m -All | ForEach-Object { $_.Text -replace '^.*suit: ', '' })
            $cloak = @(Find-Log companion 'cloaks to get away' $m -All).Count
            $cloaked = @(Find-Log companion '^\[CoopAgent\] suit: \S+ -> cloak' $m -All).Count
            $refused = @(Find-Log companion '^\[CoopAgent\] suit: .* refused' $m -All | ForEach-Object { $_.Text -replace '^.*suit: ', '' })
            $sj = @(Find-Log companion 'a strength jump onto' $m -All).Count
            # hits taken while cloaked (the soldiers told every frame they saw it,
            # cloaked or not: 11 hits in one cloak before the fix), soldiers losing it
            $inCloak = $false; $cloakHits = 0
            foreach ($l in @(Find-Log companion '^\[CoopAgent\] (suit: \S+ -> \S+|shot by )' $m -All)) {
                if ($l.Text -match 'suit: \S+ -> (\S+)') { $inCloak = ($Matches[1] -eq 'cloak') }
                elseif ($inCloak) { $cloakHits++ }
            }
            $lost = Count-Log host '^\[CoopAI\] \S+ lost sight of Companion: cloaked' $m
            Check $true ("the host counted $($dmg.Count) hits on it ($hurt hurt it), it knew of $shotBy; it hit them $hits times, killed $kills; down $downs times; seen by a soldier {0:P0} of the time" -f $exposed)
            Check $true ("cover asked: " + $(if ($asks.Count) { ($asks | Select-Object -First 5) -join ' | ' } else { 'none' }))
            Check $true ("its fights: " + $(if ($fights.Count) { $fights -join ' | ' } else { 'no line' }))
            Check $true ("suit: " + $(if ($suit.Count) { ($suit | Select-Object -First 6) -join ' | ' } else { 'no switch' }) + "; wanted to get away cloaked $cloak times, cloaked $cloaked times; strength jumps $sj")
            Check ($refused.Count -eq 0) ("the suit refused no mode" + $(if ($refused.Count) { ": " + (($refused | Select-Object -First 2) -join ' | ') } else { "" }))
            Check ($cloak -eq 0 -or $cloaked -ge 1) "it cloaked when it wanted to get away cloaked ($cloaked of $cloak)"
            $ramps = @(Find-Log host "^\[CoopAI\] Companion's cloak for the soldiers' own eyes 1.5 s on: ([\d.]+) of 1.0" $m -All | ForEach-Object { [double]($_.Text -replace '^.* on: ([\d.]+) of.*$', '$1') })
            # (a cloak shorter than 1.5 s, the suit out of energy: how far the
            # soldiers' eyes got by its end, from the line when it ended)
            $ends = @(Find-Log host "^\[CoopAI\] Companion's cloak for the soldiers' own eyes: 0\.0 \(now ([\d.]+); the suit not cloaked" $m -All | ForEach-Object { [double]($_.Text -replace '^.*\(now ([\d.]+);.*$', '$1') })
            $took = @($ramps | Where-Object { $_ -ge 0.9 }).Count + @($ends | Where-Object { $_ -ge 0.2 -and $_ -lt 0.99 }).Count
            $ramps = @($ramps) + @($ends | Where-Object { $_ -lt 0.99 })
            Check ($cloaked -eq 0 -or $took -ge 1) "the soldiers' eyes took its cloak ($took of $($ramps.Count) cloaks); hit $cloakHits times while cloaked; lost by the coop vision $lost times"
            Check ($sj -le 4) "not jumping again and again at a ledge ($sj strength jumps)"
            # its leans out round a cover's side reach the server (they were let
            # go of before the input was sent: the host never saw one)
            $leanPeeks = 0
            foreach ($f in $fights) { if ($f -match '(\d+) leaning out') { $leanPeeks += [int]$Matches[1] } }
            $hostLeans = Count-Log host '^\[Coop\] Companion leans (left|right) \(as the server sees it' $m
            Check ($leanPeeks -eq 0 -or $hostLeans -ge 1) "the server saw it lean out ($hostLeans lines; it leaned out $leanPeeks times)"
            Check ($dmg.Count -eq 0 -or $shotBy -ge 0.8 * $dmg.Count) "it knew of at least 80 % of the hits on it ($shotBy of $($dmg.Count))"
            Check ($downs -eq 0) "never down"
            # (the soldiers themselves, not their truck's armor)
            # (a head hit kills: fewer hits, more kills; either counts)
            Check ($onSoldiers -ge 3 -or $kills -ge 1) "it hit the soldiers $onSoldiers times, $heads of them in the head, killed $kills (and hit their truck $onTruck times)"
            $bursts = @(Find-Log companion '^\[CoopAgent\] burst at ' $m -All | ForEach-Object { $_.Text })
            $atGunner = @($bursts | Where-Object { $_ -match '\(\d+ m, in ' }).Count
            $intoVehicle = @($bursts | Where-Object { $_ -match 'the aim meets \S*[Tt]ruck' }).Count
            # (where the gun itself pointed: in the sights the game took the
            # shot's direction from a camera the companion's game never moved,
            # 27-52 degrees off)
            $gunOff = @($bursts | Where-Object { $_ -match 'the gun ([\d.]+) deg off' -and [double]$Matches[1] -gt 5.0 }).Count
            Check ($bursts.Count -eq 0 -or $gunOff -le [Math]::Floor($bursts.Count / 10)) "its gun pointed at whom it shot ($gunOff of $($bursts.Count) bursts more than 5 degrees off)"
            # (where the host's copy of it aimed against where its shots went)
            $dirs = @(Find-Log host '^\[CoopShotDir\] Companion hit \S+: the shot (\d+) deg off the server copy''s aim' $m -All | ForEach-Object { if ($_.Text -match 'the shot (\d+) deg off') { [int]$Matches[1] } })
            $sorted = @($dirs | Sort-Object)
            $median = if ($sorted.Count) { $sorted[[Math]::Floor($sorted.Count / 2)] } else { -1 }
            $near = @($dirs | Where-Object { $_ -le 10 }).Count
            Check $true ("its shots against its aim as the host sees it: " + $(if ($dirs.Count) { "median $median deg, $near of $($dirs.Count) within 10 deg: " + (($dirs | Select-Object -First 15) -join ' ') } else { 'no line' }))
            if ($P.LookNet -eq 1) { Check ($dirs.Count -eq 0 -or $near -ge [Math]::Ceiling(0.8 * $dirs.Count)) "on the host it looks where it shoots ($near of $($dirs.Count) hits within 10 deg of its aim)" }
            # (the sound watch: no fault and no restart of the sound system in a
            # fight, when the game has sound: the -sound variant writes it to a
            # WAV file)
            $soundFaults = @(Find-Log host '^\[CoopSound\] (the sound.s master group was|the sound.s mix is not|the heard sounds stand|sounds are heard and the mix is empty|the master group was set wrong|no sound came out)' $m -All | ForEach-Object { $_.Text -replace '^\[CoopSound\] ', '' })
            $soundLooks = @(Find-Log host '^\[CoopSound\] \d+ s without sound' $m -All).Count
            Check ($soundFaults.Count -eq 0) ("no sound fault or restart in the fight" + $(if ($soundFaults.Count) { ": " + (($soundFaults | Select-Object -First 3) -join ' | ') } else { "" }) + "; silent stretches logged: $soundLooks")
            Check $true ("its bursts: $($bursts.Count), $atGunner of them at one in a vehicle, $intoVehicle with the aim on a truck first" + $(if ($bursts.Count) { "; e.g. " + ((($bursts | Select-Object -First 2) -replace '^\[CoopAgent\] ', '') -join ' | ') } else { "" }))
        }
        Stop-Watch
    }
}
