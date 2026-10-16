# The d3d9.dll crash (0x12359C) came right after the level start's checkpoint
# was saved (CryAction takes a picture of the screen for a save). Here: $Times
# saves in a row with the test game's window left of all screens; the host
# must live through all of them.
@{
    Name = "save-stress"
    Tags = @("diag")
    SummaryRu = "Много сохранений подряд в тестовом окне за экраном: игра хоста не падает"
    Session = @{ Fixture = "new:rescue"; Companion = $false }
    Params = @{ Times = 15 }
    Body = {
        param($P)
        Send-Game host "coop_god 1"
        for ($i = 1; $i -le $P.Times; $i++) {
            Step "save #$i" {
                Send-Game host "coop_test_save stress$i"
                Pause-Game 4
            }
        }
        Check ((Count-Log host 'CRASH DETECTED') -eq 0) "the host lived through $($P.Times) saves"
    }
}

