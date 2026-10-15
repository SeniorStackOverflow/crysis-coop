// Crysis Coop: a watch over the game's sound (see CoopSound.h).
#include "StdAfx.h"
#include "CoopSound.h"
#include "CoopAI.h"
#include "Game.h"
#include <ISound.h>
#include <IMusicSystem.h>
#include "IAnimationGraph.h"
#include <IActorSystem.h>

float CoopSoundOutputPeak(bool* pActive);	// CoopSoundMeter.cpp
bool CoopFmodChannels(void* pSystem, int maxChannels, int* pPlaying, int* pVirtual, int* pSoftware, float cpu[4]);	// CoopSoundMeter.cpp
int CoopFmodChannelList(void* pSystem, int maxChannels, const char* filter, int maxLines, void (*pLine)(const char*));	// CoopSoundMeter.cpp
struct SCoopFmodWatch
{
	int stuck, stopped, late, takenAway, started, startedHeard;
	int lostLoudness[3];	// the ones that lost their voice: audibility under 0.02, under 0.1, louder
	char lostNames[256];	// the first loud ones (testing)
	float latest;
	char names[256];
};
void CoopFmodWatch(void* pSystem, int maxChannels, bool stop, float now, SCoopFmodWatch* pOut);	// CoopSoundMeter.cpp

namespace CoopSound
{
	namespace
	{
		const float CHECK_EVERY = 1.0f;		// s between looks at the output
		const int SILENT_LOOKS = 15;		// so many silent looks in a row: no sound comes out
		const float RESTART_EVERY = 120.0f;	// s at least between two restarts
		const float LOUD = 0.0005f;			// a peak above this is sound

		float s_nextLook = 0.0f, s_lastLook = -100.0f;
		int s_silent = 0;
		bool s_heard = false;
		float s_restartAt = -1000.0f, s_onAgainAt = -1.0f, s_checkAt = -1.0f;
		bool s_anyFocus = false;	// testing: the test windows never have the focus

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
		return string().Format("FMOD: %d channels playing (%d heard, %d virtual) of %d, %d mixed at most; cpu %.1f%% (dsp %.1f, stream %.1f, update %.1f)",
			playing, playing - isVirtual, isVirtual, maxChannels, software, cpu[3], cpu[0], cpu[1], cpu[2]);
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
			if (!stricmp(what, "probe"))
			{
				// probe <sound> [foley|shot] [metres away]
				StartProbe(pArgs->GetArgCount() > 2 ? pArgs->GetArg(2) : "", pArgs->GetArgCount() > 3 && !stricmp(pArgs->GetArg(3), "foley"),
					pArgs->GetArgCount() > 4 ? (float)atof(pArgs->GetArg(4)) : 0.0f);
				return;
			}
			if (!stricmp(what, "pause"))
				gEnv->pSoundSystem->Pause(true);
			else if (!stricmp(what, "mute"))
				gEnv->pSoundSystem->Mute(true);
			else if (!stricmp(what, "unmute"))
				gEnv->pSoundSystem->Mute(false);
			else if (!stricmp(what, "watch"))
				s_anyFocus = true;
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

	void Update(bool haveFocus)
	{
		if (!gEnv || !gEnv->pTimer || !gEnv->pConsole || gEnv->pSystem->IsDedicated())
			return;
		WatchMusic();
		static bool s_registered = false;
		if (!s_registered)
		{
			s_registered = true;
			gEnv->pConsole->RegisterInt("coop_sound_unstick", 1, 0, "Crysis Coop: stop the parts of sounds stuck virtual at their start for 0.4 s (they kept their sound playing and came seconds late); 0 = leave them");
			gEnv->pConsole->AddCommand("coop_test_sound", CmdTestSound, 0, "Crysis Coop testing: coop_test_sound <pause|mute|unmute|watch|nanpos|nanvel|nanwater|bigvel|underwater>: the sound system paused (muted), or its listener broken for a frame, as if by a fault; watch: the sound watch also without the window's focus; probe <sound> [foley|shot] [metres]: a sound at the player (or so far away), when it is heard and how often its voice is taken away");
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

		if (now < s_nextLook)
			return;
		s_nextLook = now + CHECK_EVERY;
		// looks far apart (a level loading) do not follow each other
		if (now - s_lastLook > 3.0f * CHECK_EVERY)
			s_silent = 0;
		s_lastLook = now;

		ICVar* pEnable = gEnv->pConsole->GetCVar("s_SoundEnable");
		IGameFramework* pFramework = g_pGame ? g_pGame->GetIGameFramework() : 0;
		if (!gEnv->pSoundSystem || !pEnable || pEnable->GetIVal() == 0 || !(haveFocus || s_anyFocus) || s_onAgainAt > 0.0f
			|| !pFramework || !pFramework->GetClientActor() || pFramework->IsGamePaused() || !CoopAI::IsCoopSession())
		{
			s_silent = 0;
			return;
		}

		bool active = false;
		const float peak = CoopSoundOutputPeak(&active);
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
			CryLogAlways("[CoopSound] still no sound 5 s after the restart (%s)", peak < 0.0f ? "no audio session" : active ? "session playing silence" : "session stopped");
			s_checkAt = -1.0f;
		}

		// only a game that was heard before (no sound device, the volume mixer's mute...)
		if (++s_silent >= SILENT_LOOKS && s_heard && now - s_restartAt > RESTART_EVERY)
		{
			ISoundSystem* pSound = gEnv->pSoundSystem;
			int memory = 0, memoryMax = 0;
			pSound->GetSoundMemoryUsageInfo(&memory, &memoryMax);
			const bool paused = pSound->IsPaused();
			CryLogAlways("[CoopSound] no sound came out for %d s (%s; sound system %s, %d voices, cpu %.1f, memory %d of %d): %s",
				s_silent, peak < 0.0f ? "no audio session" : active ? "session playing silence" : "session stopped",
				paused ? "PAUSED" : "running", pSound->GetUsedVoices(), pSound->GetCPUUsage(), memory, memoryMax,
				paused ? "it goes on" : "the sound system starts again");
			s_restartAt = now;
			s_silent = 0;
			if (paused)
			{
				pSound->Pause(false);
				s_checkAt = now + 5.0f;
				return;
			}
			gEnv->pConsole->ExecuteString("s_SoundEnable 0");
			s_onAgainAt = now + 0.3f;
		}
	}
}
