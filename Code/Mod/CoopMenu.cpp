// Crysis Coop: the co-op menu (see CoopMenu.h).
//
// Drawn with IUIDraw over the Flash menu, in its virtual 800x600 screen; the
// mouse is taken before the Flash menu sees it. The widgets are laid out
// every frame and remember where they were drawn, a click goes to the one
// under the cursor.
#include "StdAfx.h"
#include "CoopMenu.h"
#include "CoopAI.h"
#include "CoopAgent.h"
#include "CoopCloud.h"
#include "CoopRelay.h"
#include "CoopSave.h"
#include "Game.h"
#include "IUIDraw.h"
#include "IHardwareMouse.h"
#include "Menus/FlashMenuObject.h"

#include <time.h>
#include <vector>

namespace
{
	const float W = 800.0f, H = 600.0f;

	enum EPage { eP_Closed, eP_Main, eP_New, eP_Campaigns, eP_Join, eP_Game };
	EPage s_page = eP_Closed;
	bool s_inGame = false;
	int s_drawnFrame = -100;        // the frame the co-op menu was last drawn in (the Flash menu is up)

	bool Drawn()
	{
		const int frame = gEnv->pRenderer ? gEnv->pRenderer->GetFrameID(false) : 0;
		return frame - s_drawnFrame <= 3 && frame >= s_drawnFrame;
	}

	// widgets
	enum
	{
		ID_NONE, ID_CLOSE, ID_BACK,
		ID_CONTINUE, ID_NEW, ID_CAMPAIGNS, ID_JOIN_PAGE,
		ID_CLOUD, ID_DIRECT, ID_KEYBOARD,
		ID_NAME_FIELD, ID_CODE_FIELD, ID_JOIN, ID_LEAVE,
		ID_LIST_UP, ID_LIST_DOWN, ID_REFRESH, ID_CAMPAIGN_CONTINUE, ID_CAMPAIGN_PREV, ID_CAMPAIGN_DELETE,
		ID_SAVE, ID_LOAD, ID_LOAD_PREV, ID_GAME_CAMPAIGNS, ID_COMPANION,
		ID_LEVEL = 100,     // + level index
		ID_ROW = 200,       // + campaign index
	};
	struct SHit { int id; float x, y, w, h; };
	std::vector<SHit> s_hits;       // where the widgets were drawn last frame
	float s_mouseX = -1.0f, s_mouseY = -1.0f;

	int s_focus = ID_NONE;          // the text field that gets the keys
	string s_nameField, s_codeField;
	std::vector<CoopSave::SCampaignInfo> s_campaigns;
	bool s_listing = false;
	int s_selected = -1;
	int s_scroll = 0;
	int s_deleteArmed = -1;         // Delete clicked once for this campaign
	string s_note;                  // this menu's own last message
	float s_noteUntil = 0.0f;

	IUIDraw* s_pUI = 0;
	IFFont* s_pFont = 0;
	int s_white = -1;

	// the HUD line: shown a while after the code comes and when friends come or go
	int s_hudCode = 0, s_hudFriends = -1;
	float s_hudLeft = 0.0f;         // seconds it is still shown (counted while the HUD is drawn)

	float Now() { return gEnv->pTimer->GetAsyncCurTime(); }

	void Note(const char* text)
	{
		s_note = text;
		s_noteUntil = Now() + 8.0f;
	}

	// ---- drawing: the menu is laid out on an 800x600 page that keeps its
	// shape on any screen (scaled by the smaller of width/800 and height/600,
	// centered). IUIDraw's own 800x600 space stretches with the screen, and
	// each primitive goes its own way (measured at 1280x720): an image keeps
	// its aspect around its stretched center, text is scaled by height/600
	// from the top left. So every call is given what lands on the page.
	struct SView
	{
		float sx = 1, sy = 1;       // IUIDraw's scale: screen width/800, height/600
		float s = 1;                // the page's scale
		float ox = 0, oy = 0;       // the page's top left on the screen (pixels)
	} s_view;

	void UpdateView()
	{
		const float w = (float)(std::max)(1, gEnv->pRenderer->GetWidth()), h = (float)(std::max)(1, gEnv->pRenderer->GetHeight());
		s_view.sx = w / W;
		s_view.sy = h / H;
		s_view.s = (std::min)(s_view.sx, s_view.sy);
		s_view.ox = (w - W * s_view.s) * 0.5f;
		s_view.oy = (h - H * s_view.s) * 0.5f;
	}

	void Rect(float x, float y, float w, float h, float r, float g, float b, float a)
	{
		// an image (X, Y, w, h) lands at left sx*X + (sx - sy)*w/2, top sy*Y,
		// size sy*w x sy*h: scaled by the height, centered where the stretched
		// one would be (measured at 1280x720 and 1000x800)
		const float iw = w * s_view.s / s_view.sy, ih = h * s_view.s / s_view.sy;
		const float ix = (s_view.ox + s_view.s * x - (s_view.sx - s_view.sy) * iw * 0.5f) / s_view.sx;
		const float iy = (s_view.oy + s_view.s * y) / s_view.sy;
		if (s_white > 0)
			s_pUI->DrawImage(s_white, ix, iy, iw, ih, 0.0f, r, g, b, a);
		else
			s_pUI->DrawQuad(ix, iy, iw, ih, s_pUI->GetColorARGB((uint8)(a * 255), (uint8)(r * 255), (uint8)(g * 255), (uint8)(b * 255)),
				0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, false);
	}

