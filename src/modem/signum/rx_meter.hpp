// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// rx_meter.hpp: what the receiver hears, for the front panel only. Nothing here steers the modem:
// the station feeds every sample in and the panel reads the results.
//
// Received S/N (owner, 2026-11-22). The other station's signal power against the noise, from the
// cycle itself: an ARQ link alternates the other station's bursts (its packets or its control
// signals), our own transmissions (muted here, not measured) and short quiet gaps. The audio is cut
// into 10 ms frames; for each the power in the band 300-2700 Hz (a 4th-order Butterworth high pass
// and low pass) and over the whole audio band. Frames during our own transmission, and for 20 ms
// after it (the radio's receiver coming back, the filters' ringing), are left out. Over the last
// 10.24 s the band powers, smoothed over 30 ms, fall into two groups (two-means in log power): the
// other station's bursts and the quiet gaps. Only frames inside a run of one group count (a quiet
// frame 10 ms, a burst frame 20 ms from the run's ends: no burst edges). Then
//   noise density N0 = mean band power of the quiet frames / the band's noise bandwidth
//   signal S         = mean whole-band power of the burst frames - that of the quiet frames
//   S/N              = S / (N0 * 3000 Hz)    (the usual "S/N in 3 kHz")
// (means trimmed by 10 % at each end). Signal power from the whole band, so the band's edges do not
// cut it; noise from the middle of the band, where a radio's filter is flat. Not measurable ("--")
// when the two groups are not clearly apart (the bimodality index D = gap between the group means
// over their pooled spread below 3.5: no bursts, or a signal below about -5 dB), or with fewer than
// 15 burst frames or 8 quiet frames in the window. Read as -9 at the bottom, 99 at the top (gaps of
// digital silence: no noise to measure). The accuracy against known S/N: docs/md/gui.md.
//
// Sending (owner, 2026-11-22). While we send, the other station answers with control signals right after
// our packet and our next packet follows at once: the window has no gaps to take the noise from (on air,
// 0.2 s of its signal and about 70 ms of near-silence while the radio's receiver comes back, in every
// 1.25 s cycle). So the noise is also kept from the times it can be measured: while idle in standby
// (set_idle: 1 s blocks, the lower quartile of the last 30 of them) and in the gaps of a link while we
// receive (the quiet group above); the newest is held (a noise reference, at most 15 min old: after that
// "--") and the other station's signal is measured against it, from the frames above 12 dB under the
// reference (the radio's silence after our transmission is far below). While we send (more than half of the window
// is our own transmission) quiet frames must be 150 ms after our transmission (the receiver's recovery:
// audio was back 70 ms after it, on air), so that silence never counts as noise; bursts may start 20 ms
// after it. Receiving, the 20 ms stay: our control signals are short and their gaps are the only ones in
// a short cycle (P1). A reading from the held reference is an
// estimate: the radio's AGC lowers the noise while it hears a signal, so the noise in the link can be
// below the held one and the reading too low (never too high from that). noise_ref_age() says how old.
//
// Receive audio level: the peak and the RMS of the input samples (not while we transmit), each
// second, peak held for 2 s, against full scale 32768: clip (a sample at 32000 or more, held 2 s),
// high (peak above -3 dBFS), OK, low (RMS below -60 dBFS). docs/md/gui.md says why these.
//
// No heap, no OS calls: fixed arrays, a few float operations a sample, a sort of at most 1024 values
// every 0.5 s.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace fmodem {

