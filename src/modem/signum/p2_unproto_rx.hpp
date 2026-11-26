// p2_unproto_rx.hpp: P2 unproto (short cycle, levels 1-4) reception,
// from a bench tool's receiver: one packet decoded from the audio
// around a burst (decode_burst), and a streaming receiver for listen mode
// (UnprotoRx) that finds bursts by their energy and prints each new packet
// once. Checked against the firmware's unproto modes.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <complex>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "compress.hpp"
#include "fsk_discriminator.hpp"
#include "p1_frame.hpp"
#include "p1_huffman.hpp"
#include "p2_codec.hpp"
#include "p3_unproto_rx.hpp"

namespace fmodem {

struct UnprotoPacket { bool ok = false; int level = 0, counter = 0, metric = 0; uint8_t status = 0; std::string text; };
// One timing / carrier-order candidate of a burst: its raw phase steps (1/16 turn), header and data.
struct UnprotoCand { double hs[8][2], steps[72][2]; };

// Header fit and decode of one candidate's steps.
UnprotoPacket unproto_decode_cand(const UnprotoCand &c);

// Two failed copies of one packet combined (fix 148): each step the mean of the two copies'
// unit phasors, over every pairing of their candidates (the repeat has its carriers swapped, so
// its right carrier order is the other one). The first pairing that decodes.
UnprotoPacket unproto_combine(const std::vector<UnprotoCand> &a, const std::vector<UnprotoCand> &b);

// Fix 248: a burst's timing from the packets before it. pred: lo / hi are where each carrier's pulse
// at the decode's alignment point (ls_pulses' start + skip pulses) is expected (indices into x); got:
// where they were in a packet that decoded.
struct UnprotoTiming { bool pred = false, swap = false, b0_start = false; double lo = 0, hi = 0; bool got = false; double out_lo = 0, out_hi = 0; bool by_pred = false; };

// The packet whose energy starts near x[b0] (x at fs; at least 0.95 s after b0).
UnprotoPacket unproto_decode_burst(const std::vector<float> &x, int fs, size_t b0, float lo, float hi, std::vector<UnprotoCand> *cands = nullptr, UnprotoTiming *tm = nullptr);

// Burst starts in a stream, for the unproto receivers. The energy is taken in the band of the
// two carriers (mixed down to their centre, two one-pole low-passes at 400 Hz) over 5 ms, then
// smoothed over 15 ms; the floor is the lowest smoothed value of the last 3 s. A start is a
// rise above 3 x the floor after at least 25 ms below it: unproto bursts are separated by
// 46 ms (short cycle) or 66 ms (long cycle) of silence, while the envelope of a PSK burst dips
// for shorter times. (The first version took any rise above 6 x a falling minimum of the
// broadband energy: with noise those dips inside a burst counted as starts, and no packet
// landed on the grid; 2026-10-22, nothing decoded at noise 300 RMS.)
class BurstFinder {
public:
	BurstFinder(int fs, float lo, float hi) : fs_(fs), h_(fs / 200) { set_tones(lo, hi); hist_.assign(size_t(3 * 200), 1e18); }
	void set_tones(float lo, float hi)
	{
		w_ = std::polar(1.0, -2 * M_PI * 0.5 * (lo + hi) / fs_);
		a_ = 1.0 - std::exp(-2 * M_PI * 400.0 / fs_);
	}
	// true when a burst starts at this sample (start(): where, about 5 ms early)
	bool tick(int16_t v)
	{
		ph_ *= w_;
		if (++renorm_ == 4096) { ph_ /= std::abs(ph_); renorm_ = 0; }
		const std::complex<double> x = double(v) * ph_;
		y1_ += a_ * (x - y1_); y2_ += a_ * (y1_ - y2_);
		acc_ += std::norm(y2_); n_++;
		if (++k_ < h_) return false;
		const double e = acc_ / h_;
		acc_ = 0; k_ = 0;
		last_e_ = e; new_e_ = true;
		sm_[si_++ % 3] = e;
		const double s = std::sqrt((sm_[0] + sm_[1] + sm_[2]) / 3);
		hist_[hi_++ % hist_.size()] = s;
		double fl = 1e18; for (double q : hist_) fl = std::min(fl, q);
		const bool on = s > std::max(3.0 * fl, 100.0);
		bool st = false;
		if (on && off_run_ >= 5) { st = true; start_ = n_ > uint64_t(4 * h_) ? n_ - uint64_t(4 * h_) : 0; }
		off_run_ = on ? 0 : off_run_ + 1;
		return st;
	}
	uint64_t start() const { return start_; }
	// fix 248: the band energy of the 5 ms block that just ended (true once per block)
	bool block(double &e) { if (!new_e_) return false; new_e_ = false; e = last_e_; return true; }

private:
	int fs_, h_;
	std::complex<double> w_{1, 0}, ph_{1, 0}, y1_{0, 0}, y2_{0, 0};
	double a_ = 0, acc_ = 0, sm_[3] = {0, 0, 0}, last_e_ = 0;
	bool new_e_ = false;
	int k_ = 0, renorm_ = 0, off_run_ = 0;
	uint64_t n_ = 0, si_ = 0, hi_ = 0, start_ = 0;
	std::vector<double> hist_;
};

// Listen mode for unproto: bursts found by BurstFinder; a
// burst is decoded once it is all in. Taken only on the unproto grid: a decoded packet
// counts when another burst started 0.8875 s (+-30 ms) before or after it (an ARQ link's
// packets come 1.25 s apart and its decoder prints them already). Each new counter's text
// once (a repetition has the counter of the copy before it).
class UnprotoRx {
public:
	explicit UnprotoRx(int fs = 9600, float lo = 1400, float hi = 1600) : fs_(fs), lo_(lo), hi_(hi), bf_(fs, lo, hi) {}
	void set_tones(float lo, float hi) { lo_ = lo; hi_ = hi; bf_.set_tones(lo, hi); }
	void tick(int16_t v)
	{
		buf_.push_back(float(v)); n_++;
		if (bf_.tick(v) && (starts_.empty() || bf_.start() - starts_.back() > uint64_t(0.5 * fs_))) starts_.push_back(bf_.start());
		// a burst all in (0.95 s): decode it
		while (!starts_.empty() && starts_.front() + uint64_t(0.95 * fs_) <= n_ && next_ < starts_.size()) {
			const uint64_t b = starts_[next_];
			if (b + uint64_t(0.95 * fs_) > n_) break;
			if (!off_grid(b)) decode_at(b);                // fix 239: with a grid, starts off it are noise
			next_++;
		}
		comb_tick();
		// Fix 239: once packets are taken, every slot of their 0.8875 s grid is decoded too, where
		// the burst finder found no start near it: in noise its starts miss the bursts (emulated
		// reference unit U 3 at -3 dB, p2-fec3-snr-3-it-send: starts 0.1-0.4 s late, 3 packets in 30 s).
		if (slot_ && n_ >= slot_ + uint64_t(0.95 * fs_)) {
			if (n_ - last_ok_ > uint64_t(30 * fs_) || std::getenv("UNPROTO_NO_SLOTS")) slot_ = 0;   // the grid is lost
			else {
				bool near = false;
				for (uint64_t s : starts_) if ((s > slot_ ? s - slot_ : slot_ - s) < uint64_t(0.04 * fs_)) near = true;
				const uint64_t s0 = slot_;
				slot_ += uint64_t(0.8875 * fs_ + 0.5);         // (before: a packet that decodes moves the grid on from itself)
				if (!near) decode_at(s0);
			}
		}
		// keep ~4 s of audio and the burst list near it
		const size_t keep = size_t(9 * fs_);           // (fix 239: 9 s, for the slots before a first packet)
		if (buf_.size() > keep + size_t(fs_)) { const size_t drop = buf_.size() - keep; buf_.erase(buf_.begin(), buf_.begin() + long(drop)); base_ += drop; }
		while (starts_.size() > 8 && next_ > 4) { starts_.pop_front(); next_--; }
	}
	// New text since the last call ("" when none).
	std::string take() { std::string s; s.swap(text_); return s; }

private:
	struct Dec { uint64_t at; UnprotoPacket p; bool done = false; };
	int fs_; float lo_, hi_;
	BurstFinder bf_;
	std::vector<float> buf_;
	uint64_t n_ = 0, base_ = 0;
	std::deque<uint64_t> starts_;
	size_t next_ = 0;
	std::deque<Dec> dec_;
	int last_counter_ = -1;
	std::string text_;
	text::RleExpander rle_;
	std::vector<UnprotoCand> fail_cands_;              // the last burst that did not decode (fix 148)
	uint64_t fail_at_ = 0;
	uint64_t last_at_ = 0;
	std::string last_text_;
	uint64_t slot_ = 0, last_ok_ = 0;                  // fix 239: the next grid slot to decode (0: no grid yet)
	// Fix 248: the pulse timing of the last packet that decoded (stream samples: each carrier's pulse at
	// the alignment point; tref_b_ the grid slot it stands for). The burst's own timing (the first
	// pulse above 0.4 of the strongest, independently per carrier) is found badly in noise: the
	// reference unit U 3 at -3 dB (p2-fec3-snr-3-it-send), CHAN_SEED 1-8: 30 / 55 / 0 / 128 / 0 / 0 / 23 / 0
	// characters exact where the reference unit pair printed 60 / 128 / 128 / 128 / 128 / 128 / 128 / 128. As the
	// ARQ receiver's timing track (with the true timing P2 level 1 decodes ~99 %
	// of first copies where per-packet timing gets 14 %), each burst is decoded first at the timing
	// carried from the last good packet (both carrier orders, +-1 ms), then as before.
	// UNPROTO_TRACK_OLD=1: off.
	bool tref_ok_ = false, backfill_ = false;
	double tref_lo_ = 0, tref_hi_ = 0, tref_b_ = 0;
	static bool silence_old() { static const bool v = std::getenv("LISTEN_SILENCE_OLD") != nullptr; return v; }
	static bool track_old() { static const bool v = std::getenv("UNPROTO_TRACK_OLD") != nullptr; return v; }
	static bool grid(uint64_t a, uint64_t b, int fs) { const double d = std::fabs(double(a) - double(b)) / fs; return std::fabs(d - 0.8875) < 0.03; }
	bool off_grid(uint64_t b) const
	{
		if (!slot_ || std::getenv("UNPROTO_NO_SLOTS")) return false;
		const double P = 0.8875 * fs_, r = std::fmod(std::fabs(double(b) - double(slot_)), P);
		return std::min(r, P - r) > 0.04 * fs_;
	}
	// fix 239: a whole number (1-12) of periods apart: a taken packet's neighbours may be lost in noise
	static bool gridk(uint64_t a, uint64_t b, int fs)
	{
		const double d = std::fabs(double(a) - double(b)) / fs, k = std::round(d / 0.8875);
		return k >= 1 && k <= 12 && std::fabs(d - k * 0.8875) < 0.03;
	}
	// Fix 248: before any packet decodes, the grid from the band energy folded on the period. The burst
	// finder's starts are single rises above 3 x the floor; at -3 dB they miss the bursts (fix 239's
	// note: 0.1-0.4 s late) and nothing decodes until one lands near a start by chance
	// (p2-fec3-snr-3-it-send CHAN_SEED 6 / 8: the first packet at 39.5 / 31.6 s, the reference unit pair from the
	// first). The 5 ms energies go into kCombBins bins of one period (0.8875 s), each bin a running
	// mean over periods (weight 1/4); the 46 ms gap between bursts is the quietest kCombGap bins. Once
	// kCombMin periods are in and that gap is under half the bins' median, each slot at its end (the
	// burst's start) is decoded as the burst's start (UnprotoTiming::b0_start), unless a start of the
	// burst finder lies within 40 ms of it. Until the first packet decodes (then the grid's slots and
	// the timing carry take over). UNPROTO_TRACK_OLD=1: off.
	static constexpr int kCombBins = 180, kCombGap = 8, kCombMin = 4;
	double comb_[kCombBins] = {}; int comb_hits_[kCombBins] = {};
	uint64_t comb_next_ = 0;
	int comb_periods_ = 0; uint64_t comb_last_wrap_ = 0;
	void comb_tick()
	{
		double e;
		if (!bf_.block(e) || track_old()) return;
		const double P = 0.8875 * fs_, w = P / kCombBins;
		const double ph = std::fmod(double(n_), P);
		const int bin = std::min(kCombBins - 1, int(ph / w));
		comb_[bin] = comb_hits_[bin] ? comb_[bin] + 0.25 * (e - comb_[bin]) : e;
		if (comb_hits_[bin] < 1000) comb_hits_[bin]++;
		if (n_ - comb_last_wrap_ >= uint64_t(P)) { comb_last_wrap_ = n_; comb_periods_++; }
		if (slot_ || comb_periods_ < kCombMin) { comb_next_ = 0; return; }
		// the gap: the quietest kCombGap bins in a row (round the period)
		double best = 1e300; int at = 0;
		for (int i = 0; i < kCombBins; i++) { double sm = 0; for (int j = 0; j < kCombGap; j++) sm += comb_[(i + j) % kCombBins]; if (sm < best) { best = sm; at = i; } }
		double v[kCombBins]; for (int i = 0; i < kCombBins; i++) v[i] = comb_[i];
		std::nth_element(v, v + kCombBins / 2, v + kCombBins);
		// Fix 272: digital silence (a muted or unplugged sound card): every bin is 0, so 0 > 0.5 x 0 was false
		// and the "grid" was found in nothing: every period a Viterbi decode of zeros (2.3x noise's CPU in
		// Listen mode). A median of exactly 0 is no signal: no grid. LISTEN_SILENCE_OLD=1: as before.
		if (v[kCombBins / 2] <= 0 && !silence_old()) { comb_next_ = 0; return; }
		if (best / kCombGap > 0.5 * v[kCombBins / 2]) { comb_next_ = 0; return; }
		const double start_ph = std::fmod(double(at + kCombGap) * w, P);   // the burst's start in the period
		// the slot whose audio is all in next: the latest start at least 0.95 s back
		double s0 = std::floor(double(n_) / P) * P + start_ph;
		while (s0 + 0.95 * fs_ > double(n_)) s0 -= P;
		const uint64_t s = uint64_t(std::llround(s0));
		if (s <= comb_next_ + uint64_t(0.5 * P) && comb_next_) return;   // this one is done
		comb_next_ = s;
		bool near = false;
		for (uint64_t st : starts_) if ((st > s ? st - s : s - st) < uint64_t(0.04 * fs_)) near = true;
		if (std::getenv("UNPROTO_DEBUG")) std::fprintf(stderr, "unproto comb: gap %.2f of the median, slot %.3f s%s\n", best / kCombGap / v[kCombBins / 2], double(s) / fs_, near ? " (a start near: decoded as that)" : "");
		if (!near) decode_at(s, true);
	}
	void decode_at(uint64_t b, bool b0_start = false)
	{
		if (b < base_) return;
		std::vector<float> x(buf_.begin() + long(b - base_ > uint64_t(0.05 * fs_) ? b - base_ - uint64_t(0.05 * fs_) : 0), buf_.end());
		const size_t off = size_t(b - base_ > uint64_t(0.05 * fs_) ? uint64_t(0.05 * fs_) : b - base_);
		std::vector<UnprotoCand> cands;
		// Fix 248: the carriers' pulse timing carried from the last packet that decoded, a whole
		// number of periods on (the sender's grid is exact: 0.8875 s); see tref_.
		UnprotoTiming tm;
		tm.b0_start = b0_start;
		const double x0 = double(b) - double(off), P = 0.8875 * fs_;
		if (tref_ok_ && !track_old()) {
			const double kk = (double(b) - tref_b_) / P, k = std::round(kk);
			if (k != 0 && std::fabs(k) <= 40 && std::fabs(kk - k) * P < 0.06 * fs_) { tm.pred = true; tm.swap = std::fmod(std::fabs(k), 2.0) == 1.0; tm.lo = tref_lo_ + k * P - x0; tm.hi = tref_hi_ + k * P - x0; }
		}
		UnprotoPacket p = unproto_decode_burst(x, fs_, off, lo_, hi_, &cands, &tm);
		if (p.ok && tm.got) {
			tref_ok_ = true; tref_lo_ = x0 + tm.out_lo; tref_hi_ = x0 + tm.out_hi;
			tref_b_ = tm.pred ? tref_b_ + std::round((double(b) - tref_b_) / P) * P : double(b);
		}
		if (std::getenv("UNPROTO_DEBUG") && tm.pred) std::fprintf(stderr, "unproto burst %.3f s: predicted timing %s\n", double(b) / fs_, tm.by_pred ? "decoded" : "failed");
		if (!p.ok && !fail_cands_.empty() && grid(fail_at_, b, fs_) && !std::getenv("UNPROTO_NO_COMBINE")) {
			p = unproto_combine(fail_cands_, cands);       // fix 148: with the failed copy before it
			if (std::getenv("UNPROTO_DEBUG")) std::fprintf(stderr, "unproto burst %.3f s: two copies combined: %s\n", double(b) / fs_, p.ok ? "ok" : "no");
		}
		if (p.ok) fail_cands_.clear(); else { fail_cands_.swap(cands); fail_at_ = b; }
		if (std::getenv("UNPROTO_DEBUG")) std::fprintf(stderr, "unproto burst %.3f s: %s level %d status %02x \"%s\"\n", double(b) / fs_, p.ok ? "ok" : "no", p.level, p.status, p.text.c_str());
		if (!p.ok) return;
		// fix 239: the grid from any packet that decodes (CRC-valid), before it is confirmed: the
		// slots after it are decoded, and those that decode confirm it (on gridk)
		last_ok_ = n_;
		const bool new_grid = slot_ == 0;
		if (b + uint64_t(0.8875 * fs_ + 0.5) > slot_) slot_ = b + uint64_t(0.8875 * fs_ + 0.5);
		{   // in time order (the slots before a first packet are decoded after it)
			auto it = dec_.end();
			while (it != dec_.begin() && std::prev(it)->at > b) --it;
			dec_.insert(it, Dec{b, p, false});
		}
		while (dec_.size() > 16) dec_.pop_front();
		// fix 239: a new grid: the slots before this packet that are still in the buffer, oldest
		// first (with the fix 148 combining of a failed copy and the next): in noise the packets
		// before the first that decodes were otherwise lost (seed 2 of p2-fec3-snr-3-it-send: the
		// first two; the reference unit printed them)
		if (new_grid && !std::getenv("UNPROTO_NO_SLOTS")) {
			std::vector<UnprotoCand> fc; fc.swap(fail_cands_); const uint64_t fa = fail_at_;
			const uint64_t P = uint64_t(0.8875 * fs_ + 0.5);
			int k = 1;
			while (k < 9 && b >= k * P + base_ + uint64_t(0.06 * fs_)) k++;
			backfill_ = true;
			for (int j = k - 1; j >= 1; j--) decode_at(b - uint64_t(j) * P);
			backfill_ = false;
			fail_cands_.swap(fc); fail_at_ = fa;
		}
		// in time order, each packet once it is on the grid with a burst before or after it
		// (a decoded one, or one found later: looked at again then)
		// Fix 248: not while the slots before a new grid's first packet are being decoded (each of
		// those decode_at calls came here, and the first packet went out once the oldest one was in,
		// ahead of the slots between: "foxtrot golf 7de de Tnx" with the timing carry, which decodes
		// several of them); once they are all in. UNPROTO_TRACK_OLD=1: as before.
		if (backfill_ && !track_old()) return;
		for (auto &d : dec_) {
			if (d.done) continue;
			bool on = false;
			for (const auto &o : dec_) if (&o != &d && gridk(o.at, d.at, fs_)) on = true;
			if (!on) for (uint64_t s : starts_) if (s != d.at && grid(s, d.at, fs_)) on = true;
			if (!on) break;                                // its successor may still come: keep the order
			d.done = true;
			// a repetition: the counter of the packet before, up to 3 periods after it or with its
			// text (fix 239: with packets lost in noise the counter, 2 bits, comes round again)
			const double gap = (double(d.at) - double(last_at_)) / fs_;
			if (d.p.counter == last_counter_ && (gap < 3.2 * 0.8875 || d.p.text == last_text_)) continue;
			last_counter_ = d.p.counter; last_at_ = d.at; last_text_ = d.p.text;
			text_ += rle_.feed(d.p.text);                  // (idle packets carry none)
		}
	}
};

// P2 long-cycle unproto (modes 7-10, fix 141's framing): the ARQ long packet (a lead word,
// 8 header words, 320 data steps at 40 pulses a row) every 3.3875 s. The long header table is
// the station's (p2_station.hpp p2arq::kLongHeader), passed in. Bursts found by their energy;
// each decoded once all in (3.45 s), taken when another burst lies 3.3875 s (+-40 ms) before or
// after it, each new counter's text once.
UnprotoPacket unproto_decode_long(const std::vector<float> &x, int fs, size_t b0, float lo, float hi, const uint8_t (*lh)[2][8]);

class LongUnprotoRx {
public:
	LongUnprotoRx(const uint8_t (*long_hdr)[2][8], int fs = 9600, float lo = 1400, float hi = 1600) : lh_(long_hdr), fs_(fs), lo_(lo), hi_(hi), bf_(fs, lo, hi) {}
	void tick(int16_t v)
	{
		buf_.push_back(float(v)); n_++;
		if (bf_.tick(v) && (starts_.empty() || bf_.start() - starts_.back() > uint64_t(2.0 * fs_))) starts_.push_back(bf_.start());
		while (next_ < starts_.size() && starts_[next_] + uint64_t(3.45 * fs_) <= n_) { decode_at(starts_[next_]); next_++; }
		const size_t keep = size_t(9 * fs_);
		if (buf_.size() > keep + size_t(fs_)) { const size_t d = buf_.size() - keep; buf_.erase(buf_.begin(), buf_.begin() + long(d)); base_ += d; }
		while (starts_.size() > 8 && next_ > 4) { starts_.pop_front(); next_--; }
	}
	std::string take() { std::string s; s.swap(text_); return s; }

private:
	struct Dec { uint64_t at; UnprotoPacket p; bool done = false; };
	const uint8_t (*lh_)[2][8];
	int fs_; float lo_, hi_;
	BurstFinder bf_;
	std::vector<float> buf_;
	uint64_t n_ = 0, base_ = 0;
	std::deque<uint64_t> starts_;
	size_t next_ = 0;
	std::deque<Dec> dec_;
	int last_counter_ = -1;
	std::string text_;
	text::RleExpander rle_;
	static bool grid(uint64_t a, uint64_t b, int fs) { const double d = std::fabs(double(a) - double(b)) / fs; return std::fabs(d - 3.3875) < 0.04; }
	void decode_at(uint64_t b)
	{
		if (b < base_ + uint64_t(0.05 * fs_)) return;
		const size_t off = size_t(0.05 * fs_);
		std::vector<float> x(buf_.begin() + long(b - base_ - off), buf_.end());
		const UnprotoPacket p = unproto_decode_long(x, fs_, off, lo_, hi_, lh_);
		if (std::getenv("UNPROTO_DEBUG")) std::fprintf(stderr, "long unproto burst %.3f s: %s level %d counter %d \"%s\"\n", double(b) / fs_, p.ok ? "ok" : "no", p.level, p.counter, p.text.c_str());
		if (!p.ok) return;
		dec_.push_back({b, p, false});
		while (dec_.size() > 6) dec_.pop_front();
		for (auto &d : dec_) {
			if (d.done) continue;
			bool on = false;
			for (const auto &o : dec_) if (&o != &d && grid(o.at, d.at, fs_)) on = true;
			if (!on) for (uint64_t st : starts_) if (st != d.at && grid(st, d.at, fs_)) on = true;
			if (!on) break;
			d.done = true;
			if (d.p.counter == last_counter_) continue;
			last_counter_ = d.p.counter;
			text_ += rle_.feed(d.p.text);
		}
	}
};

// P1 unproto (modes 1-2, fix 139's framing): a packet every 1.0125 s, each copy with the
// shift inverted. Every 0.5 s the last 2.4 s of the FSK discriminator are searched at 100 and
// 200 Bd, both polarities. A CRC-valid packet counts when another one lies a whole number (1-3)
// of 1.0125 s periods before or after it (a P1 ARQ link's come 1.25 s apart), or when it
// lies on the period grid of packets already taken; each new counter's text once.
// Fix 182: (1) the bit clock is chosen per packet: the period is 101.25 / 202.5 bits, so
// consecutive packets sit a quarter / half bit apart and one clock for the whole window missed
// most of them in noise (noise 3000 on what we hear: 2 CRC-valid copies in 80 s, nothing
// printed); every eighth of a bit is tried now. (2) Memory ARQ on the grid: once packets have
// been taken, each grid slot that does not decode keeps its soft bits (up to 30, MAXSum), and
// the sums of the last 2..8 failed copies (signs aligned by correlation, the header from the
// newest copy) are tried; a sum is believed only for the next counter. P1U_OLD=1: the old
// receiver (one clock per window, no grid, no memory ARQ).
class P1UnprotoRx {
public:
	explicit P1UnprotoRx(int fs = 9600, float lo = 1400, float hi = 1600) : fs_(fs), disc_(float(fs), (lo + hi) / 2, hi - lo),
		wl_(2 * M_PI * lo / fs), wh_(2 * M_PI * hi / fs) { cl_.push_back(0); ch_.push_back(0); }
	void tick(int16_t v)
	{
		soft_.push_back(disc_.process(float(v)));
		// fix 182: tone correlators (running sums of the signal mixed down by each tone): the
		// energy of each tone over any bit window, noncoherent FSK detection
		const double ph = double(n_ % uint64_t(fs_));   // phases restart each second (exact: whole cycles per second)
		cl_.push_back(cl_.back() + double(v) * std::polar(1.0, -wl_ * ph));
		ch_.push_back(ch_.back() + double(v) * std::polar(1.0, -wh_ * ph));
		n_++;
		if (n_ % uint64_t(fs_ / 2) == 0 && soft_.size() >= size_t(2.4 * fs_)) scan();
		if (soft_.size() > size_t(3 * fs_)) {
			const size_t d = soft_.size() - size_t(2.5 * fs_);
			soft_.erase(soft_.begin(), soft_.begin() + long(d));
			cl_.erase(cl_.begin(), cl_.begin() + long(d)); ch_.erase(ch_.begin(), ch_.begin() + long(d));
			const std::complex<double> l0 = cl_.front(), h0 = ch_.front();
			for (auto &c : cl_) c -= l0;
			for (auto &c : ch_) c -= h0;
			base_ += d;
		}
	}
	std::string take() { std::string s; s.swap(text_); return s; }
	int marq_decodes() const { return marq_ok_; }

private:
	static constexpr double kPer = 1.0125;
	struct Pk { double t; int baud; uint8_t header, status; int nbits; uint8_t data[20]; bool done = false; };
	int fs_;
	FskDiscriminator<> disc_;
	double wl_, wh_;
	std::vector<float> soft_;
	std::vector<std::complex<double>> cl_, ch_;   // running tone sums, cl_[i] = sum of samples before soft_[i]
	// soft value of the bit window [i0, i0 + w) (indices into soft_): tone magnitudes, hi minus lo
	float tone_bit(size_t i0, int w) const
	{
		if (old()) { float acc = 0; for (int j = 0; j < w; j++) acc += soft_[i0 + size_t(j)]; return acc / w; }
		return float((std::abs(ch_[i0 + size_t(w)] - ch_[i0]) - std::abs(cl_[i0 + size_t(w)] - cl_[i0])) / w);
	}
	uint64_t n_ = 0, base_ = 0;
	std::deque<Pk> pk_;
	int last_counter_ = -1;
	std::string text_;
	// fix 182: the grid of taken packets and the failed copies on it
	int grid_baud_ = 0; double grid_t_ = -1, grid_next_ = -1; int grid_miss_ = 0;
	std::deque<std::vector<float>> marq_;
	int marq_ok_ = 0;
	static bool old() { static const bool o = std::getenv("P1U_OLD") != nullptr; return o; }
	// Fix 187: Chase decoding at 200 Bd: the K least reliable bits of a packet's soft values flipped
	// in every combination (2^K header + CRC checks). P1U_CHASE_ACQ / P1U_CHASE_GRID = K (0 = off).
	static int chase_k(bool grid)
	{
		static const int a = std::getenv("P1U_CHASE_ACQ") ? std::atoi(std::getenv("P1U_CHASE_ACQ")) : 4;
		static const int g = std::getenv("P1U_CHASE_GRID") ? std::atoi(std::getenv("P1U_CHASE_GRID")) : 6;
		return old() ? 0 : grid ? g : a;
	}
	static bool chase(const std::vector<float> &soft, int nb, int baud, int K, bool inv, P1Packet &q)
	{
		std::vector<uint8_t> bits(static_cast<size_t>(nb));
		for (int k = 0; k < nb; k++) bits[size_t(k)] = soft[size_t(k)] > 0;
		if (p1_try_packet(bits.data(), 0, nb, baud, inv, q)) return true;
		if (K <= 0) return false;
		std::vector<int> idx(static_cast<size_t>(nb));
		for (int k = 0; k < nb; k++) idx[size_t(k)] = k;
		std::partial_sort(idx.begin(), idx.begin() + K, idx.end(), [&](int a, int b) { return std::fabs(soft[size_t(a)]) < std::fabs(soft[size_t(b)]); });
		for (int m = 1; m < (1 << K); m++) {
			for (int j = 0; j < K; j++) if (m & (1 << j)) bits[size_t(idx[size_t(j)])] ^= 1;
			const bool ok = p1_try_packet(bits.data(), 0, nb, baud, inv, q);
			for (int j = 0; j < K; j++) if (m & (1 << j)) bits[size_t(idx[size_t(j)])] ^= 1;
			if (ok) return true;
		}
		return false;
	}
	double now() const { return double(base_ + soft_.size()) / fs_; }
	void add_cand(double t, int baud, const P1Packet &q)
	{
		for (const auto &e : pk_) if (std::fabs(e.t - t) < 0.02) return;
		if (std::getenv("P1U_DEBUG")) std::fprintf(stderr, "p1u cand t %.3f baud %d status %02x\n", t, baud, q.status);
		Pk e{t, baud, q.header, q.status, q.data_bits, {}, false};
		std::memcpy(e.data, q.data, sizeof e.data);
		auto it = pk_.begin(); while (it != pk_.end() && it->t < t) ++it;
		pk_.insert(it, e);
	}
	bool on_grid(double t, int baud) const
	{
		if (grid_baud_ != baud || grid_t_ < 0) return false;
		const double k = std::round((t - grid_t_) / kPer);
		return std::fabs(t - grid_t_ - k * kPer) < 0.025;
	}
	void deliver(const Pk &e)
	{
		const int cnt = e.status & 3, type = (e.status >> 2) & 7;
		if (cnt == last_counter_) return;
		last_counter_ = cnt;
		BitReader r(e.data, e.nbits);
		if (type == 1 || type == 2) { for (int c; (c = huffman_next(r)) >= 0;) if (c != IDLE_RS) text_ += char(type == 2 && std::isalpha(c) ? c ^ 0x20 : c); }   // 2: case swapped (fix 164)
		else if (type == 0)
			for (int i = 0; i + 8 <= e.nbits; i += 8) { int c = 0; for (int b = 0; b < 8; b++) c |= r.get() << b; if (c != IDLE_RS) text_ += char(c); }
	}
	void take(const Pk &e)
	{
		deliver(e);
		if (grid_baud_ != e.baud || grid_t_ < 0 || e.t > grid_t_) { grid_baud_ = e.baud; grid_t_ = e.t; }
		grid_miss_ = 0;
		if (grid_next_ < 0 || grid_next_ < e.t + 0.5 * kPer) grid_next_ = e.t + kPer;
		marq_.clear();
	}
	// soft bits of the packet whose bit 0 window starts near t (fine timing: +-6 samples)
	bool soft_at(double t, int baud, std::vector<float> &out, double &tf) const
	{
		const double spb = double(fs_) / baud;
		const int nb = p1_packet_bits(baud), win = old() ? int(spb * 0.7) : int(spb * 0.9);
		double best = -1;
		for (int d = -6; d <= 6; d += 2) {
			const double i0 = t * fs_ + d - double(base_);
			if (i0 < 0 || i0 + nb * spb + win >= double(soft_.size())) return false;
			std::vector<float> v(size_t(nb), 0.0f); double e = 0;
			for (int k = 0; k < nb; k++) { const float b = tone_bit(size_t(i0 + k * spb), win); v[size_t(k)] = b; e += std::fabs(b); }
			if (e > best) { best = e; out.swap(v); tf = t + d / double(fs_); }
		}
		return best > 0;
	}
	void grid_slots()
	{
		if (grid_baud_ == 0 || grid_next_ < 0) return;
		const int nb = p1_packet_bits(grid_baud_);
		while (grid_next_ + nb / double(grid_baud_) + 0.02 < now()) {
			const double ts = grid_next_;
			grid_next_ += kPer;
			bool have = false;
			for (const auto &e : pk_) if (std::fabs(e.t - ts) < 0.025) have = true;
			if (have) continue;
			std::vector<float> cur; double tf = ts;
			if (!soft_at(ts, grid_baud_, cur, tf)) continue;
			if (++grid_miss_ > 40) { grid_baud_ = 0; grid_t_ = grid_next_ = -1; marq_.clear(); return; }   // the sender is gone
			std::vector<uint8_t> bits(static_cast<size_t>(nb));
			bool ok = false;
			for (size_t j = grid_baud_ == 200 && chase_k(true) > 0 ? 0 : 1; j <= std::min<size_t>(marq_.size(), 7) && !ok; j++) {
				std::vector<float> sum = cur;
				for (size_t m = marq_.size() - j; m < marq_.size(); m++) {
					const auto &c = marq_[m];
					double dot = 0; for (int k = 8; k < nb; k++) dot += double(cur[size_t(k)]) * c[size_t(k)];
					const float sg = dot < 0 ? -1.0f : 1.0f;
					for (int k = 8; k < nb; k++) sum[size_t(k)] += sg * c[size_t(k)];
				}
				for (int k = 0; k < nb; k++) bits[size_t(k)] = sum[size_t(k)] > 0;
				for (int inv = 0; inv < 2 && !ok; inv++) {
					P1Packet q;
					if (!(grid_baud_ == 200 ? chase(sum, nb, grid_baud_, chase_k(true), inv != 0, q) : p1_try_packet(bits.data(), 0, nb, grid_baud_, inv != 0, q))) continue;
					const int cnt = q.status & 3;
					if (cnt == last_counter_) continue;      // a repeat of the last one taken (its text is out)
					Pk e{tf, grid_baud_, q.header, q.status, q.data_bits, {}, true};
					std::memcpy(e.data, q.data, sizeof e.data);
					marq_ok_++;
					if (std::getenv("P1U_DEBUG")) std::fprintf(stderr, "p1u memory ARQ t %.3f from %zu copies\n", tf, j + 1);
					take(e);
					ok = true;
				}
			}
			if (!ok) { marq_.push_back(std::move(cur)); if (marq_.size() > 30) marq_.pop_front(); }
		}
	}
	void scan()
	{
		const size_t w0 = soft_.size() - size_t(2.4 * fs_);
		for (int baud : {100, 200}) {
			const double spb = double(fs_) / baud;
			const int win = old() ? int(spb * 0.7) : int(spb * 0.9);
			const int nph = old() ? 16 : 8;
			double best_eye = -1;
			std::vector<std::pair<int, std::vector<uint8_t>>> clocks;   // (phase, bits)
			for (int p = 0; p < nph; p++) {
				const double off = p * spb / nph;
				std::vector<uint8_t> b; double eye = 0;
				for (int k = 0;; k++) {
					const size_t i0 = w0 + size_t(off + k * spb);
					if (i0 + size_t(win) >= soft_.size()) break;
					const float acc = tone_bit(i0, win);
					b.push_back(acc > 0); eye += std::fabs(acc);
				}
				if (old()) { if (!b.empty() && eye / b.size() > best_eye) { best_eye = eye / b.size(); clocks.assign(1, {p, std::move(b)}); } }
				else clocks.push_back({p, std::move(b)});
			}
			for (auto &ck : clocks) {
				const auto &bits = ck.second;
				const int total = int(bits.size());
				for (int off = 0; off + p1_packet_bits(baud) <= total; off++)
					for (int inv = 0; inv < 2; inv++) {
						P1Packet q;
						if (!p1_try_packet(bits.data(), off, total, baud, inv != 0, q)) continue;
						add_cand((double(base_ + w0) + ck.first * spb / nph + off * spb) / fs_, baud, q);
					}
			}
			// Acquisition in noise (fix 182): two copies one period apart summed (the shift
			// inverted between them, so the second subtracted), at every clock phase and offset.
			if (!old() && !std::getenv("P1U_NO_PAIRS")) {
				const int nb = p1_packet_bits(baud), per = int(std::lround(kPer * fs_));
				std::vector<uint8_t> bits(static_cast<size_t>(nb));
				std::vector<float> pv(static_cast<size_t>(nb));
				for (int p = 0; p < nph; p++)
					for (int off = 0;; off++) {
						const size_t i0 = w0 + size_t(p * spb / nph + off * spb);
						if (i0 + size_t(per) + size_t(nb * spb) + size_t(win) >= soft_.size()) break;
						for (int k = 0; k < nb; k++) { pv[size_t(k)] = tone_bit(i0 + size_t(k * spb), win) - tone_bit(i0 + size_t(per) + size_t(k * spb), win); bits[size_t(k)] = pv[size_t(k)] > 0; }
						for (int k = 0; k < 8; k++) pv[size_t(k)] = tone_bit(i0 + size_t(k * spb), win);
						for (int inv = 0; inv < 2; inv++) {
							// the header alternates per copy: take it from the first copy alone
							P1Packet q;
							std::vector<uint8_t> b2 = bits;
							for (int k = 0; k < 8; k++) b2[size_t(k)] = tone_bit(i0 + size_t(k * spb), win) > 0;
							if (!(baud == 200 ? chase(pv, nb, baud, chase_k(false), inv != 0, q) : p1_try_packet(b2.data(), 0, nb, baud, inv != 0, q))) continue;
							add_cand((double(base_ + i0)) / fs_, baud, q);
						}
					}
			}
		}
		while (pk_.size() > 12) pk_.pop_front();
		for (auto &e : pk_) {
			if (e.done) continue;
			bool on = false;
			const int kmax = old() ? 1 : 3;
			for (const auto &o : pk_)
				if (&o != &e && o.baud == e.baud)
					for (int k = 1; k <= kmax; k++) if (std::fabs(std::fabs(o.t - e.t) - k * kPer) < 0.025) on = true;
			if (!old() && on_grid(e.t, e.baud)) on = true;
			if (!on) { if (now() - e.t > (old() ? 3.5 : 4.5)) e.done = true; if (old()) break; continue; }   // a lone one, old: dropped
			e.done = true;
			if (old()) deliver(e); else take(e);
		}
		if (!old()) grid_slots();
	}
};

// Fix 239: the reference unit's standby prints unproto of every mode (emulated reference unit pairs: U 1-3, 6, 8),
// one mode at a time. The three receivers above (and, fix 256, the P3 one, which the reference
// unit does not have: p3_unproto_rx.hpp), run together; the one that last delivered text
// owns the output for 8 s (two long-cycle packets with their repetition), so that a false decode
// in another receiver's framing is not printed into a running transmission (2026-11-12: P1UnprotoRx /
// LongUnprotoRx printed "0k as tniela st  ennn<..." into the emulated reference unit's U 3, p2-fec3-it-send).
class AllUnprotoRx {
public:
	AllUnprotoRx(const uint8_t (*long_hdr)[2][8], int fs = 9600, float lo = 1400, float hi = 1600)
		: fs_(fs), p1_(fs, lo, hi), s_(fs, lo, hi), l_(long_hdr, fs, lo, hi), p3_(fs) {}
	void tick(int16_t v)
	{
		n_++;
		// While the P3 receiver holds a transmission the others are not run: they cannot decode it and
		// cost as much as it does (the emulated reference unit's U 33, 60 s of audio on a workstation: P1
		// 3.4 s, P2 short 3.8 s, P3 4.5 s; a Raspberry Pi's listen monitor fell behind with all of them)
		if (!p3_holds()) {
			p1_.tick(v); s_.tick(v); l_.tick(v);
			take_one(0, p1_.take()); take_one(1, s_.take()); take_one(2, l_.take());
		}
		if (!p3_off()) { p3_.tick(v); take_one(3, p3_.take()); }
	}
	std::string take() { std::string t; t.swap(text_); return t; }
	// Fix 256: the P3 receiver holds a P3 unproto transmission (no QSO and no other unproto is on the channel
	// then: the QSO monitor pauses too, monitor_thread.hpp)
	bool p3_holds() const { return !p3_off() && p3_.level() > 0 && !p3_shed_old(); }

private:
	int fs_;
	P1UnprotoRx p1_; UnprotoRx s_; LongUnprotoRx l_;
	P3UnprotoRx p3_;                                   // fix 256: P3 unproto (modes 30-41)
	// The reference unit has no P3 unproto receiver (its standby prints nothing of U 30-41), so ours is an option,
	// off by default (owner rule: defaults match the reference unit): P3U_RX=1 turns it on (Signum: --p3-unproto-rx).
	static bool p3_off() { static const bool v = std::getenv("P3U_OLD") != nullptr || std::getenv("P3U_RX") == nullptr; return v; }
	static bool p3_shed_old() { static const bool v = std::getenv("P3U_SHED_OLD") != nullptr; return v; }   // the others run beside a P3 lock
	uint64_t n_ = 0, owned_at_ = 0;
	int owner_ = -1;
	std::string text_;
	void take_one(int k, std::string t)
	{
		if (t.empty()) return;
		if (owner_ >= 0 && owner_ != k && n_ - owned_at_ < uint64_t(8 * fs_)) {
			if (std::getenv("UNPROTO_DEBUG")) std::fprintf(stderr, "unproto: receiver %d dropped \"%s\" (receiver %d owns)\n", k, t.c_str(), owner_);
			return;
		}
		owner_ = k; owned_at_ = n_; text_ += t;
	}
};

}  // namespace fmodem