	// the whole screen, not just the page
	void FillScreen(float r, float g, float b, float a)
	{
		Rect(-s_view.ox / s_view.s, -s_view.oy / s_view.s, W * s_view.sx / s_view.s, H * s_view.sy / s_view.s, r, g, b, a);
	}

	void Frame(float x, float y, float w, float h, float r, float g, float b, float a)
	{
		Rect(x, y, w, 1, r, g, b, a);
		Rect(x, y + h - 1, w, 1, r, g, b, a);
		Rect(x, y, 1, h, r, g, b, a);
		Rect(x + w - 1, y, 1, h, r, g, b, a);
	}

	enum EAlign { eA_Left, eA_Center, eA_Right };
	void Text(float x, float y, float size, const char* text, float r = 0.86f, float g = 0.95f, float b = 0.86f, float a = 1.0f, EAlign align = eA_Left)
	{
		const EUIDRAWHORIZONTAL h = align == eA_Center ? UIDRAWHORIZONTAL_CENTER : align == eA_Right ? UIDRAWHORIZONTAL_RIGHT : UIDRAWHORIZONTAL_LEFT;
		// text: position and size times height/600
		const float k = 1.0f / s_view.sy;
		s_pUI->DrawText(s_pFont, (s_view.ox + s_view.s * x) * k, (s_view.oy + s_view.s * y) * k, size * s_view.s * k, size * s_view.s * k,
			text, a, r, g, b, UIDRAWHORIZONTAL_LEFT, UIDRAWVERTICAL_TOP, h, UIDRAWVERTICAL_TOP);
	}

	// a long text over several lines of at most maxChars, broken at spaces
	void WrappedText(float x, float y, float size, const char* text, int maxChars, float r, float g, float b)
	{
		string rest = text;
		for (float dy = 0; !rest.empty(); dy += size + 5)
		{
			string::size_type cut = rest.length();
			if ((int)cut > maxChars)
			{
				cut = rest.rfind(' ', maxChars);
				if (cut == string::npos || cut == 0)
					cut = maxChars;
			}
			Text(x, y + dy, size, rest.substr(0, cut).c_str(), r, g, b);
			rest = rest.substr(cut);
			while (!rest.empty() && rest[0] == ' ')
				rest = rest.substr(1);
		}
	}

	bool Hover(float x, float y, float w, float h)
	{
		return s_mouseX >= x && s_mouseX < x + w && s_mouseY >= y && s_mouseY < y + h;
	}

	void Hit(int id, float x, float y, float w, float h)
	{
		SHit hit = { id, x, y, w, h };
		s_hits.push_back(hit);
	}

	// a button; subtitle: a second, smaller line
	void Button(int id, float x, float y, float w, float h, const char* label, bool enabled = true, const char* subtitle = 0, bool selected = false)
	{
		const bool hover = enabled && Hover(x, y, w, h);
		if (!enabled)
			Rect(x, y, w, h, 0.08f, 0.10f, 0.08f, 0.85f);
		else if (selected)
			Rect(x, y, w, h, 0.18f, 0.40f, 0.20f, 0.95f);
		else
			Rect(x, y, w, h, hover ? 0.16f : 0.09f, hover ? 0.34f : 0.20f, hover ? 0.18f : 0.10f, 0.92f);
		Frame(x, y, w, h, 0.35f, hover || selected ? 0.95f : 0.65f, 0.40f, enabled ? 1.0f : 0.4f);
		const float c = enabled ? 1.0f : 0.45f;
		if (subtitle && subtitle[0])
		{
			Text(x + w * 0.5f, y + 5, 17, label, 0.9f * c, c, 0.9f * c, 1.0f, eA_Center);
			Text(x + w * 0.5f, y + h - 18, 12, subtitle, 0.65f * c, 0.85f * c, 0.65f * c, 1.0f, eA_Center);
		}
		else
			Text(x + w * 0.5f, y + h * 0.5f - 9, 17, label, 0.9f * c, c, 0.9f * c, 1.0f, eA_Center);
		if (enabled)
			Hit(id, x, y, w, h);
	}

	void Toggle(int id, float x, float y, const char* label, bool on)
	{
		const bool hover = Hover(x, y, 300, 20);
		Rect(x, y + 2, 16, 16, 0.05f, 0.12f, 0.06f, 0.95f);
		Frame(x, y + 2, 16, 16, 0.35f, hover ? 0.95f : 0.65f, 0.40f, 1.0f);
		if (on)
			Rect(x + 4, y + 6, 8, 8, 0.45f, 0.95f, 0.50f, 1.0f);
		Text(x + 26, y + 1, 15, label, 0.8f, hover ? 1.0f : 0.9f, 0.8f);
		Hit(id, x, y, 300, 20);
	}

	void Field(int id, float x, float y, float w, const string& value, const char* placeholder)
	{
		const bool focused = s_focus == id;
		Rect(x, y, w, 30, 0.03f, 0.07f, 0.04f, 0.95f);
		Frame(x, y, w, 30, 0.35f, focused ? 1.0f : 0.6f, 0.40f, 1.0f);
		if (value.empty() && !focused)
			Text(x + 10, y + 6, 16, placeholder, 0.5f, 0.6f, 0.5f);
		else
		{
			string shown = value;
			if (focused && fmod(Now(), 1.0f) < 0.6f)
				shown += "_";
			Text(x + 10, y + 6, 16, shown.c_str(), 0.95f, 1.0f, 0.95f);
		}
		Hit(id, x, y, w, 30);
	}

