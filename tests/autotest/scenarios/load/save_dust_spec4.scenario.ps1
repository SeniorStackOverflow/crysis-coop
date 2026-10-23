# A host at sys_spec 4 saves a checkpoint in the camp fight right after a
# battle dust area (a network BattleEvent entity, only at spec 4, 1-15 s) was
# made, so it can go into the save. The checkpoint is loaded into the running
# level with the companion in the game: it stays in (or comes back) and is
# not dropped by a network error. The dust is seen in the spawn lists of
# coop_debug_flags 16 (coop_spawns_server.txt in the game folder; Lua's
# System.GetEntitiesByClass does not see script-less entities).
@{
    Name = "load-save-dust-spec4"
    Tags = @("load", "join", "companion")
    SummaryRu = "Хост на sys_spec 4 сохраняет чекпоинт во время боя сразу после появления BattleDust и загружает его: ИИ-компаньон остаётся в игре"
    Session = @{ Fixture = "save:checking1021"; Campaign = "56c79107e0d7a6ff"; Companion = $true; Extra = @("+sys_spec", "4", "+coop_debug_flags", "16"); ReadyCommands = @("coop_god 1") }
    Params = @{ Stay = 60 }
    Body = {
        param($P)
        $f = Join-Path (Get-CoopTestPaths).GameRoot "coop_spawns_server.txt"
        function Get-DustSpawns { if (Test-Path -LiteralPath $f) { return @(Get-Content -LiteralPath $f | Where-Object { $_ -match "`tBattleEvent`t" }) } else { return @() } }
        Step "a checkpoint saved right after battle dust was made" {
            $n0 = (Get-DustSpawns).Count
            $seen = $false
            for ($i = 0; $i -lt 720 -and -not $seen; $i++) { Pause-Game 0.25; if ((Get-DustSpawns).Count -gt $n0) { $seen = $true } }
            Require $seen "the fight made battle dust on the host ($((Get-DustSpawns).Count) BattleEvent spawns)"
            $s0 = New-Mark
            Send-Game host "coop_save"
            $s = Wait-Log host '^\[CoopSave\] checkpoint \S+ saved: ' 60 $s0
            Require ($null -ne $s) "the checkpoint was saved: $(if ($s) { $s.Text } else { 'no line' })"
        }
        Step "the checkpoint loaded: the companion stays for $($P.Stay) s" {
            $m = New-Mark
            $n1 = (Get-DustSpawns).Count
            Send-Game host "coop_load"
            $end = Wait-Log host "^\[Coop\] checkpoint loaded: \d+ players' places kept" 300 $m
            Require ($null -ne $end) "the load ended: $(if ($end) { $end.Text } else { 'no line' })"
            Send-Game host "coop_god 1"
            Pause-Game $P.Stay
            $later = @(Get-DustSpawns)
            Check $true "BattleEvent spawns on the host since the load (the save's and new ones): $($later.Count - $n1)$(if ($later.Count -gt $n1) { '; first: ' + ($later[$n1] -replace "`t", ' ') })"
            $lost = @(Find-Log host 'Disconnect .*cause=' $m -All | Where-Object { $_.Text -notmatch 'cause=25' })
            Check ($lost.Count -eq 0) ("the companion was not dropped by a network error: " + $(if ($lost.Count) { ($lost | Select-Object -First 2 | ForEach-Object { $_.Text }) -join ' | ' } else { 'none' }))
            $drops = @(Find-Log host 'Disconnect .*cause=' $m -All)
            $back = Count-Log host 'renamed to (\$\d)?Companion' $m
            Check ($drops.Count -eq 0 -or $back -gt 0) "it is in the game after the load ($($drops.Count) disconnects, $back joins)"
        }
    }
}
