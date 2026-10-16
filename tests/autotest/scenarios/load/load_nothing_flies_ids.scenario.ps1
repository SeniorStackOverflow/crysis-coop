# The user's game of 2026-10-15: after a load the house he was in went to
# pieces, its foundation "flew into the stratosphere". Here, on a copy of that
# campaign without the companion: Continue, then three in-place loads in a
# row; after each, nothing may fly, no leftovers may pile up. This one with
# coop_load_ids 1, the faithful ids load 0.11.34 and 0.11.35 made by default.
@{
    Name = "load-nothing-flies-ids"
    Tags = @("load", "physics", "ab")
    # the old mode's bug, kept as proof: the house by the bridge flies apart
    KnownBug = "faithful-ids-load-blows-up-the-house"
    SummaryRu = "То же с coop_load_ids 1 (как было в 0.11.34-0.11.35): разлетается ли дом при «точной» загрузке"
    Session = @{ Fixture = "save:checking"; Companion = $false; Cvars = @{ coop_load_ids = 1 } }
    Params = @{ Loads = 3 }
    Body = {
        param($P)
        Step "the faithful ids load is on" {
            $m = New-Mark
            Send-Game host "coop_load_ids"
            Require ($null -ne (Find-Log host 'coop_load_ids = (\$\d)?1\b' $m)) "coop_load_ids is 1"
        }
        Step "Continue" {
            Check ((Count-Log host '^\[CoopBug\]') -eq 0) "Continue: nothing flew"
        }
        # how many entities there are (weapons and other things apart): a load
        # that leaves what it replaced behind makes more each time
        $count = '#local n,w=0,0; for _,e in pairs(System.GetEntities() or {}) do n=n+1; if e.weapon or e.item then w=w+1 end end; System.LogAlways("[CHK] TAG: entities "..n.." items "..w)'
        function Get-Count([string]$Tag) {
            $h = Find-Log host ("^\[CHK\] " + [regex]::Escape($Tag) + ": entities (\d+) items (\d+)") -Last
            if ($h) { return @([int]$h.Match.Groups[1].Value, [int]$h.Match.Groups[2].Value) } else { return $null }
        }
        Send-Game host $count.Replace("TAG", "load 0")
        Pause-Game 1
        for ($i = 1; $i -le $P.Loads; $i++) {
            Step "in-place load $i" {
                Send-Game host "coop_god 1"
                $m = New-Mark
                Send-Game host "coop_load"
                $end = Wait-Log host "^\[Coop\] checkpoint loaded: |^\[CoopSave\] progress loaded: " 300 $m
                Check ($null -ne $end) "load $i ended"
                Pause-Game 15
                Send-Game host $count.Replace("TAG", "load $i")
                Pause-Game 1
                $a = Get-Count "load $($i - 1)"; $b = Get-Count "load $i"
                if ($a -and $b) {
                    Check (($b[0] - $a[0]) -lt 100) ("load $i`: entities {0} -> {1}, items {2} -> {3}" -f $a[0], $b[0], $a[1], $b[1])
                }
                Check ((Count-Log host '^\[CoopBug\]' $m) -eq 0) "load $i`: nothing flew"
                $bound = Count-Log host '\d+ stays: bound' $m
                Check ($bound -eq 0) "load $i`: no leftovers stayed ($bound 'stays: bound')"
                $slow = Find-Log host 'the 10 s after the load are over: (\d+) things were slowed down' $m
                if ($slow) { Check ([int]$slow.Match.Groups[1].Value -eq 0) "load $i`: $($slow.Text)" }
            }
        }
    }
}
