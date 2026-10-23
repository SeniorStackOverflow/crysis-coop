// Crysis Coop: a watch over the game's sound (see CoopSound.h).
#include "StdAfx.h"
#include "CoopSound.h"
#include "CoopAI.h"
#include "Game.h"
#include <ISound.h>
#include <IMusicSystem.h>
#include "IAnimationGraph.h"
#include <IActorSystem.h>
#include <set>
#include <windows.h>

float CoopSoundOutputPeak(bool* pActive);	// CoopSoundMeter.cpp
bool CoopSoundOutputMuted();	// CoopSoundMeter.cpp
bool CoopFmodChannels(void* pSystem, int maxChannels, int* pPlaying, int* pVirtual, int* pSoftware, float cpu[4]);	// CoopSoundMeter.cpp
int CoopFmodChannelList(void* pSystem, int maxChannels, const char* filter, int maxLines, void (*pLine)(const char*));	// CoopSoundMeter.cpp
int CoopFmodVoiceStreams(void* pSystem, int maxChannels, int* pHeard);	// CoopSoundMeter.cpp
struct SCoopFmodWatch
{
	int stuck, stopped, late, takenAway, started, startedHeard;
	int lostLoudness[3];	// the ones that lost their voice: audibility under 0.02, under 0.1, louder
	char lostNames[256];	// the first loud ones (testing)
	float latest;
	char names[256];
	int gunStopped, gunNew;	// gunfire loops that played on too long: stopped (gunNew: since the caller looked)
	char gunNames[192];
};
void CoopFmodWatch(void* pSystem, int maxChannels, bool stop, float now, SCoopFmodWatch* pOut);	// CoopSoundMeter.cpp
struct SCoopFmodSnapshot
{
	float mixPeak;
	int mixNaN;
	bool mixSame;
	float masterVolume;
	bool masterPaused, masterMute;
	float masterPeak;
	int groups;
	char groupsOff[192];
	int playing, heard, audible, pausedAudible;
	int moving, frozen;
	char top[200];
};
bool CoopFmodSnapshot(void* pSystem, int maxChannels, SCoopFmodSnapshot* pOut);	// CoopSoundMeter.cpp
int CoopFmodRepairGroups(void* pSystem, int fault, char* what, size_t size);	// CoopSoundMeter.cpp
float CoopFmodMixPeak(void* pSystem);	// CoopSoundMeter.cpp
bool CoopWindowFocused(void* hwnd);	// CoopSoundMeter.cpp

namespace CoopSound
{
	void* FmodSystem(int* pMaxChannels);

	namespace
	{
		const float CHECK_EVERY = 1.0f;		// s between looks at the output
		// so many silent looks in a row: no sound comes out (15 when one 10 ms
		// meter reading a second could miss sparse sounds; now the loudest of
		// four a second, and FMOD's own picture catches its faults in 1-3 s)
		const int SILENT_LOOKS = 10;
		const float RESTART_EVERY = 120.0f;	// s at least between two restarts
		const float DEFINITE_EVERY = 30.0f;	// s at least between two restarts for a fault FMOD itself shows
		const float LOUD = 0.0005f;			// a peak above this is sound

		float s_nextLook = 0.0f, s_lastLook = -100.0f;
		int s_silent = 0;
		bool s_heard = false;
		float s_restartAt = -1000.0f, s_onAgainAt = -1.0f, s_checkAt = -1.0f;
		bool s_anyFocus = false;	// testing: the test windows never have the focus
		bool s_fmodMeter = false;	// testing: no Windows session (sound to a file): FMOD's own mix is the meter
		// the Windows meter 4 times a second, the loudest kept for the second's look
		float s_peekNext = 0.0f, s_peakMax = -1.0f;
		bool s_peakActive = false;
		// FMOD's own picture (CoopFmodSnapshot): looks in a row with a fault
		int s_nanLooks = 0, s_frozenLooks = 0, s_emptyLooks = 0, s_noWindowsLooks = 0, s_masterLooks = 0, s_heardSilentLooks = 0;
		float s_testBlackoutUntil = -1.0f;	// testing: till then the meter reads a session playing silence
		float s_masterFixAt = -1000.0f, s_definiteRestartAt = -1000.0f;
		float s_testUnfocusedUntil = -1.0f;	// testing: the window counts as not focused till then
		float s_mixSinceRestart = 0.0f;	// FMOD's loudest mix seen since the last restart

		void ResetLooks()
		{
			s_nanLooks = s_frozenLooks = s_emptyLooks = s_noWindowsLooks = s_masterLooks = s_heardSilentLooks = 0;
		}

		// the game's window has the focus (CGame::Update's haveFocus is always
		// true: CGameStartup passes 1. CrySound pauses FMOD's master group
		// itself while the window is not focused, unless in devmode: that is
		// no fault)
		bool WindowFocused(float now)
		{
			if (now < s_testUnfocusedUntil)
				return false;
			return s_anyFocus || CoopWindowFocused(gEnv->pRenderer ? (void*)gEnv->pRenderer->GetHWND() : 0);
		}

