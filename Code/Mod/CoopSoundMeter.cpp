// Crysis Coop: how loud this game's sound comes out of Windows (Core Audio:
// the peak of this process's audio sessions on the default output device).
// Apart from the game's headers, like CoopJpeg.cpp.
#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#pragma comment(lib, "ole32.lib")

namespace
{
	template <class T> void Release(T*& p)
	{
		if (p)
			p->Release();
		p = 0;
	}
}

// the loudest of this process's sessions (0..1), -1 when it has none (or
// Core Audio does not answer); *pActive: one of them plays. The sessions are
// looked up every 5 s (or when a read fails), their meters read in between:
// it is read 4 times a second now
float CoopSoundOutputPeak(bool* pActive)
{
	static bool s_com = false;
	if (!s_com)
	{
		// the game's thread may have COM already (either way is fine)
		CoInitializeEx(0, COINIT_MULTITHREADED);
		s_com = true;
	}
	if (pActive)
		*pActive = false;
	struct SSession { IAudioMeterInformation* pMeter; IAudioSessionControl2* pControl; };
	static SSession s_sessions[4];
	static int s_count = 0;
	static DWORD s_lookedAt = 0;
	static bool s_lookedOnce = false;
	const DWORD tick = GetTickCount();
	bool failed = false;
	float best = -1.0f;
	for (int i = 0; i < s_count; ++i)
	{
		AudioSessionState state = AudioSessionStateInactive;
		float peak = 0.0f;
		if (FAILED(s_sessions[i].pMeter->GetPeakValue(&peak)) || FAILED(s_sessions[i].pControl->GetState(&state)))
		{
			failed = true;
			break;
		}
		if (state == AudioSessionStateActive && pActive)
			*pActive = true;
		best = best > peak ? best : peak;
	}
	if (!failed && s_lookedOnce && tick - s_lookedAt < 5000)
		return best;
	// look the sessions up again
	for (int i = 0; i < s_count; ++i)
	{
		Release(s_sessions[i].pMeter);
		Release(s_sessions[i].pControl);
	}
	s_count = 0;
	s_lookedAt = tick;
	s_lookedOnce = true;
	if (pActive)
		*pActive = false;
	best = -1.0f;
	IMMDeviceEnumerator* pEnum = 0;
	IMMDevice* pDevice = 0;
	IAudioSessionManager2* pManager = 0;
	IAudioSessionEnumerator* pSessions = 0;
	if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), 0, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnum))
		&& SUCCEEDED(pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice))
		&& SUCCEEDED(pDevice->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, 0, (void**)&pManager))
		&& SUCCEEDED(pManager->GetSessionEnumerator(&pSessions)))
	{
		const DWORD me = GetCurrentProcessId();
		int count = 0;
		pSessions->GetCount(&count);
		for (int i = 0; i < count; ++i)
		{
			IAudioSessionControl* pControl = 0;
			IAudioSessionControl2* pControl2 = 0;
			IAudioMeterInformation* pMeter = 0;
			DWORD pid = 0;
			if (SUCCEEDED(pSessions->GetSession(i, &pControl))
				&& SUCCEEDED(pControl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&pControl2))
				&& SUCCEEDED(pControl2->GetProcessId(&pid)) && pid == me)
			{
				AudioSessionState state = AudioSessionStateInactive;
				if (SUCCEEDED(pControl2->GetState(&state)) && state == AudioSessionStateActive && pActive)
					*pActive = true;
				float peak = 0.0f;
				if (SUCCEEDED(pControl->QueryInterface(__uuidof(IAudioMeterInformation), (void**)&pMeter)) && SUCCEEDED(pMeter->GetPeakValue(&peak)))
				{
					best = best > peak ? best : peak;
					if (s_count < 4)
					{
						s_sessions[s_count].pMeter = pMeter;
						s_sessions[s_count].pControl = pControl2;
						++s_count;
						pMeter = 0;
						pControl2 = 0;
					}
				}
				else if (best < 0.0f)
					best = 0.0f;
			}
			Release(pMeter);
			Release(pControl2);
			Release(pControl);
		}
	}
	Release(pSessions);
	Release(pManager);
	Release(pDevice);
	Release(pEnum);
	return best;
}

