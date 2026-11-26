// p3_unproto_rx.hpp: P3 unproto (modes 30-41) reception (fix 256).
//
// The reference unit sends P3 unproto ("for test purposes", its manual) but has no receiver for
// it: its standby prints nothing. This receiver is ours (a bonus over the reference unit). The
// framing is P2 unproto's: a packet every 0.8875 s (short, modes 30-35) or 3.3875 s (long, 36-41),
// each packet sent `reps` times (default 2), the carrier map alternating with every transmission
// (even, odd, ...), the header by the counter's parity, the first packet counter 3 (idle until
// text comes), an end packet with status bit 7. The packets are the ARQ packets of the speed level
// (p3_levels / p3_long), so the ARQ decoders are used; the level is not known in advance: the
// burst's spectrum names the candidates, each is tried, and the one that decodes is held while
// packets decode.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "compress.hpp"
#include "p2_codec.hpp"
#include "p3_levels.hpp"
#include "p3_long.hpp"
#include "p3_modem.hpp"
#include "p3_rx.hpp"
#include "freq_search.hpp"

namespace fmodem {
namespace p3u {

constexpr double kT = 0.010;
inline double period(bool lng) { return lng ? 3.3875 : 0.8875; }
// Pulse 0's centre after the slot's word-0 centre (slot + 0.8 T), as P2Station::p3_pulse0 places
// ours (short: the level's offset, SL5 25.5 samples; long: fix 225's offsets).
inline double pulse0_off_s(int level, bool lng)
{
	if (lng) return level <= 2 ? 0.0 : std::max(0.0, p3::level_offset_s(level) - 6.5 / 9600.0);
	return level == 5 ? 25.5 / 9600.0 : p3::level_offset_s(level);
}
inline int payload(int level, bool lng) { return lng ? p3::long_payload(level) : level == 1 ? p3::kSl1Data : level == 5 ? p3::kDataBytes : p3::level_data_bytes(level); }

struct Packet { bool ok = false; int level = 0; bool lng = false; uint8_t status = 0; std::vector<uint8_t> data; double slot = 0; int odd = -1; };

inline double nib(const std::complex<double> &a, const std::complex<double> &b) { return std::fmod(-16.0 * std::arg(b * std::conj(a)) / (2 * M_PI) + 32.0, 16.0); }

// SL1 short: P2 level-1 coding on 1080/1920 Hz with the CS pulse (p3_modem.hpp modulate_cs), the
// SL1 scrambler and CRC (sl1_check). `s`: the slot (word 0's centre at s + 0.8 T) in x.
Packet sl1_short(const std::vector<float> &x, double fs, double s, double search_ms);

// SL1 long: the two carriers' 330 words (p3::long1_decode), both orders, +-2 pulses.
Packet sl1_long(const std::vector<float> &x, double fs, double s, double search_ms);

// SL2-6 long (as P2Station::decode_p3_long_lv, without memory ARQ): products per carrier map,
// the common phase step removed (fix 241), p3::long_decode.
Packet lv_long(int lv, const std::vector<float> &x, double fs, double s, double search_ms);

// One packet at `level` (short or long) whose slot is near s (+-search_ms).
Packet decode(int level, bool lng, const std::vector<float> &x, double fs, double s, double search_ms);

// The pulse words of one packet (the station's unproto transmitter and the firmware check share
// them): the ARQ packet of the level with the header of the counter's parity (status & 3 =
// counter, so the header templates that follow the status parity agree).
std::vector<uint8_t> words(int level, bool lng, const uint8_t *data, uint8_t status);

inline std::string text_of(const Packet &p) { return text::decode((p.status >> 2) & 7, p.data.data(), int(p.data.size()), false); }

// A frequency shift of real audio by hz (a Hilbert pair, 127 taps; as P2Station::shift_audio).
void shift(std::vector<float> &a, double hz, double fs);
// The burst's frequency offset (Hz as received) from its spectrum against the level's carrier comb
// (freq_search.hpp, as the station's frequency search): SL1 its two carriers and the narrow pulse,
// SL2 its six, SL3-6 the 18-carrier comb. match < 0.35: no reading (ok false).
fsearch::Result offset(const std::vector<float> &x, double fs, int level);

// Which speed levels a burst's spectrum fits (cheap, before any decode): the power at the 18 P3
// carriers (480 + 120 k Hz) over the burst's first 0.5 s; a level fits when the mean power on its
// carriers (those on for the whole packet) is 4 x the mean off them and none of them is under 1/33 of
// that mean. SL3 and SL4, SL5 and SL6 share a set: both are named (SL6 first when the edge carriers
// k0 / k17 carry a third of the others' mean, else SL5). Best fit first.
// P2 (1400/1600 Hz) and P1 bursts fit none.
std::vector<int> levels_for(const std::vector<float> &x, double fs, double a);

}  // namespace p3u

// Fix 256: P3 unproto in standby (beside UnprotoRx & co. in AllUnprotoRx). Bursts by their band
// energy; a burst is tried only when another started a whole number (1-4) of unproto periods
// before it (a P3 ARQ link's come 1.25 s apart) and its spectrum fits a level; once a packet
// decodes, the level, the length and the slot grid are held and every slot of the grid is decoded
// (the timing carried from the last packet, the period measured from the packets); 12 short or 4
// long slots without a packet drop the lock. Each new counter's text once (a repetition has the
// counter of the copy before it). The sender's frequency offset is measured from the burst's
// spectrum (freq_search.hpp) and the audio shifted by it before decoding (measured again on a
// miss; P3U_OFFSET_OLD=1: none). P3U_DEBUG=1: a line per decode to stderr.
class P3UnprotoRx {
public:
	explicit P3UnprotoRx(int fs = 9600) : fs_(fs) { hist_.assign(800, 1e18); }
	void tick(int16_t v)
	{
		buf_.push_back(float(v)); n_++;
		// band energy (300-2700 Hz: a 2-pole low-pass at 1.2 kHz of the signal mixed down by 1.5 kHz) in 5 ms blocks
		ph_ *= w_; if (++renorm_ == 4096) { ph_ /= std::abs(ph_); renorm_ = 0; }
		const std::complex<double> z = double(v) * ph_;
		y1_ += a_ * (z - y1_); y2_ += a_ * (y1_ - y2_);
		acc_ += std::norm(y2_);
		if (++k_ == fs_ / 200) block();
		if (lock_) slots(); else pending();
		const size_t keep = size_t(9 * fs_);
		if (buf_.size() > keep + size_t(fs_)) { const size_t d = buf_.size() - keep; buf_.erase(buf_.begin(), buf_.begin() + long(d)); base_ += d; }
	}
	std::string take() { std::string s; s.swap(text_); return s; }
	int level() const { return lock_ ? lv_ : 0; }
	bool lng() const { return lng_; }

private:
	struct Start { uint64_t at; double len = -1; bool tried = false; };
	int fs_;
	std::vector<float> buf_;
	uint64_t n_ = 0, base_ = 0;
	std::complex<double> w_ = std::polar(1.0, -2 * M_PI * 1500.0 / 9600.0), ph_{1, 0}, y1_{0, 0}, y2_{0, 0};
	double a_ = 1.0 - std::exp(-2 * M_PI * 1200.0 / 9600.0), acc_ = 0;
	int k_ = 0, renorm_ = 0, off_run_ = 99, on_run_ = 0;
	std::vector<double> hist_; size_t hi_ = 0;
	std::deque<Start> starts_;
	// the lock
	bool lock_ = false, lng_ = false; int lv_ = 0;
	double s_ref_ = 0, s_last_ = 0, per_ = 0; int64_t k_last_ = 0; int miss_ = 0;
	double df_ = 0;                                    // the sender's frequency offset, Hz as received
	static bool offset_old() { static const bool v = std::getenv("P3U_OFFSET_OLD") != nullptr; return v; }   // no offset measured
	double next_ = 0;
	int last_counter_ = -1; double last_at_ = -1e18; std::string last_text_;
	std::string text_;
	text::RleExpander rle_;
	static bool dbg() { static const bool d = std::getenv("P3U_DEBUG") != nullptr; return d; }
	double secs(double s) const { return s / fs_; }
	void block()
	{
		const double e = std::sqrt(acc_ / k_); acc_ = 0; k_ = 0;
		// the floor: the quietest block of the last 4 s (more than a long period, so a gap is in it)
		hist_[hi_++ % hist_.size()] = e;
		double fl = 1e18; for (double q : hist_) fl = std::min(fl, q);
		sm_[si_++ % 3] = e;
		const double es = (sm_[0] + sm_[1] + sm_[2]) / 3;
		const bool on = es > std::max(3.0 * fl, 30.0);
		if (on) {
			if (off_run_ >= 4) starts_.push_back({n_ > uint64_t(fs_ / 100) ? n_ - uint64_t(fs_ / 100) : 0});
			on_run_++; off_run_ = 0;
		} else {
			if (off_run_ == 3 && !starts_.empty() && starts_.back().len < 0) starts_.back().len = double(n_ - starts_.back().at) / fs_ - 0.02;
			off_run_++; on_run_ = 0;
		}
		while (starts_.size() > 12) starts_.pop_front();
		comb_[0].add(double(n_), e); comb_[1].add(double(n_), e);
		if (!lock_ && !comb_old()) { comb_try(comb_[0], false); if (!lock_) comb_try(comb_[1], true); }
	}
	// In noise the energy starts miss the bursts (and long bursts' lengths): the band energy folded on each
	// period (as the P2 unproto receiver, fix 248): 180 bins of one period, a running mean (1/4) per bin; once kMin
	// periods are in and the quietest run of bins (most of the gap between bursts: 62 ms short, 82 ms long)
	// is under half the median, the burst after the gap is tried, once a period, until a packet decodes.
	// P3U_COMB_OLD=1: off.
	struct Comb {
		double P = 0; int gap = 8, kmin = 4;
		double v[180] = {}; int hits[180] = {};
		int periods = 0; double last_wrap = 0, last_try = -1e18;
		void add(double n, double e)
		{
			const int b = std::min(179, int(std::fmod(n, P) / (P / 180)));
			v[b] = hits[b] ? v[b] + 0.25 * (e - v[b]) : e;
			if (hits[b] < 1000) hits[b]++;
			if (n - last_wrap >= P) { last_wrap = n; periods++; }
		}
	};
	Comb comb_[2] = {mk_comb(false), mk_comb(true)};
	double sm_[3] = {0, 0, 0}; uint64_t si_ = 0;
	Comb mk_comb(bool lng) const { Comb c; c.P = p3u::period(lng) * fs_; c.gap = lng ? 3 : 8; c.kmin = lng ? 3 : 4; return c; }
	static bool comb_old() { static const bool v = std::getenv("P3U_COMB_OLD") != nullptr; return v; }
	void comb_try(Comb &c, bool lng)
	{
		if (c.periods < c.kmin) return;
		double best = 1e300; int at = 0;
		for (int i = 0; i < 180; i++) { double sm = 0; for (int j = 0; j < c.gap; j++) sm += c.v[(i + j) % 180]; if (sm < best) { best = sm; at = i; } }
		double w[180]; for (int i = 0; i < 180; i++) w[i] = c.v[i];
		std::nth_element(w, w + 90, w + 180);
		if (!(w[90] > 1.0) || best / c.gap > 0.5 * w[90]) return;   // (silence: nothing to fold)
		const double ph = std::fmod(double(at + c.gap) * c.P / 180, c.P);   // the burst's start in the period
		const double need = (lng ? 3.5 : 0.98) * fs_;
		double s0 = std::floor(double(n_) / c.P) * c.P + ph;
		while (s0 + need > double(n_)) s0 -= c.P;
		if (s0 < c.last_try + 0.5 * c.P) return;
		c.last_try = s0;
		if (dbg()) std::fprintf(stderr, "p3u comb %s: gap %.2f of the median, burst at %.3f s\n", lng ? "long" : "short", best / c.gap / w[90], secs(s0));
		acquire(s0, lng, lng ? 0.021 : 0.012);
	}
	bool on_grid(double a, double b, bool lng) const
	{
		const double P = p3u::period(lng) * fs_, d = std::fabs(a - b), k = std::round(d / P);
		return k >= 1 && k <= 4 && std::fabs(d - k * P) < 0.025 * fs_;
	}
	std::vector<float> window(double from, double to) const
	{
		const long a = std::max(0L, long(from) - long(base_)), b = std::min(long(buf_.size()), long(to) - long(base_));
		if (b <= a) return {};
		return std::vector<float>(buf_.begin() + a, buf_.begin() + b);
	}
	// acquisition: a burst with its length known, on the grid of another, its spectrum fitting a level
	void pending()
	{
		for (size_t i = 0; i < starts_.size() && !lock_; i++) {
			Start &s = starts_[i];
			if (s.tried || s.len < 0) continue;
			const bool lng = s.len > 2.0;
			if ((lng && (s.len < 2.6 || s.len > 3.7)) || (!lng && (s.len < 0.6 || s.len > 1.0))) { s.tried = true; continue; }
			bool grid = false;
			for (const auto &o : starts_) if (&o != &s && o.len > 0 && (o.len > 2.0) == lng && on_grid(double(o.at), double(s.at), lng)) grid = true;
			if (!grid) continue;
			s.tried = true;
			if (dbg()) std::fprintf(stderr, "p3u burst %.3f s len %.3f\n", secs(double(s.at)), s.len);
			acquire(double(s.at), lng, 0.012);
		}
	}
	// A burst that starts near `at` (+-span s): every level its spectrum fits, at offsets of 6-7 ms; the first
	// packet that decodes locks the level, the length and the grid.
	bool acquire(double at, bool lng, double span)
	{
		if (at < double(base_) + 0.05 * fs_) return false;
		const double a = at - 0.05 * fs_;
		const auto x = window(a, at + (lng ? 3.5 : 0.98) * fs_ + span * fs_);
		const auto lvs = p3u::levels_for(x, fs_, 0.05 * fs_);
		if (dbg()) { std::fprintf(stderr, "p3u try %.3f s %s:", secs(at), lng ? "long" : "short"); for (int l : lvs) std::fprintf(stderr, " SL%d", l); std::fprintf(stderr, "\n"); }
		const int nofs = int(std::ceil(span / 0.0065));
		for (int lv : lvs) {
			// the frequency offset from the burst's spectrum (the decoders remove only a small residual):
			// the audio shifted onto the nominal carriers first
			const auto fo = offset_old() ? fsearch::Result{} : p3u::offset(x, fs_, lv);
			const double df = fo.ok && std::fabs(fo.hz) > 1.0 ? fo.hz : 0.0;
			std::vector<float> xs;
			if (df != 0.0) { xs = x; p3u::shift(xs, -df, fs_); }
			const auto &xd = df != 0.0 ? xs : x;
			p3u::Packet p;
			for (int k = 0; k <= 2 * nofs && !p.ok; k++) {
				const double off = (k == 0 ? 0 : (k % 2 ? 1 : -1) * ((k + 1) / 2)) * span / std::max(nofs, 1);
				p = p3u::decode(lv, lng, xd, fs_, 0.05 * fs_ + off * fs_, 3.5);
			}
			if (dbg() && df != 0.0) std::fprintf(stderr, "p3u SL%d: offset %+.1f Hz (match %.2f): %s\n", lv, df, fo.match, p.ok ? "decoded" : "no");
			if (!p.ok) continue;
			df_ = df;
			lock_ = true; lv_ = lv; lng_ = lng; per_ = p3u::period(lng) * fs_;
			s_ref_ = s_last_ = a + p.slot; k_last_ = 0; miss_ = 0;
			if (dbg()) std::fprintf(stderr, "p3u lock: SL%d %s, slot %.4f s\n", lv, lng ? "long" : "short", secs(s_ref_));
			// the slots before it still in the buffer (the packets the grid condition waited for)
			int kb = 1;
			while (kb < 8 && s_ref_ - kb * per_ > double(base_) + 0.06 * fs_) kb++;
			for (int k = kb - 1; k >= 1; k--) try_slot(s_ref_ - k * per_, 2.0, false);
			got(p, s_ref_);
			next_ = s_ref_ + per_;
			starts_.clear();
			return true;
		}
		return false;
	}
	void slots()
	{
		const double need = (lng_ ? 3.42 : 0.90) * fs_;
		while (double(n_) >= next_ + need) {
			const double s = next_;
			next_ += per_;
			if (!try_slot(s, miss_ == 0 ? 1.5 : 3.0, true)) {
				++miss_;
				if (miss_ >= (lng_ ? 1 : 2) && !reanchor_old() && reanchor(s)) continue;
				if (miss_ >= (lng_ ? 4 : 12)) { lock_ = false; comb_[0] = mk_comb(false); comb_[1] = mk_comb(true); if (dbg()) std::fprintf(stderr, "p3u lock lost\n"); return; }
			}
		}
	}
	// The sender's audio may jump (a capture overrun skipped on a slow computer, a sound card restart): its
	// bursts then leave the grid. After misses, a burst start of the right length near the missed slot but off
	// the grid is decoded at the locked level, and the grid starts again from it. P3U_REANCHOR_OLD=1: off.
	static bool reanchor_old() { static const bool v = std::getenv("P3U_REANCHOR_OLD") != nullptr; return v; }
	bool reanchor(double s)
	{
		const double P = p3u::period(lng_) * fs_;
		for (auto &st : starts_) {
			const double at = double(st.at);
			if (st.tried || st.len < 0 || (st.len > 2.0) != lng_ || std::fabs(at - s) > 0.6 * P || std::fabs(at - s) < 0.008 * fs_) continue;
			if (at + (lng_ ? 3.45 : 0.95) * fs_ > double(n_)) continue;   // not all in yet
			st.tried = true;
			auto x = window(at - 0.05 * fs_, at + (lng_ ? 3.5 : 0.98) * fs_ + 0.012 * fs_);
			if (df_ != 0.0) p3u::shift(x, -df_, fs_);
			p3u::Packet p;
			for (double off : {0.0, -0.006, 0.006, -0.012, 0.012}) { p = p3u::decode(lv_, lng_, x, fs_, 0.05 * fs_ + off * fs_, 3.5); if (p.ok) break; }
			if (!p.ok) continue;
			const double slot = at - 0.05 * fs_ + p.slot;
			if (dbg()) std::fprintf(stderr, "p3u grid moved %+.1f ms: slot %.4f s\n", (slot - s) / fs_ * 1e3, secs(slot));
			s_ref_ = slot; per_ = P; miss_ = 0;
			next_ = slot + P;
			got(p, slot);
			return true;
		}
		return false;
	}
	bool try_slot(double s, double search_ms, bool track)
	{
		if (s - 0.05 * fs_ < double(base_)) return false;
		auto x = window(s - 0.05 * fs_, s + (lng_ ? 3.45 : 0.95) * fs_);
		const auto raw = df_ != 0.0 || track ? x : std::vector<float>();
		if (df_ != 0.0) p3u::shift(x, -df_, fs_);
		auto p = p3u::decode(lv_, lng_, x, fs_, 0.05 * fs_, search_ms);
		if (!p.ok && track && !offset_old()) {
			// a miss: the offset measured again on this burst (the sender's radio may drift or step)
			const auto fo = p3u::offset(raw, fs_, lv_);
			if (fo.ok && std::fabs(fo.hz - df_) > 5.0) {   // (readings scatter +-3 Hz on the air: a step, not the scatter)
				auto xs = raw; p3u::shift(xs, -fo.hz, fs_);
				p = p3u::decode(lv_, lng_, xs, fs_, 0.05 * fs_, search_ms);
				if (p.ok) df_ = fo.hz;
				if (dbg()) std::fprintf(stderr, "p3u slot %.4f s: offset re-measured %+.1f Hz: %s\n", secs(s), fo.hz, p.ok ? "decoded" : "no");
			}
		}
		if (dbg()) std::fprintf(stderr, "p3u slot %.4f s: %s\n", secs(s), p.ok ? "ok" : "no");
		if (!p.ok) return false;
		const double at = s - 0.05 * fs_ + p.slot;
		if (track) {
			miss_ = 0;
			const int64_t k = int64_t(std::llround((at - s_ref_) / per_));
			if (k > 0) per_ = (at - s_ref_) / double(k);   // the sender's period, from the first packet
			next_ = at + per_ * double(std::llround((next_ - at) / per_));
		}
		got(p, at);
		return true;
	}
	void got(const p3u::Packet &p, double at)
	{
		const std::string t = p3u::text_of(p);
		const int c = p.status & 3;
		const double gap = (at - last_at_) / per_;
		if (dbg()) std::fprintf(stderr, "p3u packet %.4f s SL%d%s status %02x \"%s\"\n", secs(at), p.level, p.lng ? " long" : "", p.status, t.c_str());
		if (c == last_counter_ && (gap < 3.2 || t == last_text_)) return;   // a repetition
		last_counter_ = c; last_at_ = at; last_text_ = t;
		text_ += rle_.feed(t);
	}
};

}  // namespace fmodem
