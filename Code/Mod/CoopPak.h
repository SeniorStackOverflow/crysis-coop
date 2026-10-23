#pragma once

// Crysis Coop: a fix for a race in CrySystem's pak reader (CryPak), in every
// game (host, friends, the AI companion). See CoopPak.cpp
namespace CoopPak
{
	// at the game's start (before a level loads)
	void Init();
	// every frame: logs the races it caught
	void Update();
}
