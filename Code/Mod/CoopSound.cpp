// Crysis Coop: a watch over the game's sound (see CoopSound.h).
#include "StdAfx.h"
#include "CoopSound.h"
#include "CoopAI.h"
#include "Game.h"
#include <ISound.h>
#include <IMusicSystem.h>
#include "IAnimationGraph.h"

float CoopSoundOutputPeak(bool* pActive);	// CoopSoundMeter.cpp

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

		// coop_test_sound <pause|mute|unmute|watch>: the sound system paused or
		// muted as if by a fault; watch: the watch also without the focus
		void CmdTestSound(IConsoleCmdArgs* pArgs)
		{
			const char* what = pArgs->GetArgCount() > 1 ? pArgs->GetArg(1) : "";
			if (!gEnv->pSoundSystem)
				return;
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
			gEnv->pConsole->AddCommand("coop_test_sound", CmdTestSound, 0, "Crysis Coop testing: coop_test_sound <pause|mute|unmute|watch|nanpos|nanvel|nanwater|bigvel|underwater>: the sound system paused (muted), or its listener broken for a frame, as if by a fault; watch: the sound watch also without the window's focus");
		}
		const float now = gEnv->pTimer->GetAsyncCurTime();

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
