# The host's game crashed in d3d9.dll (0x12359C, read 0x25, AMD's driver
# below it) right as it started the AI companion, in about 1 of 5 test boots
# of 2026-10-15. Here: the companion started and stopped $Times times in one
# game, so one boot is many tries; the host must live through all of them.
@{
    Name = "companion-restart-stress"
    Tags = @("diag", "companion")
    SummaryRu = "Компаньон включается и выключается много раз подряд: игра хоста не падает"
    Session = @{ Fixture = "new:rescue"; Companion = $true }
    Params = @{ Times = 10 }
    Body = {
        param($P)
        Send-Game host "coop_god 1"
        for ($i = 1; $i -le $P.Times; $i++) {
            Step "companion off and on #$i" {
                $m = New-Mark
                Send-Game host "coop_companion 0"
                Pause-Game 6
                Send-Game host "coop_companion 1"
                $started = Wait-Log host '^\[CoopAgent\] AI companion started' 30 $m
                Check ($null -ne $started) "start $i`: the companion's game started"
                Pause-Game 12
            }
        }
        Check ((Count-Log host 'CRASH DETECTED') -eq 0) "the host lived through $($P.Times) companion starts"
    }
}
