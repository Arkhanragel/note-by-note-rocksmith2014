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

void SongStats::Open(const std::wstring& dir, const std::string& name) {
    const std::wstring path = dir + std::wstring(name.begin(), name.end()) + L".txt";
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

void SongStats::Record(double t, Result r, double waitS, bool wrongNote) {
    if (path_.empty()) return;
    const float x = Trouble(r, waitS, wrongNote);
    Note& n = notes_[(int)std::lround(t * 1000.0)];
    if (r == Result::kOnTime) {
        ++n.streak;
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
        }
        best = std::max(best, sp.score);
        out.push_back(sp);
    }
    for (auto& sp : out) sp.heat = (sp.score >= 0.5f && best > 0) ? sp.score / best : 0.0f;
    return out;
}

std::string RecordName(const std::string& songKey, const std::string& arrangement, const std::vector<int>& levelCounts,
                       size_t phrases) {
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
    std::string name;
    for (char c : songKey.empty() ? std::string("song") : songKey)
        name += (std::isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_';
    name += '_';
    for (char c : arrangement) name += std::isalnum((unsigned char)c) ? c : '_';
    char hex[16];
    std::snprintf(hex, sizeof(hex), "_%08X", h);
    return name.substr(0, 60) + hex;
}

}  // namespace nbn::stats