class RxMeter {
public:
	static constexpr int kHist = 1024;            // 10 ms frames in the S/N window
	static constexpr double kBandLo = 300, kBandHi = 2700, kRefHz = 3000;
	static constexpr double kGuardS = 0.02;        // left out after our own transmission
	static constexpr double kRecoverS = 0.15;      // after it, frames do not count as noise (the receiver coming back)
	static constexpr double kRefMaxAgeS = 900;     // the held noise reference is used for 15 min
	static constexpr double kSilenceDb = -12;      // frames this far under the reference: the radio's silence, not a signal
	static constexpr double kSilenceLooseDb = -30; // ... receiving, 20 ms after our transmission: only the digital-silence-like frames
	static constexpr double kMinSnrDb = -5;        // from the reference: a reading below this is "--" (as with gaps)
	static constexpr int kIdleBlock = 100;         // 10 ms frames in an idle block (1 s)
	static constexpr int kIdleBlocks = 30, kIdleMin = 5;
	static constexpr double kMinD = 3.5;           // the bimodality the two groups need
	static constexpr double kMinDSend = 5.0;       // ... while we send with a noise reference held: real gaps, not the control signals' own spread (D 3.6-4.4 on air)
	static constexpr int kMinBurst = 15, kMinQuiet = 8;
	static constexpr float kClip = 32000.0f;       // a sample this far out: the converter is at its limit
	static constexpr double kHighDbfs = -3.0, kLowDbfs = -60.0;
	enum Level { LV_NONE = -1, LV_LOW = 0, LV_OK = 1, LV_HIGH = 2, LV_CLIP = 3 };

	explicit RxMeter(double fs = 9600.0) { init(fs); }

	void init(double fs)
	{
		fs_ = fs;
		frame_ = std::max(1, int(std::lround(fs / 100.0)));
		guard_ = int(std::lround(kGuardS * fs));
		recov_ = int(std::lround(kRecoverS * fs));
		// 4th-order Butterworth: two biquads of Q 0.5412 and 1.3066 each way (RBJ cookbook)
		const double q[2] = {0.54119610, 1.3065630};
		for (int k = 0; k < 2; k++) { bq_[k] = biquad(true, kBandLo, q[k]); bq_[2 + k] = biquad(false, kBandHi, q[k]); }
		// the noise bandwidth of the band: the integral of |H|^2 (one time, here)
		double s = 0;
		const int n = 4800;
		for (int i = 1; i < n; i++) s += gain2(fs_ / 2 * i / n);
		enbw_ = s * (fs_ / 2) / n;
		fc_ = 0; ref_ok_ = false; ref_src_ = 0; idle_ = false; ib_n_ = ib_cnt_ = ib_pos_ = 0; ib_acc_b_ = ib_acc_w_ = 0;
		reset_snr();
		lv_peak_[0] = lv_peak_[1] = -1; lv_rms_ = -1; clip_left_ = 0; blk_ss_ = 0; blk_pk_ = 0; blk_n_ = 0; blk_t_ = 0;
	}
	double enbw_hz() const { return enbw_; }

	// A new link: the S/N window starts empty (the noise reference stays: it is what a sender measures against).
	void reset_snr() { hn_ = 0; snr_ok_ = false; from_ref_ = false; fill_ = 0; acc_b_ = acc_w_ = 0; bad_ = badq_ = mut_any_ = false; since_tx_ = recov_; tick_n_ = 0; }

	// Idle in standby (no call, no link): the noise reference is measured from the audio now. Set by the station.
	void set_idle(bool idle) { idle_ = idle; }

	// Every received sample; muted: we are transmitting (the station's own receive blanking).
	void tick(float x, bool muted)
	{
		// level: the samples we hear, over 1 s blocks
		if (!muted) {
			const float a = std::fabs(x);
			if (a > blk_pk_) blk_pk_ = a;
			blk_ss_ += double(x) * x; blk_n_++;
			if (a >= kClip) clip_left_ = 2 * int(fs_);
		}
		if (clip_left_ > 0) clip_left_--;
		if (++blk_t_ >= int(fs_)) {
			if (blk_n_ >= int(fs_) / 10) {
				lv_peak_[1] = lv_peak_[0]; lv_peak_[0] = blk_pk_;
				lv_rms_ = std::sqrt(blk_ss_ / blk_n_);
			}
			blk_t_ = 0; blk_n_ = 0; blk_ss_ = 0; blk_pk_ = 0;
		}
		// S/N: the band filter runs on every sample (muted ones are zeros), the frames count only clean ones
		float y = muted ? 0.0f : x;
		const float w = y;
		for (auto &b : bq_) y = b.run(y);
		if (muted) { since_tx_ = 0; bad_ = badq_ = mut_any_ = true; }
		else {
			if (since_tx_ < guard_) bad_ = true;
			if (since_tx_ < recov_) { since_tx_++; badq_ = true; }
		}
		acc_b_ += double(y) * y; acc_w_ += double(w) * w;
		if (++fill_ >= frame_) {
			const int i = int(hn_ % kHist);
			fb_[size_t(i)] = float(acc_b_ / frame_); fw_[size_t(i)] = float(acc_w_ / frame_); fv_[size_t(i)] = !bad_; fq_[size_t(i)] = !badq_; fm_[size_t(i)] = mut_any_;
			idle_frame(acc_b_ / frame_, acc_w_ / frame_, !badq_);
			hn_++; fc_++; fill_ = 0; acc_b_ = acc_w_ = 0; bad_ = badq_ = mut_any_ = false;
			if (++tick_n_ >= 50) { tick_n_ = 0; estimate(); }   // every 0.5 s
		}
	}

