// Crysis Coop: the mod's texts in the game's language.
//
// The code says everything in English (the log stays English); what a
// player sees goes through Tr() on its way to the screen: the menu, the HUD,
// the revive texts and the HUD's messages (also those the host sends, so
// each player reads them in his own game's language). The translations are
// Languages/coop_text_<language>.txt (UTF-8): a line is the English text, a
// tab, the translation; %s stands for a part that changes (a name, a number)
// and comes back translated as well if it is a text of the list.
//
// The game's own font has no Cyrillic: the mod's texts are drawn with
// Fonts/coop.xml (DejaVu Sans Mono), through the wide-character calls.
#pragma once

struct IFFont;
struct IUIDraw;

namespace CoopText
{
	// the game's language is one the mod has texts for (not English)
	bool Translated();

	// the text a player sees (UTF-8); the English one when there is none
	const char* Tr(const char* english);

	// UTF-8 to wide characters (a byte that is not UTF-8 stays itself)
	std::wstring Wide(const char* utf8);

	// the mod's font (Cyrillic); the game's default one if it cannot load
	IFFont* Font();

	// IUIDraw's DrawText with the translation, wide characters and the mod's font
	void Draw(IUIDraw* pUI, float x, float y, float sizeX, float sizeY, const char* english,
		float a, float r, float g, float b, int dockH, int dockV, int alignH, int alignV);

	// the width a text takes (IUIDraw's units at that size), translated
	float Width(IUIDraw* pUI, float size, const char* english);

	// a message for the player in the console: shown in the game's language,
	// the log file keeps it in English. The console draws its text byte by
	// byte: Cyrillic goes to it in Windows-1251, which its font
	// (Fonts/console.xml, CoopConsole.ttf) has at those places
	void Say(const char* format, ...);

	// a text as the console shows it (translated, Windows-1251)
	string ForConsole(const char* english);
}
