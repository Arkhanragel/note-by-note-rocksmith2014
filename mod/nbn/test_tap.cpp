// test_tap.cpp: the guitar tap. Which RS_ASIO input feeds it (the rule in mod/common/GuitarTapShared.h),
// which tap the mod accepts (tap.h: only its own process's), and when a wait counts as silent (silence.h).
//
// The rule runs inside RS_ASIO's audio callback, where nothing can be tried by hand: here the inputs
// are simulated calling in every possible order, the way RS_ASIO does (it calls them in the order of
// their memory addresses, which is not the order of the ini). Prints "all passed" or the failures.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "../common/GuitarTapShared.h"
#include "silence.h"
#include "tap.h"

namespace {

int g_fails = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_fails;
    }
}

struct Input {
    int index;        // N of [Asio.Input.N]
    bool microphone;
    bool streaming;   // false = this input isn't calling (stopped, or not enabled)
};

// A copy of the few lines of GuitarTap::Write that pick the owner, with a clock we control.
struct Tap {
    int owner = -1;  // index of the input feeding the tap, -1 = none yet
    int ownerPriority = 0;
    unsigned lastWrite = 0;

    void Offer(const Input& in, unsigned now) {
        if (owner != in.index) {
            const int priority = nbn::GuitarTapPriority(in.index, in.microphone);
            if (!nbn::GuitarTapTakesOver(owner >= 0, ownerPriority, now - lastWrite >= 1000, priority)) return;
            owner = in.index;
            ownerPriority = priority;
        }
        lastWrite = now;
    }
};

// Runs `ms` of audio callbacks (one every 10 ms), the inputs called in the given order each time.
void Run(Tap& tap, const std::vector<Input>& order, unsigned& now, unsigned ms) {
    for (const unsigned end = now + ms; now < end; now += 10)
        for (const Input& in : order)
            if (in.streaming) tap.Offer(in, now);
}

// A tap as RS_ASIO makes it: a named mapping with a valid header. Closed by the destructor.
struct FakeTap {
    HANDLE map = nullptr;
    nbn::GuitarTapShared* shared = nullptr;
    bool existed = false;  // another program already had a mapping with this name (a game is running)

    FakeTap(const wchar_t* name, uint32_t writerPid)
        : map(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD)sizeof(nbn::GuitarTapShared), name)) {
        existed = map && GetLastError() == ERROR_ALREADY_EXISTS;
        if (!map || existed) return;
        shared = (nbn::GuitarTapShared*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(nbn::GuitarTapShared));
        if (!shared) return;
        shared->h.capacity = nbn::kGuitarTapCapacity;
        shared->h.version = nbn::kGuitarTapVersion;
        shared->h.sampleRate = 48000;
        shared->h.writerPid = writerPid;
        shared->h.magic = nbn::kGuitarTapMagic;
    }
    ~FakeTap() {
        if (shared) UnmapViewOfFile(shared);
        if (map) CloseHandle(map);
    }
    FakeTap(const FakeTap&) = delete;
    FakeTap& operator=(const FakeTap&) = delete;

    // False when Windows gave no mapping (then the test can say so instead of crashing).
    bool Ok() const { return shared != nullptr; }

    // Writes samples the way GuitarTap::Write does: the data first, then the new count.
    void Write(const std::vector<float>& samples) {
        const int64_t pos = shared->h.writePos;
        for (size_t i = 0; i < samples.size(); ++i)
            shared->samples[(uint32_t)(pos + (int64_t)i) & (nbn::kGuitarTapCapacity - 1)] = samples[i];
        InterlockedExchange64(&shared->h.writePos, pos + (int64_t)samples.size());
    }
};

