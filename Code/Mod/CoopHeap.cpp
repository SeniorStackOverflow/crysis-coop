// Crysis Coop: a watch on the engine's heap in the autotester's games (see
// CoopHeap.h).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "StdAfx.h"
#include "CoopHeap.h"

#include <malloc.h>

namespace
{
	typedef int (__cdecl *THeapChk)();
	typedef int (__cdecl *THeapWalk)(_HEAPINFO*);
	THeapWalk s_heapwalk = 0;

	// 0 not looked yet, 1 on, -1 off
	int s_state = 0;
	THeapChk s_heapchk = 0;
	const char* s_lastGood = "start";
	bool s_told = false;
	int s_checks = 0;
	LARGE_INTEGER s_spent = { 0 };
	int s_frame = 0;

	bool On()
	{
		if (s_state == 0)
		{
			s_state = -1;
			// the autotester's games only: the check walks the whole heap
			char value[8];
			const DWORD n = GetEnvironmentVariableA("COOP_AUTOTEST", value, sizeof(value));
			if (n > 0 && n < sizeof(value) && value[0] != '0')
			{
				// (the engine's CRT: CrySystem turns its small-block heap on;
				// Coop.dll links its own CRT, its malloc is not this one)
				if (HMODULE crt = GetModuleHandleA("msvcr80.dll"))
				{
					s_heapchk = (THeapChk)GetProcAddress(crt, "_heapchk");
					s_heapwalk = (THeapWalk)GetProcAddress(crt, "_heapwalk");
				}
				if (s_heapchk)
					s_state = 1;
			}
		}
		return s_state == 1;
	}
}

void CoopHeap::Check(const char* where)
{
	if (s_told || !On())
		return;
	LARGE_INTEGER t0, t1;
	QueryPerformanceCounter(&t0);
	// _HEAPOK -2, _HEAPEMPTY -1 (no heap yet: whole); _HEAPBADBEGIN -3,
	// _HEAPBADNODE -4, _HEAPBADPTR -6: damaged
	const int result = s_heapchk();
	QueryPerformanceCounter(&t1);
	s_spent.QuadPart += t1.QuadPart - t0.QuadPart;
	++s_checks;
	if (result == -2 || result == -1)
	{
		s_lastGood = where;
		return;
	}
	s_told = true;
	CryLogAlways("[CoopBug] the engine's heap is damaged (_heapchk %d): first seen at \"%s\", still whole at \"%s\" (check %d)",
		result, where, s_lastGood, s_checks);
	// where: the heap walked to the bad entry, the bytes round it (what wrote
	// over the bookkeeping: text of whose string?)
	if (s_heapwalk)
	{
		_HEAPINFO hi;
		hi._pentry = 0;
		int r = _HEAPOK, n = 0;
		int* last = 0;
		size_t lastSize = 0;
		int lastUse = 0;
		while ((r = s_heapwalk(&hi)) == _HEAPOK && n < 2000000)
		{
			last = hi._pentry;
			lastSize = hi._size;
			lastUse = hi._useflag;
			++n;
		}
		CryLogAlways("[CoopBug] heap walk: %d entries whole, then %d; the last whole entry %p (%u bytes, %s), the next %p",
			n, r, last, (unsigned)lastSize, lastUse == _USEDENTRY ? "in use" : "free", hi._pentry);
		const unsigned char* at[2] = { (const unsigned char*)last, (const unsigned char*)hi._pentry };
		for (int k = 0; k < 2; ++k)
		{
			const unsigned char* p = at[k];
			if (!p)
				continue;
			for (int row = -2; row < 6; ++row)
			{
				const unsigned char* q = p + row * 16;
				if (IsBadReadPtr(q, 16))
					continue;
				char hex[64], text[20];
				int h = 0;
				for (int i = 0; i < 16; ++i)
				{
					h += _snprintf(hex + h, sizeof(hex) - h, "%02X", q[i]);
					text[i] = q[i] >= 32 && q[i] < 127 ? (char)q[i] : '.';
				}
				text[16] = 0;
				CryLogAlways("[CoopBug]   %s %p: %s  %s", k ? "bad " : "last", q, hex, text);
			}
		}
	}
}

void CoopHeap::CheckFrame(bool levelReady)
{
	if (!On())
		return;
	++s_frame;
	if (levelReady && s_frame % 30 != 0)
		return;
	Check(levelReady ? "a frame in the game" : "a frame before the level is there");
	// (once: what the watch costs)
	static bool s_costTold = false;
	if (levelReady && !s_costTold && s_checks > 0)
	{
		s_costTold = true;
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		CryLogAlways("[CoopTest] heap watch: %d checks so far, %.2f ms each on average, the heap %s", s_checks,
			1000.0 * s_spent.QuadPart / f.QuadPart / s_checks, s_told ? "DAMAGED" : "whole");
	}
}
