# The user's game of 2026-10-23 (campaign "checking", Game.log 5391-6176): at
# Rescue's extraction point (the graveyard; checkpoint Rescue_10 had just been
# saved) four of the 16 NK_Suit soldiers came out (5, 6, 13, 14). He killed all
# four and nothing happened: no radio lines, no VTOL, "Secure the extraction
# point" stayed open. The level counts the suits' deaths in its flow graph
# (level.pak mission_mission0.xml, graph Graveyard_Assault): the Dead outputs of
# NK_Suit_1..16 feed Math:PortCounter 1355 (total 4) -> 5-8 s -> dialog
# AB5_NKSuit_Clear (Dead2/Dead3 at 2 and 3 dead; every one waits for
# Level.Misc.Dialog_buffer) -> 1-2 s -> game token Level.AB_5.Graveyard_secured
# = "secured" -> SpawnPoint1's graph completes Extraction_Secure
# (Rescue.Objective_11) and activates Extraction_VTOL (Objective_12);
# FG_cine_3_rescueExit plays cine_3_vtolarrives 2-4 s after the token (the
# cutscene's VTOL is cine_3_vtol_AnimDoor, hidden up in the sky till then).
# In co-op the server kills a soldier through the network rules (ProcessDeath
# -> KillPlayer -> CActor::Kill): BasicActor:Kill never ran, so BasicAI.OnDeath
# -> Event_Dead -> the flow graph's "Dead" never came.
# Here, on a copy of that campaign (fixture graveyard1023, Rescue_10): the
# suits the graph has shown (4 of 16, picked at random 1-2 s after the players
# came into the graveyard) are killed by the host one after another
# (g_gameRules:CreateHit, 9000: more than a full suit's armor), then up to 60 s
# for the level's answer. Sound into a WAV file: the radio dialogs gate the
# token, they are to play as in the user's game. Players immortal (coop_god 1).
# Expected on 0.11.45 (old): the suits die ("[Coop] HIT NK_Suit_N by Nomad ...
# KILLED"), no "death reaches the level" line, Level.AB_5.Graveyard_secured
# stays "not", no "[CoopObjective] Rescue.Objective_11 -> completed", no VTOL:
# FAIL (check 2). On the fixed build: "[Coop] NK_Suit_N's death reaches the
# level (Dead)" 1 s after each death (4 or more), the token "secured" and
# "Rescue.Objective_11 -> completed" some 10-30 s after the fourth, then
# cine_3_vtolarrives (the companion's game: "[CoopCutscene] host plays
# cine_3_vtolarrives"): PASS.
@{
    Name = "graveyard-extraction"
    Tags = @("load", "story", "regress")
    SummaryRu = "Кладбище в Rescue (точка эвакуации, чекпоинт Rescue_10, игра 23.10): хост убивает вышедших «нанокостюмов» NK_Suit — их смерть доходит до флоуграфа уровня, точка эвакуации зачищена (Objective_11), прилетает VTOL"
    Session = @{ Fixture = "save:graveyard1023"; Campaign = "56c79107e0d7a6ff"; Companion = $true; Sound = "wav"; ReadyCommands = @("coop_god 1") }
    Params = @{
        Settle = 8; ShowWithin = 60; KillGap = 3; MinDead = 4; Within = 60; CineWithin = 20
        # no suit shown within ShowWithin s (the graph's pending delay lost in the
        # load): hidden ones are killed instead, the user's four first (reported)
        KillHidden = $true; Prefer = @(5, 6, 13, 14)
    }
    Body = {
        param($P)
        $ready = (Get-Boot).Marks.Ready
        # Lua helpers on the host (each line < 1000 characters): a game token's
        # value, the game tokens of the graveyard, the 16 suits (health, hidden,
        # place), the players, the VTOLs
        $defs = @(
            '#function AtGT(n) local ok,v=pcall(function() return GameToken.GetToken(n) end); if ok then return tostring(v) end return "?" end',
            '#function AtTokens(tag) local s=""; for _,n in ipairs({"Graveyard_secured","Player_in_graveyard","Player_way_back","Player_left_Graveyard","Suit_Group_1_dead","Suit_Group_2_dead"}) do s=s.." "..n.."="..AtGT("Level.AB_5."..n) end; System.LogAlways("[CHK] tokens "..tag..":"..s.." Dialog_buffer="..AtGT("Level.Misc.Dialog_buffer")) end',
            '#function AtSuits(tag) local s,a,v="",0,0; for i=1,16 do local e=System.GetEntityByName("NK_Suit_"..i); if e and e.actor then local h=math.floor(e.actor:GetHealth()); local p=e:GetWorldPos(); if h>0 then a=a+1; if not e:IsHidden() then v=v+1 end end; s=s..string.format(" %d:%d%s@%.0f,%.0f,%.0f",i,h,e:IsHidden() and "h" or "",p.x,p.y,p.z) end end; System.LogAlways(string.format("[CHK] suits %s: alive %d shown %d;%s",tag,a,v,s)) end',
            '#function AtWho(tag) local function f(e) if not (e and e.actor) then return "-" end local p=e:GetWorldPos(); local ok,v=pcall(function() return e.actor:GetLinkedVehicleId() end); local ve=ok and v and System.GetEntity(v); return string.format("%.0f,%.0f,%.0f hp %d%s",p.x,p.y,p.z,math.floor(e.actor:GetHealth()),ve and (" in "..ve:GetName()) or "") end; System.LogAlways("[CHK] players "..tag..": host "..f(System.GetEntity(g_localActorId)).." companion "..f(System.GetEntityByName("Companion"))) end',
            '#function AtVtol(tag) local s=""; for _,n in ipairs({"cine_3_vtol_AnimDoor","Vtol_exit"}) do local e=System.GetEntityByName(n); if e then local p=e:GetWorldPos(); s=s..string.format(" %s %s at %.0f,%.0f,%.0f;",n,e:IsHidden() and "hidden" or "shown",p.x,p.y,p.z) else s=s.." "..n.." none;" end end; System.LogAlways("[CHK] vtol "..tag..":"..s) end'
        )
        # every 2 s from the kills on: the token, the dialog lock, the cutscene's VTOL
        $tick = '#AtG={on=true,t=0}; function AtGTick() if not AtG.on then return end local d=System.GetEntityByName("cine_3_vtol_AnimDoor"); local p=d and d:GetWorldPos() or {x=0,y=0,z=0}; System.LogAlways(string.format("[CHK] g t=%.0f secured=%s buffer=%s door=%s %.0f,%.0f,%.0f",AtG.t,AtGT("Level.AB_5.Graveyard_secured"),AtGT("Level.Misc.Dialog_buffer"),d and (d:IsHidden() and "hidden" or "shown") or "none",p.x,p.y,p.z)); AtG.t=AtG.t+2; Script.SetTimer(2000,AtGTick) end; AtGTick()'
        $killRx = '\] HIT (NK_Suit_\d+) by (\S+) .*KILLED'
        function Get-Suits([string]$Tag) {
            $m = New-Mark
            Send-Game host ('#AtSuits("' + $Tag + '")')
            $l = Wait-Log host ('^\[CHK\] suits ' + [regex]::Escape($Tag) + ': alive (\d+) shown (\d+);(.*)$') 5 $m
            if (-not $l) { return $null }
            $list = @([regex]::Matches($l.Match.Groups[3].Value, ' (\d+):(-?\d+)(h?)@(-?\d+),(-?\d+),(-?\d+)') | ForEach-Object {
                [pscustomobject]@{ I = [int]$_.Groups[1].Value; Hp = [int]$_.Groups[2].Value; Hidden = ($_.Groups[3].Value -eq 'h'); X = [int]$_.Groups[4].Value; Y = [int]$_.Groups[5].Value; Z = [int]$_.Groups[6].Value } })
            return [pscustomobject]@{ Alive = [int]$l.Match.Groups[1].Value; Shown = [int]$l.Match.Groups[2].Value; List = $list; Text = ($l.Text -replace '^\[CHK\] ', '') }
        }
        function Get-Tokens([string]$Tag) {
            $m = New-Mark
            Send-Game host ('#AtTokens("' + $Tag + '")')
            $l = Wait-Log host ('^\[CHK\] tokens ' + [regex]::Escape($Tag) + ':') 5 $m
            $v = '?'
            if ($l -and $l.Text -match 'Graveyard_secured=(\S+)') { $v = $Matches[1] }
            return [pscustomobject]@{ Secured = $v; Text = $(if ($l) { $l.Text -replace '^\[CHK\] ', '' } else { 'no line' }) }
        }
        function Get-ChkLine([string]$Call, [string]$Rx) {
            $m = New-Mark
            Send-Game host ('#' + $Call)
            $l = Wait-Log host $Rx 5 $m
            if ($l) { return ($l.Text -replace '^\[CHK\] ', '') } else { return 'no line' }
        }
        # a value's changes over the samples: "not -> dialog -> not"
        function Get-Runs($Vals) {
            $out = New-Object System.Collections.ArrayList
            foreach ($v in @($Vals)) { if (-not $out.Count -or $out[$out.Count - 1] -ne $v) { [void]$out.Add($v) } }
            if ($out.Count) { return ($out -join ' -> ') } else { return 'no samples' }
        }
        function Get-KillText($Hits) {
            if (-not @($Hits).Count) { return 'none' }
            return ((@($Hits) | ForEach-Object { $_.Match.Groups[1].Value + ' by ' + $_.Match.Groups[2].Value }) -join ', ')
        }

        Step "the graveyard at Rescue_10 after the load: the suits out, the extraction point not secured yet" {
            $c = Find-Log host 'continuing campaign "checking": rescue, checkpoint (\S+)'
            Require ($c -and $c.Match.Groups[1].Value -eq 'Rescue_10') ("the campaign was continued at Rescue_10, the extraction point (" + $(if ($c) { $c.Match.Groups[1].Value } else { 'no continue line' }) + ")")
            Send-Game host "coop_god 1"
            Send-Game host $defs
            Pause-Game $P.Settle
            Check $true ("after the load: " + (Get-ChkLine 'AtWho("after the load")' '^\[CHK\] players after the load: ') + "; " + (Get-ChkLine 'AtVtol("after the load")' '^\[CHK\] vtol after the load:'))
            $script:gyTok0 = Get-Tokens "after the load"
            Check $true ("game tokens after the load: " + $script:gyTok0.Text)
            # (all four come out in the same frame: two random picks of two)
            $s = $null; $firstShown = $null; $n = 0
            $until = (Get-Date).AddSeconds($P.ShowWithin)
            while ((Get-Date) -lt $until) {
                $n++
                $s = Get-Suits "wait $n"
                if ($s -and $s.Shown -ge 1 -and -not $firstShown) { $firstShown = Get-Date }
                if ($s -and $s.Shown -ge $P.MinDead) { break }
                if ($firstShown -and ((Get-Date) - $firstShown).TotalSeconds -ge 6) { break }
                Pause-Game 3
            }
            Require ($null -ne $s) "the suits' list came back from the host"
            # (the companion may have shot some while it joined)
            $pre = @(Find-Log host $killRx $ready -All)
            Check $true ("the suits before the host's kills: " + $s.Text + "; killed since the load: " + (Get-KillText $pre))
            $done = Find-Log host '^\[CoopObjective\] Rescue\.Objective_11 -> completed' $ready
            Require ((($script:gyTok0.Secured -ne 'secured') -and -not $done) -or $pre.Count -ge 1) ("the extraction point was not secured before any suit died (token " + $script:gyTok0.Secured + ", Objective_11 " + $(if ($done) { 'completed' } else { 'open' }) + ")")
            Require ($s.Shown -ge 1 -or $pre.Count -ge 1 -or $P.KillHidden) "the graveyard's suits came out after the load ($($s.Shown) shown)"
        }
        Step "the host kills the graveyard's suits one after another" {
            $script:gyHidden = $false
            for ($r = 1; $r -le 3; $r++) {
                $s = Get-Suits "kill round $r"
                if (-not $s) { break }
                $targets = @($s.List | Where-Object { $_.Hp -gt 0 -and -not $_.Hidden })
                $deadNow = @($s.List | Where-Object { $_.Hp -le 0 }).Count
                if (-not $targets.Count) {
                    if ($deadNow -ge $P.MinDead -or -not $P.KillHidden) { break }
                    $hid = @($s.List | Where-Object { $_.Hp -gt 0 -and $_.Hidden } | Sort-Object { $ix = [array]::IndexOf([int[]]$P.Prefer, [int]$_.I); if ($ix -lt 0) { 100 + $_.I } else { $ix } })
                    $targets = @($hid | Select-Object -First ($P.MinDead - $deadNow))
                    if (-not $targets.Count) { break }
                    $script:gyHidden = $true
                    Check $true ("not enough suits came out ($deadNow dead): hidden ones killed instead: " + (($targets | ForEach-Object { "NK_Suit_$($_.I)" }) -join ', '))
                }
                foreach ($t in $targets) {
                    Send-Game host ('#local e=System.GetEntityByName("NK_Suit_' + $t.I + '"); if e and e.actor and e.actor:GetHealth()>0 then g_gameRules:CreateHit(e.id,g_localActorId,g_localActorId,9000,nil,nil,nil,"bullet") end')
                    Pause-Game $P.KillGap
                }
            }
            $s = Get-Suits "after the kills"
            Require ($null -ne $s) "the suits' list after the kills came back"
            $dead = @($s.List | Where-Object { $_.Hp -le 0 })
            Check $true ("killed: " + (Get-KillText @(Find-Log host $killRx $ready -All)) + "; " + $s.Text)
            Require ($dead.Count -ge $P.MinDead) ("at least $($P.MinDead) of the graveyard's suits are dead ($($dead.Count): " + (($dead | ForEach-Object { "NK_Suit_$($_.I)" }) -join ', ') + ")")
            $script:gyWatch = New-Mark
        }
        Step "the level's answer: the suits' Dead in its flow graph, the extraction point secured, the VTOL" {
            $mW = $script:gyWatch
            Send-Game host $tick
            $secured = $null; $obj = $null
            $until = (Get-Date).AddSeconds($P.Within)
            while ((Get-Date) -lt $until) {
                Pause-Game 2
                $secured = Find-Log host '^\[CHK\] g t=(\d+) secured=secured ' $mW
                $obj = Find-Log host '^\[CoopObjective\] Rescue\.Objective_11 -> completed' $ready
                if ($secured -or $obj) { break }
            }
            # cine_3_vtolarrives 2-4 s after the token (no cutscene flag: the host
            # logs nothing of it, the companion's game is told to play it)
            $cine = $null
            if ($secured -or $obj) {
                $until = (Get-Date).AddSeconds($P.CineWithin)
                while ((Get-Date) -lt $until) {
                    Pause-Game 2
                    $cine = Find-Log companion '^\[CoopCutscene\] .*cine_3_vtolarrives' $ready
                    if (-not $cine) { $cine = Find-Log host '^\[CoopCutscene\] .*cine_3_vtolarrives' $ready }
                    if ($cine) { Pause-Game 4; break }
                }
            }
            Send-Game host '#AtG.on=false'
            Pause-Game 0.5
            $tok = Get-Tokens "at the end"
            $vt = Get-ChkLine 'AtVtol("at the end")' '^\[CHK\] vtol at the end:'
            $g = @(Find-Log host '^\[CHK\] g t=(\d+) secured=(\S+) buffer=(\S+) door=(\S+) (-?\d+),(-?\d+),(-?\d+)' $mW -All)
            $doorText = "not watched"
            if ($g.Count) {
                $g0 = $g[0].Match.Groups; $g1 = $g[-1].Match.Groups
                $moved = [Math]::Sqrt([Math]::Pow([double]$g1[5].Value - [double]$g0[5].Value, 2) + [Math]::Pow([double]$g1[6].Value - [double]$g0[6].Value, 2) + [Math]::Pow([double]$g1[7].Value - [double]$g0[7].Value, 2))
                $doorText = "{0} -> {1}, moved {2:N0} m" -f $g0[4].Value, $g1[4].Value, $moved
            }
            $deaths = @(Find-Log host "^\[Coop\] (NK_Suit_\d+)'s death reaches the level \(Dead\)" $ready -All)
            $o12 = Find-Log host '^\[CoopObjective\] Rescue\.Objective_12 -> activated' $ready
            $isSecured = ($null -ne $secured) -or ($tok.Secured -eq 'secured')
            Check $true ("(1) the suits' deaths that reached the level's flow graph (Dead): $($deaths.Count)" + $(if ($deaths.Count) { " (" + (($deaths | ForEach-Object { $_.Match.Groups[1].Value }) -join ', ') + ")" } else { "" }) + "; its counter needs $($P.MinDead): " + $(if ($deaths.Count -ge $P.MinDead) { "enough" } else { "too few" }))
            Check ($isSecured -or $null -ne $obj) ("(2) the extraction point was secured within $($P.Within) s of the kills: Level.AB_5.Graveyard_secured " + $(if ($secured) { "= secured " + $secured.Match.Groups[1].Value + " s after them" } else { "= " + $tok.Secured }) + ", Rescue.Objective_11 " + $(if ($obj) { "completed" } else { "not completed" }) + $(if ($script:gyHidden) { " (hidden suits were killed: the graph had shown too few)" } else { "" }))
            Check $true ("(3) the VTOL: cine_3_vtolarrives " + $(if ($cine) { "started (" + $cine.Text + ")" } else { "not seen" }) + "; the cutscene's VTOL cine_3_vtol_AnimDoor " + $doorText + "; Objective_12 (to the VTOL) " + $(if ($o12) { "activated" } else { "not activated" }) + "; " + $vt)
            Check $true ("the watch: Graveyard_secured " + (Get-Runs @($g | ForEach-Object { $_.Match.Groups[2].Value })) + "; Dialog_buffer " + (Get-Runs @($g | ForEach-Object { $_.Match.Groups[3].Value })) + "; at the end: " + $tok.Text)
            $sank = Count-Log host '^\[CoopAgent\] Companion sank ' $ready
            $off = Count-Log host '^\[CoopAgent\] Companion was [\d.]+ m off in its own game' $ready
            Check $true ("the companion put back up after sinking: $sank times, put back from its own game's place: $off times; " + (Get-ChkLine 'AtWho("at the end")' '^\[CHK\] players at the end: '))
        }
    }
}
