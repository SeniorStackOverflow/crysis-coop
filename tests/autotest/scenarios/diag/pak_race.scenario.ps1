# CryPak's cached-file race (the user's crash of 2026-10-22, CrySystem+0xB69C
# in the Streaming thread; vanilla's own code): coop_test_pakrace makes it
# happen - a thread holds the pak's set lock while a stream of a big
# compressed file asks for it and the file's last reference is let go; both
# go on at once. With the fix (coop_pak_fix 1, the default) the dying one is
# never taken: the races are counted, no crash. The _unfixed variant turns the
# fix off and is expected to crash (proof the test makes the race happen).
@{
    Name = "pak-race"
    Tags = @("diag", "crash")
    SummaryRu = "Гонка в архивах движка (вылет CrySystem+0xB69C): с исправлением тест её вызывает и ловит, без вылета"
    Session = @{ Fixture = "new:rescue"; Companion = $false }
    Params = @{ Fix = 1; Rounds = 80 }
    Body = {
        param($P)
        Step "the race made $($P.Rounds) times (coop_pak_fix $($P.Fix))" {
            $fixed = Find-Log host '^\[CoopPak\] the pak reader takes a cached file only while it is alive'
            Check $true ("at the start: " + $(if ($fixed) { $fixed.Text } else { "no fix line" }))
            Send-Game host "coop_pak_fix $($P.Fix)"
            $m = New-Mark
            Send-Game host "coop_test_pakrace $($P.Rounds)"
            $over = Wait-Log host '^\[CoopPak\] race test over: (\d+) rounds, (\d+) streams read, (\d+) races caught' 300 $m
            Require ($null -ne $over) "the test ran to its end"
            $races = [int]$over.Match.Groups[3].Value
            $streams = [int]$over.Match.Groups[2].Value
            $first = Find-Log host '^\[CoopPak\] a pak file was being destroyed' $m
            Check $true ("$($over.Text)" + $(if ($first) { "; " + $first.Text } else { "" }))
            Check ($streams -ge [int]($P.Rounds / 2)) "the streams were read ($streams of $($P.Rounds))"
            if ($P.Fix) { Check ($races -ge 1) "the race happened and was caught ($races times)" }
            Pause-Game 3
        }
    }
}
