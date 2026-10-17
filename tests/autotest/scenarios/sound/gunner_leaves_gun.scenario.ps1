# The user's game of 2026-10-17: the bridge reinforcements' truck came, its
# gunner (Bridge_Reinforcement_2) shot at the host with the truck's gun, got
# out and fought on with his rifle; a sound of gunfire went on and on, for
# ever. Here the same: the host goes to the bridge (the level sends the truck),
# the gunner shoots at him, then gets out (by himself, or made to after 60 s);
# no fire sound may loop on afterwards.
@{
    Name = "gunner-leaves-gun-sound"
    Tags = @("sound", "vehicle", "combat")
    SummaryRu = "Стрелок вражеского грузовика у моста стреляет из пулемёта и вылезает посреди очереди: звук стрельбы не зацикливается"
    Session = @{ Fixture = "new:rescue"; Companion = $false; Sound = "wav" }
    Params = @{ Truck = "Bridge_reinforcement_Truck_1"; Gunner = "Bridge_Reinforcement_2" }
    Body = {
        param($P)
        $t = $P.Truck; $g = $P.Gunner
        function Get-Loops([string]$Tag) {
            # FMOD's own channels (a shot's sound is no entity's): the ones
            # that loop for ever, not the level's ambience nor an engine
            $m = New-Mark
            Send-Game host "coop_test_sound channels"
            Pause-Game 2
            $all = @(Find-Log host '^\[CoopTest\] channel \d+ ' $m -All)
            $loops = @($all | Where-Object { $_.Text -match '(?i)\] channel \d+ \S*(fire_loop|_loop_|loop_close|_cu_loop|_fp_loop|3rd_loop|rapid)' })
            Check $true ("$Tag`: FMOD channels " + $all.Count + "; gunfire loops: " + $(if ($loops.Count) { (($loops | Select-Object -First 6 | ForEach-Object { ($_.Text -replace '^\[CoopTest\] ', '') }) -join "; ") } else { "none" }))
            return $loops
        }
        Step "the host by the bridge: the truck comes" {
            Send-Game host "coop_god 1"
            Pause-Game 40
            $m = New-Mark
            Send-Game host '#local p={x=1737,y=1960,z=0}; p.z=System.GetTerrainElevation(p)+1; g_gameRules.game:MovePlayer(g_localActorId,p,{x=0,y=0,z=0}); System.LogAlways("[CHK] the host by the bridge")'
            $in = Wait-Log host ("\] " + $g + " entered vehicle " + $t + " seat gunner") 90 $m
            Require ($null -ne $in) "the truck's gunner $g got to its gun"
        }
        Step "the gunner shoots at the host, then gets out" {
            $m = New-Mark
            $fire = Wait-Log host ("HIT (player Nomad|vehicle \S+) by " + $g + " weapon=VehicleShiTenV2") 120 $m
            Require ($null -ne $fire) "the gunner shoots the truck's gun"
            # (while he fires: what plays, so the list is known to see shots)
            $mf = New-Mark
            Send-Game host "coop_test_sound channels"
            Pause-Game 1
            $firing = @(Find-Log host '^\[CoopTest\] channel \d+ ' $mf -All)
            Check $true ("while he fires: " + $firing.Count + " FMOD channels: " + (($firing | Select-Object -First 12 | ForEach-Object { ($_.Text -replace '^\[CoopTest\] channel \d+ ', '') -replace ':.*loops', ' loops' }) -join "; "))
            $m2 = New-Mark
            $out = Wait-Log host ("\] " + $g + " left vehicle") 60 $m2
            if (-not $out) {
                Send-Game host ('#local v=System.GetEntityByName("' + $t + '"); local e=System.GetEntityByName("' + $g + '"); if v and e then v.vehicle:ExitVehicle(e.id); System.LogAlways("[CHK] the gunner made to get out") end')
                $out = Wait-Log host ("\] " + $g + " left vehicle") 20 $m2
            }
            Check ($null -ne $out) "the gunner got out of the truck"
            $last = Find-Log host ("by " + $g + " weapon=VehicleShiTenV2") $m -Last
            Check $true "his last shot from the truck's gun: $(if ($last) { $last.Text.Substring(0, [Math]::Min(120, $last.Text.Length)) } else { '-' })"
            Pause-Game 8
            $loops = @(Get-Loops "8 s after he got out")
            Check $true "8 s after he got out: $($loops.Count) gunfire loops (the others may still shoot)"
            Pause-Game 20
            $loops = @(Get-Loops "28 s after he got out")
            Check ($loops.Count -eq 0) "28 s after he got out: no fire sound loops on ($($loops.Count) looping)"
        }
    }
}
