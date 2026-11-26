// Ported from the clean tree (modem/clock_skew.hpp, clean master f90aa7d; clean -> dirty is allowed),
// into namespace fmodem (the real-time audio loop uses it for audio_ppm; fix 165).
// clock_skew.hpp: run a station on a sample clock other than the audio's (windowed-sinc interpolation).
// Used by the audio loops (--audio-ppm / AUDIO_PPM: the correction)
// (AUDIO_PPM: the sound card's measured rate error corrected, PROVENANCE P1-24).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace fmodem {

// Test channel: our sample clock slow by P ppm against the peer's (bench: the real reference unit's cycle
// is ~130 ppm short on our clock, E51, worker 8 H2). The station's tick k sits at lockstep time
// k / (1 - P e-6): it ticks a little less often than the peer; what it hears and what it sends are
// interpolated (windowed sinc, +-8 samples) between the two time grids, a fixed delay of 8 samples
// each way (round trip 1.7 ms). P may be negative: the station then ticks a little more often than the
// audio (AUDIO_PPM correction of a slow sound card).
struct ClockSkew {
	static constexpr int L = 1 << 14, H = 8;
	double p = 0;
	std::vector<double> rxh = std::vector<double>(L, 0.0), txh = std::vector<double>(L, 0.0);
	long k = 0;                                  // station ticks done
	// retune() changes p without a jump: both time maps restart from where they are
	// (g0 at tick k0; kb, the station index, at audio sample nb - 2H). With p never changed the maps are
	// the original k / (1 - p) and (n - 2H) (1 - p), bit for bit.
	double g0 = 0, kb = 0; long k0 = 0, nb = 2 * H, nlast = 0;
	void retune(double np)
	{
		g0 += double(k - k0) / (1.0 - p); k0 = k;
		kb += (double(nlast) - double(nb)) * (1.0 - p); nb = nlast;
		p = np;
	}
	// start stepping at audio sample n (a ClockSkew switched in mid-stream, not from 0).
	void start_at(long n) { g0 = double(n); k0 = k; kb = double(k); nb = n + 2 * H; nlast = n; }
	static double interp(const std::vector<double> &h, double pos, long last)
	{
		const long i0 = long(std::floor(pos));
		const double fr = pos - double(i0);
		double y = 0;
		for (int j = -H + 1; j <= H; j++) {
			const long i = i0 + j;
			if (i < 0 || i > last || last - i >= L) continue;
			const double u = double(j) - fr;
			const double sn = std::fabs(u) < 1e-9 ? 1.0 : std::sin(M_PI * u) / (M_PI * u);
			y += h[size_t(i % L)] * sn * (0.5 + 0.5 * std::cos(M_PI * u / (H + 1)));
		}
		return y;
	}
	template <class F> int16_t step(long n, int16_t heard, F &&tick)
	{
		rxh[size_t(n % L)] = heard;
		nlast = n;
		for (;;) {
			const double g = double(k - k0) / (1.0 - p) + g0;   // lockstep time of station tick k
			if (g + H > double(n)) break;
			const double x = interp(rxh, g, n);
			txh[size_t(k % L)] = tick(int16_t(std::max(-32767.0, std::min(32767.0, x))));
			k++;
		}
		const double kp = (double(n) - double(nb)) * (1.0 - p) + kb;  // station index of lockstep time n - 2H
		return int16_t(std::max(-32767.0, std::min(32767.0, interp(txh, kp, k - 1))));
	}
};

}  // namespace fmodem
