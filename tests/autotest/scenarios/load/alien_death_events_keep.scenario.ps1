# The _keep variant: the deaths' tokens kept through the load into the running
# level (CoopTestKeepDeaths, as before the review's fix of 2026-10-24): the load
# step is expected to FAIL (the death by the NPC's own Kill after the load is
# dropped as done), proof the step sees it.
# A review of the graveyard fix (2026-10-24): the aliens' deaths did not reach
# the level either. In co-op the server kills an NPC through the network rules
# (ProcessDeath -> KillPlayer -> CActor::Kill); an alien's own Kill
# (Trooper_x:Kill -> BasicAlien:Kill, the timer for BasicAlien.OnDeath ->
# BasicAI.OnDeath -> Event_Dead, OnDeathCustom) never ran, and the fix for the
# soldiers (CoopGameRules.lua CoopNPCDied) left the aliens out. The level
# files count alien deaths in their flow graphs: core 37, ice 29, sphere 24,
# ascension 9, fleet 32 graph edges from an alien's Dead.
# Here, a new game on Ice (Paradise Lost): its graph Trooper_wave1 (Group AB2,
# server only) feeds the Dead outputs of ab2_TR_wave1a/b/c into Logic:All ->
# game token Level.AB2.Trooperwave1_done = "yes" (its Start sets "not"). The
# host kills the three (g_gameRules:CreateHit, 9000; they wait in the level,
# hidden in their layer till their wave); b and c also get the timer a
# script's own Kill sets (a frozen alien shattered: BasicAlien:Kill), b for
# BasicAlien.OnDeath, c for BasicAI.OnDeath: each death reaches the level once
# all the same. Each Trooper's Event_Dead is counted (wrapped on the entity).
# Players immortal (coop_god 1).
# Expected on 0.11.45 (old): the Troopers die, a's Event_Dead never runs (b's
# and c's only from the test's own timers), the token stays "not": FAIL. On
# the fixed build: "[Coop] ab2_TR_wave1X's death reaches the level (Dead)" 1 s
# after each death, Event_Dead once each, the token "yes", no Lua error: PASS.
@{
    Name = "alien-death-events-keep"
    Tags = @("manual")
    SummaryRu = "То же, но метки смертей не сбрасываются при загрузке (как до исправления): шаг с загрузкой должен упасть"
    Session = @{ Fixture = "new:ice"; Companion = $false; ReadyCommands = @("coop_god 1") }
    Params = @{
        Settle = 10; KillGap = 2; Within = 15; Token = "Level.AB2.Trooperwave1_done"; Names = @("ab2_TR_wave1a", "ab2_TR_wave1b", "ab2_TR_wave1c")
        # the load step: a Trooper of wave 0 killed and the game saved at once;
        # KeepDeaths: the deaths' tokens kept through the load (as before the fix)
        Saved = "ab1_TR_wave0x1"; KeepDeaths = $true
    }
    Body = {
        param($P)
        $names = '{' + (($P.Names | ForEach-Object { '"' + $_ + '"' }) -join ',') + '}'
        $wrapNames = '{' + ((@($P.Names) + @($P.Saved) | ForEach-Object { '"' + $_ + '"' }) -join ',') + '}'
        $defs = @(
            ('#function AtGT(n) local ok,v=pcall(function() return GameToken.GetToken(n) end); if ok then return tostring(v) end return "?" end'),
            ('#function AtTr(tag) local s=""; for _,n in ipairs(' + $names + ') do local e=System.GetEntityByName(n); if e and e.actor then s=s..string.format(" %s:%d%s%s",n,math.floor(e.actor:GetHealth()),e:IsHidden() and "h" or "",e.AI and "" or " noAI") else s=s.." "..n..":none" end end; System.LogAlways("[CHK] troopers "..tag..":"..s.." token="..AtGT("' + $P.Token + '")) end'),
            # (each Trooper's Event_Dead counted, once wrapped)
            ('#for _,n in ipairs(' + $wrapNames + ') do local e=System.GetEntityByName(n); if e and e.Event_Dead and not e.atDeadWrapped then local f=e.Event_Dead; e.atDeadWrapped=true; e.Event_Dead=function(s,...) System.LogAlways("[CHK] Event_Dead "..s:GetName()); return f(s,...) end end end; System.LogAlways("[CHK] Event_Dead wrapped")')
        )
        function Get-Troopers([string]$Tag) {
            $m = New-Mark
            Send-Game host ('#AtTr("' + $Tag + '")')
            $l = Wait-Log host ('^\[CHK\] troopers ' + [regex]::Escape($Tag) + ':(.*) token=(\S+)$') 5 $m
            if (-not $l) { return $null }
            $list = @([regex]::Matches($l.Match.Groups[1].Value, ' (\S+?):(none|(-?\d+)(h?)( noAI)?)') | ForEach-Object {
                [pscustomobject]@{ Name = $_.Groups[1].Value; There = ($_.Groups[2].Value -ne 'none'); Hp = $(if ($_.Groups[3].Success) { [int]$_.Groups[3].Value } else { 0 }); Hidden = ($_.Groups[4].Value -eq 'h'); AI = (-not $_.Groups[5].Success) } })
            return [pscustomobject]@{ List = $list; Token = $l.Match.Groups[2].Value; Text = ($l.Text -replace '^\[CHK\] ', '') }
        }

        Step "Ice: the first Trooper wave waits in the level, its token 'not'" {
            Send-Game host "coop_god 1"
            # (the level's intro first: cine_1_outsideSpaceship, 79 s)
            $intro = Wait-Log host '^\[CoopCutscene\] cine_1_outsideSpaceship ended' 150 (Get-Boot).Marks.Ready
            Check $true ("the intro: " + $(if ($intro) { $intro.Text } else { 'no end line within 150 s' }))
            Pause-Game $P.Settle
            $mW = New-Mark
            Send-Game host $defs
            Require ($null -ne (Wait-Log host '^\[CHK\] Event_Dead wrapped$' 5 $mW)) "the test's Lua on the host"
            $t = Get-Troopers "before"
            Require ($null -ne $t) "the Troopers' list came back from the host"
            Check $true ("before: " + $t.Text)
            Require (@($t.List | Where-Object { $_.There -and $_.Hp -gt 0 -and $_.AI }).Count -eq $P.Names.Count) ("the three Troopers of the wave are in the level, alive, with their AI (" + $t.Text + ")")
            Require ($t.Token -ne 'yes') ("the wave's token is not 'yes' before any of them died (" + $t.Token + ")")
            $script:adMark = New-Mark
        }
        Step "the host kills the three; b and c also get a script Kill's timer (BasicAlien.OnDeath, BasicAI.OnDeath)" {
            $extra = @{ 1 = 'Script.SetTimerForFunction(1000,"BasicAlien.OnDeath",e)'; 2 = 'Script.SetTimerForFunction(1000,"BasicAI.OnDeath",e)' }
            for ($i = 0; $i -lt $P.Names.Count; $i++) {
                $x = if ($extra.ContainsKey($i)) { '; ' + $extra[$i] } else { '' }
                Send-Game host ('#local e=System.GetEntityByName("' + $P.Names[$i] + '"); if e and e.actor and e.actor:GetHealth()>0 then g_gameRules:CreateHit(e.id,g_localActorId,g_localActorId,9000,nil,nil,nil,"bullet")' + $x + ' end')
                Pause-Game $P.KillGap
            }
            Pause-Game 2
            $t = Get-Troopers "after the kills"
            Require ($null -ne $t) "the Troopers' list after the kills came back"
            Check $true ("after the kills: " + $t.Text)
            Require (@($t.List | Where-Object { $_.There -and $_.Hp -le 0 }).Count -eq $P.Names.Count) ("the three are dead (" + $t.Text + ")")
        }
        Step "the level's answer: each death once in its flow graph, the wave's token 'yes'" {
            $m = $script:adMark
            $token = $null
            $until = (Get-Date).AddSeconds($P.Within)
            while ((Get-Date) -lt $until) {
                $t = Get-Troopers "watch"
                if ($t -and $t.Token -eq 'yes') { $token = $t; break }
                Pause-Game 2
            }
            Pause-Game 3
            $t2 = Get-Troopers "the end"
            foreach ($n in $P.Names) {
                $dead = @(Find-Log host ('^\[CHK\] Event_Dead ' + [regex]::Escape($n) + '$') $m -All)
                $reach = @(Find-Log host ('^\[Coop\] ' + [regex]::Escape($n) + '''s death reaches the level \(Dead\)') $m -All)
                Check ($dead.Count -eq 1) ("$n`: its Event_Dead (the flow graph's Dead) ran once ($($dead.Count) times; 'death reaches the level' $($reach.Count))")
            }
            Check ($null -ne $token) ("the wave's token $($P.Token) became 'yes' within $($P.Within) s of the kills (" + $(if ($t2) { $t2.Text } else { 'no line' }) + ")")
            $lua = @(Find-Log host '(?i)lua error|\[lua\]|attempt to (call|index|compare|perform|concatenate)' $m -All)
            Check ($lua.Count -eq 0) ("no Lua error on the host since the kills" + $(if ($lua.Count) { " ($($lua.Count): " + $lua[0].Text + ")" } else { "" }))
        }
        # The review of 2026-10-24: the tokens that make a death count once
        # stayed on the NPCs' tables, which a load into the running level
        # keeps (the NPC alive again, its old token and "done" still on it);
        # a later death by a script's own Kill (a frozen alien shattered:
        # BasicActor:OnPostFreeze -> self:Kill -> the timer for
        # BasicAlien.OnDeath; no ProcessDeath, no new token) was then dropped
        # as done, the level never got it.
        Step "a load into the running level, then a death by the NPC's own Kill reaches the level" {
            $n = $P.Saved
            if ($P.KeepDeaths) { Send-Game host '#CoopTestKeepDeaths=true' }
            $mS = New-Mark
            Send-Game host "coop_save"
            Require ($null -ne (Wait-Log host '^\[CoopSave\] checkpoint manual saved' 30 $mS)) "the game was saved with $n alive (coop_save)"
            $mK = New-Mark
            Send-Game host ('#local e=System.GetEntityByName("' + $n + '"); if e and e.actor and e.actor:GetHealth()>0 then g_gameRules:CreateHit(e.id,g_localActorId,g_localActorId,9000,nil,nil,nil,"bullet"); System.LogAlways("[CHK] killed ' + $n + '") else System.LogAlways("[CHK] not killed ' + $n + '") end')
            $k = Wait-Log host ('^\[CHK\] (not )?killed ' + [regex]::Escape($n) + '$') 5 $mK
            Require ($k -and -not $k.Match.Groups[1].Success) ("$n killed by the host (" + $(if ($k) { $k.Text } else { 'no line' }) + ")")
            $r1 = Wait-Log host ('^\[Coop\] ' + [regex]::Escape($n) + '''s death reaches the level \(Dead\)') 5 $mK
            Require ($null -ne $r1) "its death reached the level (its token and 'done' set)"
            Pause-Game 2
            $mL = New-Mark
            Send-Game host "coop_god 1", "coop_load"
            $end = Wait-Log host '^\[Coop\] checkpoint loaded: |^\[CoopSave\] progress loaded: ' 300 $mL
            Require ($null -ne $end) "the load into the running level ended"
            Pause-Game 6
            $forgot = Find-Log host '^\[Coop\] checkpoint loaded: the deaths of (\d+) NPCs before it forgotten' $mL
            $mA = New-Mark
            Send-Game host ('#local e=System.GetEntityByName("' + $n + '"); System.LogAlways(string.format("[CHK] after the load: %s hp %d (counted %s, token %s)",tostring(e and e:GetName()),e and e.actor and math.floor(e.actor:GetHealth()) or -1,tostring(e and e.atDeadWrapped),tostring(e and e.coopDeathToken ~= nil)))')
            $a = Wait-Log host '^\[CHK\] after the load: (\S+) hp (-?\d+) ' 5 $mA
            Check $true ("after the load: " + $(if ($forgot) { $forgot.Text -replace '^\[Coop\] ', '' } else { 'no forgotten-deaths line' }) + "; " + $(if ($a) { $a.Text -replace '^\[CHK\] ', '' } else { 'no line' }))
            Require ($a -and [int]$a.Match.Groups[2].Value -gt 0) "$n alive again after the load (the checkpoint had it alive)"
            $mD = New-Mark
            Send-Game host ('#local e=System.GetEntityByName("' + $n + '"); e.actor:SetHealth(0); e:Kill(false,NULL_ENTITY,NULL_ENTITY); System.LogAlways("[CHK] own Kill ' + $n + '")')
            Require ($null -ne (Wait-Log host ('^\[CHK\] own Kill ' + [regex]::Escape($n) + '$') 5 $mD)) "$n died by its own Kill (as a frozen one shattered)"
            Pause-Game 3
            $dead = @(Find-Log host ('^\[CHK\] Event_Dead ' + [regex]::Escape($n) + '$') $mD -All)
            Check ($dead.Count -eq 1) ("$n`: its death by its own Kill after the load reached the level once - Event_Dead ($($dead.Count) times)")
            $lua = @(Find-Log host '(?i)lua error|\[lua\]|attempt to (call|index|compare|perform|concatenate)' $mL -All)
            Check ($lua.Count -eq 0) ("no Lua error on the host since the load" + $(if ($lua.Count) { " ($($lua.Count): " + $lua[0].Text + ")" } else { "" }))
        }
    }
}
