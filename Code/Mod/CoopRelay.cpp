// Crysis Coop: internet play through the relay (see CoopRelay.h and
// Relay/coop_relay.py for the protocol).
//
// Both sides talk to the relay over a WebSocket (wss://, port 443), which
// passes through practically every firewall and router:
//  - host:   one tunnel; every friend becomes a local UDP socket that talks
//            to the host's own game server (127.0.0.1:sv_port)
//  - friend: "coop_join <code>" opens a tunnel and a local UDP port; the
//            game connects to 127.0.0.1:<that port>
// The game itself still sends and receives plain UDP datagrams.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")

#include "StdAfx.h"
#include "CoopRelay.h"
#include "CoopAI.h"
#include "CoopSave.h"
#include "Game.h"
#include "GameRules.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
	enum
	{
		OP_REGISTER = 1, OP_REGISTERED, OP_PING, OP_TO_HOST, OP_FROM_HOST, OP_CLOSE, OP_ERROR,
		OP_JOIN, OP_JOINED, OP_FRIEND_DATA, OP_TO_FRIEND,
		OP_FRIEND_CANDIDATES, OP_CANDIDATES_FOR_HOST, OP_HOST_CANDIDATES, OP_CANDIDATES_FOR_FRIEND
	};
	const unsigned char PROTOCOL_VERSION = 1;

	// --------------------------------------------------------------------
	// WebSocket client on WinHTTP (Windows 8+; loaded at run time)
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

	bool LoadWebSocketApi()
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

	class CWebSocket
	{
	public:
		CWebSocket() : m_session(0), m_connect(0), m_ws(0) {}
		~CWebSocket() { Close(); }

		bool Open(const char* url)
		{
			if (!LoadWebSocketApi())
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
			m_session = WinHttpOpen(L"CrysisCoop/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
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

		// from one thread at a time
		bool Send(const void* data, int len)
		{
			std::lock_guard<std::mutex> lock(m_sendLock);
			return m_ws && s_wsSend(m_ws, WS_BINARY_MESSAGE, (PVOID)data, (DWORD)len) == NO_ERROR;
		}

		// blocking: one whole message, -1 when the connection ended
		int Receive(std::vector<char>& buf)
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

		void Close()
		{
			Abort();
			if (m_connect)
				WinHttpCloseHandle(m_connect);
			if (m_session)
				WinHttpCloseHandle(m_session);
			m_connect = m_session = 0;
		}

		// ends a Receive blocked in another thread (closing the handle cancels it)
		void Abort()
		{
			std::lock_guard<std::mutex> lock(m_sendLock);
			if (m_ws)
			{
				WinHttpCloseHandle(m_ws);
				m_ws = 0;
			}
		}

	private:
		HINTERNET m_session, m_connect, m_ws;
		std::mutex m_sendLock;
	};

	// --------------------------------------------------------------------
	ICVar* s_pRelay = 0;        // WebSocket URL of the relay
	ICVar* s_pEnable = 0;
	ICVar* s_pCode = 0;         // the code of the game this machine hosts (0 = none)
	ICVar* s_pDirect = 0;       // 1 = try a direct UDP path to the other side first

	// state the worker threads report; the main thread logs and shows it
	// (the engine log is not thread safe)
	std::atomic<int> s_hostCode(0);
	std::atomic<int> s_hostFriends(0);
	std::atomic<int> s_hostDirect(0);       // friends on a direct path
	std::atomic<int> s_hostError(0);        // 1 relay full, 2 version, 5 no connection, 6 no WebSocket support
	std::atomic<int> s_hostReconnects(0);
	std::atomic<int> s_joinPort(0);         // friend: local port the game connects to
	std::atomic<int> s_joinError(0);        // 3 no such game, 4 game full, 5 no connection, 6 no WebSocket support
	std::atomic<bool> s_joinLost(false);
	std::atomic<int> s_joinDirectRtt(-1);   // friend: ms on the direct path, -1 = no direct path
	std::atomic<int> s_relayRtt(-1);        // ms through the relay (this machine's tunnel)

	void Nap(DWORD ms, const std::atomic<bool>& stop)
	{
		for (DWORD t = 0; t < ms && !stop; t += 50)
			Sleep(50);
	}

	SOCKET LocalUdpSocket(int* pPort = 0)
	{
		SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		sockaddr_in local;
		memset(&local, 0, sizeof(local));
		local.sin_family = AF_INET;
		local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		bind(s, (sockaddr*)&local, sizeof(local));
		if (pPort)
		{
			int len = sizeof(local);
			getsockname(s, (sockaddr*)&local, &len);
			*pPort = ntohs(local.sin_port);
		}
		return s;
	}

	void SendPing(CWebSocket& ws)
	{
		unsigned char ping[5] = { OP_PING };
		const DWORD now = GetTickCount();
		memcpy(ping + 1, &now, 4);
		ws.Send(ping, sizeof(ping));
	}

	void OnPong(const std::vector<char>& buf, int n)
	{
		if (n >= 5)
		{
			DWORD sent;
			memcpy(&sent, &buf[1], 4);
			s_relayRtt = (int)(GetTickCount() - sent);
		}
	}

	// --------------------------------------------------------------------
	// Direct path. The host and a friend swap, through the relay, the UDP
	// addresses they may be reached at (their local ones and the public one a
	// STUN server sees) and both send probes to all of them. Home routers let
	// the answers of an address they just sent to in, so usually a path opens
	// both ways (NAT hole punching). The game's packets then go straight
	// between the two machines, which saves the detour through the relay;
	// while no probe comes back the relay carries them as before.
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
	const unsigned char P2P_MAGIC = 0xC5;
	enum { P2P_PROBE = 1, P2P_ACK = 2, P2P_DATA = 3 };
	const DWORD PATH_TIMEOUT = 5000;

	struct SCandidate { unsigned long ip; unsigned short port; };   // network byte order

	struct SDirectPath
	{
		unsigned char token[8];         // made by the friend, known only to both sides
		std::vector<SCandidate> peer;   // where the other side may be reached
		sockaddr_in path;
		DWORD lastRecv;                 // 0 = no path yet
		DWORD lastProbe;
		int rtt;
		SDirectPath() : lastRecv(0), lastProbe(0), rtt(-1)
		{
			memset(token, 0, sizeof(token));
			memset(&path, 0, sizeof(path));
		}
		bool Alive(DWORD now) const { return lastRecv && now - lastRecv < PATH_TIMEOUT; }
	};

	SOCKET DirectSocket()
	{
		SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		sockaddr_in any;
		memset(&any, 0, sizeof(any));
		any.sin_family = AF_INET;
		bind(s, (sockaddr*)&any, sizeof(any));
		// probes to addresses nobody listens at must not break the socket
		BOOL off = FALSE;
		DWORD ret = 0;
		WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), 0, 0, &ret, 0, 0);
		return s;
	}

	void RandomBytes(unsigned char* p, int n)
	{
		LARGE_INTEGER t;
		QueryPerformanceCounter(&t);
		unsigned long long x = t.QuadPart ^ ((unsigned long long)GetCurrentThreadId() << 32) ^ (unsigned long long)rand() * 0x9E3779B97F4A7C15ull;
		for (int i = 0; i < n; ++i)
		{
			x ^= x << 13; x ^= x >> 7; x ^= x << 17;
			p[i] = (unsigned char)(x >> 24);
		}
	}

	// the public address of this socket, from a STUN server (RFC 5389)
	bool StunMappedAddress(SOCKET s, SCandidate& out)
	{
		static const char* servers[][2] = { { "stun.l.google.com", "19302" }, { "stun.cloudflare.com", "3478" }, { "stun1.l.google.com", "19302" } };
		unsigned char req[20] = { 0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42 };
		RandomBytes(req + 8, 12);
		for (int i = 0; i < 3; ++i)
		{
			addrinfo hints;
			memset(&hints, 0, sizeof(hints));
			hints.ai_family = AF_INET;
			hints.ai_socktype = SOCK_DGRAM;
			addrinfo* pResult = 0;
			if (getaddrinfo(servers[i][0], servers[i][1], &hints, &pResult) == 0 && pResult)
			{
				sendto(s, (const char*)req, sizeof(req), 0, pResult->ai_addr, (int)pResult->ai_addrlen);
				freeaddrinfo(pResult);
			}
		}
		unsigned char buf[512];
		const DWORD start = GetTickCount();
		while (GetTickCount() - start < 1500)
		{
			fd_set set;
			FD_ZERO(&set);
			FD_SET(s, &set);
			timeval tv = { 0, 100000 };
			if (select(0, &set, 0, 0, &tv) <= 0)
				continue;
			const int n = recv(s, (char*)buf, sizeof(buf), 0);
			if (n < 20 || buf[0] != 0x01 || buf[1] != 0x01 || memcmp(buf + 4, req + 4, 16))
				continue;
			for (int pos = 20; pos + 4 <= n; )
			{
				const int type = (buf[pos] << 8) | buf[pos + 1];
				const int len = (buf[pos + 2] << 8) | buf[pos + 3];
				const unsigned char* v = buf + pos + 4;
				if (pos + 4 + len <= n && len >= 8 && v[1] == 0x01 && (type == 0x0020 || type == 0x0001))
				{
					unsigned short port = (unsigned short)((v[2] << 8) | v[3]);
					unsigned long ip = ((unsigned long)v[4] << 24) | (v[5] << 16) | (v[6] << 8) | v[7];
					if (type == 0x0020)
					{
						port ^= 0x2112;
						ip ^= 0x2112A442;
					}
					out.ip = htonl(ip);
					out.port = htons(port);
					return true;
				}
				pos += 4 + ((len + 3) & ~3);
			}
		}
		return false;
	}

	std::vector<SCandidate> GatherCandidates(SOCKET s, bool* pPublic = 0)
	{
		std::vector<SCandidate> c;
		sockaddr_in local;
		int len = sizeof(local);
		getsockname(s, (sockaddr*)&local, &len);
		SCandidate mapped;
		const bool haveMapped = StunMappedAddress(s, mapped);
		if (pPublic)
			*pPublic = haveMapped;
		if (haveMapped)
			c.push_back(mapped);
		char name[256];
		if (gethostname(name, sizeof(name)) == 0)
		{
			addrinfo hints;
			memset(&hints, 0, sizeof(hints));
			hints.ai_family = AF_INET;
			hints.ai_socktype = SOCK_DGRAM;
			addrinfo* pResult = 0;
			if (getaddrinfo(name, 0, &hints, &pResult) == 0)
			{
				for (addrinfo* p = pResult; p && c.size() < 7; p = p->ai_next)
				{
					SCandidate x = { ((sockaddr_in*)p->ai_addr)->sin_addr.s_addr, local.sin_port };
					bool dup = false;
					for (size_t i = 0; i < c.size(); ++i)
						dup = dup || (c[i].ip == x.ip && c[i].port == x.port);
					if (!dup && x.ip)
						c.push_back(x);
				}
				freeaddrinfo(pResult);
			}
		}
		SCandidate loopback = { htonl(INADDR_LOOPBACK), local.sin_port };
		c.push_back(loopback);
		return c;
	}

	void PackCandidates(const std::vector<SCandidate>& c, std::vector<char>& out)
	{
		out.push_back((char)c.size());
		for (size_t i = 0; i < c.size(); ++i)
		{
			out.insert(out.end(), (const char*)&c[i].ip, (const char*)&c[i].ip + 4);
			out.insert(out.end(), (const char*)&c[i].port, (const char*)&c[i].port + 2);
		}
	}

	void UnpackCandidates(const char* p, int n, std::vector<SCandidate>& out)
	{
		out.clear();
		if (n < 1)
			return;
		const int count = (unsigned char)p[0];
		for (int i = 0; i < count && 1 + 6 * (i + 1) <= n && i < 8; ++i)
		{
			SCandidate c;
			memcpy(&c.ip, p + 1 + 6 * i, 4);
			memcpy(&c.port, p + 5 + 6 * i, 2);
			out.push_back(c);
		}
	}

	void SendProbe(SOCKET s, const sockaddr_in& to, const unsigned char* token, unsigned char type, DWORD stamp)
	{
		unsigned char pkt[14] = { P2P_MAGIC, type };
		memcpy(pkt + 2, token, 8);
		memcpy(pkt + 10, &stamp, 4);
		sendto(s, (const char*)pkt, sizeof(pkt), 0, (const sockaddr*)&to, sizeof(to));
	}

	// probes to every address 4 times a second until a path works, then
	// once a second on the path (keeps the routers' mapping open, measures)
	void PunchAndKeepAlive(SOCKET s, SDirectPath& d, DWORD now)
	{
		if (d.peer.empty())
			return;
		if (d.Alive(now))
		{
			if (now - d.lastProbe >= 1000)
			{
				SendProbe(s, d.path, d.token, P2P_PROBE, now);
				d.lastProbe = now;
			}
			return;
		}
		if (now - d.lastProbe < 250)
			return;
		d.lastProbe = now;
		for (size_t i = 0; i < d.peer.size(); ++i)
		{
			sockaddr_in to;
			memset(&to, 0, sizeof(to));
			to.sin_family = AF_INET;
			to.sin_addr.s_addr = d.peer[i].ip;
			to.sin_port = d.peer[i].port;
			SendProbe(s, to, d.token, P2P_PROBE, now);
		}
	}

	bool SameAddress(const sockaddr_in& a, const sockaddr_in& b)
	{
		return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
	}

	// a probe or an answer of the other side arrived
	void OnProbe(SOCKET s, SDirectPath& d, const unsigned char* pkt, const sockaddr_in& from, DWORD now)
	{
		DWORD stamp;
		memcpy(&stamp, pkt + 10, 4);
		if (pkt[1] == P2P_PROBE)
			SendProbe(s, from, d.token, P2P_ACK, stamp);
		else
			d.rtt = (int)(now - stamp);
		if (!d.Alive(now))
			d.path = from;
		if (SameAddress(d.path, from))
			d.lastRecv = now;
	}

	void SendDirect(SOCKET s, const sockaddr_in& to, char* buf, int payload)
	{
		// the payload sits at buf + 2
		buf[0] = (char)P2P_MAGIC;
		buf[1] = P2P_DATA;
		sendto(s, buf, payload + 2, 0, (const sockaddr*)&to, sizeof(to));
	}

	// --------------------------------------------------------------------
	// host: the tunnel, the direct paths and one local socket per friend
	class CHostAgent
	{
	public:
		CHostAgent() : m_stop(false), m_code(0), m_gamePort(64087), m_useDirect(true), m_direct(INVALID_SOCKET)
		{
			memset(m_secret, 0, sizeof(m_secret));
		}

		void Start(const string& url, int gamePort, bool useDirect)
		{
			m_url = url;
			m_gamePort = gamePort;
			m_useDirect = useDirect;
			m_stop = false;
			m_thread = std::thread(&CHostAgent::Run, this);
		}

		void Stop()
		{
			m_stop = true;
			m_ws.Abort();
			if (m_thread.joinable())
				m_thread.join();
		}

		bool Running() const { return m_thread.joinable(); }

	private:
		struct SFriend { SOCKET s; DWORD lastActive; SDirectPath direct; };

		void Run()
		{
			WSADATA wsa;
			WSAStartup(MAKEWORD(2, 2), &wsa);
			if (m_useDirect)
			{
				m_direct = DirectSocket();
				m_candidates = GatherCandidates(m_direct);
			}
			std::vector<char> buf(4096);
			while (!m_stop)
			{
				if (!m_ws.Open(m_url.c_str()))
				{
					s_hostError = LoadWebSocketApi() ? 5 : 6;
					m_ws.Close();
					Nap(3000, m_stop);
					continue;
				}
				unsigned char reg[12] = { OP_REGISTER, PROTOCOL_VERSION };
				memcpy(reg + 2, &m_code, 2);
				memcpy(reg + 4, m_secret, 8);
				m_ws.Send(reg, sizeof(reg));
				std::atomic<bool> lost(false);
				std::thread reader(&CHostAgent::Read, this, std::ref(lost));
				DWORD lastPing = 0;
				while (!m_stop && !lost)
				{
					Pump(buf);
					if (GetTickCount() - lastPing > 5000)
					{
						SendPing(m_ws);
						lastPing = GetTickCount();
					}
				}
				m_ws.Abort();
				reader.join();
				m_ws.Close();
				if (!m_stop)
				{
					++s_hostReconnects;     // keeps the code and the friends' sockets
					Nap(2000, m_stop);
				}
			}
			std::lock_guard<std::mutex> lock(m_lock);
			for (std::map<unsigned short, SFriend>::iterator it = m_friends.begin(); it != m_friends.end(); ++it)
				closesocket(it->second.s);
			m_friends.clear();
			if (m_direct != INVALID_SOCKET)
				closesocket(m_direct);
			m_direct = INVALID_SOCKET;
			s_hostFriends = 0;
			s_hostDirect = 0;
			s_hostCode = 0;
			WSACleanup();
		}

		// under m_lock: the friend's "address" for the host's game, a local socket
		SFriend& Friend(unsigned short cid)
		{
			std::map<unsigned short, SFriend>::iterator it = m_friends.find(cid);
			if (it == m_friends.end())
			{
				sockaddr_in game;
				memset(&game, 0, sizeof(game));
				game.sin_family = AF_INET;
				game.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
				game.sin_port = htons((u_short)m_gamePort);
				SFriend f;
				f.s = LocalUdpSocket();
				f.lastActive = GetTickCount();
				connect(f.s, (sockaddr*)&game, sizeof(game));
				it = m_friends.insert(std::make_pair(cid, f)).first;
				s_hostFriends = (int)m_friends.size();
			}
			return it->second;
		}

		// the tunnel's messages (own thread: the receive blocks)
		void Read(std::atomic<bool>& lost)
		{
			std::vector<char> buf(4096);
			for (;;)
			{
				const int n = m_ws.Receive(buf);
				if (n < 1)
					break;
				const unsigned char op = (unsigned char)buf[0];
				if (op == OP_REGISTERED && n >= 11)
				{
					memcpy(&m_code, &buf[1], 2);
					memcpy(m_secret, &buf[3], 8);
					s_hostCode = m_code;
					s_hostError = 0;
				}
				else if (op == OP_PING)
					OnPong(buf, n);
				else if (op == OP_ERROR && n >= 2)
				{
					s_hostError = (unsigned char)buf[1];
					break;
				}
				else if (op == OP_TO_HOST && n >= 3)
				{
					unsigned short cid;
					memcpy(&cid, &buf[1], 2);
					std::lock_guard<std::mutex> lock(m_lock);
					SFriend& f = Friend(cid);
					f.lastActive = GetTickCount();
					send(f.s, &buf[3], n - 3, 0);
				}
				else if (op == OP_CLOSE && n >= 3)
				{
					unsigned short cid;
					memcpy(&cid, &buf[1], 2);
					std::lock_guard<std::mutex> lock(m_lock);
					m_closing.push_back(cid);
				}
				else if (op == OP_CANDIDATES_FOR_HOST && n >= 12 && m_direct != INVALID_SOCKET)
				{
					// a friend's addresses: answer with ours, start punching
					unsigned short cid;
					memcpy(&cid, &buf[1], 2);
					{
						std::lock_guard<std::mutex> lock(m_lock);
						SDirectPath& d = Friend(cid).direct;
						if (memcmp(d.token, &buf[3], 8))
						{
							memcpy(d.token, &buf[3], 8);
							d.lastRecv = 0;     // a new friend on this client number
						}
						UnpackCandidates(&buf[11], n - 11, d.peer);
					}
					std::vector<char> reply;
					reply.push_back((char)OP_HOST_CANDIDATES);
					reply.insert(reply.end(), (const char*)&cid, (const char*)&cid + 2);
					PackCandidates(m_candidates, reply);
					m_ws.Send(&reply[0], (int)reply.size());
				}
			}
			lost = true;
		}

		// the friends' game sockets and the direct socket
		void Pump(std::vector<char>& buf)
		{
			fd_set set;
			FD_ZERO(&set);
			const DWORD now = GetTickCount();
			{
				std::lock_guard<std::mutex> lock(m_lock);
				int direct = 0;
				for (std::map<unsigned short, SFriend>::iterator it = m_friends.begin(); it != m_friends.end(); )
				{
					const bool closing = std::find(m_closing.begin(), m_closing.end(), it->first) != m_closing.end();
					if (closing || now - it->second.lastActive > 90000)
					{
						closesocket(it->second.s);
						m_friends.erase(it++);
						continue;
					}
					FD_SET(it->second.s, &set);
					if (m_direct != INVALID_SOCKET)
						PunchAndKeepAlive(m_direct, it->second.direct, now);
					direct += it->second.direct.Alive(now) ? 1 : 0;
					++it;
				}
				m_closing.clear();
				s_hostFriends = (int)m_friends.size();
				s_hostDirect = direct;
			}
			if (m_direct != INVALID_SOCKET)
				FD_SET(m_direct, &set);
			if (!set.fd_count)
			{
				Sleep(20);
				return;
			}
			timeval tv = { 0, 20000 };
			if (select(0, &set, 0, 0, &tv) <= 0)
				return;
			std::lock_guard<std::mutex> lock(m_lock);
			if (m_direct != INVALID_SOCKET && FD_ISSET(m_direct, &set))
			{
				sockaddr_in from;
				int fromLen = sizeof(from);
				const int n = recvfrom(m_direct, &buf[0], (int)buf.size(), 0, (sockaddr*)&from, &fromLen);
				if (n >= 2 && (unsigned char)buf[0] == P2P_MAGIC)
					OnDirectPacket(buf, n, from, GetTickCount());
			}
			for (std::map<unsigned short, SFriend>::iterator it = m_friends.begin(); it != m_friends.end(); ++it)
			{
				if (!FD_ISSET(it->second.s, &set))
					continue;
				// the host's game answers a friend: direct, or back through the tunnel
				const int n = recv(it->second.s, &buf[3], (int)buf.size() - 3, 0);
				if (n <= 0)
					continue;
				it->second.lastActive = GetTickCount();
				if (m_direct != INVALID_SOCKET && it->second.direct.Alive(it->second.lastActive))
				{
					SendDirect(m_direct, it->second.direct.path, &buf[1], n);
					continue;
				}
				buf[0] = OP_FROM_HOST;
				memcpy(&buf[1], &it->first, 2);
				m_ws.Send(&buf[0], n + 3);
			}
		}

		// under m_lock
		void OnDirectPacket(std::vector<char>& buf, int n, const sockaddr_in& from, DWORD now)
		{
			const unsigned char* pkt = (const unsigned char*)&buf[0];
			for (std::map<unsigned short, SFriend>::iterator it = m_friends.begin(); it != m_friends.end(); ++it)
			{
				SDirectPath& d = it->second.direct;
				if (d.peer.empty())
					continue;
				if ((pkt[1] == P2P_PROBE || pkt[1] == P2P_ACK) && n >= 14 && !memcmp(pkt + 2, d.token, 8))
				{
					OnProbe(m_direct, d, pkt, from, now);
					return;
				}
				if (pkt[1] == P2P_DATA && d.Alive(now) && SameAddress(d.path, from))
				{
					d.lastRecv = now;
					it->second.lastActive = now;
					send(it->second.s, &buf[2], n - 2, 0);
					return;
				}
			}
		}

		CWebSocket m_ws;
		std::thread m_thread;
		std::atomic<bool> m_stop;
		std::mutex m_lock;
		std::map<unsigned short, SFriend> m_friends;
		std::vector<unsigned short> m_closing;
		std::vector<SCandidate> m_candidates;
		string m_url;
		unsigned short m_code;
		unsigned char m_secret[8];
		int m_gamePort;
		bool m_useDirect;
		SOCKET m_direct;
	};

	// --------------------------------------------------------------------
	// friend: a local UDP port for the game, the direct path and the tunnel
	class CJoinAgent
	{
	public:
		CJoinAgent() : m_stop(false), m_code(0), m_useDirect(true), m_direct(INVALID_SOCKET), m_haveGame(false) {}

		void Start(const string& url, int code, bool useDirect)
		{
			m_url = url;
			m_code = code;
			m_useDirect = useDirect;
			m_stop = false;
			s_joinPort = 0;
			s_joinError = 0;
			s_joinLost = false;
			s_joinDirectRtt = -1;
			s_relayRtt = -1;
			m_thread = std::thread(&CJoinAgent::Run, this);
		}

		void Stop()
		{
			m_stop = true;
			m_ws.Abort();
			if (m_thread.joinable())
				m_thread.join();
		}

		bool Running() const { return m_thread.joinable(); }

	private:
		void Run()
		{
			WSADATA wsa;
			WSAStartup(MAKEWORD(2, 2), &wsa);
			std::vector<char> buf(4096);
			SOCKET local = INVALID_SOCKET;
			m_haveGame = false;
			m_path = SDirectPath();
			RandomBytes(m_path.token, 8);
			std::vector<SCandidate> candidates;
			// a lost tunnel is opened again (the game keeps its local port)
			while (!m_stop)
			{
				if (!m_ws.Open(m_url.c_str()))
				{
					m_ws.Close();
					if (!s_joinPort)
					{
						s_joinError = LoadWebSocketApi() ? 5 : 6;
						break;
					}
					Nap(2000, m_stop);
					continue;
				}
				// the token brings this machine back as the same client after a lost connection
				unsigned char join[12] = { OP_JOIN, PROTOCOL_VERSION };
				memcpy(join + 2, &m_code, 2);
				memcpy(join + 4, m_path.token, 8);
				m_ws.Send(join, sizeof(join));
				const int n = m_ws.Receive(buf);
				if (n < 1 || (unsigned char)buf[0] != OP_JOINED)
				{
					s_joinError = (n >= 2 && (unsigned char)buf[0] == OP_ERROR) ? (unsigned char)buf[1] : 5;
					m_ws.Close();
					break;
				}
				if (local == INVALID_SOCKET)
				{
					int port = 0;
					local = LocalUdpSocket(&port);
					s_joinPort = port;      // the main thread connects the game to it now
				}
				if (m_useDirect)
				{
					if (m_direct == INVALID_SOCKET)
					{
						m_direct = DirectSocket();
						candidates = GatherCandidates(m_direct);
					}
					std::vector<char> msg;
					msg.push_back((char)OP_FRIEND_CANDIDATES);
					msg.insert(msg.end(), (const char*)m_path.token, (const char*)m_path.token + 8);
					PackCandidates(candidates, msg);
					m_ws.Send(&msg[0], (int)msg.size());
				}
				std::atomic<bool> lost(false);
				std::thread reader(&CJoinAgent::Read, this, local, std::ref(lost));
				DWORD lastPing = 0;
				while (!m_stop && !lost)
				{
					Pump(local, buf);
					if (GetTickCount() - lastPing > 5000)
					{
						SendPing(m_ws);
						lastPing = GetTickCount();
					}
				}
				m_ws.Abort();
				reader.join();
				m_ws.Close();
				if (!m_stop)
				{
					s_joinLost = true;
					Nap(1000, m_stop);
				}
			}
			if (local != INVALID_SOCKET)
				closesocket(local);
			if (m_direct != INVALID_SOCKET)
				closesocket(m_direct);
			m_direct = INVALID_SOCKET;
			s_joinDirectRtt = -1;
			WSACleanup();
		}

		void Pump(SOCKET local, std::vector<char>& buf)
		{
			const DWORD now = GetTickCount();
			{
				std::lock_guard<std::mutex> lock(m_lock);
				if (m_direct != INVALID_SOCKET)
					PunchAndKeepAlive(m_direct, m_path, now);
				s_joinDirectRtt = m_path.Alive(now) ? (m_path.rtt < 0 ? 0 : m_path.rtt) : -1;
			}
			fd_set set;
			FD_ZERO(&set);
			FD_SET(local, &set);
			if (m_direct != INVALID_SOCKET)
				FD_SET(m_direct, &set);
			timeval tv = { 0, 20000 };
			if (select(0, &set, 0, 0, &tv) <= 0)
				return;
			if (FD_ISSET(local, &set))
			{
				// the game's packet for the host: direct, or through the tunnel
				sockaddr_in from;
				int fromLen = sizeof(from);
				const int got = recvfrom(local, &buf[2], (int)buf.size() - 2, 0, (sockaddr*)&from, &fromLen);
				if (got > 0)
				{
					std::lock_guard<std::mutex> lock(m_lock);
					m_game = from;
					m_haveGame = true;
					if (m_direct != INVALID_SOCKET && m_path.Alive(GetTickCount()))
						SendDirect(m_direct, m_path.path, &buf[0], got);
					else
					{
						buf[1] = OP_FRIEND_DATA;
						m_ws.Send(&buf[1], got + 1);
					}
				}
			}
			if (m_direct != INVALID_SOCKET && FD_ISSET(m_direct, &set))
			{
				sockaddr_in from;
				int fromLen = sizeof(from);
				const int n = recvfrom(m_direct, &buf[0], (int)buf.size(), 0, (sockaddr*)&from, &fromLen);
				const unsigned char* pkt = (const unsigned char*)&buf[0];
				if (n >= 2 && pkt[0] == P2P_MAGIC)
				{
					std::lock_guard<std::mutex> lock(m_lock);
					const DWORD t = GetTickCount();
					if ((pkt[1] == P2P_PROBE || pkt[1] == P2P_ACK) && n >= 14 && !memcmp(pkt + 2, m_path.token, 8))
						OnProbe(m_direct, m_path, pkt, from, t);
					else if (pkt[1] == P2P_DATA && m_path.Alive(t) && SameAddress(m_path.path, from))
					{
						m_path.lastRecv = t;
						if (m_haveGame)
							sendto(local, &buf[2], n - 2, 0, (sockaddr*)&m_game, sizeof(m_game));
					}
				}
			}
		}

		void Read(SOCKET local, std::atomic<bool>& lost)
		{
			std::vector<char> buf(4096);
			for (;;)
			{
				const int n = m_ws.Receive(buf);
				if (n < 1 || (unsigned char)buf[0] == OP_ERROR)
					break;
				const unsigned char op = (unsigned char)buf[0];
				if (op == OP_PING)
					OnPong(buf, n);
				else if (op == OP_CANDIDATES_FOR_FRIEND)
				{
					std::lock_guard<std::mutex> lock(m_lock);
					UnpackCandidates(&buf[1], n - 1, m_path.peer);
				}
				else if (op == OP_TO_FRIEND)
				{
					std::lock_guard<std::mutex> lock(m_lock);
					if (m_haveGame)
						sendto(local, &buf[1], n - 1, 0, (sockaddr*)&m_game, sizeof(m_game));
				}
			}
			lost = true;
		}

		CWebSocket m_ws;
		std::thread m_thread;
		std::atomic<bool> m_stop;
		std::mutex m_lock;
		sockaddr_in m_game;
		bool m_haveGame;
		SDirectPath m_path;
		string m_url;
		unsigned short m_code;
		bool m_useDirect;
		SOCKET m_direct;
	};

	CHostAgent s_hostAgent;
	CJoinAgent s_joinAgent;
	bool s_hosting = false;
	float s_serverGoneFor = 0.0f;
	int s_announcedCode = 0;
	int s_loggedFriends = 0, s_loggedHostDirect = 0, s_loggedHostError = 0, s_loggedReconnects = 0, s_joinConnected = 0;
	int s_loggedJoinDirect = -1;
	bool s_joinErrorShown = false;

	void StopHost()
	{
		if (s_hosting || s_hostAgent.Running())
			s_hostAgent.Stop();
		s_hosting = false;
		s_announcedCode = 0;
		s_loggedFriends = 0;
		s_loggedHostDirect = 0;
		s_loggedHostError = 0;
		if (s_pCode)
			s_pCode->ForceSet("0");
	}

	void StartHost()
	{
		ICVar* pPort = gEnv->pConsole->GetCVar("sv_port");
		const int gamePort = pPort ? pPort->GetIVal() : 64087;
		s_relayRtt = -1;
		s_hostAgent.Start(s_pRelay->GetString(), gamePort, s_pDirect->GetIVal() != 0);
		s_hosting = true;
		s_serverGoneFor = 0.0f;
		CryLogAlways("[CoopRelay] hosting through the relay %s (game port %d)", s_pRelay->GetString(), gamePort);
	}

	const char* ErrorText(int e)
	{
		switch (e)
		{
		case 1: return "the relay is full, try again later";
		case 2: return "the relay runs another version of the mod";
		case 3: return "no coop game with this code (check the code; the host must be in the game)";
		case 4: return "that coop game is full";
		case 5: return "cannot reach the relay (internet connection?)";
		case 6: return "this Windows has no WebSocket support (Windows 8 or later needed)";
		}
		return "unknown error";
	}

	// coop_join <code>: join a friend's coop game through the relay
	void CmdJoin(IConsoleCmdArgs* pArgs)
	{
		if (pArgs->GetArgCount() < 2 || atoi(pArgs->GetArg(1)) <= 0)
		{
			CryLogAlways("usage: coop_join <code>   (the code the host sees when he starts a coop game)");
			return;
		}
		s_joinAgent.Stop();
		s_joinConnected = 0;
		s_loggedJoinDirect = -1;
		s_joinErrorShown = false;
		s_joinAgent.Start(s_pRelay->GetString(), atoi(pArgs->GetArg(1)), s_pDirect->GetIVal() != 0);
		CryLogAlways("[CoopRelay] joining coop game %s through %s ...", pArgs->GetArg(1), s_pRelay->GetString());
	}

	// coop_host [level]: host a coop game, from the first level or the one named
	// (there are no saved games in coop: this is how a campaign continues)
	void CmdHost(IConsoleCmdArgs* pArgs)
	{
		static const char* s_levels[] = { "island", "village", "rescue", "harbor", "tank", "mine", "core", "ice", "sphere", "ascension", "fleet" };
		const char* level = s_levels[0];
		if (pArgs->GetArgCount() > 1)
		{
			const char* arg = pArgs->GetArg(1);
			const int number = atoi(arg);
			level = 0;
			for (int i = 0; i < 11; ++i)
				if (number == i + 1 || !stricmp(arg, s_levels[i]))
					level = s_levels[i];
			if (!level)
			{
				CryLogAlways("usage: coop_host [level]   (island village rescue harbor tank mine core ice sphere ascension fleet, or 1-11)");
				return;
			}
		}
		gEnv->pConsole->ExecuteString("exec coop_settings.cfg");
		string cmd;
		cmd.Format("map multiplayer/tia/coop_%s s", level);
		gEnv->pConsole->ExecuteString(cmd.c_str());
	}

	ICVar* s_pEnglishKeyboard = 0;
	bool s_keyboardDone = false;

	void CmdStatus(IConsoleCmdArgs*)
	{
		if (s_hosting && s_hostCode)
			CryLogAlways("[CoopRelay] friends join with: coop_join %d   (%d connected, %d of them directly; relay %d ms)",
				(int)s_hostCode, (int)s_hostFriends, (int)s_hostDirect, (int)s_relayRtt);
		else if (s_hosting)
			CryLogAlways("[CoopRelay] waiting for the relay %s%s%s", s_pRelay->GetString(),
				s_hostError ? ": " : "", s_hostError ? ErrorText(s_hostError) : "");
		else
			CryLogAlways("[CoopRelay] not hosting a coop game (coop_relay_enable %d, relay %s)",
				s_pEnable->GetIVal(), s_pRelay->GetString());
		if (s_joinAgent.Running())
		{
			if (s_joinDirectRtt >= 0)
				CryLogAlways("[CoopRelay] joined, direct connection to the host: %d ms (through the relay: %d ms)", (int)s_joinDirectRtt, (int)s_relayRtt);
			else
				CryLogAlways("[CoopRelay] joined through the relay: %d ms (no direct connection)", (int)s_relayRtt);
		}
	}
}

