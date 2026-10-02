// picking.h: which way to pick each note (down or up), for the tab's pick marks.
//
// The song file has a pick direction per note, but songs almost never set it: in the user's 145
// arrangements (2026-10-02) not one note said "up" (0 = down is just the default). So:
// - the song's own directions are used only when they make sense: some notes say "up", and most agree
//   with the rhythm (below). Otherwise they're ignored;
// - else they're suggested from the rhythm, the way alternate picking is usually taught: the picking
//   hand moves down and up steadily with the beat, so notes on the beat are down strokes and notes in
//   between are up strokes. In a bar with 16th notes the hand moves twice as fast: the "and" between
//   two beats is a down stroke too, and the 16ths around it are up strokes. Notes off that grid
//   (triplets...) alternate with the note before. Chords (strums) the same way.
// Notes that aren't picked get no direction: hammer-ons, pull-offs, taps, and notes linked from the one
// before (a slide's end, a vibrato into a bend...).
// Pure code on the chart (tested by nbn_picking_test).
#pragma once
#include <vector>

#include "chart.h"

namespace nbn::picking {

// Not picked: hammer-on, pull-off, tap, or linked from the note before.
bool Picked(const Target& t);

// Sets Target::pick of one level's notes (in time order) from the rhythm.
void Suggest(const std::vector<Target*>& notes, const std::vector<Beat>& beats);

struct Result {
    int picked = 0;     // picked notes (all levels)
    int songUps = 0;    // of those, the song says "up"
    int agree = 0;      // the song's direction = the rhythm's suggestion
    bool fromSong = false;
};

// Every note of every level: the song's directions if they make sense, else the rhythm's.
// Sets chart->picksFromSong.
Result Assign(Chart* chart);

}  // namespace nbn::picking
