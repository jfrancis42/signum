// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// P2Station member functions, part 1 of 8 (split out of p2_station.hpp so they build in parallel).
#include "p2_station.hpp"

namespace fmodem {

P2Station::P2Station(const P2Config &c)
	: cfg_(c), disc_(c.fs, (c.tone_lo + c.tone_hi) / 2, c.tone_hi - c.tone_lo)
{
	if (std::getenv("P2_DEBUG")) debug_ = true;    // test: the debug log lines
	pre_.assign(kRing, 0.0);                   // the larger ring first (esp32/: it needs the largest free block)
	raw_.assign(kRing, 0.0f);
	if (cfg_.rx_mute_delay > 0) ptt_hist_.assign(kRing, 0);   // fix 243
	ps_hi_.assign(size_t(0.6 * cfg_.fs) + 2, 0.0); ps_lo_.assign(size_t(0.6 * cfg_.fs) + 2, 0.0);   // the answer search's running sums (p1_tone_sums)
	gd_ = (disc_.taps() - 1) / 2.0 + 0.5;
	for (auto &ch : cfg_.mycall) ch = char(std::toupper(ch));
	T_ = 0.010 * cfg_.fs;
	nw_ = int(std::lround(1.6 * T_));
}

void P2Station::connect(const std::string &call)
{
	if (state_ != ST_IDLE && state_ != ST_LISTEN) return;
	remote_ = call;
	for (auto &ch : remote_) ch = char(std::toupper(ch));
	long_path_ = !remote_.empty() && (remote_[0] == '!' || remote_[0] == ';');   // long path
	if (long_path_) remote_.erase(0, 1);
	// MYLevel 1: the call was placed by the P1 station itself, which has no frequency
	// offset handling: against the reference unit at MYLevel 1, 0 characters from +40 Hz (fix 207).
	// Now the P2 station places it as every other call (fix 185's answer grid, fix 108's FSK
	// measurement, fix 145's transmit shift), with a plain P1 set-up packet (no P2/P3
	// bits: send_p1_setup), and hands the link over on its acknowledgement with the
	// measured offset (fix 184). P1_OWN_CALL=1: the old way.
	if (cfg_.p1_only && kP1Link && p1_own_call()) {
		p1_new(n_);
		p1_->connect((long_path_ ? "!" : "") + remote_);
		master_ = true; state_ = ST_P1; p1_up_seen_ = false;
		log("calling " + remote_ + " (P1)");
		return;
	}
	master_ = true;
	state_ = ST_CALLING;
	fsk_afc_reset();
	p1_trk_n_ = 0; p1_trk_sum_ = 0; p1_trk_lo_ = 1e9; p1_trk_hi_ = -1e9;   // fix 207
	misses_ = 0;
	send_call(n_ + samples(0.1));
	log("calling " + remote_ + (long_path_ ? " (long path)" : ""));
}

void P2Station::send_text(const std::string &t)
{
	pstats_.tx_text += t.size();
	if (state_ == ST_P1) { p1_->send_text(t); return; }
	// Fix 207: our MYLevel 1 call not yet handed over: the P1 station gets this queue as it is
	// (adopt_master), so it is kept in its form (P1 transparency), not P2's.
	if (cfg_.p1_only && master_ && (state_ == ST_CALLING || state_ == ST_SETUP_M)) { txq_ += P1Station::escape(t); return; }
	txq_ += rle_tx_.feed(t);
	// The next new packet changed (Pat's data comes in bursts): its audio made ahead is
	// made again, once, on the next tick. Not here: the terminal hands typed text over a
	// byte at a time, and a 2,000-character paste remade it 2,000 times in one audio
	// period (0.2 s: a capture overrun, after which the link never recovered).
	// P3_PREMOD_EACH=1: remade on every call, as before.
	static const bool each = std::getenv("P3_PREMOD_EACH") != nullptr;
	if (state_ == ST_ISS && !premod_.empty()) { if (each) premod_next(false); else premod_stale_ = true; }
}

bool P2Station::changeover_key(int keys)
{
	if (state_ == ST_P1) return p1_->changeover_key(keys);
	if (state_ == ST_UNPROTO || !link_up_ || !(connected() || state_ == ST_P3SETUP)) return false;   // P3SETUP: our break-in under way
	if ((keys & P1Station::KEY_BK) && state_ == ST_IRS) { bk_req_ = true; log("break-in requested"); return true; }
	if ((keys & P1Station::KEY_CHO) && state_ != ST_IRS) { cho_req_ = true; log("changeover requested"); return true; }
	return false;
}

bool P2Station::start_unproto(int mode, int reps)
{
	// Fix 256: modes 30-41, P3 unproto (30-35 SL1-SL6 short, 36-41 long; the reference
	// firmware's U command). P3U_OLD=1: refused, as before.
	static const bool p3u_old = std::getenv("P3U_OLD") != nullptr;
	const bool p3 = mode >= 30 && mode <= 41 && !p3u_old;
	if ((state_ != ST_IDLE && state_ != ST_LISTEN) || ((mode < 1 || mode > 10) && !p3)) return false;
	up_p3_ = p3;
	up_long_ = p3 ? mode >= 36 : mode >= 7;
	up_p1_ = mode <= 2; up_baud_ = mode == 1 ? 100 : 200;
	up_inv_ = true; up_hdr_ = 0x55; up_phase_ = 0;     // the first copy inverted, AA first (toggled before use), as the reference unit sends them
	up_level_ = p3 ? (mode - 30) % 6 + 1 : mode >= 7 ? mode - 6 : std::max(1, mode - 2); up_reps_ = std::max(1, std::min(5, reps)); up_counter_ = p3 ? 3 : 1; up_rep_ = 0; up_slot_ = 0; up_qrt_sent_ = false;
	up_first_ = true; up_idle_since_ = n_; want_qrt_ = false; txq_.clear();
	state_ = ST_UNPROTO;
	schedule(n_ + samples(0.1), EV_UNPROTO, n_ + samples(0.1));
	log("unproto mode " + std::to_string(mode) + (up_p1_ ? " (P1 FSK " + std::to_string(up_baud_) + " Bd)" : up_p3_ ? " (P3 SL" + std::to_string(up_level_) + (up_long_ ? " long)" : " short)") : " (P2 level " + std::to_string(up_level_) + (up_long_ ? ", long cycle)" : ")")) + ", " + std::to_string(up_reps_) + " repetitions");
	return true;
}

void P2Station::host_status(uint8_t out[4]) const
{
	if (state_ == ST_P1 && p1_ && link_up_ && !std::getenv("P2_HOST_P1_OLD")) {
		// A P1 link as the reference unit reports it (emulated V4.1r in host mode, G3 on
		// channel 254 every 0.25 s, the clean tree's P1-7/P1-13 [bb-emu]): byte 0 high
		// nibble A, bit 3 sending, low 3 bits 3 steady (sending, or receiving idle), 2
		// receiving text, 4 a break-in in progress; byte 1 level 1; byte 2 the speed, 0 at
		// 100 Bd, 1 at 200 Bd (fix 166). P2_HOST_P1_OLD: the P2 form (old).
		const bool snd = (p1_->sending() && !p1_->bk_ours()) || p1_->bk_theirs();
		const int low = p1_->bk_ours() || p1_->bk_theirs() ? 4 : (!snd && p1_->last_rx_text()) ? 2 : 3;
		out[0] = uint8_t(0xA0 | (snd ? 8 : 0) | low);
		out[1] = 1;
		out[2] = uint8_t(p1_->cur_baud() == 200 ? 1 : 0);
		out[3] = 128;
		return;
	}
	const bool linked = link_up_, calling = setting_up();
	if (standby()) {                                 // the reference firmware's standby byte (busy_detector.hpp)
		out[0] = standby_status();
		out[1] = 0; out[2] = 1; out[3] = 128;
		return;
	}
	const bool sending = state_ == ST_P1 ? p1_->sending() || p1_->calling() : state_ == ST_ISS || state_ == ST_BK_SENT || state_ == ST_CALLING;
	out[0] = uint8_t((linked || calling ? 2 : 0) << 4 | (sending ? 8 : 0) | (calling ? 1 : linked ? 2 : 0));
	out[1] = uint8_t(linked ? link_plevel() : 0);
	out[2] = uint8_t(std::max(1, speed_level()));
	out[3] = 128;
}

int16_t P2Station::tick(int16_t rx_sample)
{
	const int64_t i = n_ % kRing;
	const bool muted = cfg_.rx_mute_delay > 0 ? (n_ > cfg_.rx_mute_delay && ptt_hist_[size_t((n_ - 1 - cfg_.rx_mute_delay) % kRing)]) : ptt_;
	const float x = muted ? 0.0f : float(rx_sample);
	raw_[size_t(i)] = x;
	const float d = disc_.process(x);
	pre_[size_t(i)] = (n_ ? pre_[size_t((n_ - 1) % kRing)] : 0.0) + d;
	busy_tick(x);
	// the front panel's S/N and audio level (rx_meter.hpp): measurement only, read by nothing here
	if (link_up_ != meter_link_) { meter_link_ = link_up_; if (link_up_) meter_.reset_snr(); }
	meter_.set_idle(!link_up_ && (state_ == ST_IDLE || state_ == ST_LISTEN));   // standby: the noise reference is measured now
	meter_.tick(float(rx_sample), muted);
	if (premod_stale_) { premod_stale_ = false; if (state_ == ST_ISS && !premod_.empty()) premod_next(false); }
	else if (chg_redo_ && n_ - chg_at_ >= samples(kChgRedoS)) { chg_redo_ = false; if (state_ == ST_ISS && !premod_.empty()) premod_next(false); }
	while (!events_.empty() && events_.front().at <= n_) {
		const Event e = events_.front();
		events_.pop_front();
		handle(e);
	}
	if (link_up_ && state_ != ST_P1 && (n_ & 255) == 0) link_watch();
	if (state_ == ST_P1) {                          // a P1 link: the P1 station has the radio
		const int16_t y = p1_tick(rx_sample);
		if (!ptt_hist_.empty()) ptt_hist_[size_t(n_ % kRing)] = ptt_;
		n_++;
		return y;
	}
	if (state_ == ST_LISTEN) acquire_call();
	const int16_t y = play();
	if (!ptt_hist_.empty()) ptt_hist_[size_t(n_ % kRing)] = ptt_;
	n_++;
	return y;
}

void P2Station::schedule(int64_t at, EvType t, int64_t ref, int arg)
{
	// every P3 decode launch (ref + kP3Launch) gets its timing search
	// first, while the packet is still arriving (ref + kP3Sync)
	if (t == EV_P3_LAUNCH) { schedule(ref + samples(kP3Sync), EV_P3_SYNC, ref, arg); schedule(ref + samples(0.835), EV_P3_SL1, ref, 0); }
	// fix 236: every long-packet decode launch too (its search and plan, EV_P3L_SYNC)
	if (t == EV_P3L_LAUNCH) {
		schedule(ref + samples(kP3Sync), EV_P3L_SYNC, ref);
		// the long demodulation in pieces while the packet arrives (EV_P3L_PRE, advance_p3_long)
		for (double at : kP3LPre) schedule(ref + samples(at), EV_P3L_PRE, ref);
	}
	// every P2 packet look (arg 0: not a re-look) gets its pulse timing
	// first, from the packet's first 0.3 s, while the rest arrives
	if (t == EV_PACKET && arg == 0 && !p3_) schedule(ref + samples(kP2Sync), EV_P2_SYNC, ref, 0);
	// Fix 73: the P2 decode starts as soon as the packet is in (its last
	// pulse ends ~0.84 s after its start, plus the peer's tracked lateness),
	// not at the first look (0.86 s): as caller our reply was due only ~3 ms
	// after that look with N5TW (40 m, 2026-10-16 23:48; decodes took 4-5 ms,
	// all late, then our T and fix 53 no longer applied: timeout).
	if (t == EV_PACKET && arg == 0 && !p3_ && cfg_.realtime && !std::getenv("P2_NO_EARLY"))
		schedule(ref + samples(kP2Launch) + (trk_ok_ ? int64_t(std::max(0.0, trk_dt_)) : 0), EV_P2_LAUNCH, ref, 0);
	// Fix 79: in real time the first look at a P3 packet follows its decode
	// (launched at kP3Launch; on_packet polls every 2 ms until shortly before
	// our answer), not a fixed 0.88 s: as caller our answer was due before
	// that look on the air ("send CS P at -15 ms from now" in every cycle,
	// KC0TPS / N5TW / KB0TNG 2026-10-17: the control signal's start cut off).
	// The QRT end-packet test needs the whole packet: it keeps 0.88 s.
	if (t == EV_PACKET && arg == 0 && p3_ && cfg_.realtime && !qrt_rx_ && !std::getenv("P3_LATE_LOOK"))
		at = std::min(at, ref + samples(kP3Launch + 0.004));
	Event e{at, t, ref, arg};
	auto it = events_.begin();
	while (it != events_.end() && it->at <= at) ++it;
	events_.insert(it, e);
}

double P2Station::bit_soft(double a, int baud) const
{
	const double w = cfg_.fs / baud;
	// The two tones' energies over the bit, from the raw samples, instead of
	// the integrated FM discriminator (no threshold effect): over our 182
	// on-air session recordings (p1reply_replay) the call answers found rose
	// 403 -> 490, 345 -> 434 of them strong (>= 0.95), the weak ones about
	// the same (2026-10-20; the P1 station likewise, a bench tool:
	// ~1 dB at 200 Bd, ~2.5 dB at 100 Bd). The envelope difference (2) found
	// more but ~35 at impossible times. P2_P1_DISC: the discriminator;
	// P2_P1_TONE=2: the envelope (test).
	static const int tone = std::getenv("P2_P1_DISC") ? 0 : std::getenv("P2_P1_TONE") ? std::atoi(std::getenv("P2_P1_TONE")) : 1;
	if (tone) {                                     // the two tones' correlation over the bit, from the raw samples
		const int64_t lo = int64_t(a + 0.05 * w), hi = int64_t(a + 0.95 * w);
		if (lo < 1 || hi >= n_ || n_ - lo >= kRing - 1) return 0.0;
		std::complex<double> ch = 0, cl = 0;
		const double wh = -2 * M_PI * (cfg_.tone_hi + p1_df_) / cfg_.fs, wl = -2 * M_PI * (cfg_.tone_lo + p1_df_) / cfg_.fs;
		const std::complex<double> rh = std::polar(1.0, wh), rl = std::polar(1.0, wl);
		std::complex<double> ph = std::polar(1.0, wh * double(lo % 96000)), pl = std::polar(1.0, wl * double(lo % 96000));
		for (int64_t t = lo; t <= hi; t++) {
			const double x = raw_[size_t(t % kRing)];
			ch += x * ph; cl += x * pl; ph *= rh; pl *= rl;
		}
		return tone == 2 ? (std::abs(ch) - std::abs(cl)) / w : (std::norm(ch) - std::norm(cl)) / (w * w * 1e3);
	}
	a += gd_;
	const int64_t lo = int64_t(a + 0.2 * w), hi = int64_t(a + 0.8 * w);
	if (lo < 1 || hi >= n_ || n_ - lo >= kRing - 1) return 0.0;
	return pre_[size_t(hi % kRing)] - pre_[size_t((lo - 1) % kRing)];
}

double P2Station::p1_tone_snr_db(int64_t t0, int n) const
{
	// The reply can end just as we decide (EV_P1_CS comes 1.22 s after our
	// first bit, the reply ~1.09–1.22 s): measure what has arrived, if at
	// least three quarters of it (the full window returned 0 live and
	// rejected W5WK's CS4 that the same check passed in replay).
	if (t0 < 0 || n_ - t0 >= kRing - 1) return 0.0;
	if (t0 + n > n_) n = int(n_ - t0);
	if (n < int(0.75 * 12 * cfg_.fs / 100)) return 0.0;
	auto power = [&](double fq) {
		const double w = 2 * M_PI * fq / cfg_.fs, c = 2 * std::cos(w);
		double s1 = 0, s2 = 0;
		for (int k = 0; k < n; k++) { const double s0 = raw_[size_t((t0 + k) % kRing)] + c * s1 - s2; s2 = s1; s1 = s0; }
		return s1 * s1 + s2 * s2 - c * s1 * s2;
	};
	const double sig = power(cfg_.tone_lo + p1_df_) + power(cfg_.tone_hi + p1_df_);
	const double ref = power(cfg_.tone_hi + p1_df_ + 800.0) + power(cfg_.tone_hi + p1_df_ + 1000.0) + 1e-9;
	return 10.0 * std::log10(sig / ref + 1e-12);
}

bool P2Station::p1_tone_sums(int64_t a, int64_t b) const
{
	static const bool direct = std::getenv("P2_P1_DISC") || std::getenv("P2_P1_TONE") || std::getenv("P1_SEARCH_DIRECT");
	const double w = cfg_.fs / 100;
	ps_t0_ = std::max<int64_t>(1, a + int64_t(0.05 * w));
	const int64_t t1 = std::min<int64_t>(n_ - 1, b + int64_t(11 * w + 0.95 * w) + 1);
	if (direct || t1 < ps_t0_ || t1 - ps_t0_ + 2 > int64_t(ps_hi_.size()) || n_ - ps_t0_ >= kRing - 1) return false;
	ps_t1_ = t1;
	const double wh = -2 * M_PI * (cfg_.tone_hi + p1_df_) / cfg_.fs, wl = -2 * M_PI * (cfg_.tone_lo + p1_df_) / cfg_.fs;
	const std::complex<double> rh = std::polar(1.0, wh), rl = std::polar(1.0, wl);
	std::complex<double> ph = 1.0, pl = 1.0, ch = 0, cl = 0;
	ps_hi_[0] = 0; ps_lo_[0] = 0;
	for (int64_t t = ps_t0_; t <= t1; t++) {
		const double x = raw_[size_t(t % kRing)];
		ch += x * ph; cl += x * pl; ph *= rh; pl *= rl;
		if (((t - ps_t0_) & 1023) == 1023) { ph /= std::abs(ph); pl /= std::abs(pl); }   // keep the rotators on the unit circle
		ps_hi_[size_t(t - ps_t0_ + 1)] = ch; ps_lo_[size_t(t - ps_t0_ + 1)] = cl;
	}
	return true;
}

double P2Station::p1_bit_fast(double a) const
{
	const double w = cfg_.fs / 100;
	const int64_t lo = int64_t(a + 0.05 * w), hi = int64_t(a + 0.95 * w);
	if (lo < 1 || hi >= n_ || n_ - lo >= kRing - 1) return 0.0;
	if (lo < ps_t0_ || hi > ps_t1_) return bit_soft(a, 100);
	const std::complex<double> ch = ps_hi_[size_t(hi - ps_t0_ + 1)] - ps_hi_[size_t(lo - ps_t0_)];
	const std::complex<double> cl = ps_lo_[size_t(hi - ps_t0_ + 1)] - ps_lo_[size_t(lo - ps_t0_)];
	const double v = (std::norm(ch) - std::norm(cl)) / (w * w * 1e3);
	return v;
}

double P2Station::find_p1_code(int64_t a, int64_t b, const std::vector<uint16_t> &codes, uint16_t &code, int64_t &at) const
{
	double best = 0;
	code = 0;
	// Fix 75: a reply still arriving when we decide is scored on the bits
	// already in (at least 10 of 12; the five codes stay >= 3 bits apart,
	// either polarity). The decision is due ~1.24 s after our first bit
	// (our next transmission follows at 1.25 s) and a reply is 0.12 s long:
	// one arriving after ~1.12 s (a longer path or audio chain than the
	// RMSs we heard, 1.02-1.11 s with our 50 ms) read its last bits as
	// zeros, failed the weak-bit rule and was never seen. The loopback
	// bench lost the call phase at +60 ms this way (fix 73's notes).
	const int64_t bw = int64_t(cfg_.fs / 100);
	static const int min_bits = std::getenv("P1_FULL_REPLY") ? 12 : 10;
	static const bool partial_weak = std::getenv("P1_PARTIAL_WEAK") != nullptr;
	const bool fast = p1_tone_sums(a, b);          // the bits from running sums (same values, ~100x less work)
	for (int64_t t = a; t <= b; t += 2) {
		double s[12], norm = 0, mx = 0;
		int m = 0;                                   // bits received so far
		while (m < 12 && int64_t(double(t) + gd_ + m * cfg_.fs / 100 + 0.8 * bw) < n_) m++;
		if (m < min_bits) continue;
		for (int k = 0; k < 12; k++) {
			s[k] = k < m ? (fast ? p1_bit_fast(double(t) + k * cfg_.fs / 100) : bit_soft(double(t) + k * cfg_.fs / 100, 100)) : 0.0;
			norm += std::fabs(s[k]); mx = std::max(mx, std::fabs(s[k]));
		}
		// At most one weak bit (< 0.35 of the strongest). Requiring all 12
		// rejected W5WK's replies on the air: perfectly correlated, at the
		// same place every cycle, but one bit near zero each time (2026-10-15;
		// a bench tool: 6/40 found instead of 1/40; 0 false in a
		// recording with no answer; two weak bits gave false ones).
		int weak = 0;
		for (int k = 0; k < m; k++) weak += std::fabs(s[k]) < 0.35 * mx;
		if (norm <= 0 || weak > 1) continue;
		for (uint16_t c : codes) {
			double corr = 0;
			for (int k = 0; k < m; k++) corr += ((c >> k) & 1 ? s[k] : -s[k]);
			double sc = std::fabs(corr / norm);
			// Fix 85: a reply not yet complete (m < 12) counts only when strong
			// (KC0TPS 2026-10-18 00:41: a CS1 at 0.87 on 11 bits, nothing with the
			// 12th; we took it and the link set-up timed out)
			if (m < 12 && sc < 0.95 && !partial_weak) sc = std::min(sc, 0.74);
			if (sc > best) { best = sc; code = c; at = t; }
		}
	}
	// A code is only real if there is a signal: the bit-sign correlation
	// alone matched band noise once in ~60 cycles after the one-weak-bit
	// rule (a false CS1 on the air, 2026-10-15: +1.4 dB in that gap; real
	// replies +7.4 dB and up; the worst false match +4.6 dB: threshold +6).
	if (best > 0 && !std::getenv("P1_NO_SNR") && p1_tone_snr_db(at + int64_t(gd_), int(12 * cfg_.fs / 100)) < 6.0) { best = 0; code = 0; }
	return best;
}

std::vector<uint8_t> P2Station::call200(const std::string &call)
{
	// The bytes after the callee + NUL are filler in the reference unit's calls:
	// "*SCSPTC*" on a fresh (emulated, unlicensed) unit, leftovers of earlier
	// calls on a real one. The real reference unit's call that W5WK answered on the
	// air had 0x00 there (off-air recording, 2026-10-15); with 'T' ours was
	// never answered. Only callees shorter than 5 characters expose it.
	std::vector<uint8_t> v(8, 0);
	if (std::getenv("SIGNUM_P1_OLD_FILL")) v = {'*', 'S', 'C', 'S', 'P', 'T', 'C', '*'};   // experiment: the old fill
	if (std::getenv("P1_CALL_BAD200")) { v.assign(6, 0x5A); return v; }   // test: a 200 Bd part that grades badly (callee answers CS4)
	for (size_t i = 0; i < call.size() && i < 8; i++) v[i] = uint8_t(call[i]);
	if (call.size() < 8) v[call.size()] = 0;
	v.resize(6);
	// test (fix 180's grading measured on the reference unit): P1_CALL_FLIP200="i,j,..." flips those
	// bits (0 = LSB of the first byte) of our call's 200 Bd part
	if (const char *e = std::getenv("P1_CALL_FLIP200"))
		for (const char *q = e; *q; ) { const int b = std::atoi(q); if (b >= 0 && b < 48) v[size_t(b / 8)] ^= uint8_t(1 << (b % 8)); const char *c = std::strchr(q, ','); if (!c) break; q = c + 1; }
	return v;
}

void P2Station::queue_fsk(int64_t first, const std::vector<std::pair<int, std::vector<uint8_t>>> &segs)
{
	const bool inv = fsk_inv_;
	fsk_inv_ = !fsk_inv_;
	// Fix 243: a burst whose first bit is already past (our answer's instant, kP1Reply less our
	// round trip, comes before we have read what we answer: an audio path longer than ~90 ms
	// round trip) went out with its first bits cut, and nobody read it. It goes whole, as early
	// as it can (the lead-in shortened, its first bit 2 ms from now), late: a caller of ours reads
	// a late answer (fix 243's late look); the reference unit refuses it, as it did the cut one.
	// Bursts whose bits were whole are not moved. P1_LATE_CUT_OLD=1: the old way.
	static const bool cut_old = std::getenv("P1_LATE_CUT_OLD") != nullptr;
	if (!cut_old && cfg_.realtime && first < n_ + samples(0.002)) {
		if (fsk_late_logs_++ < 3)
			log("our FSK burst goes out " + std::to_string(int(std::lround(1000.0 * double(n_ + samples(0.002) - first) / cfg_.fs)))
			    + " ms late: its instant had passed (our audio path is longer than the answer time allows)");
		first = n_ + samples(0.002);
	}
	const int64_t onset = first - samples(cfg_.txd_s);
	std::vector<uint8_t> chips;                    // one bit value per sample
	for (int64_t t = onset; t < first; t++) chips.push_back(1);
	double pos = 0;
	for (const auto &s : segs) {
		const double spb = cfg_.fs / s.first;
		for (uint8_t b : s.second) {
			const double end = pos + spb;
			while (double(chips.size()) - double(first - onset) < end) chips.push_back(b);
			pos = end;
		}
	}
	TxBurst tb;
	tb.start = onset;
	tb.audio.resize(chips.size());
	double ph = 0;
	const double ramp = 0.002 * cfg_.fs;
	for (size_t k = 0; k < chips.size(); k++) {
		const int bit = chips[k] ^ (inv ? 1 : 0);
		ph += 2 * M_PI * (bit ? cfg_.tone_hi : cfg_.tone_lo) / cfg_.fs;
		double g = 1.0;
		if (double(k) < ramp) g = 0.5 - 0.5 * std::cos(M_PI * double(k) / ramp);
		if (double(chips.size() - k) < ramp) g = std::min(g, 0.5 - 0.5 * std::cos(M_PI * double(chips.size() - k) / ramp));
		tb.audio[k] = float(cfg_.amplitude * fsk_scale() * g * std::sin(ph));
	}
	tx_push(std::move(tb));
}

void P2Station::queue_accept(int64_t first, uint16_t code)
{
	queue_fsk(first, {{100, code_bits(code)}});
	const char *e = std::getenv(code == p3::kP3Accept ? "P3_TEST_ACCEPT_MUTE" : "P2_TEST_ACCEPT_MUTE");
	static int n = 0;
	if (e && n++ < std::atoi(e)) { std::fill(tx_.back().audio.begin(), tx_.back().audio.end(), 0.0f); log("test: our accept muted"); }
}

void P2Station::send_call(int64_t first)
{
	uint8_t f[9];
	std::memset(f, 0x0f, 9);
	f[0] = 0x55;
	std::memcpy(f + 1, remote_.data(), std::min<size_t>(8, remote_.size()));
	if (long_path_) f[1] = uint8_t(~f[1]);         // long path: the call's first byte inverted (as the firmware sends it, [bb-emu])
	std::vector<uint8_t> a, b;
	push_bits(a, f, 72);
	const auto p = call200(remote_);
	push_bits(b, p.data(), 48);
	queue_fsk(first, {{100, a}, {200, b}});
	my_first_ = first;
	p1_call_first_ = first;
	schedule(first + samples(cyc_s() - 0.01) - samples(cfg_.txd_s), EV_P1_CS, first);
}

void P2Station::send_p1_setup(int64_t first)
{
	uint8_t pkt[24];
	pkt[0] = p1_hdr_;
	std::string t = "1" + lower(cfg_.mycall) + "\r";
	// Test hook: P1_TEST_SETUP_TEXT=hex: this text after the call in the set-up
	// packet, escaped as the stream (as the reference unit sends the first text it has queued; our queue is
	// not taken from here).
	if (const char *e = std::getenv("P1_TEST_SETUP_TEXT")) {
		std::string x;
		for (size_t i = 0; e[i] && e[i + 1]; i += 2) { char h[3] = {e[i], e[i + 1], 0}; x += char(std::strtol(h, nullptr, 16)); }
		t += text::rle_escape(x);
	}
	// 200 Bd: 20 data bytes; 100 Bd (after CS4): 8 (same 0.96 s packet)
	const int nd = p1_slow_ ? 8 : 20;
	for (int i = 0; i < nd; i++) pkt[1 + i] = i < int(t.size()) ? uint8_t(t[size_t(i)]) : 0x1e;
	pkt[1 + nd] = cfg_.allow_p3 ? 0x31 : 0x11;      // bit 5: P3 offered
	// Fix 207: MYLevel 1: a plain P1 first packet (status: counter 1, ASCII), what the
	// P1 station sent as its first packet (header 0xAA, "1<call>\r").
	if (cfg_.p1_only) pkt[1 + nd] = 0x01;
	const uint16_t crc = crc16_ccitt_reflected(pkt + 1, size_t(nd + 1));
	pkt[2 + nd] = uint8_t(crc);
	pkt[3 + nd] = uint8_t(crc >> 8);
	if (std::getenv("P1_SETUP_BAD200") && !p1_slow_) pkt[2 + nd] ^= 0x5A;   // test: a 200 Bd set-up packet that fails its CRC
	std::vector<uint8_t> bits;
	push_bits(bits, pkt, (nd + 4) * 8);
	queue_fsk(first, {{p1_slow_ ? 100 : 200, bits}});
	my_first_ = first;
	schedule(first + samples(cyc_s() - 0.01) - samples(cfg_.txd_s), EV_P1_CS, first, 1);
}

void P2Station::acquire_call()
{
	if (n_ % 4) return;
	const double mx = p1_acq_max_hz();
	for (int k = 0; 25.0 * k <= mx + 1e-9; k++) {   // 0, +50, -50, +100, -100, ...
		const double d = (k & 1) ? 25.0 * (k + 1) : -25.0 * k;
		if (k && (n_ % 16 || std::fabs(d) > mx + 1e-9)) continue;
		p1_df_ = d;
		acquire_call_at();
		if (state_ != ST_LISTEN) return;          // found: p1_df_ stays
	}
	p1_df_ = 0;
}

void P2Station::acquire_call_at()
{
	if (n_ < acq_hold_until_) return;
	double t = double(n_) - gd_ - 72 * 96.0 - 48 * 48.0 - 2 - 60;
	if (t < 1) return;
	{   // cheap pre-check, same result: the first byte must be 0x55 in one
	    // polarity (0xAA in the other). Cuts the idle cost ~9x (esp32/).
		uint8_t b8[8], first;
		bits_at(t, 100, 8, false, b8);
		pack(b8, 8, &first);
		if (first != 0x55 && first != 0xAA) return;
	}
	uint8_t bits[72], bytes[9], want[8];
	std::memset(want, 0x0f, 8);
	std::memcpy(want, cfg_.mycall.data(), std::min<size_t>(8, cfg_.mycall.size()));
	for (int inv = 0; inv < 2; inv++) {
		bits_at(t, 100, 72, inv, bits);
		pack(bits, 72, bytes);
		if (std::getenv("P1_CALL_DUMP") && bytes[0] == 0x55 && n_ - last_call_dump_ > samples(0.5)) {   // test: what a call frame carries
			uint8_t b2[48], by2[6];
			bits_at(t + 72 * 96.0, 200, 48, inv, b2);
			pack(b2, 48, by2);
			char h[160]; int k = 0;
			for (int i = 0; i < 9; i++) k += std::snprintf(h + k, sizeof h - size_t(k), "%02x ", bytes[i]);
			k += std::snprintf(h + k, sizeof h - size_t(k), "| ");
			for (int i = 0; i < 6; i++) k += std::snprintf(h + k, sizeof h - size_t(k), "%02x ", by2[i]);
			log(std::string("call frame: ") + h);
			last_call_dump_ = n_;
		}
		// a long-path call carries our call's first byte inverted (manual 6.22.1, [bb-emu])
		const bool lp = bytes[0] == 0x55 && bytes[1] == uint8_t(~want[0]) && !std::memcmp(bytes + 2, want + 1, 7);
		if (bytes[0] != 0x55 || (!lp && std::memcmp(bytes + 1, want, 8))) continue;
		auto matches = [&](double tt) {
			uint8_t bb[72], by[9];
			bits_at(tt, 100, 72, inv, bb);
			pack(bb, 72, by);
			return by[0] == 0x55 && by[1] == (lp ? uint8_t(~want[0]) : want[0]) && !std::memcmp(by + 2, want + 1, 7);
		};
		int64_t lo = int64_t(t), hi = int64_t(t);
		while (hi - int64_t(t) < 90 && matches(double(hi + 1))) hi++;
		while (int64_t(t) - lo < 90 && matches(double(lo - 1))) lo--;
		t = (lo + hi) / 2.0;
		// P1_TEST_CALL_CENTRE_SHIFT=S: test, the 100 Bd span's centre moved S samples (-48: the bench's
		// p1-404/504 calls, read 5 ms early; fix 234).
		if (const char *e = std::getenv("P1_TEST_CALL_CENTRE_SHIFT")) t += std::atof(e);
		if (const int ta = test_call_answer(); ta != 1) {
			log("call received: test answer " + std::to_string(ta));
			if (ta == 2 || ta == 4) queue_fsk(int64_t(t) + samples(kP1Reply - cfg_.rt_latency_s + test_call_answer_s()), {{100, code_bits(ta == 2 ? p2arq::kP1Cs2 : p2arq::kP1Cs4)}});
			acq_hold_until_ = int64_t(t) + samples(1.2) + 72 * 96 + 48 * 48;
			return;
		}
		master_ = false;
		state_ = ST_SETUP_S;
		misses_ = 0;
		long_path_ = lp;
		int64_t first = int64_t(t);
		// Fix 180: the call's 200 Bd part grades the channel: our call + NUL (the
		// first 6 bytes at most; the rest is the caller's leftover fill) read
		// right -> CS1, else CS4 and the link at 100 Bd: its set-up packet then
		// comes at 100 Bd (8 data bytes) [spec: ARRL P1 section 4, "CS4 if
		// not, which leads to an instant reduction of data rate"; P1Station does
		// it standalone; bb-emu below]. We answered CS1 always, and a caller whose
		// 200 Bd part we could not read sent its set-up packet at 200 Bd, which we
		// mostly could not read either. P2_CALLEE_CS4=0: CS1 always (old);
		// P2_CALLEE_FORCE_CS4=1: test, every call graded bad.
		{
			uint8_t b2[48], by2[6], exp2[8];
			std::memset(exp2, 0, sizeof exp2);
			std::memcpy(exp2, cfg_.mycall.data(), std::min<size_t>(8, cfg_.mycall.size()));
			// Fix 211: the 200 Bd part read where it best matches what it should carry, within
			// +-60 samples (+-1.25 bits) of t + 72 bits. The 100 Bd span's centre was ~48 samples
			// early on 4 of 71 bench calls (about -18 Hz, found on the 0 or +50 Hz grid): the part
			// read one bit early (the call sign shifted one bit) and a clean call graded CS4.
			// The grade is whether bytes 1-5 come out right at that best position.
			// P1_CALL200_NOALIGN: the fixed position (old).
			// P1_TEST_CALL200_SHIFT=S: test, the 200 Bd part's nominal position moved S samples (-48: the
			// bench's one-bit-early read) before the alignment.
			double call_corr = 0;
			double t2 = t + 72 * 96.0 + (std::getenv("P1_TEST_CALL200_SHIFT") ? std::atof(std::getenv("P1_TEST_CALL200_SHIFT")) : 0.0);
			if (!std::getenv("P1_CALL200_NOALIGN")) {
				double best = -1e300, bt = t2;
				for (int d = -60; d <= 60; d += 2) {
					double c = 0;
					for (int k = 0; k < 48; k++) {
						const double v = bit_soft(t2 + d + k * 48.0, 200);
						const int want = (exp2[k / 8] >> (k % 8)) & 1;
						c += (want ^ (inv ? 1 : 0)) ? v : -v;
					}
					if (c > best) { best = c; bt = t2 + d; }
				}
				if (debug_ && bt != t2) log("call 200 Bd part best at " + std::to_string(int(bt - t2)) + " samples");
				t2 = bt;
				// Fix 234: the call's timing taken from that best position when it is a quarter of a
				// 200 Bd bit or more (12 samples) off the 100 Bd span's centre. The reference unit as master
				// places its window for our control signals where our answer to its call landed and
				// moves it slowly after; our P1 control signals after it are timed from each packet
				// read. On p1-404/504 the centre was 48 samples early (fix 211's one-bit-early read),
				// so our call answer went 5 ms early, and the real reference unit then repeated its packet for
				// 12 cycles after the speed-up (404; 504 likewise), the stall. Emulator: our call
				// answer 5 ms early (P2_TEST_CALL_ANSWER_MS=-5) repeats the first packet 7 cycles at
				// 100 Bd and stalls again after the speed-up; at 0 none. The reference unit's ~5 ms window
				// (fix 205) is that rule.
				// The 200 Bd part's 5 ms bits place the call more sharply than the 100 Bd span (the
				// bench's clean calls: best at -2..+6 samples). P1_CALL_RECENTRE_OFF=1: as before.
				// Only when the part reads right there (below): a part the channel spoilt places nothing.
				call_corr = bt - (t + 72 * 96.0);   // (the test shift of the read left out)
			}
			bits_at(t2, 200, 48, inv, b2);
			pack(b2, 48, by2);
			// The reference unit as callee ignores the first byte (a flipped bit 0 or 7 there:
			// CS1; any flipped bit in bytes 1-5, the NUL included: CS4) [bb-emu, P1_CALL_FLIP200].
			const size_t nchk = std::min<size_t>(6, cfg_.mycall.size() + 1);
			const bool ok200 = std::getenv("P2_CALLEE_GRADE_ALL") ? !std::memcmp(by2, exp2, nchk) : !std::memcmp(by2 + 1, exp2 + 1, nchk - 1);
			if (ok200 && std::fabs(call_corr) >= 12 && !std::getenv("P1_CALL_RECENTRE_OFF")) {
				first = int64_t(std::llround(t + call_corr));
				log("call timing moved " + std::to_string(int(call_corr)) + " samples (its 200 Bd part)");
			}
			// Fix 227: the reference unit's grade has a second part: a call whose 100 Bd part it reads only
			// narrowly gets CS4 however the 200 Bd part reads. Our span (hi - lo, samples of
			// our integrating detector) stands in for it; P1_CALL_SPAN_MIN=N: the threshold (0: off).
			static const int kSpanMin = std::getenv("P1_CALL_SPAN_MIN") ? std::atoi(std::getenv("P1_CALL_SPAN_MIN")) : 30;
			const bool span_ok = hi - lo >= kSpanMin;
			p1_slow_ = callee_cs4() && (!ok200 || !span_ok || std::getenv("P2_CALLEE_FORCE_CS4"));
			if (debug_) { char hx[32]; std::snprintf(hx, sizeof hx, "%02x%02x%02x%02x%02x%02x", by2[0], by2[1], by2[2], by2[3], by2[4], by2[5]); log(std::string("call 200 Bd part ") + hx + (ok200 ? " read" : " not read") + ", 100 Bd span " + std::to_string(hi - lo) + (span_ok ? "" : " (narrow)")); }
		}
		log(std::string(lp ? "call received (long path)" : "call received") + (p1_slow_ ? ", its 200 Bd part not read: CS4, link at 100 Bd" : "") + (p1_df_ != 0 ? " (" + std::to_string(int(p1_df_)) + " Hz grid)" : std::string()));
		p1_call_first_ = first;
		{ const double d = p1_df_; fsk_afc_reset(); p1_df_ = d; }
		fsk_afc_add(first + samples(0.01), first + 72 * 96 + 48 * 48 - samples(0.01), "call");
		queue_fsk(first + samples(kP1Reply - cfg_.rt_latency_s + test_call_answer_s()), {{100, code_bits(p1_slow_ ? p2arq::kP1Cs4 : kP1Cs1())}});
		schedule(first + samples(cyc_s()) + samples(0.96 + 0.012), EV_P1_PKT, first + samples(cyc_s()));
		return;
	}
}

bool P2Station::p1_setup_at(int64_t exp, int64_t &t_out, uint8_t *bytes) const
{
	const int baud = p1_slow_ ? 100 : 200, nb = p1_slow_ ? 96 : 192, nd = p1_slow_ ? 8 : 20;
	uint8_t bits[192];
	for (int64_t t = exp - samples(0.012); t <= exp + samples(0.012); t += 2)
		for (int inv = 0; inv < 2; inv++) {
			bits_at(double(t), baud, nb, inv, bits);
			pack(bits, nb, bytes);
			const uint16_t crc = uint16_t(bytes[2 + nd] | bytes[3 + nd] << 8);
			if (crc16_ccitt_reflected(bytes + 1, size_t(nd + 1)) == crc) { bytes[21] = bytes[1 + nd]; t_out = t; return true; }
		}
	return false;
}

bool P2Station::p1_setup_scan(int64_t exp, int baud, int nb, int nd, int64_t &t_out, uint8_t *bytes_out) const
{
	if (nb <= 0 || nb > 192 || nd < 0 || nd > 20) return false;
	const int64_t t0 = exp - samples(0.012), t1 = exp + samples(0.012);
	const int npos = int((t1 - t0) / 2) + 1;
	const int parts = std::max(1, std::min(par::ways(), npos));
	struct Hit { int idx = -1; uint8_t bytes[24]; };
	std::vector<Hit> hit(static_cast<size_t>(parts));
	par::parallel_for(parts, [&](int w) {
		uint8_t bits[192], by[24] = {};
		for (int i = npos * w / parts; i < npos * (w + 1) / parts; i++) {
			bits_at(double(t0 + 2 * int64_t(i)), baud, nb, false, bits);
			for (int inv = 0; inv < 2; inv++) {
				if (inv) for (int k = 0; k < nb; k++) bits[k] ^= 1;
				std::memset(by, 0, sizeof by);             // (pack(), the whole buffer)
				for (int k = 0; k < nb; k++) if (bits[k]) by[k / 8] |= uint8_t(1u << (k % 8));
				const uint16_t crc = uint16_t(by[2 + nd] | by[3 + nd] << 8);
				if (crc16_ccitt_reflected(by + 1, size_t(nd + 1)) != crc) continue;
				if (p1_slow_ && by[0] != 0x55 && by[0] != 0xAA) continue;
				hit[size_t(w)].idx = 2 * i + inv; std::memcpy(hit[size_t(w)].bytes, by, sizeof by);
				return;
			}
		}
	});
	for (const Hit &h : hit)
		if (h.idx >= 0) { t_out = t0 + 2 * int64_t(h.idx / 2); std::memcpy(bytes_out, h.bytes, 24); return true; }
	return false;
}

void P2Station::on_p1_packet(int64_t exp)
{
	if (state_ != ST_SETUP_S) return;
	uint8_t bytes[24];
	// fix 180: after our CS4 call answer the set-up packet comes at 100 Bd (8 data bytes)
	const int baud = p1_slow_ ? 100 : 200, nb = p1_slow_ ? 96 : 192, nd = p1_slow_ ? 8 : 20;
	int64_t t = 0;
	// Fix 227b test hook: P1_TEST_SETUP_MISS=N takes the first N set-up packets as not read.
	static int test_miss = std::getenv("P1_TEST_SETUP_MISS") ? std::atoi(std::getenv("P1_TEST_SETUP_MISS")) : 0;
	const bool test_skip = test_miss > 0 && (test_miss--, log("test: set-up packet taken as not read"), true);
	if (!test_skip && p1_setup_scan(exp, baud, nb, nd, t, bytes)) {
		bytes[21] = bytes[1 + nd];
		fsk_afc_add(t + samples(0.01), t + nb * int64_t(cfg_.fs / baud) - samples(0.01), "set-up packet");
		// fix 180: after our CS4 the reference unit as caller takes our P2 accept (P2
		// link, 600 of 600 characters) and our P3 accept [bb-emu] (the clean side saw its P2
		// accept refused, bb-clean P1-16; not here). P2_CALLEE_CS4_P1=1: a P1 link
		// instead, unless P3 (the clean side's rule).
		const bool slow_p2 = p1_slow_ && !(cfg_.allow_p3 && (bytes[21] & 0x20)) && std::getenv("P2_CALLEE_CS4_P1");
		if (!(bytes[21] & 0x10) || cfg_.p1_only || slow_p2) {
			// TODO O: a P1 link (the caller offers no P2, or we
			// are at MYLevel 1). The P1 station takes over: as if it had
			// answered the call itself, it reads this packet and answers.
			// P1_ONLY_DROP: the old refusal (test).
			if (!kP1Link || std::getenv("P1_ONLY_DROP")) { log("caller is P1 only"); disconnect("P1 only"); return; }
			log(cfg_.p1_only ? "MYLevel 1: P1 link" : !(bytes[21] & 0x10) ? "caller is P1 only: P1 link" : "set-up at 100 Bd after our CS4: P1 link");
			// Fix 227b: the P1 station's first look is the slot this set-up packet was read in (exp), not
			// the slot after the call. When we had missed the first copy, its first look was that missed
			// copy, again in the replayed past: "rx nothing", and its request, too late to be sent, used
			// up one polarity. Every control signal after it went out with the shift inverted against
			// the cycle (ours n,i,i,n where the reference-unit pair sends i,n,i,n [bb-emu, B's D/A]) and the
			// emulated reference unit never took one: its first packet 50 times (a compatibility run at -6 dB SNR, two seeds).
			// P1_SETUP_REQ_OLD=1: the old first look (and the old request time above).
			static const bool first_old = std::getenv("P1_SETUP_REQ_OLD") != nullptr;
			const int64_t slot_call = first_old ? p1_call_first_ : exp - samples(cyc_s());   // the "call" one cycle before the packet read
			p1_new(p1_replay_from(slot_call + samples(cyc_s()) - samples(0.010)));   // (its first look: this set-up packet)
			p1_->set_long_path(long_path_);
			master_ = false;
			p1_->adopt_slave(p1_caller_guess(bytes), slot_call, p1_slow_ ? fmodem::CS4 : fmodem::CS1, fsk_inv_);
			p1_take_over();
			return;
		}
		// Content "<level><call>\r<first text>": ASCII or, when the caller
		// compresses (status bits 2-3 = 1, as in P1), Huffman.
		const std::string body = text::decode((bytes[21] >> 2) & 3, bytes + 1, nd);
		const size_t cr = body.find('\r');
		std::string who = body.substr(0, cr);
		for (auto &ch : who) ch = char(std::toupper(uint8_t(ch)));
		if (!who.empty() && std::isdigit(uint8_t(who[0]))) who.erase(0, 1);
		remote_ = who;
		// The rest of the set-up packet is the caller's first text,
		// delivered once the link is up (the host layer puts it after the link-up event), through the
		// receive byte stream as every packet's text: the escapes, run-length triplets and SIs are
		// undone (it went in raw: a 0x1C escape of the reference unit's reached the host as two bytes).
		// P1_SETUP_TEXT_RAW=1: raw, as before (test).
		const std::string first = cr == std::string::npos ? std::string() : body.substr(cr + 1);
		auto deliver_first = [&]() {
			static const bool raw = std::getenv("P1_SETUP_TEXT_RAW") != nullptr;
			if (raw) { rx_ += first; return; }
			const size_t base = rx_.size();
			rx_ += rle_.feed(first);
			for (auto &e : rle_.si.ext_done) if (si_rx_.size() < 16) si_rx_.push_back({base + e.pos, std::move(e.body)});
			rle_.si.ext_done.clear();
			pstats_.rx_bytes += first.size();
			if (!first.empty()) led_rx_data_ = true;
		};
		if (cfg_.allow_p3 && (bytes[21] & 0x20)) {
			queue_accept(t + samples(kP1Reply - cfg_.rt_latency_s + test_call_answer_s()), p3::kP3Accept);
			acc_first_ = tx_.back().start;                // fix 222: the cycle grid's parity for our first PSK burst
			start_p3_slave(t - samples(0.0225) + samples(cyc_s() + 0.036));
			deliver_first();
			return;
		}
		queue_accept(t + samples(kP1Reply - cfg_.rt_latency_s + test_call_answer_s()), p2arq::kP2Accept);
		acc_first_ = tx_.back().start;                    // fix 222
		// The caller's first PSK packet starts 1.286 s after its P1
		// packet's onset (first bit - lead-in).
		const int64_t psk = t - samples(0.0225) + samples(cyc_s() + 0.036);
		enter_psk_irs(psk);
		p2_setup_watch_ = true;                        // until the first good packet: our P2-accept may be lost (TODO 0e)
		link_status(true, "");
		deliver_first();
		log("P2 link set up with " + remote_);
		return;
	}
	if (++misses_ > 3) { disconnect("no set-up packet"); return; }
	// ask again: after our CS4 call answer the request is CS4 (the reference unit's own, bb-clean P1-16), else CS1.
	// Fix 227b: in the reply slot of the packet we missed (its first bit + kP1Reply, as our call answer
	// and the P1 station's replies). It was queued 0.25 s earlier, before this decision (made at
	// exp + 0.972), so it was never sent: the caller heard nothing, and the reference unit then never
	// took our CS1 for its repeated first packet (a compatibility run at -6 dB SNR, two seeds: 0 characters, the
	// same packet 50 times; the reference-unit pair sends CS4 here [bb-emu]). P1_SETUP_REQ_OLD=1: the old time.
	static const bool req_old = std::getenv("P1_SETUP_REQ_OLD") != nullptr;
	queue_fsk(exp - samples(req_old ? 0.25 : 0.0) + samples(kP1Reply - cfg_.rt_latency_s + test_call_answer_s()), {{100, code_bits(p1_slow_ ? p2arq::kP1Cs4 : kP1Cs1())}});
	schedule(exp + samples(cyc_s()) + samples(0.96 + 0.012), EV_P1_PKT, exp + samples(cyc_s()));
}

void P2Station::on_p1_cs(int64_t ref, int which)
{
	if (state_ != ST_CALLING && state_ != ST_SETUP_M) return;
	uint16_t code = 0; int64_t at = 0;
	const std::vector<uint16_t> want = {p2arq::kP1Cs1, p2arq::kP1Cs2, p2arq::kP2Accept, p3::kP3Accept, p2arq::kP1Cs4};
	// Fix 243 (a long audio path): the answer to our previous packet, read whole now. We decide
	// 1.22 s after our first bit and need 10 of the answer's 12 bits by then; an answer timed for
	// the reference unit (a Signum callee: ~1.06 s after our first bit on the air) reaches us
	// after 1.12 s once our own audio round trip passes ~55 ms (loopback, --prime-ms 60: no
	// call got past P1). Missed at the decision, it went unseen, and the next cycle's answer
	// came just as late. The callee asks again when our packet does not come (its answer
	// again, or its accept again to a repeated set-up packet), so an answer read whole one
	// cycle later is taken for the packet after: the set-up packet (or the P2/P3 start) one
	// cycle later than with a short path. Only answers that were not yet complete at the
	// decision are looked at, read on all 12 bits (>= 0.95, the SNR rule); real time only.
	// P1_LATE_ANSWER_OFF=1: the old way.
	bool late_hit = false;
	{
		static const bool late_off = std::getenv("P1_LATE_ANSWER_OFF") != nullptr;
		const int64_t lref = p1_late_ref_, ldec = p1_late_dec_;
		const int lwhich = p1_late_which_;
		p1_late_ref_ = ref; p1_late_which_ = which; p1_late_dec_ = n_;
		if (!late_off && cfg_.realtime && lref >= 0 && lwhich == which && ref - lref == samples(cyc_s())) {
			const int64_t a0 = std::max(lref + samples(0.90), ldec - int64_t(gd_ + 11.8 * cfg_.fs / 100.0));
			uint16_t lc = 0; int64_t la = 0;
			const double ls = a0 <= lref + samples(1.35) ? find_p1_code(a0, lref + samples(1.35), want, lc, la) : 0.0;
			const bool take = ls >= 0.95 && (which == 0 ? (lc == p2arq::kP1Cs1 || lc == p2arq::kP1Cs4) : (lc == p3::kP3Accept || lc == p2arq::kP2Accept));
			if (ls >= 0.75 || debug_)
				log("p1 reply " + std::to_string(lc) + " at +" + std::to_string(double(la - lref) / cfg_.fs) + " s after our previous first bit, read after the decision"
				    + (take ? ": taken one cycle late (a long audio path)" : ": not taken"));
			if (take) {
				late_hit = true; code = lc; at = la;
				const double d = double(la - lref); p1_trk_n_++; p1_trk_sum_ += d; p1_trk_lo_ = std::min(p1_trk_lo_, d); p1_trk_hi_ = std::max(p1_trk_hi_, d);   // fix 207's track: its own packet's time
			}
		}
	}
	double sc = late_hit ? 1.0 : find_p1_code(ref + samples(0.90), ref + samples(1.35), want, code, at);
	// Fix 185: no answer read on the nominal tones while calling: the offset grid (as acquire_call).
	if (!late_hit && which == 0 && sc < 0.95 && fsk_afc_w_ == 0 && p1_df_ == 0) {
		const double mx = p1_acq_max_hz();
		bool grid_hit = false;
		for (int k = 1; 25.0 * k <= mx + 1e-9; k++) {
			const double d = (k & 1) ? 25.0 * (k + 1) : -25.0 * k;
			if (std::fabs(d) > mx + 1e-9) continue;
			p1_df_ = d;
			uint16_t c2 = 0; int64_t a2 = 0;
			const double s2 = find_p1_code(ref + samples(0.90), ref + samples(1.35), want, c2, a2);
			// Fix 207: an answer on the grid counts only if the FSK offset measured on it (fix 108)
			// lies within 45 Hz of that grid offset, else the next grid offset is tried (a control
			// signal can read 1.0 50 Hz off its tones: an answer at +80 Hz is found on the +50 Hz
			// grid, one at +100 Hz there too, measured 100 Hz, and then taken on the +100 Hz grid). In white noise alone (no station, 36 seeds x
			// 55 calls, lockstep) the grid gave 5 false call answers (each a lost call: the set-up
			// packet then goes unanswered), 4 of them < 0.95, one 1.0 (10 bits read); 4 of the 5
			// measured no clear peak, the 5th -76.75 Hz on the -100 Hz grid. A real answer at an
			// offset measures it within 1 Hz. P1_ACQ_WEAK_ANY=1: the old way.
			if (s2 > sc && s2 >= 0.75 && !std::getenv("P1_ACQ_WEAK_ANY")) {
				double hz = 0;
				if (!fsk_offset_hz(a2 + samples(0.005), a2 + 12 * 96 - samples(0.005), hz) || std::fabs(hz - d) > 45.0) {
					if (debug_) log("p1 reply " + std::to_string(c2) + " (" + std::to_string(s2) + ") on the " + std::to_string(int(d)) + " Hz grid: no FSK offset near it, not taken");
					continue;
				}
			}
			if (s2 > sc && s2 >= 0.75) { sc = s2; code = c2; at = a2; grid_hit = true; break; }
		}
		// Fix 207: an answer from the nominal tones (no grid offset beat it) left p1_df_ at the
		// last offset tried (-100 Hz): logged as found there, and the FSK measurement centred
		// on it. P1_ACQ_OLD_DF=1: the old way.
		if (!grid_hit && !std::getenv("P1_ACQ_OLD_DF")) p1_df_ = 0;
		if (sc < 0.75 || code == 0) p1_df_ = 0;
		else if (p1_df_ != 0) log("p1 reply on the " + std::to_string(int(p1_df_)) + " Hz grid");
	}
	if (debug_) log("p1 reply code " + std::to_string(code) + " score " + std::to_string(sc));
	if (sc >= 0.75 && !late_hit)                     // calibration: their reply time as we hear it
		log("p1 reply " + std::to_string(code) + " at +" + std::to_string(double(at - ref) / cfg_.fs) + " s after our first bit");
	const int64_t next = ref + samples(cyc_s());
	if (sc < 0.75) code = 0;
	if (code) fsk_afc_add(at + samples(0.005), at + 12 * 96 - samples(0.005), "P1 reply");
	if (code && sc >= 0.95 && !late_hit) { const double d = double(at - ref); p1_trk_n_++; p1_trk_sum_ += d; p1_trk_lo_ = std::min(p1_trk_lo_, d); p1_trk_hi_ = std::max(p1_trk_hi_, d); }   // fix 207
	// Fix 78: a weak reply (< 0.95) earlier than any callee can answer is
	// noise. The firmware answers 0.995 s after our first bit with no audio
	// delay (emulator); on the air our round trip (rt_latency + 0.065) adds
	// to it and the RMS's radio and the path only add more. Our recordings
	// (125 calls, a bench tool --list, round trip 50 ms): every
	// real answer at 1.059-1.115 s; the weak matches before 1.04 s false (a
	// CS1 at 1.033 that lost a KB5LZK call, a P3-accept to a call packet).
	// Real time only (the lockstep emulator has no audio delay).
	// (2026-10-20: 10 ms later, 1.000 s: with the tone-energy bits two weak
	// false answers came at 1.043 / 1.045 s on the bench's timing, one in a
	// session where nobody answered; real answers on the air start at 1.059 s.
	// P1_EARLY_990: the old limit.)
	static const double early = std::getenv("P1_EARLY_990") ? 0.990 : 1.000;
	if (code && sc < 0.95 && cfg_.realtime && !std::getenv("P1_NO_EARLY") && double(at - ref) / cfg_.fs < early + cfg_.rt_latency_s + 0.065) {
		log("p1 reply " + std::to_string(code) + " score " + std::to_string(sc) + " too early for an answer: ignored");
		code = 0;
	}
	if (which == 0) {
		// The callee answers our call with CS1 (link at 200 Bd) or CS4 (at
		// 100 Bd: it graded our call's 200 Bd part badly). We took only CS1:
		// W5WK answered every call with CS4 on the air (2026-10-15, decoded
		// from our recording) and we went on calling for 90 s.
		if (code == p2arq::kP1Cs1 || code == p2arq::kP1Cs4) {
			state_ = ST_SETUP_M;
			misses_ = 0;
			p1_slow_ = code == p2arq::kP1Cs4;
			if (debug_ || p1_slow_) log(std::string("call answered with ") + (p1_slow_ ? "CS4: link at 100 Bd" : "CS1: link at 200 Bd"));
			send_p1_setup(next);
		} else if (++misses_ > cfg_.timeout_cycles) disconnect("no answer");
		else send_call(next);
		return;
	}
	if (cfg_.p1_only && (code == p3::kP3Accept || code == p2arq::kP2Accept)) code = 0;   // fix 207: not asked for
	// Test hook (fix 218): P2_TEST_ACCEPT_MISS=N / P3_TEST_ACCEPT_MISS=N: the callee's first N
	// P2- / P3-accepts taken as nothing heard (the set-up packet goes again), to see what a
	// callee does when its accept is lost.
	if (code == p3::kP3Accept || code == p2arq::kP2Accept) {
		const char *e = std::getenv(code == p3::kP3Accept ? "P3_TEST_ACCEPT_MISS" : "P2_TEST_ACCEPT_MISS");
		static int n = 0;
		if (e && n++ < std::atoi(e)) { log("test: accept ignored"); code = 0; }
	}
	if (code == p3::kP3Accept && cfg_.allow_p3) { start_p3_master(ref); return; }
	if (code == p2arq::kP2Accept) {
		// P2 starts at level 2, or at level 1 after a 100 Bd (CS4) link: the
		// callee expects level 1 then and repeats P2-accept until it decodes
		// a level-1 packet (fix 43).
		level_ = p1_slow_ ? 1 : 2; long_ = false;
		counter_ = 1;
		if (p1_sd_) hdr_flip_ ^= 1;                  // fix 82: the toggle restarts after the speed-down
		psk_swap_ = false;
		expect_ack_ = 0;
		state_ = ST_ISS; new_send_turn();
		misses_ = 0;
		link_status(true, "");
		log("P2 link set up");
		// First PSK packet 1.286 s after our P1 packet's onset.
		next_start_ = ref + samples(1.261 + (cyc_s() - 1.25) + cfg_.first_psk_adjust_s);   // reference: 1.261 s after the first bit
		grid_ = next_start_;
		build_packet(true);
	} else if (code == p2arq::kP1Cs4 && !p1_slow_ && !std::getenv("P1_NO_SETUP_CS4")) {
		// Fix 82: CS4 to our 200 Bd set-up packet is a speed-down (protocol notes
		// §2.7): resend it at 100 Bd. We repeated it at 200 Bd until the link
		// set-up timed out: KB5LZK answered CS1 to our call, then CS4 to 17
		// set-up packets (40 m, 2026-10-17 20:17).
		// After a speed-down both sides use header 0x55 (protocol notes §2.7)
		// and the header toggle restarts: emulator, set-up packet spoilt at
		// 200 Bd: the firmware repeats CS1 5 times, then CS4. With header
		// 0xAA or without the toggle our data packets were not taken (P3:
		// 0/4 without the toggle; P2: 0/2 with 0xAA), with both: 4/4.
		p1_slow_ = true; p1_sd_ = !std::getenv("P1_SD_NOFLIP");
		p1_hdr_ = 0x55;
		if (const char *h = std::getenv("P1_SPEEDDOWN_HDR")) p1_hdr_ = uint8_t(std::strtol(h, nullptr, 0));
		misses_ = 0;
		log("set-up packet answered with CS4: resending at 100 Bd");
		send_p1_setup(next);
	} else if (code == p2arq::kP1Cs2 || (code == p2arq::kP1Cs1 && p1_slow_ && !std::getenv("P1_OLD_SLOW_ACK"))) {
		// Acknowledged as plain P1 data: the callee is P1 only (or at
		// MYLevel 1). TODO O: go on as a P1 link from our next packet.
		// At 100 Bd (the call answered CS4, or our 200 Bd set-up packet
		// stepped down by CS4) the acknowledgement is CS1: "CS4 serves as the
		// REQUEST CS for the first 100 Bd block, acknowledged with CS1"
		// [spec: P1 1990, Geschwindigkeitsverminderung]; the emulated
		// reference unit at MYLevel 1 answered our 100 Bd set-up packet with CS1 for
		// ever while we read CS1 as "again" [bb-emu, owner's p1-cs4, fix 159].
		// A P2/P3 callee answers with its accept code instead.
		// P1_ONLY_DROP: the old disconnect (test). P1_OLD_SLOW_ACK: CS1 = again.
		if (!kP1Link || std::getenv("P1_ONLY_DROP")) { disconnect("remote is P1 only"); return; }
		log("callee is P1 only: P1 link");
		p1_new(p1_replay_from(n_));                    // (it reads only what comes from here: the answers to its next packet)
		p1_->set_long_path(long_path_);
		p1_->adopt_master(remote_, p1_call_first_, ref, !p1_slow_, p1_hdr_, 1, code == p2arq::kP1Cs1 ? fmodem::CS1 : fmodem::CS2, fsk_inv_, txq_);
		// Fix 207: fix 183's reply track (P1 sender) starts from the replies the set-up heard
		// clearly (2 or more, within 6 ms of each other), not empty. Empty, a false CS4 (0.83,
		// 17 ms early) to our first P1 packet in noise was taken as a speed-down; the link then
		// never read the reference unit's break-in (0 of 600 characters). P1_NO_TRACK_SEED=1: off.
		if (p1_trk_n_ >= 2 && p1_trk_hi_ - p1_trk_lo_ <= double(samples(0.006))) p1_->seed_reply_track(p1_trk_sum_ / p1_trk_n_, ref);
		txq_.clear(); pending_used_ = 0;
		p1_take_over();
	} else if (++misses_ > cfg_.timeout_cycles) {
		disconnect("no answer to set-up");
	} else {
		send_p1_setup(next);                           // CS1 again / nothing: repeat
	}
}

int64_t P2Station::queue_psk(int64_t start, const std::vector<uint8_t> &words, bool may_move)
{
	// The leading carrier (nibble A) alternates between the low and the
	// high tone with every PSK transmission of a station, starting low;
	// the reference receiver expects it.
	if (acc_first_ >= 0) first_psk_parity(start);    // fix 222: the callee's first PSK burst
	// Fix 257 (a long audio path): a short burst (a control signal, B1, A2, a break-in) whose
	// leading tail is already past goes whole, its leading tail starting 1 ms from now. Its
	// instant (the answer time less our round trip) came before we had read what we answer; it
	// was sent with its first part cut (its reference pulse and first steps: two of our stations
	// at a 100 ms prime read every such control signal as no letter, "cs - score 0.62-0.71"). A
	// late whole burst is read by our looks, which take what is heard (fix 243); a moved break-in
	// (B1, A2, a data-phase break-in) moves what follows it with it (queue_words returns where it
	// went), as the far end takes its grid from it. P3 only; data packets keep their slot.
	// Fix 269 (on the air 2026-11-19, Winlink RMSs, reference-unit firmware): while our late answers
	// went whole and 5-17 ms late every link broke (receiving stuck at levels 1-2, our break-in
	// answered Q until the set-up timeout); cut (0.3.4, P3_LATE_CUT_OLD=1) the links worked, the cuts
	// 0.7-4.9 ms, all inside the 12 ms leading tail (pulse 0 whole). So: (1) a burst whose pulse 0 is
	// still ahead loses only part of its leading tail and is never moved, with every station; (2) a
	// burst whose pulse 0 is past goes whole and moved only once the other station is known to be a
	// Signum (its marker or encryption record read on this link), else it is cut as before fix 257.
	// P3_LATE_WHOLE=1: (2) with every station (two Signums with a long audio path, whose set-up
	// comes before any marker). P3_LATE_CUT_OLD=1: always cut. P3_LATE_257=1: fix 257 exactly.
	static const bool as_257 = std::getenv("P3_LATE_257") != nullptr;
	if (may_move && (as_257 ? cfg_.realtime && !std::getenv("P3_LATE_CUT_OLD") : late_psk_whole())) {
		const int64_t pre = int64_t(std::lround(p2::preroll(cfg_.fs)));
		if (as_257 ? start - pre < n_ + samples(0.001) : start < n_ + samples(0.001)) {
			const int64_t moved = n_ + samples(0.001) + pre;
			if (psk_late_logs_++ < 3)
				log("our burst goes out " + std::to_string(int(std::lround(1000.0 * double(moved - start) / cfg_.fs)))
				    + " ms late, whole: its instant had passed (our audio path is longer than the answer time allows)");
			start = moved;
		}
	}
	TxBurst tb;
	tb.start = start - int64_t(std::lround(p2::preroll(cfg_.fs)));   // the pulse's leading tail
	// Transmit AFC (P2): shift by the offset we hear the peer at, so that it
	// hears us near zero offset (the two radios' offsets are symmetric).
	// Without it a drifting radio pair broke our break-ins (RF bench).
	const double shift = !p3_ && cfg_.tx_afc && std::fabs(afc_hz()) > 0.5 ? afc_hz() : 0.0;
	if (words.size() > 100) {
		// A long packet: ~256k pulse-shape evaluations, 10-30 ms on the RF
		// bench's audio loop (40 ms cushion). Made on a worker; it starts
		// ~50 ms later. Short bursts stay here (never queued behind a decode).
		// P3 SL1 long packets too (2.5 s of audio; made on the audio loop
		// until 2026-10-18).
		const PremodKey k = psk_key(words, shift);
		if (cfg_.realtime) {
			tb.fut = premod_take(k, &tb.tr, &tb.prog);
			if (!tb.fut.valid() && !tb.prog) {
				tb.tr = std::make_shared<TxTrace>();
				if (pieces_key(k)) { tb.prog = prog_new(k); submit_prog(tb.prog, tb.tr, true); }
				else {
					const float fs = cfg_.fs, lo = cfg_.tone_lo, hi = cfg_.tone_hi;
					tb.fut = pool_.submit(traced(tb.tr, [k, fs, lo, hi]() { return premod_make(k, fs, lo, hi); }), true).share();
				}
			} else if (tb.tr) tb.tr->hit = true;
			if (tb.tr) tb.tr->queued_n = n_;
			psk_swap_ = !psk_swap_;
			tx_push(std::move(tb));
			return start;
		}
		if (premod_check()) premod_take(k);
	}
	tb.audio = p3_ ? p3::modulate_cs(words, cfg_.fs, psk_swap_, p3_psk_amp())
	               : p2::modulate(words, cfg_.fs, cfg_.tone_lo, cfg_.tone_hi, psk_swap_, p2_amp(), 0.0f);
	if (shift != 0.0) shift_audio(tb.audio, shift, cfg_.fs);
	psk_swap_ = !psk_swap_;
	tx_push(std::move(tb));
	return start;
}

}  // namespace fmodem
