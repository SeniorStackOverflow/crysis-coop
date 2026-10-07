// Crysis Coop launcher (CrysisCoop.exe): starts the game with the co-op mod,
// the way the game's own shortcut starts the plain game:
//
//   Bin32\Crysis.exe -mod Coop -dx9 [the launcher's own arguments]
//
// The installer puts it into the Crysis folder (it also works from Bin32 or
// Mods\Coop). Its arguments are passed on: +coop_join 123456, -dx10... Before
// starting it checks what the mod needs (C1-Launcher, Coop.dll, the co-op
// levels) and says what is missing instead of a game that silently starts
// without the mod.
//
// -coop_check: only the checks, no window and no game; the exit code says
// what is missing (0 ready, see EExit). The installer uses it.
//
// The game runs on 4 CPUs (GameAffinity: Crysis plays no sound on CPUs with
// many; -coop_allcpus gives it all of them).
//
// Before the game starts, the mod updates itself when a newer release is out
// (Update.cpp; -coop_noupdate skips it, -coop_update_only updates without
// starting the game: exit code 0 up to date, 10 updated, 11 skipped, 12
// failed).
//
// -coop_mcp: no game, an MCP server on stdio for AI agents who play the AI
// companion (Mcp.h).
#include <windows.h>

#include <string>
#include <vector>

#include "Mcp.h"
#include "Update.h"

namespace
{
	enum EExit
	{
		eExit_Ok,
		eExit_NoGame = 2,
		eExit_NoMod,
		eExit_NoLevels,
		eExit_NoC1Launcher,
		eExit_StartFailed,
	};

	bool s_quiet = false;           // -coop_check

	bool Exists(const std::wstring& path)
	{
		return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
	}

	std::wstring Lower(std::wstring s)
	{
		for (wchar_t& c : s)
			c = (wchar_t)towlower(c);
		return s;
	}

	// a whole word of the command line ("-dx10", not a part of "+x-dx10y")
	bool HasArg(const std::wstring& args, const wchar_t* arg)
	{
		const std::wstring a = L" " + Lower(args) + L" ";
		return a.find(L" " + std::wstring(arg) + L" ") != std::wstring::npos;
	}

	int Fail(EExit code, const wchar_t* english, const wchar_t* russian, const std::wstring& detail = L"")
	{
		if (s_quiet)
			return code;
		const bool ru = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
		std::wstring text = ru ? russian : english;
		if (!detail.empty())
			text += L"\n\n" + detail;
		MessageBoxW(nullptr, text.c_str(), L"Crysis Coop", MB_OK | MB_ICONERROR);
		return code;
	}

	// the Crysis folder: the launcher's own one, or one above it (Bin32,
	// Mods\Coop)
	std::wstring FindGame()
	{
		std::vector<wchar_t> path(32768);
		const DWORD n = GetModuleFileNameW(nullptr, path.data(), (DWORD)path.size());
		std::wstring dir(path.data(), n);
		for (int up = 0; up < 4; ++up)
		{
			const size_t slash = dir.find_last_of(L"\\/");
			if (slash == std::wstring::npos)
				break;
			dir.resize(slash);
			if (Exists(dir + L"\\Bin32\\Crysis.exe") && Exists(dir + L"\\Game"))
				return dir;
		}
		return L"";
	}

