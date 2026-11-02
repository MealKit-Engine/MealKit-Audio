#pragma once

// Offline-render golden tests for events: an event is played alone (2D, fixed seed, its parameters at their defaults) on a mixer without a
// device, and its loudness over time is kept as text. A later render is compared with tolerance, so changing a clip, a curve or a bus
// shows up as a failed check while float noise between compilers/OSes does not. The MealKit Audio tool runs them for a project
// (MealKitAudio --golden --project <folder> [--update]: <project>/tests/audio/<event>.golden).
//
//   @format audio-golden 1
//   event hit
//   rate 48000
//   block 480                 frames per value (10 ms)
//   rms -18.2 -17.9 ...       dB per block (left+right), -90 = silence
//   peak -12.1 -11.8 ...
#include "mkaudio/Events.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mkaudio {

inline constexpr float kGoldenFloorDb = -90.0f;

struct Envelope {
    std::string event;
    uint32_t sampleRate = 48000;
    uint32_t blockFrames = 480;
    std::vector<float> rms, peak; // dB per block
};

struct GoldenOptions {
    uint64_t seed = 1;
    float maxSeconds = 10.0f;  // one-shots stop at their end (plus tail), loops and longer sounds here
    float loopSeconds = 2.0f;  // loops are rendered this long
    float tailSeconds = 0.25f; // after the sound ends (reverb, fades)
};

// Renders one event of a file whose clips are already resolved (resolveClips). false + error if the event is missing or nothing plays
bool renderEnvelope(const EventFile& file, const std::string& event, Envelope& out, std::string& error, const GoldenOptions& options = {});

std::string writeEnvelope(const Envelope& envelope);
bool parseEnvelope(const std::string& text, Envelope& out, std::string& error);

struct GoldenDiff {
    bool same = true;
    std::string reason; // first difference, "" if same
};
// Blocks are compared above floorDb (quieter than that counts as equal); tolerance in dB. A length change beyond lengthTolerance blocks fails
GoldenDiff compareEnvelopes(const Envelope& expected, const Envelope& actual, float toleranceDb = 1.5f, float floorDb = -60.0f,
                            uint32_t lengthTolerance = 3);

} // namespace mkaudio