	bool CVarOn(const char* name)
	{
		ICVar* p = gEnv->pConsole->GetCVar(name);
		return p && p->GetIVal() != 0;
	}

	void FlipCVar(const char* name)
	{
		if (ICVar* p = gEnv->pConsole->GetCVar(name))
			p->Set(p->GetIVal() ? 0 : 1);
	}

	string When(unsigned int stamp)
	{
		char when[32] = "";
		const time_t t = (time_t)stamp;
		if (t)
			strftime(when, sizeof(when), "%d.%m.%Y %H:%M", localtime(&t));
		return when;
	}

	string Describe(const CoopSave::SCampaignInfo& c)
	{
		string text;
		text.Format("%s (%s), checkpoint %s, %s", CoopSave::LevelTitle(c.level.c_str()), c.level.c_str(), c.checkpoint.c_str(), When(c.stamp).c_str());
		return text;
	}

	const char* Where(const CoopSave::SCampaignInfo& c)
	{
		if (c.local && c.cloud)
			return c.cloudNewer ? "newer in the cloud" : "this PC + cloud";
		if (c.local)
			return "this PC";
		return c.own ? "cloud" : "cloud (a friend's game)";
	}

	void RequestList()
	{
		s_listing = true;
		s_selected = -1;
		s_deleteArmed = -1;
		CoopSave::RequestCampaigns();
	}

	void UpdateList()
	{
		if (s_listing && CoopSave::CampaignsReady(s_campaigns))
		{
			s_listing = false;
			if (s_selected >= (int)s_campaigns.size())
				s_selected = -1;
		}
	}

	void Open(EPage page)
	{
		s_page = page;
		s_focus = ID_NONE;
		s_deleteArmed = -1;
		if (page == eP_Main || page == eP_Campaigns)
			RequestList();
		if (page == eP_Join)
		{
			const int last = CoopRelay::LastJoinCode();
			s_codeField = last > 0 ? string().Format("%d", last) : string();
			s_focus = ID_CODE_FIELD;
		}
		if (page == eP_New)
			s_focus = ID_NAME_FIELD;
	}

	void Close()
	{
		s_page = eP_Closed;
		s_focus = ID_NONE;
	}

	// ---- what the player gets back: the last message of this menu, the
	// save system or the relay
	void StatusLine(float x, float y)
	{
		const char* text = 0;
		if (!s_note.empty() && Now() < s_noteUntil)
			text = s_note.c_str();
		else if (CoopSave::MenuStatus()[0])
			text = CoopSave::MenuStatus();
		if (text)
			Text(x, y, 14, text, 0.95f, 0.85f, 0.45f);
	}

	// ---- pages
	const float PX = 150, PY = 60, PW = 500, PH = 480;

	void DrawSettings(float y)
	{
		Toggle(ID_CLOUD, PX + 30, y, "Keep my checkpoints in the cloud", CVarOn("coop_cloud"));
		Toggle(ID_DIRECT, PX + 30, y + 24, "Connect directly when possible (lower latency)", CVarOn("coop_direct"));
		// no friend at hand: a second player played by the game, or by an AI
		// agent (MCP), in a light copy of the game in the background
		Toggle(ID_COMPANION, PX + 30, y + 48, "AI companion (a second player, bot or AI agent)", CVarOn("coop_companion"));
	}

	void DrawMain()
	{
		UpdateList();
		const float x = PX + 30, w = PW - 60;
		float y = PY + 70;
		string sub = s_listing ? string("looking for your campaigns...")
			: s_campaigns.empty() ? string("no campaign yet") : "\"" + s_campaigns[0].name + "\" - " + Describe(s_campaigns[0]);
		Button(ID_CONTINUE, x, y, w, 50, "Continue", !s_listing && !s_campaigns.empty() && !CoopSave::IsBusy(), sub.c_str());
		y += 62;
		Button(ID_NEW, x, y, w, 40, "New campaign");
		y += 52;
		Button(ID_CAMPAIGNS, x, y, w, 40, "All campaigns");
		y += 52;
		Button(ID_JOIN_PAGE, x, y, w, 40, "Join a friend");
		y += 64;
		DrawSettings(y);
		StatusLine(x, PY + PH - 62);
		if (s_inGame)
			Button(ID_BACK, PX + PW - 130, PY + PH - 44, 110, 30, "Back");
	}

	void DrawNew()
	{
		const float x = PX + 30;
		Text(x, PY + 60, 16, "Campaign name (optional):");
		Field(ID_NAME_FIELD, x, PY + 84, PW - 60, s_nameField, "Campaign N");
		Text(x, PY + 128, 16, "Start on the level:");
		for (int i = 0; i < CoopSave::LevelCount(); ++i)
		{
			const int col = i < 6 ? 0 : 1, row = i < 6 ? i : i - 6;
			string label;
			label.Format("%d. %s", i + 1, CoopSave::LevelTitle(CoopSave::LevelName(i)));
			Button(ID_LEVEL + i, x + col * 225, PY + 152 + row * 38, 215, 32, label.c_str(), !CoopSave::IsBusy());
		}
		if (CoopRelay::IsHosting() && CoopRelay::FriendsConnected() > 0)
			Text(x, PY + PH - 84, 13, "Your friends stay with you: their games join the new one by themselves.", 0.7f, 0.85f, 0.7f);
		StatusLine(x, PY + PH - 62);
		Button(ID_BACK, PX + PW - 130, PY + PH - 44, 110, 30, "Back");
	}

