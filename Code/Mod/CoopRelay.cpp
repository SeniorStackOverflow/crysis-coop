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
//
// The host keeps his code (it belongs to his player key). When his game
// restarts (coop_continue, coop_load, a new game) or loses its connection,
// the friends' tunnels stay open and wait; once the host's game is ready
// again their games connect by themselves.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")

#include "StdAfx.h"
#include "CoopRelay.h"
#include "CoopAI.h"
#include "CoopCloud.h"
#include "CoopSave.h"
#include "CoopWebSocket.h"
#include "Game.h"
#include "GameRules.h"
#include "ICryPak.h"
#include "Menus/FlashMenuObject.h"
#include "Menus/MPHub.h"

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
		OP_FRIEND_CANDIDATES, OP_CANDIDATES_FOR_HOST, OP_HOST_CANDIDATES, OP_CANDIDATES_FOR_FRIEND,
		OP_HOST_STATE, OP_FRIEND_ID
	};
	const unsigned char PROTOCOL_VERSION = 2;
	// the host's game, as the relay tells the friends (OP_HOST_STATE)
	enum { HS_NONE = 0, HS_RESTARTING = 1, HS_READY = 2, HS_GONE = 3, HS_CLOSED = 4 };

	typedef CCoopWebSocket CWebSocket;
	bool LoadWebSocketApi() { return CCoopWebSocket::Supported(); }

	// --------------------------------------------------------------------
	// the player: a random secret key made once on this PC; the relay only
	// keeps its public id (the first 8 bytes of its SHA-256). Kept in
	// %USER%/coop_player.txt with the code of the game last joined.
