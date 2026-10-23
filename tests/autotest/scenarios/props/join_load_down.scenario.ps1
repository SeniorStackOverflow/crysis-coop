# Carrying things (CoopProps), the hard cases in one game: a thing the host
# threw must be where it lies for a game that joins later; after everybody is
# down and the checkpoint is loaded into the running level, the kept
# companion's game must have it where the load put it; a companion that goes
# down with it in its hands lets go of it in every game, and it rests at the
# same spot everywhere.
@{
    Name = "props-join-load-down"
    Tags = @("props", "sync", "load", "companion")
    SummaryRu = "Брошенная вещь на своём месте у зашедшего позже компаньона, после загрузки чекпоинта и когда компаньон падает с ней в руках"
    Session = @{ Fixture = "new:rescue"; Companion = $true }
    Params = @{ Prop = "Storage.rusty_metal_barrel_b35"; Mid = 0.45 }
    Body = {
        param($P)
        $n = $P.Prop
        $where = '#local e=System.GetEntityByName("' + $n + '"); if e then local p=e:GetWorldPos(); System.LogAlways(string.format("[CHK] thing TAG at %.2f %.2f %.2f", p.x, p.y, p.z)) else System.LogAlways("[CHK] thing TAG none") end'
        function Dist($a, $b) { [Math]::Sqrt(($a.X - $b.X) * ($a.X - $b.X) + ($a.Y - $b.Y) * ($a.Y - $b.Y) + ($a.Z - $b.Z) * ($a.Z - $b.Z)) }
        function Get-Where([string]$tag) {
            $m = New-Mark
            Send-Game host $where.Replace("TAG", $tag)
            Send-Game companion $where.Replace("TAG", $tag)
            $rx = '^\[CHK\] thing ' + [regex]::Escape($tag) + ' at (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)'
            $h = Wait-Log host $rx 10 $m
            $c = Wait-Log companion $rx 10 $m
            $hp = if ($h) { [pscustomobject]@{ X = [double]$h.Match.Groups[1].Value; Y = [double]$h.Match.Groups[2].Value; Z = [double]$h.Match.Groups[3].Value } } else { $null }
            $cp = if ($c) { [pscustomobject]@{ X = [double]$c.Match.Groups[1].Value; Y = [double]$c.Match.Groups[2].Value; Z = [double]$c.Match.Groups[3].Value } } else { $null }
            return @($hp, $cp)
        }
        Step "the host throws the barrel" {
            Send-Game host "coop_god 1"
            Pause-Game 15
            $w = Get-Where "start"
            Require ($w[0] -and $w[1]) "the barrel is in both games"
            $hp = $w[0]
            $script:base = $hp
            $script:side = $null
            foreach ($sd in @(@(0, -1), @(0, 1), @(1, 0), @(-1, 0))) {
                $px = $hp.X - $sd[0] * 1.3; $py = $hp.Y - $sd[1] * 1.3
                $yaw = [Math]::Atan2(-$sd[0], $sd[1])
                $pitch = [Math]::Atan2($P.Mid - 1.6, 1.3)
                Send-Game host ("#local p={x=$px,y=$py,z=0}; p.z=System.GetTerrainElevation(p)+0.1; g_gameRules.game:MovePlayer(g_localActorId,p,{x=$pitch,y=0,z=$yaw})")
                Pause-Game 1
                Send-Game host ("#g_localActor:SetAngles({x=$pitch,y=0,z=$yaw})")
                Pause-Game 1
                $m = New-Mark
                Send-Game host "coop_test_key use 1"
                Pause-Game 0.2
                Send-Game host "coop_test_key use 0"
                $ans = Wait-Log host '^\[CoopProp\] Nomad.s left hand: (.*)' 5 $m
                if ($ans -and $ans.Match.Groups[1].Value -like 'a thing*') { $script:side = $sd; break }
            }
            Require ($null -ne $script:side) "the host picked it up"
            Pause-Game 2
            $sd = $script:side
            $back = [Math]::Atan2($sd[0], -$sd[1])
            Send-Game host ("#g_localActor:SetAngles({x=0.45,y=0,z=$back})")
            Pause-Game 1
            $m = New-Mark
            Send-Game host "coop_test_key use 1"
            Pause-Game 0.15
            Send-Game host "coop_test_key use 0"
            $rest = Wait-Log host ('^\[CoopProp\] ' + [regex]::Escape($n) + ' \(\d+\) rests at') 20 $m
            Require ($null -ne $rest) "it was thrown and came to rest: $(if ($rest) { $rest.Text })"
            Pause-Game 2
            $w = Get-Where "thrown"
            Check ((Dist $w[0] $script:base) -gt 3.0) ("it lies {0:N1} m from where it was" -f (Dist $w[0] $script:base))
            Check ((Dist $w[0] $w[1]) -lt 0.3) ("in both games at the same spot ({0:N2} m apart)" -f (Dist $w[0] $w[1]))
            $script:thrown = $w[0]
        }
        Step "the companion leaves and joins again: the barrel where it lies" {
            $m = New-Mark
            Send-Game host "coop_companion 0"
            Pause-Game 8
            Send-Game host "coop_companion 1"
            $in = Wait-Companion 300 $m
            Require ($null -ne $in) "the companion joined again"
            Pause-Game 12
            $told = Find-Log host '^\[CoopProp\] a joining game is told of (\d+) moved things' $m
            Check $true ("the host: " + $(if ($told) { $told.Text } else { "no history line" }))
            $w = Get-Where "joined"
            Require ($w[0] -and $w[1]) "the barrel in both games after the join"
            Check ((Dist $w[1] $script:thrown) -lt 0.3) ("the joined game has it where it was thrown to ({0:N2} m off; {1:N1} m from the level's place)" -f (Dist $w[1] $script:thrown), (Dist $w[1] $script:base))
        }
        Step "everybody down: the checkpoint loaded, the barrel the same in both games" {
            $m = New-Mark
            Send-Game host "coop_god 0"
            Send-Game host "coop_test_kill Companion", "coop_test_kill Nomad"
            $end = Wait-Log host "^\[Coop\] checkpoint loaded: \d+ players' places kept" 300 $m
            Require ($null -ne $end) "the checkpoint was loaded"
            Send-Game host "coop_god 1"
            Pause-Game 15
            $sent = Find-Log host '^\[CoopProp\] after the load: (\d+) moved things' $m
            Check $true ("the host: " + $(if ($sent) { $sent.Text } else { "no line" }))
            $w = Get-Where "loaded"
            Require ($w[0] -and $w[1]) "the barrel in both games after the load"
            Check ((Dist $w[0] $w[1]) -lt 0.3) ("in both games at the same spot after the load ({0:N2} m apart; the host's {1:N1} m from the level's place)" -f (Dist $w[0] $w[1]), (Dist $w[0] $script:base))
            $script:now = $w[0]
        }
        Step "the companion goes down with it in its hands" {
            $hp = $script:now
            Send-Game host ("#local c=System.GetEntityByName(""Companion""); g_gameRules.game:MovePlayer(c.id,{x=$($hp.X + 2),y=$($hp.Y + 2),z=$($hp.Z + 0.5)},{x=0,y=0,z=0})")
            Pause-Game 3
            $m = New-Mark
            $r = Send-Agent ("grab " + $n)
            Require ($r.ok) ("the order taken: " + $(if ($r.ok) { $r.result } else { $r.error }))
            $up = Wait-Log host ('^\[CoopProp\] Companion picks up ' + [regex]::Escape($n)) 30 $m
            Require ($null -ne $up) "it picked it up: $(if ($up) { $up.Text } else { 'no line' })"
            Pause-Game 3
            $m = New-Mark
            Send-Game host "coop_god 0"
            Send-Game host "coop_test_kill Companion"
            $let = Wait-Log host ('^\[CoopProp\] Companion lets go of ' + [regex]::Escape($n) + '.*he is down') 10 $m
            Check ($null -ne $let) "the server took it out of its hands: $(if ($let) { $let.Text } else { 'no line' })"
            Send-Game host "coop_god 1"
            $rest = Wait-Log host ('^\[CoopProp\] ' + [regex]::Escape($n) + ' \(\d+\) rests at') 20 $m
            Check ($null -ne $rest) "it came to rest: $(if ($rest) { $rest.Text })"
            Pause-Game 3
            $w = Get-Where "dropped"
            Require ($w[0] -and $w[1]) "the barrel in both games"
            Check ((Dist $w[0] $w[1]) -lt 0.3) ("in both games at the same spot ({0:N2} m apart)" -f (Dist $w[0] $w[1]))
            $held = Find-Log companion ('^\[CoopAgent\] (holds|no longer holds) ' + [regex]::Escape($n)) $m -Last
            Check $true ("the companion: " + $(if ($held) { $held.Text } else { "no line" }))
        }
    }
}