	void DrawCampaigns()
	{
		UpdateList();
		const float x = PX + 30, w = PW - 60;
		const int visible = 6;
		if (s_listing)
			Text(x, PY + 70, 15, "Looking for your campaigns (this PC and the cloud)...");
		else if (s_campaigns.empty())
			Text(x, PY + 70, 15, "No campaign yet: start one with New campaign.");
		if (s_scroll > (int)s_campaigns.size() - visible)
			s_scroll = (std::max)(0, (int)s_campaigns.size() - visible);
		for (int i = 0; i < visible && s_scroll + i < (int)s_campaigns.size(); ++i)
		{
			const int index = s_scroll + i;
			const CoopSave::SCampaignInfo& c = s_campaigns[index];
			const float ry = PY + 60 + i * 44;
			const bool selected = index == s_selected;
			const bool hover = Hover(x, ry, w - 30, 40);
			Rect(x, ry, w - 30, 40, selected ? 0.18f : hover ? 0.12f : 0.06f, selected ? 0.40f : hover ? 0.26f : 0.14f, selected ? 0.20f : hover ? 0.13f : 0.07f, 0.92f);
			Frame(x, ry, w - 30, 40, 0.35f, selected || hover ? 0.95f : 0.55f, 0.40f, 1.0f);
			Text(x + 8, ry + 3, 16, c.name.c_str());
			Text(x + w - 38, ry + 4, 12, Where(c), 0.65f, 0.85f, 0.65f, 1.0f, eA_Right);
			Text(x + 8, ry + 22, 12, Describe(c).c_str(), 0.65f, 0.85f, 0.65f);
			Hit(ID_ROW + index, x, ry, w - 30, 40);
		}
		if ((int)s_campaigns.size() > visible)
		{
			Button(ID_LIST_UP, x + w - 26, PY + 60, 26, 40, "^", s_scroll > 0);
			Button(ID_LIST_DOWN, x + w - 26, PY + 60 + (visible - 1) * 44, 26, 40, "v", s_scroll + visible < (int)s_campaigns.size());
		}
		const bool picked = s_selected >= 0 && s_selected < (int)s_campaigns.size() && !CoopSave::IsBusy();
		const float by = PY + 60 + visible * 44 + 6;
		Button(ID_CAMPAIGN_CONTINUE, x, by, 140, 34, "Continue", picked);
		Button(ID_CAMPAIGN_PREV, x + 150, by, 150, 34, "Checkpoint before", picked);
		Button(ID_CAMPAIGN_DELETE, x + 310, by, 130, 34, s_deleteArmed == s_selected && picked ? "Sure? Delete" : "Delete", picked);
		StatusLine(x, PY + PH - 62);
		Button(ID_REFRESH, x, PY + PH - 44, 110, 30, "Refresh", !s_listing);
		Button(ID_BACK, PX + PW - 130, PY + PH - 44, 110, 30, "Back");
	}

	void DrawJoin()
	{
		const float x = PX + 30;
		Text(x, PY + 60, 16, "Your friend's code:");
		Field(ID_CODE_FIELD, x, PY + 84, 200, s_codeField, "123456");
		Button(ID_JOIN, x + 215, PY + 84, 120, 30, "Join", !s_codeField.empty());
		Text(x, PY + 130, 13, "The host sees his code on the screen and in his co-op menu.", 0.7f, 0.85f, 0.7f);
		Text(x, PY + 148, 13, "It is always the same code: next time it is filled in already.", 0.7f, 0.85f, 0.7f);
		string error;
		int code = 0;
		const CoopRelay::EJoinState state = CoopRelay::GetJoinState(&error, &code);
		string text;
		switch (state)
		{
		case CoopRelay::eJS_Connecting: text.Format("Connecting to game %d...", code); break;
		case CoopRelay::eJS_Loading: text.Format("Game %d: loading the level...", code); break;
		case CoopRelay::eJS_InGame: text.Format("In game %d", code); break;
		case CoopRelay::eJS_WaitingForHost: text.Format("Game %d: the host is loading, you join again by yourself", code); break;
		case CoopRelay::eJS_Failed: text.Format("Cannot join: %s", error.c_str()); break;
		default: break;
		}
		if (!text.empty())
			WrappedText(x, PY + 190, 15, text.c_str(), 52, 0.95f, 0.85f, 0.45f);
		Button(ID_BACK, PX + PW - 130, PY + PH - 44, 110, 30, "Back");
	}