		string SnapshotLine(const SCoopFmodSnapshot& s)
		{
			string nan;
			if (s.mixNaN)
				nan.Format(" (%d not a number)", s.mixNaN);
			int maxChannels = 0, voiceHeard = 0;
			void* pSystem = FmodSystem(&maxChannels);
			const int voice = pSystem ? CoopFmodVoiceStreams(pSystem, maxChannels, &voiceHeard) : 0;
			return string().Format("mix peak %.4f%s%s; master volume %.2f%s%s, peak %.4f; %d groups%s%s; channels %d playing, %d heard, %d audible (%d moving on, %d standing, %d more paused); voice-chat streams %d (%d heard); loudest:%s",
				s.mixPeak, nan.c_str(), s.mixSame ? " (the same samples as a second ago)" : "", s.masterVolume, s.masterPaused ? " PAUSED" : "", s.masterMute ? " MUTED" : "",
				s.masterPeak, s.groups, s.groupsOff[0] ? ", off:" : "", s.groupsOff, s.playing, s.heard, s.audible, s.moving, s.frozen, s.pausedAudible, voice, voiceHeard, s.top[0] ? s.top : " -");
		}

		// the sound system off and on again (0.3 s)
		void Restart(float now)
		{
			s_mixSinceRestart = 0.0f;
			s_restartAt = now;
			s_silent = 0;
			gEnv->pConsole->ExecuteString("s_SoundEnable 0");
			s_onAgainAt = now + 0.3f;
		}

		// testing: a sound played at the player and followed every frame for up to
		// 15 s: when it was heard, how often FMOD took its voice away (virtual: only
		// 64 voices are mixed) and gave it back. The user's sound was late and
		// choppy with 120-180 voices (2026-10-14)
		_smart_ptr<ISound> s_probe;
		string s_probeName;
		float s_probeAt = -1.0f, s_probeLast = 0.0f, s_probeHeardAt = -1.0f, s_probeHeardMs = 0.0f;
		int s_probeLost = 0, s_probeVirtual = 0, s_probeLoading = 0, s_probeVoices = 0;
		bool s_probeWasHeard = false;
		string s_probeTrack;	// whether it still played at 1, 2, 4 and 8 s
		int s_probeMark = 0;

		void StartProbe(const char* name, bool foley, float distance)
		{
			IActor* pActor = g_pGame && g_pGame->GetIGameFramework() ? g_pGame->GetIGameFramework()->GetClientActor() : 0;
			if (!pActor || !name || !*name)
				return;
			if (s_probe)
				s_probe->Stop();
			s_probe = gEnv->pSoundSystem->CreateSound(name, FLAG_SOUND_DEFAULT_3D);
			if (!s_probe)
			{
				CryLogAlways("[CoopTest] probe %s: no such sound", name);
				return;
			}
			s_probe->SetSemantic(foley ? eSoundSemantic_Player_Foley : eSoundSemantic_Living_Entity);
			s_probe->SetPosition(pActor->GetEntity()->GetWorldPos() + Vec3(distance, 0.0f, 1.5f));
			s_probeName = name;
			s_probeAt = s_probeLast = gEnv->pTimer->GetAsyncCurTime();
			s_probeHeardAt = -1.0f;
			s_probeHeardMs = 0.0f;
			s_probeLost = s_probeVirtual = s_probeLoading = 0;
			s_probeWasHeard = false;
			s_probeTrack = "";
			s_probeMark = 0;
			s_probeVoices = gEnv->pSoundSystem->GetUsedVoices();
			s_probe->Play();
		}

		void FollowProbe();
	}

	// FMOD's channels (CoopFmodChannels): CrySoundSystem's audio device keeps
	// FMOD's System at +0x34 and the channels FMOD was started with at +0x64
	// (CrySoundSystem 6156: GetUsedVoices and the init read them there)
	// FMOD's System and its channel count, 0 without real sound (the AI
	// companion's game runs CrySound's dummy sound system)
	void* FmodSystem(int* pMaxChannels)
	{
		ICVar* pEnable = gEnv->pConsole->GetCVar("s_SoundEnable");
		ICVar* pDummy = gEnv->pConsole->GetCVar("s_DummySound");
		IAudioDevice* pDevice = gEnv->pSoundSystem ? gEnv->pSoundSystem->GetIAudioDevice() : 0;
		if (!pDevice || (pEnable && pEnable->GetIVal() == 0) || (pDummy && pDummy->GetIVal() != 0))
			return 0;
		*pMaxChannels = *(const int*)((const char*)pDevice + 0x64);
		return *(void**)((char*)pDevice + 0x34);
	}

	string FmodLine()
	{
		int maxChannels = 0;
		void* pSystem = FmodSystem(&maxChannels);
		if (!pSystem)
			return "FMOD: no sound";
		int playing = 0, isVirtual = 0, software = 0;
		float cpu[4];
		if (!CoopFmodChannels(pSystem, maxChannels, &playing, &isVirtual, &software, cpu))
			return string().Format("FMOD: no answer (%d channels)", maxChannels);
		// (voice-chat streams: one per soldier took every voice, see
		// CPlayer::CreateVoiceListener)
		int voiceHeard = 0;
		const int voice = CoopFmodVoiceStreams(pSystem, maxChannels, &voiceHeard);
		return string().Format("FMOD: %d channels playing (%d heard, %d virtual) of %d, %d mixed at most; cpu %.1f%% (dsp %.1f, stream %.1f, update %.1f); voice-chat streams %d (%d heard)",
			playing, playing - isVirtual, isVirtual, maxChannels, software, cpu[3], cpu[0], cpu[1], cpu[2], voice, voiceHeard);
	}

	// FMOD's one-shot channels stuck virtual at their start (CoopFmodWatch):
	// stopped (coop_sound_unstick), and every minute a line in the log with
	// how many there were, how many came late anyway and how often a heard
	// sound lost its voice in the middle
	float s_watchAt = 0.0f, s_watchLogAt = -1.0f;
	SCoopFmodWatch s_watch;

