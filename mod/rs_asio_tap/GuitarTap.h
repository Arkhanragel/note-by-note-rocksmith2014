// GuitarTap: added to RS_ASIO by the Note-by-Note project (see mod/common/GuitarTapShared.h).
// Copies the raw guitar input into a named shared-memory ring buffer for the Note-by-Note mod.
#pragma once

#include "asio.h"

namespace GuitarTap {

// Called from RSAsioAudioClient::OnAsioBufferSwitch for every INPUT client, with the raw ASIO buffer
// of its first game channel (before software volume).
//   owner       identifies the audio client
//   deviceId    RS_ASIO's name for the input, "{ASIO IN 0}": the number is N of [Asio.Input.N]
//   microphone  the [Asio.Input.Mic] input
//   channel     the ASIO channel the input reads
// Only ONE input feeds the tap: the enabled guitar input with the lowest number (the one the game
// listens to), see GuitarTapTakesOver in GuitarTapShared.h. `TapInput=N` in NoteByNote.ini forces
// [Asio.Input.N] instead (2 = the microphone input).
void Write(const void* owner, const wchar_t* deviceId, bool microphone, unsigned channel, const void* asioBuffer,
           ASIOSampleType asioType, unsigned asioSampleSize, unsigned numFrames, unsigned sampleRate);

// Loads NoteByNote.dll (from the same folder as RS_ASIO.dll) on a new thread, if the file exists.
// Loading another DLL inside DllMain isn't safe (the loader lock is held), so it's done on a thread,
// which runs as soon as DllMain has returned.
void StartModLoader(HMODULE rsAsioModule);

}  // namespace GuitarTap