	void DrawGame()
	{
		const float x = PX + 30, w = PW - 60;
		float y = PY + 64;
		if (gEnv->bServer)
		{
			const int code = CoopRelay::HostCode();
			string text;
			if (code)
				text.Format("Co-op code: %d", code);
			else
				text = CoopRelay::IsHosting() ? "Co-op code: waiting for the relay..." : "Co-op code: not online yet";
			Text(x, y, 26, text.c_str(), 0.95f, 1.0f, 0.95f);
			y += 36;
			Text(x, y, 13, "Friends: Co-op game > Join a friend, and this code. It never changes.", 0.7f, 0.85f, 0.7f);
			y += 26;
			int direct = 0;
			const int friends = CoopRelay::FriendsConnected(&direct);
			text.Format("Friends in the game: %d%s", friends, friends ? string().Format(" (%d directly)", direct).c_str() : "");
			Text(x, y, 15, text.c_str());
			y += 22;
			if (CVarOn("coop_companion"))
			{
				const int state = CoopAgent::CompanionState();
				Text(x, y, 13, state == 2 ? "AI companion: in the game (AI agents: CrysisCoop.exe -coop_mcp)"
					: state == 1 ? "AI companion: joining..." : state == 3 ? "AI companion: its game did not start (companion.log). Turn it off and on to try again"
					: "AI companion: starting...", 0.7f, 0.85f, 0.95f);
				y += 18;
			}
			const char* campaign = CoopSave::CurrentCampaign();
			text.Format("Campaign: %s", campaign[0] ? campaign : "(saved at the first checkpoint)");
			Text(x, y, 15, text.c_str());
			y += 34;
			const bool busy = CoopSave::IsBusy();
			Button(ID_SAVE, x, y, w, 36, "Save now", !busy);
			y += 44;
			Button(ID_LOAD, x, y, w * 0.5f - 5, 36, "Back to the last checkpoint", !busy && campaign[0]);
			Button(ID_LOAD_PREV, x + w * 0.5f + 5, y, w * 0.5f - 5, 36, "The checkpoint before", !busy && campaign[0]);
			y += 44;
			Button(ID_GAME_CAMPAIGNS, x, y, w, 36, "Other campaigns / new campaign");
			y += 52;
		}
		else
		{
			string error;
			int code = 0;
			const CoopRelay::EJoinState state = CoopRelay::GetJoinState(&error, &code);
			string text;
			if (state == CoopRelay::eJS_None)
				text = "Playing on a server (not through the relay)";
			else
				text.Format("In your friend's game %d", code);
			Text(x, y, 22, text.c_str(), 0.95f, 1.0f, 0.95f);
			y += 34;
			bool direct = false;
			const int ping = CoopRelay::PingMs(&direct);
			if (ping >= 0)
			{
				text.Format("Connection: %d ms %s", ping, direct ? "(directly)" : "(through the relay)");
				Text(x, y, 15, text.c_str());
			}
			y += 26;
			Text(x, y, 13, "The host saves the game. If he loads a checkpoint, you join again by yourself.", 0.7f, 0.85f, 0.7f);
			y += 40;
			Button(ID_LEAVE, x, y, w, 36, "Leave the game");
			y += 60;
		}
		DrawSettings(PY + PH - 134);
		StatusLine(x, PY + PH - 62);
	}

	void DrawPanel()
	{
		// the rest of the screen is dimmed: the panel has the input
		FillScreen(0.0f, 0.0f, 0.0f, 0.55f);
		Rect(PX, PY, PW, PH, 0.02f, 0.05f, 0.03f, 0.94f);
		Frame(PX, PY, PW, PH, 0.35f, 0.85f, 0.40f, 1.0f);
		Rect(PX, PY, PW, 40, 0.07f, 0.18f, 0.08f, 1.0f);
		const char* titles[] = { "", "CRYSIS CO-OP", "NEW CAMPAIGN", "CAMPAIGNS", "JOIN A FRIEND", "CRYSIS CO-OP" };
		Text(PX + 18, PY + 9, 20, titles[s_page], 0.75f, 1.0f, 0.75f);
		Button(ID_CLOSE, PX + PW - 36, PY + 6, 28, 28, "X");
		switch (s_page)
		{
		case eP_Main: DrawMain(); break;
		case eP_New: DrawNew(); break;
		case eP_Campaigns: DrawCampaigns(); break;
		case eP_Join: DrawJoin(); break;
		case eP_Game: DrawGame(); break;
		default: break;
		}
	}

	bool Begin()
	{
		s_pUI = g_pGame && g_pGame->GetIGameFramework() ? g_pGame->GetIGameFramework()->GetIUIDraw() : 0;
		if (!s_pFont)
			s_pFont = gEnv->pCryFont ? gEnv->pCryFont->GetFont("default") : 0;
		if (!s_pUI || !s_pFont)
			return false;
		if (s_white < 0)
		{
			s_white = s_pUI->CreateTexture("Textures/Defaults/White.dds");
			if (s_white <= 0)
				s_white = 0;
		}
		return true;
	}

