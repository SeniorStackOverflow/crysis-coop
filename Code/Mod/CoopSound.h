// Crysis Coop: a watch over the game's sound. Once it had stopped coming out
// in the middle of a mission (s_SoundEnable 0 and 1 brought it back): when
// the game is silent for a long while in a level, the sound system starts
// again. The log tells what was seen.
#pragma once

namespace CoopSound
{
	void Update(bool haveFocus);
	// FMOD's own count of its channels: playing, heard, virtual (not heard:
	// only 64 are mixed), and its cpu use
	string FmodLine();
	// no real sound system (it failed to start: s_SoundEnable 0, no sound
	// device): its console variables point into freed memory, none may change
	// (on: CGame::Init, before the spec detection and the command line; off:
	// as the game ends)
	void GuardFailedSoundSystem(bool on);
	bool NoRealSoundSystem();
}