	void Unstick(float now)
	{
		if (now < s_watchAt)
			return;
		s_watchAt = now + 0.25f;
		ICVar* pVar = gEnv->pConsole->GetCVar("coop_sound_unstick");
		IGameFramework* pFramework = g_pGame ? g_pGame->GetIGameFramework() : 0;
		int maxChannels = 0;
		void* pSystem = 0;
		if (!pFramework || !pFramework->GetClientActor() || !(pSystem = FmodSystem(&maxChannels)))
			return;
		if (s_watchLogAt < 0.0f)
		{
			memset(&s_watch, 0, sizeof(s_watch));
			s_watchLogAt = now;
		}
		const bool stop = !pVar || pVar->GetIVal() != 0;
		CoopFmodWatch(pSystem, maxChannels, stop, now, &s_watch);
		if (s_watch.gunNew > 0)
		{
			s_watch.gunNew = 0;
			CryLogAlways("[CoopSound] a gunfire loop played on with nobody firing that long: stopped (%d so far:%s)", s_watch.gunStopped, s_watch.gunNames);
		}
		if (now - s_watchLogAt < 60.0f)
			return;
		if (s_watch.stuck > 0 || s_watch.late > 0 || s_watch.takenAway > 0)
			CryLogAlways("[CoopSound] last minute: %d sound parts stuck at their start (virtual), %d stopped%s%s; %d came late (0.5 s or more, the latest %.1f s); audible sounds lost their voice midway %d times (audibility under 0.02: %d, under 0.1: %d, louder: %d%s%s); one-shots heard at once %d of %d; %s",
				s_watch.stuck, s_watch.stopped, s_watch.stopped ? ":" : "", s_watch.names, s_watch.late, s_watch.latest, s_watch.takenAway,
				s_watch.lostLoudness[0], s_watch.lostLoudness[1], s_watch.lostLoudness[2], s_watch.lostNames[0] ? ":" : "", s_watch.lostNames, s_watch.startedHeard, s_watch.started, FmodLine().c_str());
		memset(&s_watch, 0, sizeof(s_watch));
		s_watchLogAt = now;
	}

	namespace
	{
		void FollowProbe()
		{
			if (!s_probe)
				return;
			const float now = gEnv->pTimer->GetAsyncCurTime();
			const float ms = (now - s_probeLast) * 1000.0f;
			s_probeLast = now;
			const bool loading = s_probe->IsLoading(), isVirtual = s_probe->IsPlayingVirtual();
			const bool heard = s_probe->IsPlaying() && !isVirtual;
			s_probeLoading += loading;
			s_probeVirtual += isVirtual;
			if (heard)
			{
				if (s_probeHeardAt < 0.0f)
					s_probeHeardAt = now;
				else
					s_probeHeardMs += ms;
			}
			else if (s_probeWasHeard && isVirtual)
				++s_probeLost;
			s_probeWasHeard = heard;
			static const float marks[] = { 1.0f, 2.0f, 4.0f, 8.0f };
			if (s_probeMark < 4 && now - s_probeAt >= marks[s_probeMark])
				s_probeTrack += string().Format(" %.0fs:%s", marks[s_probeMark++], heard ? "heard" : isVirtual ? "virtual" : loading ? "loading" : "over");
			const bool over = !loading && !isVirtual && !heard;
			if (now - s_probeAt < 15.0f && !(over && now - s_probeAt > 0.2f))
				return;
			CryLogAlways("[CoopTest] probe %s: heard %s%.0f ms, heard %.0f ms of %.0f, voice taken away %d times; frames virtual %d, loading %d; %d voices at the start, %d now; length %d ms, at%s",
				s_probeName.c_str(), s_probeHeardAt < 0.0f ? "never " : "after ", s_probeHeardAt < 0.0f ? 0.0f : (s_probeHeardAt - s_probeAt) * 1000.0f,
				s_probeHeardMs, (now - s_probeAt) * 1000.0f, s_probeLost, s_probeVirtual, s_probeLoading, s_probeVoices, gEnv->pSoundSystem->GetUsedVoices(),
				s_probe->GetLengthMs(), s_probeTrack.c_str());
			CryLogAlways("[CoopTest] probe %s: %s", s_probeName.c_str(), FmodLine().c_str());
			s_probe->Stop();
			s_probe = 0;
		}

