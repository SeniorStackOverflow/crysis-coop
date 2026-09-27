// Crysis Coop: campaigns' checkpoints on the relay server (see CoopCloud.h).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "StdAfx.h"
#include "CoopCloud.h"
#include "CoopRelay.h"
#include "CoopWebSocket.h"

#include <atomic>
#include <stdarg.h>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace
{
	enum
	{
		OP_PING = 3, OP_ERROR = 7,
		OP_HELLO = 20, OP_WELCOME, OP_UPLOAD, OP_GO, OP_CHUNK, OP_DONE, OP_STORED,
		OP_LIST, OP_CAMPAIGNS, OP_DOWNLOAD, OP_FILE, OP_DELETE = 33, OP_DELETED
	};
	const unsigned char PROTOCOL_VERSION = 2;
	const int CHUNK = 48 * 1024;
	const size_t MAX_BLOB = 16 << 20;

	ICVar* s_pCloud = 0;

	enum EJob { eJ_Upload, eJ_List, eJ_Download, eJ_Delete };
	struct SJob
	{
		EJob type;
		string campaign;
		int which;
		int attempts;
		DWORD notBefore;
	};

	std::mutex s_lock;
	std::condition_variable s_wake;
	std::deque<SJob> s_jobs;
	// campaign -> the newest checkpoint to send; gen counts the checkpoints
	// handed in, so an upload that finished knows whether a newer one came
	struct SUpload { std::vector<char> blob; int gen; };
	std::map<string, SUpload> s_uploads;
	std::vector<string> s_log;                          // for the main thread (the engine log is not thread safe)
	std::thread s_thread;
	std::atomic<bool> s_done(true);     // the thread has ended (see Shutdown)
	bool s_stop = false;
	string s_url;
	unsigned char s_key[16];

	CoopCloud::EStatus s_listStatus = CoopCloud::eS_Idle;
	std::vector<CoopCloud::SCampaign> s_list;
	CoopCloud::EStatus s_downloadStatus = CoopCloud::eS_Idle;
	std::vector<char> s_download;

	void Report(const char* fmt, ...)
	{
		char text[512];
		va_list args;
		va_start(args, fmt);
		_vsnprintf(text, sizeof(text), fmt, args);
		va_end(args);
		text[sizeof(text) - 1] = 0;
		std::lock_guard<std::mutex> lock(s_lock);
		s_log.push_back(text);
	}

	const char* ErrorText(int e)
	{
		switch (e)
		{
		case 8: return "not allowed (not your campaign)";
		case 9: return "too big, or no room left on the server";
		case 10: return "not found";
		case 11: return "too often, retrying";
		case 12: return "the server did not accept the data";
		case 100: return "cannot reach the server";
		case 101: return "the connection broke";
		}
		return "error";
	}

	// a connection that said HELLO; 0 = fine, else an error number
	int Connect(CCoopWebSocket& ws, std::vector<char>& buf)
	{
		if (!ws.Open(s_url.c_str()))
			return 100;
		unsigned char hello[18] = { OP_HELLO, PROTOCOL_VERSION };
		memcpy(hello + 2, s_key, 16);
		const int n = ws.Send(hello, sizeof(hello)) ? ws.Receive(buf) : -1;
		if (n < 1)
			return 101;
		if ((unsigned char)buf[0] == OP_ERROR)
			return n >= 2 ? (unsigned char)buf[1] : 101;
		return (unsigned char)buf[0] == OP_WELCOME ? 0 : 101;
	}

	void CampaignBytes(const string& hex, unsigned char out[8])
	{
		for (int i = 0; i < 8; ++i)
			out[i] = (unsigned char)strtoul(hex.substr(i * 2, 2).c_str(), 0, 16);
	}

	// the answer to a request: 0 = the expected op arrived, else an error number
	int Expect(CCoopWebSocket& ws, std::vector<char>& buf, unsigned char op, int* pLen = 0)
	{
		const int n = ws.Receive(buf);
		if (n < 1)
			return 101;
		if ((unsigned char)buf[0] == OP_ERROR)
			return n >= 2 ? (unsigned char)buf[1] : 101;
		if ((unsigned char)buf[0] != op)
			return 101;
		if (pLen)
			*pLen = n;
		return 0;
	}

	int DoUpload(CCoopWebSocket& ws, std::vector<char>& buf, const string& campaign, const std::vector<char>& blob)
	{
		unsigned char head[13] = { OP_UPLOAD };
		CampaignBytes(campaign, head + 1);
		const unsigned int size = (unsigned int)blob.size();
		memcpy(head + 9, &size, 4);
		if (!ws.Send(head, sizeof(head)))
			return 101;
		if (int e = Expect(ws, buf, OP_GO))
			return e;
		std::vector<char> chunk(CHUNK + 1);
		chunk[0] = (char)OP_CHUNK;
		for (size_t pos = 0; pos < blob.size(); pos += CHUNK)
		{
			const size_t n = (std::min)((size_t)CHUNK, blob.size() - pos);
			memcpy(&chunk[1], &blob[pos], n);
			if (!ws.Send(&chunk[0], (int)n + 1))
				return 101;
		}
		const unsigned char done = OP_DONE;
		if (!ws.Send(&done, 1))
			return 101;
		return Expect(ws, buf, OP_STORED);
	}

	int DoList(CCoopWebSocket& ws, std::vector<char>& buf, std::vector<CoopCloud::SCampaign>& out)
	{
		const unsigned char op = OP_LIST;
		int n = 0;
		if (!ws.Send(&op, 1))
			return 101;
		if (int e = Expect(ws, buf, OP_CAMPAIGNS, &n))
			return e;
		string text(&buf[1], n - 1);
		size_t start = 0;
		while (start < text.length())
		{
			size_t end = text.find('\n', start);
			if (end == string::npos)
				end = text.length();
			const string line = text.substr(start, end - start);
			start = end + 1;
			std::vector<string> f;
			size_t a = 0;
			for (;;)
			{
				const size_t b = line.find('\t', a);
				f.push_back(line.substr(a, b == string::npos ? string::npos : b - a));
				if (b == string::npos)
					break;
				a = b + 1;
			}
			if (f.size() < 8 || f[0].length() != 16)
				continue;
			CoopCloud::SCampaign c;
			c.id = f[0];
			c.own = f[1] == "own";
			c.stamp = (unsigned int)strtoul(f[2].c_str(), 0, 10);
			c.level = f[3];
			c.checkpoint = f[4];
			c.name = f[5];
			c.host = f[6];
			c.hasPrevious = f[7] == "1";
			out.push_back(c);
		}
		return 0;
	}

	int DoDownload(CCoopWebSocket& ws, std::vector<char>& buf, const string& campaign, int which, std::vector<char>& out)
	{
		unsigned char req[10] = { OP_DOWNLOAD };
		CampaignBytes(campaign, req + 1);
		req[9] = (unsigned char)which;
		int n = 0;
		if (!ws.Send(req, sizeof(req)))
			return 101;
		if (int e = Expect(ws, buf, OP_FILE, &n))
			return e;
		unsigned int size = 0;
		if (n >= 5)
			memcpy(&size, &buf[1], 4);
		if (size > MAX_BLOB)
			return 12;
		out.clear();
		out.reserve(size);
		for (;;)
		{
			n = ws.Receive(buf);
			if (n < 1)
				return 101;
			const unsigned char op = (unsigned char)buf[0];
			if (op == OP_DONE)
				break;
			if (op != OP_CHUNK || out.size() + n - 1 > size)
				return 12;
			out.insert(out.end(), buf.begin() + 1, buf.begin() + n);
		}
		return out.size() == size ? 0 : 12;
	}

	void Run()
	{
		std::vector<char> buf(CHUNK + 1024);
		for (;;)
		{
			SJob job;
			std::vector<char> blob;
			{
				std::unique_lock<std::mutex> lock(s_lock);
				for (;;)
				{
					if (s_stop)
						return;
					// the first job whose time has come
					const DWORD now = GetTickCount();
					std::deque<SJob>::iterator it = s_jobs.begin();
					while (it != s_jobs.end() && (int)(it->notBefore - now) > 0)
						++it;
					if (it != s_jobs.end())
					{
						job = *it;
						s_jobs.erase(it);
						break;
					}
					s_wake.wait_for(lock, std::chrono::milliseconds(1000));
				}
				if (job.type == eJ_Upload)
				{
					std::map<string, SUpload>::iterator u = s_uploads.find(job.campaign);
					if (u == s_uploads.end())
						continue;
					blob = u->second.blob;
					job.which = u->second.gen;
				}
			}

			CCoopWebSocket ws;
			int error = Connect(ws, buf);
			std::vector<CoopCloud::SCampaign> list;
			std::vector<char> download;
			if (!error)
			{
				switch (job.type)
				{
				case eJ_Upload: error = DoUpload(ws, buf, job.campaign, blob); break;
				case eJ_List: error = DoList(ws, buf, list); break;
				case eJ_Download: error = DoDownload(ws, buf, job.campaign, job.which, download); break;
				case eJ_Delete:
					{
						unsigned char req[9] = { OP_DELETE };
						CampaignBytes(job.campaign, req + 1);
						error = ws.Send(req, sizeof(req)) ? Expect(ws, buf, OP_DELETED) : 101;
					}
					break;
				}
			}
			ws.Close();

			std::lock_guard<std::mutex> lock(s_lock);
			switch (job.type)
			{
			case eJ_Upload:
				{
					std::map<string, SUpload>::iterator u = s_uploads.find(job.campaign);
					const bool newer = u != s_uploads.end() && u->second.gen != job.which;
					char msg[192];
					msg[0] = 0;
					if (!error)
						_snprintf(msg, sizeof(msg), "[CoopCloud] checkpoint of campaign %s stored on the server (%d KB)", job.campaign.c_str(), (int)(blob.size() / 1024));
					else if (!newer && !((error == 11 || error >= 100) && job.attempts < 6))
						_snprintf(msg, sizeof(msg), "[CoopCloud] checkpoint of campaign %s NOT stored on the server: %s", job.campaign.c_str(), ErrorText(error));
					msg[sizeof(msg) - 1] = 0;
					if (msg[0])
						s_log.push_back(msg);
					if (!error && !newer)
						s_uploads.erase(u);
					else if (newer || ((error == 11 || error >= 100) && job.attempts < 6))
					{
						// a newer checkpoint came meanwhile, or too soon after the last
						// one, or no connection: (again) a little later
						SJob again = job;
						again.attempts = newer ? 0 : job.attempts + 1;
						again.notBefore = GetTickCount() + (error == 11 || (newer && !error) ? 11000 : error ? 20000 * again.attempts : 0);
						s_jobs.push_back(again);
					}
					else
						s_uploads.erase(u);
				}
				break;
			case eJ_List:
				s_list = list;
				s_listStatus = error ? CoopCloud::eS_Failed : CoopCloud::eS_Done;
				if (error)
					s_log.push_back(string("[CoopCloud] the list of saved campaigns is not available: ") + ErrorText(error));
				break;
			case eJ_Download:
				s_download.swap(download);
				s_downloadStatus = error ? CoopCloud::eS_Failed : CoopCloud::eS_Done;
				if (error)
					s_log.push_back(string("[CoopCloud] download failed: ") + ErrorText(error));
				break;
			case eJ_Delete:
				s_log.push_back(error ? string("[CoopCloud] delete failed: ") + ErrorText(error)
					: "[CoopCloud] campaign " + job.campaign + " removed from the server");
				break;
			}
		}
	}

	void Queue(EJob type, const string& campaign, int which)
	{
		CoopRelay::GetPlayerKey(s_key);
		std::lock_guard<std::mutex> lock(s_lock);
		s_url = CoopRelay::RelayUrl();
		SJob job = { type, campaign, which, 0, GetTickCount() };
		s_jobs.push_back(job);
		if (!s_thread.joinable())
		{
			s_stop = false;
			s_done = false;
			s_thread = std::thread([] { Run(); s_done = true; });
		}
		s_wake.notify_all();
	}
}

