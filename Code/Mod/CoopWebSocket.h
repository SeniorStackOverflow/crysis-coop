// Crysis Coop: WebSocket client (WinHTTP, Windows 8 or later) for the
// relay's tunnel and the cloud checkpoints.
#pragma once

#include <mutex>
#include <vector>

class CCoopWebSocket
{
public:
	CCoopWebSocket() : m_session(0), m_connect(0), m_ws(0) {}
	~CCoopWebSocket() { Close(); }

	// ws://host[:port]/path or wss://...; blocking
	bool Open(const char* url);
	// one binary message; from any thread
	bool Send(const void* data, int len);
	// blocking: one whole message into buf (at most buf.size() bytes), -1 when
	// the connection ended or the message does not fit
	int Receive(std::vector<char>& buf);
	void Close();
	// ends a Receive blocked in another thread
	void Abort();

	// this Windows has WinHTTP's WebSocket functions
	static bool Supported();

private:
	void* m_session;
	void* m_connect;
	void* m_ws;
	std::mutex m_sendLock;
};
