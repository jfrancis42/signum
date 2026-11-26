// 48 kHz <-> 9600 Hz (ratio 5) polyphase FIR resampler, C++17, no heap.
//
// Sound cards run at 48 kHz natively; the modem runs at 9600 Hz. ALSA's plug
// layer would convert with linear interpolation (passband tilt, aliasing),
// which PSK and P3 multitone signals do not survive well. This
// windowed-sinc filter is flat to ~3.6 kHz and > 70 dB down above 4.8 kHz,
// with a constant group delay (half the filter length).
#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace fmodem {

class Resample5 {
public:
	static constexpr int kTaps = 160;                  // 32 per phase
	Resample5()
	{
		// Low-pass at 4.0 kHz for 48 kHz input, Blackman window, DC gain 1.
		const double fc = 4000.0 / 48000.0;
		double sum = 0;
		for (int n = 0; n < kTaps; n++) {
			const double k = n - (kTaps - 1) / 2.0;
			const double sinc = k == 0 ? 2 * fc : std::sin(2 * M_PI * fc * k) / (M_PI * k);
			const double w = 0.42 - 0.5 * std::cos(2 * M_PI * n / (kTaps - 1)) + 0.08 * std::cos(4 * M_PI * n / (kTaps - 1));
			h_[n] = sinc * w;
			sum += h_[n];
		}
		for (auto &v : h_) v /= sum;
	}

	// Decimate: 5 input samples at 48 kHz -> 1 output sample at 9600 Hz.
	// (Each sample is stored twice, kTaps apart, so the dot product runs over
	// contiguous memory without a modulo per tap; same order, same result.)
	float down(const int16_t *in5)
	{
		for (int i = 0; i < 5; i++) { dbuf_[dpos_] = dbuf_[dpos_ + kTaps] = in5[i]; dpos_ = dpos_ + 1 == kTaps ? 0 : dpos_ + 1; }
		double acc = 0;
		const float *b = &dbuf_[size_t(dpos_)];
		for (int n = 0; n < kTaps; n++) acc += h_[n] * b[n];
		return float(acc);
	}

	// Interpolate: 1 input sample at 9600 Hz -> 5 output samples at 48 kHz.
	void up(float x, int16_t *out5)
	{
		constexpr int kU = kTaps / 5;
		ubuf_[upos_] = ubuf_[upos_ + kU] = x;
		upos_ = upos_ + 1 == kU ? 0 : upos_ + 1;
		const float *b = &ubuf_[size_t(upos_)];
		for (int p = 0; p < 5; p++) {
			double acc = 0;
			// Taps of phase p: h[p], h[p+5], ... (zero-stuffed input, gain 5).
			for (int m = kU - 1, k = 0; m >= 0; m--, k++) acc += h_[p + 5 * m] * b[k];
			const double v = 5.0 * acc;
			out5[p] = int16_t(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
		}
	}

private:
	std::array<double, kTaps> h_{};
	std::array<float, 2 * kTaps> dbuf_{};
	std::array<float, 2 * (kTaps / 5)> ubuf_{};
	int dpos_ = 0, upos_ = 0;
};

}  // namespace fmodem
