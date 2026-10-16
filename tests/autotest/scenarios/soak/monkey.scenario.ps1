# A game played by chance: for $Minutes the host does what a player does on
# Rescue (goes somewhere, fights, drives with the companion, dies, everybody
# dies, loads the checkpoint, blows something up) in an order from $Seed. After
# each action its own check; the judge reads the whole game after it. A
# failure names the seed and the step, so the same game can be played again.
@{
    Name = "monkey"
    Tags = @("soak")
    SummaryRu = "Игра наугад: хост ходит, воюет, ездит с компаньоном, умирает, грузит чекпойнт, взрывает — после каждого шага проверка"
    Session = @{ Fixture = "new:rescue"; Companion = $true }
    Params = @{ Minutes = 20; Seed = 4711 }
    Body = {
        param($P)
        $rng = New-Object System.Random ([int]$P.Seed)
        $until = (Get-Date).AddMinutes($P.Minutes)
        $pois = @(
            @{ n = "bridge"; x = 1737; y = 1960 }, @{ n = "hut by the bridge"; x = 1695; y = 1825 }, @{ n = "farm"; x = 1309; y = 2158 },
            @{ n = "helicopter camp"; x = 1568; y = 2132 }, @{ n = "convoy road"; x = 1493; y = 1907 }, @{ n = "beach"; x = 1678; y = 1805 }
        )
        $state = '#local c=System.GetEntityByName("Companion"); local n=System.GetEntityByName("Nomad"); local function f(e) if not e then return "- - - hp -1 veh -" end local p=e:GetWorldPos(); local v=e.actor:GetLinkedVehicleId(); return string.format("%.0f %.0f %.0f hp %d veh %s", p.x, p.y, p.z, e.actor:GetHealth(), v and "in" or "-") end; local d=(c and n) and math.floor(c:GetDistance(n.id)) or -1; System.LogAlways("[CHK] state TAG: companion "..f(c).." host "..f(n).." apart "..d)'
        function Get-State([string]$Tag) {
            $m = New-Mark
            Send-Game host $state.Replace("TAG", $Tag)
            Pause-Game 1
            $h = Find-Log host ("^\[CHK\] state " + [regex]::Escape($Tag) + ": companion (\S+) (\S+) (\S+) hp (-?\d+) veh (\S+) host (\S+) (\S+) (\S+) hp (-?\d+) veh (\S+) apart (-?\d+)") $m
            if (-not $h) { return $null }
            $g = $h.Match.Groups
            return [pscustomobject]@{ CompHp = [int]$g[4].Value; CompVeh = $g[5].Value; HostHp = [int]$g[9].Value; HostVeh = $g[10].Value; Apart = [int]$g[11].Value; HostX = $g[6].Value; HostY = $g[7].Value }
        }
        # both alive and on foot before an action (a load or a revive may still be going on)
        function Wait-Ready([string]$Tag) {
            for ($k = 0; $k -lt 30; $k++) {
                $s = Get-State "$Tag ready check $k"
                if ($s -and $s.HostHp -gt 0 -and $s.CompHp -gt 0) {
                    if ($s.HostVeh -eq "in") { Send-Game host "coop_test_enter out"; Pause-Game 3; continue }
                    return $s
                }
                # the companion down, the host up: nobody else brings him back
                # (a player holds the use key next to him), so the host does,
                # as a player would (the 16.10 suite waited 2 min for nothing)
                if ($s -and $s.HostHp -gt 0 -and $s.CompHp -le 0) {
                    $mr = New-Mark
                    Send-Game host "coop_test_revive near"
                    Pause-Game 1
                    Send-Game host "coop_test_revive press"
                    $up = Wait-Log host 'Nomad revived Companion' 10 $mr
                    Send-Game host "coop_test_revive release"
                    if ($up) { Pause-Game 2; continue }
                }
                Pause-Game 4
            }
            Check $false "$Tag`: host and companion alive again within 2 min"
            return $null
        }
        $actions = @(
            @{ n = "travel"; w = 15 }, @{ n = "fight"; w = 15 }, @{ n = "ride"; w = 12 }, @{ n = "revive"; w = 8 },
            @{ n = "alldown"; w = 7 }, @{ n = "load"; w = 7 }, @{ n = "explode"; w = 6 }, @{ n = "idle"; w = 8 }
        )
        $total = 0; foreach ($x in $actions) { $total += $x.w }
        Send-Game host "coop_god 1"
        $step = 0
        while ((Get-Date) -lt $until) {
            $step++
            $r = $rng.Next($total); $a = $null
            foreach ($x in $actions) { if ($r -lt $x.w) { $a = $x.n; break }; $r -= $x.w }
            $poi = $pois[$rng.Next($pois.Count)]
            Step "monkey step $step`: $a (seed $($P.Seed))" {
                Send-Game host ('#System.LogAlways("[CHK] monkey step=' + $step + ' action=' + $a + ' seed=' + $P.Seed + '")')
                $s0 = Wait-Ready "step $step"
                if (-not $s0) { return }
                $m = New-Mark
                switch ($a) {
                    "travel" {
                        Send-Game host ('#local p={x=' + $poi.x + ',y=' + $poi.y + ',z=0}; p.z=System.GetTerrainElevation(p)+1; g_gameRules.game:MovePlayer(g_localActorId,p,{x=0,y=0,z=0}); System.LogAlways("[CHK] host to ' + $poi.n + '")')
                        # (as a player arrives: walking, so his place goes to the companion's game)
                        Send-Game host "coop_test_key moveforward 1"
                        Pause-Game 1.5
                        Send-Game host "coop_test_key moveforward 0"
                        Pause-Game 58
                        $s = Get-State "step $step after travel"
                        Check ($s -and $s.Apart -ge 0 -and $s.Apart -lt 60) "step $step travel to $($poi.n): the companion caught up ($(if ($s) { $s.Apart } else { '?' }) m away after 60 s)"
                    }
                    "fight" {
                        Send-Game host ('#local p={x=' + $poi.x + ',y=' + $poi.y + ',z=0}; p.z=System.GetTerrainElevation(p)+1; g_gameRules.game:MovePlayer(g_localActorId,p,{x=0,y=0,z=0}); System.LogAlways("[CHK] host fights at ' + $poi.n + '")')
                        Send-Game host "coop_god 0"
                        Pause-Game 25
                        Send-Game host "coop_god 1"
                        $hitsBy = Count-Log host '\] HIT \S+ by (Nomad|Companion) ' $m
                        $hitsOn = Count-Log host 'HIT player (Nomad|Companion) by ' $m
                        Check $true "step $step fight at $($poi.n): players hit $hitsBy times, were hit $hitsOn times"
                    }
                    "ride" {
                        $veh = '#local best,bd=nil,1e9; local h=System.GetEntity(g_localActorId); for _,e in ipairs(System.GetEntitiesByClass("Civ_car1") or {}) do end; for _,cls in ipairs({"US_ltv","Asian_ltv","Civ_car1","Asian_truck"}) do for _,e in ipairs(System.GetEntitiesByClass(cls) or {}) do local d=e:GetDistance(g_localActorId); if d<bd and not e:IsHidden() and e.vehicle and not e.vehicle:IsDestroyed() and not string.find(e:GetName(),"Non_Driveable") then best,bd=e,d end end end; if best then local p=best:GetWorldPos(); g_gameRules.game:MovePlayer(g_localActorId,{x=p.x+4,y=p.y,z=p.z+2},{x=0,y=0,z=0}); System.LogAlways("[CHK] ride vehicle "..best:GetName()) else System.LogAlways("[CHK] ride vehicle none") end'
                        Send-Game host $veh
                        Pause-Game 2
                        $v = Find-Log host '^\[CHK\] ride vehicle (\S+)' $m
                        if (-not $v -or $v.Match.Groups[1].Value -eq "none") { Check $true "step $step ride: no vehicle near"; break }
                        $name = $v.Match.Groups[1].Value
                        $m1 = New-Mark
                        Send-Game host "coop_test_enter $name 1"
                        # (an enemy's truck with its driver in it: the host may not get in, nor follow it)
                        Pause-Game 2
                        if (Find-Log host 'the host gets into \S+ seat \S+: refused' $m1) { Check $true "step $step ride $name`: its seat is taken, no ride"; break }
                        $in = Wait-Log host 'Companion gets into|Companion entered vehicle' 90 $m
                        Check ($null -ne $in) "step $step ride $name`: the companion got in"
                        Send-Game host "coop_vehtest 8"
                        Pause-Game 12
                        $m2 = New-Mark
                        Send-Game host "coop_test_enter out"
                        Pause-Game 8
                        $s = Get-State "step $step after the ride"
                        Check ($s -and $s.CompVeh -eq "-" -and $s.Apart -lt 40) "step $step ride $name`: the companion got out by the host ($(if ($s) { $s.Apart } else { '?' }) m)"
                    }
                    "revive" {
                        Send-Game host "coop_god 0"
                        Send-Game host '#g_gameRules:CreateHit(g_localActorId,g_localActorId,g_localActorId,9000,nil,nil,nil,"bullet"); System.LogAlways("[CHK] the host is down")'
                        $up = Wait-Log host 'Nomad is up again|revived Nomad|Nomad revived|everybody is down' 150 $m
                        Send-Game host "coop_god 1"
                        Check ($null -ne $up -and $up.Text -notmatch 'everybody') "step $step revive: the companion revived the host ($(if ($up) { $up.Text } else { 'not within 150 s' }))"
                    }
                    "alldown" {
                        Send-Game host "coop_god 0"
                        Send-Game host "coop_test_kill Companion", "coop_test_kill Nomad"
                        $end = Wait-Log host "^\[Coop\] checkpoint loaded: \d+ players' places kept" 300 $m
                        Send-Game host "coop_god 1"
                        Check ($null -ne $end) "step $step everybody down: the checkpoint was loaded"
                        Pause-Game 20
                        $s = Get-State "step $step after everybody down"
                        Check ($s -and $s.CompHp -gt 0 -and (Count-Log host '^Disconnect ' $m) -eq 0) "step $step everybody down: the companion is still in the game"
                    }
                    "load" {
                        Send-Game host "coop_load"
                        $end = Wait-Log host "^\[Coop\] checkpoint loaded: |^\[CoopSave\] progress loaded: " 300 $m
                        Check ($null -ne $end) "step $step load: the checkpoint was loaded"
                        Pause-Game 20
                        $s = Get-State "step $step after the load"
                        Check ($s -and $s.CompHp -gt 0 -and (Count-Log host '^Disconnect ' $m) -eq 0) "step $step load: the companion is still in the game"
                    }
                    "explode" {
                        Send-Game host ('#local p={x=' + $poi.x + ',y=' + $poi.y + ',z=0}; p.z=System.GetTerrainElevation(p)+1; g_gameRules.game:MovePlayer(g_localActorId,{x=p.x+15,y=p.y,z=p.z},{x=0,y=0,z=0}); g_gameRules:CreateExplosion(g_localActorId,g_localActorId,800,p,{x=0,y=0,z=1},8,45,2000,0,"explosions.C4_explosion.c4_small",1); System.LogAlways("[CHK] explosion at ' + $poi.n + '")')
                        Pause-Game 20
                        Check ((Count-Log host '^\[CoopBug\]' $m) -eq 0) "step $step explosion at $($poi.n): nothing flew off wildly"
                    }
                    default { Pause-Game 20 }
                }
                Check ((Count-Log host 'CRASH DETECTED' $m) -eq 0) "step $step`: no crash"
            }
        }
        Send-Game host ('#System.LogAlways("[CHK] monkey done after ' + $step + ' steps")')
    }
}
