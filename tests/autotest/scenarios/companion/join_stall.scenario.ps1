# The autotester's game of 2026-10-16 (1 join in ~110): the companion's
# channel came up, then its level never began to load; 44 packets a second
# both ways for minutes, "the AI companion cannot join your game". Here the
# companion's relay lets none of the host's big packets through on its first
# try (COOP_TEST_JOIN_STALL=1): it has to notice, leave, take a new relay and
# come in on the second try.
@{
    Name = "companion-join-stall"
    Tags = @("companion", "join")
    SummaryRu = "Вход ИИ-компаньона застрял (канал есть, уровень не грузится): компаньон сам выходит и заходит снова"
    Session = @{ Fixture = "new:rescue"; Companion = $true; Env = @{ COOP_TEST_JOIN_STALL = "1" } }
    Params = @{}
    Body = {
        param($P)
        Step "the companion got in after its first try stalled" {
            $stall = Find-Log companion 'connected \d+ s and its level never began to load' $null
            Check ($null -ne $stall) "the companion noticed its stalled join: $(if ($stall) { $stall.Text } else { 'no such line' })"
            $again = Count-Log companion 'leads to the host''s game at' $null
            Check ($again -ge 2) "it took a new relay for the second try ($again relays)"
            Send-Game host "coop_god 1"
            Pause-Game 10
            $c = Count-Log host 'renamed to .*Companion' $null
            Check ($c -gt 0) "the host has the companion in its game"
        }
    }
}
