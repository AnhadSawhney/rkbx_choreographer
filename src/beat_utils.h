#pragma once
#include <utility>
#include <cstdint>

// Beat conversion utilities
static inline int32_t barBeatToBeatNumber(int bar, int beat) {
    return (bar - 1) * 4 + beat;
}

static inline std::pair<int, int> beatNumberToBarBeat(int32_t beatNumber) {
    int bar = ((beatNumber - 1) / 4) + 1;
    int beat = ((beatNumber - 1) % 4) + 1;
    return {bar, beat};
}

static inline double timeToBeatNumber(double timeSeconds, double bpm) {
    return (timeSeconds * bpm / 60.0) + 1.0; // +1 because music is 1-indexed
}

static inline double beatNumberToTime(double beatNumber, double bpm) {
    return (beatNumber - 1.0) * 60.0 / bpm; // -1 because music is 1-indexed
}