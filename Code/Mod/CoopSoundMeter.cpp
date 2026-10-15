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
// Core Audio does not answer); *pActive: one of them plays
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

	float best = -1.0f;
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
					best = best > peak ? best : peak;
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
	struct SChannel { void* pSound; float stuckSince; bool heard, oneShot; };
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
			// a one-shot just started (at most a look ago): heard right away?
			if (c.oneShot)
			{
				++pOut->started;
				pOut->startedHeard += !isVirtual;
			}
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
