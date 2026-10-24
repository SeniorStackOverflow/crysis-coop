# The user's game of 2026-10-23 (0.11.45, the campaign "checking"): on the
# host's screen the AI companion swam holding its rifle out, and sat at the
# riverboat's machine gun holding its rifle. Two causes: (1) the bot's
# ChooseWeapon took out its best rifle while it swam (going into the water
# had put the fists in its hands, as in single player) - fixed: no weapon
# choice while swimming or in a vehicle; (2) the host applied a weapon change
# of the companion's own game that came after the host had seated it (the
# enter and the item update are not ordered): the rifle was drawn again in
# the seat, and its own game's later "nothing in hand" never put it away (an
# item id 0 selects nothing) - fixed: CActor::NetSetCurrentItem ignores an
# item for a player in a vehicle.
# Here, on a copy of that campaign (Rescue_10), at the river by the bridge
# with its two riverboats (US_smallboat: a ShiTen at the gunner's seat;
# River_Vehicle_4 of the fixture is the LTV the user drove, on land at the
# graveyard, so the boats are used). Every 0.5 s the host logs what the
# companion holds on its screen ("[CHK] item"), its own game the same for
# itself ("[CHK] mine").
#  (a) swimming: deep water is looked up with rays down to the water's
#      surface and the river bed, the companion is put in it by its own game,
#      its own swim stance is checked, and it swims across by goto: never a
#      gun in its hands on the host, no "takes the" in its log;
#  (b) seated: the host takes the boat's wheel, the companion swims to the
#      boat and sits at the gun (its server gives it the gunner's seat
#      first): no gun of its own in its hands on the host; then its own game
#      draws its rifle in the seat - the late weapon change of the race,
#      made certain - and the host must keep it away.
# On 0.11.45 (expected FAIL): about a second after it is in the water its log
# says "takes the FY71", the host shows the FY71 in its hands for the rest of
# the swim (the host's samples count from a second after its own game swims:
# the fists it took there need that moment to reach the host); at the gun
# the late change draws the FY71 on the host and it stays. (The natural
# boarding is the race itself: on the old build it may or may not show the
# rifle; the late change shows it every time.)
# On the fixed build (expected PASS): Fists (or nothing) the whole swim, no
# "takes the", nothing in its hands at the gun, the late change ignored.
@{
    Name = "companion-weapon-away"
    Tags = @("companion", "vehicle", "sync")
    SummaryRu = "Компаньон в воде и за пулемётом речной лодки (игра 23.10): на экране хоста плывёт без винтовки в руках, за пулемётом её не держит - и когда смена оружия приходит после посадки"
    Session = @{ Fixture = "save:graveyard1023"; Campaign = "56c79107e0d7a6ff"; Companion = $true; ReadyCommands = @("coop_god 1") }
    Params = @{
        Boats = @("Riverboat_3", "Riverboat_6")
        # (more of the river: the level's fish shoals in it, by the bridge)
        Centers = @(@(1725, 1849), @(1695, 1894), @(1747, 1801))
        # the host stands here during the swim (ledge-bridge's top of the bank)
        Bank = @(1699.0, 1856.0)
        # if the rays find no water: between the two boats, the surface as at the boats
        Fallback = @(1710.0, 1872.0); Surface = 190.3
        MinDepth = 1.8
        # its hand guns (the bot's ranks, the LAW): none of them may be in its hands
        Guns = @("FY71", "SCAR", "SMG", "Shotgun", "DSG1", "GaussRifle", "Hurricane", "MOAC", "MOAR", "SOCOM", "LAW", "TACGun")
        SwimFor = 30; SwimMin = 8; MinSamples = 8; Board = 45; BoardAgain = 30; Seated = 10; Late = 6
    }
    Body = {
        param($P)
        $S = @{}
        $gunRx = '^(' + ($P.Guns -join '|') + ')$'
        $gunLua = ($P.Guns | ForEach-Object { '"' + $_ + '"' }) -join ','
        function F([double]$v) { $v.ToString("0.###", [Globalization.CultureInfo]::InvariantCulture) }
        function Get-D2($a, [double]$x, [double]$y) { [Math]::Sqrt([Math]::Pow($a.X - $x, 2) + [Math]::Pow($a.Y - $y, 2)) }
        # what the companion holds on the host's screen
        function Get-Items($Since) {
            $rx = '^\[CHK\] item t=([\d.]+) cls=(\S+) swim=(\d) veh=(\S+) pos=(-?[\d.]+),(-?[\d.]+),(-?[\d.]+) dep=(-?[\d.]+) wl=(-?[\d.]+)'
            @(Find-Log host $rx $Since -All | ForEach-Object { $g = $_.Match.Groups
                [pscustomobject]@{ No = $_.No; Cls = $g[2].Value; Swim = [int]$g[3].Value; Veh = $g[4].Value; X = [double]$g[5].Value; Y = [double]$g[6].Value; Z = [double]$g[7].Value; Dep = [double]$g[8].Value; Wl = [double]$g[9].Value } })
        }
        # what it holds in its own game
        function Get-Mine($Since) {
            $rx = '^\[CHK\] mine t=([\d.]+) cls=(\S+) swim=(\d) veh=(\d) pos=(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)'
            @(Find-Log companion $rx $Since -All | ForEach-Object { $g = $_.Match.Groups
                [pscustomobject]@{ No = $_.No; Cls = $g[2].Value; Swim = [int]$g[3].Value; Veh = [int]$g[4].Value; X = [double]$g[5].Value; Y = [double]$g[6].Value; Z = [double]$g[7].Value } })
        }
        # in the water on the host: swimming there, or in deep water below its surface, on foot
        function Test-InWater($s) { $s.Veh -eq '-' -and ($s.Swim -eq 1 -or ($s.Dep -ge $P.MinDepth -and $s.Z -lt $s.Wl - 0.3)) }
        function Get-Hist($Samples) {
            $h = @($Samples | Group-Object Cls | Sort-Object Count -Descending | ForEach-Object { "$($_.Name) x$($_.Count)" })
            if ($h.Count) { $h -join ', ' } else { 'no samples' }
        }
        # put there by its own game (the server's MovePlayer does not move a
        # client's own player), at once a goto there (an old hold spot makes
        # it go back), then where its own game has it
        function Set-Companion([double]$X, [double]$Y, [double]$Z, [double]$Within, [string]$Tag) {
            $at = $null; $off = 99.0
            for ($i = 0; $i -lt 4; $i++) {
                $S.PutMark = New-Mark
                Send-Game companion ('#System.GetEntity(g_localActorId):SetWorldPos({x=' + (F $X) + ',y=' + (F $Y) + ',z=' + (F $Z) + '})')
                Pause-Game 0.3
                $mA = New-Mark
                Send-Agent ("goto " + (F $X) + " " + (F $Y) + " " + (F $Z)) | Out-Null
                [void](Wait-Log companion '^\[CoopAgent\] arrived: holding there' 6 $mA)
                $mC = New-Mark
                Send-Game companion '#local p=System.GetEntity(g_localActorId):GetWorldPos(); System.LogAlways(string.format("[CHK] companion at %.1f %.1f %.1f",p.x,p.y,p.z))'
                $at = Wait-Log companion '^\[CHK\] companion at (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)' 3 $mC
                $off = if ($at) { [Math]::Sqrt([Math]::Pow([double]$at.Match.Groups[1].Value - $X, 2) + [Math]::Pow([double]$at.Match.Groups[2].Value - $Y, 2)) } else { 99.0 }
                if ($off -lt $Within) { break }
            }
            Require ($off -lt $Within) ("$Tag`: the companion is there in its own game (" + $(if ($at) { ("{0:N1} m off: {1}" -f $off, $at.Text) } else { 'no line' }) + ")")
        }

        Step "the river by the bridge: the watch, nobody alive there, the boats, deep water" {
            Send-Game host "coop_god 1", "v_goliathMode 1", "coop_test_enter out"
            Pause-Game 8
            # depth at x y: the water's surface (a ray down that hits only water,
            # ent_water = 512; else CryAction.GetWaterInfo) above the terrain
            # (AtD never fails: the watch and the look-ups go on without it)
            Send-Game host '#function AtD(x,y) local ok,d,w,g=pcall(AtD0,x,y); if ok and d then return d,w,g end return -1,0,0 end'
            Send-Game host '#function AtD0(x,y) local g=System.GetTerrainElevation({x=x,y=y,z=0}); local w; if Physics.RayWorldIntersection({x=x,y=y,z=g+40},{x=0,y=0,z=-41},1,512,g_localActorId,nil,g_HitTable)>0 and g_HitTable[1] and g_HitTable[1].pos and g_HitTable[1].pos.z>g then w=g_HitTable[1].pos.z end; if not w then local l=CryAction.GetWaterInfo({x=x,y=y,z=g+0.2}); if l and l>g+0.2 then w=l end end; if not w then return -1,0,g end return w-g,w,g end'
            Send-Game host '#function AtFar(x,y) for _,b in ipairs(AtBoats or {}) do if (x-b.x)^2+(y-b.y)^2<25 then return false end end return true end'
            Send-Game host '#function AtDeep(cx,cy,r) local b={d=-1}; for i=-r,r,3 do for j=-r,r,3 do local x,y=cx+i,cy+j; if AtFar(x,y) then local d,w,g=AtD(x,y); if d>b.d then b={x=x,y=y,d=d,w=w,g=g} end end end end return b end'
            Send-Game host '#function AtLine(a,x,y) local n=math.max(1,math.floor(math.sqrt((x-a.x)^2+(y-a.y)^2)/2)); local m=99; for k=0,n do local px,py=a.x+(x-a.x)*k/n,a.y+(y-a.y)*k/n; if not AtFar(px,py) then return -1 end local d=AtD(px,py); if d<m then m=d end end return m end'
            # the watch: the host's copy of the companion, its own game (a sample that fails does not stop it)
            Send-Game host '#function AtIOne() local c=System.GetEntityByName("Companion"); if not (c and c.actor and c.inventory) then return end local id=c.inventory:GetCurrentItemId(); local e=id and System.GetEntity(id); local p=c:GetWorldPos(); local v=c.actor:GetLinkedVehicleId(); local ve=v and System.GetEntity(v); local s=c.actorStats and c.actorStats.stance; local d,w=AtD(p.x,p.y); System.LogAlways(string.format("[CHK] item t=%.1f cls=%s swim=%d veh=%s pos=%.1f,%.1f,%.1f dep=%.1f wl=%.1f",AtI.t,e and e.class or "none",(STANCE_SWIM and s==STANCE_SWIM) and 1 or 0,ve and ve:GetName() or "-",p.x,p.y,p.z,d,w)) end'
            Send-Game host '#AtI={on=true,t=0}; function AtITick() if not AtI.on then return end AtI.t=AtI.t+0.5; pcall(AtIOne); Script.SetTimer(500,AtITick) end; AtITick()'
            Send-Game companion '#function AtMOne() local c=System.GetEntity(g_localActorId); if not (c and c.actor and c.inventory) then return end local id=c.inventory:GetCurrentItemId(); local e=id and System.GetEntity(id); local p=c:GetWorldPos(); local s=c.actorStats and c.actorStats.stance; System.LogAlways(string.format("[CHK] mine t=%.1f cls=%s swim=%d veh=%d pos=%.1f,%.1f,%.1f",AtM.t,e and e.class or "none",(STANCE_SWIM and s==STANCE_SWIM) and 1 or 0,c.actor:GetLinkedVehicleId() and 1 or 0,p.x,p.y,p.z)) end'
            Send-Game companion '#AtM={on=true,t=0}; function AtMTick() if not AtM.on then return end AtM.t=AtM.t+0.5; pcall(AtMOne); Script.SetTimer(500,AtMTick) end; AtMTick()'
            $mG = New-Mark
            Send-Game companion ('#local p=System.GetEntity(g_localActorId); local s=""; for _,k in ipairs({' + $gunLua + '}) do if p.inventory:GetItemByClass(k) then s=s.." "..k end end; System.LogAlways("[CHK] its guns:"..s)')
            $guns = Wait-Log companion '^\[CHK\] its guns:(.*)$' 5 $mG
            # (one the bot would take out: the LAW is no choice of its own)
            Require ($guns -and $guns.Match.Groups[1].Value -match '\b(FY71|SCAR|SMG|Shotgun|DSG1|GaussRifle|Hurricane|MOAC|MOAR|SOCOM)\b') ("the companion carries a gun it would take out (" + $(if ($guns) { $guns.Text } else { 'no line' }) + ")")
            # (out of the LTV, if the fixture has them in it: it follows the host out)
            $last = $null
            for ($i = 0; $i -lt 20; $i++) {
                Pause-Game 1
                $w = @(Get-Items $mG)
                $last = if ($w.Count) { $w[-1] } else { $null }
                if ($last -and $last.Veh -eq '-') { break }
            }
            Require ($last -and $last.Veh -eq '-') ("the host's watch runs and the companion is on foot (" + $(if ($last) { "in $($last.Veh), holds $($last.Cls)" } else { 'no sample' }) + ")")
            Send-Agent "hold" | Out-Null
            $bx = F $P.Bank[0]; $by = F $P.Bank[1]
            $mK = New-Mark
            Send-Game host ('#local q={x=' + $bx + ',y=' + $by + ',z=0}; q.z=System.GetTerrainElevation(q)+0.3; g_gameRules.game:MovePlayer(g_localActorId,q,{x=0,y=0,z=0}); local k=0; for _,e in ipairs(System.GetEntitiesInSphere({x=q.x,y=q.y,z=q.z},300) or {}) do if e.actor and not e.actor:IsPlayer() and e.actor:GetHealth()>0 then g_gameRules:CreateHit(e.id,g_localActorId,g_localActorId,9000,nil,nil,nil,"bullet"); k=k+1 end end; System.LogAlways("[CHK] the host on the bank; soldiers by the river shot: "..k)')
            $kl = Wait-Log host '^\[CHK\] the host on the bank; ' 5 $mK
            # (the companion's game takes the host's new place only once he moves)
            Send-Game host "coop_test_key jump 1"
            Pause-Game 0.3
            Send-Game host "coop_test_key jump 0"
            Pause-Game 3
            $mB = New-Mark
            Send-Game host ('#AtBoats={}; local s=""; for _,n in ipairs({' + (($P.Boats | ForEach-Object { '"' + $_ + '"' }) -join ',') + '}) do local e=System.GetEntityByName(n); if e then local p=e:GetWorldPos(); table.insert(AtBoats,{x=p.x,y=p.y}); local ok,dd=pcall(function() return e.vehicle:IsDestroyed() end); local d,w=AtD(p.x,p.y); s=s..string.format(" %s=%.1f,%.1f,%.1f,hidden=%s,wreck=%s,depth=%.1f,water=%.2f;",n,p.x,p.y,p.z,tostring(e:IsHidden()),tostring(ok and dd),d,w) end end; System.LogAlways("[CHK] boats:"..s)')
            $bl = Wait-Log host '^\[CHK\] boats:(.*)$' 5 $mB
            $boats = @(if ($bl) { [regex]::Matches($bl.Match.Groups[1].Value, ' (\S+?)=(-?[\d.]+),(-?[\d.]+),(-?[\d.]+),hidden=(\w+),wreck=(\w+),depth=(-?[\d.]+),water=(-?[\d.]+);') | ForEach-Object {
                [pscustomobject]@{ Name = $_.Groups[1].Value; X = [double]$_.Groups[2].Value; Y = [double]$_.Groups[3].Value; Z = [double]$_.Groups[4].Value; Hidden = $_.Groups[5].Value; Wreck = $_.Groups[6].Value; Depth = [double]$_.Groups[7].Value; Water = [double]$_.Groups[8].Value } } })
            $S.Boat = @($boats | Where-Object { $_.Hidden -ne 'true' -and $_.Wreck -ne 'true' }) | Select-Object -First 1
            Check $true ("by the river: " + $(if ($kl) { $kl.Text -replace '^\[CHK\] ', '' } else { 'no line' }) + "; boats:" + $(if ($bl) { $bl.Match.Groups[1].Value } else { ' no line' }))
            Require ($null -ne $S.Boat) "a riverboat by the bridge, shown and whole, to sit in"
            $mS = New-Mark
            Send-Game host ('#local cs={}; for _,b in ipairs(AtBoats) do table.insert(cs,{b.x,b.y}) end; for _,c in ipairs({' + (($P.Centers | ForEach-Object { '{' + (F $_[0]) + ',' + (F $_[1]) + '}' }) -join ',') + '}) do table.insert(cs,c) end; local best={d=-1}; for _,c in ipairs(cs) do local b=AtDeep(c[1],c[2],12); if b.d>best.d then best=b end end; AtA=best; AtB=nil; if best.d>=' + (F $P.MinDepth) + ' then for _,r in ipairs({22,16,11}) do for a=0,345,15 do local x=best.x+r*math.cos(math.rad(a)); local y=best.y+r*math.sin(math.rad(a)); if not AtB and AtLine(best,x,y)>=' + (F $P.MinDepth) + ' then AtB={x=x,y=y,r=r} end end end end; System.LogAlways(string.format("[CHK] deep water at %.1f %.1f: %.1f m (surface %.2f, bed %.1f); across to %s",best.x or 0,best.y or 0,best.d,best.w or 0,best.g or 0,AtB and string.format("%.1f %.1f, %d m",AtB.x,AtB.y,AtB.r) or "none"))')
            $dw = Wait-Log host '^\[CHK\] deep water at (-?[\d.]+) (-?[\d.]+): (-?[\d.]+) m \(surface (-?[\d.]+), bed (-?[\d.]+)\); across to (?:(-?[\d.]+) (-?[\d.]+), (\d+) m|none)' 20 $mS
            if ($dw -and [double]$dw.Match.Groups[3].Value -ge $P.MinDepth) {
                $g = $dw.Match.Groups
                $S.A = @([double]$g[1].Value, [double]$g[2].Value); $S.Surf = [double]$g[4].Value
                $S.B = if ($g[6].Success) { @([double]$g[6].Value, [double]$g[7].Value) } else { $null }
                Check $true ("deep water: " + ($dw.Text -replace '^\[CHK\] ', ''))
            } else {
                # (the rays found none: its own swim stance below says whether this spot is water)
                $S.A = @($P.Fallback[0], $P.Fallback[1]); $S.B = $null
                $S.Surf = if ($S.Boat.Water -gt 0) { $S.Boat.Water } else { $P.Surface }
                Check $true ("no deep water found by the rays (" + $(if ($dw) { $dw.Text -replace '^\[CHK\] ', '' } else { 'no line' }) + ("): the spot between the boats, its surface taken as {0:N2}" -f $S.Surf))
            }
        }
        Step "(a) in deep water: it swims across, never a gun in its hands on the host's screen" {
            $a = $S.A
            Set-Companion $a[0] $a[1] ($S.Surf - 0.8) 2.5 "in the water"
            $mPut = $S.PutMark
            # its own game swims there
            $sw = $null
            for ($i = 0; $i -lt 12 -and -not $sw; $i++) {
                Pause-Game 0.5
                $sw = @(Get-Mine $mPut | Where-Object { $_.Swim -eq 1 }) | Select-Object -First 1
            }
            $hostIn = @(Get-Items $mPut | Where-Object { Test-InWater $_ })
            $m0 = @(Get-Mine $mPut)
            Require ($null -ne $sw -or $hostIn.Count -ge 2) ("it swims (its own game: " + $(if ($m0.Count) { "swim=$($m0[-1].Swim) at z {0:N1}" -f $m0[-1].Z } else { 'no sample' }) + "; the host saw it in the water $($hostIn.Count) times)")
            # (counted from a second on: the host may have its copy in the water
            # a moment before the fists its own game took there reach it; the
            # rifle the old build takes out a second later stays out the whole swim)
            Pause-Game 1
            $mCount = New-Mark
            $to = if ($S.B) { $S.B } else { $a }
            $mGo = New-Mark
            Send-Agent ("goto " + (F $to[0]) + " " + (F $to[1]) + " " + (F $S.Surf)) | Out-Null
            $arrived = $null
            $t0 = Get-Date
            $end = $t0.AddSeconds($P.SwimFor)
            while ((Get-Date) -lt $end) {
                Pause-Game 1
                $arrived = Find-Log companion '^\[CoopAgent\] arrived: holding there' $mGo
                # (there: on the spot in the water a little longer, 8 s at least in all)
                if ($arrived) { Pause-Game ([Math]::Max(4, $P.SwimMin - ((Get-Date) - $t0).TotalSeconds)); break }
            }
            $all = @(Get-Items $mCount)
            $in = @($all | Where-Object { Test-InWater $_ })
            $mine = @(Get-Mine $mCount)
            $mineSwim = @($mine | Where-Object { $_.Swim -eq 1 })
            # (the host's copy said neither, its own game swam the whole time:
            # every host sample on foot counts)
            if ($in.Count -lt $P.MinSamples -and $mineSwim.Count -ge $P.MinSamples -and $mineSwim.Count -eq $mine.Count) {
                $in = @($all | Where-Object { $_.Veh -eq '-' })
            }
            Require ($in.Count -ge $P.MinSamples) "the host saw it in the water $($in.Count) times, 0.5 s apart (at least $($P.MinSamples))"
            $swum = Get-D2 $in[0] $in[-1].X $in[-1].Y
            Check $true ("the swim: {0:N1} m from where it began, to {1} ({2}); host samples in the water {3} of {4}, swimming there {5}; its own game swam in {6} of {7} samples, in its hands there: {8}" -f $swum,
                $(if ($S.B) { (F $S.B[0]) + " " + (F $S.B[1]) } else { 'its spot (none across)' }), $(if ($arrived) { 'arrived' } else { "not there in $($P.SwimFor) s" }),
                $in.Count, $all.Count, @($in | Where-Object { $_.Swim -eq 1 }).Count, $mineSwim.Count, $mine.Count, (Get-Hist $mineSwim))
            $bad = @($in | Where-Object { $_.Cls -match $gunRx })
            Check ($bad.Count -eq 0) ("on the host's screen it swam with no gun in its hands (" + (Get-Hist $in) + $(if ($bad.Count) { "; a gun in $($bad.Count) of $($in.Count) samples, first at " + (F $bad[0].X) + " " + (F $bad[0].Y) + " " + (F $bad[0].Z) } else { "" }) + ")")
            $takes = @(Find-Log companion '^\[CoopAgent\] takes the (\S+)' $mPut -All)
            Check ($takes.Count -eq 0) ("it took out no weapon in the water (" + $(if ($takes.Count) { "$($takes.Count) times: " + (($takes | Select-Object -First 3 | ForEach-Object { $_.Text -replace '^\[CoopAgent\] ', '' }) -join ' | ') } else { "no 'takes the'" }) + ")")
        }
        Step "(b) at the boat's gun: the host takes the wheel, the companion swims to the boat and sits at the gun" {
            $bn = $S.Boat.Name
            $mE = New-Mark
            Send-Game host ('#local v=System.GetEntityByName("' + $bn + '"); local p=v:GetWorldPos(); local s=v:GetDirectionVector(0); g_gameRules.game:MovePlayer(g_localActorId,{x=p.x+s.x*2.5,y=p.y+s.y*2.5,z=p.z+0.6},{x=0,y=0,z=0}); System.LogAlways("[CHK] the host by ' + $bn + '")')
            Pause-Game 1
            Send-Game host "coop_test_enter $bn 1"
            $hin = Wait-Log host ('^\[CoopTest\] the host gets into ' + [regex]::Escape($bn) + ' seat (\S+): (\S+)') 10 $mE
            Require ($hin -and $hin.Match.Groups[2].Value -eq 'ok') ("the host at the boat's wheel (" + $(if ($hin) { $hin.Text } else { 'no line' }) + ")")
            Send-Agent "follow" | Out-Null
            $rx = 'Companion entered vehicle ' + [regex]::Escape($bn) + ' seat (\S+)'
            $cin = Wait-Log host $rx $P.Board $mE
            $helped = $false
            if (-not $cin) {
                # (not near enough by itself: put 3 m off the boat's other side in the water, where it asks for a seat)
                $helped = $true
                Send-Game companion ('#local v=System.GetEntityByName("' + $bn + '"); local p=v:GetWorldPos(); local s=v:GetDirectionVector(0); System.GetEntity(g_localActorId):SetWorldPos({x=p.x-s.x*3,y=p.y-s.y*3,z=' + (F ($S.Surf - 0.8)) + '})')
                $cin = Wait-Log host $rx $P.BoardAgain $mE
            }
            Require ($null -ne $cin) ("the companion got into $bn" + $(if ($helped) { " (put next to it after $($P.Board) s)" } else { "" }))
            $S.Seat = $cin.Match.Groups[1].Value
            $mIn = New-Mark
            Pause-Game $P.Seated
            $seated = @(Get-Items $mIn | Where-Object { $_.Veh -eq $bn })
            Require ($seated.Count -ge $P.MinSamples) "the host saw it in the boat $($seated.Count) times (at least $($P.MinSamples))"
            Check $true ("it sits at the $($S.Seat) seat" + $(if ($helped) { " (put next to the boat)" } else { " (swam to it)" }) + "; in its own game it holds: " + (Get-Hist @(Get-Mine $mIn | Where-Object { $_.Veh -eq 1 })))
            $bad = @($seated | Where-Object { $_.Cls -match $gunRx })
            Check ($bad.Count -eq 0) ("seated, no gun of its own in its hands on the host's screen (" + (Get-Hist $seated) + ")")
        }
        Step "(b) a late weapon change: its own game draws its rifle in the seat, the host keeps it away" {
            $bn = $S.Boat.Name
            $mF = New-Mark
            Send-Game companion ('#local p=System.GetEntity(g_localActorId); local n; for _,k in ipairs({' + $gunLua + '}) do if not n and p.inventory:GetItemByClass(k) then n=k end end; if n then p.actor:SelectItemByName(n) end; local id=p.inventory:GetCurrentItemId(); local e=id and System.GetEntity(id); System.LogAlways("[CHK] its own game draws "..tostring(n).." in the seat: holds "..(e and e.class or "none"))')
            $dr = Wait-Log companion '^\[CHK\] its own game draws (\S+) in the seat: holds (\S+)' 5 $mF
            Pause-Game 1.5
            $own = @(Get-Mine $mF | Where-Object { $_.Veh -eq 1 })
            Require ($dr -and $own.Count -and $own[-1].Cls -match $gunRx) ("its own game holds a gun in the seat (" + $(if ($dr) { $dr.Text -replace '^\[CHK\] ', '' } else { 'no line' }) + "; then: " + (Get-Hist $own) + ")")
            Pause-Game $P.Late
            $after = @(Get-Items $mF | Where-Object { $_.Veh -eq $bn })
            Require ($after.Count -ge $P.MinSamples) "the host saw it in the boat $($after.Count) times after the change (at least $($P.MinSamples))"
            $bad = @($after | Where-Object { $_.Cls -match $gunRx })
            Check ($bad.Count -eq 0) ("the weapon change that came after the enter left its hands empty on the host's screen (" + (Get-Hist $after) + ")")
            Send-Game companion '#pcall(function() System.GetEntity(g_localActorId).actor:HolsterItem(true) end)'
        }
        Send-Game host '#AtI.on=false'
        Send-Game companion '#AtM.on=false'
    }
}
