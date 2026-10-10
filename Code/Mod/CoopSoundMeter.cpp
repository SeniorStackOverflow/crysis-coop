// Crysis Coop: how loud this game's sound comes out of Windows (Core Audio:
// the peak of this process's audio sessions on the default output device).
// Apart from the game's headers, like CoopJpeg.cpp.
#include <windows.h>
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