// the player turned this game's sound off in Windows: the device muted or at
// volume 0, or this process's session muted or at 0 in the volume mixer (no
// sound coming out is then no fault). Asked once a second at most
bool CoopSoundOutputMuted()
{
	bool muted = false;
	IMMDeviceEnumerator* pEnum = 0;
	IMMDevice* pDevice = 0;
	IAudioEndpointVolume* pEndpoint = 0;
	IAudioSessionManager2* pManager = 0;
	IAudioSessionEnumerator* pSessions = 0;
	if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), 0, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnum))
		&& SUCCEEDED(pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice)))
	{
		if (SUCCEEDED(pDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, 0, (void**)&pEndpoint)))
		{
			BOOL mute = FALSE;
			float level = 1.0f;
			if ((SUCCEEDED(pEndpoint->GetMute(&mute)) && mute) || (SUCCEEDED(pEndpoint->GetMasterVolumeLevelScalar(&level)) && level < 0.01f))
				muted = true;
		}
		if (!muted && SUCCEEDED(pDevice->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, 0, (void**)&pManager))
			&& SUCCEEDED(pManager->GetSessionEnumerator(&pSessions)))
		{
			const DWORD me = GetCurrentProcessId();
			int count = 0;
			pSessions->GetCount(&count);
			for (int i = 0; i < count && !muted; ++i)
			{
				IAudioSessionControl* pControl = 0;
				IAudioSessionControl2* pControl2 = 0;
				ISimpleAudioVolume* pVolume = 0;
				DWORD pid = 0;
				if (SUCCEEDED(pSessions->GetSession(i, &pControl))
					&& SUCCEEDED(pControl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&pControl2))
					&& SUCCEEDED(pControl2->GetProcessId(&pid)) && pid == me
					&& SUCCEEDED(pControl->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&pVolume)))
				{
					BOOL mute = FALSE;
					float level = 1.0f;
					if ((SUCCEEDED(pVolume->GetMute(&mute)) && mute) || (SUCCEEDED(pVolume->GetMasterVolume(&level)) && level < 0.01f))
						muted = true;
				}
				Release(pVolume);
				Release(pControl2);
				Release(pControl);
			}
		}
	}
	Release(pSessions);
	Release(pManager);
	Release(pEndpoint);
	Release(pDevice);
	Release(pEnum);
	return muted;
}

// FMOD's own view of its channels (CrySoundSystem lets FMOD mix only 64 at
// once, setSoftwareChannels(64); the others play "virtual": not heard).
// pSystem: CrySoundSystem's FMOD::System. Out: channels playing, of them
// virtual, the software channels, and FMOD's cpu use (dsp, stream, update,
// total in %). False when FMOD or the pointer does not answer
bool CoopFmodChannels(void* pSystem, int maxChannels, int* pPlaying, int* pVirtual, int* pSoftware, float cpu[4])
{
	typedef int(__stdcall* FnGetInt)(void*, int*);
	typedef int(__stdcall* FnGetCpu)(void*, float*, float*, float*, float*);
	typedef int(__stdcall* FnGetChannel)(void*, int, void**);
	typedef int(__stdcall* FnGetBool)(void*, bool*);
	static FnGetInt s_playing = 0, s_software = 0;
	static FnGetCpu s_cpu = 0;
	static FnGetChannel s_channel = 0;
	static FnGetBool s_isPlaying = 0, s_isVirtual = 0;
	static bool s_looked = false;
	if (!s_looked)
	{
		s_looked = true;
		if (HMODULE h = GetModuleHandleA("fmodex.dll"))
		{
			s_playing = (FnGetInt)GetProcAddress(h, "?getChannelsPlaying@System@FMOD@@QAG?AW4FMOD_RESULT@@PAH@Z");
			s_software = (FnGetInt)GetProcAddress(h, "?getSoftwareChannels@System@FMOD@@QAG?AW4FMOD_RESULT@@PAH@Z");
			s_cpu = (FnGetCpu)GetProcAddress(h, "?getCPUUsage@System@FMOD@@QAG?AW4FMOD_RESULT@@PAM000@Z");
			s_channel = (FnGetChannel)GetProcAddress(h, "?getChannel@System@FMOD@@QAG?AW4FMOD_RESULT@@HPAPAVChannel@2@@Z");
			s_isPlaying = (FnGetBool)GetProcAddress(h, "?isPlaying@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PA_N@Z");
			s_isVirtual = (FnGetBool)GetProcAddress(h, "?isVirtual@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PA_N@Z");
		}
	}
	if (!pSystem || !s_playing || !s_software || !s_cpu || !s_channel || !s_isPlaying || !s_isVirtual || maxChannels <= 0 || maxChannels > 4096
		|| IsBadReadPtr(pSystem, 64))
		return false;
	*pPlaying = *pVirtual = *pSoftware = 0;
	cpu[0] = cpu[1] = cpu[2] = cpu[3] = 0.0f;
	if (s_playing(pSystem, pPlaying) != 0)
		return false;
	s_software(pSystem, pSoftware);
	s_cpu(pSystem, &cpu[0], &cpu[1], &cpu[2], &cpu[3]);
	for (int i = 0; i < maxChannels; ++i)
	{
		void* pChannel = 0;
		bool playing = false, isVirtual = false;
		if (s_channel(pSystem, i, &pChannel) != 0 || !pChannel || s_isPlaying(pChannel, &playing) != 0 || !playing)
			continue;
		if (s_isVirtual(pChannel, &isVirtual) == 0 && isVirtual)
			++*pVirtual;
	}
	return true;
}