void CoopRelay::Init()
{
	if (!gEnv->pConsole || s_pRelay)
		return;
	s_pRelay = gEnv->pConsole->RegisterString("coop_relay", "wss://crysis.46-225-103-75.sslip.io/", VF_DUMPTODISK,
		"Crysis Coop: relay that connects coop hosts and their friends over the internet (WebSocket URL)");
	s_pEnable = gEnv->pConsole->RegisterInt("coop_relay_enable", 1, VF_DUMPTODISK,
		"Crysis Coop: 1 = a coop host is reachable through the relay (friends use coop_join <code>)");
	s_pDirect = gEnv->pConsole->RegisterInt("coop_direct", 1, VF_DUMPTODISK,
		"Crysis Coop: 1 = the host and his friends try to reach each other directly (lower latency); the relay stays the fallback");
	s_pCode = gEnv->pConsole->RegisterInt("coop_relay_code", 0, VF_READONLY,
		"Crysis Coop: code of the coop game this machine hosts through the relay (0 = none)");
	s_pEnglishKeyboard = gEnv->pConsole->RegisterInt("coop_english_keyboard", 1, VF_DUMPTODISK,
		"Crysis Coop: 1 = English keyboard layout in the game window (console commands can be typed with any system layout)");
	gEnv->pConsole->AddCommand("coop_host", CmdHost, 0, "Crysis Coop: host a coop game: coop_host [level]  (default: the first level)");
	gEnv->pConsole->AddCommand("coop_join", CmdJoin, 0, "Crysis Coop: join a friend's coop game: coop_join <code>");
	gEnv->pConsole->AddCommand("coop_relay_status", CmdStatus, 0, "Crysis Coop: the code friends join with, connection and latency");
}

