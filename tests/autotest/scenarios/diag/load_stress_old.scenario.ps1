# load-stress with the load as it was before 2026-10-16 (the world's ids
# counted as the save's, the soldiers' old things left): does the CryPhysics
# crash come as often without the removals made since?
@{
    Name = "load-stress-old"
    Tags = @("diag", "load", "ab")
    SummaryRu = "То же со старым поведением загрузки (до правок 16.10): падает ли игра так же часто"
    Session = @{ Fixture = "save:checking"; Companion = $true; Cvars = @{ coop_test_load_world_ids = 1; coop_test_rearm_keep_old = 1 } }
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
