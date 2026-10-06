// test_tap.cpp: which RS_ASIO input feeds the guitar tap (the rule in mod/common/GuitarTapShared.h).
//
// The rule runs inside RS_ASIO's audio callback, where nothing can be tried by hand: here the inputs
// are simulated calling in every possible order, the way RS_ASIO does (it calls them in the order of
// their memory addresses, which is not the order of the ini). Prints "all passed" or the failures.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "../common/GuitarTapShared.h"

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

}  // namespace

int main() {
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
    // The header keeps its size: an old and a new DLL side by side must agree on where the samples start.
    Check(sizeof(nbn::GuitarTapHeader) == 64, "the header is 64 bytes");

    if (g_fails == 0) std::printf("all passed\n");
    return g_fails ? 1 : 0;
}
