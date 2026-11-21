// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dat/sound.h"

// Format 3 waves (docs/sound-format.md): FFmpeg's ATRAC3 decoder on the unscrambled 384-byte frames.
namespace xi::audio
{

// Fills w.pcm from w.atrac3 (16-bit, w.channels interleaved, cut to w.frames). False when it does not decode.
// The frames decode as two independent channels (coding mode 0; joint stereo fails on every file tried, DATA).
bool decodeAtrac3(dat::SeWave& w, bool jointStereo = false);

} // namespace xi::audio
