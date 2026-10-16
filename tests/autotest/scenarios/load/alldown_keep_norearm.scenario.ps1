# The user's game of 2026-10-15: everybody down -> the last checkpoint loaded
# into the running level; the AI companion was thrown out (cause=4) and could
# not join again, the hut by the bridge flew apart. Here, on a copy of that
# campaign: both die twice; after each load the companion must still be in the
# game, nothing may fly, nobody hurt by debris.
@{
    Name = "alldown-keep-norearm"
    Tags = @("diag")
    SummaryRu = "Диагностика: то же без повторной выдачи оружия после загрузки (coop_load_rearm 0) — откуда лишние вещи"
    Session = @{ Fixture = "save:checking"; Companion = $true; Cvars = @{ coop_load_rearm = 0 } }
    Params = @{ Rounds = 2 }
    Body = {
        param($P)
        $state = '#local c=System.GetEntityByName("Companion"); local n=System.GetEntityByName("Nomad"); local function f(e) if not e then return "-" end local p=e:GetWorldPos(); return string.format("%.0f %.0f %.0f hp %d", p.x, p.y, p.z, e.actor:GetHealth()) end; System.LogAlways("[CHK] TAG: companion "..f(c).." host "..f(n))'
        # entities, items, items nobody (alive) owns, and of those the hidden ones
        $census = '#local n,it,free,hid=0,0,0,0; for _,e in pairs(System.GetEntities() or {}) do n=n+1; if e.item then it=it+1; local o=e.item:GetOwnerId(); if not (o and System.GetEntity(o)) then free=free+1; if e:IsHidden() then hid=hid+1 end end end end; System.LogAlways(string.format("[CHK] census TAG: entities %d items %d unowned %d unowned_hidden %d", n, it, free, hid))'
        function Get-Census([string]$Tag) {
            $h = Find-Log host ("^\[CHK\] census " + [regex]::Escape($Tag) + ": entities (\d+) items (\d+) unowned (\d+) unowned_hidden (\d+)") -Last
            if ($h) { return @(1..4 | ForEach-Object { [int]$h.Match.Groups[$_].Value }) } else { return $null }
        }
        Pause-Game 10
        Send-Game host "coop_god 0", $state.Replace("TAG", "before"), $census.Replace("TAG", "round 0")
        for ($r = 1; $r -le $P.Rounds; $r++) {
            Step "everybody down #$r" {
                $m = New-Mark
                Send-Game host "coop_test_kill Companion", "coop_test_kill Nomad"
                $down = Wait-Log host 'everybody is down: the last checkpoint is loaded' 60 $m
                Check ($null -ne $down) "round $r`: everybody down -> the checkpoint is loaded"
                $end = Wait-Log host "^\[Coop\] checkpoint loaded: \d+ players' places kept" 300 $m
                Check ($null -ne $end) "round $r`: the load ended"
                Check ($null -ne (Find-Log host 'runs already: the checkpoint is loaded into it' $m)) "round $r`: loaded into the running level"
                $kept = Find-Log host 'friend\(s\) stay in the game through the load' $m
                Check ($null -ne $kept) ("round $r`: the companion is kept through the load" + $(if ($kept) { " ($($kept.Text))" } else { "" }))
                Pause-Game 25
                Send-Game host $state.Replace("TAG", "round $r, 25 s after the load"), $census.Replace("TAG", "round $r")
                Pause-Game 2
                $a = Get-Census "round $($r - 1)"; $b = Get-Census "round $r"
                if ($a -and $b) {
                    Check (($b[0] - $a[0]) -lt 100) ("round $r`: entities {0} -> {1}, items {2} -> {3}, unowned items {4} -> {5} (hidden {6} -> {7})" -f $a[0], $b[0], $a[1], $b[1], $a[2], $b[2], $a[3], $b[3])
                }
                Check ((Count-Log host '^Disconnect |out of the host' $m) -eq 0) "round $r`: nobody was disconnected"
                $chk = Find-Log host ("^\[CHK\] round $r, 25 s") $m
                Check ($null -ne $chk -and $chk.Text -notmatch 'companion -') "round $r`: the companion is there 25 s after the load ($(if ($chk) { $chk.Text } else { 'no state line' }))"
                $things = Find-Log host '^\[CoopSave\] things after the load: (.*)' $m
                if ($things) { Check $true "round $r`: things $($things.Match.Groups[1].Value)" }
                Check ((Count-Log host '^\[CoopBug\]' $m) -eq 0) "round $r`: nothing flew after the load"
                Check ((Count-Log host 'DAMAGE player .* type=collision hit=([1-9]\d\d|\d{4,})' $m) -eq 0) "round $r`: no player hurt by debris"
            }
        }
    }
}
