// stats.h: trouble spots. How each note went (played on time, waited for, skipped), kept per song and
// arrangement in NoteByNote_stats\, so the practice bar can show where the player stops most and the
// menu can list the hardest parts.
//
// Each note has a "trouble" value between 0 and 1, from the times it went wrong:
//   waited for, then played           0.25 + up to 0.5 for a long wait (4 s or more) + 0.25 after a wrong note
//   skipped (F9 / the menu)           1
//   missed ("Show the notes": the song passed it while the player was playing)   0.75
// A new one counts as much as all the earlier ones together. Played on time (no stop) counts towards
// clearing it instead: after `clearAfter` times in a row (the player's setting) the note is no trouble
// any more, and on the way there it fades (2 of 3 = a third of its trouble left). Going wrong again
// starts the count over.
//
// The file has no song data: just the times of the notes and their numbers ("t trouble tries streak"
// per line, t in ms).
#pragma once
#include <map>
#include <string>
#include <vector>

namespace nbn::stats {

enum class Result { kOnTime, kWaited, kSkipped, kMissed };

// The trouble of one try.
float Trouble(Result r, double waitS, bool wrongNote);

// A phrase of the song and how much trouble the player had there.
struct Spot {
    double start = 0, end = 0;  // song seconds
    float heat = 0;             // 0..1, relative to the hardest phrase of the song (1 = the hardest)
    float score = 0;            // the sum of its notes' trouble
    int troubled = 0;           // its notes that ever went wrong, and how many of them are cleared
    int cleared = 0;            // (played on time clearAfter times in a row since)
};

class SongStats {
public:
    // Loads the record of a song (file = dir + name + ".txt"); a new song starts empty. Saves the
    // previous song first if it changed.
    void Open(const std::wstring& dir, const std::string& name);
    // Returns true when this try cleared the note (its clearAfter-th time on time in a row after going
    // wrong).
    bool Record(double t, Result r, double waitS, bool wrongNote, int clearAfter);

    // A note that went wrong before: true, and how many times in a row it was played on time since.
    // False for a note that never went wrong (or isn't in the record).
    bool Progress(double t, int* streak) const;
    // Times in a row any note was played on time (0 = not in the record yet, or it just went wrong).
    int Streak(double t) const;
    void Save();            // if anything changed since the last save
    void Forget();          // this song's record is deleted
    bool Empty() const { return notes_.empty(); }

    // The trouble per phrase (phrase starts and ends in song seconds), with notes played well
    // `clearAfter` times in a row cleared. Phrases with less than half a stop's worth of trouble get
    // heat 0.
    std::vector<Spot> Spots(const std::vector<std::pair<double, double>>& phrases, int clearAfter) const;

    // This time in the song (since Open or ResetRun): stops, skips, longest wait and where.
    struct Run {
        int stops = 0, skips = 0, onTime = 0, missed = 0;
        int cleared = 0;  // notes that reached "cleared" this time
        double longestWait = 0, longestAt = -1;
    };
    const Run& ThisRun() const { return run_; }
    void ResetRun() { run_ = Run{}; }

private:
    struct Note {
        float trouble = 0;  // from the tries that went wrong
        int tries = 0;      // all tries
        int streak = 0;     // played on time this many times in a row (since it last went wrong)
    };
    std::map<int, Note> notes_;  // by time in ms
    std::wstring path_;
    bool dirty_ = false;
    Run run_;
};

// A file name for a song: its key (from the song list, may be empty) and a fingerprint of the
// arrangement (the notes per difficulty level), so lead and rhythm get their own records.
std::string RecordName(const std::string& songKey, const std::string& arrangement, const std::vector<int>& levelCounts,
                       size_t phrases);

}  // namespace nbn::stats
