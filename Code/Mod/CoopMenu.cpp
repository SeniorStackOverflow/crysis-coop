// Crysis Coop: the co-op menu (see CoopMenu.h).
//
// Drawn with IUIDraw over the Flash menu, in its virtual 800x600 screen; the
// mouse is taken before the Flash menu sees it. The widgets are laid out
// every frame and remember where they were drawn, a click goes to the one
// under the cursor.
#include "StdAfx.h"
#include "CoopText.h"
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

	// testing (coop_ui framelog): the screen size and the panel, frame by frame
	struct SFrameNote { int frame; int w, h; float pw, ph; };
	SFrameNote s_frameNotes[32];
	int s_frameNoteCount = 0;

	void UpdateView()
	{
		const float w = (float)(std::max)(1, gEnv->pRenderer->GetWidth()), h = (float)(std::max)(1, gEnv->pRenderer->GetHeight());
		s_view.sx = w / W;
		s_view.sy = h / H;
		s_view.s = (std::min)(s_view.sx, s_view.sy);
		s_view.ox = (w - W * s_view.s) * 0.5f;
		s_view.oy = (h - H * s_view.s) * 0.5f;
	}

	// ---- the panel: centered on the page, as large as what it shows. Each
	// frame the page is laid out three times: its width (the texts and the
	// buttons' labels on one line), its height (long texts broken into lines
	// at that width), then it is drawn (see DrawPanel)
	float PX = 150, PY = 60, PW = 500, PH = 480;
	const float MIN_W = 500, MAX_W = 784, MIN_H = 200, MAX_H = 592, MARGIN = 30, TITLE_H = 40;
	enum EPass { ePass_Width, ePass_Height, ePass_Draw };
	EPass s_pass = ePass_Draw;
	float s_right = 0.0f;           // the farthest right the page's texts reach
	float s_bottom = 0.0f;          // the lowest any of its things reaches
	float s_needInner = 0.0f;       // the inside width its rows of buttons need

	bool Drawing() { return s_pass == ePass_Draw; }
	float Inner() { return PW - 2 * MARGIN; }
	void Reach(float right, float bottom)
	{
		s_right = (std::max)(s_right, right);
		s_bottom = (std::max)(s_bottom, bottom);
	}
	void NeedInner(float w) { s_needInner = (std::max)(s_needInner, w); }

	// the width of a text on the page (in the game's language)
	float TextW(float size, const char* text)
	{
		// IUIDraw measures at a size in its own units; given the page's size
		// it gives the page's width (text is scaled alike in both directions)
		return CoopText::Width(s_pUI, size, text);
	}

	void Rect(float x, float y, float w, float h, float r, float g, float b, float a)
	{
		if (!Drawing())
		{
			s_bottom = (std::max)(s_bottom, y + h);
			return;
		}
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
	// a text in the game's language; measured: it widens the panel
	void Text(float x, float y, float size, const char* text, float r = 0.86f, float g = 0.95f, float b = 0.86f, float a = 1.0f, EAlign align = eA_Left, bool measured = true)
	{
		if (!Drawing())
		{
			const float w = measured ? TextW(size, text) : 0.0f;
			Reach(align == eA_Left ? x + w : align == eA_Center ? x + w * 0.5f : x, y + size + 4);
			return;
		}
		const int h = align == eA_Center ? UIDRAWHORIZONTAL_CENTER : align == eA_Right ? UIDRAWHORIZONTAL_RIGHT : UIDRAWHORIZONTAL_LEFT;
		// text: position and size times height/600
		const float k = 1.0f / s_view.sy;
		CoopText::Draw(s_pUI, (s_view.ox + s_view.s * x) * k, (s_view.oy + s_view.s * y) * k, size * s_view.s * k, size * s_view.s * k,
			text, a, r, g, b, UIDRAWHORIZONTAL_LEFT, UIDRAWVERTICAL_TOP, h, UIDRAWVERTICAL_TOP);
	}

	// a long text broken at spaces into lines that fit the panel; the height it takes
	float WrappedText(float x, float y, float size, const char* text, float r, float g, float b)
	{
		const float lineH = size + 5;
		const string all = CoopText::Tr(text);
		if (s_pass == ePass_Width)
		{
			// it does not widen the panel (but its longest word must fit)
			float word = 0.0f;
			for (size_t from = 0; from < all.length(); )
			{
				size_t to = all.find(' ', from);
				if (to == string::npos)
					to = all.length();
				word = (std::max)(word, TextW(size, all.substr(from, to - from).c_str()));
				from = to + 1;
			}
			Reach(x + word, y + lineH);
			return lineH;
		}
		const float limit = (std::max)(40.0f, PX + PW - MARGIN - x);
		float dy = 0.0f;
		string line;
		size_t from = 0;
		while (from <= all.length())
		{
			size_t to = all.find(' ', from);
			if (to == string::npos)
				to = all.length();
			const string word = all.substr(from, to - from);
			const string longer = line.empty() ? word : line + " " + word;
			if (!line.empty() && TextW(size, longer.c_str()) > limit)
			{
				Text(x, y + dy, size, line.c_str(), r, g, b, 1.0f, eA_Left, false);
				dy += lineH;
				line = word;
			}
			else
				line = longer;
			from = to + 1;
		}
		if (!line.empty())
		{
			Text(x, y + dy, size, line.c_str(), r, g, b, 1.0f, eA_Left, false);
			dy += lineH;
		}
		s_bottom = (std::max)(s_bottom, y + dy);
		return dy;
	}

	bool Hover(float x, float y, float w, float h)
	{
		return s_mouseX >= x && s_mouseX < x + w && s_mouseY >= y && s_mouseY < y + h;
	}

	void Hit(int id, float x, float y, float w, float h)
	{
		if (!Drawing())
			return;
		SHit hit = { id, x, y, w, h };
		s_hits.push_back(hit);
	}

	// a button's width for its label (at least minW)
	float FitW(const char* label, float minW)
	{
		return (std::max)(minW, TextW(17, label) + 26);
	}

	// a button; subtitle: a second, smaller line. A label wider than the
	// button widens the panel (the button grows with it)
	void Button(int id, float x, float y, float w, float h, const char* label, bool enabled = true, const char* subtitle = 0, bool selected = false)
	{
		if (!Drawing())
		{
			float need = TextW(17, label) + 26;
			if (subtitle && subtitle[0])
				need = (std::max)(need, TextW(12, subtitle) + 26);
			// the inside width at which the label fits, said every frame (said
			// only while it did not fit, the panel grew one frame and shrank
			// the next: it jumped between two sizes). A small fixed button,
			// like the list's arrows, does not grow
			if (w >= 60)
				NeedInner(Inner() * need / w);
			s_bottom = (std::max)(s_bottom, y + h);
			return;
		}
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
			Text(x + w * 0.5f, y + 5, 17, label, 0.9f * c, c, 0.9f * c, 1.0f, eA_Center, false);
			Text(x + w * 0.5f, y + h - 18, 12, subtitle, 0.65f * c, 0.85f * c, 0.65f * c, 1.0f, eA_Center, false);
		}
		else
			Text(x + w * 0.5f, y + h * 0.5f - 9, 17, label, 0.9f * c, c, 0.9f * c, 1.0f, eA_Center, false);
		if (enabled)
			Hit(id, x, y, w, h);
	}

	// the panel's Back button at the right of a row; the width it takes
	float BackButton(float y)
	{
		const float w = FitW("Back", 110);
		Button(ID_BACK, PX + PW - MARGIN - w, y, w, 30, "Back");
		return w;
	}

	// a check box and its label; the height it takes
	float Toggle(int id, float x, float y, const char* label, bool on)
	{
		const float w = 26 + TextW(15, label);
		const bool hover = Hover(x, y, w, 20);
		Rect(x, y + 2, 16, 16, 0.05f, 0.12f, 0.06f, 0.95f);
		Frame(x, y + 2, 16, 16, 0.35f, hover ? 0.95f : 0.65f, 0.40f, 1.0f);
		if (on)
			Rect(x + 4, y + 6, 8, 8, 0.45f, 0.95f, 0.50f, 1.0f);
		Text(x + 26, y + 1, 15, label, 0.8f, hover ? 1.0f : 0.9f, 0.8f);
		Hit(id, x, y, w, 20);
		return 24;
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
			Text(x + 10, y + 6, 16, shown.c_str(), 0.95f, 1.0f, 0.95f, 1.0f, eA_Left, false);
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
	// save system or the relay; the height it takes
	float StatusLine(float x, float y)
	{
		const char* text = 0;
		if (!s_note.empty() && Now() < s_noteUntil)
			text = s_note.c_str();
		else if (CoopSave::MenuStatus()[0])
			text = CoopSave::MenuStatus();
		if (!text)
			return 0.0f;
		return WrappedText(x, y, 14, text, 0.95f, 0.85f, 0.45f) + 6;
	}

	// ---- pages: laid out from the top down; the panel ends below the last row
	float DrawSettings(float y)
	{
		const float x = PX + MARGIN;
		y += Toggle(ID_CLOUD, x, y, "Keep my checkpoints in the cloud", CVarOn("coop_cloud"));
		y += Toggle(ID_DIRECT, x, y, "Connect directly when possible (lower latency)", CVarOn("coop_direct"));
		// no friend at hand: a second player played by the game, or by an AI
		// agent (MCP), in a light copy of the game in the background
		y += Toggle(ID_COMPANION, x, y, "AI companion (a second player, bot or AI agent)", CVarOn("coop_companion"));
		return y;
	}

	void DrawMain()
	{
		UpdateList();
		const float x = PX + MARGIN, w = Inner();
		float y = PY + TITLE_H + 30;
		string sub = s_listing ? string(CoopText::Tr("looking for your campaigns..."))
			: s_campaigns.empty() ? string(CoopText::Tr("no campaign yet")) : "\"" + s_campaigns[0].name + "\" - " + Describe(s_campaigns[0]);
		Button(ID_CONTINUE, x, y, w, 50, "Continue", !s_listing && !s_campaigns.empty() && !CoopSave::IsBusy(), sub.c_str());
		y += 62;
		Button(ID_NEW, x, y, w, 40, "New campaign");
		y += 52;
		Button(ID_CAMPAIGNS, x, y, w, 40, "All campaigns");
		y += 52;
		Button(ID_JOIN_PAGE, x, y, w, 40, "Join a friend");
		y += 64;
		y = DrawSettings(y) + 10;
		y += StatusLine(x, y);
		if (s_inGame)
			BackButton(y);
	}

	void DrawNew()
	{
		const float x = PX + MARGIN, w = Inner();
		float y = PY + TITLE_H + 20;
		Text(x, y, 16, "Campaign name (optional):");
		y += 24;
		Field(ID_NAME_FIELD, x, y, w, s_nameField, "Campaign N");
		y += 44;
		Text(x, y, 16, "Start on the level:");
		y += 24;
		const int rows = (CoopSave::LevelCount() + 1) / 2;
		const float colW = (w - 10) * 0.5f;
		for (int i = 0; i < CoopSave::LevelCount(); ++i)
		{
			const int col = i < rows ? 0 : 1, row = i < rows ? i : i - rows;
			string label;
			label.Format("%d. %s", i + 1, CoopText::Tr(CoopSave::LevelTitle(CoopSave::LevelName(i))));
			Button(ID_LEVEL + i, x + col * (colW + 10), y + row * 38, colW, 32, label.c_str(), !CoopSave::IsBusy());
		}
		y += rows * 38 + 10;
		if (CoopRelay::IsHosting() && CoopRelay::FriendsConnected() > 0)
			y += WrappedText(x, y, 13, "Your friends stay with you: their games join the new one by themselves.", 0.7f, 0.85f, 0.7f) + 6;
		y += StatusLine(x, y);
		BackButton(y);
	}

	void DrawCampaigns()
	{
		UpdateList();
		const float x = PX + MARGIN, w = Inner();
		const int visible = 6;
		float y = PY + TITLE_H + 20;
		if (s_listing)
			y += WrappedText(x, y, 15, "Looking for your campaigns (this PC and the cloud)...", 0.86f, 0.95f, 0.86f) + 6;
		else if (s_campaigns.empty())
			y += WrappedText(x, y, 15, "No campaign yet: start one with New campaign.", 0.86f, 0.95f, 0.86f) + 6;
		if (s_scroll > (int)s_campaigns.size() - visible)
			s_scroll = (std::max)(0, (int)s_campaigns.size() - visible);
		const float listY = y;
		for (int i = 0; i < visible && s_scroll + i < (int)s_campaigns.size(); ++i)
		{
			const int index = s_scroll + i;
			const CoopSave::SCampaignInfo& c = s_campaigns[index];
			const float ry = listY + i * 44;
			const bool selected = index == s_selected;
			const bool hover = Hover(x, ry, w - 30, 40);
			Rect(x, ry, w - 30, 40, selected ? 0.18f : hover ? 0.12f : 0.06f, selected ? 0.40f : hover ? 0.26f : 0.14f, selected ? 0.20f : hover ? 0.13f : 0.07f, 0.92f);
			Frame(x, ry, w - 30, 40, 0.35f, selected || hover ? 0.95f : 0.55f, 0.40f, 1.0f);
			// the name and where it is on one line: the row widens the panel
			if (!Drawing())
				NeedInner(TextW(16, c.name.c_str()) + TextW(12, Where(c)) + 30 + 30);
			Text(x + 8, ry + 3, 16, c.name.c_str(), 0.86f, 0.95f, 0.86f, 1.0f, eA_Left, false);
			Text(x + w - 38, ry + 4, 12, Where(c), 0.65f, 0.85f, 0.65f, 1.0f, eA_Right, false);
			if (!Drawing())
				NeedInner(TextW(12, Describe(c).c_str()) + 16 + 30);
			Text(x + 8, ry + 22, 12, Describe(c).c_str(), 0.65f, 0.85f, 0.65f, 1.0f, eA_Left, false);
			Hit(ID_ROW + index, x, ry, w - 30, 40);
		}
		const int shown = (std::min)(visible, (int)s_campaigns.size() - s_scroll);
		if ((int)s_campaigns.size() > visible)
		{
			Button(ID_LIST_UP, x + w - 26, listY, 26, 40, "^", s_scroll > 0);
			Button(ID_LIST_DOWN, x + w - 26, listY + (visible - 1) * 44, 26, 40, "v", s_scroll + visible < (int)s_campaigns.size());
		}
		y = listY + (std::max)(0, shown) * 44 + 6;
		// the campaign's buttons side by side, each as wide as its label
		const bool picked = s_selected >= 0 && s_selected < (int)s_campaigns.size() && !CoopSave::IsBusy();
		const char* del = s_deleteArmed == s_selected && picked ? "Sure? Delete" : "Delete";
		const float w1 = FitW("Continue", 140), w2 = FitW("Checkpoint before", 150), w3 = (std::max)(FitW("Sure? Delete", 130), FitW("Delete", 130));
		if (!Drawing())
			NeedInner(w1 + w2 + w3 + 20);
		Button(ID_CAMPAIGN_CONTINUE, x, y, w1, 34, "Continue", picked);
		Button(ID_CAMPAIGN_PREV, x + w1 + 10, y, w2, 34, "Checkpoint before", picked);
		Button(ID_CAMPAIGN_DELETE, x + w1 + w2 + 20, y, w3, 34, del, picked);
		y += 46;
		y += StatusLine(x, y);
		const float wr = FitW("Refresh", 110);
		Button(ID_REFRESH, x, y, wr, 30, "Refresh", !s_listing);
		const float wb = BackButton(y);
		if (!Drawing())
			NeedInner(wr + wb + 20);
	}

	void DrawJoin()
	{
		const float x = PX + MARGIN;
		float y = PY + TITLE_H + 20;
		Text(x, y, 16, "Your friend's code:");
		y += 24;
		Field(ID_CODE_FIELD, x, y, 200, s_codeField, "123456");
		const float wj = FitW("Join", 120);
		Button(ID_JOIN, x + 215, y, wj, 30, "Join", !s_codeField.empty());
		if (!Drawing())
			NeedInner(215 + wj);
		y += 46;
		y += WrappedText(x, y, 13, "The host sees his code on the screen and in his co-op menu.", 0.7f, 0.85f, 0.7f);
		y += WrappedText(x, y, 13, "It is always the same code: next time it is filled in already.", 0.7f, 0.85f, 0.7f);
		y += 16;
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
			y += WrappedText(x, y, 15, text.c_str(), 0.95f, 0.85f, 0.45f) + 8;
		BackButton(y);
	}

	void DrawGame()
	{
		const float x = PX + MARGIN, w = Inner();
		float y = PY + TITLE_H + 24;
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
			y += WrappedText(x, y, 13, "Friends: Co-op game > Join a friend, and this code. It never changes.", 0.7f, 0.85f, 0.7f) + 8;
			int direct = 0;
			const int friends = CoopRelay::FriendsConnected(&direct);
			if (friends)
				text.Format("Friends in the game: %d (%d directly)", friends, direct);
			else
				text.Format("Friends in the game: %d", friends);
			Text(x, y, 15, text.c_str());
			y += 22;
			if (CVarOn("coop_companion"))
			{
				const int state = CoopAgent::CompanionState();
				const int secs = (int)CoopAgent::CompanionSeconds();
				string status;
				bool failed = false;
				if (state == 2)
					status = "AI companion: in the game (AI agents: CrysisCoop.exe -coop_mcp)";
				else if (state == 1 && secs < CoopAgent::JOIN_TIMEOUT)
					status.Format("AI companion: joining (%d s, under a minute)", secs);
				else if (state == 1)
				{
					status.Format("AI companion CANNOT JOIN: not in the game after %d s (companion.log). Turn it off and on to try again", secs);
					failed = true;
				}
				else if (state == 3)
				{
					status = "AI companion CANNOT JOIN: its game did not start, 5 tries (companion.log). Turn it off and on to try again";
					failed = true;
				}
				else if (state == 4)
					status = "AI companion: started; joins when your game is ready (after the intro or a checkpoint load)";
				else if (state == 5)
					status.Format("AI companion: its game closed; starting it again (try %d of 5)", CoopAgent::CompanionTries() + 1);
				else
					status = "AI companion: starting...";
				y += WrappedText(x, y, 13, status.c_str(), failed ? 1.0f : 0.7f, failed ? 0.45f : 0.85f, failed ? 0.35f : 0.95f) + 4;
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
				text.Format(direct ? "Connection: %d ms (directly)" : "Connection: %d ms (through the relay)", ping);
				Text(x, y, 15, text.c_str());
			}
			y += 26;
			y += WrappedText(x, y, 13, "The host saves the game. If he loads a checkpoint, you join again by yourself.", 0.7f, 0.85f, 0.7f) + 12;
			Button(ID_LEAVE, x, y, w, 36, "Leave the game");
			y += 52;
		}
		y = DrawSettings(y) + 10;
		StatusLine(x, y);
	}

	void DrawPage()
	{
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

	void DrawPanel()
	{
		const char* titles[] = { "", "CRYSIS CO-OP", "NEW CAMPAIGN", "CAMPAIGNS", "JOIN A FRIEND", "CRYSIS CO-OP" };
		// the width: what the texts and the rows of buttons need on one line
		s_pass = ePass_Width;
		s_right = s_bottom = s_needInner = 0.0f;
		DrawPage();
		const float title = TextW(20, titles[s_page]) + 18 + 50;
		const float inner = (std::max)(s_right - (PX + MARGIN), s_needInner);
		PW = (std::min)(MAX_W, (std::max)((std::max)(MIN_W, title), inner + 2 * MARGIN));
		PX = (W - PW) * 0.5f;
		// the height: long texts broken into lines at that width
		s_pass = ePass_Height;
		s_right = s_bottom = 0.0f;
		DrawPage();
		PH = (std::min)(MAX_H, (std::max)(MIN_H, s_bottom - PY + 16));
		PY = (H - PH) * 0.5f;
		// drawn
		s_pass = ePass_Draw;
		// the rest of the screen is dimmed: the panel has the input
		FillScreen(0.0f, 0.0f, 0.0f, 0.55f);
		Rect(PX, PY, PW, PH, 0.02f, 0.05f, 0.03f, 0.94f);
		Frame(PX, PY, PW, PH, 0.35f, 0.85f, 0.40f, 1.0f);
		Rect(PX, PY, PW, TITLE_H, 0.07f, 0.18f, 0.08f, 1.0f);
		Text(PX + 18, PY + 9, 20, titles[s_page], 0.75f, 1.0f, 0.75f);
		Button(ID_CLOSE, PX + PW - 36, PY + 6, 28, 28, "X");
		DrawPage();
	}

	bool Begin()
	{
		s_pUI = g_pGame && g_pGame->GetIGameFramework() ? g_pGame->GetIGameFramework()->GetIUIDraw() : 0;
		if (!s_pFont)
			s_pFont = CoopText::Font();
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
		else if (!stricmp(what, "measure") && pArgs->GetArgCount() > 2)
		{
			// testing the mod's font: its name, a text's size (IUIDraw's units)
			IUIDraw* pUI = g_pGame->GetIGameFramework()->GetIUIDraw();
			IFFont* pFont = CoopText::Font();
			float w = 0, h = 0, wa = 0, ha = 0;
			const std::wstring text = CoopText::Wide(CoopText::Tr(pArgs->GetArg(2)));
			if (pUI && pFont)
			{
				pUI->GetTextDimW(pFont, &w, &h, 17, 17, text.c_str());
				pUI->GetTextDim(pFont, &wa, &ha, 17, 17, pArgs->GetArg(2));
				float wd = 0, hd = 0;
				IFFont* pDefault = gEnv->pCryFont->GetFont("default");
				pUI->GetTextDim(pDefault, &wd, &hd, 17, 17, pArgs->GetArg(2));
				pFont->SetSize(vector2f(17, 17));
				const vector2f own = pFont->GetTextSizeW(text.c_str());
				CryLogAlways("[CoopMenu] measure: default font %.1f x %.1f; the font's own size %.1f x %.1f, char %.2f x %.2f",
					wd, hd, own.x, own.y, pFont->GetCharWidth(), pFont->GetCharHeight());
			}
			CryLogAlways("[CoopMenu] measure '%s' -> '%s': font %p (%s), W %.1f x %.1f, plain %.1f x %.1f, renderer %dx%d",
				pArgs->GetArg(2), CoopText::Tr(pArgs->GetArg(2)), (void*)pFont, pFont == (gEnv->pCryFont ? gEnv->pCryFont->GetFont("coop") : 0) ? "coop" : "other",
				w, h, wa, ha, gEnv->pRenderer->GetWidth(), gEnv->pRenderer->GetHeight());
		}
		else if (!stricmp(what, "framelog"))
			for (int i = 0; i < 32 && i < s_frameNoteCount; ++i)
			{
				const SFrameNote& n = s_frameNotes[(s_frameNoteCount - 1 - i) % 32];
				CryLogAlways("[CoopMenu] frame %d: screen %dx%d, panel %.0fx%.0f", n.frame, n.w, n.h, n.pw, n.ph);
			}
		else if (!stricmp(what, "console"))
			gEnv->pConsole->ShowConsole(pArgs->GetArgCount() < 3 || atoi(pArgs->GetArg(2)) != 0);
		else if (!stricmp(what, "page") && pArgs->GetArgCount() > 2)
			Open((EPage)atoi(pArgs->GetArg(2)));
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
	// the mod's font is loaded with the game (one made later measured nothing)
	CoopText::Font();
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
	SFrameNote& note = s_frameNotes[s_frameNoteCount++ % 32];
	note.frame = s_drawnFrame;
	note.w = gEnv->pRenderer->GetWidth();
	note.h = gEnv->pRenderer->GetHeight();
	note.pw = PW;
	note.ph = PH;
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
	CoopText::Draw(pUIDraw, 12, 8, 15, 15, text.c_str(), 0.9f, 0.8f, 1.0f, 0.8f,
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
