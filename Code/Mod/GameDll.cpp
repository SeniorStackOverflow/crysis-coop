/*************************************************************************
	Crytek Source File.
	Copyright (C), Crytek Studios, 2001-2004.
	-------------------------------------------------------------------------
	$Id$
	$DateTime$
	Description: Game DLL entry point.

	-------------------------------------------------------------------------
	History:
	- 2:8:2004   10:38 : Created by Marcio Martins

*************************************************************************/
#include "StdAfx.h"
#include "Game.h"

#include <CryLibrary.h>
#include <platform_impl.h>
#include <windows.h>

// Crysis Coop: a test instance (the tests start it with coop_test_free_cursor)
// never becomes the active window. Somebody works at the computer while two
// of them run: they must not take the keyboard focus or the mouse
namespace
{
	HHOOK s_noActivateHook = 0;

	LRESULT CALLBACK NoActivate(int code, WPARAM wParam, LPARAM lParam)
	{
		if (code == HCBT_ACTIVATE)
			return 1;
		if (code == HCBT_CREATEWND)
		{
			CBT_CREATEWNDA* pCreate = reinterpret_cast<CBT_CREATEWNDA*>(lParam);
			if (pCreate && pCreate->lpcs && !(pCreate->lpcs->style & WS_CHILD))
				pCreate->lpcs->dwExStyle |= WS_EX_NOACTIVATE;
		}
		return CallNextHookEx(s_noActivateHook, code, wParam, lParam);
	}

	BOOL CALLBACK NoActivateStyle(HWND hWnd, LPARAM)
	{
		SetWindowLongA(hWnd, GWL_EXSTYLE, GetWindowLongA(hWnd, GWL_EXSTYLE) | WS_EX_NOACTIVATE);
		return TRUE;
	}
}

// DllMain (the window may not exist yet) and CreateGame (it does)
void CoopKeepTestWindowBehind(bool windowExists)
{
	if (!strstr(GetCommandLineA(), "coop_test_free_cursor"))
		return;
	if (!s_noActivateHook)
		s_noActivateHook = SetWindowsHookExA(WH_CBT, NoActivate, 0, GetCurrentThreadId());
	if (!windowExists)
		return;
	EnumThreadWindows(GetCurrentThreadId(), NoActivateStyle, 0);
	// the window may have become the active one when it was created: the one
	// under it (the user's) gets it back
	HWND fg = GetForegroundWindow();
	DWORD pid = 0;
	GetWindowThreadProcessId(fg, &pid);
	if (!fg || pid != GetCurrentProcessId())
		return;
	for (HWND w = GetWindow(fg, GW_HWNDNEXT); w; w = GetWindow(w, GW_HWNDNEXT))
	{
		DWORD other = 0;
		GetWindowThreadProcessId(w, &other);
		if (other != GetCurrentProcessId() && IsWindowVisible(w) && !IsIconic(w) && GetWindowTextLengthA(w) > 0
			&& !(GetWindowLongA(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
		{
			SetForegroundWindow(w);
			break;
		}
	}
}

extern "C"
{
	GAME_API IGame *CreateGame(IGameFramework* pGameFramework)
	{
		ModuleInitISystem(pGameFramework->GetISystem());
		CoopKeepTestWindowBehind(true);

		static char pGameBuffer[sizeof(CGame)];
		return new ((void*)pGameBuffer) CGame();
	}
}
