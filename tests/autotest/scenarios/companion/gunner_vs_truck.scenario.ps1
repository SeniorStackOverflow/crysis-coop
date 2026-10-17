# The user's games of 2026-10-16/17: with the AI companion at the gun of the
# LTV the host drove, the bridge reinforcements' truck came; its gunner shot
# them up and the companion never hit him or his truck. Here the same: the
# host drives the LTV (Non_Driveable_Vehicles.Civ_Car8) to the bridge, the
# companion at its gun; the level sends the truck; the companion has to hit
# the truck's crew (its gunner above all).
@{
    Name = "companion-gunner-vs-truck"
    Tags = @("companion", "vehicle", "combat")
    SummaryRu = "Компаньон за пулемётом джипа у моста стреляет по приехавшему грузовику с подкреплением и его стрелку (как в игре 16-17.10)"
    Session = @{ Fixture = "new:rescue"; Companion = $true }
    Params = @{ Vehicle = "Non_Driveable_Vehicles.Civ_Car8"; Truck = "Bridge_reinforcement_Truck_1"; Gunner = "Bridge_Reinforcement_2"; Fight = 90; MinHits = 3 }
    Body = {
        param($P)
        $v = $P.Vehicle
        Step "the host at the bridge calls the truck, then takes the LTV with the companion" {
            Send-Game host "coop_god 1"
            Send-Game host "v_goliathMode 1"
            Pause-Game 40
            $m = New-Mark
            Send-Game host '#local p={x=1737,y=1960,z=0}; p.z=System.GetTerrainElevation(p)+1; g_gameRules.game:MovePlayer(g_localActorId,p,{x=0,y=0,z=0}); local c=System.GetEntityByName("Companion"); if c then g_gameRules.game:MovePlayer(c.id,{x=p.x-4,y=p.y-3,z=p.z},{x=0,y=0,z=0}) end; System.LogAlways("[CHK] the players by the bridge")'
            $truck = Wait-Log host ("\] " + $P.Gunner + " entered vehicle " + $P.Truck + " seat gunner") 90 $m
            Require ($null -ne $truck) "the truck's gunner got to its gun"
            Send-Game host ('#local v=System.GetEntityByName("' + $v + '"); local p={x=1730,y=1950,z=0}; p.z=System.GetTerrainElevation(p)+1.2; v:SetWorldPos(p); v:SetAngles({x=0,y=0,z=0}); System.LogAlways("[CHK] the LTV by the players")')
            Pause-Game 2
            $m2 = New-Mark
            Send-Game host "coop_test_enter $v 1"
            $in = Wait-Log host 'Companion entered vehicle' 90 $m2
            Require ($null -ne $in) "the companion got to the LTV's gun"
        }
        Step "the companion fights the truck" {
            $m2 = New-Mark
            $end = (Get-Date).AddSeconds($P.Fight)
            # (the host drives now and then, as the user did)
            while ((Get-Date) -lt $end) {
                Pause-Game 12
                Send-Game host "coop_vehtest 2"
            }
            $hits = Count-Log host '\] HIT (Bridge_Reinforcement_\d+|vehicle Bridge_reinforcement_Truck_1) by Companion ' $m2
            $onGunner = Count-Log host ('\] HIT ' + $P.Gunner + ' by Companion ') $m2
            $killed = Count-Log host '\] HIT Bridge_Reinforcement_\d+ by Companion .*KILLED' $m2
            $byThem = Count-Log host 'HIT (player (Nomad|Companion)|vehicle Non_Driveable_Vehicles.Civ_Car8) by Bridge_Reinforcement_' $m2
            Check ($hits -ge $P.MinHits) "the companion hit the truck and its crew $hits times ($onGunner on the gunner, $killed killed); they hit the players and the LTV $byThem times"
        }
    }
}