// The mod's side: it opens only the tap of its own process.
void TestReader() {
    const uint32_t me = GetCurrentProcessId();
    wchar_t mine[64];
    nbn::GuitarTapNameFor(me, mine, 64);
    // Nothing there yet (RS_ASIO creates the tap when the guitar input starts).
    {
        nbn::TapReader reader;
        Check(!reader.Open() && !reader.IsOpen(), "no tap yet: the reader stays closed");
        Check(reader.InputName().empty() && reader.InputId() == 0 && reader.SampleRate() == 0, "a closed reader says nothing");
    }
    // This process's tap, written by this process: accepted, and the samples come through.
    {
        FakeTap tap(mine, me);
        Check(tap.Ok(), "the test's own tap could be created");
        if (!tap.Ok()) return;
        tap.shared->h.input = 2;    // [Asio.Input.1]
        tap.shared->h.channel = 1;  // channel 0
        nbn::TapReader reader;
        Check(reader.Open(), "this process's tap is opened");
        Check(reader.SampleRate() == 48000, "its sample rate is read");
        Check(reader.InputName() == "[Asio.Input.1], channel 0", "its input is named");
        std::vector<float> got;
        reader.ReadNew(got);  // the first read starts at the present
        tap.Write({0.25f, -0.5f, 0.75f});
        reader.ReadNew(got);
        Check(got.size() == 3 && got[0] == 0.25f && got[1] == -0.5f && got[2] == 0.75f, "new samples are read, in order");
        got.clear();
        reader.ReadNew(got);
        Check(got.empty(), "nothing new: nothing read");
        const uint32_t guitarId = reader.InputId();
        tap.shared->h.input = nbn::kGuitarTapMicInput;
        const uint32_t micId = reader.InputId();
        Check(micId != guitarId && reader.InputName() == "[Asio.Input.Mic], channel 0", "a change of input shows");
        tap.shared->h.input = 0;
        Check(reader.InputName().empty(), "a tap that doesn't say its input (an older RS_ASIO): no name");
    }
    // A tap under this process's name that another process wrote (cannot happen with our RS_ASIO; the
    // check is what protects against it): refused.
    {
        FakeTap tap(mine, me + 1);
        nbn::TapReader reader;
        Check(!reader.Open(), "a tap written by another process is refused");
    }
    // The old fixed name (0.3.1 and before). Another game's tap: refused. That is the bug this fixes: the
    // mod used to attach to a second game's tap, or to the tap of a game that was still closing.
    {
        FakeTap tap(nbn::kGuitarTapName, me + 1);
        if (tap.existed) {
            std::printf("(a game with an older build is running: the old-name checks are skipped)\n");
        } else if (tap.Ok()) {
            nbn::TapReader reader;
            Check(!reader.Open(), "another game's tap under the old name is refused");
            // An older RS_ASIO build beside the new mod, in this process: accepted.
            tap.shared->h.writerPid = me;
            Check(reader.Open(), "this process's tap under the old name is still opened");
        }
    }
    // A mapping with the right name that isn't a tap (no magic): refused.
    {
        FakeTap tap(mine, me);
        if (!tap.Ok()) return;
        tap.shared->h.magic = 0;
        nbn::TapReader reader;
        Check(!reader.Open(), "a mapping without the tap's mark is refused");
    }
}

// When a wait counts as "no sound from the guitar".
void TestSilence() {
    using Event = nbn::SilenceWatch::Event;
    const float quiet = 0.0001f;  // -80 dB: an input with nothing playing
    const float pluck = 0.05f;    // -26 dB: a note
    nbn::SilenceWatch w;
    Check(w.Update(quiet, 0) == Event::kNone && w.Update(quiet, 5999) == Event::kNone, "under 6 s of silence: nothing yet");
    Check(w.Update(quiet, 6000) == Event::kSilent, "6 s of silence: the message");
    Check(w.Update(quiet, 9000) == Event::kNone && w.told, "it is said once, and stays up");
    Check(w.Update(pluck, 9500) == Event::kHeardAgain && !w.told, "a sound: the message goes");
    Check(w.Update(pluck, 20000) == Event::kNone, "a wait that has heard something is never silent");
    // A wait where the player plays a wrong note at once: never the message, however long it lasts.
    w.Reset();
    Check(w.Update(pluck, 100) == Event::kNone && w.Update(pluck, 60000) == Event::kNone, "a played wait: no message");
    // The next wait starts clean.
    w.Reset();
    Check(w.Update(quiet, 7000) == Event::kSilent, "a new wait can be silent again");
    // Just under and just at the line.
    w.Reset();
    Check(w.Update(nbn::SilenceWatch::kSilentPeak * 0.99f, 7000) == Event::kSilent, "just under the line is silence");
    w.Reset();
    Check(w.Update(nbn::SilenceWatch::kSilentPeak, 7000) == Event::kNone, "at the line is sound");
}

}  // namespace

