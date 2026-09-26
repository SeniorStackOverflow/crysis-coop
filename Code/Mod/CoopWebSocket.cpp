// Crysis Coop: WebSocket client (see CoopWebSocket.h). The WinHTTP WebSocket
// functions are looked up at run time: the SDK builds for Windows XP, where
// they do not exist.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include "StdAfx.h"
#include "CoopWebSocket.h"

#include <string>

namespace
{
	typedef HINTERNET (WINAPI *PFN_Upgrade)(HINTERNET, DWORD_PTR);
	typedef DWORD (WINAPI *PFN_Send)(HINTERNET, int, PVOID, DWORD);
	typedef DWORD (WINAPI *PFN_Receive)(HINTERNET, PVOID, DWORD, DWORD*, int*);
	typedef DWORD (WINAPI *PFN_Close)(HINTERNET, USHORT, PVOID, DWORD);
	PFN_Upgrade s_wsUpgrade = 0;
	PFN_Send s_wsSend = 0;
	PFN_Receive s_wsReceive = 0;
	PFN_Close s_wsClose = 0;
	enum { WS_BINARY_MESSAGE = 0, WS_BINARY_FRAGMENT = 1, WS_CLOSE = 4 };
	const DWORD OPTION_UPGRADE_TO_WEB_SOCKET = 114;
}

bool CCoopWebSocket::Supported()
{
	if (s_wsSend)
		return true;
	HMODULE h = LoadLibraryA("winhttp.dll");
	if (!h)
		return false;
	s_wsUpgrade = (PFN_Upgrade)GetProcAddress(h, "WinHttpWebSocketCompleteUpgrade");
	s_wsReceive = (PFN_Receive)GetProcAddress(h, "WinHttpWebSocketReceive");
	s_wsClose = (PFN_Close)GetProcAddress(h, "WinHttpWebSocketClose");
	s_wsSend = (PFN_Send)GetProcAddress(h, "WinHttpWebSocketSend");
	if (!s_wsUpgrade || !s_wsReceive || !s_wsClose || !s_wsSend)
	{
		s_wsSend = 0;
		return false;
	}
	return true;
}

bool CCoopWebSocket::Open(const char* url)
{
	if (!Supported())
		return false;
	string u(url);
	bool secure = true;
	if (!strnicmp(u.c_str(), "wss://", 6))
		u = u.substr(6);
	else if (!strnicmp(u.c_str(), "ws://", 5))
	{
		u = u.substr(5);
		secure = false;
	}
	string path = "/";
	const size_t slash = u.find('/');
	if (slash != string::npos)
	{
		path = u.substr(slash);
		u = u.substr(0, slash);
	}
	INTERNET_PORT port = secure ? 443 : 80;
	const size_t colon = u.find(':');
	if (colon != string::npos)
	{
		port = (INTERNET_PORT)atoi(u.substr(colon + 1).c_str());
		u = u.substr(0, colon);
	}
	std::wstring host(u.begin(), u.end()), wpath(path.begin(), path.end());
	m_session = WinHttpOpen(L"CrysisCoop/2", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!m_session)
		return false;
	WinHttpSetTimeouts(m_session, 10000, 10000, 10000, 10000);
	m_connect = WinHttpConnect(m_session, host.c_str(), port, 0);
	HINTERNET request = m_connect ? WinHttpOpenRequest(m_connect, L"GET", wpath.c_str(), 0, WINHTTP_NO_REFERER,
		WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0) : 0;
	if (!request)
		return false;
	bool ok = WinHttpSetOption(request, OPTION_UPGRADE_TO_WEB_SOCKET, 0, 0)
		&& WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, 0, 0, 0, 0)
		&& WinHttpReceiveResponse(request, 0);
	if (ok)
	{
		DWORD status = 0, size = sizeof(status);
		WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, 0, &status, &size, 0);
		ok = status == 101;
	}
	if (ok)
		m_ws = s_wsUpgrade(request, 0);
	WinHttpCloseHandle(request);
	if (m_ws)
	{
		// no timeout on a quiet tunnel: pings keep it alive
		DWORD zero = 0;
		WinHttpSetOption(m_ws, WINHTTP_OPTION_RECEIVE_TIMEOUT, &zero, sizeof(zero));
	}
	return m_ws != 0;
}

bool CCoopWebSocket::Send(const void* data, int len)
{
	std::lock_guard<std::mutex> lock(m_sendLock);
	return m_ws && s_wsSend(m_ws, WS_BINARY_MESSAGE, (PVOID)data, (DWORD)len) == NO_ERROR;
}

int CCoopWebSocket::Receive(std::vector<char>& buf)
{
	int total = 0;
	for (;;)
	{
		if (!m_ws || total >= (int)buf.size())
			return -1;
		DWORD read = 0;
		int type = 0;
		if (s_wsReceive(m_ws, &buf[total], (DWORD)(buf.size() - total), &read, &type) != NO_ERROR || type == WS_CLOSE)
			return -1;
		total += (int)read;
		if (type != WS_BINARY_FRAGMENT)
			return total;
	}
}

void CCoopWebSocket::Close()
{
	Abort();
	if (m_connect)
		WinHttpCloseHandle(m_connect);
	if (m_session)
		WinHttpCloseHandle(m_session);
	m_connect = m_session = 0;
}

void CCoopWebSocket::Abort()
{
	std::lock_guard<std::mutex> lock(m_sendLock);
	if (m_ws)
	{
		WinHttpCloseHandle(m_ws);
		m_ws = 0;
	}
}
