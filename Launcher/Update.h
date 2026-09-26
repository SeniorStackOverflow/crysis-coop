// Crysis Coop launcher: automatic updates of the mod. Before the game starts,
// the launcher asks the update servers (the mod's VPS, then GitHub's latest
// release) for update.txt. When it names a newer version, the package is
// downloaded, checked (the manifest's ECDSA signature by the release key,
// the package's SHA-256) and installed into Mods\Coop, the launcher in the
// game folder too. Nobody is asked; nothing here ever stops the game from
// starting. Everything goes to Mods\Coop\update.log.
#pragma once

#include <string>

namespace Update
{
	enum EResult
	{
		eResult_UpToDate,
		eResult_Updated,
		eResult_Skipped,        // off (noupdate), the game is running...
		eResult_Failed,         // no server, a bad package, a file that could not be written
	};

	// game: the Crysis folder; args: the launcher's command line
	// (-coop_noupdate, -coop_update_url <base> for tests)
	EResult Run(const std::wstring& game, const std::wstring& args);
}
