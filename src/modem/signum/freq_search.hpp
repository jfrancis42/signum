// Frequency offset of a received PSK burst from its power spectrum, without decoding it
// (2026-11-14). C++17, no OS calls, no heap: fixed arrays on the caller's stack.
//
// The decoders take the peer's offset as given and remove only a small residual (P3: +-2 steps of
// 1/16 turn per pulse, +-12.5 Hz), so after a sudden step (an operator retuning, a VFO jump) nothing
// decodes and nothing measures the new offset. The burst's power spectrum still shows where its
// carriers are: each carrier's spectrum is the pulse's |G(f)|^2 (random phase steps), so the
// expected spectrum is a known comb. The offset is the shift of that comb that best matches the
// received spectrum (Welch average, Hann, 2048 points; correlation on a 0.5 Hz grid, parabolic
// refinement).
//
// P3 data carriers lie 120 Hz apart and the flat-top pulse's spectrum is down to 0.2 at +-60 Hz, so
// the comb is sharp but repeats every 120 Hz: the search is kept to +-70 Hz around a centre, and
// the band edges (the outer carriers) decide between the comb's repeats where the level uses them.
// The two-carrier control signals (840 Hz apart, wide pulse) have no such repeat.
//
// `match` compares the whole spectrum with the comb, envelope and all. On a radio's audio the noise
// itself has an envelope (the receive filter's pass band, a few hundred Hz to ~2.5-3 kHz), and the
// 18-carrier comb spans almost the same band: band-limited noise alone matches it at 0.45-0.65
// (2026-11-18, fix 261), so a failed packet with nothing readable in it looked like a burst.
// `comb` is the same correlation with both envelopes removed (each less its own average over one
// comb period, 120 Hz): only the carriers' structure counts, and band-limited noise of any shape
// gives under ~0.27 (white or band-limited, tilted or not), a P3 burst of any level 0.45-0.65 at
// 10 dB.
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>

namespace fmodem::fsearch {

constexpr int kN = 2048;                 // FFT length
constexpr int kMaxW = 420;               // pulse spectrum kept to +-kMaxW Hz

struct Result {
	bool ok = false;
	double hz = 0;        // the burst's offset from the nominal carriers, Hz as received (+ = higher)
	double match = 0;     // correlation coefficient of the spectrum with the comb at hz (white noise alone: < ~0.2)
	double comb = 0;      // the same with both envelopes removed (fix 261: band-limited noise alone < ~0.27)
	double snr_db = 0;    // the burst's in-band power over the out-of-band power
};

void fft(std::complex<double> *a, int n);

// |G(f)|^2 of a symmetric pulse given as one side at 9600 Hz (index 0 = centre), on a 1 Hz grid
// 0..kMaxW, normalised to 1 at 0.
struct PulsePower {
	double p[kMaxW + 2] = {};
	PulsePower(const float *half, int nhalf)
	{
		for (int f = 0; f <= kMaxW + 1; f++) {
			double g = half[0];
			for (int i = 1; i <= nhalf; i++) g += 2.0 * half[i] * std::cos(2 * M_PI * f * i / 9600.0);
			p[f] = g * g;
		}
		const double p0 = p[0] > 0 ? p[0] : 1.0;
		for (double &v : p) v /= p0;
	}
	double at(double f) const
	{
		f = std::fabs(f);
		if (f >= kMaxW) return 0.0;
		const int i = int(f);
		const double u = f - i;
		return p[i] * (1 - u) + p[i + 1] * u;
	}
};

// x[0..n): the burst (with a little noise either side, it does not matter); carriers: the nominal
// carrier frequencies (Hz), nc of them, with weights (the share of pulses each is on; null: 1); pw: the pulse's power spectrum; centre +- range: where to
// look for the offset (range at most ~70 Hz for the 120 Hz P3 comb).
Result burst_offset(const float *x, long n, double fs, const double *carriers, const double *weights, int nc, const PulsePower &pw,
                           double centre, double range);

}  // namespace fmodem::fsearch
