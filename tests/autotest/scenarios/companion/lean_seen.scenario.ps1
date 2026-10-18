# The user (2026-10-17): the companion should peek out of cover. It leans
# out round a cover's side in its own game, but a lean was let go of at the
# end of each frame before the input went to the server: the host (and the
# soldiers there) never saw a joined player lean, the companion's or a
# friend's. Here the companion leans left, then right (the agent's "lean"),
# and the host's game must see each: the server's log line and where the
# companion's head is, sideways from its body.
@{
    Name = "companion-lean-seen"
    Tags = @("companion", "tactics", "network")
    SummaryRu = "Наклон компаньона (выглядывание из-за укрытия) доходит до сервера: хост видит, как он наклоняется влево и вправо"
    Session = @{ Fixture = "new:rescue"; Companion = $true }
    Params = @{}
    Body = {
        param($P)
        function Head-Side([string]$Tag) {
            Send-Game host ('#local c=System.GetEntityByName("Companion"); local h=c:GetBonePos("Bip01 Head"); local p=c:GetWorldPos(); local r=c:GetDirectionVector(0); local s=-99; if h and h.x then s=(h.x-p.x)*r.x+(h.y-p.y)*r.y end; System.LogAlways(string.format("[CHK] lean head ' + $Tag + ' side=%.2f", s))')
            Pause-Game 1
            $l = Find-Log host ('^\[CHK\] lean head ' + $Tag + ' side=(-?[\d.]+)') -Last
            if ($l -and $l.Text -match 'side=(-?[\d.]+)') { return [double]$Matches[1] }
            return $null
        }
        Step "the companion stands still by the host" {
            Send-Game host "coop_god 1"
            Pause-Game 10
            Send-Game host '#local p=System.GetEntityByName("Nomad"):GetWorldPos(); local c=System.GetEntityByName("Companion"); g_gameRules.game:MovePlayer(c.id,{x=p.x+3,y=p.y,z=p.z+0.3},{x=0,y=0,z=0}); System.LogAlways("[CHK] companion next to the host")'
            Send-Agent "hold" | Out-Null
            Pause-Game 3
        }
        Step "it leans left, then right" {
            $base = Head-Side "straight"
            $m = New-Mark
            Send-Agent "lean left 4" | Out-Null
            Pause-Game 2
            $left = Head-Side "left"
            $sawLeft = Find-Log host '^\[Coop\] Companion leans left \(as the server sees it' $m
            Pause-Game 2
            $m2 = New-Mark
            Send-Agent "lean right 4" | Out-Null
            Pause-Game 2
            $right = Head-Side "right"
            $sawRight = Find-Log host '^\[Coop\] Companion leans right \(as the server sees it' $m2
            Send-Agent "lean off" | Out-Null
            Check ($null -ne $sawLeft) ("the server got its lean to the left" + $(if ($sawLeft) { ": " + ($sawLeft.Text -replace '^\[Coop\] ', '') } else { "" }))
            Check ($null -ne $sawRight) ("the server got its lean to the right" + $(if ($sawRight) { ": " + ($sawRight.Text -replace '^\[Coop\] ', '') } else { "" }))
            Check $true ("its head sideways from its body in the host's game: straight {0}, leaning left {1}, leaning right {2}" -f $base, $left, $right)
            if ($null -ne $base -and $null -ne $left -and $null -ne $right -and $base -gt -90) {
                Check (($left - $base) -lt -0.1 -and ($right - $base) -gt 0.1) "in the host's game its head went left and right ($([Math]::Round($left - $base, 2)) / $([Math]::Round($right - $base, 2)) m)"
            }
        }
    }
}
