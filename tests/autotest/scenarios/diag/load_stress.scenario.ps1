# The host crashed in CryPhysics.dll+0x10EB29 (read from 0x2F) right after an
# in-place load of the user's campaign "checking" (1 in 81 loads, 2026-10-16;
# the same crash on 2026-10-14). Here: everybody down $Rounds times in a row,
# with the companion in the game, to see how often and after what.
@{
    Name = "load-stress"
    Tags = @("diag", "load")
    SummaryRu = "Все погибли много раз подряд: игра хоста не падает после загрузок"
    Session = @{ Fixture = "save:checking"; Companion = $true }
    Params = @{ Rounds = 15 }
    Body = {
        param($P)
        Pause-Game 8
        Send-Game host "coop_god 0"
        for ($r = 1; $r -le $P.Rounds; $r++) {
            Step "everybody down #$r" {
                $m = New-Mark
                Send-Game host "coop_test_kill Companion", "coop_test_kill Nomad"
                $end = Wait-Log host "^\[Coop\] checkpoint loaded: \d+ players' places kept" 120 $m
                Check ($null -ne $end) "round $r`: the checkpoint was loaded"
                Pause-Game 12
            }
        }
        Check ((Count-Log host 'CRASH DETECTED') -eq 0) "the host lived through $($P.Rounds) loads"
    }
}
