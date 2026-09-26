// GuitarTap: added to RS_ASIO by the Note-by-Note project (see mod/common/GuitarTapShared.h).
// Copies the raw guitar input into a named shared-memory ring buffer for the Note-by-Note mod.
#pragma once

#include "asio.h"

namespace GuitarTap {

// Called from RSAsioAudioClient::OnAsioBufferSwitch for INPUT clients, with the raw ASIO buffer
// of the first game channel (before software volume). "owner" identifies the audio client: only one
// client feeds the tap at a time. Another one can take over if the owner stops writing for 1 s.
void Write(const void* owner, const void* asioBuffer, ASIOSampleType asioType, unsigned asioSampleSize,
           unsigned numFrames, unsigned sampleRate);

// Loads NoteByNote.dll (from the same folder as RS_ASIO.dll) on a new thread, if the file exists.
// Loading another DLL inside DllMain isn't safe (the loader lock is held), so it's done on a thread,
// which runs as soon as DllMain has returned.
void StartModLoader(HMODULE rsAsioModule);

}  // namespace GuitarTap
