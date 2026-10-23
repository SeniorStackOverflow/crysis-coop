# The same at sys_spec 4 (Very High): before the 10-22 fix the companion never
# came in (battle dust areas of the camp fight made client-only on it while
# it loaded, see CoopAI IsNetworkedClass).
# The user's silences of 2026-10-18 and 10-21: FMOD's 64 mixed voices all
# held by nameless channels at full volume (positions under 200 ms) while the
# mix was 0. They are CryAction's voice-chat playback streams: the co-op host
# runs as a network game and CPlayer made a voice listener for every
# non-local CPlayer - every AI soldier too; each loops 200 ms of silence,
# head-relative, never stopped. The autotester never saw it: its games run
# with voice chat off, the user's with the default (on). Here voice chat is
# on (the microphone off), the user's own campaign at the camp checkpoint,
# and the user's loads: at every look the voice-chat streams of soldiers must
# be gone (at most the companion's), and no silence.
@{
    Name = "sound-voip-ai-spec4"
    Tags = @("sound", "load", "join")
    SummaryRu = "sys_spec 4 (очень высокое): голосовой чат включён (как у пользователя): у ИИ-солдат не должно быть потоков голосового чата, занимающих голоса FMOD; загрузки чекпоинтов как у пользователя - звук не пропадает"
    Session = @{ Fixture = "save:checking1021"; Campaign = "56c79107e0d7a6ff"; Companion = $true; Sound = "wav"; VoiceChat = $true; ReadyCommands = @("coop_god 1"); Extra = @("+sys_spec", "4") }
    Params = @{ MaxStreams = 2 }
    Body = {
        param($P)
        $script:looks = @()
        function Get-VoiceStreams([string]$Tag) {
            $m = New-Mark
            Send-Game host "coop_test_sound channels"
            Pause-Game 2
            $all = @(Find-Log host '^\[CoopTest\] channel \d+ ' $m -All)
            $voip = @($all | Where-Object { $_.Text -match '^\[CoopTest\] channel \d+ (\?)?: (heard|virtual)[^,]*, at \d+ of 200 ms' })
            $sum = Find-Log host '^\[CoopTest\] coop_test_sound channels : (\d+); (.*)' $m
            $line = if ($sum) { $sum.Match.Groups[2].Value } else { '' }
            $heard = @($voip | Where-Object { $_.Text -match ': heard' }).Count
            $n = $voip.Count
            # (the exact count over all channels, when the build writes it: the
            # list stops at 60 lines, the long-lived streams last, the review)
            if ($line -match 'voice-chat streams (\d+) \((\d+) heard\)') { $n = [int]$Matches[1]; $heard = [int]$Matches[2] }
            Check $true ("$Tag`: FMOD channels listed $($all.Count), voice-chat streams (nameless, 200 ms) $n, $heard of them heard; $line")
            $script:looks += [pscustomobject]@{ Tag = $Tag; N = $n; Heard = $heard }
            return $n
        }
        function Get-Silence($Mark) {
            return @(Find-Log host '^\[CoopSound\] (\d+ s without sound|no sound came out|still no sound|the sound.s master group was|the sound.s mix is not|the heard sounds stand|sounds are heard and the mix is empty|FMOD plays \d+ audible sounds)' $Mark -All | ForEach-Object { $_.Text -replace '^\[CoopSound\] ', '' })
        }
        function Invoke-Load([string]$Cmd, [string]$Tag) {
            $m = New-Mark
            Send-Game host $Cmd
            $end = Wait-Log host "^\[Coop\] checkpoint loaded: \d+ players' places kept" 300 $m
            Require ($null -ne $end) "$Tag`: the load ended"
            Send-Game host "coop_god 1"
            return $m
        }
        $start = New-Mark
        Step "at the camp (Rescue_05), voice chat on" {
            Send-Game host "coop_god 1", "coop_test_sound watch fmod"
            $vc = Find-Log host 'net_enable_voice_chat = (\$\d)?1\b'
            # (the positive control: a run with voice chat off would pass the rest)
            Require ($null -ne $vc) "voice chat is on in the host's game (net_enable_voice_chat = 1 in its log)"
            Pause-Game 25
            $first = Get-VoiceStreams "the camp, 25 s"
            # (the fingerprint's positive control: the host's stream for the
            # companion, a player, is there)
            Check ($first -ge 1) "the companion's own voice-chat stream is seen ($first)"
        }
        Step "the user's loads: the checkpoint before, back, before again" {
            $m = Invoke-Load "coop_load prev" "manual"
            Pause-Game 20
            [void](Get-VoiceStreams "manual, 20 s")
            $swapped = $null -ne (Find-Log host "is the campaign's current checkpoint now")
            $m = Invoke-Load $(if ($swapped) { "coop_load prev" } else { "coop_load" }) "Rescue_05"
            Pause-Game 25
            [void](Get-VoiceStreams "Rescue_05 again, 25 s")
            $m = Invoke-Load "coop_load prev" "manual kept"
            Pause-Game 3
            [void](Get-VoiceStreams "manual kept, 3 s")
            Pause-Game 20
            $kept = Get-VoiceStreams "manual kept, 23 s"
            # (a friend kept through the load has voice chat again)
            Check ($kept -ge 1) "the companion's voice-chat stream is there again after the load ($kept)"
        }
        Step "the verdict" {
            $most = ($script:looks | Measure-Object -Property N -Maximum).Maximum
            Check ($most -le $P.MaxStreams) "voice-chat streams at most $($P.MaxStreams) at every look (the most: $most)"
            $released = @(Find-Log host '^\[CoopSound\] voice-chat listeners of (\d+) AI soldiers released' $start -All)
            Check $true ("AI soldiers' listeners released by the safety net: " + $(if ($released.Count) { ($released | Select-Object -First 3 | ForEach-Object { $_.Text -replace '^\[CoopSound\] ', '' }) -join ' | ' } else { 'none (none made)' }))
            $silence = @(Get-Silence $start)
            Check ($silence.Count -eq 0) ("no silence: " + $(if ($silence.Count) { ($silence | Select-Object -First 3) -join ' | ' } else { 'none' }))
        }
    }
}