void CoopRelay::Update(float frameTime)
{
	if (!s_pEnable)
		return;

	// once, when the command line cvars are set: typing console commands with a
	// Cyrillic (or any non-Latin) layout gives garbage, so the game's own
	// thread gets the English layout (the rest of the system keeps its own)
	if (!s_keyboardDone)
	{
		s_keyboardDone = true;
		if (s_pEnglishKeyboard && s_pEnglishKeyboard->GetIVal())
			LoadKeyboardLayoutA("00000409", KLF_ACTIVATE);
	}

	// ---- host
	// coop_continue: not before the save is loaded (the load disconnects
	// everybody but the host)
	const bool hosting = gEnv->bServer && CoopAI::IsCoopSession() && g_pGame->GetIGameFramework()->IsGameStarted() && !CoopSave::IsLoadPending();
	if (!s_hosting)
	{
		if (hosting && s_pEnable->GetIVal())
			StartHost();
	}
	else
	{
		// a level change keeps the game (and the friends): only a server that
		// is gone for a while ends the relay session
		s_serverGoneFor = (gEnv->bServer && CoopAI::IsNetGame()) ? 0.0f : s_serverGoneFor + frameTime;
		if (s_serverGoneFor > 10.0f || !s_pEnable->GetIVal())
		{
			CryLogAlways("[CoopRelay] hosting ended, relay session closed");
			StopHost();
		}
	}
	if (s_hosting)
	{
		if (s_hostFriends != s_loggedFriends || s_hostDirect != s_loggedHostDirect)
		{
			s_loggedFriends = s_hostFriends;
			s_loggedHostDirect = s_hostDirect;
			CryLogAlways("[CoopRelay] %d friend(s) connected, %d of them directly (relay %d ms)", s_loggedFriends, s_loggedHostDirect, (int)s_relayRtt);
		}
		if (s_hostError != s_loggedHostError)
		{
			s_loggedHostError = s_hostError;
			if (s_loggedHostError)
				CryLogAlways("[CoopRelay] %s (%s)", ErrorText(s_loggedHostError), s_pRelay->GetString());
		}
		if (s_hostReconnects != s_loggedReconnects)
		{
			s_loggedReconnects = s_hostReconnects;
			CryLogAlways("[CoopRelay] connection to the relay lost, reconnecting (the code stays the same)");
		}
		const int code = s_hostCode;
		if (code && code != s_announcedCode)
		{
			s_announcedCode = code;
			char text[16];
			_snprintf(text, sizeof(text), "%d", code);
			text[sizeof(text) - 1] = 0;
			s_pCode->ForceSet(text);
			string msg;
			msg.Format("Co-op code: %d  -  your friend types in the console: coop_join %d", code, code);
			CryLogAlways("[CoopRelay] %s", msg.c_str());
			if (CGameRules* pRules = g_pGame->GetGameRules())
				pRules->SendTextMessage(eTextMessageCenter, msg.c_str(), eRMI_ToAllClients);
		}
	}

	// ---- friend
	if (s_joinAgent.Running())
	{
		const int port = s_joinPort;
		if (port && !s_joinConnected)
		{
			s_joinConnected = port;
			string cmd;
			cmd.Format("connect 127.0.0.1 %d", port);
			CryLogAlways("[CoopRelay] tunnel to the host open, %s", cmd.c_str());
			gEnv->pConsole->ExecuteString(cmd.c_str());
		}
		if (s_joinError && !s_joinErrorShown)
		{
			s_joinErrorShown = true;
			CryLogAlways("[CoopRelay] cannot join: %s", ErrorText(s_joinError));
		}
		if (s_joinLost)
		{
			s_joinLost = false;
			CryLogAlways("[CoopRelay] connection to the relay lost, reconnecting");
		}
		const bool direct = s_joinDirectRtt >= 0;
		if (direct != (s_loggedJoinDirect >= 0))
		{
			s_loggedJoinDirect = direct ? (int)s_joinDirectRtt : -1;
			if (direct)
				CryLogAlways("[CoopRelay] direct connection to the host: %d ms (through the relay: %d ms)", (int)s_joinDirectRtt, (int)s_relayRtt);
			else
				CryLogAlways("[CoopRelay] no direct connection to the host, playing through the relay (%d ms)", (int)s_relayRtt);
		}
	}
}

void CoopRelay::OnDisconnected()
{
	// the friend left the game (or was dropped): the tunnel is not needed
	if (s_joinAgent.Running() && !gEnv->bServer)
	{
		s_joinAgent.Stop();
		s_joinConnected = 0;
		CryLogAlways("[CoopRelay] left the coop game, tunnel closed");
	}
}

void CoopRelay::Shutdown()
{
	s_joinAgent.Stop();
	StopHost();
}