// testing: FMOD's playing channels one by one (their sound's name containing
// filter, every one for an empty filter): heard or virtual, paused, where it
// plays in its sound, priority, volume, audibility, loops left. Calls
// pLine(text) for each, at most maxLines; returns how many matched
int CoopFmodChannelList(void* pSystem, int maxChannels, const char* filter, int maxLines, void (*pLine)(const char*))
{
	typedef int(__stdcall* FnGetChannel)(void*, int, void**);
	typedef int(__stdcall* FnGetBool)(void*, bool*);
	typedef int(__stdcall* FnGetPtr)(void*, void**);
	typedef int(__stdcall* FnGetName)(void*, char*, int);
	typedef int(__stdcall* FnGetUIntUnit)(void*, unsigned int*, unsigned int);
	typedef int(__stdcall* FnGetInt)(void*, int*);
	typedef int(__stdcall* FnGetFloat)(void*, float*);
	static FnGetChannel s_channel = 0;
	static FnGetBool s_isPlaying = 0, s_isVirtual = 0, s_paused = 0;
	static FnGetPtr s_sound = 0;
	static FnGetName s_name = 0;
	static FnGetUIntUnit s_position = 0, s_length = 0;
	static FnGetInt s_priority = 0, s_loops = 0;
	static FnGetFloat s_volume = 0, s_audibility = 0;
	static bool s_looked = false;
	if (!s_looked)
	{
		s_looked = true;
		if (HMODULE h = GetModuleHandleA("fmodex.dll"))
		{
			s_channel = (FnGetChannel)GetProcAddress(h, "?getChannel@System@FMOD@@QAG?AW4FMOD_RESULT@@HPAPAVChannel@2@@Z");
			s_isPlaying = (FnGetBool)GetProcAddress(h, "?isPlaying@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PA_N@Z");
			s_isVirtual = (FnGetBool)GetProcAddress(h, "?isVirtual@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PA_N@Z");
			s_paused = (FnGetBool)GetProcAddress(h, "?getPaused@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PA_N@Z");
			s_sound = (FnGetPtr)GetProcAddress(h, "?getCurrentSound@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PAPAVSound@2@@Z");
			s_name = (FnGetName)GetProcAddress(h, "?getName@Sound@FMOD@@QAG?AW4FMOD_RESULT@@PADH@Z");
			s_position = (FnGetUIntUnit)GetProcAddress(h, "?getPosition@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PAII@Z");
			s_length = (FnGetUIntUnit)GetProcAddress(h, "?getLength@Sound@FMOD@@QAG?AW4FMOD_RESULT@@PAII@Z");
			s_priority = (FnGetInt)GetProcAddress(h, "?getPriority@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PAH@Z");
			s_loops = (FnGetInt)GetProcAddress(h, "?getLoopCount@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PAH@Z");
			s_volume = (FnGetFloat)GetProcAddress(h, "?getVolume@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PAM@Z");
			s_audibility = (FnGetFloat)GetProcAddress(h, "?getAudibility@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PAM@Z");
		}
	}
	if (!pSystem || !s_channel || !s_isPlaying || !s_isVirtual || !s_paused || !s_sound || !s_name || !s_position || !s_length || !s_priority
		|| !s_loops || !s_volume || !s_audibility || maxChannels <= 0 || maxChannels > 4096 || IsBadReadPtr(pSystem, 64))
		return 0;
	int matched = 0, lines = 0;
	for (int i = 0; i < maxChannels; ++i)
	{
		void* pChannel = 0;
		bool playing = false;
		if (s_channel(pSystem, i, &pChannel) != 0 || !pChannel || s_isPlaying(pChannel, &playing) != 0 || !playing)
			continue;
		void* pSound = 0;
		char name[128] = "?";
		if (s_sound(pChannel, &pSound) == 0 && pSound)
			s_name(pSound, name, sizeof(name) - 1);
		name[sizeof(name) - 1] = 0;
		if (filter && *filter)
		{
			char lower[128], want[64];
			int k = 0;
			for (; name[k] && k < 127; ++k)
				lower[k] = (char)tolower((unsigned char)name[k]);
			lower[k] = 0;
			for (k = 0; filter[k] && k < 63; ++k)
				want[k] = (char)tolower((unsigned char)filter[k]);
			want[k] = 0;
			if (!strstr(lower, want))
				continue;
		}
		++matched;
		if (lines >= maxLines)
			continue;
		bool isVirtual = false, paused = false;
		unsigned int position = 0, length = 0;
		int priority = -1, loops = 0;
		float volume = -1.0f, audibility = -1.0f;
		s_isVirtual(pChannel, &isVirtual);
		s_paused(pChannel, &paused);
		s_position(pChannel, &position, 1);	// FMOD_TIMEUNIT_MS
		if (pSound)
			s_length(pSound, &length, 1);
		s_priority(pChannel, &priority);
		s_loops(pChannel, &loops);
		s_volume(pChannel, &volume);
		s_audibility(pChannel, &audibility);
		char line[300];
		_snprintf(line, sizeof(line) - 1, "channel %d %s: %s%s, at %u of %u ms, priority %d, volume %.2f, audibility %.3f, loops %d", i, name,
			isVirtual ? "virtual" : "heard", paused ? " (paused)" : "", position, length, priority, volume, audibility, loops);
		line[sizeof(line) - 1] = 0;
		pLine(line);
		++lines;
	}
	return matched;
}

// the game's window is the one in front (CryGame tells the systems so with
// ESYSTEM_EVENT_CHANGE_FOCUS; CGame::Update's haveFocus is always true)
bool CoopWindowFocused(void* hwnd)
{
	// (exactly CryGame's test before it tells the systems of a focus change)
	return hwnd && GetForegroundWindow() == (HWND)hwnd && GetFocus() == (HWND)hwnd;
}

