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
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")

// Crysis Coop: a test instance or the AI companion's game (both start with
// coop_test_free_cursor) never becomes the active window and has no button
// on the taskbar. Somebody works at the computer (or plays) while they run:
// they must not take the keyboard focus or the mouse, and buttons coming and
// going made the taskbar jump
extern void* g_hInst;    // DllMain.cpp

namespace
{
	HHOOK s_noActivateHook = 0;

	// the AI companion's window stays to the left of all screens, wherever the
	// engine moves it (it centres its window when it sets the video mode)
	LRESULT CALLBACK KeepAway(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
	{
		if (msg == WM_WINDOWPOSCHANGING)
		{
			WINDOWPOS* p = reinterpret_cast<WINDOWPOS*>(lParam);
			if (!(p->flags & SWP_NOMOVE))
			{
				RECT r;
				GetWindowRect(hWnd, &r);
				const int w = (p->flags & SWP_NOSIZE) ? r.right - r.left : p->cx;
				p->x = GetSystemMetrics(SM_XVIRTUALSCREEN) - w - 100;
				p->y = GetSystemMetrics(SM_YVIRTUALSCREEN);
			}
		}
		// the engine sets its own extended style after making the window (a
		// taskbar button came back): it stays a tool window that is never active
		else if (msg == WM_STYLECHANGING && wParam == (WPARAM)GWL_EXSTYLE)
		{
			STYLESTRUCT* st = reinterpret_cast<STYLESTRUCT*>(lParam);
			st->styleNew = (st->styleNew | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE) & ~WS_EX_APPWINDOW;
		}
		else if (msg == WM_NCDESTROY)
			RemoveWindowSubclass(hWnd, KeepAway, 1);
		return DefSubclassProc(hWnd, msg, wParam, lParam);
	}

	LRESULT CALLBACK NoActivate(int code, WPARAM wParam, LPARAM lParam)
	{
		if (code == HCBT_ACTIVATE)
			return 1;
		if (code == HCBT_CREATEWND)
		{
			CBT_CREATEWNDA* pCreate = reinterpret_cast<CBT_CREATEWNDA*>(lParam);
			if (pCreate && pCreate->lpcs && !(pCreate->lpcs->style & WS_CHILD))
			{
				pCreate->lpcs->dwExStyle = (pCreate->lpcs->dwExStyle | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW) & ~WS_EX_APPWINDOW;
				// the AI companion's window is born out of sight: to the left of all screens
				static const bool s_companion = strstr(GetCommandLineA(), "coop_agent 1") != 0;
				if (s_companion)
				{
					const int w = pCreate->lpcs->cx > 0 && pCreate->lpcs->cx < 4096 ? pCreate->lpcs->cx : 800;
					pCreate->lpcs->x = GetSystemMetrics(SM_XVIRTUALSCREEN) - w - 100;
					pCreate->lpcs->y = GetSystemMetrics(SM_YVIRTUALSCREEN);
					SetWindowSubclass((HWND)wParam, KeepAway, 1, 0);
					// the extended style in the create structure is not taken: set on the window itself
					SetWindowLongA((HWND)wParam, GWL_EXSTYLE, pCreate->lpcs->dwExStyle);
				}
			}
		}
		return CallNextHookEx(s_noActivateHook, code, wParam, lParam);
	}

	struct SHostWindow { DWORD pid; HWND hWnd; };
	BOOL CALLBACK FindHostWindow(HWND hWnd, LPARAM lParam)
	{
		SHostWindow* h = (SHostWindow*)lParam;
		DWORD pid = 0;
		GetWindowThreadProcessId(hWnd, &pid);
		if (pid == h->pid && IsWindowVisible(hWnd) && !GetParent(hWnd))
		{
			h->hWnd = hWnd;
			return FALSE;
		}
		return TRUE;
	}

	BOOL CALLBACK NoActivateStyle(HWND hWnd, LPARAM)
	{
		// no taskbar button either: the style is taken while the window is hidden
		const LONG style = (GetWindowLongA(hWnd, GWL_EXSTYLE) | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW) & ~WS_EX_APPWINDOW;
		if (style != GetWindowLongA(hWnd, GWL_EXSTYLE))
		{
			const bool visible = IsWindowVisible(hWnd) != 0;
			if (visible)
				ShowWindow(hWnd, SW_HIDE);
			SetWindowLongA(hWnd, GWL_EXSTYLE, style);
			if (visible)
				ShowWindow(hWnd, SW_SHOWNOACTIVATE);
		}
		return TRUE;
	}
}

// The host hooks the main thread of the AI companion's game before it runs:
// Windows loads this DLL into that game when its first window is made, and
// NoActivate sees the window being made (the engine makes it long before it
// loads the game DLL)
HHOOK CoopHookWindowsOf(DWORD threadId)
{
	return SetWindowsHookExA(WH_CBT, NoActivate, (HINSTANCE)g_hInst, threadId);
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
	// started hidden (a button for the window the engine made before this
	// DLL was there made the taskbar's icons jump): now a tool window, it is
	// shown, behind the others; the AI companion's out of sight, to the left
	// of all screens (a hidden game all but stops its frames)
	HWND hGame = gEnv && gEnv->pRenderer ? (HWND)gEnv->pRenderer->GetHWND() : 0;
	if (hGame && !IsWindowVisible(hGame))
	{
		if (strstr(GetCommandLineA(), "coop_agent 1"))
		{
			RECT r;
			GetWindowRect(hGame, &r);
			SetWindowPos(hGame, HWND_BOTTOM, GetSystemMetrics(SM_XVIRTUALSCREEN) - (r.right - r.left) - 100, GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0,
				SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
		}
		else
			SetWindowPos(hGame, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
	}
	// the window may have become the active one when it was created: it goes
	// back to the host's game (the AI companion's) or to the window under it
	HWND fg = GetForegroundWindow();
	DWORD pid = 0;
	GetWindowThreadProcessId(fg, &pid);
	if (!fg || pid != GetCurrentProcessId())
		return;
	if (const char* parent = strstr(GetCommandLineA(), "coop_agent_parent "))
	{
		SHostWindow host = { (DWORD)atoi(parent + 18), 0 };
		EnumWindows(FindHostWindow, (LPARAM)&host);
		if (host.hWnd)
		{
			SetForegroundWindow(host.hWnd);
			return;
		}
	}
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

// Crysis Coop: Crysis Warhead's paks copied into this game's Game folder
// (Warhead_*.pak) replace a thousand of Crysis' own files: AI and entity
// scripts, animations, objects. Warhead's soldier script calls a function
// this game has not, and a soldier took no damage from bullets. The campaign
// is Crysis': the log says so (the player should take those paks away).
static void CoopWarnForeignPaks()
{
	if (!gEnv || !gEnv->pCryPak)
		return;
	std::vector<string> foreign;
	if (ICryPak::PakInfo* pInfo = gEnv->pCryPak->GetPakInfo())
	{
		for (unsigned i = 0; i < pInfo->numOpenPaks; ++i)
		{
			const char* path = pInfo->arrPaks[i].szFilePath;
			if (!path)
				continue;
			const char* base = path;
			for (const char* c = path; *c; ++c)
				if (*c == '\\' || *c == '/')
					base = c + 1;
			if (strnicmp(base, "Warhead_", 8) == 0)
				foreign.push_back(path);
		}
		gEnv->pCryPak->FreePakInfo(pInfo);
	}
	for (size_t i = 0; i < foreign.size(); ++i)
		CryLogAlways("[Coop] WARNING: %s is a Crysis Warhead file in Crysis' folder: it replaces Crysis' own scripts and the enemies may take no damage. Remove it from Crysis\\Game.",foreign[i].c_str());
}

extern "C"
{
	GAME_API IGame *CreateGame(IGameFramework* pGameFramework)
	{
		ModuleInitISystem(pGameFramework->GetISystem());
		CoopWarnForeignPaks();
		CoopKeepTestWindowBehind(true);

		static char pGameBuffer[sizeof(CGame)];
		return new ((void*)pGameBuffer) CGame();
	}
}
