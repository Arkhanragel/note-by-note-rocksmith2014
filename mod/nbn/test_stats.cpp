// nbn_stats_test: checks the trouble spots (stats.h): the trouble of a try, the average that forgets,
// the heat per phrase, and saving / loading a record. Exit code 1 on failure.
#define NOMINMAX
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <string>

#include "stats.h"

using nbn::stats::Result;

int main() {
    int fails = 0;
    auto expect = [&](const char* what, double got, double want) {
        const bool ok = std::abs(got - want) < 1e-3;
        fails += !ok;
        std::printf("%s  %-36s %.3f (wanted %.3f)\n", ok ? "ok  " : "FAIL", what, got, want);
    };

    // One try.
    expect("on time", nbn::stats::Trouble(Result::kOnTime, 0, false), 0);
    expect("short wait", nbn::stats::Trouble(Result::kWaited, 0, false), 0.25);
    expect("2 s wait", nbn::stats::Trouble(Result::kWaited, 2, false), 0.5);
    expect("long wait + wrong note", nbn::stats::Trouble(Result::kWaited, 9, true), 1);
    expect("skipped", nbn::stats::Trouble(Result::kSkipped, 0, false), 1);
    expect("missed", nbn::stats::Trouble(Result::kMissed, 0, false), 0.75);

    // A record in a temporary folder.
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"nbn_stats_test\\";
    const std::string name = nbn::stats::RecordName("Test Song!", "guitar", {10, 20, 30}, 4);
    expect("name is stable", name == nbn::stats::RecordName("Test Song!", "guitar", {10, 20, 30}, 4), 1);
    expect("lead and rhythm differ", name != nbn::stats::RecordName("Test Song!", "guitar", {10, 21, 30}, 4), 1);
    std::printf("      name: %s\n", name.c_str());

    nbn::stats::SongStats st;
    st.Open(dir, name);
    st.Forget();  // (a left-over from an earlier run)
    st.Open(dir, name + "_x");
    st.Open(dir, name);
    // Phrases 0-10, 10-20, 20-30. A skip in the first, two waits in the second, on time in the third.
    st.Record(1.0, Result::kSkipped, 0, false, 2);
    st.Record(12.0, Result::kWaited, 2, false, 2);
    st.Record(13.0, Result::kWaited, 0, false, 2);
    st.Record(25.0, Result::kOnTime, 0, false, 2);
    const std::vector<std::pair<double, double>> phrases{{0, 10}, {10, 20}, {20, 30}};
    auto spots = st.Spots(phrases, 2);
    expect("phrase 1 is the hardest", spots[0].heat, 1);
    expect("phrase 2 score", spots[1].score, 0.75);
    expect("phrase 2 heat", spots[1].heat, 0.75);
    expect("phrase 3: none", spots[2].heat, 0);
    expect("this time: waited", st.ThisRun().stops, 2);
    expect("this time: longest wait", st.ThisRun().longestWait, 2);

    // Playing the skipped note on time: it fades, and is cleared after "clear after" times in a row.
    st.Record(1.0, Result::kOnTime, 0, false, 2);
    expect("skipped note, 1 of 2 good tries", st.Spots(phrases, 2)[0].score, 0.5);
    expect("skipped note, 1 of 3 good tries", st.Spots(phrases, 3)[0].score, 2.0 / 3.0);
    st.Record(1.0, Result::kOnTime, 0, false, 2);
    spots = st.Spots(phrases, 2);
    expect("skipped note after 2 of 2: cleared", spots[0].score, 0);
    expect("now phrase 2 is the hardest", spots[1].heat, 1);
    expect("phrase 1: 1 of 1 notes cleared", spots[0].cleared, 1);
    expect("phrase 2: 0 of 2 notes cleared", spots[1].troubled - spots[1].cleared, 2);
    expect("this time: 1 note cleared", st.ThisRun().cleared, 1);
    int streak = -1;
    expect("progress of the cleared note", st.Progress(1.0, &streak) && streak == 2, 1);
    expect("no progress for a note never wrong", st.Progress(25.0, &streak), 0);
    expect("but its streak: 1", st.Streak(25.0), 1);
    expect("streak of a note not in the record", st.Streak(99.0), 0);
    expect("with 3 needed: a third left", st.Spots(phrases, 3)[0].score, 1.0 / 3.0);
    // Going wrong again starts the count over.
    st.Record(1.0, Result::kWaited, 0, false, 2);
    expect("wrong again: 0.5 x 1 + 0.5 x 0.25", st.Spots(phrases, 2)[0].score, 0.625);
    st.Record(1.0, Result::kOnTime, 0, false, 2);
    st.Record(1.0, Result::kOnTime, 0, false, 2);

    // Saved and loaded again.
    st.Save();
    nbn::stats::SongStats again;
    again.Open(dir, name);
    const auto loaded = again.Spots(phrases, 2);
    expect("loaded: phrase 1 (cleared, the streak kept)", loaded[0].score, 0);
    expect("loaded: phrase 2", loaded[1].score, 0.75);
    expect("loaded: no run yet", again.ThisRun().stops, 0);
    again.Forget();
    nbn::stats::SongStats gone;
    gone.Open(dir, name);
    expect("forgotten", gone.Empty(), 1);
    RemoveDirectoryW(dir.c_str());

    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