void CoopCloud::Init()
{
	if (!gEnv->pConsole || s_pCloud)
		return;
	s_pCloud = gEnv->pConsole->RegisterInt("coop_cloud", 1, VF_DUMPTODISK,
		"Crysis Coop: 1 = the host's checkpoints are also kept on the relay server, so every player of the campaign can carry it on");
	// this player's own choice, not the host's (see CoopRelay::Init)
	s_pCloud->SetFlags(s_pCloud->GetFlags() | VF_NOT_NET_SYNCED);
}

bool CoopCloud::Enabled()
{
	return s_pCloud && s_pCloud->GetIVal() != 0 && CoopRelay::RelayUrl()[0];
}

void CoopCloud::Update()
{
	std::vector<string> lines;
	{
		std::lock_guard<std::mutex> lock(s_lock);
		lines.swap(s_log);
	}
	for (size_t i = 0; i < lines.size(); ++i)
		CryLogAlways("%s", lines[i].c_str());
}

void CoopCloud::Shutdown()
{
	{
		std::lock_guard<std::mutex> lock(s_lock);
		s_stop = true;
		s_wake.notify_all();
	}
	if (!s_thread.joinable())
		return;
	// the game is quitting: a transfer stuck on a dead connection must not
	// keep it from closing (under Wine a blocked receive never ends by itself)
	for (DWORD start = GetTickCount(); !s_done && GetTickCount() - start < 3000; Sleep(10))
		;
	if (s_done)
		s_thread.join();
	else
		s_thread.detach();
}

