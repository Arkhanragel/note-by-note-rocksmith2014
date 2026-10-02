#include "stats.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace nbn::stats {

float Trouble(Result r, double waitS, bool wrongNote) {
    switch (r) {
        case Result::kOnTime: return 0.0f;
        case Result::kSkipped: return 1.0f;
        case Result::kMissed: return 0.75f;
        default: break;
    }
    const float wait = (float)std::min(std::max(waitS, 0.0), 4.0) / 4.0f;
    return std::min(1.0f, 0.25f + 0.5f * wait + (wrongNote ? 0.25f : 0.0f));
}

namespace {

std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }  // (names are ASCII)

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// The newest file in dir whose name ends in tail + ".txt"; "" if none.
std::wstring NewestEndingIn(const std::wstring& dir, const std::wstring& tail) {
    const std::wstring end = tail + L".txt";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*" + end).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::wstring best;
    FILETIME bestTime{};
    do {
        const std::wstring n = fd.cFileName;
        // (the pattern can also match a file's short 8.3 name: check the long one)
        if (n.size() < end.size() || n.compare(n.size() - end.size(), end.size(), end) != 0) continue;
        if (best.empty() || CompareFileTime(&fd.ftLastWriteTime, &bestTime) > 0) {
            best = n;
            bestTime = fd.ftLastWriteTime;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return best.empty() ? L"" : dir + best;
}

}  // namespace

void SongStats::Open(const std::wstring& dir, const std::string& name, const std::string& tail) {
    std::wstring path = dir + Wide(name) + L".txt";
    if (!tail.empty() && !Exists(path)) {
        const std::wstring keyless = dir + L"song" + Wide(tail) + L".txt";
        if (path == keyless) {
            // The song's key isn't known (the mod was loaded in the middle of a game): its record has
            // the arrangement's fingerprint at the end of its name.
            if (const std::wstring found = NewestEndingIn(dir, Wide(tail)); !found.empty()) path = found;
        } else if (Exists(keyless)) {
            // A record made without the key: from now on it's under the song's name.
            if (path_ == keyless) Save();
            if (!MoveFileW(keyless.c_str(), path.c_str())) path = keyless;
            else if (path_ == keyless) path_ = path;  // (the same record, open already)
        }
    }
    if (path == path_) return;
    Save();
    notes_.clear();
    run_ = Run{};
    dirty_ = false;
    path_ = path;
    std::ifstream in(path_);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == ';') continue;
        std::istringstream s(line);
        int t = 0, tries = 0, streak = 0;
        float trouble = 0;
        if (!(s >> t >> trouble >> tries)) continue;
        if (!(s >> streak)) streak = 0;  // (files from before the streak)
        notes_[t] = Note{std::min(1.0f, std::max(0.0f, trouble)), tries, std::max(0, streak)};
    }
}

bool SongStats::Record(double t, Result r, double waitS, bool wrongNote, int clearAfter) {
    if (path_.empty()) return false;
    const float x = Trouble(r, waitS, wrongNote);
    Note& n = notes_[(int)std::lround(t * 1000.0)];
    bool cleared = false;
    if (r == Result::kOnTime) {
        ++n.streak;
        cleared = n.trouble > 0 && n.streak == std::max(1, clearAfter);
        run_.cleared += cleared;
    } else {
        n.trouble = n.trouble <= 0 ? x : 0.5f * n.trouble + 0.5f * x;
        n.streak = 0;
    }
    ++n.tries;
    dirty_ = true;
    if (r == Result::kOnTime) ++run_.onTime;
    else if (r == Result::kSkipped) ++run_.skips;
    else if (r == Result::kMissed) ++run_.missed;
    else ++run_.stops;
    if (r == Result::kWaited && waitS > run_.longestWait) {
        run_.longestWait = waitS;
        run_.longestAt = t;
    }
    return cleared;
}

int SongStats::Streak(double t) const {
    const auto it = notes_.find((int)std::lround(t * 1000.0));
    return it == notes_.end() ? 0 : it->second.streak;
}

bool SongStats::Progress(double t, int* streak) const {
    const auto it = notes_.find((int)std::lround(t * 1000.0));
    if (it == notes_.end() || it->second.trouble <= 0) return false;
    *streak = it->second.streak;
    return true;
}

void SongStats::Save() {
    if (!dirty_ || path_.empty()) return;
    const size_t slash = path_.find_last_of(L'\\');
    if (slash != std::wstring::npos) CreateDirectoryW(path_.substr(0, slash).c_str(), nullptr);
    std::ofstream out(path_, std::ios::trunc);
    if (!out) return;
    out << "; Note-by-Note trouble spots: note time (ms), trouble (0 = none .. 1 = skipped), tries, played on time"
           " in a row since\n";
    char buf[64];
    for (const auto& [t, n] : notes_) {
        std::snprintf(buf, sizeof(buf), "%d %.3f %d %d\n", t, n.trouble, n.tries, n.streak);
        out << buf;
    }
    dirty_ = false;
}

void SongStats::Forget() {
    notes_.clear();
    run_ = Run{};
    dirty_ = false;
    if (!path_.empty()) DeleteFileW(path_.c_str());
}

std::vector<Spot> SongStats::Spots(const std::vector<std::pair<double, double>>& phrases, int clearAfter) const {
    clearAfter = std::max(1, clearAfter);
    std::vector<Spot> out;
    out.reserve(phrases.size());
    float best = 0;
    for (const auto& [a, b] : phrases) {
        Spot sp;
        sp.start = a;
        sp.end = b;
        const auto from = notes_.lower_bound((int)std::lround(a * 1000.0));
        const auto to = notes_.lower_bound((int)std::lround(b * 1000.0));
        for (auto it = from; it != to; ++it) {
            const Note& n = it->second;
            const float left = 1.0f - (float)std::min(n.streak, clearAfter) / (float)clearAfter;  // 0 = cleared
            sp.score += n.trouble * left;
            if (n.trouble > 0) {
                ++sp.troubled;
                sp.cleared += n.streak >= clearAfter;
            }
        }
        best = std::max(best, sp.score);
        out.push_back(sp);
    }
    for (auto& sp : out) sp.heat = (sp.score >= 0.5f && best > 0) ? sp.score / best : 0.0f;
    return out;
}

std::string RecordTail(const std::string& arrangement, const std::vector<int>& levelCounts, size_t phrases) {
    // FNV-1a over the arrangement's shape: the same song and arrangement always give the same name.
    uint32_t h = 2166136261u;
    auto mix = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            h ^= (v >> (8 * i)) & 0xFF;
            h *= 16777619u;
        }
    };
    for (int c : levelCounts) mix((uint32_t)c);
    mix((uint32_t)phrases);
    std::string tail = "_";
    for (char c : arrangement) tail += std::isalnum((unsigned char)c) ? c : '_';
    char hex[16];
    std::snprintf(hex, sizeof(hex), "_%08X", h);
    return tail.substr(0, 20) + hex;
}

std::string RecordName(const std::string& songKey, const std::string& arrangement, const std::vector<int>& levelCounts,
                       size_t phrases) {
    std::string name;
    for (char c : songKey.empty() ? std::string("song") : songKey)
        name += (std::isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_';
    return name.substr(0, 40) + RecordTail(arrangement, levelCounts, phrases);
}

}  // namespace nbn::stats
