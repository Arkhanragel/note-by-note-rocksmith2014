// game.h: everything the mod knows about Rocksmith2014.exe.
//
// Verified build: Steam "Learn & Play" (December 2024, RSMods "LPDecember2024"): every address was
// checked by disassembling a memory dump of the running game (the exe is encrypted on disk and
// protected by VMProtect; see docs/TECHNICAL.md, "Pausing the song without a pause screen"). Other builds (the older
// Remastered of September 2022) are handled by finding the same addresses by byte pattern; they
// only run in test mode until verified, and NoteByNote_report.txt says what was found.
// Nothing here modifies game CODE: we only read/write game DATA and call game functions.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nbn {
struct Chart;
}

namespace nbn::game {

// Checks the exe version and waits (up to ~2 min) until the game's code has been decrypted in memory
// and every function we call looks right. Returns false if the mod must stay disabled.
// allowUnverified: on a build that isn't verified, use the addresses found by pattern (test mode).
// patternsOnly (dev): ignore the verified table, as if the build were unknown (tests the patterns).
// Writes the start of the report (report.h), which must be open.
bool Init(bool allowUnverified, bool patternsOnly = false);

// True after Init on a build whose addresses are verified (false: unknown build, a build only found
// by pattern, or the dev's patternsOnly). What has never run on a build stays off there: see main.cpp.
bool Verified();

// Call often from the main loop: finishes the report's checks that need time (song clock after a resume).
void Tick();

// Internal menu/screen name, e.g. "LearnASong_Game", "LearnASong_Pause", "MainMenu".
bool GetMenu(std::string* menu);

// Before the first dialog (intro, title) GetMenu fails: the same field then holds a short name in
// place instead of a pointer to it (per RSMods: "", "TitleScreen", "MainOverlay"...).
bool GetPreMenu(std::string* name);

// The SongKey of the song highlighted in the song list (e.g. "NoteGel1"). The game only keeps it
// while browsing, so callers should remember the last value.
bool GetSongKey(std::string* key);

// Current song position in seconds: the clock the highway, scoring, etc. all read.
bool GetSongTime(double* t);
// The game greys out every note before this song time (seconds): after resuming from the pause
// screen it replays a few seconds with the notes already passed greyed. Usually <= the song time.
bool GetGreyTime(double* t);
// Riff Repeater's loop (song seconds; at `end` the game rewinds to a few seconds before `start`).
// false = no loop (normal play).
bool GetLoop(double* start, double* end);
// Length of the loaded song in seconds (SongLength of the arrangement being played).
bool GetSongLength(double* len);

// Dynamic Difficulty: the level each phrase iteration is CURRENTLY showing on the highway
// (index = phrase iteration). Read fresh every time; the game changes it as the player improves.
bool GetPhraseLevels(std::vector<int>* levels);

// Number of notes in each difficulty level of the arrangement being played, as loaded by the game
// (used to pick the chart that matches the arrangement on screen).
bool GetLevelNoteCounts(std::vector<int>* counts);

// Address of the loaded arrangement (changes when another song/arrangement is loaded), 0 if none.
uintptr_t SongDataAddress();

// Builds the chart of the arrangement being played straight from game memory: every difficulty
// level, phrase iterations, chords, tuning and capo. No exported chart files are needed, so it
// works for any song or CDLC. False while the song is still loading.
bool ReadSongChart(Chart* chart);

// Freezes / resumes the song: pauses the Wwise playback the clock follows AND sets the clock
// provider's "stopped" flag, so music and highway stop together and resume in sync.
void PrepareFreeze();  // finds what Freeze needs ahead of time (the search can take most of a second)
bool Freeze();
bool Unfreeze();

// Plays a UI sound event (feedback when the mode is switched on/off).
void PostUiEvent(const char* name);

// Forgets cached objects (call when a song ends/starts).
void ResetSongCache();

}  // namespace nbn::game
