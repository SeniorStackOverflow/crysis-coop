#include "StdAfx.h"
#include "CoopPak.h"
#include "ICryPak.h"
#include "IStreamEngine.h"
#include "IConsole.h"
#include <windows.h>

// Crysis Coop: CryPak keeps one CCachedFileData per pak file in use (a
// reference count at +0, no lock round it). CCryPak::GetFileData finds the
// file's one in its set and adds a reference with a plain InterlockedIncrement
// - also when another thread has just let go of the last reference and waits
// for the pak's lock to take it out of the set (the count is 0 then). The
// "Streaming" thread so took a file that was being destroyed: the destructor
// zeroed its data, zip and entry and freed it, and the next read of it
// (CCachedFileData::GetData) read the entry's size from 0 - CrySystem+0xB69C,
// "Read from 0x00000008". The user's game of 2026-10-22, the autotester's runs
// of 10-07, 10-15 (twice): the same registers each time; vanilla's own code,
// no mod code on the way (the investigation of 2026-10-22).
// The fix: GetFileData's found branch takes a reference only while the count
// is above 0 (compare and swap); a dying one is left alone and the file gets a
// new one, the branch GetFileData takes for a file not in the set. CrySystem
// 6156 only: its bytes are checked first, another CrySystem is left as it is
namespace
{
	const unsigned FOUND_RVA = 0xEAC3;      // mov edx,[edx+10h]; test edx,edx (the found node's CCachedFileData)
	const unsigned STORED_RVA = 0xEAD9;     // after the reference: mov ecx,[ebp+10h] (unlock and return)
	const unsigned CREATE_RVA = 0xEB06;     // a new one: push 18h; call (operator new)
	const unsigned char FOUND_BYTES[22] = {
		0x8B, 0x52, 0x10,               // +EAC3 mov edx,[edx+10h]
		0x85, 0xD2,                     // +EAC6 test edx,edx
		0x8B, 0x74, 0x24, 0x34,         // +EAC8 mov esi,[esp+34h]
		0x89, 0x16,                     // +EACC mov [esi],edx
		0x74, 0x09,                     // +EACE je +EAD9
		0x52,                           // +EAD0 push edx
		0xE8, 0x8A, 0xA1, 0x00, 0x00,   // +EAD1 call InterlockedIncrement (+18C60)
		0x83, 0xC4, 0x04 };             // +EAD6 add esp,4
	const unsigned char STORED_BYTES[3] = { 0x8B, 0x4D, 0x10 };   // mov ecx,[ebp+10h]
	const unsigned char CREATE_BYTES[2] = { 0x6A, 0x18 };         // push 18h

	DWORD s_stored = 0;          // CrySystem+0xEAD9
	volatile LONGLONG* s_qword = 0;   // the aligned 8 bytes round +EAC3
	LONGLONG s_original = 0, s_fixed = 0;
	DWORD s_create = 0;          // CrySystem+0xEB06
	volatile LONG s_races = 0;   // dying ones not taken (the Streaming thread or another)
	LONG s_racesLogged = 0;
	char s_raceFile[260] = "";   // the last one's file (no log while the pak's locks are held)
	bool s_patched = false;

	// on GetFileData's thread with the pak's two locks held: nothing that
	// waits, no allocation
	void __stdcall RecordRace(const char* name)
	{
		InterlockedIncrement(&s_races);
		if (name)
		{
			size_t i = 0;
			for (; i + 1 < sizeof(s_raceFile) && name[i]; ++i)
				s_raceFile[i] = name[i];
			s_raceFile[i] = 0;
		}
	}

	// in place of +EAC3..+EAD8 (jumped to with GetFileData's stack and
	// registers: edx the set's node, esi the FileEntry, ebp the CCryPak, ebx
	// the zip's place: what +EB06 uses)
	__declspec(naked) void TakeIfAlive()
	{
		__asm
		{
			mov edx, [edx + 10h]
			test edx, edx
			jz store
		retry:
			mov eax, [edx]
			test eax, eax
			jle dying
			lea ecx, [eax + 1]
			lock cmpxchg dword ptr [edx], ecx
			jnz retry
		store:
			// (esi the place for the answer, as the original: GetFileData
			// returns it)
			mov esi, [esp + 34h]
			mov [esi], edx
			jmp dword ptr [s_stored]
		dying:
			// (its name is GetFileData's first argument: [esp+38h] here, +20h
			// for the pushad)
			pushad
			push dword ptr [esp + 58h]
			call RecordRace
			popad
			jmp dword ptr [s_create]
		}
	}

