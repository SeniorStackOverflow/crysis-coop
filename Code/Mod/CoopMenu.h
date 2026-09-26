// Crysis Coop: the co-op menu. A "CO-OP" button on the main menu and on the
// in-game menu (Esc) opens a panel from which everything co-op is done with
// the mouse: a new campaign on any level, the campaigns on this PC and in the
// cloud (continue, the checkpoint before, delete), joining a friend by his
// code, and in the game the host's code, saving and going back to a
// checkpoint, or leaving a friend's game. While the host plays, the HUD
// shows his code and how many friends are in. The console commands
// (coop_host, coop_join...) do the same and stay as the fallback.
#pragma once

struct SInputEvent;
struct IUIDraw;
struct IFFont;

namespace CoopMenu
{
	void Init();                                // coop_ui test command
	// the Flash menu is on screen (inGame: the Esc menu): draws the CO-OP
	// button, and the panel when it is open
	void RenderMenu(bool inGame);
	// in the game with no menu: the host's code line
	void RenderHud(IUIDraw* pUIDraw, IFFont* pFont);
	// the menu's mouse (window pixels; HARDWAREMOUSEEVENT_*) and keyboard:
	// true when the co-op menu used it (the Flash menu must not get it)
	bool OnMouse(int x, int y, int hardwareMouseEvent);
	bool OnKey(const SInputEvent& event);
	// the Flash menu was closed: so is the panel
	void OnMenuClosed();
	bool IsOpen();
}
