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

}  // namespace GuitarTap