	// ---- testing: makes the race happen (coop_test_pakrace) ----------------
	// A thread holds the pak's set lock (CCryPak+0x10) while a stream of the
	// file asks for it on the Streaming thread and this test's thread lets
	// go of the file's last reference: when the lock is free both go on, in
	// either order. Without the fix a read of a destroyed file follows about
	// every other time (the crash at +B69C, or +A34E0 later); with it the
	// count goes up
	struct STestRace
	{
		string file;
		int rounds;
		volatile LONG done;
		volatile LONG streams;
		LONG racesBefore;
	};
	STestRace* s_test = 0;
	HANDLE s_testThread = 0;

	CRITICAL_SECTION* PakSetLock()
	{
		// gEnv->pCryPak is the CCryPak (no other base before ICryPak): its
		// critical section pointer at +0x10, as GetFileData's lock +EA7F
		return gEnv->pCryPak ? *(CRITICAL_SECTION**)((char*)gEnv->pCryPak + 0x10) : 0;
	}

	struct SStreamDone : public IStreamCallback
	{
		volatile LONG finished;
		unsigned error;
		SStreamDone(): finished(0), error(0) {}
		virtual void StreamOnComplete(IReadStream* pStream, unsigned nError) { error = nError; InterlockedExchange(&finished, 1); }
	};

	DWORD WINAPI TestRaceThread(void* p)
	{
		STestRace* t = (STestRace*)p;
		CRITICAL_SECTION* pLock = PakSetLock();
		for (int i = 0; i < t->rounds && pLock; ++i)
		{
			// the file's one reference: an open file
			FILE* f = gEnv->pCryPak->FOpen(t->file.c_str(), "rb");
			if (!f)
				break;
			EnterCriticalSection(pLock);
			// (the callback comes on the main thread, maybe after this round:
			// never freed, a few bytes a round)
			SStreamDone& done = *new SStreamDone;
			StreamReadParams params;
			params.nFlags = SRP_QUICK_STARTREAD;
			IReadStreamPtr pStream = gEnv->pSystem->GetStreamEngine()->StartRead("CoopTest", t->file.c_str(), &done, &params);
			// (the Streaming thread is in GetFileData now, waiting for the lock)
			Sleep(15);
			HANDLE hRelease = 0;
			struct SClose { FILE* f; static DWORD WINAPI Run(void* p) { gEnv->pCryPak->FClose(((SClose*)p)->f); return 0; } } close = { f };
			// (the last reference let go on its own thread: it waits for the
			// lock in the destructor)
			hRelease = CreateThread(0, 64 * 1024, &SClose::Run, &close, 0, 0);
			Sleep(15);
			LeaveCriticalSection(pLock);
			if (hRelease)
			{
				WaitForSingleObject(hRelease, 5000);
				CloseHandle(hRelease);
			}
			for (int w = 0; w < 200 && !done.finished; ++w)
				Sleep(5);
			if (done.finished)
				InterlockedIncrement(&t->streams);
			pStream = 0;
			// (the stream's own references go on the main thread's update)
			Sleep(60);
		}
		InterlockedExchange(&t->done, 1);
		return 0;
	}

	void CmdTestRace(IConsoleCmdArgs* pArgs)
	{
		if (s_test && !s_test->done)
		{
			CryLogAlways("[CoopPak] a race test is running");
			return;
		}
		if (s_testThread)
		{
			WaitForSingleObject(s_testThread, 5000);
			CloseHandle(s_testThread);
			s_testThread = 0;
		}
		delete s_test;
		s_test = new STestRace;
		s_test->rounds = pArgs->GetArgCount() > 1 ? max(1, atoi(pArgs->GetArg(1))) : 40;
		s_test->file = pArgs->GetArgCount() > 2 ? pArgs->GetArg(2) : "Libs/EntityArchetypes/Props.xml";
		s_test->done = 0;
		s_test->streams = 0;
		s_test->racesBefore = s_races;
		CryLogAlways("[CoopPak] race test: %d rounds with %s (the pak reader is %s; set lock %p)", s_test->rounds, s_test->file.c_str(),
			s_patched ? "fixed" : "NOT fixed", (void*)PakSetLock());
		s_testThread = CreateThread(0, 256 * 1024, TestRaceThread, s_test, 0, 0);
	}
}

