# The user's game of 2026-10-22 (fixture dome1022: checkpoint Rescue_08, saved
# right after the research dome's cutscene): the cutscene put the host outside
# the dome, the companion stayed inside behind the wall by the door. The host
# stood 5-7 m off at the open door; the companion's own squares found no way
# out ("blocked round it: 3 at the knees"), the host's navigation none indoors,
# and its catch-ups wanted the host > 8 m off: it went to and fro at the wall
# for minutes. Here the same two places; it must get to the host (walk out or
# be put next to him) within 40 s, and not dither there.
@{
    Name = "companion-dome-door-stuck"
    Tags = @("companion", "navigation")
    SummaryRu = "Компаньон за стеной у двери купола (Rescue_08 после катсцены): не топчется на месте, а выходит к хосту или его ставят рядом"
    Session = @{ Fixture = "save:dome1022"; Campaign = "56c79107e0d7a6ff"; Companion = $true; ReadyCommands = @("coop_god 1") }
    Params = @{ Host = @(1208.0, 2598.9, 243.6); Inside = @(1206.6, 2592.6, 244.5); Within = 40 }
    Body = {
        param($P)
        Step "the companion inside behind the wall, the host outside by the open door" {
            Send-Game host "coop_god 1"
            Pause-Game 8
            Send-Agent "free" | Out-Null
            $h = $P.Host; $c = $P.Inside
            Send-Game host ("#g_gameRules.game:MovePlayer(g_localActorId,{x=$($h[0]),y=$($h[1]),z=$($h[2])},{x=0,y=0,z=0}); local c=System.GetEntityByName(""Companion""); g_gameRules.game:MovePlayer(c.id,{x=$($c[0]),y=$($c[1]),z=$($c[2])},{x=0,y=0,z=0}); System.LogAlways(""[CHK] placed"")")
            Pause-Game 3
            Start-Watch @() $h
            $script:m0 = New-Mark
        }
        Step "it gets to the host within $($P.Within) s" {
            $m = $script:m0
            Pause-Game $P.Within
            Stop-Watch
            $w = @(Get-Watch $m)
            Require ($w.Count -ge 10) "the watch ran ($($w.Count) samples)"
            $start = $w[0]
            $near = @($w | Where-Object { $_.Dh -le 4.5 } | Select-Object -First 1)
            $t = if ($near.Count) { $near[0].T - $start.T } else { -1 }
            $last = $w[-1]
            $put = Find-Log host '^\[CoopAgent\] Companion caught up with Nomad' $m
            $why = Find-Log companion 'put next to him' $m
            $way = Find-Log companion 'a way round to Nomad close by: [1-9]' $m
            $moved = ($w | Measure-Object -Property Mv -Sum).Sum
            Check $true ("start {0:N1} m from the host (inside: {1:N1} {2:N1}); at the end {3:N1} m; {4}; walked {5:N1} m in all" -f $start.Dh, $start.X, $start.Y, $last.Dh,
                $(if ($t -ge 0) { "within 4.5 m after {0:N1} s" -f $t } else { "never within 4.5 m" }), $moved)
            Check $true ("how: " + $(if ($why) { $why.Text } elseif ($put) { $put.Text } else { "no catch-up line" }))
            Check ($t -ge 0) "it got to the host (within 4.5 m) in $($P.Within) s"
            Check ($last.Dh -le 5.0) "it is by the host at the end ($('{0:N1}' -f $last.Dh) m)"
        }
    }
}
