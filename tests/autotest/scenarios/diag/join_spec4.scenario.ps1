# A host at sys_spec 4 (Very High; a fresh profile without the spec line, or
# a friend's machine): the companion could not join the continued campaign at
# the camp (fixture checking1021, a fight going on), 5 of 5, each time with
# another CryNetwork error ("Failed ReconfigureObject 1", "Malformed packet",
# "... for entity BattleDust for RMI CGameRules:ClSetGameTime not found").
# At spec 3 it joined. Of the spec groups only sys_spec_GameEffects 4 turns on
# g_battleDust_enable: the server then makes network BattleEvent entities
# ("BattleDust", 1-15 s each) at the fight's shots and impacts. The joining
# companion got the live ones while its level loaded and its spawn filter
# made them client-only (fixed 2026-10-22, CoopAI IsNetworkedClass).
# The proof that dust came during the join: coop_debug_flags 16, both games
# list every spawn in coop_spawns_server.txt / coop_spawns_client.txt (the
# game folder); a client's "BattleEvent ... load=1" line is one it got while
# its level loaded, flags 00000100 = made client-only (Lua's
# System.GetEntitiesByClass does not see them: they have no script).
@{
    Name = "join-spec4"
    Tags = @("join", "companion", "load", "regress")
    SummaryRu = "Хост на sys_spec 4 (очень высокое качество): ИИ-компаньон входит в продолженную кампанию у лагеря во время боя (с BattleDust) и остаётся в игре"
    Session = @{ Fixture = "save:checking1021"; Campaign = "56c79107e0d7a6ff"; Companion = $true; Extra = @("+sys_spec", "4", "+coop_debug_flags", "24"); ReadyCommands = @("coop_god 1") }
    Params = @{ Stay = 90; Dust = $true; Spec = "4" }
    Body = {
        param($P)
        $joined = New-Mark
        function Get-DustSpawns([string]$Side) {
            $f = Join-Path (Get-CoopTestPaths).GameRoot "coop_spawns_$Side.txt"
            if (-not (Test-Path -LiteralPath $f)) { return @() }
            return @(Get-Content -LiteralPath $f | Where-Object { $_ -match "`tBattleEvent`t" })
        }
        Step "the host's spec" {
            Send-Game host "sys_spec", "g_battleDust_enable"
            Pause-Game 1
            $spec = Find-Log host '(\$\d|\s|^)sys_spec = (\$\d)?(\d)' $joined
            Check ($spec -and $spec.Match.Groups[3].Value -eq $P.Spec) "the host runs at sys_spec $($P.Spec): $(if ($spec) { $spec.Text.Trim() } else { 'no answer' })"
            $dust = Find-Log host 'g_battleDust_enable = (\$\d)?(\d)' $joined
            Check $true "battle dust on the host: $(if ($dust) { $dust.Text.Trim() } else { 'no answer' })"
        }
        Step "the companion stays in the game for $($P.Stay) s of the fight" {
            Pause-Game $P.Stay
            $drops = @(Find-Log host 'Disconnect .*cause=' $joined -All)
            Check ($drops.Count -eq 0) ("the companion was not dropped: " + $(if ($drops.Count) { ($drops | Select-Object -First 2 | ForEach-Object { $_.Text }) -join ' | ' } else { 'no disconnect' }))
            $again = Count-Log host 'renamed to (\$\d)?Companion' $joined
            Check ($again -eq 0) "it did not have to come in again ($again more joins)"
            $server = Get-DustSpawns "server"
            $client = Get-DustSpawns "client"
            $inLoad = @($client | Where-Object { $_ -match "`tload=1`t" })
            $local = @($client | Where-Object { ([Convert]::ToInt32(($_ -replace '^.*flags=', ''), 16) -band 0x100) -ne 0 })
            Check $true "BattleEvent spawns: the host $($server.Count), the companion $($client.Count) ($($inLoad.Count) while its level loaded); $(if ($inLoad.Count) { $inLoad[0] -replace "`t", ' ' })"
            if ($P.Dust) {
                Check ($local.Count -eq 0) "none made client-only on the companion ($($local.Count))"
                Require ($inLoad.Count -gt 0) "battle dust reached the companion while its level loaded ($($inLoad.Count))"
            } else {
                Check ($server.Count -eq 0) "no battle dust on the host ($($server.Count))"
            }
        }
    }
}