	// ---- clicks
	void Perform(int id)
	{
		s_focus = ID_NONE;
		if (id != ID_CAMPAIGN_DELETE && (id < ID_ROW || id >= ID_ROW + 1000))
			s_deleteArmed = -1;
		if (id == ID_CLOSE)
			Close();
		else if (id == ID_BACK)
			// in the game the co-op page is the first one, the main page below it
			Open(s_page == eP_Main && s_inGame ? eP_Game : eP_Main);
		else if (id == ID_CONTINUE && !s_campaigns.empty())
		{
			CoopSave::ContinueCampaign(s_campaigns[0].id.c_str(), false);
			Note("Continuing...");
		}
		else if (id == ID_NEW)
		{
			s_nameField.clear();
			Open(eP_New);
		}
		else if (id == ID_CAMPAIGNS || id == ID_GAME_CAMPAIGNS)
			Open(id == ID_GAME_CAMPAIGNS ? eP_Main : eP_Campaigns);
		else if (id == ID_JOIN_PAGE)
			Open(eP_Join);
		else if (id == ID_CLOUD)
			FlipCVar("coop_cloud");
		else if (id == ID_DIRECT)
			FlipCVar("coop_direct");
		else if (id == ID_COMPANION)
		{
			FlipCVar("coop_companion");
			Note(CVarOn("coop_companion") ? "AI companion on: it joins your game in a moment" : "AI companion off");
		}
		else if (id == ID_NAME_FIELD || id == ID_CODE_FIELD)
			s_focus = id;
		else if (id == ID_JOIN)
		{
			if (CoopRelay::Join(atoi(s_codeField.c_str())))
				Note("Joining...");
		}
		else if (id == ID_LEAVE)
		{
			CoopRelay::Leave();
			Close();
		}
		else if (id == ID_LIST_UP)
			s_scroll = (std::max)(0, s_scroll - 1);
		else if (id == ID_LIST_DOWN)
			++s_scroll;
		else if (id == ID_REFRESH)
			RequestList();
		else if ((id == ID_CAMPAIGN_CONTINUE || id == ID_CAMPAIGN_PREV) && s_selected >= 0 && s_selected < (int)s_campaigns.size())
		{
			CoopSave::ContinueCampaign(s_campaigns[s_selected].id.c_str(), id == ID_CAMPAIGN_PREV);
			Note(id == ID_CAMPAIGN_PREV ? "Continuing from the checkpoint before..." : "Continuing...");
		}
		else if (id == ID_CAMPAIGN_DELETE && s_selected >= 0 && s_selected < (int)s_campaigns.size())
		{
			if (s_deleteArmed != s_selected)
				s_deleteArmed = s_selected;
			else
			{
				CoopSave::DeleteCampaign(s_campaigns[s_selected].id.c_str());
				s_campaigns.erase(s_campaigns.begin() + s_selected);
				s_selected = -1;
				s_deleteArmed = -1;
			}
		}
		else if (id == ID_SAVE)
			CoopSave::SaveNow();
		else if (id == ID_LOAD || id == ID_LOAD_PREV)
			CoopSave::LoadCheckpoint(id == ID_LOAD_PREV);
		else if (id >= ID_LEVEL && id < ID_LEVEL + CoopSave::LevelCount())
		{
			CoopSave::NewCampaign(CoopSave::LevelName(id - ID_LEVEL), s_nameField.c_str());
			Note("Starting...");
			Close();
		}
		else if (id >= ID_ROW && id < ID_ROW + (int)s_campaigns.size())
		{
			s_selected = id - ID_ROW;
			if (s_deleteArmed != s_selected)
				s_deleteArmed = -1;
		}
	}

	bool Click(float vx, float vy)
	{
		s_mouseX = vx;
		s_mouseY = vy;
		// the last widget drawn is the one on top
		for (int i = (int)s_hits.size() - 1; i >= 0; --i)
		{
			const SHit& h = s_hits[i];
			if (vx >= h.x && vx < h.x + h.w && vy >= h.y && vy < h.y + h.h)
			{
				Perform(h.id);
				return true;
			}
		}
		s_focus = ID_NONE;
		return s_page != eP_Closed;
	}

	void ToVirtual(int x, int y, float& vx, float& vy)
	{
		// window pixels to the page (see UpdateView)
		UpdateView();
		vx = (x - s_view.ox) / s_view.s;
		vy = (y - s_view.oy) / s_view.s;
	}

	// the menu texts the mod changes (Network > Quick game is the co-op game).
	// A label that is loaded already keeps its first text, so the mod's
	// spreadsheet is loaded first and the game's own ones again after it
	// (the game's files are not touched).
	void OverrideMenuTexts()
	{
		ILocalizationManager* pLoc = gEnv->pSystem->GetLocalizationManager();
		if (!pLoc)
			return;
		const string language = pLoc->GetLanguage() ? pLoc->GetLanguage() : "";
		const bool russian = !stricmp(language.c_str(), "russian");
		std::vector<string> tables;
		_finddata_t fd;
		const intptr_t h = gEnv->pCryPak->FindFirst("Languages/*.xml", &fd);
		if (h != -1)
		{
			do
			{
				if (strnicmp(fd.name, "coop_", 5))
					tables.push_back(string("Languages/") + fd.name);
			}
			while (gEnv->pCryPak->FindNext(h, &fd) >= 0);
			gEnv->pCryPak->FindClose(h);
		}
		const int before = pLoc->GetLocalizedStringCount();
		if (tables.empty() || before <= 0)
			return;
		pLoc->FreeData();
		pLoc->SetLanguage(language.c_str());     // FreeData forgets it
		const bool loaded = pLoc->LoadExcelXmlSpreadsheet(russian ? "Languages/coop_ui_text_russian.xml" : "Languages/coop_ui_text.xml");
		for (size_t i = 0; i < tables.size(); ++i)
			pLoc->LoadExcelXmlSpreadsheet(tables[i].c_str());
		CryLogAlways("[CoopMenu] localization reloaded after the mod's texts: %d tables, %d texts (%d before)",
			(int)tables.size(), pLoc->GetLocalizedStringCount(), before);
		wstring label;
		pLoc->LocalizeLabel("@ui_menu_QUICKGAME", label);
		string ascii;
		for (size_t i = 0; i < label.length(); ++i)
			ascii += label[i] < 128 ? (char)label[i] : '?';
		CryLogAlways("[CoopMenu] menu texts (%s) %s: Quick game is now \"%s\" (first letter U+%04X)", language.c_str(),
			loaded ? "loaded" : "NOT loaded", ascii.c_str(), label.empty() ? 0 : (unsigned)label[0]);
	}