	// Crysis plays no sound on CPUs with many logical processors (the old
	// CrysisLauncher.exe's fix, 12 of them on the user's PC). The game gets
	// 4, one per physical core where the system says which is which, so
	// hyper-threads do not share a core
	DWORD_PTR GameAffinity()
	{
		const int COUNT = 4;
		DWORD_PTR process = 0, system = 0;
		if (!GetProcessAffinityMask(GetCurrentProcess(), &process, &system) || !process)
			return 0;
		int available = 0;
		for (DWORD_PTR m = process; m; m &= m - 1)
			++available;
		if (available <= COUNT)
			return 0;
		DWORD_PTR mask = 0;
		int n = 0;
		DWORD len = 0;
		GetLogicalProcessorInformation(nullptr, &len);
		std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> cores(len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) + 1);
		len = (DWORD)(cores.size() * sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
		if (GetLogicalProcessorInformation(cores.data(), &len))
			for (DWORD i = 0; i < len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) && n < COUNT; ++i)
			{
				const DWORD_PTR own = cores[i].ProcessorMask & process;
				if (cores[i].Relationship == RelationProcessorCore && own)
				{
					mask |= own & (~own + 1);       // the core's first logical processor
					++n;
				}
			}
		// fewer cores than that (or no information): more logical processors
		for (DWORD_PTR bit = 1; bit && n < COUNT; bit <<= 1)
			if ((process & bit) && !(mask & bit))
			{
				mask |= bit;
				++n;
			}
		return mask;
	}

	std::wstring ProductName(const std::wstring& file)
	{
		DWORD handle = 0;
		const DWORD size = GetFileVersionInfoSizeW(file.c_str(), &handle);
		if (!size)
			return L"";
		std::vector<BYTE> info(size);
		if (!GetFileVersionInfoW(file.c_str(), 0, size, info.data()))
			return L"";
		struct SLang { WORD language, codepage; }* langs = nullptr;
		UINT len = 0;
		if (!VerQueryValueW(info.data(), L"\\VarFileInfo\\Translation", (void**)&langs, &len) || len < sizeof(SLang))
			return L"";
		wchar_t key[64];
		wsprintfW(key, L"\\StringFileInfo\\%04x%04x\\ProductName", langs[0].language, langs[0].codepage);
		wchar_t* value = nullptr;
		if (!VerQueryValueW(info.data(), key, (void**)&value, &len) || !value)
			return L"";
		return value;
	}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR args, int)
{
	const std::wstring extra = args ? args : L"";
	// an MCP server for AI agents (stdio), no game: see Mcp.h
	if (HasArg(extra, L"-coop_mcp"))
		return Mcp::Run(extra);
	s_quiet = HasArg(extra, L"-coop_check");
	const std::wstring game = FindGame();
	if (game.empty())
	{
		return Fail(eExit_NoGame, L"Crysis was not found. CrysisCoop.exe must be in the Crysis folder (the one with Bin32 and Game in it).",
			L"Crysis не найден. CrysisCoop.exe должен лежать в папке Crysis (в той, где Bin32 и Game).");
	}
	const std::wstring exe = game + L"\\Bin32\\Crysis.exe";
	const std::wstring mod = game + L"\\Mods\\Coop";
	const wchar_t* reinstall = L"install.bat: https://github.com/SeniorStackOverflow/crysis-coop/releases/latest";
	if (!Exists(mod + L"\\Bin32\\Coop.dll"))
	{
		return Fail(eExit_NoMod, L"The Crysis Coop mod is not installed (Mods\\Coop\\Bin32\\Coop.dll is missing). Run the mod's installer:",
			L"Мод Crysis Coop не установлен (нет Mods\\Coop\\Bin32\\Coop.dll). Запустите установщик мода:", reinstall);
	}
	if (!s_quiet)
	{
		const Update::EResult update = Update::Run(game, extra);
		static const int UPDATE_EXIT[] = { 0, 10, 11, 12 };      // up to date, updated, skipped, failed
		if (HasArg(extra, L"-coop_update_only"))
			return UPDATE_EXIT[update];
	}
	if (!Exists(mod + L"\\Game\\Levels\\Multiplayer\\TIA\\coop_island\\coop_island.xml"))
	{
		return Fail(eExit_NoLevels, L"The co-op levels are missing. The mod's installer builds them from the game's levels, run it again:",
			L"Нет кооперативных уровней. Их собирает из уровней игры установщик мода, запустите его ещё раз:", reinstall);
	}
	if (ProductName(exe) != L"C1-Launcher")
	{
		return Fail(eExit_NoC1Launcher, L"Bin32\\Crysis.exe is not C1-Launcher, without it the game cannot load the mod. The mod's installer puts it in (and keeps the original):",
			L"Bin32\\Crysis.exe - не C1-Launcher, без него игра не загрузит мод. Установщик мода ставит его (оригинал сохраняется):", reinstall);
	}
	if (s_quiet)
		return eExit_Ok;

	std::wstring cmd = L"\"" + exe + L"\"";
	if (!HasArg(extra, L"-mod"))
		cmd += L" -mod Coop";
	if (!HasArg(extra, L"-dx9") && !HasArg(extra, L"-dx10"))
		cmd += L" -dx9";
	if (!extra.empty())
		cmd += L" " + extra;

	// the way this launcher was started (a shortcut set to "minimized"...)
	// goes on to the game
	STARTUPINFOW own = {};
	GetStartupInfoW(&own);
	STARTUPINFOW si = {};
	si.cb = sizeof(si);
	si.dwFlags = own.dwFlags & STARTF_USESHOWWINDOW;
	si.wShowWindow = own.wShowWindow;
	PROCESS_INFORMATION pi = {};
	std::vector<wchar_t> line(cmd.begin(), cmd.end());
	line.push_back(0);
	// started suspended: the CPUs are set before the game's first instruction
	// (-coop_allcpus: all of them)
	const DWORD_PTR affinity = HasArg(extra, L"-coop_allcpus") ? 0 : GameAffinity();
	if (!CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, affinity ? CREATE_SUSPENDED : 0, nullptr, game.c_str(), &si, &pi))
	{
		wchar_t code[32];
		wsprintfW(code, L" (error %lu)", GetLastError());
		return Fail(eExit_StartFailed, L"The game could not be started:", L"Не удалось запустить игру:", exe + code);
	}
	if (affinity)
	{
		SetProcessAffinityMask(pi.hProcess, affinity);
		ResumeThread(pi.hThread);
	}
	AllowSetForegroundWindow(pi.dwProcessId);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return eExit_Ok;
}