// FMOD's own picture of the output for the sound watch (CoopSound.cpp): the
// final mix (System::getWaveData), the master channel group and the groups
// under it (the event system's categories: paused, muted or at volume 0
// behind the game's back), the heard channels and whether they move on since
// the last look. The user's game went silent for 15 s in a firefight with 81
// channels playing (2026-10-18); the Windows meter alone could not say why
struct SCoopFmodSnapshot
{
	float mixPeak;          // the loudest sample of the final mix, -1: no answer
	int mixNaN;             // samples that are not numbers
	bool mixSame;           // the very same samples as at the last look (a mixer that stands)
	float masterVolume;
	bool masterPaused, masterMute;
	float masterPeak;
	int groups;             // channel groups under the master
	char groupsOff[192];    // those paused, muted or at volume 0
	int playing, heard, audible;   // channels playing; of them not virtual; not paused and heard with audibility 0.02 or more
	int pausedAudible;      // paused ones that would be (the event system pauses a category channel by channel)
	int moving, frozen;     // audible channels whose position moved on / stood since the last look
	char top[200];          // the loudest heard channels
};

namespace
{
	typedef int(__stdcall* FnGroupGetFloat)(void*, float*);
	typedef int(__stdcall* FnGroupGetBool)(void*, bool*);
	typedef int(__stdcall* FnGroupSetBool)(void*, bool);
	typedef int(__stdcall* FnGroupGetInt)(void*, int*);
	typedef int(__stdcall* FnGroupGetGroup)(void*, int, void**);
	typedef int(__stdcall* FnGroupGetName)(void*, char*, int);
	typedef int(__stdcall* FnWaveData)(void*, float*, int, int);
	typedef int(__stdcall* FnGetMaster)(void*, void**);
	struct SFmodGroupApi
	{
		FnGetMaster master;
		FnWaveData systemWave, groupWave;
		FnGroupGetFloat volume;
		FnGroupGetBool paused, mute;
		FnGroupSetBool setPaused, setMute;
		FnGroupGetInt numGroups;
		FnGroupGetGroup group;
		FnGroupGetName name;
		bool ok;
	};
	const SFmodGroupApi& FmodGroupApi()
	{
		static SFmodGroupApi s_api;
		static bool s_looked = false;
		if (!s_looked)
		{
			s_looked = true;
			memset(&s_api, 0, sizeof(s_api));
			if (HMODULE h = GetModuleHandleA("fmodex.dll"))
			{
				s_api.master = (FnGetMaster)GetProcAddress(h, "?getMasterChannelGroup@System@FMOD@@QAG?AW4FMOD_RESULT@@PAPAVChannelGroup@2@@Z");
				s_api.systemWave = (FnWaveData)GetProcAddress(h, "?getWaveData@System@FMOD@@QAG?AW4FMOD_RESULT@@PAMHH@Z");
				s_api.groupWave = (FnWaveData)GetProcAddress(h, "?getWaveData@ChannelGroup@FMOD@@QAG?AW4FMOD_RESULT@@PAMHH@Z");
				s_api.volume = (FnGroupGetFloat)GetProcAddress(h, "?getVolume@ChannelGroup@FMOD@@QAG?AW4FMOD_RESULT@@PAM@Z");
				s_api.paused = (FnGroupGetBool)GetProcAddress(h, "?getPaused@ChannelGroup@FMOD@@QAG?AW4FMOD_RESULT@@PA_N@Z");
				s_api.mute = (FnGroupGetBool)GetProcAddress(h, "?getMute@ChannelGroup@FMOD@@QAG?AW4FMOD_RESULT@@PA_N@Z");
				s_api.setPaused = (FnGroupSetBool)GetProcAddress(h, "?setPaused@ChannelGroup@FMOD@@QAG?AW4FMOD_RESULT@@_N@Z");
				s_api.setMute = (FnGroupSetBool)GetProcAddress(h, "?setMute@ChannelGroup@FMOD@@QAG?AW4FMOD_RESULT@@_N@Z");
				s_api.numGroups = (FnGroupGetInt)GetProcAddress(h, "?getNumGroups@ChannelGroup@FMOD@@QAG?AW4FMOD_RESULT@@PAH@Z");
				s_api.group = (FnGroupGetGroup)GetProcAddress(h, "?getGroup@ChannelGroup@FMOD@@QAG?AW4FMOD_RESULT@@HPAPAV12@@Z");
				s_api.name = (FnGroupGetName)GetProcAddress(h, "?getName@ChannelGroup@FMOD@@QAG?AW4FMOD_RESULT@@PADH@Z");
			}
			s_api.ok = s_api.master && s_api.systemWave && s_api.groupWave && s_api.volume && s_api.paused && s_api.mute && s_api.setPaused
				&& s_api.setMute && s_api.numGroups && s_api.group && s_api.name;
		}
		return s_api;
	}

	// the peak of 256 samples of a mix and how many of them are no numbers
	float WavePeak(const float* wave, int n, int* pNaN)
	{
		float peak = 0.0f;
		*pNaN = 0;
		for (int i = 0; i < n; ++i)
		{
			const float x = wave[i];
			if (x != x || x > 1e30f || x < -1e30f)
				++*pNaN;
			else if (x > peak)
				peak = x;
			else if (-x > peak)
				peak = -x;
		}
		return peak;
	}

