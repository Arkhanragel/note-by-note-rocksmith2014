// fastintro.h: plays the game's start-up logos faster.
//
// The intro (Ubisoft, studios, PEGI, brands, Gamebryo, then the Rocksmith title) is a Scaleform
// movie with fixed timing (~26 s); keys don't skip it, and it is also the time the game loads in
// the background. We don't touch the game files: instead, while the intro plays, the Windows clocks
// the game reads (QueryPerformanceCounter, timeGetTime) run N times faster, so the movie reaches
// the title N times sooner. At the title (or the first dialog) the clocks go back to normal speed.
//
// A clock can never go backwards, so "back to normal" means: real time + the seconds we skipped,
// from then on (the hooks stay installed, adding a constant; cheap).
#pragma once
#include <string>

namespace nbn::fastintro {

// Installs the clock hooks with the given speed (2..8; 1 = off: nothing is installed). Call as early
// as possible (the intro is already playing when the mod loads).
void Start(int speed);

// Call every main-loop iteration. titleOrLater = the title screen or any later screen is showing.
// Ends the fast part (once), and logs which clocks the game used.
void Tick(bool titleOrLater);

// Removes the hooks (before the DLL unloads).
void Stop();

}  // namespace nbn::fastintro