#ifndef PROV_RSA_AES
#define PROV_RSA_AES 24
#endif
#ifndef CALG_SHA_256
#define CALG_SHA_256 0x0000800c
#endif
	const char* const PLAYER_FILE = "%USER%/coop_player.txt";
	unsigned char s_key[16];
	unsigned char s_id[8];
	bool s_haveKey = false;
	int s_lastJoinCode = 0;

	string UserPath(const char* path)
	{
		char buf[ICryPak::g_nMaxPath];
		const char* p = gEnv->pCryPak->AdjustFileName(path, buf, ICryPak::FLAGS_NO_MASTER_FOLDER_MAPPING | ICryPak::FLAGS_FOR_WRITING);
		return p ? string(p) : string(path);
	}

	bool Sha256(const void* data, DWORD len, unsigned char out[32])
	{
		HCRYPTPROV prov = 0;
		HCRYPTHASH hash = 0;
		bool ok = false;
		if (CryptAcquireContextA(&prov, 0, 0, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
		{
			if (CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash))
			{
				DWORD n = 32;
				ok = CryptHashData(hash, (const BYTE*)data, len, 0) && CryptGetHashParam(hash, HP_HASHVAL, out, &n, 0);
				CryptDestroyHash(hash);
			}
			CryptReleaseContext(prov, 0);
		}
		return ok;
	}

	bool SecureRandom(unsigned char* p, DWORD n)
	{
		HCRYPTPROV prov = 0;
		bool ok = false;
		if (CryptAcquireContextA(&prov, 0, 0, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
		{
			ok = CryptGenRandom(prov, n, p) != FALSE;
			CryptReleaseContext(prov, 0);
		}
		return ok;
	}

	string Hex(const unsigned char* p, int n)
	{
		string s;
		for (int i = 0; i < n; ++i)
		{
			char b[3];
			_snprintf(b, sizeof(b), "%02x", p[i]);
			b[2] = 0;
			s += b;
		}
		return s;
	}

	void SavePlayer()
	{
		FILE* f = fopen(UserPath(PLAYER_FILE).c_str(), "wb");
		if (!f)
			return;
		fprintf(f, "key=%s\nlast_code=%d\n", Hex(s_key, 16).c_str(), s_lastJoinCode);
		fclose(f);
	}

	void LoadPlayer()
	{
		if (s_haveKey)
			return;
		if (FILE* f = fopen(UserPath(PLAYER_FILE).c_str(), "rb"))
		{
			char line[256];
			while (fgets(line, sizeof(line), f))
			{
				string s = string(line).Trim();
				if (!strncmp(s.c_str(), "key=", 4) && s.length() == 36)
				{
					for (int i = 0; i < 16; ++i)
						s_key[i] = (unsigned char)strtoul(s.substr(4 + i * 2, 2).c_str(), 0, 16);
					s_haveKey = true;
				}
				else if (!strncmp(s.c_str(), "last_code=", 10))
					s_lastJoinCode = atoi(s.c_str() + 10);
			}
			fclose(f);
		}
		if (!s_haveKey)
		{
			if (!SecureRandom(s_key, 16))
				return;
			s_haveKey = true;
			SavePlayer();
			CryLogAlways("[CoopRelay] new player key made (%s)", PLAYER_FILE);
		}
		unsigned char digest[32];
		if (Sha256(s_key, 16, digest))
			memcpy(s_id, digest, 8);
		else
			s_haveKey = false;
	}

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
	std::atomic<int> s_joinHostState(HS_NONE);  // friend: the host's game as the relay reports it

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
		CHostAgent() : m_stop(false), m_state(HS_RESTARTING), m_gamePort(64087), m_useDirect(true), m_direct(INVALID_SOCKET)
		{
			memset(m_key, 0, sizeof(m_key));
		}

		void Start(const string& url, const unsigned char* key, int gamePort, bool useDirect)
		{
			m_url = url;
			memcpy(m_key, key, sizeof(m_key));
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

		// the host's game (HS_RESTARTING / HS_READY) for the friends; sent
		// now, and again after every registration
		void SetState(int state)
		{
			if (m_state.exchange(state) == state)
				return;
			const unsigned char msg[2] = { OP_HOST_STATE, (unsigned char)state };
			m_ws.Send(msg, sizeof(msg));
		}

		string IdOfPort(int port)
		{
			std::lock_guard<std::mutex> lock(m_lock);
			for (std::map<unsigned short, SFriend>::iterator it = m_friends.begin(); it != m_friends.end(); ++it)
				if (it->second.port == port)
				{
					std::map<unsigned short, string>::iterator id = m_ids.find(it->first);
					return id != m_ids.end() ? id->second : string();
				}
			return string();
		}

	private:
		struct SFriend { SOCKET s; int port; DWORD lastActive; SDirectPath direct; };

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
				unsigned char reg[18] = { OP_REGISTER, PROTOCOL_VERSION };
				memcpy(reg + 2, m_key, 16);
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
				f.s = LocalUdpSocket(&f.port);
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
				if (op == OP_REGISTERED && n >= 13)
				{
					unsigned int code;
					memcpy(&code, &buf[1], 4);
					s_hostCode = (int)code;
					s_hostError = 0;
					const unsigned char state[2] = { OP_HOST_STATE, (unsigned char)m_state };
					m_ws.Send(state, sizeof(state));
				}
				else if (op == OP_FRIEND_ID && n >= 11)
				{
					unsigned short cid;
					memcpy(&cid, &buf[1], 2);
					std::lock_guard<std::mutex> lock(m_lock);
					m_ids[cid] = Hex((const unsigned char*)&buf[3], 8);
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
						if (closing)
							m_ids.erase(it->first);
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
		std::map<unsigned short, string> m_ids;     // client -> player id
		unsigned char m_key[16];
		std::atomic<int> m_state;
		int m_gamePort;
		bool m_useDirect;
		SOCKET m_direct;
	};

	// --------------------------------------------------------------------
	// friend: a local UDP port for the game, the direct path and the tunnel
	class CJoinAgent
	{
	public:
		CJoinAgent() : m_stop(false), m_code(0), m_useDirect(true), m_direct(INVALID_SOCKET), m_haveGame(false)
		{
			memset(m_key, 0, sizeof(m_key));
		}

		void Start(const string& url, const unsigned char* key, int code, bool useDirect)
		{
			m_url = url;
			memcpy(m_key, key, sizeof(m_key));
			m_code = (unsigned int)code;
			s_joinHostState = HS_NONE;
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
				unsigned char join[30] = { OP_JOIN, PROTOCOL_VERSION };
				memcpy(join + 2, &m_code, 4);
				memcpy(join + 6, m_path.token, 8);
				memcpy(join + 14, m_key, 16);
				m_ws.Send(join, sizeof(join));
				const int n = m_ws.Receive(buf);
				if (n < 1 || (unsigned char)buf[0] != OP_JOINED)
				{
					s_joinError = (n >= 2 && (unsigned char)buf[0] == OP_ERROR) ? (unsigned char)buf[1] : 5;
					m_ws.Close();
					break;
				}
				s_joinHostState = n >= 2 ? (unsigned char)buf[1] : HS_READY;
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
				else if (op == OP_HOST_STATE && n >= 2)
					s_joinHostState = (unsigned char)buf[1];
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
		unsigned char m_key[16];
		unsigned int m_code;
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

	// friend: the game lost the host. Whether the host restarts (then the
	// tunnel stays and the game rejoins once he is ready) or the player left
	// is known from the relay's host state.
	bool s_gameDropped = false;
	float s_droppedAt = 0.0f;
	bool s_waitingForHost = false;
	float s_waitSince = 0.0f;
	int s_loggedHostState = HS_NONE;
	float s_rejoinedAt = -100.0f;
	int s_rejoinTries = 0;
	int s_dropCause = 0;
	string s_dropText;
	const float HOST_WAIT = 600.0f;
	// host: the map command of a restart, run once the friends were told
	string s_restartCommand;
	float s_restartAt = 0.0f;

	float Now()
	{
		return gEnv->pTimer->GetAsyncCurTime();
	}

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
		s_hostAgent.Start(s_pRelay->GetString(), s_key, gamePort, s_pDirect->GetIVal() != 0);
		s_hosting = true;
		s_serverGoneFor = 0.0f;
		CryLogAlways("[CoopRelay] hosting through the relay %s (game port %d)", s_pRelay->GetString(), gamePort);
	}

	void StopJoin(const char* why)
	{
		if (!s_joinAgent.Running())
			return;
		s_joinAgent.Stop();
		s_joinConnected = 0;
		s_gameDropped = false;
		s_waitingForHost = false;
		CryLogAlways("[CoopRelay] %s, tunnel closed", why);
	}

	// the friend's menu while he waits for the host: the message box the
	// menu shows a lost connection in (it comes up once the menu is there)
	bool s_waitMessageDue = false;
	void ShowWaiting(bool show)
	{
		s_waitMessageDue = show;
	}

	void UpdateWaitingMessage(float now)
	{
		if (!s_waitMessageDue)
			return;
		CFlashMenuObject* pMenu = g_pGame ? g_pGame->GetMenu() : 0;
		if (CMPHub* pHub = pMenu ? pMenu->GetMPHub() : 0)
		{
			s_waitMessageDue = false;
			pHub->ShowError("The host is loading the game. You will join again by yourself.", false);
		}
	}

	// the friend's menu: why he is out of the game
	void ShowReason(const char* text)
	{
		CFlashMenuObject* pMenu = g_pGame ? g_pGame->GetMenu() : 0;
		if (CMPHub* pHub = pMenu ? pMenu->GetMPHub() : 0)
		{
			pHub->CloseLoadingDlg();
			if (text)
				pHub->ShowError(text, false);
			else
				pHub->DisconnectError((EDisconnectionCause)s_dropCause, false, s_dropText.c_str());
		}
	}

	void Rejoin()
	{
		string cmd;
		cmd.Format("connect 127.0.0.1 %d", (int)s_joinPort);
		CryLogAlways("[CoopRelay] the host is ready again: %s", cmd.c_str());
		ShowWaiting(false);
		s_rejoinedAt = Now();
		gEnv->pConsole->ExecuteString(cmd.c_str());
	}

	const char* ErrorText(int e)
	{
		switch (e)
		{
		case 1: return "the relay is full, try again later";
		case 2: return "the relay runs another version of the mod (update the mod)";
		case 3: return "no coop game with this code (check the code; the host must be in the game)";
		case 4: return "that coop game is full";
		case 5: return "cannot reach the relay (internet connection?)";
		case 6: return "this Windows has no WebSocket support (Windows 8 or later needed)";
		}
		return "unknown error";
	}

	// coop_join [code]: join a friend's coop game through the relay (no code:
	// the game joined last time; a host keeps his code)
	void CmdJoin(IConsoleCmdArgs* pArgs)
	{
		LoadPlayer();
		const int code = pArgs->GetArgCount() > 1 ? atoi(pArgs->GetArg(1)) : s_lastJoinCode;
		if (code <= 0)
		{
			CryLogAlways("usage: coop_join <code>   (the code the host sees when he starts a coop game)");
			return;
		}
		if (!s_haveKey)
		{
			CryLogAlways("[CoopRelay] no player key (%s could not be written): cannot join", PLAYER_FILE);
			return;
		}
		StopJoin("left the last game");
		s_joinConnected = 0;
		s_loggedJoinDirect = -1;
		s_joinErrorShown = false;
		s_loggedHostState = HS_NONE;
		s_rejoinTries = 0;
		if (code != s_lastJoinCode)
		{
			s_lastJoinCode = code;
			SavePlayer();
		}
		s_joinAgent.Start(s_pRelay->GetString(), s_key, code, s_pDirect->GetIVal() != 0);
		CryLogAlways("[CoopRelay] joining coop game %d through %s ...", code, s_pRelay->GetString());
	}

	// coop_leave: leave the joined game for good (no joining again)
	void CmdLeave(IConsoleCmdArgs*)
	{
		ShowWaiting(false);
		StopJoin("left the coop game");
		gEnv->pConsole->ExecuteString("disconnect");
	}

	ICVar* s_pEnglishKeyboard = 0;
	bool s_keyboardDone = false;

	void CmdStatus(IConsoleCmdArgs*)
	{
		CryLogAlways("[CoopRelay] player id %s", s_haveKey ? Hex(s_id, 8).c_str() : "(none)");
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
			if (s_waitingForHost)
				CryLogAlways("[CoopRelay] joined game %d: waiting for the host to be ready", s_lastJoinCode);
			else if (s_joinDirectRtt >= 0)
				CryLogAlways("[CoopRelay] joined, direct connection to the host: %d ms (through the relay: %d ms)", (int)s_joinDirectRtt, (int)s_relayRtt);
			else
				CryLogAlways("[CoopRelay] joined through the relay: %d ms (no direct connection)", (int)s_relayRtt);
		}
	}

	void UpdateFriend()
	{
		if (!s_joinAgent.Running())
			return;
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
			if (s_waitingForHost)
			{
				ShowReason("The host's game is gone");
				StopJoin("the host's game is gone");
			}
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

		const int state = s_joinHostState;
		if (state != s_loggedHostState)
		{
			static const char* names[] = { "?", "restarting", "ready", "connection lost", "gone" };
			CryLogAlways("[CoopRelay] the host's game: %s", names[state >= 0 && state <= 4 ? state : 0]);
			s_loggedHostState = state;
		}
		const float now = Now();
		if (s_gameDropped)
		{
			if (state == HS_RESTARTING || state == HS_GONE)
			{
				s_gameDropped = false;
				s_waitingForHost = true;
				s_waitSince = now;
				CryLogAlways("[CoopRelay] the host restarts his game: waiting for him, then joining again");
				ShowWaiting(true);
			}
			else if (state == HS_READY && now - s_rejoinedAt < 60.0f && s_rejoinTries < 3)
			{
				// joining again right after he got ready did not work: once more
				s_gameDropped = false;
				s_waitingForHost = true;
				s_waitSince = now;
				++s_rejoinTries;
				ShowWaiting(true);
			}
			else if (state == HS_CLOSED || now - s_droppedAt > 3.0f)
			{
				// the host is still there: a real disconnection (or he closed)
				ShowReason(state == HS_CLOSED ? "The host has closed the game" : 0);
				StopJoin(state == HS_CLOSED ? "the host closed the game" : "lost the connection to the host");
			}
		}
		if (s_waitingForHost)
		{
			UpdateWaitingMessage(now);
			if (state == HS_READY && now - s_waitSince > (s_rejoinTries ? 5.0f : 1.0f))
			{
				s_waitingForHost = false;
				Rejoin();
			}
			else if (state == HS_CLOSED || now - s_waitSince > HOST_WAIT)
			{
				ShowReason(state == HS_CLOSED ? "The host has closed the game" : "The host did not come back");
				StopJoin(state == HS_CLOSED ? "the host closed the game" : "the host did not come back");
			}
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
	// this player's own settings: the engine sends a server's console
	// variables to every client that connects, these are not taken over
	ICVar* own[] = { s_pRelay, s_pEnable, s_pDirect, s_pCode, s_pEnglishKeyboard };
	for (int i = 0; i < (int)(sizeof(own) / sizeof(own[0])); ++i)
		own[i]->SetFlags(own[i]->GetFlags() | VF_NOT_NET_SYNCED);
	gEnv->pConsole->AddCommand("coop_join", CmdJoin, 0, "Crysis Coop: join a friend's coop game: coop_join <code>  (no code: the last one)");
	gEnv->pConsole->AddCommand("coop_leave", CmdLeave, 0, "Crysis Coop: leave the joined coop game (no joining again by itself)");
	gEnv->pConsole->AddCommand("coop_relay_status", CmdStatus, 0, "Crysis Coop: the code friends join with, connection and latency");
	CoopCloud::Init();
}

void CoopRelay::Update(float frameTime)
{
	if (!s_pEnable)
		return;

	// once, when the command line cvars are set: the player key, and the
	// English keyboard layout (console commands typed with a Cyrillic or any
	// other non-Latin layout give garbage; the rest of the system keeps its own)
	if (!s_keyboardDone)
	{
		s_keyboardDone = true;
		LoadPlayer();
		if (s_pEnglishKeyboard && s_pEnglishKeyboard->GetIVal())
			LoadKeyboardLayoutA("00000409", KLF_ACTIVATE);
	}
	CoopCloud::Update();

	// ---- host
	// the friends may connect once the level runs (coop_continue: once the
	// save is loaded, the load disconnects everybody but the host)
	const bool ready = gEnv->bServer && CoopAI::IsCoopSession() && g_pGame->GetIGameFramework()->IsGameStarted() && !CoopSave::IsLoadPending();
	if (!s_hosting)
	{
		if (ready && s_pEnable->GetIVal() && s_haveKey)
		{
			StopJoin("hosting a game now");
			StartHost();
		}
	}
	else
	{
		// a level change or a restart keeps the game (and the friends): only a
		// server that is gone for a while ends the relay session
		s_serverGoneFor = (gEnv->bServer && CoopAI::IsNetGame()) ? 0.0f : s_serverGoneFor + frameTime;
		if (s_serverGoneFor > 10.0f || !s_pEnable->GetIVal())
		{
			CryLogAlways("[CoopRelay] hosting ended, relay session closed");
			StopHost();
		}
	}
	if (!s_restartCommand.empty() && Now() >= s_restartAt)
	{
		const string cmd = s_restartCommand;
		s_restartCommand.clear();
		gEnv->pConsole->ExecuteString(cmd.c_str());
	}
	if (s_hosting)
	{
		s_hostAgent.SetState(ready && s_restartCommand.empty() ? HS_READY : HS_RESTARTING);
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
			msg.Format("Co-op code: %d (always the same)  -  your friend types in the console: coop_join %d", code, code);
			CryLogAlways("[CoopRelay] %s", msg.c_str());
			if (CGameRules* pRules = g_pGame->GetGameRules())
				pRules->SendTextMessage(eTextMessageCenter, msg.c_str(), eRMI_ToAllClients);
		}
	}

	// ---- friend
	UpdateFriend();
}

void CoopRelay::OnDisconnected(int cause, const char* description)
{
	if (!s_joinAgent.Running() || gEnv->bServer || s_waitingForHost)
		return;
	// the player left himself
	if (cause == eDC_UserRequested)
	{
		StopJoin("left the coop game");
		return;
	}
	// the host restarts, or the connection was lost for good: decided in
	// UpdateFriend once the relay's word on the host is known
	s_gameDropped = true;
	s_droppedAt = Now();
	s_dropCause = cause;
	s_dropText = description ? description : "";
}

void CoopRelay::Shutdown()
{
	s_joinAgent.Stop();
	StopHost();
	CoopCloud::Shutdown();
}

void CoopRelay::GetPlayerKey(unsigned char key[16])
{
	LoadPlayer();
	memcpy(key, s_key, 16);
}

string CoopRelay::LocalPlayerId()
{
	return s_haveKey ? Hex(s_id, 8) : string();
}

string CoopRelay::PlayerIdOfPort(int port)
{
	return s_hosting ? s_hostAgent.IdOfPort(port) : string();
}

const char* CoopRelay::RelayUrl()
{
	return s_pRelay ? s_pRelay->GetString() : "";
}

void CoopRelay::RestartGame(const char* mapCommand)
{
	if (s_joinAgent.Running())
	{
		ShowWaiting(false);
		StopJoin("hosting a game now");
	}
	if (s_hosting && s_hostFriends > 0)
	{
		// the friends hear it from the relay before their games lose this one
		s_hostAgent.SetState(HS_RESTARTING);
		s_restartCommand = mapCommand;
		s_restartAt = Now() + 1.0f;
		CryLogAlways("[CoopRelay] the game restarts: the friends wait and join again by themselves");
		return;
	}
	gEnv->pConsole->ExecuteString(mapCommand);
}

bool CoopRelay::HandlesDisconnect()
{
	return s_joinAgent.Running() && !gEnv->bServer;
}

string CoopRelay::RandomHex(int bytes)
{
	unsigned char b[32];
	bytes = bytes < 1 ? 1 : bytes > 32 ? 32 : bytes;
	if (!SecureRandom(b, bytes))
		RandomBytes(b, bytes);
	return Hex(b, bytes);
}
