// Crysis Coop: a watch over the game's sound. Once it had stopped coming out
// in the middle of a mission (s_SoundEnable 0 and 1 brought it back): when
// the game is silent for a long while in a level, the sound system starts
// again. The log tells what was seen.
#pragma once

namespace CoopSound
{
	void Update(bool haveFocus);
}
