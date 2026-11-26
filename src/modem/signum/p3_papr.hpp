// Clip-and-filter peak reduction for P3 data bursts (2026-10-18).
//
// The firmware's P3 symbols carry some clip-and-filter PAPR reduction
// (the protocol description 4.3); ours were ideal, with ~2.3 dB more peak-to-average
// than the emulated firmware's SL5 bursts. A transmitter's power limit is a
// peak limit, so at the same peak we sent less average power. This clips the
// burst at clip x RMS, removes the clipping products outside the P3 band
// (linear-phase FIR, 300-2700 Hz) and repeats; the result is scaled back to
// the original peak, so the average power rises and the peak does not.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace fmodem::p3 {

// Linear-phase band-pass taps (windowed sinc, Blackman), odd length.
// Computed per call (cheap; workers call this concurrently).
std::vector<float> papr_taps(double fs);

// x: one burst (zeros around it are fine). clip: in RMS multiples (RMS over
// the burst's active samples). peak: the peak to scale the result to (0: the
// input's own peak).
std::vector<float> papr_reduce(const std::vector<float> &x, double fs, float clip, int iterations, double peak = 0);

}  // namespace fmodem::p3
