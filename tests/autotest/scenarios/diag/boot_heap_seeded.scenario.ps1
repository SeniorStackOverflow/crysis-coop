# The heap watch: is the engine's heap damaged at the start of a game (the
# MSVCR80 start crash, "[CoopBug] the engine's heap is damaged ... first
# seen at \"level loading progress\""). The host alone boots into Rescue and
# looks once more (here with a seeded profile: no first-login spec detection); run with -Repeat N, and -Dll for another build (A/B).
@{
    Name = "boot-heap-seeded"
    Tags = @("diag", "heap")
    SummaryRu = "С готовым профилем (без автоопределения железа при первом входе): Хост один загружает Rescue: куча движка цела с самого старта (A/B сборок через -Repeat и -Dll)"
    Session = @{ Fixture = "new:rescue"; Companion = $false; SeedProfile = $true }
    Body = {
        param($P)
        Step "booted" {
            Pause-Game 3
        }
        $bad = Find-Log host '\[CoopBug\] the engine.s heap is damaged'
        Check ($null -eq $bad) "the engine's heap is whole after the start$(if ($bad) { ': ' + $bad.Text })"
    }
}
