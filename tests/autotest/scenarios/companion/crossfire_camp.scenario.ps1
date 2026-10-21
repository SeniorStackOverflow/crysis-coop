# The user's game of 2026-10-21, checkpoint Rescue_05 (fixture checking1021):
# the host down in the enemy camp, the AI companion by him, soldiers, snipers
# and a machine gun shooting at it from 20-55 m. It fell back 22-28 m from the
# body, hopping from one 4 m cover to the next, took 15 hits on the way, never
# cloaked (the armor had taken all the energy), and died: everybody down.
# Here the same: the host down where the checkpoint starts, the companion
# free. It must stay by the body (cover within 12 m of it), get out of their
# sight quickly, cloak while the energy is there, lie down where nothing else
# hides it, and revive the host or be alive at the end.
@{
    Name = "companion-crossfire-camp"
    Tags = @("companion", "combat", "tactics", "revive")
    SummaryRu = "Бой в лагере (чекпоинт Rescue_05 пользователя, без снайперов): хост упал, компаньон под перекрёстным огнём держится у тела, прячется, использует невидимость, поднимает хоста"
    Session = @{ Fixture = "save:checking1021"; Campaign = "56c79107e0d7a6ff"; Companion = $true; ReadyCommands = @("coop_god 1") }
    Params = @{ Watch = 120; MaxDrift = 14; Foes = @("Camp_Force_One_1_s", "Camp_Force_One_2_s", "Camp_Force_One_3", "Camp_Force_Two_1_s", "Camp_Force_Relaxed_3", "Camp_Force_Relaxed_4", "Camp_MG_1", "Camp_MG_3", "Camp_MG_5", "Camp_Sniper_1", "Camp_Sniper_2", "Camp_Sniper_3", "Camp_Assault_11", "Camp_Assault_12", "Camp_Assault_13", "Around_Dome_1", "Around_Dome_2", "Around_Dome_3") }
    Body = {
        param($P)
        Step "the host goes down where the checkpoint starts, the companion by him" {
            Send-Game host "coop_god 1"
            Pause-Game 8
            Send-Agent "free" | Out-Null
            # (the camp's snipers kill with one hit from 60-120 m: the first run
            # went down to health 1 by the first shot, before any tactics; they
            # are left out, the riflemen and the machine gun stay)
            Send-Game host '#local k=0; for i=1,6 do local e=System.GetEntityByName("Camp_Sniper_"..i); if e and e.actor and e.actor:GetHealth()>0 then g_gameRules:CreateHit(e.id,g_localActorId,g_localActorId,9000,nil,nil,nil,"bullet"); k=k+1 end end; System.LogAlways("[CHK] snipers shot: "..k)'
            Send-Game host '#local n=System.GetEntityByName("Nomad"); local c=System.GetEntityByName("Companion"); local p=n:GetWorldPos(); g_gameRules.game:MovePlayer(c.id,{x=p.x-2,y=p.y-1.5,z=p.z+0.3},{x=0,y=0,z=0}); System.LogAlways(string.format("[CHK] body at %.1f %.1f %.1f", p.x, p.y, p.z))'
            Pause-Game 2
            $b = Find-Log host '^\[CHK\] body at (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)' -Last
            Require ($null -ne $b) "the host's place is known"
            $script:body = @([double]$b.Match.Groups[1].Value, [double]$b.Match.Groups[2].Value, [double]$b.Match.Groups[3].Value)
            Start-Watch $P.Foes $script:body
            # (from the host's fall on: the companion cloaks at once)
            $script:fightMark = New-Mark
            Send-Game host "coop_god 0", "coop_test_kill Nomad"
            Pause-Game 1
            # (the host's body takes no more damage; the companion does)
            Send-Game host "coop_god 2"
        }
        Step "the companion's fight for the host" {
            $m = $script:fightMark
            $end = Wait-Log host 'Companion revived Nomad|revived Nomad$|everybody is down|^\[Coop\] Companion is down' $P.Watch $m
            Pause-Game 1
            Send-Game host "coop_god 1"
            Stop-Watch
            $w = @(Get-Watch $m)
            $alive = @($w | Where-Object { $_.Hp -gt 0 })
            $drift = if ($alive.Count) { ($alive | Measure-Object -Property Dg -Maximum).Maximum } else { -1 }
            $hits = Count-Log host '^\[Coop\] DAMAGE player Companion .* taken=[1-9]' $m
            $armored = Count-Log host '^\[Coop\] DAMAGE player Companion .* taken=0\.0' $m
            $downs = Count-Log host '^\[Coop\] Companion is down' $m
            $revived = $null -ne (Find-Log host 'Companion revived Nomad|revived Nomad$' $m)
            $firstHit = @(Find-Log host '^\[Coop\] DAMAGE player Companion ' $m -All | Select-Object -First 1)
            $after = if ($firstHit.Count) { @($alive | Where-Object { $_.No -gt $firstHit[0].No }) } else { @() }
            $hiddenAt = @($after | Where-Object { $_.Seen -eq 0 } | Select-Object -First 1)
            $toHide = if ($after.Count -and $hiddenAt.Count) { $hiddenAt[0].T - $after[0].T } else { -1 }
            $seenShare = if ($after.Count) { @($after | Where-Object { $_.Seen -gt 0 }).Count / $after.Count } else { 0 }
            $cloakedTicks = @($alive | Where-Object { $_.Suit -eq 2 }).Count
            $lyingTicks = @($alive | Where-Object { $_.Head -lt 0.8 }).Count
            $cloaks = @(Find-Log companion '^\[CoopAgent\] cloaks: (.*)' $m -All | ForEach-Object { $_.Text -replace '^\[CoopAgent\] ', '' })
            $falls = @(Find-Log companion '^\[CoopAgent\] hurt \(.*' $m -All | ForEach-Object { $_.Text -replace '^\[CoopAgent\] ', '' })
            $covers = @(Find-Log host '^\[CoopAgent\] cover for Companion from (.*)' $m -All | ForEach-Object { $_.Match.Groups[1].Value })
            $suit = @(Find-Log companion '^\[CoopAgent\] suit: (.*)' $m -All | ForEach-Object { $_.Text -replace '^\[CoopAgent\] suit: ', '' })
            Check $true ("hits on its health {0}, on its armor {1}; down {2}; revived the host: {3}; the end: {4}" -f $hits, $armored, $downs, $revived, $(if ($end) { $end.Text } else { 'none in time' }))
            Check $true ("farthest from the body {0:N1} m; first hit -> out of every sight {1} s; seen {2:P0} of the fight after the first hit; cloaked {3} s, lying down {4} s" -f $drift, $toHide, $seenShare, (0.5 * $cloakedTicks), (0.5 * $lyingTicks))
            Check $true ("cloaked moves: " + $(if ($cloaks.Count) { ($cloaks | Select-Object -First 4) -join ' | ' } else { 'none' }))
            Check $true ("fall backs: " + $(if ($falls.Count) { ($falls | Select-Object -First 4) -join ' | ' } else { 'none' }))
            Check $true ("covers named ($($covers.Count)): " + $(if ($covers.Count) { ($covers | Select-Object -First 5) -join ' | ' } else { 'none' }))
            Check $true ("suit: " + $(if ($suit.Count) { ($suit | Select-Object -First 6) -join ' | ' } else { 'no switch' }))
            Require ($alive.Count -ge 4) "the watch has the fight ($($alive.Count) samples while it was up)"
            Check ($drift -ge 0 -and $drift -le $P.MaxDrift) ("stayed within {0} m of the downed host ({1:N1} m)" -f $P.MaxDrift, $drift)
            Check ($revived -or $downs -eq 0) "revived the host or is still up at the end"
            # (the energy there when it was first hit in the open: a cloak then)
            $firstSample = if ($after.Count) { $after[0] } else { $null }
            if ($firstSample -and $firstSample.En -ge 75) {
                Check ($cloaks.Count -ge 1) "with energy $($firstSample.En) at its first hit it cloaked at least once ($($cloaks.Count))"
            }
        }
    }
}