	// The S/N in dB (in 3 kHz), false when not measurable now.
	bool snr(double &db) const { if (snr_ok_) db = snr_db_; return snr_ok_; }
	// What the last estimate used (tests, debugging): noise and burst frames, band noise power, signal power.
	int last_quiet() const { return nq_; }
	int last_burst() const { return nb_; }
	double last_noise_band() const { return nband_; }
	double last_signal() const { return sig_; }
	double last_d() const { return d_; }
	// The held noise reference (the band power of a frame, as the S/N uses it): its age in seconds, false when there is
	// none. Used up to kRefMaxAgeS. ref_from_gaps: measured in a link's gaps (else idle in standby).
	bool noise_ref_age(double &s) const { if (ref_ok_) s = double(fc_ - ref_at_) * frame_ / fs_; return ref_ok_; }
	bool sending() const { return sending_; }   // the last estimate: more than half the window was our own transmission
	bool ref_from_gaps() const { return ref_ok_ && ref_src_ == 1; }
	double ref_band() const { return ref_b_; }
	// true when the reading now comes from the held reference (the window has no gaps), false: from its own gaps
	bool snr_from_ref() const { return snr_ok_ && from_ref_; }

	int level() const
	{
		if (clip_left_ > 0) return LV_CLIP;
		if (lv_rms_ < 0) return LV_NONE;
		if (peak_dbfs() > kHighDbfs) return LV_HIGH;
		return rms_dbfs() < kLowDbfs ? LV_LOW : LV_OK;
	}
	static double dbfs(double v) { return v > 0 ? 20.0 * std::log10(v / 32768.0) : -120.0; }
	double peak_dbfs() const { return dbfs(std::max(lv_peak_[0], lv_peak_[1])); }
	double rms_dbfs() const { return dbfs(lv_rms_); }

private:
	struct Biquad {
		float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
		float run(float x) { const float y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
	};
	Biquad biquad(bool hp, double f0, double q) const
	{
		const double w0 = 2 * M_PI * f0 / fs_, c = std::cos(w0), al = std::sin(w0) / (2 * q), a0 = 1 + al;
		Biquad b;
		const double k = hp ? (1 + c) / 2 : (1 - c) / 2;
		b.b0 = float(k / a0); b.b1 = float((hp ? -2 * k : 2 * k) / a0); b.b2 = float(k / a0);
		b.a1 = float(-2 * c / a0); b.a2 = float((1 - al) / a0);
		return b;
	}
	double gain2(double f) const
	{
		const double w = 2 * M_PI * f / fs_;
		double g = 1;
		for (const auto &b : bq_) {
			// |B(e^jw)|^2 / |A(e^jw)|^2
			const double cr = std::cos(w), ci = -std::sin(w), c2r = std::cos(2 * w), c2i = -std::sin(2 * w);
			const double nr = b.b0 + b.b1 * cr + b.b2 * c2r, ni = b.b1 * ci + b.b2 * c2i;
			const double dr = 1 + b.a1 * cr + b.a2 * c2r, di = b.a1 * ci + b.a2 * c2i;
			g *= (nr * nr + ni * ni) / (dr * dr + di * di);
		}
		return g;
	}
	// mean of v[0..n) without the lowest and highest 10 % (sorts v)
	static double trimmed(float *v, int n)
	{
		std::sort(v, v + n);
		const int c = n / 10;
		double s = 0;
		for (int i = c; i < n - c; i++) s += v[i];
		return s / (n - 2 * c);
	}
	bool ref_usable() const { return ref_ok_ && ref_b_ > 1e-9 && double(fc_ - ref_at_) * frame_ / fs_ <= kRefMaxAgeS; }
	void set_ref(double b, double w, int src) { ref_b_ = b; ref_w_ = w; ref_at_ = fc_; ref_ok_ = true; ref_src_ = src; }
	// idle in standby: 1 s blocks of frames far from our transmissions; the reference is the block at the lower quartile of
	// the band power among the last 30 (a signal now and then raises a block, never the quartile; 5 blocks at least)
	void idle_frame(double b, double w, bool clean)
	{
		if (!idle_ || !clean) { ib_n_ = 0; ib_acc_b_ = ib_acc_w_ = 0; return; }
		ib_acc_b_ += b; ib_acc_w_ += w;
		if (++ib_n_ < kIdleBlock) return;
		ib_b_[size_t(ib_pos_)] = ib_acc_b_ / ib_n_; ib_w_[size_t(ib_pos_)] = ib_acc_w_ / ib_n_;
		ib_pos_ = (ib_pos_ + 1) % kIdleBlocks; if (ib_cnt_ < kIdleBlocks) ib_cnt_++;
		ib_n_ = 0; ib_acc_b_ = ib_acc_w_ = 0;
		if (ib_cnt_ < kIdleMin) return;
		int idx[kIdleBlocks];
		for (int k = 0; k < ib_cnt_; k++) idx[k] = k;
		std::sort(idx, idx + ib_cnt_, [&](int x, int y) { return ib_b_[size_t(x)] < ib_b_[size_t(y)]; });
		const int q = idx[ib_cnt_ / 4];
		set_ref(ib_b_[size_t(q)], ib_w_[size_t(q)], 0);
	}
	void estimate()
	{
		snr_ok_ = false; from_ref_ = false;
		const int64_t n = std::min<int64_t>(hn_, kHist), s0 = hn_ - n;
		auto at = [&](int64_t k) { return size_t(k % kHist); };
		// sending: more than half of the window's frames hold our own transmission. Then the radio's silence after each
		// transmission is most of what is not a signal; the held reference is what the other station's control signals
		// are measured against (the window's "gaps" would be only the variation of those signals, and must not become
		// the reference), and the gaps count only when they are clearly gaps (D) and not silence.
		int nm = 0;
		for (int64_t k = s0; k < hn_; k++) nm += fm_[at(k)];
		sending_ = 2 * nm > n;
		const bool withref = sending_ && ref_usable();
		// Sending: only the gaps 150 ms or more after our transmission count as noise (the radio's receiver is back: on air
		// its audio was back 70 ms after, and the silence before that is no noise). Receiving: 20 ms after it, as before
		// (control signals of ours are short, and in a short cycle, P1, those gaps are all there are); silence still never
		// counts when a reference says what noise there is.
		const int m = smooth(s0, sending_, withref ? kSilenceDb : ref_usable() ? kSilenceLooseDb : NAN);
		if (withref) estimate_ref(s0);
		else if (!estimate_gaps(m, s0, kMinD)) estimate_ref(s0);
	}
	// The band power smoothed over 3 frames (all 3 clean), in log, into sm_; qs_: all 3 may count as noise (strict: also
	// 150 ms after our transmission; silence_db: also above that many dB under the reference). Only those go into the grouping
	// (tmp_); the others may still be bursts. silence_db: NAN none. Returns how many.
	int smooth(int64_t s0, bool strict, double silence_db)
	{
		auto at = [&](int64_t k) { return size_t(k % kHist); };
		const bool silence = !std::isnan(silence_db);
		const float lfloor = silence ? float(std::log(ref_b_ * std::pow(10.0, silence_db / 10.0))) : 0.0f;
		int m = 0;
		for (int64_t k = s0; k < hn_; k++) {
			float &o = sm_[size_t(k - s0)];
			o = NAN;
			qs_[size_t(k - s0)] = false;
			if (k < s0 + 1 || k > hn_ - 2) continue;
			bool ok = true, q = true;
			double sum = 0;
			for (int64_t j = k - 1; j <= k + 1 && ok; j++) { ok = fv_[at(j)]; q = q && (!strict || fq_[at(j)]); sum += fb_[at(j)]; }
			if (!ok) continue;
			o = float(std::log(std::max(sum / 3, 1e-6)));   // (digital silence: a floor, not -inf)
			if (silence && o <= lfloor) q = false;           // silence (the radio, a muted input) is not noise
			qs_[size_t(k - s0)] = q;
			if (q) tmp_[size_t(m++)] = o;
		}
		return m;
	}
	// Two-means in log power on v[0..m) (reordered), from the 10th and 90th percentiles: the threshold between the two groups
	// and the bimodality D (gap between the group means over their pooled spread). false: one group only.
	static bool split(float *v, int m, double &thr, double &d)
	{
		std::nth_element(v, v + m / 10, v + m);
		double lo = v[m / 10];
		std::nth_element(v, v + (9 * m) / 10, v + m);
		double hi = v[(9 * m) / 10];
		double vlo = 0, vhi = 0;
		int clo = 0, chi = 0;
		for (int it = 0; it < 12; it++) {
			thr = (lo + hi) / 2;
			double sl = 0, sh = 0, ql = 0, qh = 0;
			clo = chi = 0;
			for (int i = 0; i < m; i++) {
				const double x = v[i];
				if (x > thr) { sh += x; qh += x * x; chi++; } else { sl += x; ql += x * x; clo++; }
			}
			if (!clo || !chi) return false;
			lo = sl / clo; hi = sh / chi;
			vlo = std::max(0.0, ql / clo - lo * lo); vhi = std::max(0.0, qh / chi - hi * hi);
		}
		const double pooled = std::sqrt((vlo * clo + vhi * chi) / (clo + chi));
		d = pooled > 0 ? (hi - lo) / pooled : 99;
		return true;
	}
	// From the window's own gaps: the two groups. false: not measurable that way.
	bool estimate_gaps(int m, int64_t s0, double min_d)
	{
		auto at = [&](int64_t k) { return size_t(k % kHist); };
		nq_ = nb_ = 0;
		if (m < kMinBurst + kMinQuiet) return false;
		double thr = 0;
		if (!split(tmp_.data(), m, thr, d_)) return false;
		if (d_ < min_d) return false;
		// frames inside a run of one group: quiet when its own smoothed value and the one each side are below the
		// threshold (the gaps are short: 100 ms in P2); a burst frame needs the two each side above too (no edges).
		// A frame in our transmission's recovery is a burst when above the threshold, else left out (never quiet).
		int nq = 0, nb = 0;
		float *qb = tmp_.data(), *qw = tmp2_.data(), *bw = tmp3_.data(), *fb = tmp4_.data(), *fw = tmp5_.data();
		int nf = 0;
		auto cls = [&](int64_t j) {
			if (j < s0 || j >= hn_) return -1;
			const float v = sm_[size_t(j - s0)];
			if (std::isnan(v)) return -1;
			return v > thr ? 1 : qs_[size_t(j - s0)] ? 0 : -1;
		};
		for (int64_t k = s0 + 1; k < hn_ - 1; k++) {
			const int c = cls(k);
			if (c < 0 || cls(k - 1) != c || cls(k + 1) != c) continue;
			if (c == 0) {
				qb[nq] = fb_[at(k)]; qw[nq] = fw_[at(k)]; nq++;
				if (fq_[at(k - 1)] && fq_[at(k)] && fq_[at(k + 1)]) { fb[nf] = fb_[at(k)]; fw[nf] = fw_[at(k)]; nf++; }
			}
			else if (cls(k - 2) == 1 && cls(k + 2) == 1) bw[nb++] = fw_[at(k)];
		}
		nq_ = nq; nb_ = nb;
		if (nq < kMinQuiet || nb < kMinBurst) return false;
		const double nband = trimmed(qb, nq), nwide = trimmed(qw, nq), swide = trimmed(bw, nb);
		const double sig = swide - nwide, n0 = nband / enbw_;
		nband_ = nband; sig_ = sig;
		// no noise at all in the gaps (digital silence: a test link, a sound card's input muted between bursts): 99,
		// "above the range"
		snr_db_ = n0 <= 1e-9 ? 99.0 : sig > 0 ? std::min(99.0, std::max(-9.0, 10.0 * std::log10(sig / (n0 * kRefHz)))) : -9.0;
		snr_ok_ = true;
		// the link's own noise is the newest reference: from the quiet frames 150 ms or more after our transmission only
		if (!idle_ && !sending_ && nf >= kMinQuiet) set_ref(trimmed(fb, nf) , trimmed(fw, nf), 1);
		return true;
	}
	// From the held noise reference (no gaps in the window: we send, the other station's control signals fill the time
	// between our packets): the frames not in the radio's silence (above kSilenceDb under the reference, five in a row: no
	// edges) are the other station's signal over noise.
	void estimate_ref(int64_t s0)
	{
		if (!ref_usable()) return;
		const float lfloor = float(std::log(ref_b_ * std::pow(10.0, kSilenceDb / 10.0)));
		auto up = [&](int64_t j) { if (j < s0 || j >= hn_) return false; const float v = sm_[size_t(j - s0)]; return !std::isnan(v) && v > lfloor; };
		// candidate frames: above the silence floor, five in a row. In a window that also has real gaps (noise-only frames:
		// the reference unit's cycle, a test link) the candidates split in two groups: then only the upper one is signal.
		int nc = 0;
		for (int64_t k = s0 + 2; k < hn_ - 2; k++)
			if (up(k - 2) && up(k - 1) && up(k) && up(k + 1) && up(k + 2)) tmp_[size_t(nc++)] = sm_[size_t(k - s0)];
		double thr = -1e30, dd = 0;
		float lthr = -1e30f;
		if (nc >= kMinBurst + kMinQuiet && split(tmp_.data(), nc, thr, dd) && dd >= kMinDSend) lthr = float(thr);
		int nb = 0;
		float *bw = tmp3_.data();
		for (int64_t k = s0 + 2; k < hn_ - 2; k++)
			if (up(k - 2) && up(k - 1) && up(k) && up(k + 1) && up(k + 2) && sm_[size_t(k - s0)] > lthr) bw[nb++] = fw_[size_t(k % kHist)];
		nq_ = 0; nb_ = nb;
		if (nb < kMinBurst) return;
		const double sig = trimmed(bw, nb) - ref_w_, n0 = ref_b_ / enbw_;
		nband_ = ref_b_; sig_ = sig;
		if (!(sig > 0) || 10.0 * std::log10(sig / (n0 * kRefHz)) < kMinSnrDb) return;
		snr_db_ = std::min(99.0, 10.0 * std::log10(sig / (n0 * kRefHz)));
		snr_ok_ = from_ref_ = true;
	}