	void CleanName(char* name)
	{
		for (char* p = name; *p; ++p)
			if ((unsigned char)*p < 32 || (unsigned char)*p > 126)
				*p = '?';
	}

	// the groups under one: those paused, muted or at volume 0 go into the text
	void WalkGroups(const SFmodGroupApi& api, void* pGroup, int depth, int& count, char* text, size_t size)
	{
		int n = 0;
		if (depth > 4 || count > 64 || api.numGroups(pGroup, &n) != 0)
			return;
		for (int i = 0; i < n && count <= 64; ++i)
		{
			void* pChild = 0;
			if (api.group(pGroup, i, &pChild) != 0 || !pChild)
				continue;
			++count;
			float volume = 1.0f;
			bool paused = false, mute = false;
			api.volume(pChild, &volume);
			api.paused(pChild, &paused);
			api.mute(pChild, &mute);
			if (paused || mute || volume < 0.001f)
			{
				char name[48] = "?";
				api.name(pChild, name, sizeof(name) - 1);
				name[sizeof(name) - 1] = 0;
				CleanName(name);
				const size_t used = strlen(text);
				if (used < size - 60)
				{
					_snprintf(text + used, size - used - 1, " %s:%s%s%s", name, paused ? "paused" : "", mute ? "muted" : "", volume < 0.001f ? "vol0" : "");
					text[size - 1] = 0;
				}
			}
			WalkGroups(api, pChild, depth + 1, count, text, size);
		}
	}
}

// FMOD's final mix now: the loudest sample of every output channel (testing:
// the meter of a game whose sound goes to a file, no Windows session to ask)
float CoopFmodMixPeak(void* pSystem)
{
	const SFmodGroupApi& api = FmodGroupApi();
	if (!pSystem || !api.ok || IsBadReadPtr(pSystem, 64))
		return -1.0f;
	static float wave[1024];
	float best = -1.0f;
	for (int ch = 0; ch < 8; ++ch)
	{
		if (api.systemWave(pSystem, wave, 1024, ch) != 0)
			break;
		int nan = 0;
		const float peak = WavePeak(wave, 1024, &nan);
		best = best > peak ? best : peak;
	}
	return best;
}