int main() {
    TestReader();
    TestSilence();
    // Three inputs enabled, like the report that started this (guitar on Input.0): whatever the order
    // RS_ASIO calls them in, the tap carries Input.0.
    {
        std::vector<Input> inputs = {{0, false, true}, {1, false, true}, {2, true, true}};
        std::sort(inputs.begin(), inputs.end(), [](const Input& a, const Input& b) { return a.index < b.index; });
        do {
            Tap tap;
            unsigned now = 5000;
            Run(tap, inputs, now, 3000);
            Check(tap.owner == 0, "three inputs: the tap carries Input.0 in every call order");
        } while (std::next_permutation(inputs.begin(), inputs.end(),
                                       [](const Input& a, const Input& b) { return a.index < b.index; }));
    }
    // Only Input.1 enabled (Input.0 has no driver): it is the guitar.
    {
        Tap tap;
        unsigned now = 5000;
        Run(tap, {{1, false, true}}, now, 2000);
        Check(tap.owner == 1, "only Input.1 enabled: the tap carries it");
    }
    // Input.1 and the microphone: the guitar input, never the microphone, in both orders.
    {
        Tap a, b;
        unsigned now = 5000;
        Run(a, {{1, false, true}, {2, true, true}}, now, 2000);
        now = 5000;
        Run(b, {{2, true, true}, {1, false, true}}, now, 2000);
        Check(a.owner == 1 && b.owner == 1, "a guitar input wins over the microphone input");
    }
    // The microphone input alone (someone playing an acoustic guitar into a microphone): better than nothing.
    {
        Tap tap;
        unsigned now = 5000;
        Run(tap, {{2, true, true}}, now, 2000);
        Check(tap.owner == 2, "only the microphone input: the tap carries it");
    }
    // The preferred input stops: after a second the next one takes over; when it starts again it gets
    // the tap back at once.
    {
        Tap tap;
        unsigned now = 5000;
        std::vector<Input> inputs = {{1, false, true}, {0, false, true}};
        Run(tap, inputs, now, 1000);
        Check(tap.owner == 0, "two guitar inputs: Input.0");
        inputs[1].streaming = false;  // Input.0 stops
        Run(tap, inputs, now, 900);
        Check(tap.owner == 0, "Input.0 quiet for under a second: still the owner");
        Run(tap, inputs, now, 300);
        Check(tap.owner == 1, "Input.0 quiet for over a second: Input.1 takes over");
        inputs[1].streaming = true;
        Run(tap, inputs, now, 20);
        Check(tap.owner == 0, "Input.0 is back: it gets the tap back at once");
    }
    // The name of a process's tap.
    {
        wchar_t name[64];
        nbn::GuitarTapNameFor(1234, name, 64);
        Check(std::wstring(name) == L"Local\\NoteByNote_GuitarInput_1234", "the tap's name ends with the process id");
    }
    // RS_ASIO's name for an input -> its number.
    Check(nbn::GuitarTapInputIndex(L"{ASIO IN 0}") == 0 && nbn::GuitarTapInputIndex(L"{ASIO IN 1}") == 1 &&
              nbn::GuitarTapInputIndex(L"{ASIO IN 2}") == 2,
          "the input's number is read from RS_ASIO's name");
    Check(nbn::GuitarTapInputIndex(nullptr) == 0 && nbn::GuitarTapInputIndex(L"") == 0 &&
              nbn::GuitarTapInputIndex(L"{ASIO Out}") == 0 && nbn::GuitarTapInputIndex(L"{ASIO IN x}") == 0 &&
              nbn::GuitarTapInputIndex(L"{ASIO IN -3}") == 0 && nbn::GuitarTapInputIndex(L"{ASIO IN 99999}") == 0,
          "a name that isn't an input's, or a silly number, gives 0");
    // The header keeps its size: an old and a new DLL side by side must agree on where the samples start.
    Check(sizeof(nbn::GuitarTapHeader) == 64, "the header is 64 bytes");

    if (g_fails == 0) std::printf("all passed\n");
    return g_fails ? 1 : 0;
}
