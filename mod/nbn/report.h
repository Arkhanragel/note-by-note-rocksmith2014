// report.h: NoteByNote_report.txt, a short summary of what the mod found in this game build and
// whether its checks passed (for testing other game versions: the tester sends this file).
//
// Unlike NoteByNote.log it only has the facts that matter for "does Note-by-Note work with this
// exe": the exe's numbers, where each game address was found, and the results of the checks made
// while playing (chart read, Dynamic Difficulty, song clock, freeze / resume). Rewritten at every
// start; each kind of check is reported a few times, not on every song.
#pragma once
#include <string>

namespace nbn::report {

void Open(const std::wstring& path);
void Close();

// Adds a line (printf format). Also copied to the log with a "report:" prefix.
void Line(const char* fmt, ...);

// Adds a line only the first `limit` times this key is used (e.g. "freeze", 3).
void Limited(const char* key, int limit, const char* fmt, ...);

}  // namespace nbn::report
