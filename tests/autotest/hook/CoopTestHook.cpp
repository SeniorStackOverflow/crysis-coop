// CoopTestHook.dll: the autotester's window hook for a test game it starts.
//
// The engine makes and shows its window long before it loads the mod's DLL
// (whose own hook keeps test windows from the foreground), and a game started
// while the person had not touched the keyboard for a few minutes took the
// foreground for a second, on screen (2026-10-15). The autotester starts the
// game suspended, a 32-bit helper loads this DLL and hooks the game's main
// thread with CoopTestHookWait (Windows then loads this DLL into the game
// when the thread first calls into the window system), and only then the game
// runs. Its top-level windows are made left of all screens, never active,
// without a taskbar button, and kept there (the engine centres its window
// when it sets the video mode, and sets its own extended style).
//
// No engine dependencies: Coop.dll itself cannot be loaded outside the game
// (its memory manager needs CrySystem.dll and says so in a message box).
#include <windows.h>
#include <commctrl.h>

#pragma comment(lib, "comctl32.lib")

namespace
{
	HINSTANCE s_inst = 0;

	// coop_test_deactivate's empty window (CoopAgent.h COOP_TEST_INACTIVE_CLASS)
	// may be the active one: the game's window then is not
	bool MayActivate(HWND hWnd)
	{
		char cls[32];
		return GetClassNameA(hWnd, cls, sizeof(cls)) && !strcmp(cls, "CoopTestInactive");
	}

	int OffScreenX(int width)
	{
		return GetSystemMetrics(SM_XVIRTUALSCREEN) - (width > 0 && width < 8192 ? width : 1024) - 3000;
	}

	// a top-level window stays left of all screens, a tool window that is never active
	LRESULT CALLBACK KeepAway(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
	{
		if (msg == WM_WINDOWPOSCHANGING)
		{
			WINDOWPOS* wp = reinterpret_cast<WINDOWPOS*>(lParam);
			if (!(wp->flags & SWP_NOMOVE))
			{
				RECT r;
				GetWindowRect(hWnd, &r);
				const int w = (wp->flags & SWP_NOSIZE) ? r.right - r.left : wp->cx;
				wp->x = OffScreenX(w);
				wp->y = GetSystemMetrics(SM_YVIRTUALSCREEN);
			}
			wp->flags |= SWP_NOACTIVATE;
		}
		else if (msg == WM_STYLECHANGING && wParam == (WPARAM)GWL_EXSTYLE)
		{
			STYLESTRUCT* st = reinterpret_cast<STYLESTRUCT*>(lParam);
			st->styleNew = (st->styleNew | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE) & ~WS_EX_APPWINDOW;
		}
		else if (msg == WM_NCDESTROY)
			RemoveWindowSubclass(hWnd, KeepAway, 7);
		return DefSubclassProc(hWnd, msg, wParam, lParam);
	}

	LRESULT CALLBACK NoActivate(int code, WPARAM wParam, LPARAM lParam)
	{
		if (code == HCBT_ACTIVATE && !MayActivate((HWND)wParam))
			return 1;
		if (code == HCBT_CREATEWND)
		{
			CBT_CREATEWNDW* pCreate = reinterpret_cast<CBT_CREATEWNDW*>(lParam);
			if (pCreate && pCreate->lpcs && !(pCreate->lpcs->style & WS_CHILD) && !MayActivate((HWND)wParam))
			{
				pCreate->lpcs->dwExStyle = (pCreate->lpcs->dwExStyle | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW) & ~WS_EX_APPWINDOW;
				pCreate->lpcs->x = OffScreenX(pCreate->lpcs->cx);
				pCreate->lpcs->y = GetSystemMetrics(SM_YVIRTUALSCREEN);
				// (the extended style in the create structure is not always taken)
				SetWindowLongW((HWND)wParam, GWL_EXSTYLE, pCreate->lpcs->dwExStyle);
				SetWindowSubclass((HWND)wParam, KeepAway, 7, 0);
			}
		}
		return CallNextHookEx(0, code, wParam, lParam);
	}
}

extern "C" __declspec(dllexport) HHOOK CoopTestHook(DWORD threadId)
{
	return SetWindowsHookExW(WH_CBT, NoActivate, s_inst, threadId);
}

// A thread takes a hook only once it has made its first call into the window
// system (a suspended one never): tried every millisecond until it works, up
// to timeoutMs; how long it took in *pMs
extern "C" __declspec(dllexport) HHOOK CoopTestHookWait(DWORD threadId, DWORD timeoutMs, DWORD* pMs)
{
	const DWORD start = GetTickCount();
	HHOOK hook = 0;
	while (!hook && GetTickCount() - start < timeoutMs)
	{
		hook = SetWindowsHookExW(WH_CBT, NoActivate, s_inst, threadId);
		if (!hook)
			Sleep(1);
	}
	if (pMs)
		*pMs = GetTickCount() - start;
	return hook;
}

BOOL APIENTRY DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		s_inst = hInst;
		DisableThreadLibraryCalls(hInst);
		// loaded into the game by the hook: it stays (its windows keep this
		// DLL's subclass after the helper's hook ends, which would unload it)
		HMODULE self = 0;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&KeepAway), &self);
	}
	return TRUE;
}
