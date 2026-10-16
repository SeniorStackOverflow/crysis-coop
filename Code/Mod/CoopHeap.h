// Crysis Coop: a watch on the engine's heap in the autotester's games. The
// host crashed now and then right at the start of a level load inside
// MSVCR80's small-block heap (malloc, +0x68FF: its bookkeeping was damaged
// earlier, 2 boots of 83, 2026-10-15/16). In a game with COOP_AUTOTEST set
// the heap is checked at many points (each frame, every step of the mod's
// level start, every 100th entity spawned); the first damage is logged with
// the last point it was still whole, so a later crash tells where it began.
#pragma once

namespace CoopHeap
{
	// the heap checked here (nothing outside the autotester's games)
	void Check(const char* where);
	// the frame's check (every frame until the level is there, then every 30th)
	void CheckFrame(bool levelReady);
}
