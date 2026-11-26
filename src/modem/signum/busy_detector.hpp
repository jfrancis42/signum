// Channel busy detector: is someone on the channel while we are in standby?
//
// Three tests, every 10 ms; the channel is busy while any of them holds:
//
// 1. A keyed or steady carrier on one of our tones. Each tone is mixed to 0 Hz and summed over
//    10 ms (one 100 Bd symbol), every 2.5 ms. The product of a sum with the conjugate of the one
//    10 ms earlier, raised to the 4th power, has the same phase for a steady tone and for DBPSK or
//    DQPSK keying (steps of 0, 90, 180 or 270 degrees) and a random one for noise. Its magnitude-
//    weighted average over ~0.25 s against the average magnitude (the coherence, 0..1) is ~0.1-0.3
//    on noise and near 1 on a clean P2 control signal, P1 set-up idle or a tuning carrier: busy
//    above kCoherence. A product's weight is capped at kWeightCap times the average, so a static
//    crash does not take the average over.
// 2. Power concentrated on our tones (P1 FSK, P2): 2 x (power in the two 10 ms tone sums) against
//    the power in 300-2700 Hz, averaged over ~0.25 s, is 1 for a signal on the tones alone and
//    about 0.1 on band noise: busy above kToneShare.
// 3. Any signal well above the band noise (P3, other modes, voice): the median 10 ms power in
//    300-2700 Hz over the last 0.25 s against the floor, the lower quartile of the 0.5 s means of
//    the last 60 s: busy kAboveFloorDb or more above it. The median ignores static crashes, the
//    quartile the seconds of lowered noise a receiver's AGC leaves after a strong signal (with the
//    lowest mean as the floor, plain noise read 8-10 dB above it then); a channel busy for most of
//    a minute holds the floor up and is left to tests 1 and 2.
//
// Busy holds for kHangS after the last detection, as the reference unit's busy indication does;
// after a link it shows for kAfterLinkS, and after start-up for the whole hang. The status (what
// the host reads, Listen off) and the lamp (any standby) are kept apart: the status only moves
// when the caller says so. An input under kMinRms (digital silence, a muted or unconnected sound
// card: a few LSB of the 16-bit scale) detects nothing in any test.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>

namespace fmodem {

class BusyDetector {
public:
	static constexpr double kHangS = 3.75;           // busy held after the last detection
	static constexpr double kAfterLinkS = 0.625;     // busy shown on entering standby from a link
	static constexpr double kCoherence = 0.45;
	static constexpr double kToneShare = 0.3;
		static constexpr double kAboveFloorDb = 8.0;
	static constexpr double kMinRms = 4.0;           // 16-bit scale: below this nothing is detected

	explicit BusyDetector(double fs = 9600.0, double f1 = 1400.0, double f2 = 1600.0) : fs_(fs)
	{
		sub_ = std::max(1, int(std::lround(fs * 0.0025)));
		set_tones(f1, f2);
		hp_ = Biquad::make(fs, 300.0, true);
		lp_ = Biquad::make(fs, 2700.0, false);
	}
	void set_tones(double f1, double f2)
	{
		rot_[0] = std::polar(1.0, -2.0 * M_PI * f1 / fs_);
		rot_[1] = std::polar(1.0, -2.0 * M_PI * f2 / fs_);
	}

	// One received sample. Feed only while in standby and not transmitting.
	// status: update the host-visible status (standby, Listen off); the lamp is updated either way.
	void process(float x, bool status = true)
	{
		const double v = x;
		acc_[0] += v * osc_[0]; acc_[1] += v * osc_[1];
		const double b = lp_.run(hp_.run(v));
		pw_acc_ += b * b;
		osc_[0] *= rot_[0]; osc_[1] *= rot_[1];
		if (++renorm_ >= 4096) { renorm_ = 0; osc_[0] /= std::abs(osc_[0]); osc_[1] /= std::abs(osc_[1]); }
		if (++ns_ < sub_) return;
		ns_ = 0;
		hop();
		acc_[0] = acc_[1] = 0; pw_acc_ = 0;
		if (++nh_ < 4) return;
		nh_ = 0;
		decide(status);
	}

	void enter_standby() { hang_status_ = hang_lamp_ = kAfterLinkS; }

	bool busy() const { return busy_status_; }       // the host-visible status and the Traffic LED
	bool busy_lamp() const { return busy_lamp_; }    // the same detector, kept up in any standby (Listen too)
	bool detecting() const { return detect_; }       // the last decision (no hang)
	double coherence(int t) const { return coh_[t & 1]; }
	double tone_share() const { return share_; }
	double above_floor_db() const { return above_db_; }

private:
	static constexpr int kWin = 4;                   // sub-blocks of 2.5 ms in a 10 ms sum
	static constexpr double kAlpha = 1.0 / 100;      // ~0.25 s at 400 updates/s
	static constexpr int kMedBlocks = 25;            // 0.25 s of 10 ms powers
	static constexpr int kFloorBlocks = 50;          // 0.5 s per floor reading
	static constexpr int kFloorReadings = 120;       // 60 s of readings
	static constexpr double kWeightCap = 4.0;

