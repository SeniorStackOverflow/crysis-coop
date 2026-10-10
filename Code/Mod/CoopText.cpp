// Crysis Coop: the mod's texts in the game's language (see CoopText.h).
#include "StdAfx.h"
#include "CoopText.h"
#include "IUIDraw.h"
#include <ISystem.h>
#include <IFont.h>

#include <map>
#include <vector>
#include <algorithm>

namespace
{
	struct SPattern
	{
		std::vector<string> pieces;     // the English text around each %s
		string translation;
		size_t literal = 0;             // the length of the pieces: longer ones are tried first
	};

	string s_language;                  // the language the texts are for
	bool s_translated = false;
	std::map<string, string> s_exact;
	std::vector<SPattern> s_patterns;
	std::map<string, string> s_cache;   // a text seen before and what it became
	IFFont* s_pFont = 0;
	bool s_fontTried = false;

	std::vector<string> Split(const string& text)
	{
		std::vector<string> pieces;
		size_t from = 0;
		for (size_t at; (at = text.find("%s", from)) != string::npos; from = at + 2)
			pieces.push_back(text.substr(from, at - from));
		pieces.push_back(text.substr(from));
		return pieces;
	}

	void Load(const string& language)
	{
		s_language = language;
		s_translated = false;
		s_exact.clear();
		s_patterns.clear();
		s_cache.clear();
		string lower = language;
		lower.MakeLower();
		if (lower.empty() || lower == "english")
			return;
		const string path = "Languages/coop_text_" + lower + ".txt";
		FILE* f = gEnv->pCryPak->FOpen(path.c_str(), "rb");
		if (!f)
		{
			CryLogAlways("[CoopText] no texts for %s (%s): English", language.c_str(), path.c_str());
			return;
		}
		const size_t size = gEnv->pCryPak->FGetSize(f);
		std::vector<char> data(size + 1, 0);
		gEnv->pCryPak->FReadRaw(&data[0], 1, size, f);
		gEnv->pCryPak->FClose(f);
		const char* p = &data[0];
		if (size >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
			p += 3;
		while (*p)
		{
			const char* end = strchr(p, '\n');
			string line = end ? string(p, end - p) : string(p);
			p = end ? end + 1 : p + strlen(p);
			if (!line.empty() && line[line.length() - 1] == '\r')
				line = line.substr(0, line.length() - 1);
			const size_t tab = line.find('\t');
			if (line.empty() || line[0] == '#' || tab == string::npos || tab == 0)
				continue;
			const string english = line.substr(0, tab), translation = line.substr(tab + 1);
			if (english.find("%s") == string::npos)
				s_exact[english] = translation;
			else
			{
				SPattern pat;
				pat.pieces = Split(english);
				pat.translation = translation;
				for (size_t i = 0; i < pat.pieces.size(); ++i)
					pat.literal += pat.pieces[i].length();
				s_patterns.push_back(pat);
			}
		}
		std::stable_sort(s_patterns.begin(), s_patterns.end(), [](const SPattern& a, const SPattern& b) { return a.literal > b.literal; });
		s_translated = true;
		CryLogAlways("[CoopText] %d texts and %d patterns for %s (%s)", (int)s_exact.size(), (int)s_patterns.size(), language.c_str(), path.c_str());
	}

	void CheckLanguage()
	{
		ILocalizationManager* pLoc = gEnv && gEnv->pSystem ? gEnv->pSystem->GetLocalizationManager() : 0;
		const char* language = pLoc && pLoc->GetLanguage() ? pLoc->GetLanguage() : "";
		if (s_language != language || (s_language.empty() && !s_exact.empty()))
			Load(language);
	}

	// the English text matched against a pattern: the parts its %s stand for
	bool Match(const SPattern& pat, const string& text, std::vector<string>& parts)
	{
		const std::vector<string>& pieces = pat.pieces;
		// (CryString's compare(pos, n, text) does not say 0 for equal parts)
		if (strncmp(text.c_str(), pieces[0].c_str(), pieces[0].length()) != 0)
			return false;
		size_t pos = pieces[0].length();
		parts.clear();
		for (size_t i = 1; i < pieces.size(); ++i)
		{
			const string& piece = pieces[i];
			size_t at;
			if (i + 1 == pieces.size())
			{
				// the last piece ends the text
				if (text.length() < pos + 1 + piece.length())
					return false;
				at = text.length() - piece.length();
				if (strncmp(text.c_str() + at, piece.c_str(), piece.length()) != 0)
					return false;
			}
			else
			{
				at = piece.empty() ? string::npos : text.find(piece, pos + 1);
				if (at == string::npos)
					return false;
			}
			if (at <= pos)
				return false;
			parts.push_back(text.substr(pos, at - pos));
			pos = at + piece.length();
		}
		return true;
	}

	string Translate(const string& text, int depth)
	{
		std::map<string, string>::const_iterator exact = s_exact.find(text);
		if (exact != s_exact.end())
			return exact->second;
		if (depth > 2)
			return text;
		std::vector<string> parts;
		for (size_t i = 0; i < s_patterns.size(); ++i)
			if (Match(s_patterns[i], text, parts))
			{
				const std::vector<string> out = Split(s_patterns[i].translation);
				string result = out[0];
				for (size_t k = 1; k < out.size(); ++k)
				{
					if (k - 1 < parts.size())
						result += Translate(parts[k - 1], depth + 1);
					result += out[k];
				}
				return result;
			}
		return text;
	}
}

bool CoopText::Translated()
{
	CheckLanguage();
	return s_translated;
}

const char* CoopText::Tr(const char* english)
{
	if (!english || !english[0] || !Translated())
		return english ? english : "";
	std::map<string, string>::const_iterator it = s_cache.find(english);
	if (it != s_cache.end())
		return it->second.c_str();
	const string result = Translate(english, 0);
	if (s_cache.size() < 20000)
		return s_cache.insert(std::make_pair(string(english), result)).first->second.c_str();
	// a long session of changing texts: no more remembered
	static string s_ring[64];
	static int s_next = 0;
	string& slot = s_ring[s_next++ & 63];
	slot = result;
	return slot.c_str();
}

std::wstring CoopText::Wide(const char* utf8)
{
	std::wstring out;
	if (!utf8)
		return out;
	const unsigned char* p = (const unsigned char*)utf8;
	while (*p)
	{
		unsigned int c = *p, extra = 0;
		if (c >= 0xF0 && c < 0xF8) { c &= 0x07; extra = 3; }
		else if (c >= 0xE0) { c &= 0x0F; extra = 2; }
		else if (c >= 0xC0) { c &= 0x1F; extra = 1; }
		bool ok = c < 0x80 || extra > 0;
		for (unsigned int i = 1; ok && i <= extra; ++i)
			ok = (p[i] & 0xC0) == 0x80;
		if (!ok)
		{
			// not UTF-8: the byte as it is
			out += (wchar_t)*p++;
			continue;
		}
		for (unsigned int i = 1; i <= extra; ++i)
			c = (c << 6) | (p[i] & 0x3F);
		p += extra + 1;
		out += (wchar_t)(c > 0xFFFF ? L'?' : c);
	}
	return out;
}

IFFont* CoopText::Font()
{
	if (!s_fontTried && gEnv->pCryFont)
	{
		s_fontTried = true;
		s_pFont = gEnv->pCryFont->GetFont("coop");
		if (!s_pFont)
		{
			s_pFont = gEnv->pCryFont->NewFont("coop");
			if (s_pFont && !s_pFont->Load("Fonts/coop.xml"))
			{
				CryLogAlways("[CoopText] Fonts/coop.xml did not load: the default font (no Cyrillic)");
				s_pFont = 0;
			}
		}
	}
	return s_pFont ? s_pFont : (gEnv->pCryFont ? gEnv->pCryFont->GetFont("default") : 0);
}

void CoopText::Draw(IUIDraw* pUI, float x, float y, float sizeX, float sizeY, const char* english,
	float a, float r, float g, float b, int dockH, int dockV, int alignH, int alignV)
{
	IFFont* pFont = Font();
	if (!pUI || !pFont || !english)
		return;
	const std::wstring text = Wide(Tr(english));
	pUI->DrawTextW(pFont, x, y, sizeX, sizeY, text.c_str(), a, r, g, b,
		(EUIDRAWHORIZONTAL)dockH, (EUIDRAWVERTICAL)dockV, (EUIDRAWHORIZONTAL)alignH, (EUIDRAWVERTICAL)alignV);
}

float CoopText::Width(IUIDraw* pUI, float size, const char* english)
{
	IFFont* pFont = Font();
	if (!pUI || !pFont || !english || !english[0])
		return 0.0f;
	const std::wstring text = Wide(Tr(english));
	float w = 0.0f, h = 0.0f;
	pUI->GetTextDimW(pFont, &w, &h, size, size, text.c_str());
	return w;
}

string CoopText::ForConsole(const char* english)
{
	const std::wstring wide = Wide(Tr(english));
	string out;
	for (size_t i = 0; i < wide.length(); ++i)
	{
		const unsigned int c = wide[i];
		if (c < 0x80)
			out += (char)c;
		else if (c >= 0x410 && c <= 0x44F)
			out += (char)(0xC0 + (c - 0x410));
		else if (c == 0x401) out += (char)0xA8;    // Ё
		else if (c == 0x451) out += (char)0xB8;    // ё
		else if (c == 0xAB) out += (char)0xAB;     // «
		else if (c == 0xBB) out += (char)0xBB;     // »
		else if (c == 0x2014) out += (char)0x97;   // —
		else if (c == 0x2013) out += (char)0x96;   // –
		else if (c == 0x2116) out += (char)0xB9;   // №
		else
			out += '?';
	}
	return out;
}

void CoopText::Say(const char* format, ...)
{
	char english[2048];
	va_list args;
	va_start(args, format);
	_vsnprintf(english, sizeof(english) - 1, format, args);
	va_end(args);
	english[sizeof(english) - 1] = 0;
	if (!Translated() || !gEnv->pLog)
	{
		CryLogAlways("%s", english);
		return;
	}
	// the log file: English (LogToFile writes only at some verbosity)
	ICVar* pVerbosity = gEnv->pConsole ? gEnv->pConsole->GetCVar("log_FileVerbosity") : 0;
	const int was = pVerbosity ? pVerbosity->GetIVal() : 0;
	if (pVerbosity && was < 3)
		pVerbosity->Set(3);
	gEnv->pLog->LogToFile("%s", english);
	if (pVerbosity && was < 3)
		pVerbosity->Set(was);
	// the console: the game's language
	if (gEnv->pConsole)
		gEnv->pConsole->PrintLine(ForConsole(english).c_str());
}