		// coop_test_sound <pause|mute|unmute|watch>: the sound system paused or
		// muted as if by a fault; watch: the watch also without the focus
		void CmdTestSound(IConsoleCmdArgs* pArgs)
		{
			const char* what = pArgs->GetArgCount() > 1 ? pArgs->GetArg(1) : "";
			if (!gEnv->pSoundSystem)
				return;
			if (!stricmp(what, "channels"))
			{
				// FMOD's playing channels (their sound's name containing the filter)
				struct Log { static void Line(const char* text) { CryLogAlways("[CoopTest] %s", text); } };
				int maxChannels = 0;
				void* pSystem = FmodSystem(&maxChannels);
				const int n = pSystem ? CoopFmodChannelList(pSystem, maxChannels, pArgs->GetArgCount() > 2 ? pArgs->GetArg(2) : "", 60, &Log::Line) : 0;
				CryLogAlways("[CoopTest] coop_test_sound channels %s: %d; %s", pArgs->GetArgCount() > 2 ? pArgs->GetArg(2) : "", n, FmodLine().c_str());
				return;
			}
			if (!stricmp(what, "playloop"))
			{
				// playloop <sound event> | stop: a weapon's shot played 5 m
				// from this game's player and never told to stop (a fire loop
				// left playing: the watch has to end it)
				static _smart_ptr<ISound> s_loop;
				const char* name = pArgs->GetArgCount() > 2 ? pArgs->GetArg(2) : "";
				if (s_loop)
				{
					s_loop->Stop(ESoundStopMode_AtOnce);
					s_loop = 0;
				}
				IActor* pActor = g_pGame && g_pGame->GetIGameFramework() ? g_pGame->GetIGameFramework()->GetClientActor() : 0;
				if (!pActor || !*name || !stricmp(name, "stop"))
					return;
				s_loop = gEnv->pSoundSystem->CreateSound(name, FLAG_SOUND_DEFAULT_3D);
				if (!s_loop)
				{
					CryLogAlways("[CoopTest] playloop %s: no such sound", name);
					return;
				}
				s_loop->SetSemantic(eSoundSemantic_Living_Entity);
				s_loop->SetPosition(pActor->GetEntity()->GetWorldPos() + Vec3(5.0f, 0.0f, 1.5f));
				// (as an automatic weapon's fire: the shot repeats until stopped)
				s_loop->SetLoopMode(true);
				s_loop->Play();
				CryLogAlways("[CoopTest] playloop %s: playing, never stopped", name);
				return;
			}
			if (!stricmp(what, "probe"))
			{
				// probe <sound> [foley|shot] [metres away]
				StartProbe(pArgs->GetArgCount() > 2 ? pArgs->GetArg(2) : "", pArgs->GetArgCount() > 3 && !stricmp(pArgs->GetArg(3), "foley"),
					pArgs->GetArgCount() > 4 ? (float)atof(pArgs->GetArg(4)) : 0.0f);
				return;
			}
			if (!stricmp(what, "snapshot") || !stricmp(what, "fmodpause") || !stricmp(what, "fmodmute"))
			{
				// snapshot: FMOD's own picture of the output now; fmodpause /
				// fmodmute [unfocused]: FMOD's master group paused / muted behind
				// the game's back, as a fault would (the watch has to set it
				// right); unfocused: the window counts as not focused for 8 s, as
				// when CrySound pauses it on a focus loss (the watch leaves it)
				int maxChannels = 0;
				void* pSystem = FmodSystem(&maxChannels);
				SCoopFmodSnapshot snap;
				if (!pSystem || !CoopFmodSnapshot(pSystem, maxChannels, &snap))
				{
					CryLogAlways("[CoopTest] coop_test_sound %s: FMOD gives no picture (%s)", what, FmodLine().c_str());
					return;
				}
				if (stricmp(what, "snapshot"))
				{
					// (fmodpause unfocused: as CrySound does when the window
					// loses the focus; the watch must leave it alone)
					if (pArgs->GetArgCount() > 2 && !stricmp(pArgs->GetArg(2), "unfocused"))
						s_testUnfocusedUntil = gEnv->pTimer->GetAsyncCurTime() + 8.0f;
					char text[160];
					const int n = CoopFmodRepairGroups(pSystem, !stricmp(what, "fmodpause") ? 1 : 2, text, sizeof(text));
					CryLogAlways("[CoopTest] coop_test_sound %s: the master group %s", what, n ? "set as by a fault" : "NOT set");
					return;
				}
				CryLogAlways("[CoopTest] coop_test_sound snapshot: %s; %s", SnapshotLine(snap).c_str(), FmodLine().c_str());
				return;
			}
			if (!stricmp(what, "pause"))
				gEnv->pSoundSystem->Pause(true);
			else if (!stricmp(what, "mute"))
				gEnv->pSoundSystem->Mute(true);
			else if (!stricmp(what, "unmute"))
				gEnv->pSoundSystem->Mute(false);
			else if (!stricmp(what, "blackout"))
			{
				// blackout [s]: for so long nothing comes out as Windows sees it
				// (a session playing silence) while FMOD goes on: the watch's
				// "heard in FMOD, silent in Windows" rule must restart the sound
				s_testBlackoutUntil = gEnv->pTimer->GetAsyncCurTime() + (pArgs->GetArgCount() > 2 ? (float)atof(pArgs->GetArg(2)) : 8.0f);
			}
			else if (!stricmp(what, "watch"))
			{
				s_anyFocus = true;
				// (watch fmod: the meter is FMOD's own mix where Windows has no
				// session to ask, sound to a WAV file)
				s_fmodMeter = pArgs->GetArgCount() > 2 && !stricmp(pArgs->GetArg(2), "fmod");
			}
			else if (IListener* pListener = gEnv->pSoundSystem->GetListener(LISTENERID_STANDARD))
			{
				// one frame of a broken listener (the game sets it again every frame)
				const float nan = sqrtf(-1.0f + 0.0f * (float)pArgs->GetArgCount());
				if (!stricmp(what, "nanpos"))
					pListener->SetPosition(Vec3(nan, nan, nan));
				else if (!stricmp(what, "nanvel"))
					pListener->SetVelocity(Vec3(nan, nan, nan));
				else if (!stricmp(what, "nanwater"))
					pListener->SetUnderwater(nan);
				else if (!stricmp(what, "bigvel"))
					pListener->SetVelocity(Vec3(1e7f, 0, 0));
				else if (!stricmp(what, "underwater"))
					pListener->SetUnderwater(-5.0f);
			}
			bool active = false;
			CryLogAlways("[CoopTest] coop_test_sound %s: paused %d, %d voices, output peak %.3f", what,
				(int)gEnv->pSoundSystem->IsPaused(), gEnv->pSoundSystem->GetUsedVoices(), CoopSoundOutputPeak(&active));
		}
	}

