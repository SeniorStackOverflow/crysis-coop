// Crysis Coop: the campaigns' checkpoints on the relay server ("cloud"), so
// the campaign is not tied to one PC: every player who played it may carry
// it on as the host. Talks to the relay over its own WebSocket connection in
// a worker thread (see Relay/coop_relay.py, messages 20-34).
#pragma once

#include <vector>

namespace CoopCloud
{
	struct SCampaign
	{
		string id;              // 16 hex digits
		bool own;               // stored by this player first (else: he played in it)
		unsigned int stamp;     // time of the last checkpoint (unix)
		string level, checkpoint, name, host;
		bool hasPrevious;
	};

	enum EStatus { eS_Idle, eS_Busy, eS_Done, eS_Failed };

	void Init();                    // console variables
	bool Enabled();                 // coop_cloud 1
	void Update();                  // main thread: reports what the worker did
	void Shutdown();

	// a checkpoint (see CoopSave: "CCSV" blob); the newest one per campaign
	// wins if an older one is still waiting
	void Upload(const string& campaign, const std::vector<char>& blob);

	// the campaigns this player may load; Status then Done / Failed
	void RequestList();
	EStatus ListStatus(std::vector<SCampaign>* pOut);

	// which: 0 the last checkpoint, 1 the one before
	void RequestDownload(const string& campaign, int which);
	EStatus DownloadStatus(std::vector<char>* pOut);

	// the owner deletes it, anybody else drops it from his list
	void RequestDelete(const string& campaign);
}