	// coop_ui open | close | click <x> <y> (800x600) | mouse <x> <y> (pixels) | type <text> | ingame [0|1] | hits
	//         flashdown|flashup <x> <y> (pixels: the Flash menu, as the mouse) | fs <command> [<args>]
	void CmdUI(IConsoleCmdArgs* pArgs)
	{
		const char* what = pArgs->GetArgCount() > 1 ? pArgs->GetArg(1) : "";
		if (!stricmp(what, "open"))
			Open(s_inGame ? eP_Game : eP_Main);
		else if (!stricmp(what, "close"))
			Close();
		else if (!stricmp(what, "click") && pArgs->GetArgCount() > 3)
			CryLogAlways("[CoopMenu] click %s,%s: %s", pArgs->GetArg(2), pArgs->GetArg(3),
				Click((float)atof(pArgs->GetArg(2)), (float)atof(pArgs->GetArg(3))) ? "used" : "not used");
		else if (!stricmp(what, "mouse") && pArgs->GetArgCount() > 3)
			CryLogAlways("[CoopMenu] mouse %s,%s: %s", pArgs->GetArg(2), pArgs->GetArg(3),
				CoopMenu::OnMouse(atoi(pArgs->GetArg(2)), atoi(pArgs->GetArg(3)), HARDWAREMOUSEEVENT_LBUTTONDOWN) ? "used" : "not used");
		else if ((!stricmp(what, "type") || !stricmp(what, "set")) && pArgs->GetArgCount() > 2)
		{
			string& field = s_focus == ID_CODE_FIELD ? s_codeField : s_nameField;
			if (!stricmp(what, "set"))
				field.clear();
			for (int i = 2; i < pArgs->GetArgCount(); ++i)
			{
				if (i > 2)
					field += " ";
				field += pArgs->GetArg(i);
			}
		}
		else if (!stricmp(what, "ingame"))
		{
			if (CFlashMenuObject* pMenu = g_pGame->GetMenu())
				pMenu->ShowInGameMenu(pArgs->GetArgCount() < 3 || atoi(pArgs->GetArg(2)) != 0);
		}
		else if ((!stricmp(what, "flashdown") || !stricmp(what, "flashup")) && pArgs->GetArgCount() > 3)
		{
			// the mouse on the Flash menu: down and up come in different frames
			if (CFlashMenuObject* pMenu = g_pGame->GetMenu())
			{
				const int x = atoi(pArgs->GetArg(2)), y = atoi(pArgs->GetArg(3));
				pMenu->OnHardwareMouseEvent(x, y, HARDWAREMOUSEEVENT_MOVE);
				pMenu->OnHardwareMouseEvent(x, y, !stricmp(what, "flashdown") ? HARDWAREMOUSEEVENT_LBUTTONDOWN : HARDWAREMOUSEEVENT_LBUTTONUP);
			}
		}
		else if (!stricmp(what, "fs") && pArgs->GetArgCount() > 2)
		{
			if (CFlashMenuObject* pMenu = g_pGame->GetMenu())
				pMenu->HandleFSCommand(pArgs->GetArg(2), pArgs->GetArgCount() > 3 ? pArgs->GetArg(3) : "");
		}
		else if (!stricmp(what, "hits"))
			for (size_t i = 0; i < s_hits.size(); ++i)
				CryLogAlways("[CoopMenu] widget %d at %.0f,%.0f %.0fx%.0f", s_hits[i].id, s_hits[i].x, s_hits[i].y, s_hits[i].w, s_hits[i].h);
		CryLogAlways("[CoopMenu] page %d, focus %d, %d widgets", (int)s_page, s_focus, (int)s_hits.size());
	}
}

void CoopMenu::Init()
{
	OverrideMenuTexts();
	gEnv->pConsole->AddCommand("coop_ui", CmdUI, 0,
		"Crysis Coop testing: the co-op menu: open | close | click <x> <y> (800x600) | mouse <x> <y> (pixels) | type <text> | ingame [0|1] | hits");
	gEnv->pConsole->RegisterInt("coop_hud_code", 1, VF_DUMPTODISK,
		"Crysis Coop: 1 = the host's HUD shows his co-op code for a while after the game starts and when friends come or go")
		->SetFlags(VF_DUMPTODISK | VF_NOT_NET_SYNCED);
}