	// the music: its theme and mood whenever they change (it was missing in
	// coop fights), single player too for comparison
	string s_musicTheme = "?", s_musicMood = "?", s_musicMode = "?";
	float s_musicAt = 0.0f;

	// the music logic's graph (CryAction): Mode running / pauseAll and what
	// it chooses the mood by
	string MusicLogicState(string& mode)
	{
		mode = "-";
		IAnimationGraphState* pGraph = g_pGame && g_pGame->GetIGameFramework() ? g_pGame->GetIGameFramework()->GetMusicGraphState() : 0;
		if (!pGraph)
			return "";
		char buf[256] = "";
		pGraph->GetInput(pGraph->GetInputId("Mode"), buf);
		buf[sizeof(buf) - 1] = 0;
		mode = buf;
		string s;
		s.Format("logic %s, AI %.0f, player %.0f, game %.0f, music played %.0f s", buf, pGraph->GetInputAsFloat(pGraph->GetInputId("AI_Intensity")),
			pGraph->GetInputAsFloat(pGraph->GetInputId("Player_Intensity")), pGraph->GetInputAsFloat(pGraph->GetInputId("Game_Intensity")),
			pGraph->GetInputAsFloat(pGraph->GetInputId("PlayingTime_Music")));
		return s;
	}

	void WatchMusic()
	{
		const float now = gEnv->pTimer->GetAsyncCurTime();
		if (now - s_musicAt < 1.0f)
			return;
		s_musicAt = now;
		IMusicSystem* pMusic = gEnv->pSystem->GetIMusicSystem();
		if (!pMusic)
			return;
		const char* theme = pMusic->GetTheme();
		const char* mood = pMusic->GetMood();
		const string t = theme ? theme : "", m = mood ? mood : "";
		string mode;
		const string logic = MusicLogicState(mode);
		if (t == s_musicTheme && m == s_musicMood && mode == s_musicMode)
			return;
		s_musicTheme = t;
		s_musicMood = m;
		s_musicMode = mode;
		SMusicSystemStatus* pStatus = pMusic->GetStatus();
		CryLogAlways("[CoopMusic] theme \"%s\", mood \"%s\"%s; %s", t.c_str(), m.c_str(), pStatus && pStatus->bPlaying ? ", playing" : ", not playing",
			logic.c_str());
	}

