# The user's game of 2026-10-17: in the fight by the bridge the host and the
# AI companion both went down in the middle of shooting; a sound of gunfire
# went on and on, for ever. Here: soldiers 25 m off by the bridge; (1) the
# companion shoots at them and is struck down while firing, (2) the host
# holds the trigger and is struck down. Then no fire sound may loop on, on the
# host's game, and none after he is up again.
@{
    Name = "downed-while-firing-sound"
    Tags = @("sound", "combat", "revive")
    SummaryRu = "Компаньон и хост падают посреди стрельбы: звук стрельбы не зацикливается"
    Session = @{ Fixture = "new:rescue"; Companion = $true; Sound = "wav" }
    Params = @{}
    Body = {
        param($P)
        function Get-Loops([string]$Tag) {
            # FMOD's own channels (a shot's sound is no entity's): the ones
            # that loop for ever, not the level's ambience nor an engine
            $m = New-Mark
            Send-Game host "coop_test_sound channels"
            Pause-Game 2
            $all = @(Find-Log host '^\[CoopTest\] channel \d+ ' $m -All)
            $loops = @($all | Where-Object { $_.Text -match 'loops -1' -and $_.Text -notmatch '(?i)amb|jungle|wind|ocean|river|water|bird|insect|cicada|rain|engine|_run|idle|rotor|turbine|hum|fridge|motor|vehicle|wave' })
            Check $true ("$Tag`: FMOD channels " + $all.Count + "; looping, not ambience or an engine: " + $(if ($loops.Count) { (($loops | Select-Object -First 6 | ForEach-Object { ($_.Text -replace '^\[CoopTest\] ', '') }) -join "; ") } else { "none" }))
            return $loops
        }
        Step "stage the soldiers by the bridge" {
            Send-Game host "coop_god 1"
            Pause-Game 5
            Send-Game host '#local p={x=1737,y=1960,z=0}; p.z=System.GetTerrainElevation(p)+1; local keep={}; local n=0; for i=1,30 do local e=System.GetEntityByName("Bridge_"..i); if e and e.actor and e.actor:GetHealth()>0 and n<6 then local t={x=p.x+(n-2.5)*5,y=p.y+25,z=0}; t.z=System.GetTerrainElevation(t)+0.3; e:SetWorldPos(t); keep[e.id]=true; n=n+1 end end; local k=0; for _,e in ipairs(System.GetEntitiesInSphere(p,400) or {}) do if e.actor and not e.actor:IsPlayer() and e.actor:GetHealth()>0 and not keep[e.id] then g_gameRules:CreateHit(e.id,g_localActorId,g_localActorId,9000,nil,nil,nil,"bullet"); k=k+1 end end; System.LogAlways("[CHK] soldiers by the bridge: "..n..", others shot: "..k)'
            Send-Game host '#local p={x=1737,y=1960,z=0}; p.z=System.GetTerrainElevation(p)+1; local c=System.GetEntityByName("Companion"); g_gameRules.game:MovePlayer(c.id,{x=p.x-4,y=p.y-3,z=p.z},{x=0,y=0,z=0}); g_gameRules.game:MovePlayer(g_localActorId,p,{x=0,y=0,z=0}); System.LogAlways("[CHK] the players by the bridge")'
            Pause-Game 3
        }
        Step "the companion is struck down while it fires" {
            $m = New-Mark
            $shot = Wait-Log host '\] HIT \S+ by Companion weapon=' 90 $m
            Require ($null -ne $shot) "the companion shoots at the soldiers"
            Send-Game host "coop_god 2", "coop_test_kill Companion"
            Pause-Game 8
            $loops = @(Get-Loops "8 s after the companion went down firing")
            Check ($loops.Count -eq 0) "8 s after the companion went down firing: no fire sound loops on ($($loops.Count) looping)"
            Send-Game host "coop_god 1"
        }
        Step "the host is struck down holding the trigger" {
            Send-Game host "coop_test_key attack1 1"
            Pause-Game 2
            Send-Game host "coop_god 0", "coop_test_kill Nomad"
            Pause-Game 2
            Send-Game host "coop_test_key attack1 0"
            Pause-Game 6
            $loops = @(Get-Loops "8 s after the host went down firing")
            Check ($loops.Count -eq 0) "8 s after the host went down firing: no fire sound loops on ($($loops.Count) looping)"
            $m = New-Mark
            $back = Wait-Log host "checkpoint loaded|revived Nomad|Nomad is up" 150 $m
            Send-Game host "coop_god 1"
            Pause-Game 10
            $loops = @(Get-Loops "after everybody is up again")
            Check ($loops.Count -eq 0) "after everybody is up again: no fire sound loops on ($($loops.Count) looping)"
        }
    }
}