bool CoopFmodSnapshot(void* pSystem, int maxChannels, SCoopFmodSnapshot* pOut)
{
	typedef int(__stdcall* FnGetChannel)(void*, int, void**);
	typedef int(__stdcall* FnGetBool)(void*, bool*);
	typedef int(__stdcall* FnGetPtr)(void*, void**);
	typedef int(__stdcall* FnGetName)(void*, char*, int);
	typedef int(__stdcall* FnGetUIntUnit)(void*, unsigned int*, unsigned int);
	typedef int(__stdcall* FnGetFloat)(void*, float*);
	static FnGetChannel s_channel = 0;
	static FnGetBool s_isPlaying = 0, s_isVirtual = 0, s_paused = 0;
	static FnGetPtr s_sound = 0;
	static FnGetName s_name = 0;
	static FnGetUIntUnit s_position = 0;
	static FnGetFloat s_audibility = 0;
	static bool s_looked = false;
	// per channel: its sound and position at the last look
	struct SLast { void* pSound; unsigned int position; };
	static SLast s_last[4096];
	static float s_lastMix[1024];
	static bool s_haveLastMix = false;
	if (!s_looked)
	{
		s_looked = true;
		memset(s_last, 0, sizeof(s_last));
		if (HMODULE h = GetModuleHandleA("fmodex.dll"))
		{
			s_channel = (FnGetChannel)GetProcAddress(h, "?getChannel@System@FMOD@@QAG?AW4FMOD_RESULT@@HPAPAVChannel@2@@Z");
			s_isPlaying = (FnGetBool)GetProcAddress(h, "?isPlaying@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PA_N@Z");
			s_isVirtual = (FnGetBool)GetProcAddress(h, "?isVirtual@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PA_N@Z");
			s_paused = (FnGetBool)GetProcAddress(h, "?getPaused@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PA_N@Z");
			s_sound = (FnGetPtr)GetProcAddress(h, "?getCurrentSound@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PAPAVSound@2@@Z");
			s_name = (FnGetName)GetProcAddress(h, "?getName@Sound@FMOD@@QAG?AW4FMOD_RESULT@@PADH@Z");
			s_position = (FnGetUIntUnit)GetProcAddress(h, "?getPosition@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PAII@Z");
			s_audibility = (FnGetFloat)GetProcAddress(h, "?getAudibility@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PAM@Z");
		}
	}
	const SFmodGroupApi& api = FmodGroupApi();
	memset(pOut, 0, sizeof(*pOut));
	pOut->mixPeak = pOut->masterPeak = -1.0f;
	pOut->masterVolume = -1.0f;
	if (!pSystem || !api.ok || !s_channel || !s_isPlaying || !s_isVirtual || !s_paused || !s_sound || !s_name || !s_position || !s_audibility
		|| maxChannels <= 0 || maxChannels > 4096 || IsBadReadPtr(pSystem, 64))
		return false;
	// the final mix: every output channel (a sound behind or to the right
	// is not in the front left one), 1024 samples of each
	static float wave[1024];
	for (int ch = 0; ch < 8; ++ch)
	{
		if (api.systemWave(pSystem, wave, 1024, ch) != 0)
			break;
		int nan = 0;
		const float peak = WavePeak(wave, 1024, &nan);
		pOut->mixPeak = pOut->mixPeak > peak ? pOut->mixPeak : peak;
		pOut->mixNaN += nan;
		if (ch == 0)
		{
			pOut->mixSame = s_haveLastMix && peak > 1e-6f && memcmp(wave, s_lastMix, sizeof(s_lastMix)) == 0;
			memcpy(s_lastMix, wave, sizeof(s_lastMix));
			s_haveLastMix = true;
		}
	}
	// the master group and the groups under it
	void* pMaster = 0;
	if (api.master(pSystem, &pMaster) == 0 && pMaster)
	{
		api.volume(pMaster, &pOut->masterVolume);
		api.paused(pMaster, &pOut->masterPaused);
		api.mute(pMaster, &pOut->masterMute);
		int nan = 0;
		if (api.groupWave(pMaster, wave, 256, 0) == 0)
			pOut->masterPeak = WavePeak(wave, 256, &nan);
		WalkGroups(api, pMaster, 0, pOut->groups, pOut->groupsOff, sizeof(pOut->groupsOff));
	}
	// the channels
	int topN = 0;
	for (int i = 0; i < maxChannels; ++i)
	{
		void* pChannel = 0;
		void* pSound = 0;
		bool playing = false, isVirtual = false, paused = false;
		unsigned int position = 0;
		float audibility = 0.0f;
		if (s_channel(pSystem, i, &pChannel) != 0 || !pChannel || s_isPlaying(pChannel, &playing) != 0 || !playing)
		{
			s_last[i].pSound = 0;
			continue;
		}
		++pOut->playing;
		if (s_isVirtual(pChannel, &isVirtual) != 0 || isVirtual)
		{
			s_last[i].pSound = 0;
			continue;
		}
		++pOut->heard;
		s_sound(pChannel, &pSound);
		s_position(pChannel, &position, 1);	// FMOD_TIMEUNIT_MS
		s_paused(pChannel, &paused);
		if (s_audibility(pChannel, &audibility) == 0 && audibility >= 0.02f && paused)
			++pOut->pausedAudible;
		else if (audibility >= 0.02f)
		{
			++pOut->audible;
			if (s_last[i].pSound == pSound && pSound && !paused)
			{
				if (position == s_last[i].position)
					++pOut->frozen;
				else
					++pOut->moving;
			}
			if (audibility >= 0.1f && topN < 4 && pSound)
			{
				char name[48] = "?";
				s_name(pSound, name, sizeof(name) - 1);
				name[sizeof(name) - 1] = 0;
				CleanName(name);
				const size_t used = strlen(pOut->top);
				_snprintf(pOut->top + used, sizeof(pOut->top) - used - 1, " %s(%.2f,%u ms)", name, audibility, position);
				pOut->top[sizeof(pOut->top) - 1] = 0;
				++topN;
			}
		}
		s_last[i].pSound = pSound;
		s_last[i].position = position;
	}
	return true;
}

// the master channel group (and any group under it called master) unpaused
// and unmuted; what was set right goes into the text. Returns how many
// settings changed. With fault: as a fault would, the master paused (1) or
// muted (2), for the tests
int CoopFmodRepairGroups(void* pSystem, int fault, char* what, size_t size)
{
	const SFmodGroupApi& api = FmodGroupApi();
	what[0] = 0;
	void* pMaster = 0;
	if (!pSystem || !api.ok || IsBadReadPtr(pSystem, 64) || api.master(pSystem, &pMaster) != 0 || !pMaster)
		return 0;
	if (fault == 1)
		return api.setPaused(pMaster, true) == 0 ? 1 : 0;
	if (fault == 2)
		return api.setMute(pMaster, true) == 0 ? 1 : 0;
	int changed = 0;
	bool paused = false, mute = false;
	if (api.paused(pMaster, &paused) == 0 && paused && api.setPaused(pMaster, false) == 0)
	{
		++changed;
		strncat(what, " master unpaused", size - strlen(what) - 1);
	}
	if (api.mute(pMaster, &mute) == 0 && mute && api.setMute(pMaster, false) == 0)
	{
		++changed;
		strncat(what, " master unmuted", size - strlen(what) - 1);
	}
	// (the event system's own master category one level down)
	int n = 0;
	if (api.numGroups(pMaster, &n) == 0)
		for (int i = 0; i < n && i < 32; ++i)
		{
			void* pChild = 0;
			char name[48] = "";
			if (api.group(pMaster, i, &pChild) != 0 || !pChild || api.name(pChild, name, sizeof(name) - 1) != 0)
				continue;
			name[sizeof(name) - 1] = 0;
			if (_stricmp(name, "master") != 0)
				continue;
			if (api.paused(pChild, &paused) == 0 && paused && api.setPaused(pChild, false) == 0)
			{
				++changed;
				strncat(what, " category master unpaused", size - strlen(what) - 1);
			}
			if (api.mute(pChild, &mute) == 0 && mute && api.setMute(pChild, false) == 0)
			{
				++changed;
				strncat(what, " category master unmuted", size - strlen(what) - 1);
			}
		}
	return changed;
}