void CoopMenu::RenderMenu(bool inGame)
{
	if (inGame != s_inGame)
	{
		// the other menu: the panel starts over
		s_inGame = inGame;
		if (s_page != eP_Closed)
			Open(inGame ? eP_Game : eP_Main);
	}
	// the panel is opened from Network > Co-op game: closed, nothing is drawn
	if (s_page == eP_Closed || !Begin())
		return;
	s_drawnFrame = gEnv->pRenderer->GetFrameID(false);
	s_hits.clear();
	UpdateView();
	s_pUI->PreRender();
	// IUIDraw's images are depth tested against the game's scene: over the
	// in-game menu, a character near the camera cut through the panel (the
	// text, which is not depth tested, stayed on top). Nothing of the scene
	// is drawn after the menu, so its depth can go
	gEnv->pRenderer->ClearBuffer(FRT_CLEAR_DEPTH | FRT_CLEAR_IMMEDIATE, nullptr);
	DrawPanel();
	s_pUI->PostRender();
}

void CoopMenu::RenderHud(IUIDraw* pUIDraw, IFFont* pFont)
{
	ICVar* pShow = gEnv->pConsole->GetCVar("coop_hud_code");
	if (!pUIDraw || !pFont || (pShow && !pShow->GetIVal()) || !gEnv->bServer || !CoopAI::IsCoopSession())
		return;
	const int code = CoopRelay::HostCode();
	const int friends = CoopRelay::FriendsConnected();
	if (code != s_hudCode || friends != s_hudFriends)
	{
		// 45 s of HUD after the code comes, a few seconds when friends come or go
		s_hudLeft = code != s_hudCode ? 45.0f : (std::max)(s_hudLeft, 8.0f);
		s_hudCode = code;
		s_hudFriends = friends;
	}
	if (!code || s_hudLeft <= 0.0f)
		return;
	s_hudLeft -= gEnv->pTimer->GetFrameTime(ITimer::ETIMER_UI);
	string text;
	text.Format("CO-OP code %d  -  %d friend%s in the game  -  Esc > Co-op game for the co-op menu", code, friends, friends == 1 ? "" : "s");
	pUIDraw->DrawText(pFont, 12, 8, 15, 15, text.c_str(), 0.9f, 0.8f, 1.0f, 0.8f,
		UIDRAWHORIZONTAL_LEFT, UIDRAWVERTICAL_TOP, UIDRAWHORIZONTAL_LEFT, UIDRAWVERTICAL_TOP);
}

bool CoopMenu::OnMouse(int x, int y, int hardwareMouseEvent)
{
	if (!Drawn())
		return false;
	float vx, vy;
	ToVirtual(x, y, vx, vy);
	s_mouseX = vx;
	s_mouseY = vy;
	if (hardwareMouseEvent == HARDWAREMOUSEEVENT_LBUTTONDOWN)
		return Click(vx, vy);
	// the open panel keeps the rest (the Flash menu under it must not react)
	return s_page != eP_Closed && hardwareMouseEvent != HARDWAREMOUSEEVENT_MOVE;
}

bool CoopMenu::OnKey(const SInputEvent& event)
{
	if (s_page == eP_Closed || !Drawn() || event.deviceId != eDI_Keyboard)
		return false;
	if (event.state != eIS_Pressed)
		return event.keyId != eKI_Tilde;
	if (event.keyId == eKI_Tilde)
		return false;       // the console
	if (event.keyId == eKI_Escape)
	{
		if (s_focus != ID_NONE)
			s_focus = ID_NONE;
		else if (s_page == eP_Main || s_page == eP_Game)
			Close();
		else
			Open(s_inGame ? eP_Game : eP_Main);
		return true;
	}
	if (s_focus == ID_NONE)
		return true;
	string& field = s_focus == ID_CODE_FIELD ? s_codeField : s_nameField;
	if (event.keyId == eKI_Backspace)
	{
		if (!field.empty())
			field = field.substr(0, field.length() - 1);
		return true;
	}
	if (event.keyId == eKI_Enter || event.keyId == eKI_NP_Enter)
	{
		if (s_focus == ID_CODE_FIELD && !field.empty())
			Perform(ID_JOIN);
		else
			s_focus = ID_NONE;
		return true;
	}
	// the key's character: "a", "7", "np_7", "space"
	const char* name = event.keyName.c_str();
	char c = 0;
	if (!strnicmp(name, "np_", 3) && name[3] >= '0' && name[3] <= '9' && !name[4])
		c = name[3];
	else if (!stricmp(name, "space"))
		c = ' ';
	else if (name[0] && !name[1] && (unsigned char)name[0] >= 32 && (unsigned char)name[0] < 127)
	{
		c = name[0];
		if ((event.modifiers & eMM_Shift) && c >= 'a' && c <= 'z')
			c = (char)(c - 'a' + 'A');
	}
	if (s_focus == ID_CODE_FIELD)
	{
		if (c >= '0' && c <= '9' && field.length() < 9)
			field += c;
	}
	else if (c && field.length() < 32)
		field += c;
	return true;
}

bool CoopMenu::OnQuickGame()
{
	// the multiplayer servers are gone: Network > Co-op game opens this menu
	CryLogAlways("[CoopMenu] Network > Co-op game: the co-op menu");
	Open(s_inGame ? eP_Game : eP_Main);
	return true;
}

bool CoopMenu::IsOpen()
{
	return s_page != eP_Closed && Drawn();
}

void CoopMenu::OnJoinFailed()
{
	if (s_page != eP_Closed && s_page != eP_Join)
		Open(eP_Join);
}

void CoopMenu::OnMenuClosed()
{
	s_drawnFrame = -100;
	Close();
}
