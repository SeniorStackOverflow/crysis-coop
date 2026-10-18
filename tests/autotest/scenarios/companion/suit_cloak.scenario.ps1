# The user (2026-10-17): the companion should use all the suit's modes. In
# the bridge fights it "cloaked to get away" (the log said so) but the suit
# never went to cloak: no "suit: armor -> cloak" line, the armor drained on.
# Here: is cloak given to it at all, does its own game take it, does the
# server's copy of it go cloaked too, and do the soldiers lose it then? The
# companion (immortal) stands in the open with five soldiers 25 m off,
# turned to it; it shoots at them 2 s (they know it is there), then holds
# fire: 12 s in armor, then 12 s cloaked: the hits on it and the soldiers
# looking at it are counted in each.
@{
    Name = "companion-suit-cloak"
    Tags = @("companion", "combat", "suit")
    SummaryRu = "Камуфляж компаньона: костюм его принимает, на сервере он тоже невидим, солдаты теряют его и почти не попадают"
    Session = @{ Fixture = "new:rescue"; Companion = $true }
    Params = @{ Phase = 12 }
    Body = {
        param($P)
        function Count-Phase($m) {
            $att = @(Find-Log host '^\[CHK\] cloak att=(\d+) of (\d+) suit=(-?\d+) en=(-?[\d.]+)' $m -All)
            $looked = 0; $all = 0; $suits = @{}
            foreach ($a in $att) {
                if ($a.Text -match 'att=(\d+) of (\d+) suit=(-?\d+) en=(-?[\d.]+)') { $looked += [int]$Matches[1]; $all += [int]$Matches[2]; $suits[$Matches[3]] = 1 + [int]$suits[$Matches[3]] }
            }
            # (under coop_god there is no DAMAGE line, the hit lines say "god")
            $hits = Count-Log host '^\[Coop\] HIT player Companion by ' $m
            return @{ Hits = $hits; Looked = $looked; All = $all; Samples = $att.Count; Suits = (($suits.Keys | Sort-Object | ForEach-Object { "suit $_ x$($suits[$_])" }) -join ', ') }
        }
        Step "the companion in the open, five soldiers 25 m off" {
            Send-Game host "coop_god 1"
            Pause-Game 20
            Send-Game host '#local p={x=1737,y=1960,z=0}; p.z=System.GetTerrainElevation(p)+1; local c=System.GetEntityByName("Companion"); g_gameRules.game:MovePlayer(c.id,p,{x=0,y=0,z=0}); local h={x=1737,y=1840,z=0}; h.z=System.GetTerrainElevation(h)+1; g_gameRules.game:MovePlayer(g_localActorId,h,{x=0,y=0,z=0}); CloakFoes={}; local keep={}; local n=0; for i=1,30 do local e=System.GetEntityByName("Bridge_"..i); if e and e.actor and e.actor:GetHealth()>0 and n<5 then local t={x=p.x+(n-2)*5,y=p.y+25,z=0}; t.z=System.GetTerrainElevation(t)+0.3; e:SetWorldPos(t); e:SetWorldAngles({x=0,y=0,z=math.pi}); keep[e.id]=true; table.insert(CloakFoes,e.id); n=n+1 end end; local k=0; for _,e in ipairs(System.GetEntitiesInSphere(p,400) or {}) do if e.actor and not e.actor:IsPlayer() and e.actor:GetHealth()>0 and not keep[e.id] then g_gameRules:CreateHit(e.id,g_localActorId,g_localActorId,9000,nil,nil,nil,"bullet"); k=k+1 end end; System.LogAlways("[CHK] soldiers by the companion: "..n..", others shot: "..k)'
            Send-Game host '#CloakOn=true; function CloakTick() if not CloakOn then return end local c=System.GetEntityByName("Companion"); local n,k=0,0; for _,id in ipairs(CloakFoes or {}) do local e=System.GetEntity(id); if e and e.actor and e.actor:GetHealth()>0 then k=k+1; local ok,t=pcall(AI.GetAttentionTargetOf,e.id); if ok and t==c:GetName() then n=n+1 end end end; System.LogAlways(string.format("[CHK] cloak att=%d of %d suit=%d en=%.0f",n,k,c.actor:GetNanoSuitMode(),c.actor:GetNanoSuitEnergy())); Script.SetTimer(1000,CloakTick) end; CloakTick()'
            Send-Agent "hold" | Out-Null
            Send-Agent "fire off" | Out-Null
            Pause-Game 6
        }
        $armor = $null
        Step "in armor" {
            Send-Agent "suit armor" | Out-Null
            Send-Agent "fire on" | Out-Null
            Pause-Game 2
            Send-Agent "fire off" | Out-Null
            $m = New-Mark
            Pause-Game $P.Phase
            $script:armor = Count-Phase $m
            Check $true ("in armor: {0} hits on it, soldiers looking at it {1} of {2} ({3} samples; {4})" -f $script:armor.Hits, $script:armor.Looked, $script:armor.All, $script:armor.Samples, $script:armor.Suits)
        }
        Step "cloaked" {
            Send-Game host '#local c=System.GetEntityByName("Companion"); local ok=pcall(function() c.actor:SetNanoSuitEnergy(200) end); System.LogAlways("[CHK] the companion energy full: "..tostring(ok))'
            Pause-Game 1
            $mk = New-Mark
            $r = Send-Agent "suit cloak"
            Check ($r.ok) ("its suit took cloak: " + $(if ($r.ok) { $r.result } else { $r.error }))
            Pause-Game 2
            $m = New-Mark
            Pause-Game $P.Phase
            $c = Count-Phase $m
            $a = $script:armor
            Check $true ("cloaked: {0} hits on it, soldiers looking at it {1} of {2} ({3} samples; {4})" -f $c.Hits, $c.Looked, $c.All, $c.Samples, $c.Suits)
            $refused = @(Find-Log companion '^\[CoopAgent\] suit: .* refused' $mk -All | ForEach-Object { $_.Text })
            Check ($refused.Count -eq 0) ("no mode refused" + $(if ($refused.Count) { ": " + ($refused | Select-Object -First 2) -join ' | ' } else { "" }))
            $cloakSamples = @(Find-Log host '^\[CHK\] cloak att=\d+ of \d+ suit=2 ' $m -All).Count
            Check ($cloakSamples -ge [Math]::Floor($c.Samples / 2)) "the server saw it cloaked in $cloakSamples of $($c.Samples) samples"
            # the soldiers' own eyes: the engine moved a joined player's cloak
            # for them only for the host (it stayed 0, they saw it cloaked)
            $ramp = Find-Log host "^\[CoopAI\] Companion's cloak for the soldiers' own eyes 1.5 s on: ([\d.]+) of 1.0" $mk
            Check ($ramp -and [double]($ramp.Text -replace '^.* on: ([\d.]+) of.*$', '$1') -ge 0.9) ("the soldiers' eyes took the cloak: " + $(if ($ramp) { $ramp.Text -replace '^.*eyes ', '' } else { 'no line' }))
            if ($a.Looked -ge 5) { Check ($c.Looked -le [Math]::Floor($a.Looked / 3)) "cloaked the soldiers had it as their target far less ($($c.Looked) against $($a.Looked) samples in armor)" }
            else { Check $true "too few soldiers looking at it in armor to compare ($($a.Looked); cloaked $($c.Looked))" }
            # (the soldiers put there seldom shoot: then the hits say nothing)
            if ($a.Hits -ge 3) { Check ($c.Hits -le [Math]::Floor($a.Hits / 3)) "cloaked it was hit far less ($($c.Hits) against $($a.Hits) in armor)" }
            else { Check $true "too few hits in armor to compare ($($a.Hits); cloaked $($c.Hits))" }
            Send-Agent "suit auto" | Out-Null
            Send-Agent "fire on" | Out-Null
            Send-Game host '#CloakOn=false'
        }
    }
}