// In a busy fight a new sound's quiet parts (a shot's far or near layer,
// volume 0 at that distance; or any part once FMOD's 64 mixed voices are all
// taken) go virtual at their start and stay there: not heard, never over.
// Their sound counts as playing for as long as the fight lasts (a 1.5 s shot
// for minutes), every new one adds more, and they start later, seconds late.
// Looked at four times a second: a one-shot channel (no loop, not streamed:
// speech and music are left alone) virtual at its very start for 0.4 s is
// stopped (if stop).
// It also counts, for the log: such parts, those stopped, those that started
// late after all (0.5 s or more) and how late, heard sounds that lost their
// voice in the middle while still audible (the choppy sound), and of the
// one-shots that started how many were heard at once
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

void CoopFmodWatch(void* pSystem, int maxChannels, bool stop, float now, SCoopFmodWatch* pOut)
{
	typedef int(__stdcall* FnGetChannel)(void*, int, void**);
	typedef int(__stdcall* FnGetBool)(void*, bool*);
	typedef int(__stdcall* FnGetUIntUnit)(void*, unsigned int*, unsigned int);
	typedef int(__stdcall* FnGetUInt)(void*, unsigned int*);
	typedef int(__stdcall* FnGetPtr)(void*, void**);
	typedef int(__stdcall* FnGetName)(void*, char*, int);
	typedef int(__stdcall* FnStop)(void*);
	typedef int(__stdcall* FnGetFloat)(void*, float*);
	static FnGetFloat s_audibility = 0;
	static FnGetChannel s_channel = 0;
	static FnGetBool s_isPlaying = 0, s_isVirtual = 0;
	static FnGetUIntUnit s_position = 0;
	static FnGetUInt s_mode = 0, s_soundMode = 0;
	static FnGetPtr s_sound = 0;
	static FnGetName s_name = 0;
	static FnStop s_stop = 0;
	static bool s_looked = false;
	// per channel: its sound at the last look, since when it is stuck at its
	// start (-1: not), whether it was heard, whether it may be stopped
	struct SChannel { void* pSound; float stuckSince, since; bool heard, oneShot, gunLoop; };
	static SChannel s_channels[4096];
	if (!s_looked)
	{
		s_looked = true;
		memset(s_channels, 0, sizeof(s_channels));
		if (HMODULE h = GetModuleHandleA("fmodex.dll"))
		{
			s_channel = (FnGetChannel)GetProcAddress(h, "?getChannel@System@FMOD@@QAG?AW4FMOD_RESULT@@HPAPAVChannel@2@@Z");
			s_isPlaying = (FnGetBool)GetProcAddress(h, "?isPlaying@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PA_N@Z");
			s_isVirtual = (FnGetBool)GetProcAddress(h, "?isVirtual@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PA_N@Z");
			s_position = (FnGetUIntUnit)GetProcAddress(h, "?getPosition@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PAII@Z");
			s_mode = (FnGetUInt)GetProcAddress(h, "?getMode@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PAI@Z");
			s_soundMode = (FnGetUInt)GetProcAddress(h, "?getMode@Sound@FMOD@@QAG?AW4FMOD_RESULT@@PAI@Z");
			s_sound = (FnGetPtr)GetProcAddress(h, "?getCurrentSound@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PAPAVSound@2@@Z");
			s_name = (FnGetName)GetProcAddress(h, "?getName@Sound@FMOD@@QAG?AW4FMOD_RESULT@@PADH@Z");
			s_stop = (FnStop)GetProcAddress(h, "?stop@Channel@FMOD@@QAG?AW4FMOD_RESULT@@XZ");
			s_audibility = (FnGetFloat)GetProcAddress(h, "?getAudibility@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PAM@Z");
		}
	}
	if (!pSystem || !s_channel || !s_isPlaying || !s_isVirtual || !s_position || !s_mode || !s_soundMode || !s_sound || !s_name || !s_stop
		|| !s_audibility || maxChannels <= 0 || maxChannels > 4096 || IsBadReadPtr(pSystem, 64))
		return;
	for (int i = 0; i < maxChannels; ++i)
	{
		SChannel& c = s_channels[i];
		void* pChannel = 0;
		void* pSound = 0;
		bool playing = false, isVirtual = false;
		unsigned int position = 1;
		if (s_channel(pSystem, i, &pChannel) != 0 || !pChannel || s_isPlaying(pChannel, &playing) != 0 || !playing
			|| s_sound(pChannel, &pSound) != 0 || !pSound || s_isVirtual(pChannel, &isVirtual) != 0 || s_position(pChannel, &position, 1) != 0)
		{
			c.pSound = 0;
			continue;
		}
		if (c.pSound != pSound)
		{
			// a new sound on this channel: a one-shot (no loop, not streamed, no speech)?
			c.pSound = pSound;
			c.stuckSince = -1.0f;
			c.heard = false;
			unsigned int mode = 0, soundMode = 0;
			char name[64] = "";
			s_name(pSound, name, sizeof(name) - 1);
			name[sizeof(name) - 1] = 0;
			for (char* p = name; *p; ++p)
				*p = (char)tolower((unsigned char)*p);
			c.oneShot = s_mode(pChannel, &mode) == 0 && !(mode & 0x6) && s_soundMode(pSound, &soundMode) == 0 && !(soundMode & 0x86)	// loops, FMOD_CREATESTREAM
				&& !strstr(name, ".mp2") && !strstr(name, ".mp3") && !strstr(name, ".ogg");
			// a weapon's fire loop (the layer of an automatic weapon's shot
			// that goes on while it fires: fy71_3rd_loop_close..., fy71_cu_loop...,
			// mg_fire_loop...): no weapon fires 15 s without a pause (a rifle's
			// magazine is empty in 3, a mounted gun overheats in about 5)
			c.since = now;
			c.gunLoop = strstr(name, "loop") && (strstr(name, "3rd") || strstr(name, "_cu_") || strstr(name, "_fp") || strstr(name, "fire")
				|| strstr(name, "mg_") || strstr(name, "rapid")) && !strstr(name, "amb") && !strstr(name, "engine")
				// (guns that may fire on longer: a minigun, the Hurricane, turrets,
				// anti-aircraft guns, the aliens' weapons)
				&& !strstr(name, "minigun") && !strstr(name, "mingun") && !strstr(name, "hurricane") && !strstr(name, "turret")
				&& !strstr(name, "aaa_") && !strstr(name, "moac") && !strstr(name, "moar") && !strstr(name, "spin");
			// a one-shot just started (at most a look ago): heard right away?
			if (c.oneShot)
			{
				++pOut->started;
				pOut->startedHeard += !isVirtual;
			}
		}
		// a gunfire loop that went on and on: nobody fires that long (a fire
		// loop whose weapon was never told to stop played for ever, the
		// user's game of 2026-10-17)
		if (c.gunLoop && stop && now - c.since >= 15.0f)
		{
			char name[64] = "";
			s_name(pSound, name, sizeof(name) - 1);
			name[sizeof(name) - 1] = 0;
			for (char* p = name; *p; ++p)
				if ((unsigned char)*p < 32 || (unsigned char)*p > 126)
					*p = 0;
			if (s_stop(pChannel) == 0)
			{
				++pOut->gunStopped;
				++pOut->gunNew;
				const size_t used = strlen(pOut->gunNames);
				if (used < sizeof(pOut->gunNames) - 40)
				{
					_snprintf(pOut->gunNames + used, sizeof(pOut->gunNames) - used - 1, " %s(%.0f s)", name, now - c.since);
					pOut->gunNames[sizeof(pOut->gunNames) - 1] = 0;
				}
			}
			c.pSound = 0;
			continue;
		}
		if (isVirtual && position == 0 && c.oneShot)
		{
			if (c.stuckSince < 0.0f)
			{
				c.stuckSince = now;
				++pOut->stuck;
			}
			else if (stop && now - c.stuckSince >= 0.4f)
			{
				if (pOut->stopped < 4)
				{
					char name[64] = "";
					s_name(pSound, name, sizeof(name) - 1);
					name[sizeof(name) - 1] = 0;
					for (char* p = name; *p; ++p)
						if ((unsigned char)*p < 32 || (unsigned char)*p > 126)
							*p = 0;
					const size_t used = strlen(pOut->names);
					_snprintf(pOut->names + used, sizeof(pOut->names) - used - 1, " %s", name);
					pOut->names[sizeof(pOut->names) - 1] = 0;
				}
				if (s_stop(pChannel) == 0)
					++pOut->stopped;
				c.pSound = 0;
			}
			continue;
		}
		if (!isVirtual)
		{
			// heard now: after being stuck at its start, it comes late
			if (c.stuckSince >= 0.0f && now - c.stuckSince >= 0.5f)
			{
				++pOut->late;
				if (now - c.stuckSince > pOut->latest)
					pOut->latest = now - c.stuckSince;
			}
			c.stuckSince = -1.0f;
			c.heard = true;
		}
		else
		{
			// virtual in the middle of the sound after being heard, still
			// loud enough to be heard (not a layer faded out by distance):
			// choppy
			float audibility = 0.0f;
			if (c.heard && position > 0 && s_audibility(pChannel, &audibility) == 0 && audibility > 0.001f)
			{
				++pOut->takenAway;
				++pOut->lostLoudness[audibility < 0.02f ? 0 : audibility < 0.1f ? 1 : 2];
				if (audibility >= 0.1f && strlen(pOut->lostNames) < 200)
				{
					char name[48] = "";
					s_name(pSound, name, sizeof(name) - 1);
					name[sizeof(name) - 1] = 0;
					for (char* p = name; *p; ++p)
						if ((unsigned char)*p < 32 || (unsigned char)*p > 126)
							*p = 0;
					const size_t used = strlen(pOut->lostNames);
					_snprintf(pOut->lostNames + used, sizeof(pOut->lostNames) - used - 1, " %s(%.2f,%u ms)", name, audibility, position);
					pOut->lostNames[sizeof(pOut->lostNames) - 1] = 0;
				}
			}
			c.heard = false;
			// it plays (virtual): not stuck at its start any more
			if (position > 0)
				c.stuckSince = -1.0f;
		}
	}
}
