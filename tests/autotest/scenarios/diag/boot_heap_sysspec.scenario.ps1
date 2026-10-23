# The heap watch on the AI companion's way of starting: a game without a
# real sound system and "+sys_spec 1" on its command line (the companion's).
# A spec change sets the sound console variables of sys_spec_Sound; in a game
# whose sound system failed (s_SoundEnable 0: "Creating DummySoundSystem
# now!") they still point into the deleted CSoundSystem. Run with -Repeat N,
# -Dll for another build (A/B).
@{
    Name = "boot-heap-sysspec"
    Tags = @("diag", "heap")
    SummaryRu = "Хост без звука и с +sys_spec 1 (как компаньон) загружает Rescue: куча движка цела"
    Session = @{ Fixture = "new:rescue"; Companion = $false; Cvars = @{ sys_spec = 1 } }
    Body = {
        param($P)
        Step "booted" {
            Pause-Game 3
        }
        $bad = Find-Log host '\[CoopBug\] the engine.s heap is damaged'
        Check ($null -eq $bad) "the engine's heap is whole after the start$(if ($bad) { ': ' + $bad.Text })"
    }
}
