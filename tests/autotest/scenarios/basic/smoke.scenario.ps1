# The test system itself: a new game on Rescue, the AI companion joins, half a
# minute of play. Proves the rules (window, focus, sound, saves) in the report.
@{
    Name = "smoke"
    Tags = @("smoke", "companion")
    SummaryRu = "Новая игра (Rescue), ИИ-компаньон входит, 30 с игры без ошибок"
    Session = @{ Fixture = "new:rescue"; Companion = $true }
    Body = {
        param($P)
        Step "30 s of play" {
            Send-Game host "coop_god 1"
            Pause-Game 30
        }
        Check ($null -ne (Find-Log host 'renamed to (\$\d)?Companion|revived Companion')) "the companion is in the game"
        Check ((Count-Log host '^\[CoopPos\] ') -gt 0) "the host logs player positions"
    }
}