	void Update(bool /*haveFocus: always true, see WindowFocused*/)
	{
		if (!gEnv || !gEnv->pTimer || !gEnv->pConsole || gEnv->pSystem->IsDedicated())
			return;
		WatchMusic();
		static bool s_registered = false;
		if (!s_registered)
		{
			s_registered = true;
			gEnv->pConsole->RegisterInt("coop_sound_unstick", 1, 0, "Crysis Coop: stop the parts of sounds stuck virtual at their start for 0.4 s (they kept their sound playing and came seconds late); 0 = leave them");
			gEnv->pConsole->AddCommand("coop_test_sound", CmdTestSound, 0, "Crysis Coop testing: coop_test_sound <pause|mute|unmute|fmodpause|fmodmute [unfocused]|watch|snapshot|nanpos|nanvel|nanwater|bigvel|underwater>: the sound system paused (muted), FMOD's master group paused (muted) behind the game's back, or its listener broken for a frame, as if by a fault; watch: the sound watch also without the window's focus; snapshot: FMOD's picture of the output; probe <sound> [foley|shot] [metres]: a sound at the player (or so far away), when it is heard and how often its voice is taken away");
		}
		const float now = gEnv->pTimer->GetAsyncCurTime();
		if (gEnv->pSoundSystem)
		{
			FollowProbe();
			Unstick(now);
		}

		// the second half of a restart: on again
		if (s_onAgainAt > 0.0f && now >= s_onAgainAt)
		{
			s_onAgainAt = -1.0f;
			gEnv->pConsole->ExecuteString("s_SoundEnable 1");
			s_checkAt = now + 5.0f;
		}

		ICVar* pEnable = gEnv->pConsole->GetCVar("s_SoundEnable");
		IGameFramework* pFramework = g_pGame ? g_pGame->GetIGameFramework() : 0;
		const bool watching = gEnv->pSoundSystem && pEnable && pEnable->GetIVal() != 0 && WindowFocused(now) && s_onAgainAt <= 0.0f
			&& pFramework && pFramework->GetClientActor() && !pFramework->IsGamePaused() && CoopAI::IsCoopSession();
		// the Windows meter 4 times a second (a 10 ms window once a second
		// missed sparse sounds), the loudest kept for the second's look
		if (watching && now >= s_peekNext)
		{
			s_peekNext = now + 0.25f;
			bool active = false;
			float peek = CoopSoundOutputPeak(&active);
			if (peek < 0.0f && s_fmodMeter)
			{
				int maxChannels = 0;
				if (void* pSystem = FmodSystem(&maxChannels))
				{
					peek = CoopFmodMixPeak(pSystem);
					active = true;
				}
			}
			if (now < s_testBlackoutUntil)
			{
				peek = 0.0f;
				active = true;
			}
			if (peek > s_peakMax)
				s_peakMax = peek;
			s_peakActive = s_peakActive || active;
		}

		if (now < s_nextLook)
			return;
		s_nextLook = now + CHECK_EVERY;
		// looks far apart (a level loading, a load) do not follow each other
		if (now - s_lastLook > 3.0f * CHECK_EVERY)
		{
			s_silent = 0;
			ResetLooks();
		}
		s_lastLook = now;
		const float peak = s_peakMax;
		const bool active = s_peakActive;
		s_peakMax = -1.0f;
		s_peakActive = false;
		if (!watching)
		{
			s_silent = 0;
			ResetLooks();
			return;
		}

		// FMOD's own picture: a fault that cannot be right (the master group
		// paused or muted behind the game's back, a mix that is not a number,
		// heard sounds that stand still, heard sounds and an empty mix) is set
		// right or restarted after 1-3 s, and never when nothing is expected
		ISoundSystem* pSound = gEnv->pSoundSystem;
		int maxChannels = 0;
		void* pSystem = FmodSystem(&maxChannels);
		SCoopFmodSnapshot snap;
		const bool haveSnap = pSystem && CoopFmodSnapshot(pSystem, maxChannels, &snap);
		if (haveSnap && s_checkAt > 0.0f && snap.mixPeak > s_mixSinceRestart)
			s_mixSinceRestart = snap.mixPeak;
		// (Windows hears nothing from this game this second, or there is no
		// audio session to ask: a WAV file. The rules below need it)
		const bool windowsQuiet = peak <= LOUD;
		if (haveSnap && !pSound->IsPaused())
		{
			// (two looks in a row: CrySound's own unpause on the focus coming
			// back may come a frame late)
			s_masterLooks = snap.masterPaused || snap.masterMute ? s_masterLooks + 1 : 0;
			if (s_masterLooks >= 2)
			{
				s_masterLooks = 0;
				// CrySound's own way back first (it unpauses the master group and
				// the event system's master category on the focus coming back;
				// not in devmode), then the master group itself
				if (ISystemEventDispatcher* pDispatcher = gEnv->pSystem->GetISystemEventDispatcher())
					pDispatcher->OnSystemEvent(ESYSTEM_EVENT_CHANGE_FOCUS, 1, 0);
				char text[160];
				const int changed = CoopFmodRepairGroups(pSystem, 0, text, sizeof(text));
				CryLogAlways("[CoopSound] the sound's master group was%s%s behind the game's back: %s (%s)", snap.masterPaused ? " paused" : "",
					snap.masterMute ? " muted" : "", changed ? "set right:" : "set right by the focus event", changed ? text + 1 : SnapshotLine(snap).c_str());
				if (now - s_masterFixAt < 20.0f && now - s_definiteRestartAt > DEFINITE_EVERY)
				{
					CryLogAlways("[CoopSound] the master group was set wrong again within 20 s: the sound system starts again");
					s_definiteRestartAt = now;
					Restart(now);
					return;
				}
				s_masterFixAt = now;
			}
			s_nanLooks = snap.mixNaN > 0 ? s_nanLooks + 1 : 0;
			s_frozenLooks = windowsQuiet && snap.audible > 0 && snap.moving == 0 && snap.frozen > 0 ? s_frozenLooks + 1 : 0;
			s_emptyLooks = windowsQuiet && snap.audible >= 2 && snap.mixPeak >= 0.0f && snap.mixPeak < 1e-5f && !snap.masterPaused && !snap.masterMute
				&& snap.masterVolume > 0.01f ? s_emptyLooks + 1 : 0;
			const char* why = s_nanLooks >= 2 ? "the sound's mix is not a number" : s_frozenLooks >= 3 ? "the heard sounds stand still for 3 s"
				: s_emptyLooks >= 3 ? "sounds are heard and the mix is empty for 3 s" : 0;
			if (why && now - s_definiteRestartAt > DEFINITE_EVERY)
			{
				CryLogAlways("[CoopSound] %s: the sound system starts again (%s)", why, SnapshotLine(snap).c_str());
				s_definiteRestartAt = now;
				s_nanLooks = s_frozenLooks = s_emptyLooks = 0;
				Restart(now);
				return;
			}
			// sounds FMOD hears (two or more, audibility 0.02 and over: -34 dB,
			// the meter's limit is -66) while this game's Windows session plays
			// silence, 4 looks of 4 readings each: the user's 15 s of silence
			// in a fight with 81 channels playing (2026-10-18), ended by a
			// restart. Not when the player turned the game down in Windows
			s_heardSilentLooks = peak >= 0.0f && peak <= LOUD && active && snap.audible >= 2 && !snap.masterPaused && !snap.masterMute
				&& snap.masterVolume > 0.01f ? s_heardSilentLooks + 1 : 0;
			if (s_heardSilentLooks >= 4 && now - s_definiteRestartAt > DEFINITE_EVERY)
			{
				if (now >= s_testBlackoutUntil && CoopSoundOutputMuted())
					s_heardSilentLooks = 0;
				else
				{
					CryLogAlways("[CoopSound] FMOD plays %d audible sounds and nothing comes out of this game for 4 s: the sound system starts again (%s)",
						snap.audible, SnapshotLine(snap).c_str());
					s_definiteRestartAt = now;
					s_heardSilentLooks = 0;
					Restart(now);
					return;
				}
			}
			// FMOD mixes sound, Windows gets none from this game: the fault is
			// after FMOD (the device or the session)
			if (snap.mixPeak > 0.01f && peak >= 0.0f && peak <= LOUD)
			{
				if (++s_noWindowsLooks == 3)
					CryLogAlways("[CoopSound] FMOD mixes sound (peak %.3f) but Windows gets none from this game for 3 s: the fault is after FMOD (%s)",
						snap.mixPeak, active ? "the session plays silence" : "the session is stopped");
			}
			else
				s_noWindowsLooks = 0;
		}
		else
			ResetLooks();

		if (peak > LOUD)
		{
			s_heard = true;
			s_silent = 0;
			if (s_checkAt > 0.0f)
			{
				CryLogAlways("[CoopSound] the sound comes out again after the restart (peak %.3f)", peak);
				s_checkAt = -1.0f;
			}
			return;
		}
		if (s_checkAt > 0.0f && now > s_checkAt)
		{
			// (no Windows session to ask, sound to a file: FMOD's own mix tells)
			if (peak < 0.0f && s_mixSinceRestart > 0.001f)
				CryLogAlways("[CoopSound] the sound comes out again after the restart (FMOD's mix peak %.3f; no audio session to ask)", s_mixSinceRestart);
			else
				CryLogAlways("[CoopSound] still no sound 5 s after the restart (%s; FMOD's loudest mix since %.4f)", peak < 0.0f ? "no audio session" : active ? "session playing silence" : "session stopped",
					s_mixSinceRestart);
			s_checkAt = -1.0f;
		}

		++s_silent;
		// (nothing heard in FMOD either, a quiet place: the old 15 s)
		const int silentLooks = haveSnap && snap.audible == 0 ? 15 : SILENT_LOOKS;
		// (what FMOD shows at 3 s of silence and just before the restart)
		if ((s_silent == 3 || s_silent == silentLooks - 1) && s_heard && haveSnap)
			CryLogAlways("[CoopSound] %d s without sound from this game (%s): %s", s_silent,
				peak < 0.0f ? "no audio session" : active ? "session playing silence" : "session stopped", SnapshotLine(snap).c_str());
		// only a game that was heard before (no sound device, the volume mixer's mute...)
		if (s_silent >= silentLooks && s_heard && now - s_restartAt > RESTART_EVERY)
		{
			int memory = 0, memoryMax = 0;
			pSound->GetSoundMemoryUsageInfo(&memory, &memoryMax);
			const bool paused = pSound->IsPaused();
			CryLogAlways("[CoopSound] no sound came out for %d s (%s; sound system %s, %d voices, cpu %.1f, memory %d of %d): %s",
				s_silent, peak < 0.0f ? "no audio session" : active ? "session playing silence" : "session stopped",
				paused ? "PAUSED" : "running", pSound->GetUsedVoices(), pSound->GetCPUUsage(), memory, memoryMax,
				paused ? "it goes on" : "the sound system starts again");
			if (paused)
			{
				s_restartAt = now;
				s_silent = 0;
				pSound->Pause(false);
				s_checkAt = now + 5.0f;
				s_mixSinceRestart = 0.0f;
				return;
			}
			Restart(now);
		}
	}
}

