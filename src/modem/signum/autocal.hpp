// autocal.hpp: the station's timing calibration measured by the station itself, so a new
// sound card or radio needs no calibration file.
//
//  1. The sound card's rate error (ppm). The far station's cycle is exact at its end (a crystal). As
//     the link's slave every one of our transmissions is placed from what we hear of the master (our
//     control signals on its packets, our packets on its control signals), so their instants, on our
//     sample clock, lie on the master's cycle lattice as our clock sees it. The lattice period as we
//     see it against the nominal 1.25 s (1.4 s long path; a long cycle is 3 of them) is our rate error.
//     Counted over segments: consecutive transmissions whose spacing is a whole number of cycles
//     within 2 ms (a changeover, a break-in, a slip of the capture or a follow step breaks a segment;
//     only whole segments of >= 5 s count, so their inner jitter cancels and a step never enters;
//     a segment is cut every 10 s, contiguously, so one long stretch still has two halves). A link's
//     reading counts only when it spans >= 40 s and its two halves agree (within 8 ppm, or 25 %),
//     so no single stretch moves the clock. The result is the residual against the correction
//     already applied; the host adds it and applies it between links (ClockSkew::retune).
//  2. The hardware latency (codec + radio round trip, what calibration.conf's hw_latency_s holds).
//     As caller we hear the callee's P1 answers to our call and set-up packets. The reference
//     firmware answers kPeerReply after our first bit with no audio path (measured on the emulated
//     reference unit, every run: 0.995833 s), so an answer at +M s says our round trip plus the far end's audio path is
//     M - kPeerReply. The host takes off what it knows (its software round trip, the ClockSkew
//     filter, a frame constant) and hands the rest to add_hw() as one reading. The frame constant
//     against the real reference unit is 0 (bench, 2026-11-13: M - kPeerReply - software - filter =
//     21.7 / 21.5 / 22.1 ms, the bench file's hw_latency_s 0.022 that every bench link runs on, itself
//     derived the same way): the reference unit's own audio path is part of "hardware" as the file has
//     always held it. The split between our codec + radio and its path is not measured. The far station's
//     radio and the path only add (answers on the air came 13-70 ms apart between far stations),
//     so the estimate is the LOWEST value that at least kConfirm readings confirm (within 1.5 ms),
//     not the mean: a slow far station cannot pull our timing late, and no single reading moves it.
//
// No OS calls, no heap: fixed arrays. The host persists to_text() / from_text() if it has a place.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace fmodem {

struct AutoCal {
	static constexpr double kPeerReply = 0.995833;   // the reference firmware's P1 answer, s after our first bit, no audio path
	static constexpr int kRing = 12, kConfirm = 3;
	static constexpr double kCluster = 0.0015;        // readings within this confirm each other (s)
	// A reading outside [kHwMin, kHwMax] is no reading (also one read back from the state file).
	// kHwMax (2026-11-13, was 0.250): the readings assume the far station answers as the reference
	// unit does. Another Signum (or a station on the same modem code) answers about 63-68 ms later
	// (its replies are timed for the reference unit's own audio path: kRtFromRound): on the ALSA
	// loopback, with no hardware at all, every call read 58.8 ms, three readings confirmed it, and
	// the station applied it (rt_latency +0.037 s): every P3 set-up after that timed out. Such a
	// reading is our hardware plus ~60 ms or more; real hardware read 20-22 ms on the bench (codec,
	// radio and the reference unit's path). So 45 ms: twice the bench's, below any Signum peer's
	// reading. [A judgement: a far station with a radio slower than ~20 ms beyond the bench's is
	// ignored too; the estimate takes the lowest confirmed value anyway.]
	static constexpr double kHwMin = -0.005, kHwMax = 0.045;
	// The bound is the host's to set (signum --autocal-max-ms N, 2026-11-13: 45 ms by default, with an
	// override for a radio path known to be slower than the bench's, e.g. an SDR transceiver).
	double hw_max_ = kHwMax;
	double hw_max() const { return hw_max_; }
	bool set_hw_max_ms(double ms) { if (!(ms > 0 && ms <= 1000)) return false; hw_max_ = ms / 1000.0; return true; }

	// ---- hardware latency ----
	float hw_[kRing] = {};
	int nhw_ = 0, ihw_ = 0;
	bool add_hw(double v)
	{
		if (!(v >= kHwMin && v <= hw_max_)) return false;
		hw_[ihw_] = float(v); ihw_ = (ihw_ + 1) % kRing; nhw_ = std::min(nhw_ + 1, kRing);
		return true;
	}
	void clear_hw() { nhw_ = ihw_ = 0; }
	// The lowest value confirmed by >= kConfirm readings within kCluster: their median.
	bool hw_estimate(double &out) const
	{
		float s[kRing];
		const int n = std::min(nhw_, kRing);
		std::copy(hw_, hw_ + n, s);
		std::sort(s, s + n);
		for (int i = 0; i + kConfirm <= n; i++) {
			int j = i;
			while (j + 1 < n && s[j + 1] - s[i] <= kCluster) j++;
			if (j - i + 1 >= kConfirm) { out = 0.5 * (double(s[i + (j - i) / 2]) + double(s[i + (j - i + 1) / 2])); return true; }
		}
		return false;
	}

