// game.h: everything the mod knows about Rocksmith2014.exe (Steam, "LPDecember2024" build).
//
// All addresses were verified on this build by disassembling a memory dump of the running game
// (the exe is encrypted on disk and protected by VMProtect). See BITACORA.md, "the freeze mechanism".
// Nothing here modifies game CODE: we only read/write game DATA and call game functions.
#pragma once
#include <string>
#include <vector>

namespace nbn::game {

// Checks the exe version and waits (up to ~2 min) until the game's code has been decrypted in memory
// and every function we call looks right. Returns false if the mod must stay disabled.
bool Init();

// Internal menu/screen name, e.g. "LearnASong_Game", "LearnASong_Pause", "MainMenu".
bool GetMenu(std::string* menu);

// The SongKey of the song highlighted in the song list (e.g. "NoteGel1"). The game only keeps it
// while browsing, so callers should remember the last value.
bool GetSongKey(std::string* key);

// Current song position in seconds: the clock the highway, scoring, etc. all read.
bool GetSongTime(double* t);

// Dynamic Difficulty: the level each phrase iteration is CURRENTLY showing on the highway
// (index = phrase iteration). Read fresh every time; the game changes it as the player improves.
bool GetPhraseLevels(std::vector<int>* levels);

// Number of notes in each difficulty level of the arrangement being played, as loaded by the game
// (used to pick the chart that matches the arrangement on screen).
bool GetLevelNoteCounts(std::vector<int>* counts);

// Freezes / resumes the song: pauses the Wwise playback the clock follows AND sets the clock
// provider's "stopped" flag, so music and highway stop together and resume in sync.
bool Freeze();
bool Unfreeze();

// Plays a UI sound event (feedback when the mode is switched on/off).
void PostUiEvent(const char* name);

// Forgets cached objects (call when a song ends/starts).
void ResetSongCache();

}  // namespace nbn::game