	// Every 2.5 ms: the 10 ms sums, tests 1 and 2.
	void hop()
	{
		sub_z_[0][size_t(si_)] = acc_[0]; sub_z_[1][size_t(si_)] = acc_[1]; sub_p_[size_t(si_)] = pw_acc_;
		si_ = (si_ + 1) % kWin;
		const double n = double(sub_) * kWin;
		double pb = 0;
		for (int k = 0; k < kWin; k++) pb += sub_p_[size_t(k)];
		pb /= n;
		double pt = 0;
		for (int t = 0; t < 2; t++) {
			std::complex<double> z = 0;
			for (int k = 0; k < kWin; k++) z += sub_z_[size_t(t)][size_t(k)];
			z /= n;
			pt += std::norm(z);
			const std::complex<double> d = z * std::conj(lag_[size_t(t)][size_t(li_)]);
			lag_[size_t(t)][size_t(li_)] = z;
			const double m = std::abs(d);
			if (m > 0) {
				const double w = cw_[t] > 0 ? std::min(m, kWeightCap * cw_[t]) : m;
				const std::complex<double> u = d * d * d * d / (m * m * m * m) * w;
				cu_[t] += kAlpha * (u - cu_[t]);
				cw_[t] += kAlpha * (w - cw_[t]);
			} else {
				cu_[t] *= 1 - kAlpha; cw_[t] *= 1 - kAlpha;
			}
			coh_[t] = cw_[t] > 0 ? std::abs(cu_[t]) / cw_[t] : 0;
		}
		li_ = (li_ + 1) % kWin;
		pt_avg_ += kAlpha * (2 * pt - pt_avg_);
		pb_avg_ += kAlpha * (pb - pb_avg_);
		share_ = pb_avg_ > 0 ? pt_avg_ / pb_avg_ : 0;
		blk_p_ += pb;
	}

	// Every 10 ms: test 3, the decision and the hang.
	void decide(bool status)
	{
		const double p = blk_p_ / kWin;
		blk_p_ = 0;
		med_[size_t(mi_)] = p; mi_ = (mi_ + 1) % kMedBlocks; if (med_n_ < kMedBlocks) med_n_++;
		fsum_ += p;
		if (++fn_ >= kFloorBlocks) {
			if (fsum_ / kFloorBlocks >= kMinRms * kMinRms) {   // silence is no floor reading
				floor_r_[size_t(fi_)] = fsum_ / kFloorBlocks; fi_ = (fi_ + 1) % kFloorReadings;
				if (floor_n_ < kFloorReadings) floor_n_++;
			}
			fsum_ = 0; fn_ = 0;
		}
		above_db_ = -99;
		bool loud = false;
		if (floor_n_ > 0 && med_n_ == kMedBlocks) {
			std::array<double, kFloorReadings> q = floor_r_;
			const auto qk = q.begin() + floor_n_ / 4;
			std::nth_element(q.begin(), qk, q.begin() + floor_n_);
			const double fl = *qk;
			std::array<double, kMedBlocks> s = med_;
			std::nth_element(s.begin(), s.begin() + kMedBlocks / 2, s.end());
			const double m = s[kMedBlocks / 2];
			if (m >= kMinRms * kMinRms) { above_db_ = 10 * std::log10(m / fl); loud = above_db_ >= kAboveFloorDb; }
		}
		const bool heard = pb_avg_ >= kMinRms * kMinRms;
		detect_ = (heard && (coh_[0] > kCoherence || coh_[1] > kCoherence || share_ > kToneShare)) || loud;
		const double dt = 0.01;
		step(detect_, dt, hang_lamp_, busy_lamp_);
		if (status) step(detect_, dt, hang_status_, busy_status_);
	}
	static void step(bool det, double dt, double &hang, bool &busy)
	{
		if (det) { hang = 0; busy = true; return; }
		if (hang < kHangS) { hang += dt; busy = true; return; }
		busy = false;
	}

	// Second-order Butterworth high- or low-pass (the band for tests 2 and 3).
	struct Biquad {
		double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
		static Biquad make(double fs, double fc, bool high)
		{
			const double w = 2 * M_PI * fc / fs, c = std::cos(w), al = std::sin(w) / std::sqrt(2.0), a0 = 1 + al;
			Biquad q;
			q.b1 = (high ? -(1 + c) : 1 - c) / a0;
			q.b0 = q.b2 = (high ? (1 + c) : (1 - c)) / 2 / a0;
			q.a1 = -2 * c / a0; q.a2 = (1 - al) / a0;
			return q;
		}
		double run(double x)
		{
			const double y = b0 * x + z1;
			z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y;
			return y;
		}
	};

	double fs_;
	Biquad hp_, lp_;
	int sub_ = 24, ns_ = 0, nh_ = 0, renorm_ = 0;
	std::complex<double> osc_[2] = {{1, 0}, {1, 0}}, rot_[2] = {{1, 0}, {1, 0}};
	std::complex<double> acc_[2] = {0, 0};
	double pw_acc_ = 0;
	std::array<std::array<std::complex<double>, kWin>, 2> sub_z_{}, lag_{};
	std::array<double, kWin> sub_p_{};
	int si_ = 0, li_ = 0;
	std::complex<double> cu_[2] = {0, 0};
	double cw_[2] = {0, 0}, coh_[2] = {0, 0};
	double pt_avg_ = 0, pb_avg_ = 0, share_ = 0, blk_p_ = 0;
	std::array<double, kMedBlocks> med_{};
	int mi_ = 0, med_n_ = 0;
	std::array<double, kFloorReadings> floor_r_{};
	int fi_ = 0, floor_n_ = 0, fn_ = 0;
	double fsum_ = 0, above_db_ = -99;
	bool detect_ = false;
	double hang_status_ = 0, hang_lamp_ = 0;         // 0 at start-up: busy for the whole hang
	bool busy_status_ = false, busy_lamp_ = false;
};

}  // namespace fmodem