	// ---- sound-card rate error ----
	// Two chains, one per role (receiving: our control signals; sending: our packets), each
	// carried across the other role's stretches when its next transmission falls on the lattice
	// predicted from the chain so far (within 2 ms, <= 60 s on): its offset inside the cycle is
	// its own, so only a chain's own transmissions are compared. A chain is cut into contiguous
	// pieces of <= 10 s (nothing lost: the pieces telescope) so a link has two halves.
	struct Seg { double dev = 0, span = 0; };
	static constexpr int kSegs = 160;
	Seg seg_[kSegs];
	int nseg_ = 0;
	struct Chain { bool on = false; int64_t last = 0; double dev = 0, span = 0, cdev = 0, cspan = 0; };
	Chain ch_[2];
	bool last_ptt_ = false;
	double cyc_ = 0;
	double ppm_applied = 0;                           // the correction in place (the host's)
	int ppm_links = 0;                                // readings taken (persisted)
	void close_piece(Chain &c)
	{
		if (c.span > 0 && nseg_ < kSegs) seg_[nseg_++] = Seg{c.dev, c.span};
		c.dev = c.span = 0;
	}
	// The audio lost or gained samples (an xrun, a resync): both chains end here.
	void interrupt() { for (auto &c : ch_) { close_piece(c); c = Chain{}; } }
	void link_reset() { nseg_ = 0; ch_[0] = ch_[1] = Chain{}; last_ptt_ = false; cyc_ = 0; }
	// Per sample while a link is up and we are its slave (any mode): our PTT and whether we receive (IRS).
	void tick_slave(int64_t n, bool ptt, bool irs, double cyc_samples)
	{
		const bool rise = ptt && !last_ptt_;
		last_ptt_ = ptt;
		if (!rise) return;
		if (cyc_samples != cyc_) { interrupt(); cyc_ = cyc_samples; }
		Chain &c = ch_[irs ? 1 : 0];
		if (!c.on) { c.on = true; c.last = n; return; }
		const double d = double(n - c.last), k = std::round(d / cyc_);
		const double rate = c.cspan >= 10.0 * 9600.0 ? c.cdev / c.cspan : 0.0;   // the chain's slope so far
		const double e = d - k * cyc_;
		if (k < 1 || k * cyc_ > 60.0 * 9600.0 || std::fabs(e - rate * k * cyc_) > 0.002 * 9600.0) { close_piece(c); c.last = n; c.cdev = c.cspan = 0; return; }
		c.dev += e; c.span += k * cyc_; c.cdev += e; c.cspan += k * cyc_; c.last = n;
		if (c.span >= 10.0 * 9600.0) close_piece(c);
	}
	// At the link's end: the residual rate error (ppm, + = our clock fast: the master's cycle looks
	// long), or false when the link gave no trustworthy reading. `why` says what it saw.
	bool link_end(double &ppm, char *why, size_t whyn)
	{
		for (auto &c : ch_) close_piece(c);
		double dv = 0, sp = 0;
		int used = 0;
		for (int i = 0; i < nseg_; i++) if (seg_[i].span >= 5.0 * 9600.0) { dv += seg_[i].dev; sp += seg_[i].span; used++; }
		const double total = sp / 9600.0;
		if (used == 0 || total < 40.0) { std::snprintf(why, whyn, "%.0f s of segments, need 40", total); link_reset(); return false; }
		// halves by accumulated span, in time order
		double d1 = 0, s1 = 0, d2 = 0, s2 = 0, acc = 0;
		for (int i = 0; i < nseg_; i++) {
			if (seg_[i].span < 5.0 * 9600.0) continue;
			if (acc + seg_[i].span * 0.5 <= sp * 0.5) { d1 += seg_[i].dev; s1 += seg_[i].span; } else { d2 += seg_[i].dev; s2 += seg_[i].span; }
			acc += seg_[i].span;
		}
		ppm = dv / sp * 1e6;
		const double p1 = s1 > 0 ? d1 / s1 * 1e6 : ppm, p2 = s2 > 0 ? d2 / s2 * 1e6 : ppm;
		const bool agree = s1 > 0 && s2 > 0 && std::fabs(p1 - p2) <= std::max(12.0, 0.25 * std::max(std::fabs(p1), std::fabs(p2)));
		std::snprintf(why, whyn, "%.1f ppm over %.0f s in %d segments, halves %.1f / %.1f", ppm, total, used, p1, p2);
		link_reset();
		if (!agree) return false;
		ppm_links++;
		return true;
	}

	// The correction to apply after a reading r (and whether it changes).
	bool next_ppm(double r, double &out) const
	{
		out = ppm_links <= 1 ? r : ppm_applied + 0.5 * (r - ppm_applied);
		return std::fabs(out - ppm_applied) > 3.0;
	}

	// ---- persistence (the host stores the text) ----
	int to_text(char *buf, size_t n) const
	{
		int k = std::snprintf(buf, n, "audio_ppm_auto = %.2f\nppm_links = %d\n", ppm_applied, ppm_links);
		for (int i = 0; i < nhw_ && k > 0 && size_t(k) < n; i++) k += std::snprintf(buf + k, n - size_t(k), "hw_reading = %.5f\n", double(hw_[(ihw_ - nhw_ + i + kRing) % kRing]));
		return k;
	}
	void from_line(const char *line)
	{
		char key[32]; double v;
		if (std::sscanf(line, " %31[a-z_] = %lf", key, &v) != 2) return;
		if (!std::strcmp(key, "audio_ppm_auto")) ppm_applied = v;
		else if (!std::strcmp(key, "ppm_links")) ppm_links = int(v);
		else if (!std::strcmp(key, "hw_reading")) add_hw(v);
	}
};

}  // namespace fmodem
