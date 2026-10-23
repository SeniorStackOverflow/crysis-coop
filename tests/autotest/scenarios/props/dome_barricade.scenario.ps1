# The user's game of 2026-10-22 (fixture dome1022, Rescue_08): "I cannot take
# ordinary things in my hands - I have to clear a blockage here" by the
# research dome (the host at 1220, 2578). Every loose thing within 7 m that
# coop carrying allows is tried: the host stands before it, looks at it,
# presses use; picked up, it is put down again. Most must come into his hands.
@{
    Name = "props-dome-barricade"
    Tags = @("props", "user")
    SummaryRu = "Завал у купола из игры пользователя (Rescue_08): вещи рядом берутся в руки (хост подходит к каждой, нажимает «использовать»)"
    Session = @{ Fixture = "save:dome1022"; Campaign = "56c79107e0d7a6ff"; Companion = $true; ReadyCommands = @("coop_god 1") }
    Params = @{ At = @(1220.0, 2578.0, 244.2); Radius = 7; Max = 10 }
    Body = {
        param($P)
        Step "the loose things by the blockage" {
            Send-Game host "coop_god 1"
            Pause-Game 8
            $a = $P.At
            Send-Game host ("#g_gameRules.game:MovePlayer(g_localActorId,{x=$($a[0]),y=$($a[1]),z=$($a[2])},{x=0,y=0,z=0})")
            Pause-Game 2
            $m = New-Mark
            Send-Game host ('#local n=0; for _,e in pairs(System.GetEntitiesInSphere({x=' + $a[0] + ',y=' + $a[1] + ',z=' + $a[2] + '},' + $P.Radius + ') or {}) do local c=e.class; if (c=="BasicEntity" or c=="RigidBodyEx") and not e:IsHidden() then local p=e:GetWorldPos(); n=n+1; System.LogAlways(string.format("[CHK] loose %s|%.2f|%.2f|%.2f|%.1f", e:GetName(), p.x, p.y, p.z, (e.GetMass and e:GetMass()) or 0)) end end; System.LogAlways("[CHK] loose count "..n)')
            $cnt = Wait-Log host '^\[CHK\] loose count (\d+)' 10 $m
            $script:things = @(Find-Log host '^\[CHK\] loose (.+)\|(-?[\d.]+)\|(-?[\d.]+)\|(-?[\d.]+)\|([\d.]+)' $m -All | ForEach-Object {
                $g = $_.Match.Groups; [pscustomobject]@{ Name = $g[1].Value; X = [double]$g[2].Value; Y = [double]$g[3].Value; Z = [double]$g[4].Value; Mass = [double]$g[5].Value } })
            Require ($script:things.Count -ge 1) "loose things within $($P.Radius) m: $(if ($cnt) { $cnt.Match.Groups[1].Value } else { 0 })"
            Check $true ("found: " + (($script:things | Select-Object -First 15 | ForEach-Object { "{0} ({1:N0} kg)" -f $_.Name, $_.Mass }) -join ", "))
        }
        Step "each one tried by hand" {
            $tried = 0; $taken = 0; $refused = @()
            foreach ($t in ($script:things | Sort-Object Mass | Select-Object -First $P.Max)) {
                $tried++
                $picked = $false
                foreach ($sd in @(@(0, -1), @(1, 0), @(0, 1), @(-1, 0))) {
                    $px = $t.X - $sd[0] * 1.2; $py = $t.Y - $sd[1] * 1.2
                    $yaw = [Math]::Atan2(-$sd[0], $sd[1])
                    $pitch = [Math]::Atan2(($t.Z + 0.3) - ($t.Z + 1.6), 1.2)
                    Send-Game host ("#local p={x=$px,y=$py,z=$($t.Z + 0.2)}; g_gameRules.game:MovePlayer(g_localActorId,p,{x=$pitch,y=0,z=$yaw})")
                    Pause-Game 0.8
                    Send-Game host ("#g_localActor:SetAngles({x=$pitch,y=0,z=$yaw})")
                    Pause-Game 0.6
                    $m = New-Mark
                    Send-Game host "coop_test_key use 1"
                    Pause-Game 0.2
                    Send-Game host "coop_test_key use 0"
                    $ans = Wait-Log host '^\[CoopProp\] Nomad.s left hand: (.*)' 4 $m
                    if ($ans -and $ans.Match.Groups[1].Value -like 'a thing*') {
                        $up = Wait-Log host '^\[CoopProp\] Nomad picks up (.+?) \(' 4 $m
                        if ($up) {
                            $picked = $true
                            Check $true ("{0}: picked up ({1})" -f $t.Name, $up.Match.Groups[1].Value)
                            Pause-Game 1.2
                            Send-Game host "coop_test_key zoom 1"
                            Pause-Game 0.2
                            Send-Game host "coop_test_key zoom 0"
                            Pause-Game 1.5
                            break
                        }
                    }
                    elseif ($ans -and $sd -eq $null) { }
                    if ($ans -and $ans.Match.Groups[1].Value -like 'a weapon*') { Pause-Game 1 }
                }
                if ($picked) { $taken++ } else { $refused += $t.Name }
            }
            Check $true ("could not take: " + $(if ($refused.Count) { $refused -join ", " } else { "none" }))
            Check ($taken -ge [Math]::Ceiling($tried / 2)) "most of the loose things came into his hands ($taken of $tried)"
        }
    }
}
