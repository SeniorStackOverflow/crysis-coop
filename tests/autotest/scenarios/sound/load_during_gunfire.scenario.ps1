# The user's game of 2026-10-17: the bridge reinforcements' truck gunner
# was firing when everybody went down; the checkpoint loaded into the running
# level; from then on a sound of gunfire went on and on, for ever. Here the
# same: the host (and the AI companion) by the bridge, the truck's gunner
# shoots at them, everybody goes down while he fires, the checkpoint loads;
# 10 s and 30 s later no gunfire loop may still be playing.
@{
    Name = "load-during-gunfire-sound"
    Tags = @("sound", "load", "combat")
    SummaryRu = "Все погибли, пока по ним стрелял пулемёт грузовика, загрузился чекпойнт: звук стрельбы не зацикливается"
    Session = @{ Fixture = "new:rescue"; Companion = $true; Sound = "wav" }
    Params = @{ Truck = "Bridge_reinforcement_Truck_1"; Gunner = "Bridge_Reinforcement_2" }
    Body = {
        param($P)
        $t = $P.Truck; $g = $P.Gunner
        function Get-GunLoops([string]$Tag) {
            # FMOD's own channels: a weapon's fire loop sample (mg_fire_loop,
            # fy71_3rd_loop, ...) still playing
            $m = New-Mark
            Send-Game host "coop_test_sound channels"
            Pause-Game 2
            $all = @(Find-Log host '^\[CoopTest\] channel \d+ ' $m -All)
            $loops = @($all | Where-Object { $_.Text -match '(?i)\] channel \d+ \S*(fire_loop|_loop_|loop_close|_cu_loop|_fp_loop|3rd_loop|rapid)' })
            Check $true ("$Tag`: FMOD channels " + $all.Count + "; gunfire loops: " + $(if ($loops.Count) { (($loops | Select-Object -First 6 | ForEach-Object { ($_.Text -replace '^\[CoopTest\] ', '') }) -join "; ") } else { "none" }))
            return $loops
        }
        Step "the host by the bridge: the truck comes, its gunner shoots" {
            Send-Game host "coop_god 1"
            Pause-Game 40
            $m = New-Mark
            Send-Game host '#local p={x=1737,y=1960,z=0}; p.z=System.GetTerrainElevation(p)+1; g_gameRules.game:MovePlayer(g_localActorId,p,{x=0,y=0,z=0}); local c=System.GetEntityByName("Companion"); if c then g_gameRules.game:MovePlayer(c.id,{x=p.x-4,y=p.y-3,z=p.z},{x=0,y=0,z=0}) end; System.LogAlways("[CHK] the players by the bridge")'
            $in = Wait-Log host ("\] " + $g + " entered vehicle " + $t + " seat gunner") 90 $m
            Require ($null -ne $in) "the truck's gunner $g got to its gun"
            $fire = Wait-Log host ("HIT (player \S+|vehicle \S+) by " + $g + " weapon=VehicleShiTenV2") 120 $m
            Require ($null -ne $fire) "the gunner shoots the truck's gun"
            [void](Get-GunLoops "while he fires")
        }
        Step "everybody goes down while he fires: the checkpoint loads" {
            $m = New-Mark
            Send-Game host "coop_god 0", "coop_test_kill Companion", "coop_test_kill Nomad"
            $end = Wait-Log host "^\[Coop\] checkpoint loaded: \d+ players' places kept" 300 $m
            Require ($null -ne $end) "the checkpoint was loaded"
            Send-Game host "coop_god 1"
            Pause-Game 10
            $l10 = @(Get-GunLoops "10 s after the load")
            Pause-Game 20
            $l30 = @(Get-GunLoops "30 s after the load")
            Check ($l30.Count -eq 0) "30 s after the load: no gunfire loop still playing ($($l30.Count))"
        }
    }
}