	double fs_ = 9600, enbw_ = 2400;
	int frame_ = 96, guard_ = 192, recov_ = 1440;
	std::array<Biquad, 4> bq_{};
	// S/N
	std::array<float, kHist> fb_{}, fw_{}, sm_{}, tmp_{}, tmp2_{}, tmp3_{}, tmp4_{}, tmp5_{};
	std::array<bool, kHist> fv_{}, fq_{}, qs_{}, fm_{};
	int64_t hn_ = 0;
	int fill_ = 0, since_tx_ = 0, tick_n_ = 0;
	double acc_b_ = 0, acc_w_ = 0;
	bool bad_ = false, badq_ = false, mut_any_ = false, snr_ok_ = false, from_ref_ = false;
	// the held noise reference and the idle blocks it comes from
	int64_t fc_ = 0, ref_at_ = 0;                  // frames since init (never reset), and when the reference was measured
	double ref_b_ = 0, ref_w_ = 0;
	bool ref_ok_ = false, idle_ = false, sending_ = false;
	int ref_src_ = 0, ib_n_ = 0, ib_cnt_ = 0, ib_pos_ = 0;
	double ib_acc_b_ = 0, ib_acc_w_ = 0;
	std::array<double, kIdleBlocks> ib_b_{}, ib_w_{};
	double snr_db_ = 0, nband_ = 0, sig_ = 0, d_ = 0;
	int nq_ = 0, nb_ = 0;
	// level
	float blk_pk_ = 0, lv_peak_[2] = {-1, -1};
	double blk_ss_ = 0, lv_rms_ = -1;
	int blk_n_ = 0, blk_t_ = 0, clip_left_ = 0;
};

}   // namespace fmodem