// The engine's sound system did not start (s_SoundEnable 0 - the AI
// companion's game, the autotester's - or no sound device that works):
// CrySystem deleted its CSoundSystem ("Cannot initialize SoundSystem!") and
// made a dummy one, but the console variables it registered with its own
// memory still point into that freed memory. Setting one wrote into MSVCR80's
// small-block heap, and the game crashed later in MSVCR80: a spec change (the
// first start's spec detection, the companion's +sys_spec 1: sys_spec_Sound
// sets nine of them), the mod's sound limits, the host's values a joining
// client takes (CryAction sets the server's s_PriorityThreshold and
// s_MaxActiveSounds and disconnects a client that does not take them)
// (2026-10-22). Each one gets memory of its own: the console's object stays
// (the engine keeps pointers to them), its pointer is moved; one that cannot
// be moved is not changed any more
namespace
{
	struct SFailedSoundCvars : public IConsoleVarSink
	{
		std::set<ICVar*> dead;	// still pointing into the freed memory
		std::set<string> told;
		bool on;
		SFailedSoundCvars() : on(false) {}

		virtual bool OnBeforeVarChange(ICVar* pVar, const char* sNewValue)
		{
			if (!dead.count(pVar))
				return true;
			if (told.insert(pVar->GetName()).second)
				CryLogAlways("[CoopSound] no sound system: %s stays (\"%s\" refused: it still points into the failed sound system's freed memory)",
					pVar->GetName(), sNewValue ? sNewValue : "");
			return false;
		}
		virtual void OnAfterVarChange(ICVar* pVar) {}
	};
	SFailedSoundCvars s_failedSoundCvars;

	// memory for the moved values: the process heap's (Coop.dll's own CRT heap
	// goes with the DLL, the console outlives it), never freed
	template <class T>
	T* KeepMemory()
	{
		return (T*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(T));
	}

	// a kind of console variable CrySystem makes: its class (the vtable) and,
	// for one with the caller's memory, the place in it of that pointer -
	// learnt from one of the mod's own (exactly one place holds the address)
	struct SVarKind
	{
		void* vtable;
		int offset;
	};

	void* VTable(ICVar* pVar)
	{
		return pVar ? *(void**)pVar : 0;
	}

	template <class T>
	SVarKind LearnRefKind(const char* name, T value)
	{
		SVarKind kind = { 0, -1 };
		T* pProbe = KeepMemory<T>();
		if (!pProbe)
			return kind;
		*pProbe = value;
		ICVar* pVar = gEnv->pConsole->Register(name, pProbe, value, 0, "Crysis Coop: the console's kind of variable with its own memory (testing it)");
		if (!pVar)
			return kind;
		kind.vtable = VTable(pVar);
		int found = 0;
		// (CrySystem makes them 0x1c bytes: the vtable, then the pointer at 0x18)
		for (int k = 4; k < 0x1c; k += 4)
			if (*(T**)((char*)pVar + k) == pProbe)
			{
				kind.offset = k;
				++found;
			}
		if (found != 1)
			kind.offset = -1;
		return kind;
	}

