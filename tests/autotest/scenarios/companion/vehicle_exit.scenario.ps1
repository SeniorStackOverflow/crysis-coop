# The user's game of 2026-10-15: the AI companion got out of the car next to
# the host and was put 200 m back, where it had got in. Here: both drive, the
# host stops and gets out, the companion follows: it must come out by the car.
# Then the host jumps out of the car at speed: he must stay where he jumped.
@{
    Name = "vehicle-exit-in-place"
    Tags = @("companion", "vehicle")
    SummaryRu = "Выход из машины: компаньон выходит у машины, хост выпрыгивает на ходу и остаётся там, где выпрыгнул"
    Session = @{ Fixture = "new:rescue"; Companion = $true }
    Params = @{ Vehicle = "Land.Chinese_4wd1"; Drive = 8; JumpSpeed = 25 }
    Body = {
        param($P)
        $v = $P.Vehicle
        $where = '#local v=System.GetEntityByName("' + $v + '"); local c=System.GetEntityByName("Companion"); local n=System.GetEntityByName("Nomad"); local function f(e) if not e then return "-" end local p=e:GetWorldPos(); return string.format("%.0f %.0f %.0f", p.x, p.y, p.z) end; System.LogAlways("[CHK] TAG: car "..f(v).." companion "..f(c).." host "..f(n))'
        function Get-Pos([string]$Tag, [string]$Who) {
            $h = Find-Log host ("^\[CHK\] " + [regex]::Escape($Tag) + ": car ") -Last
            if (-not $h) { return $null }
            $m = [regex]::Match($h.Text, "$Who (-?\d+) (-?\d+) (-?\d+)")
            if (-not $m.Success) { return $null }
            return @([double]$m.Groups[1].Value, [double]$m.Groups[2].Value)
        }
        function Get-Dist($a, $b) { if (-not $a -or -not $b) { return 99999 }; return [Math]::Sqrt([Math]::Pow($a[0] - $b[0], 2) + [Math]::Pow($a[1] - $b[1], 2)) }
        Step "both in the car" {
            Send-Game host "coop_god 1"
            Send-Game host ('#local v=System.GetEntityByName("' + $v + '"); local p=v:GetWorldPos(); g_gameRules.game:MovePlayer(g_localActorId, {x=p.x+4,y=p.y,z=p.z+2}, {x=0,y=0,z=0}); System.LogAlways("[CHK] host by the car")')
            Pause-Game 3
            $m = New-Mark
            Send-Game host "coop_test_enter $v 1"
            Require ($null -ne (Wait-Log host 'Companion gets into|Companion entered vehicle' 120 $m)) "the companion got into the car"
            Pause-Game 2
        }
        Step "drive, stop, get out" {
            Send-Game host "coop_vehtest $($P.Drive)"
            Pause-Game ($P.Drive + 4)
            Send-Game host $where.Replace("TAG", "stopped")
            $m = New-Mark
            Send-Game host "coop_test_enter out"
            $out = Wait-Log host 'Companion left vehicle' 60 $m
            Check ($null -ne $out) "the companion got out after the host"
            Pause-Game 4
            Send-Game host $where.Replace("TAG", "4 s after")
            $car = Get-Pos "stopped" "car"; $c = Get-Pos "4 s after" "companion"
            $d = Get-Dist $car $c
            Check ($d -lt 25) ("the companion came out by the car: {0:N0} m from it" -f $d)
        }
        Step "the host jumps out at speed" {
            Send-Game host "coop_test_enter $v 1"
            Pause-Game 3
            Send-Game host $where.Replace("TAG", "before the jump")
            Send-Game host "coop_test_enter out $($P.JumpSpeed)"
            Pause-Game 1
            Send-Game host $where.Replace("TAG", "1 s after the jump")
            Pause-Game 4
            Send-Game host $where.Replace("TAG", "5 s after the jump")
            $a = Get-Pos "1 s after the jump" "host"; $b = Get-Pos "5 s after the jump" "host"
            $d = Get-Dist $a $b
            Check ($d -lt 40) ("the host stayed where he jumped: moved {0:N0} m in 4 s" -f $d)
            Check ((Count-Log host 'put back|came out of .* put') -eq 0) "nobody was put back to where they got in"
        }
    }
}