void CoopCloud::Upload(const string& campaign, const std::vector<char>& blob)
{
	if (campaign.length() != 16 || blob.empty() || blob.size() > MAX_BLOB)
		return;
	{
		std::lock_guard<std::mutex> lock(s_lock);
		std::map<string, SUpload>::iterator u = s_uploads.find(campaign);
		if (u != s_uploads.end())
		{
			// the job already queued or running sends the newest
			u->second.blob = blob;
			u->second.gen++;
			return;
		}
		SUpload up = { blob, 1 };
		s_uploads[campaign] = up;
	}
	Queue(eJ_Upload, campaign, 0);
}

void CoopCloud::RequestList()
{
	{
		std::lock_guard<std::mutex> lock(s_lock);
		s_listStatus = eS_Busy;
		s_list.clear();
	}
	Queue(eJ_List, "", 0);
}

CoopCloud::EStatus CoopCloud::ListStatus(std::vector<SCampaign>* pOut)
{
	std::lock_guard<std::mutex> lock(s_lock);
	if (pOut && s_listStatus == eS_Done)
		*pOut = s_list;
	return s_listStatus;
}

void CoopCloud::RequestDownload(const string& campaign, int which)
{
	{
		std::lock_guard<std::mutex> lock(s_lock);
		s_downloadStatus = eS_Busy;
		s_download.clear();
	}
	Queue(eJ_Download, campaign, which);
}

CoopCloud::EStatus CoopCloud::DownloadStatus(std::vector<char>* pOut)
{
	std::lock_guard<std::mutex> lock(s_lock);
	if (pOut && s_downloadStatus == eS_Done)
		pOut->swap(s_download);
	return s_downloadStatus;
}

void CoopCloud::RequestDelete(const string& campaign)
{
	Queue(eJ_Delete, campaign, 0);
}