	// the variable reads its new memory: a value written there is what it gives
	bool ReadsFrom(ICVar* pVar, int* p)
	{
		const int was = *p;
		*p = was ^ 0x5A5A;
		const bool ok = pVar->GetIVal() == *p;
		*p = was;
		return ok;
	}
	bool ReadsFrom(ICVar* pVar, float* p)
	{
		const float was = *p;
		*p = was == 1234.5f ? 4321.5f : 1234.5f;
		const bool ok = pVar->GetFVal() == *p;
		*p = was;
		return ok;
	}

	// moves one of the caller's-memory kind to memory of its own
	template <class T>
	bool MoveToOwnMemory(ICVar* pVar, const SVarKind& kind)
	{
		if (!kind.vtable || kind.offset < 0 || VTable(pVar) != kind.vtable)
			return false;
		T** pRef = (T**)((char*)pVar + kind.offset);
		T* pOld = *pRef;
		T* pNew = KeepMemory<T>();
		if (!pOld || !pNew)
			return false;
		*pNew = *pOld;	// the value it had (read from the freed memory)
		*pRef = pNew;
		if (ReadsFrom(pVar, pNew))
			return true;
		*pRef = pOld;
		return false;
	}

	void MoveFailedSoundCvars()
	{
		IConsole* pConsole = gEnv->pConsole;
		const SVarKind intRef = LearnRefKind<int>("coop_sound_probe_int", 7);
		const SVarKind floatRef = LearnRefKind<float>("coop_sound_probe_float", 7.0f);
		// (the console's own memory: nothing freed under them)
		const void* own[3] = { VTable(pConsole->RegisterInt("coop_sound_probe_own_int", 7, 0, "Crysis Coop: testing")),
			VTable(pConsole->RegisterFloat("coop_sound_probe_own_float", 7.0f, 0, "Crysis Coop: testing")),
			VTable(pConsole->RegisterString("coop_sound_probe_own_string", "7", 0, "Crysis Coop: testing")) };
		const char* names[1024];
		const size_t n = pConsole->GetSortedVars(names, 1024, "s_");
		int moved = 0, owned = 0;
		string refused;
		for (size_t i = 0; i < n; ++i)
		{
			// (the dummy sound system registers these two again, in its own
			// memory: the failed one took them out as it went)
			if (!names[i] || _strnicmp(names[i], "s_", 2) || !_stricmp(names[i], "s_SoundEnable") || !_stricmp(names[i], "s_DummySound"))
				continue;
			ICVar* pVar = pConsole->GetCVar(names[i]);
			if (!pVar)
				continue;
			const void* vtable = VTable(pVar);
			bool ok = false;
			if (vtable == own[0] || vtable == own[1] || vtable == own[2])
			{
				ok = true;
				++owned;
			}
			else if (pVar->GetType() == CVAR_INT ? MoveToOwnMemory<int>(pVar, intRef) : pVar->GetType() == CVAR_FLOAT && MoveToOwnMemory<float>(pVar, floatRef))
			{
				ok = true;
				++moved;
			}
			if (!ok)
			{
				s_failedSoundCvars.dead.insert(pVar);
				refused += string(" ") + pVar->GetName();
				continue;
			}
			// (what a change of it called was the failed sound system's)
			pVar->SetOnChangeCallback(0);
		}
		CryLogAlways("[CoopSound] no sound system: of its %d console variables %d have memory of their own now, %d had the console's, %d are not changed any more:%s (the kinds: int %d, float %d)",
			(int)n, moved, owned, (int)s_failedSoundCvars.dead.size(), refused.empty() ? " -" : refused.c_str(), intRef.offset, floatRef.offset);
	}
}

bool CoopSound::NoRealSoundSystem()
{
	return s_failedSoundCvars.on;
}

void CoopSound::GuardFailedSoundSystem(bool on)
{
	if (!gEnv || !gEnv->pConsole)
		return;
	if (!on)
	{
		if (s_failedSoundCvars.on)
		{
			gEnv->pConsole->RemoveConsoleVarSink(&s_failedSoundCvars);
			s_failedSoundCvars.on = false;
		}
		return;
	}
	ICVar* pEnable = gEnv->pConsole->GetCVar("s_SoundEnable");
	ICVar* pDummy = gEnv->pConsole->GetCVar("s_DummySound");
	ISoundSystem* pSound = gEnv->pSoundSystem;
	IAudioDevice* pDevice = pSound ? pSound->GetIAudioDevice() : 0;
	const bool off = (pEnable && !pEnable->GetIVal()) || (pDummy && pDummy->GetIVal());
	CryLogAlways("[CoopSound] the sound system at the start: %s (s_SoundEnable %d, s_DummySound %d, audio device %p)",
		off || !pDevice ? "none (a dummy one)" : "a real one", pEnable ? pEnable->GetIVal() : -1, pDummy ? pDummy->GetIVal() : -1, pDevice);
	if ((off || !pDevice) && !s_failedSoundCvars.on)
	{
		MoveFailedSoundCvars();
		gEnv->pConsole->AddConsoleVarSink(&s_failedSoundCvars);
		s_failedSoundCvars.on = true;
	}
}
