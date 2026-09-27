// music.cpp: see music.h.
#include "music.h"

#include <algorithm>
#include <cctype>

namespace nbn::music {

namespace {

const char* kSharp[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
const char* kFlat[12] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};

int Pc(int midi) { return ((midi % 12) + 12) % 12; }

// Chord symbol suffixes (what follows the root) and their meaning. Checked longest first.
struct Suffix {
    const char* text;
    const char* words;  // "%s" = the root
};
const Suffix kSuffixes[] = {
    {"m7b5", "%s half-diminished"}, {"maj7", "%s major 7th"},  {"maj9", "%s major 9th"},
    {"min7", "%s minor 7th"},       {"7sus4", "%s 7th, suspended 4th"}, {"sus2", "%s suspended 2nd"},
    {"sus4", "%s suspended 4th"},   {"add9", "%s major, added 9th"},    {"madd9", "%s minor, added 9th"},
    {"dim7", "%s diminished 7th"},  {"aug", "%s augmented"},            {"dim", "%s diminished"},
    {"min", "%s minor"},            {"sus", "%s suspended 4th"},        {"M7", "%s major 7th"},
    {"m7", "%s minor 7th"},         {"m6", "%s minor 6th"},             {"m9", "%s minor 9th"},
    {"7", "%s 7th"},                {"6", "%s 6th"},                    {"9", "%s 9th"},
    {"5", "%s power chord"},        {"m", "%s minor"},                  {"-", "%s minor"},
    {"+", "%s augmented"},          {"", "%s major"},
};

// Chord shapes recognised from notes alone (intervals above the root, as a pitch-class set).
struct Shape {
    std::vector<int> iv;
    const char* words;
};
const Shape kShapes[] = {
    {{0, 4, 7}, "%s major"},          {{0, 3, 7}, "%s minor"},          {{0, 7}, "%s power chord"},
    {{0, 4, 7, 10}, "%s 7th"},        {{0, 3, 7, 10}, "%s minor 7th"},  {{0, 4, 7, 11}, "%s major 7th"},
    {{0, 2, 7}, "%s suspended 2nd"},  {{0, 5, 7}, "%s suspended 4th"},  {{0, 3, 6}, "%s diminished"},
    {{0, 4, 8}, "%s augmented"},
    // Two notes of a major/minor chord without its 5th: say so, it helps to know the "colour".
    {{0, 4}, "part of %s major"},     {{0, 3}, "part of %s minor"},
};

std::string Format(const char* words, const std::string& root) {
    std::string s = words;
    const size_t p = s.find("%s");
    if (p != std::string::npos) s.replace(p, 2, root);
    return s;
}

std::vector<int> PitchClasses(const std::vector<int>& notes) {  // lowest note first, no repeats
    std::vector<int> pcs;
    for (int m : notes)
        if (std::find(pcs.begin(), pcs.end(), Pc(m)) == pcs.end()) pcs.push_back(Pc(m));
    return pcs;
}

// Recognise the chord from its notes: the bass note is tried as the root first.
std::string FromNotes(const std::vector<int>& notes, bool flats) {
    const std::vector<int> pcs = PitchClasses(notes);
    if (pcs.size() < 2) return "";
    for (int root : pcs) {
        std::vector<int> iv;
        for (int pc : pcs) iv.push_back((pc - root + 12) % 12);
        std::sort(iv.begin(), iv.end());
        for (const auto& sh : kShapes) {
            if (sh.iv != iv) continue;
            std::string s = Format(sh.words, NoteName(root, flats));
            if (root != pcs[0] && sh.iv.size() > 2) s += ", with " + NoteName(pcs[0], flats) + " as the lowest note";
            return s;
        }
    }
    return "";
}

}  // namespace

std::string NoteName(int midi, bool flats) { return (flats ? kFlat : kSharp)[Pc(midi)]; }

bool UsesFlats(const std::string& n) { return n.size() >= 2 && std::isupper((unsigned char)n[0]) && n[1] == 'b'; }

std::string NoteList(const std::vector<int>& notes, bool flats) {
    const std::vector<int> pcs = PitchClasses(notes);
    std::string s;
    for (size_t i = 0; i < pcs.size(); ++i) {
        if (i) s += (i + 1 == pcs.size()) ? " and " : ", ";
        s += NoteName(pcs[i], flats);
    }
    return s;
}

std::string ChordMeaning(const std::string& rawName, const std::vector<int>& notes) {
    const bool flats = UsesFlats(rawName);
    std::string name = rawName;
    name.erase(std::remove(name.begin(), name.end(), ' '), name.end());
    // Root: a letter A-G, optionally # or b.
    if (name.empty() || name[0] < 'A' || name[0] > 'G') return FromNotes(notes, flats);
    size_t i = 1;
    if (i < name.size() && (name[i] == '#' || name[i] == 'b')) ++i;
    const std::string root = name.substr(0, i);
    std::string rest = name.substr(i), bass;
    const size_t slash = rest.find('/');
    if (slash != std::string::npos) {  // "G/B": B is the lowest note
        bass = rest.substr(slash + 1);
        rest = rest.substr(0, slash);
    }
    if (!rest.empty() && rest.front() == '(' && rest.back() == ')') rest = rest.substr(1, rest.size() - 2);
    for (const auto& sx : kSuffixes) {
        if (rest != sx.text) continue;
        std::string s = Format(sx.words, root);
        if (!bass.empty()) s += ", with " + bass + " as the lowest note";
        return s;
    }
    // A name we don't know (song authors write all sorts of things): describe the notes instead.
    return FromNotes(notes, flats);
}

}  // namespace nbn::music
