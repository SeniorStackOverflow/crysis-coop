# The user's game of 2026-10-18 (0.11.41): "an enemy at a stationary
# turret, in plain view, and the companion does not shoot him". At the
# Rescue field huts field_hut_2 mans the mounted ShiTen17 (in the user's game
# he fired it at the jeep from 46 m). A soldier behind a mounted gun stands
# behind its body: the companion's sight and line-of-fire rays to his chest
# ended in the gun (only a vehicle counted as a gunner's own), a ShiTen hit
# sent it fleeing cloaked as from a truck's gun, and on foot it looked only
# 80 m out. Here the host wakes the huts, the others there are shot, the
# companion holds 42 m in front of the nest (not immortal), the host stands
# far back, immortal.
@{
    Name = "companion-mg-nest"
    Tags = @("companion", "combat")
    SummaryRu = "Враг за станковым пулемётом у полевых хижин (как в игре 18.10): компаньон стреляет в пулемётчика, а не убегает"
    Session = @{ Fixture = "new:rescue"; Companion = $true }
    Params = @{ Gun = "ShiTen17"; Gunner = "field_hut_2"; Spot = @(1650, 2289); Wake = @(1610, 2290); Back = @(1650, 2170); Fight = 70 }
    Body = {
        param($P)
        Step "the host wakes the huts, the companion in front of the nest" {
            Send-Game host "coop_god 1"
            Send-Game host "coop_test_shottrace 1"
            Pause-Game 15
            Send-Game host ('#local p={x=' + $P.Wake[0] + ',y=' + $P.Wake[1] + ',z=0}; p.z=System.GetTerrainElevation(p)+0.5; g_gameRules.game:MovePlayer(g_localActorId,p,{x=0,y=0,z=0}); local c=System.GetEntityByName("Companion"); g_gameRules.game:MovePlayer(c.id,{x=p.x-3,y=p.y-3,z=p.z},{x=0,y=0,z=0}); System.LogAlways("[CHK] the players by the huts")')
            Send-Agent "hold" | Out-Null
            # (the huts' soldiers come out when a player is near)
            $up = $false
            for ($i = 0; $i -lt 12 -and -not $up; $i++) {
                Pause-Game 3
                $mk = New-Mark
                Send-Game host ('#local s=System.GetEntityByName("' + $P.Gunner + '"); System.LogAlways("[CHK] gunner hidden="..tostring(s and s:IsHidden()))')
                $l = Wait-Log host '^\[CHK\] gunner hidden=' 5 $mk
                $up = $l -and $l.Text -match 'hidden=false'
            }
            Require $up "the huts' soldiers came out ($($P.Gunner) shown)"
            # (the others there shot: this fight is the nest's)
            Send-Game host ('#local g=System.GetEntityByName("' + $P.Gun + '"); local p=g:GetWorldPos(); local k=0; for _,e in ipairs(System.GetEntitiesInSphere(p,400) or {}) do if e.actor and not e.actor:IsPlayer() and e.actor:GetHealth()>0 and e:GetName()~="' + $P.Gunner + '" then g_gameRules:CreateHit(e.id,g_localActorId,g_localActorId,9000,nil,nil,nil,"bullet"); k=k+1 end end; local s=System.GetEntityByName("' + $P.Gunner + '"); local q=s:GetWorldPos(); System.LogAlways(string.format("[CHK] others shot: %d; the gunner at %.0f %.0f %.0f, %.1f m from the gun", k, q.x, q.y, q.z, math.sqrt((q.x-p.x)^2+(q.y-p.y)^2)))')
            Send-Game host ('#local c=System.GetEntityByName("Companion"); local t={x=' + $P.Spot[0] + ',y=' + $P.Spot[1] + ',z=0}; t.z=System.GetTerrainElevation(t)+0.3; g_gameRules.game:MovePlayer(c.id,t,{x=0,y=0,z=0}); local h={x=' + $P.Back[0] + ',y=' + $P.Back[1] + ',z=0}; h.z=System.GetTerrainElevation(h)+0.3; g_gameRules.game:MovePlayer(g_localActorId,h,{x=0,y=0,z=0}); System.LogAlways("[CHK] the companion in front of the nest, the host back")')
            # (held where it was put, once its own game has it there: a "hold" right
            # after the move held wherever its game had it a moment later, 1-5 m
            # off, and from x 1652 a hut's post hides the gunner - both builds
            # failed there, 2026-10-23)
            # (the server's MovePlayer does not move a client's own player - its
            # game keeps its place and sends that back: it moves itself)
            $at = $null
            # (and at once a goto there: held at the first step's spot, 45 m west,
            # it walked back toward it)
            for ($i = 0; $i -lt 6; $i++) {
                Send-Game companion ('#local t={x=' + $P.Spot[0] + ',y=' + $P.Spot[1] + ',z=0}; t.z=System.GetTerrainElevation(t)+0.3; System.GetEntity(g_localActorId):SetWorldPos(t)')
                Pause-Game 0.3
                $mA = New-Mark
                Send-Agent ("goto " + $P.Spot[0] + " " + $P.Spot[1] + " 230") | Out-Null
                [void](Wait-Log companion '^\[CoopAgent\] arrived: holding there' 6 $mA)
                $mC = New-Mark
                Send-Game companion '#local p=System.GetEntity(g_localActorId):GetWorldPos(); System.LogAlways(string.format("[CHK] companion at %.1f %.1f %.1f",p.x,p.y,p.z))'
                $at = Wait-Log companion '^\[CHK\] companion at (-?[\d.]+) (-?[\d.]+)' 3 $mC
                if ($at -and [Math]::Abs([double]$at.Match.Groups[1].Value - $P.Spot[0]) -lt 1.0 -and [Math]::Abs([double]$at.Match.Groups[2].Value - $P.Spot[1]) -lt 1.0) { break }
            }
            Require ($at -and [Math]::Abs([double]$at.Match.Groups[1].Value - $P.Spot[0]) -lt 1.5) ("the companion holds at the spot (" + $(if ($at) { $at.Text } else { 'no line' }) + ")")
            Send-Game host "coop_god 2"
        }
        Step "the fight" {
            $m = New-Mark
            $end = (Get-Date).AddSeconds($P.Fight)
            while ((Get-Date) -lt $end) {
                Pause-Game 5
                if ((Count-Log host ('\] HIT ' + $P.Gunner + ' by \S+ .*KILLED') $m) -ge 1) { break }
                if ((Count-Log host '^\[Coop\] Companion is down' $m) -ge 1) { break }
            }
            Send-Game host "coop_god 1"
            Pause-Game 2
            $nestOnIt = Count-Log host ('\] HIT player Companion by ' + $P.Gunner + ' weapon=ShiTen ') $m
            $gunnerAny = @(Find-Log host ('\] HIT \S+( \S+)? by ' + $P.Gunner + ' weapon=(\S+)') $m -All | ForEach-Object { if ($_.Text -match 'weapon=(\S+)') { $Matches[1] } } | Group-Object | ForEach-Object { "$($_.Name) x$($_.Count)" })
            # (from when the huts woke: it may take him on before the fight step)
            $fighting = Count-Log companion ('^\[CoopAgent\] fighting ' + $P.Gunner)
            $hits = @(Find-Log host ('\] HIT ' + $P.Gunner + ' by Companion ') $m -All)
            $killed = @($hits | Where-Object { $_.Text -match 'KILLED' }).Count
            $heads = @($hits | Where-Object { $_.Text -match 'dmg=([\d.]+)' -and [double]$Matches[1] -ge 1000 }).Count
            $fled = Count-Log companion 'cloaks to get away' $m
            $bursts = @(Find-Log companion ('^\[CoopAgent\] burst at ' + $P.Gunner) $m -All).Count
            $notPicked = @(Find-Log companion ('^\[CoopAgent\] not picked ' + $P.Gunner) $m -All | ForEach-Object { $_.Text -replace '^\[CoopAgent\] ', '' })
            $blocked = @(Find-Log companion ('(on foot at|the gun''s line at) ' + $P.Gunner + '.*blocked') $m -All | ForEach-Object { $_.Text -replace '^\[CoopAgent\] ', '' })
            $downs = Count-Log host '^\[Coop\] Companion is down' $m
            Check $true ("the gunner's hits: " + $(if ($gunnerAny) { $gunnerAny -join ', ' } else { 'none' }) + "; on the companion with the ShiTen: $nestOnIt")
            Check $true ("not picked: " + $(if ($notPicked.Count) { ($notPicked | Select-Object -First 3) -join ' | ' } else { 'never' }) + "; blocked lines: " + $(if ($blocked.Count) { ($blocked | Select-Object -First 2) -join ' | ' } else { 'none' }))
            Require ($nestOnIt -ge 1 -or $fighting -ge 1) "the nest took part in the fight (it hit the companion $nestOnIt times, the companion fought its gunner $fighting times)"
            Check ($fighting -ge 1) "the companion picked the gunner behind the mounted gun as its enemy"
            Check ($hits.Count -ge 1) "the companion hit the gunner $($hits.Count) times ($heads in the head, killed $killed; $bursts bursts at him)"
            Check ($fled -le 1) "it did not keep fleeing the nest cloaked ($fled escapes; down $downs times)"
        }
    }
}