namespace
{
	// the fix on or off (the original bytes back: testing)
	bool Apply(bool on)
	{
		if (!s_qword)
			return false;
		DWORD old = 0;
		if (!VirtualProtect((void*)s_qword, 8, PAGE_EXECUTE_READWRITE, &old))
			return false;
		const LONGLONG want = on ? s_fixed : s_original, was = on ? s_original : s_fixed;
		const bool ok = *s_qword == want || InterlockedCompareExchange64(s_qword, want, was) == was;
		VirtualProtect((void*)s_qword, 8, old, &old);
		FlushInstructionCache(GetCurrentProcess(), (void*)s_qword, 8);
		if (ok)
			s_patched = on;
		return ok;
	}

	void OnFixChanged(ICVar* pVar)
	{
		const bool on = pVar->GetIVal() != 0;
		if (!s_qword)
			CryLogAlways("[CoopPak] the pak reader's race cannot be fixed here");
		else if (Apply(on))
			CryLogAlways(on ? "[CoopPak] the pak reader's race is fixed again" : "[CoopPak] the pak reader's race is NOT fixed now (testing)");
	}
}

void CoopPak::Init()
{
	if (gEnv->pConsole)
		gEnv->pConsole->AddCommand("coop_test_pakrace", CmdTestRace, 0,
			"Crysis Coop testing: coop_test_pakrace [rounds] [file] - makes CryPak's cached-file race happen (a stream and a last release at once)");
	ICVar* pFix = gEnv->pConsole ? gEnv->pConsole->RegisterInt("coop_pak_fix", 1, 0,
		"Crysis Coop: 1 = CryPak takes a cached pak file only while it is alive (a crash in the Streaming thread otherwise); 0 = as the game was (testing)",
		OnFixChanged) : 0;
	HMODULE h = GetModuleHandleA("CrySystem.dll");
	unsigned char* found = h ? (unsigned char*)h + FOUND_RVA : 0;
	if (!found || memcmp(found, FOUND_BYTES, sizeof(FOUND_BYTES)) || memcmp((unsigned char*)h + STORED_RVA, STORED_BYTES, sizeof(STORED_BYTES))
		|| memcmp((unsigned char*)h + CREATE_RVA, CREATE_BYTES, sizeof(CREATE_BYTES))
		|| (DWORD)(found + 14 + 5) + *(int*)(found + 15) != (DWORD)h + 0x18C60)
	{
		CryLogAlways("[CoopPak] the pak reader's race is not fixed (another CrySystem.dll)");
		return;
	}
	s_stored = (DWORD)h + STORED_RVA;
	s_create = (DWORD)h + CREATE_RVA;
	// the 5 bytes at +EAC3 in one 8-byte write of the aligned qword at +EAC0
	// (the Streaming thread may already run: never half a jump)
	volatile LONGLONG* qword = (volatile LONGLONG*)((DWORD)found & ~7u);
	const unsigned at = (unsigned)((DWORD)found - (DWORD)qword);
	if (at > 3)
	{
		CryLogAlways("[CoopPak] the pak reader's race is not fixed (no write)");
		return;
	}
	s_qword = qword;
	s_original = *qword;
	s_fixed = s_original;
	unsigned char* b = (unsigned char*)&s_fixed;
	b[at] = 0xE9;
	*(int*)(b + at + 1) = (int)((DWORD)&TakeIfAlive - (DWORD)(found + 5));
	const bool ok = (!pFix || pFix->GetIVal()) && Apply(true);
	CryLogAlways(ok ? "[CoopPak] the pak reader takes a cached file only while it is alive (the Streaming thread crash of CrySystem+0xB69C)"
		: "[CoopPak] the pak reader's race is not fixed (coop_pak_fix 0, or the code changed while it was written)");
}

void CoopPak::Update()
{
	const LONG races = s_races;
	if (races != s_racesLogged)
	{
		s_racesLogged = races;
		char file[sizeof(s_raceFile)];
		memcpy(file, s_raceFile, sizeof(file));
		file[sizeof(file) - 1] = 0;
		CryLogAlways("[CoopPak] a pak file was being destroyed while another thread opened it: %s (%d so far; a new one was made, no crash)", file, (int)races);
	}
	if (s_test && s_test->done == 1)
	{
		s_test->done = 2;
		CryLogAlways("[CoopPak] race test over: %d rounds, %d streams read, %d races caught", s_test->rounds, (int)s_test->streams,
			(int)(s_races - s_test->racesBefore));
	}
}
