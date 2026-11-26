// Streaming two-tone FSK discriminator. Portable C++17, no heap, no FFT:
// suitable for a host build and for the ESP32 (hardware FPU). For a mark/space
// pair centred at fc with shift Hz, each input sample yields a soft value whose
// sign tracks which tone is present (positive toward the higher tone).
//
// Chain: quadrature downmix by fc (NCO) -> FIR low-pass (cutoff ~= shift) ->
// quadrature FM discriminator d = I[n]Q[n-1] - Q[n]I[n-1] (multiply-only;
// proportional to the instantaneous frequency offset from fc).
//
// This replaces the prototype's FFT-Hilbert demod with an equivalent streaming
// form. The protocol's shift polarity inverts each packet; the receiver handles that
// downstream by trying both bit polarities, so this stage needs no AFC sign.
#pragma once

#include <array>
#include <cmath>
#include <cstddef>

namespace fmodem {

template <int MaxTaps = 129>
class FskDiscriminator {
public:
	FskDiscriminator(float sample_rate, float center_hz, float shift_hz)
		: fs_(sample_rate)
	{
		// NCO phase increment for the downmix.
		dphi_ = -2.0f * float(M_PI) * center_hz / fs_;
		// Low-pass cutoff a little above the shift keeps both tones after the
		// downmix (they sit at +-shift/2) while rejecting the sum image.
		design_lowpass(shift_hz * 1.25f);
	}

	// Push one input sample; returns the soft discriminator output.
	float process(float x)
	{
		// Downmix to complex baseband.
		const float c = std::cos(phase_), s = std::sin(phase_);
		phase_ += dphi_;
		if (phase_ < -2.0f * float(M_PI)) phase_ += 2.0f * float(M_PI);
		const float i_in = x * c, q_in = x * s;

		// FIR low-pass on each arm (shared taps and position, separate lines).
		i_bufs_[pos_] = i_in;
		q_bufs_[pos_] = q_in;
		float acc_i = 0.0f, acc_q = 0.0f;
		int idx = pos_;
		for (int k = 0; k < ntaps_; k++) {
			acc_i += taps_[k] * i_bufs_[idx];
			acc_q += taps_[k] * q_bufs_[idx];
			if (--idx < 0) idx = ntaps_ - 1;
		}
		if (++pos_ >= ntaps_) pos_ = 0;

		// Quadrature FM discriminator (multiply-only).
		const float d = acc_i * q_prev_ - acc_q * i_prev_;
		i_prev_ = acc_i;
		q_prev_ = acc_q;
		return d;
	}

	int taps() const { return ntaps_; }

private:
	void design_lowpass(float cutoff_hz)
	{
		ntaps_ = MaxTaps;
		const float fc = cutoff_hz / fs_;            // normalised (cycles/sample)
		const int M = ntaps_ - 1;
		float sum = 0.0f;
		for (int n = 0; n < ntaps_; n++) {
			const float k = n - M / 2.0f;
			// Windowed sinc, Hamming window.
			const float sinc = (k == 0.0f) ? 2.0f * fc
			                               : std::sin(2.0f * float(M_PI) * fc * k) / (float(M_PI) * k);
			const float w = 0.54f - 0.46f * std::cos(2.0f * float(M_PI) * n / M);
			taps_[n] = sinc * w;
			sum += taps_[n];
		}
		for (int n = 0; n < ntaps_; n++) taps_[n] /= sum;   // unity DC gain
	}

	float fs_, dphi_, phase_ = 0.0f;
	int ntaps_ = 0, pos_ = 0;
	std::array<float, MaxTaps> taps_{};
	std::array<float, MaxTaps> i_bufs_{}, q_bufs_{};
	float i_prev_ = 0.0f, q_prev_ = 0.0f;
};

}  // namespace fmodem
