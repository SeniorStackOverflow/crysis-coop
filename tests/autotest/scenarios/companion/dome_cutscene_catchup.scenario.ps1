# The user's game of 2026-10-22 (fixture dome1022): the research dome's
# cutscene put the host outside the dome and left the companion inside behind
# the wall. Right after a cutscene it is put next to the host when he is far
# or something is between them (as the cut in single player), not left to find
# its way for 20 s. Here: the same two places, then a short cutscene
# (black_bars, 5 s, played by the host and so by its game too); within 8 s of
# its end it must be by the host, by the cutscene rule.
@{
    Name = "companion-dome-cutscene-catchup"
    Tags = @("companion", "navigation", "cutscene")
    SummaryRu = "После катсцены компаньон, оставшийся за стеной купола, сразу оказывается рядом с хостом"
    Session = @{ Fixture = "save:dome1022"; Campaign = "56c79107e0d7a6ff"; Companion = $true; ReadyCommands = @("coop_god 1") }
    Params = @{ Host = @(1208.0, 2598.9, 243.6); Inside = @(1206.6, 2592.6, 244.5); Sequence = "black_bars" }
    Body = {
        param($P)
        Step "the companion inside behind the wall, the host outside; a cutscene" {
            Send-Game host "coop_god 1"
            Pause-Game 8
            Send-Agent "free" | Out-Null
            $h = $P.Host; $c = $P.Inside
            Send-Game host ("#g_gameRules.game:MovePlayer(g_localActorId,{x=$($h[0]),y=$($h[1]),z=$($h[2])},{x=0,y=0,z=0}); local c=System.GetEntityByName(""Companion""); g_gameRules.game:MovePlayer(c.id,{x=$($c[0]),y=$($c[1]),z=$($c[2])},{x=0,y=0,z=0}); System.LogAlways(""[CHK] placed"")")
            Pause-Game 2
            Start-Watch @() $h
            $script:m0 = New-Mark
            Send-Game host ('#Movie.PlaySequence("' + $P.Sequence + '"); System.LogAlways("[CHK] cutscene played")')
            $seen = Wait-Log companion ('^\[CoopCutscene\] host plays ' + $P.Sequence) 20 $script:m0
            Require ($null -ne $seen) "the companion's game plays the cutscene too"
            $end = Wait-Log host ('^\[CoopCutscene\] ' + $P.Sequence + ' ended') 30 $script:m0
            Require ($null -ne $end) "the cutscene ended: $(if ($end) { $end.Text })"
            $script:mEnd = New-Mark
        }
        Step "within 8 s of its end it is by the host" {
            Pause-Game 8
            Stop-Watch
            $w = @(Get-Watch $script:mEnd)
            $rule = Find-Log companion '^\[CoopAgent\] the cutscene is over, Nomad .*: put next to him' $script:m0
            $late = Find-Log companion 'with something between for \d+ s, no nearer: put next to him' $script:m0
            $last = if ($w.Count) { $w[-1] } else { $null }
            Check $true ("rule: " + $(if ($rule) { $rule.Text } else { "no cutscene line" }) + $(if ($late) { "; then: " + $late.Text } else { "" }))
            Check ($null -ne $rule) "put next to the host by the cutscene rule"
            Check ($null -ne $last -and $last.Dh -le 5.0) ("by the host 8 s after the cutscene ({0:N1} m)" -f $(if ($last) { $last.Dh } else { -1 }))
        }
    }
}
