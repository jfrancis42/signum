// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// P1Station member functions (out of p1_station.hpp, so that they build in parallel).
#include "p1_station.hpp"

namespace fmodem {

P1Station::P1Station(const P1Config &c)
	: cfg_(c), disc_(c.fs, (c.tone_lo + c.tone_hi) / 2 + c.rx_shift_hz, c.tone_hi - c.tone_lo)
{
	soft_.assign(kRing, 0.0f);
	raw_.assign(kRing, 0.0f);
	if (cfg_.rx_mute_delay > 0) ptt_hist_.assign(kRing, 0);   // fix 243
	gd_ = (disc_.taps() - 1) / 2.0 + 0.5;          // FIR group delay + discriminator's one-sample lag
	pre_.assign(kRing, 0.0);
	for (auto &ch : cfg_.mycall) ch = char(std::toupper(ch));
}

void P1Station::connect(const std::string &call)
{
	if (state_ != ST_IDLE && state_ != ST_LISTEN) return;
	remote_ = call;
	for (auto &ch : remote_) ch = char(std::toupper(ch));
	long_path_ = !remote_.empty() && (remote_[0] == '!' || remote_[0] == ';');
	if (long_path_) remote_.erase(0, 1);
	master_ = true;
	state_ = ST_CALLING;
	next_first_bit_ = n_ + samples(0.1);
	schedule_call();
	log("calling " + remote_ + (long_path_ ? " (long path)" : ""));
}

void P1Station::adopt_slave(const std::string &caller, int64_t call_first, Cs ack, bool next_inv)
{
	remote_ = caller; master_ = false; state_ = ST_IRS; fast_ = ack != CS4; ack_cs_ = ack;
	cs_ref_ok_ = pol_ok_ = false;
	ident_pending_ = true; ident_buf_.clear();
	last_rx_first_ = call_first; last_rx_counter_ = -1; misses_ = 0; tx_inv_ = next_inv;
	rx_fast_ = fast_; rx_bad_ = rx_good100_ = 0; rx_up_after_ = kRxUpAfter; rx_up_try_ = false; rx_up_wait_ = false;
	const int64_t next = call_first + samples(cyc_s());
	events_.push_back({next + samples(0.96 + 0.012), EV_PACKET_DUE, next});
	log("P1 link with " + remote_ + " (callee)");
}

void P1Station::adopt_master(const std::string &callee, int64_t call_first, int64_t pkt_first, bool fast, uint8_t header, int counter, Cs acked, bool next_inv, const std::string &txq)
{
	remote_ = callee; master_ = true; state_ = ST_ISS; fast_ = fast; grid0_ = call_first;
	cs_ref_ok_ = pol_ok_ = false;
	my_first_ = pkt_first; header_ = header; counter_ = counter; last_rx_cs_ = acked; tx_inv_ = next_inv;
	txq_ = txq; pending_used_ = 0; sent_text_ = false; misses_ = 0; last_was_cs4_ = false; up_wait_ = false;
	next_first_bit_ = pkt_first + samples(cyc_s());
	log("P1 link with " + remote_ + " (caller)");
	build_packet(true);
}

int16_t P1Station::tick(int16_t rx_sample)
{
	// Receive path (half duplex: deaf while transmitting).
	const int64_t i = n_ % kRing;
	const bool muted = cfg_.rx_mute_delay > 0 ? (n_ > cfg_.rx_mute_delay && ptt_hist_[size_t((n_ - 1 - cfg_.rx_mute_delay) % kRing)]) : ptt_;
	const float x = muted ? 0.0f : float(rx_sample);
	const float d = disc_.process(x);
	soft_[i] = d;
	raw_[i] = x;
	pre_[i] = (n_ ? pre_[(n_ - 1) % kRing] : 0.0) + d;
	if (tone_det_) {                                // experiment: per-tone correlation sums (tone-energy bits)
		if (chi_.empty()) { chi_.assign(kRing, {0, 0}); clo_.assign(kRing, {0, 0}); }
		const std::complex<double> phi = std::polar(1.0, -2 * M_PI * (cfg_.tone_hi + cfg_.rx_shift_hz) * double(n_) / cfg_.fs), plo = std::polar(1.0, -2 * M_PI * (cfg_.tone_lo + cfg_.rx_shift_hz) * double(n_) / cfg_.fs);
		chi_[i] = (n_ ? chi_[(n_ - 1) % kRing] : 0.0) + double(x) * phi;
		clo_[i] = (n_ ? clo_[(n_ - 1) % kRing] : 0.0) + double(x) * plo;
	}

	// Scheduled decisions.
	while (!events_.empty() && events_.front().at <= n_) {
		Event e = events_.front();
		events_.pop_front();
		handle(e);
	}
	if (state_ == ST_LISTEN) acquire_call();

	// Transmit path.
	const int16_t y = modulate();
	if (!ptt_hist_.empty()) ptt_hist_[size_t(n_ % kRing)] = ptt_;
	n_++;
	return y;
}

double P1Station::bit_soft(double a, int baud) const
{
	const double w = spb(baud);
	if (tone_det_ && !chi_.empty()) {               // tone energy over the bit (no discriminator delay)
		const int64_t lo = int64_t(a + 0.05 * w), hi = int64_t(a + 0.95 * w);
		if (lo < 1 || hi >= n_ || n_ - lo >= kRing - 1) return 0.0;
		const double eh = std::norm(chi_[hi % kRing] - chi_[(lo - 1) % kRing]), el = std::norm(clo_[hi % kRing] - clo_[(lo - 1) % kRing]);
		return (eh - el) / (w * 1e4);
	}
	a += gd_;
	const int64_t lo = int64_t(a + 0.2 * w), hi = int64_t(a + 0.8 * w);
	if (lo < 1 || hi >= n_ || n_ - lo >= kRing - 1) return 0.0;
	return pre_[hi % kRing] - pre_[(lo - 1) % kRing];
}

double P1Station::find_cs(int64_t a, int64_t b, Cs &cs, int64_t &at, bool &inv) const
{
	double best = 0;
	cs = CS_NONE;
	// A CS still arriving when we decide is scored on the bits already in,
	// at least 10 of 12 (the four codes stay >= 3 bits apart on their first
	// 10, either polarity): as P2Station's call answers (fix 75).
	const double w = spb(100);
	for (int64_t t = a; t <= b; t += 2) {
		double s[12], norm = 0, mx = 0;
		int m = 0;                                   // bits received so far
		while (m < 12 && int64_t(double(t) + gd_ + m * w + 0.8 * w) < n_) m++;
		if (m < (std::getenv("P1_FULL_REPLY") ? 12 : 10)) continue;
		for (int k = 0; k < 12; k++) {
			s[k] = k < m ? bit_soft(t + k * w, 100) : 0.0;
			if (k >= m) continue;
			norm += std::fabs(s[k]);
			mx = std::max(mx, std::fabs(s[k]));
		}
		// Every bit must carry signal: a window that is mostly silence
		// (our own muted transmission) and only touches the CS's first
		// bits would otherwise "correlate perfectly" with some code. At most
		// one bit may be weak (< 0.35 of the strongest), as P2Station's
		// control signals (fix 37: real replies with one bit near zero on
		// the air; fix 161 here). P1_CS_ALL_BITS: every bit (old).
		int weak = 0;
		for (int k = 0; k < m; k++) weak += std::fabs(s[k]) < 0.35 * mx;
		if (norm <= 0 || weak > (cs_all_bits() ? 0 : 1)) continue;
		for (int c = 1; c <= 4; c++) {
			double corr = 0;
			for (int k = 0; k < m; k++) corr += ((kCsBits[c] >> k) & 1 ? s[k] : -s[k]);
			corr /= norm;
			double sc = std::fabs(corr);
			// as P2Station's call answers (fix 85): a CS not yet complete counts
			// only when strong (a misread CS here acknowledges a lost packet)
			if (m < 12 && sc < 0.95 && !std::getenv("P1_PARTIAL_WEAK")) sc = std::min(sc, 0.74);
			if (sc > best) { best = sc; cs = Cs(c); at = t; inv = corr < 0; }
		}
	}
	// A control signal is only real if there is a signal: the power at the two
	// tones at least 6 dB over two tones outside the signal (fix 38's check,
	// with fix 37's weak bit; fix 161). P1_NO_SNR: off.
	if (best > 0 && !cs_all_bits() && !std::getenv("P1_NO_SNR") && tone_snr_db(at, int(12 * w)) < 6.0) { best = 0; cs = CS_NONE; }
	return best;
}

double P1Station::find_cs_expected(int64_t a, int64_t b, bool want_inv, Cs &cs, int64_t &at) const
{
	double best = 0;
	cs = CS_NONE;
	const double w = spb(100);
	for (int64_t t = a; t <= b; t += 2) {
		if (int64_t(double(t) + gd_ + 11 * w + 0.8 * w) >= n_) continue;   // the whole control signal
		double s[12], norm = 0;
		for (int k = 0; k < 12; k++) { s[k] = bit_soft(t + k * w, 100); norm += std::fabs(s[k]); }
		if (norm <= 0) continue;
		for (int c = 1; c <= 4; c++) {
			double corr = 0;
			for (int k = 0; k < 12; k++) corr += ((kCsBits[c] >> k) & 1 ? s[k] : -s[k]);
			corr /= norm;
			const double sc = want_inv ? -corr : corr;
			if (sc > best) { best = sc; cs = Cs(c); at = t; }
		}
	}
	if (best > 0 && tone_snr_db(at, int(12 * w)) < 6.0) { best = 0; cs = CS_NONE; }
	return best;
}

double P1Station::tone_snr_db(int64_t t0, int n) const
{
	if (t0 < 0 || n_ - t0 >= kRing - 1) return 0.0;
	if (t0 + n > n_) n = int(n_ - t0);
	if (n < int(0.75 * 12 * cfg_.fs / 100)) return 0.0;
	auto power = [&](double fq) {
		const double w = 2 * M_PI * fq / cfg_.fs, c = 2 * std::cos(w);
		double s1 = 0, s2 = 0;
		for (int k = 0; k < n; k++) { const double s0 = raw_[size_t((t0 + k) % kRing)] + c * s1 - s2; s2 = s1; s1 = s0; }
		return s1 * s1 + s2 * s2 - c * s1 * s2;
	};
	const double sig = power(cfg_.tone_lo + cfg_.rx_shift_hz) + power(cfg_.tone_hi + cfg_.rx_shift_hz);
	const double ref = power(cfg_.tone_hi + cfg_.rx_shift_hz + 800.0) + power(cfg_.tone_hi + cfg_.rx_shift_hz + 1000.0) + 1e-9;
	return 10.0 * std::log10(sig / ref + 1e-12);
}

int64_t P1Station::best_timing(int64_t a, int64_t b, int baud) const
{
	const int nb = baud == 200 ? 192 : 96;
	const double w = spb(baud);
	double best = -1; int64_t bt = (a + b) / 2;
	for (int64_t t = a; t <= b; t += 2) {
		double e = 0;
		for (int k = 0; k < nb; k++) e += std::fabs(bit_soft(double(t) + k * w, baud));
		if (e > best) { best = e; bt = t; }
	}
	return bt;
}

bool P1Station::find_packet(int64_t a, int64_t b, RxPacket &p, bool allow_bk, bool focus) const
{
	uint8_t bits[200], bytes[25];
	focus = focus && !focus_old();
	int64_t ft[2] = {0, 0};                         // fix 249: the best timing at 200 / 100 Bd
	if (focus) { ft[0] = best_timing(a, b, 200); ft[1] = best_timing(a, b, 100); }
	for (int64_t t = a - (focus ? focus_half() : 0); t <= b + (focus ? focus_half() : 0); t += focus ? 1 : 2)
		for (int baud : {200, 100}) {
			if (focus && !focus_take(t, ft[baud == 200 ? 0 : 1], baud)) continue;
			const int nb = baud == 200 ? 192 : 96, db = baud == 200 ? 160 : 64;
			for (int inv = 0; inv < 2; inv++) {
				bits_at(double(t), baud, nb, inv, bits);
				pack(bits, nb, bytes);
				const int body = db / 8 + 1;
				const uint16_t crc = uint16_t(bytes[1 + body] | bytes[2 + body] << 8);
				// The header is 55 or AA [spec: P1 description, header
				// byte]: ~400 positions, polarities and speeds per look made
				// random CRC passes, ~1 % of looks at low SNR (a bench tool),
				// garbage delivered as text. P1_ANY_HEADER: the old check (test).
				static const bool any_hdr = std::getenv("P1_ANY_HEADER") != nullptr;
				if (!any_hdr && bytes[0] != 0x55 && bytes[0] != 0xAA) continue;
				if (crc16_ccitt_reflected(bytes + 1, size_t(body)) == crc) {
					p.baud = baud; p.inv = inv; p.header = bytes[0]; p.status = bytes[body];
					p.data_bits = db; std::memcpy(p.data, bytes + 1, size_t(db / 8)); p.first = t; p.bk = false;
					return true;
				}
			}
		}
	if (!allow_bk) return false;
	// BK packet: CS3 at 100 Bd, then at 200 Bd 144 data bits + status + CRC
	// (at 100 Bd: CS3 + 4 unused bits, then 56 data bits + status + CRC).
	Cs cs; int64_t at; bool inv;
	if (find_cs(a, b, cs, at, inv) < 0.75 || cs != CS3) return false;
	for (int baud : {200, 100}) {
		const double start = at + 12 * spb(100) + (baud == 100 ? 4 * spb(100) : 0);
		const int db = baud == 200 ? 144 : 56, nb = db + 24;
		for (int inv2 = 0; inv2 < 2; inv2++)
			for (int adj = -16; adj <= 16; adj += 2) {
				bits_at(start + adj, baud, nb, inv2, bits);
				pack(bits, nb, bytes);
				const int body = db / 8 + 1;
				const uint16_t crc = uint16_t(bytes[body] | bytes[body + 1] << 8);
				if (crc16_ccitt_reflected(bytes, size_t(body)) == crc) {
					p.baud = baud; p.inv = inv; p.header = 0; p.status = bytes[body - 1];
					p.data_bits = db; std::memcpy(p.data, bytes, size_t(db / 8)); p.first = at; p.bk = true;
					return true;
				}
			}
	}
	return false;
}

bool P1Station::marq_decode(int64_t exp, int baud, RxPacket &p)
{
	if (std::getenv("P1_NO_MARQ")) return false;
	const int nb = baud == 200 ? 192 : 96, db = baud == 200 ? 160 : 64;
	const double w = spb(baud);
	// The link knows the packet's start to a sample or two (the lattice):
	// a fine search on the whole packet's bit energy. (The header's
	// alternating bits correlate the same every 2 bits: searching +-10 ms
	// on them locked whole bits off, and the copies did not add up.)
	double best = -1; int64_t bt = exp;
	// Fix 249: each copy at its best timing within +-kMarqHalf samples (3.1 ms) of exp, not +-6: the
	// packets we read land up to +-26 samples from exp in noise (p1-rx-stepdown), and a copy summed
	// off its timing adds less than it should. With the CRC tried at fewer timings (find_packet's
	// focus) the sum carries more of the decoding: p1-rx-stepdown CHAN_SEED 0-8 with the focus alone
	// 881 / 857 / 881 / 857 / 857 / 869 / 881 / 918 / 845 characters, with this too 990 / 905 / 905 /
	// 930 / 951 / 905 / 893 / 881 / 893. P1_RX_FOCUS_OLD=1: +-6 as before (with the old look).
	if (!focus_old()) {                              // the start of that phase nearest exp (which bit: as focus_take)
		bt = best_timing(exp - kMarqHalf, exp + kMarqHalf, baud);
		bt += int64_t(std::llround(std::round(double(exp - bt) / w) * w));
	}
	else
	for (int64_t t = exp - 6; t <= exp + 6; t += 2) {
		double e = 0;
		for (int k = 0; k < nb; k++) e += std::fabs(bit_soft(t + k * w, baud));
		if (e > best) { best = e; bt = t; }
	}
	std::vector<double> cur(static_cast<size_t>(nb));
	for (int k = 0; k < nb; k++) cur[size_t(k)] = bit_soft(bt + k * w, baud);
	if (baud != marq_baud_) { marq_.clear(); marq_baud_ = baud; }
	bool ok = false;
	if (!marq_.empty()) {
		std::vector<double> sum = cur;
		for (const auto &c : marq_) {
			double dot = 0;
			for (int k = 8; k < nb; k++) dot += cur[size_t(k)] * c[size_t(k)];   // (the header aside: it alternates per cycle)
			const double sg = dot < 0 ? -1.0 : 1.0;
			for (int k = 0; k < nb; k++) sum[size_t(k)] += sg * c[size_t(k)];
		}
		uint8_t bits[200], bytes[25];
		for (int inv = 0; inv < 2 && !ok; inv++) {
			for (int k = 0; k < nb; k++) bits[k] = uint8_t((sum[size_t(k)] > 0 ? 1 : 0) ^ inv);
			// the header from this copy alone (the sum's header bits cancel when the header alternated)
			for (int k = 0; k < 8; k++) bits[k] = uint8_t((cur[size_t(k)] > 0 ? 1 : 0) ^ inv);
			pack(bits, nb, bytes);
			if (bytes[0] != 0x55 && bytes[0] != 0xAA) continue;
			const int body = db / 8 + 1;
			const uint16_t crc = uint16_t(bytes[1 + body] | bytes[2 + body] << 8);
			if (crc16_ccitt_reflected(bytes + 1, size_t(body)) != crc) continue;
			// A sum of several noisy copies tried at two polarities passes the
			// 16-bit CRC by chance now and then (an emulated link at noise 4500
			// delivered one wrong packet, 2026-10-20): only the packet expected
			// next, or a repeat of the last one, is believed.
			const int cnt = bytes[body] & 3;
			if (last_rx_counter_ >= 0 && cnt != ((last_rx_counter_ + 1) & 3) && cnt != last_rx_counter_) continue;
			p.baud = baud; p.inv = inv; p.header = bytes[0]; p.status = bytes[body];
			p.data_bits = db; std::memcpy(p.data, bytes + 1, size_t(db / 8)); p.first = bt; p.bk = false;
			ok = true;
		}
	}
	if (ok) { log("memory ARQ: decoded from " + std::to_string(marq_.size() + 1) + " copies"); marq_clear(); return true; }
	marq_.push_back(std::move(cur));
	// Fix 181: up to 30 copies kept, as the reference unit's MAXSum default (30) [bb-emu: its console,
	// "MAXSUM" no argument]; were 4. P1_MARQ_MAX=N: other.
	static const size_t kMax = std::getenv("P1_MARQ_MAX") ? size_t(std::max(1, std::atoi(std::getenv("P1_MARQ_MAX")))) : 30;
	if (marq_.size() > kMax) marq_.erase(marq_.begin());
	return false;
}

void P1Station::acquire_call()
{
	// Every 4 samples, test whether a call packet's first bit started
	// 72 bits + a margin ago.
	if (n_ % 4) return;
	double t = double(n_) - gd_ - 72 * spb(100) - 48 * spb(200) - 2 - 60;
	if (t < 1) return;
	uint8_t bits[72], bytes[9];
	for (int inv = 0; inv < 2; inv++) {
		bits_at(t, 100, 72, inv, bits);
		pack(bits, 72, bytes);
		if (bytes[0] != 0x55) continue;
		uint8_t want[8];
		std::memset(want, 0x0f, 8);
		std::memcpy(want, cfg_.mycall.data(), std::min<size_t>(8, cfg_.mycall.size()));
		const bool lp = bytes[1] == uint8_t(~want[0]) && !std::memcmp(bytes + 2, want + 1, 7);   // long path: first byte inverted
		if (!lp && std::memcmp(bytes + 1, want, 8)) continue;
		// The first matching position is early by up to the 100 Bd
		// tolerance; centre on the range of positions that still match.
		int64_t lo = int64_t(t), hi = int64_t(t);
		auto matches = [&](double tt) {
			uint8_t bb[72], by[9];
			bits_at(tt, 100, 72, inv, bb);
			pack(bb, 72, by);
			return by[0] == 0x55 && by[1] == (lp ? uint8_t(~want[0]) : want[0]) && !std::memcmp(by + 2, want + 1, 7);
		};
		while (hi - int64_t(t) < 90 && matches(double(hi + 1))) hi++;
		while (int64_t(t) - lo < 90 && matches(double(lo - 1))) lo--;
		t = (lo + hi) / 2.0;
		// The 200 Bd part only grades the channel: CS1 if it matches, CS4 if not.
		uint8_t b2[48], by2[6];
		bits_at(t + 72 * spb(100), 200, 48, inv, b2);
		pack(b2, 48, by2);
		const auto exp = call200(cfg_.mycall);
		const bool ok200 = !std::memcmp(by2, exp.data(), std::min<size_t>(6, cfg_.mycall.size() + 1));
		if (debug_) {
			char hx[64]; std::snprintf(hx, sizeof hx, "%02x%02x%02x%02x%02x%02x", by2[0], by2[1], by2[2], by2[3], by2[4], by2[5]);
			log(std::string("call 200 Bd part ") + hx + " span " + std::to_string(hi - lo));
		}
		master_ = false;
		state_ = ST_IRS;
		ident_pending_ = true; ident_buf_.clear(); esc_pending_ = false; si_ = text::SiFilter();
		fast_ = ok200;
		rx_fast_ = ok200; rx_bad_ = rx_good100_ = 0; rx_up_after_ = kRxUpAfter; rx_up_try_ = false; rx_up_wait_ = false;
		ack_cs_ = ok200 ? CS1 : CS4;
		last_rx_first_ = int64_t(t);
		last_rx_counter_ = -1;
		if (cfg_.inline_status) rx_ += "*** CONNECTED TO (call received)\r";
		long_path_ = lp;
		log(std::string("call received, ") + (ok200 ? "200" : "100") + " Bd" + (lp ? ", long path" : ""));
		reply_as_irs(int64_t(t), false);
		return;
	}
}

void P1Station::queue_burst(int64_t first_bit, std::vector<Seg> segs, bool on_grid)
{
	if (on_grid) grid0_ = first_bit;
	// Fix 228: as master (we called), our 200 Bd data packets go 0.5 ms before the grid. The reference unit
	// took them on a clean channel from 3 ms early to 1 ms late (not at 4 early or 2 late: every
	// speed-up rejected), so ours sat 0.8 ms after the middle of its window; its own sender's 200 Bd
	// packets come 0.4 ms before ours after its receiver's control signal [bb-emu, compat
	// p1-we-call-we-send swept, pair DAC]. Under noise or multipath the late edge cost many 200 Bd
	// packets (one sent 9 times though 4 copies reached the reference unit's input with no bit error for a plain
	// tone-energy decoder). -1 ms did better on compat p1-poor and plain noise but made the owner's
	// p1-send-fade slower (1,483 of 1,500 in time), so -0.5. The grid itself (CS windows, 100 Bd,
	// break-ins) is unchanged. P1_TX200_SHIFT_MS=X: X ms instead (0 = the old timing).
	{ static const double sh = std::getenv("P1_TX200_SHIFT_MS") ? std::atof(std::getenv("P1_TX200_SHIFT_MS")) : -0.5;
	  if (on_grid && master_ && !segs.empty() && segs[0].baud == 200) first_bit += samples(sh * 1e-3); }
	Burst b;
	b.first_bit = first_bit;
	b.inv = tx_inv_;
	tx_inv_ = !tx_inv_;                         // polarity flips per transmission
	b.onset = first_bit - samples(cfg_.txd_s);
	double len = 0;
	for (auto &s : segs) len += s.bits.size() * spb(s.baud);
	b.end = first_bit + int64_t(std::ceil(len));
	b.segs = std::move(segs);
	if (debug_ && b.onset < n_) {                  // fix 253: queued after its start, its first part not sent
		char t[96]; std::snprintf(t, sizeof t, "TX burst queued %.1f ms after its start: its first part not sent", 1e3 * double(n_ - b.onset) / cfg_.fs); log(t);
	}
	bursts_.push_back(std::move(b));
}

void P1Station::schedule_call()
{
	std::vector<Seg> segs(2);
	segs[0].baud = 100;
	uint8_t f[9];
	std::memset(f, 0x0f, 9);
	f[0] = 0x55;
	std::memcpy(f + 1, remote_.data(), std::min<size_t>(8, remote_.size()));
	if (long_path_) f[1] = uint8_t(~f[1]);
	push_bits(segs[0].bits, f, 72);
	segs[1].baud = 200;
	const auto p2 = call200(remote_);
	push_bits(segs[1].bits, p2.data(), 48);
	queue_burst(next_first_bit_, std::move(segs), true);
	my_first_ = next_first_bit_;
	events_.push_back({my_first_ + samples(cyc_s() - 0.01) - samples(cfg_.txd_s), EV_CS_WINDOW, my_first_});
}

int P1Station::fill_data(const std::string &q, uint8_t *data, int dbits, int &mode) const
{
	const int cap8 = dbits / 8;
	// Try Huffman.
	int used_h = 0, bits_h = 0;
	for (char ch : q) {
		// The Huffman alphabet is 7-bit: a byte >= 0x80 ends the Huffman run
		// (it was masked to 7 bits, which spoilt binary data: Pat's B2F
		// transfer over P1, 2026-10-20); such bytes go in ASCII-8 packets.
		if (uint8_t(ch) >= 0x80) break;
		const auto &e = kHuff[uint8_t(ch) & 0x7f];
		if (bits_h + e.len > dbits) break;
		bits_h += e.len;
		used_h++;
	}
	const int used_a = std::min<int>(cap8, int(q.size()));
	std::memset(data, 0, size_t((dbits + 7) / 8));
	if (used_h > used_a) {
		mode = 1;
		int pos = 0;
		auto put = [&](uint16_t code, int len) {
			for (int i = 0; i < len && pos < dbits; i++, pos++)
				if (code >> i & 1) data[pos / 8] |= uint8_t(1u << (pos % 8));
		};
		for (int i = 0; i < used_h; i++) { const auto &e = kHuff[uint8_t(q[i]) & 0x7f]; put(e.code, e.len); }
		const auto &idle = kHuff[IDLE_RS];
		while (pos < dbits) put(idle.code, idle.len);   // last idle may be cut off
		return used_h;
	}
	mode = 0;
	for (int i = 0; i < cap8; i++) data[i] = i < used_a ? uint8_t(q[i]) : IDLE_RS;
	return used_a;
}

void P1Station::build_packet(bool new_info)
{
	if (new_info) {
		txq_.erase(0, size_t(pending_used_));
		counter_ = (counter_ + 1) & 3;
		header_ = header_ == 0xAA ? 0x55 : 0xAA;
		// Fix 245: the first packet after our acknowledged break-in carries header AA. The reference
		// unit's receiver resets its expected header when it takes the break-in, takes the break-in
		// packet with any header, then expects AA, 55, AA ... Its hard
		// decode never looks at the header, but its P1 memory ARQ adds only a copy whose header
		// matches: with 55 here every later copy of ours was refused for the
		// rest of the link (CCIR poor, we call and send). P1_BK_HEADER_OLD=1: 55.
		if (after_bk_) { header_ = std::getenv("P1_BK_HEADER_OLD") ? 0x55 : 0xAA; after_bk_ = false; }
		const int dbits = fast_ ? 160 : 64;
		int mode = 0;
		pending_qrt_ = false;
		if (want_qrt_ && txq_.empty()) {
			// QRT packet: the other station's call as a 100 Bd bit pattern,
			// then the header byte, status bit 7 set.
			std::memset(pending_data_, 0, sizeof pending_data_);
			std::string c = remote_.substr(0, 7);
			if (fast_) {
				std::fill(pending_data_, pending_data_ + 20, uint8_t(0x0f));
				auto put2 = [&](int byte_index, uint8_t v) {   // one 100 Bd byte = two 200 Bd bytes
					uint16_t w = 0;
					for (int i = 0; i < 8; i++) if (v >> i & 1) w |= uint16_t(3u << (2 * i));
					pending_data_[byte_index] = uint8_t(w);
					pending_data_[byte_index + 1] = uint8_t(w >> 8);
				};
				for (size_t i = 0; i < c.size(); i++) put2(int(13 - 2 * i), uint8_t(c[i]));
				put2(15, header_);
			} else {
				for (size_t i = 0; i < c.size(); i++) pending_data_[6 - i] = uint8_t(c[i]);
				pending_data_[7] = header_;
			}
			mode = 0;
			pending_used_ = 0;
			pending_qrt_ = true;
		} else {
			pending_used_ = fill_data(txq_, pending_data_, dbits, mode);
		}
		// "Over" (status bit 6 obliges the receiver to answer with a break-in), as the reference unit
		// (owner decision a, 2026-11-15): only after the changeover key (CHOchr, host %O) or, with
		// PDuplex 1, while our queue is empty; kept on every new packet until the link is taken
		// (as the reference unit). P1_BK_AUTO_OLD=1 or OVER_OLD=1: the
		// old way, once on the first packet after our text has gone (fix 50), or after the key.
		bool over = false;
		if (!pending_qrt_ && pending_used_ == 0 && txq_.empty()) {
			if (over_old()) { if (sent_text_ || cho_req_) { over = true; sent_text_ = false; cho_req_ = false; } }
			else over = cho_req_ || cfg_.pduplex;
		}
		if (pending_used_ > 0) sent_text_ = true;
		pending_status_ = uint8_t(counter_ | mode << 2 | (pending_qrt_ ? 0x80 : 0) | (over ? 0x40 : 0));
	}
	const int dbits = fast_ ? 160 : 64, body = dbits / 8 + 1;
	if (debug_) {                                   // what this packet carries
		std::string t = txq_.substr(0, size_t(std::max(0, pending_used_)));
		for (auto &ch : t) if (uint8_t(ch) < 32 || uint8_t(ch) > 126) ch = '.';
		log(std::string(new_info ? "tx packet" : "tx packet again") + " status " + std::to_string(pending_status_) + " header " + std::to_string(header_) + " \"" + t + "\"");
	}
	uint8_t pkt[24];
	pkt[0] = header_;
	std::memcpy(pkt + 1, pending_data_, size_t(dbits / 8));
	pkt[body] = pending_status_;
	const uint16_t crc = crc16_ccitt_reflected(pkt + 1, size_t(body));
	pkt[body + 1] = uint8_t(crc);
	pkt[body + 2] = uint8_t(crc >> 8);
	std::vector<Seg> segs(1);
	segs[0].baud = fast_ ? 200 : 100;
	push_bits(segs[0].bits, pkt, (body + 3) * 8);
	queue_burst(next_first_bit_, std::move(segs), true);
	my_first_ = next_first_bit_;
	events_.push_back({my_first_ + samples(cyc_s() - 0.01) - samples(cfg_.txd_s), EV_CS_WINDOW, my_first_});
}

void P1Station::send_bk(int64_t first_bit, bool repeat)
{
	uint8_t data[24];   // 18 used at most (24: GCC's vectorised fill warned at 20)
	const int dbits = fast_ ? 144 : 56;
	int mode = 0;
	if (repeat && !std::getenv("P1_BK_REFILL")) pending_used_ = fill_data(txq_.substr(0, size_t(std::max(0, pending_used_))), data, dbits, mode);
	else pending_used_ = fill_data(txq_, data, dbits, mode);
	// Decision a: text in our break-in packet is text sent, so the "over" follows when the queue
	// runs empty (the peer, as P1 receiver, no longer breaks in on its own text). Before, a reply
	// that fitted the break-in packet was never followed by "over" (harmless while every receiver
	// broke in on text: Pat over P1 hung here). P1_BK_AUTO_OLD=1: as before.
	if (pending_used_ > 0 && !bk_auto_old()) sent_text_ = true;
	const uint8_t status = uint8_t(0 | mode << 2);
	if (debug_) {
		std::string t = txq_.substr(0, size_t(pending_used_));
		for (auto &ch : t) if (uint8_t(ch) < 32 || uint8_t(ch) > 126) ch = '.';
		log("tx break-in \"" + t + "\"");
	}
	uint8_t body[20];
	std::memcpy(body, data, size_t(dbits / 8));
	body[dbits / 8] = status;
	uint16_t crc = crc16_ccitt_reflected(body, size_t(dbits / 8 + 1));
	// P1_TEST_BK_SPOIL=N: test, the CRC of our first N 200 Bd break-ins spoilt, so the other station
	// cannot read them and REJECTs (CS4) [fix 211: the REJECT path on the reference unit].
	static int spoil_left = std::getenv("P1_TEST_BK_SPOIL") ? std::atoi(std::getenv("P1_TEST_BK_SPOIL")) : 0;
	if (fast_ && spoil_left > 0) { spoil_left--; crc ^= 0x5A5A; log("test: break-in CRC spoilt"); }
	// P1_TEST_BK_NOHEAD=N: test, our first N break-ins sent with their CS3 head blanked, so the
	// other station does not take them and goes on sending [fix 211, p1-504's case].
	static int nohead_left = std::getenv("P1_TEST_BK_NOHEAD") ? std::atoi(std::getenv("P1_TEST_BK_NOHEAD")) : 0;
	const bool nohead = nohead_left > 0;
	if (nohead) { nohead_left--; log("test: break-in head blanked"); }
	std::vector<Seg> segs(2);
	segs[0].baud = 100;
	push_value(segs[0].bits, nohead ? 0u : kCsBits[CS3], 12);
	if (!fast_) push_value(segs[0].bits, 0, 4);
	segs[1].baud = fast_ ? 200 : 100;
	push_bits(segs[1].bits, body, dbits + 8);
	push_value(segs[1].bits, crc, 16);
	queue_burst(first_bit, std::move(segs), true);
	my_first_ = first_bit;
	bk_sent_ = true;
	log("break-in");
}

int64_t P1Station::follow_step(int64_t reading, Follow &f, const char *what)
{
	static const bool off = std::getenv("P1_NO_SLAVE_FOLLOW") != nullptr;
	if (off && &f == &fl_ && !master_) return 0;
	const double th = double(reading) + f.shift;
	if (f.base_n < 4) { f.base_sum += th; f.base_n++; return 0; }
	f.hist.push_back(th);
	if (f.hist.size() > 6) f.hist.pop_front();
	if (f.hist.size() < 6) return 0;
	std::vector<double> v(f.hist.begin(), f.hist.end());
	std::sort(v.begin(), v.end());
	const double held = (v[1] + v[2] + v[3] + v[4]) / 4.0, want = held - f.base_sum / 4.0;
	const double lim = 0.0005 * cfg_.fs, d = want - f.shift;
	if (std::fabs(d) < lim) return 0;
	const int64_t st = int64_t(std::llround(std::max(-lim, std::min(lim, d))));
	if (st == 0) return 0;
	f.shift += double(st);
	if (debug_) log(std::string(what) + " follow: " + std::to_string(double(st) / cfg_.fs * 1000.0) + " ms (in all " + std::to_string(f.shift / cfg_.fs * 1000.0) + " ms)");
	return st;
}

int64_t P1Station::master_reply_time(int64_t rx_first) const
{
	const double cyc = cyc_s() * cfg_.fs;
	const double k = std::round((rx_first + 0.25 * cfg_.fs - grid0_) / cyc);
	// 0.845 s measured on reference signals; a reference receiver perceives our
	// (unshaped) FSK bits ~5.6 ms earlier than its own station's, so
	// 0.839 s lands where it expects (calibrated against the firmware).
	return grid0_ + int64_t(k * cyc) + samples(0.839);
}

int64_t P1Station::reply_time(int64_t rx_first, bool after_bk_packet) const
{
	if (master_) return master_reply_time(rx_first);
	// The callee's reply moves with the audio path's calibration, as the P2
	// station's P1-phase replies do (kP1Reply - rt_latency): created by the
	// P2 station, a P1 link had none (2026-10-20).
	return rx_first + samples((after_bk_packet ? slave_bk_reply_s() : 1.003) - cfg_.rt_latency_s - slave_advance_s());
}

void P1Station::reply_as_irs(int64_t rx_first, bool after_bk_packet)
{
	const int64_t at = reply_time(rx_first, after_bk_packet);
	if ((want_qrt_ || must_break_in_ || bk_req_ || auto_break_in()) && rx_ok_since_connect_) {
		bk_asked_ = must_break_in_ || bk_req_;       // kept if this break-in is given up untaken (fix 234's listening cycle)
		must_break_in_ = false; bk_req_ = false; cho_req_ = false;
		counter_ = 0;
		// Our break-in goes at the speed the other station's packets run at: the
		// emulated reference unit, sending at 200 Bd after our CS4, did not read our 100 Bd
		// break-in (CS2 x4, then CS4) [bb-emu, owner's p1-cs4, fix 162].
		// P1_OLD_BK_SPEED: our own sending speed (old).
		if (!std::getenv("P1_OLD_BK_SPEED")) fast_ = rx_fast_;
		send_bk(at);
		bk_rx_off_ = at - rx_first;                 // fix 234: where its packets are against our break-in
		state_ = ST_BK_SENT;
		events_.push_back({at + samples(cyc_s() - 0.01) - samples(cfg_.txd_s), EV_CS_WINDOW, at});
		return;
	}
	send_cs(ack_cs_, at);
	const int64_t next = rx_first + samples(cyc_s());
	events_.push_back({next + samples(0.96 + 0.012), EV_PACKET_DUE, next});
}

void P1Station::on_cs_window(int64_t ref)
{
	if (state_ != ST_ISS && state_ != ST_CALLING && state_ != ST_BK_SENT) return;
	Cs cs = CS_NONE; int64_t at = 0; bool inv = false;
	const double score = find_cs(ref + samples(0.90), ref + samples(cyc_s() - 0.01) - (std::getenv("P1_FULL_REPLY") ? 12 : 10) * int64_t(spb(100)) - samples(cfg_.txd_s) + samples(0.01), cs, at, inv);
	if (debug_) log("cs window: cs" + std::to_string(int(cs)) + " score " + std::to_string(score) +
	                 " at +" + std::to_string(double(at - ref) / cfg_.fs) + " s, last " + std::to_string(int(last_rx_cs_)));
	double score2 = score;
	// Fix 228: a second look where the reference unit's control signal must be. Under CCIR-poor fading (1 Hz,
	// 2 ms, compat p1-poor) half of the reference unit's control signals failed the weak-bit rule above
	// (fix 37/161: no more than one bit below 0.35 of the strongest; with fix 38's tone energies, which
	// are amplitudes squared, that is 0.59 in amplitude, and a 1 Hz fade moves more than that inside
	// 120 ms): 22 of 44 control signals heard as nothing and our packet repeated, while the reference unit's
	// own sender, on the same channel, repeated only when its receiver asked [bb-emu, pair DAC
	// recordings]. The reference unit inverts every transmission (its control signals alternate in polarity
	// cycle by cycle in both runs), so once a clear one has shown the polarity, the next is
	// predictable; and the reply track (fix 183) gives the time. In +-4 ms of the track, with the
	// predicted polarity, no weak-bit rule; score >= 0.85; the 6 dB tone check stays.
	// P1_CS_EXPECT_OFF: off.
	if (score < 0.75 && state_ == ST_ISS && cs_ref_ok_ && pol_ok_ && !std::getenv("P1_CS_EXPECT_OFF")) {
		const int64_t k = std::llround(double(ref - pol_ref_) / (cyc_s() * cfg_.fs));
		if (k > 0 && k <= 8) {
			const bool want = pol_inv_ ^ bool(k & 1);
			const int64_t c0 = ref + int64_t(std::llround(cs_ref_off_)), d4 = samples(0.004);
			Cs c2; int64_t at2 = 0;
			const double sc2 = find_cs_expected(c0 - d4, c0 + d4, want, c2, at2);
			if (sc2 >= 0.85) {
				log("cs" + std::to_string(int(c2)) + " (" + std::to_string(sc2) + ") at the expected time and polarity");
				cs = c2; at = at2; inv = want; score2 = sc2;
			}
		}
	}
	if (score2 < 0.75) cs = CS_NONE;
	if (cs != CS_NONE && score >= 0.95) { pol_ok_ = true; pol_inv_ = inv; pol_ref_ = ref; }   // a clear one, found without a prediction
	// P1_TEST_CS_MISREAD=N:MS: test, every Nth clear control signal read while we send is taken MS ms
	// off where it was (a misread timing, as the f231-4 bench run's; fix 234 must not move on one).
	if (state_ == ST_ISS && score >= 0.95 && cs != CS_NONE) {
		static int mr_n = 0, mr_k = 0; static double mr_ms = 0; static bool mr_init = false;
		if (!mr_init) { mr_init = true; if (const char *e = std::getenv("P1_TEST_CS_MISREAD")) std::sscanf(e, "%d:%lf", &mr_n, &mr_ms); }
		if (mr_n > 0 && ++mr_k % mr_n == 0) { at += samples(mr_ms / 1000.0); log("test: control signal timing misread " + std::to_string(mr_ms) + " ms"); }
	}
	next_first_bit_ = ref + samples(cyc_s());
	if (state_ == ST_CALLING) {
		if (cs == CS1 || cs == CS4) {
			state_ = ST_ISS; fast_ = cs == CS1; last_rx_cs_ = cs == CS4 ? CS1 : cs; misses_ = 0;
			last_was_cs4_ = false; up_wait_ = false;
			// Fix 207: a call answered with CS4 (link at 100 Bd): "CS4 serves as the REQUEST CS
			// for the first 100 Bd block, acknowledged with CS1" [spec: P1 1990], as after
			// a speed-down below and as P2Station's caller since fix 159. We took the reference unit's CS1
			// as "again" and repeated our first packet for ever (it answers our call with CS4
			// when it hears it 40 Hz and more off: 0 characters). P1_OLD_CALL_CS4: the old way.
			if (cs == CS4 && !std::getenv("P1_OLD_CALL_CS4")) { last_rx_cs_ = CS4; last_was_cs4_ = true; }
			counter_ = 0; header_ = 0x55; si_ = text::SiFilter();
			log(std::string("connected to ") + remote_ + (fast_ ? " (200 Bd)" : " (100 Bd)"));
			if (cfg_.inline_status) rx_ += "*** CONNECTED TO " + remote_ + "\r";
			txq_.insert(0, std::string("1") + lower(cfg_.mycall) + "\r");
			pending_used_ = 0;
			build_packet(true);
		} else if (++misses_ > cfg_.timeout_cycles) {
			disconnect("no answer");
		} else {
			schedule_call();
		}
		return;
	}
	if (state_ == ST_BK_SENT) {                     // waiting for the BK acknowledgement
		// Fix 211: a CS4 to our break-in counts as a REJECT only when clear (>= 0.95, as fix 183's
		// "clear"). Bench p1-504: the real reference unit did not take our 200 Bd break-in and went on
		// sending; our window found a "CS4" at 0.76 in its own packet, we took it as a REJECT and
		// sent our text at 100 Bd on top of its packets for 35 s (more "CS4"s at 0.66-0.77, none
		// real: every real control signal in the P1 bench logs scores 1.000). A weak one is now
		// nothing heard: the break-in again (a peer that did REJECT answers it with CS4 again at
		// once [bb-emu]). P1_BK_WEAK_CS4: the old way.
		if (cs == CS4 && score < 0.95 && !std::getenv("P1_BK_WEAK_CS4")) {
			log("cs4 (" + std::to_string(score) + ") to our break-in not clear: taken as nothing heard");
			cs = CS_NONE;
		}
		if (cs == CS1) {
			state_ = ST_ISS; last_rx_cs_ = CS1; misses_ = 0;
			last_was_cs4_ = false; up_wait_ = false;
			after_bk_ = true;
			log("break-in acknowledged, now sending");
			rej_cs4_ = 0; rej_total_ = 0;
			follow_reset();
			build_packet(true);                     // drops the BK's text, header 55, counter 1
		} else if (cs == CS4 && fast_ && !std::getenv("P1_NO_SPEEDCHANGE")) {
			// REJECT of our break-in [spec]: its text again as an ordinary
			// 100 Bd packet, header 55, the break-in's counter (0); CS1 acknowledges it.
			state_ = ST_ISS; misses_ = 0; last_was_cs4_ = false; up_wait_ = false;
			fast_ = false; header_ = 0x55; counter_ = 0; last_rx_cs_ = CS4; last_was_cs4_ = true;   // another CS4: a request, not a speed-up
			log("break-in rejected (CS4): sending at 100 Bd");
			rej_cs4_ = 1;
			rebuild_same();
		} else if (cs == CS4 && !fast_ && !std::getenv("P1_OLD_BK_SPEED")) {
			// A CS4 to our 100 Bd break-in: the other station wants 200 Bd; the
			// break-in again at 200 Bd (fix 162; with the CS4 to a 200 Bd break-in
			// above, a CS4 never leaves the break-in unanswered).
			if (++misses_ > cfg_.timeout_cycles) { disconnect("timeout"); return; }
			fast_ = true;
			log("break-in answered with CS4: again at 200 Bd");
			send_bk(next_first_bit_, true);
			events_.push_back({next_first_bit_ + samples(cyc_s() - 0.01) - samples(cfg_.txd_s), EV_CS_WINDOW, next_first_bit_});
		} else if (cs == CS3 && !std::getenv("P1_OLD_REBREAK")) {
			// "Rebreak": our break-in read, and the other station breaks in at
			// once; its CS3 heads its own break-in packet [spec: P1 1990,
			// Senderichtungswechsel: "CS1, bzw. CS3, falls ein sofortiges Rebreak
			// gewuenscht wird"] (fix 162). P1_OLD_REBREAK: repeated as unanswered.
			txq_.erase(0, size_t(std::max(0, pending_used_))); pending_used_ = 0;
			log("break-in answered by a break-in");
			state_ = ST_BK_RX;
			bk_hold_until_ = at + samples(cfg_.pdtimer_s);   // PDTimer
			cho_req_ = false;                       // the link is taken: our changeover is done
			events_.push_back({at + samples(0.96 + 0.012), EV_BK_DONE, at});
		} else if (cs == CS2 || cs == CS_NONE || cs == CS3 || cs == CS4) {
			if (++misses_ > cfg_.timeout_cycles) { disconnect("timeout"); return; }
			// Fix 234: while our capture is slipping (rx_search_s), a break-in not taken is followed
			// by one cycle of listening instead of the next break-in. The reference unit goes on with its
			// packets when it did not take ours, but a break-in repeated every cycle covers them
			// all, so we never saw where they had moved and every break-in went on the old grid:
			// emulator, capture -38 ms over 10 s just before our break-in, the break-in sent 58
			// times untaken until the link timed out. The packet read moves the grid (reply_as_irs)
			// and the break-in goes again from it. P1_WIDEN_OFF=1: as before.
			if (rx_search_s() > 0.010 && bk_rx_off_ > 0) {
				state_ = ST_IRS;
				if (bk_asked_ && !bk_auto_old()) bk_req_ = true;   // decision a: the reason to break in still stands (text alone may not be one)
				tx_inv_ = !tx_inv_;                 // the silent cycle counts: the polarity keeps the cycle (fix 227b's rule)
				const int64_t exp = ref - bk_rx_off_ + 2 * samples(cyc_s());   // its first packet our break-in does not cover
				log("break-in not taken while our capture slips: listening for its packet");
				events_.push_back({exp + samples(0.96 + 0.012), EV_PACKET_DUE, exp});
				return;
			}
			send_bk(next_first_bit_, true);         // repeat the BK packet
			events_.push_back({next_first_bit_ + samples(cyc_s() - 0.01) - samples(cfg_.txd_s), EV_CS_WINDOW, next_first_bit_});
		}
		return;
	}
	// Connected ISS.
	// Fix 183: the reply's timing tracked. The window above is ~230 ms wide and every
	// position in it is tried: in noise alone 1 window in 400 gave a control signal
	// (half of those an acknowledgement when the receiver's signal was lost: a packet
	// lost for good), and a CS3 read two bits early scores as CS4 (-18..-22 ms, 0.75-0.87)
	// [p1cs_conf, offline]. The receiver answers at a fixed time after our packet
	// (+0.996 s on the reference unit, the same within 0.3 ms over a link). Once 3 clear ones
	// (>= 0.95) have shown where, a weaker one more than 6 ms away is nothing heard (we
	// repeat). A new sending turn starts the track again. P1_NO_CS_GATE: off.
	// Fix 234: as sender while our capture slips (slip_hint_on), our next packet moves by a whole
	// offset of the master's control signal from the reply track (2-60 ms) at once, instead of the
	// follow's 0.5 ms a cycle. With the round trip held, what we hear and what we send move together
	// on our clock, so the master's grid, fixed on the air, comes back as its control signals arriving
	// early by the slip; our packets left its window and it repeated its request: emulator, capture
	// -38 ms over 10 s while we sent, the same packet 30+ times and the link lost. One reading never
	// moves us (fix 231 on the bench, f231-4: two misread control signals, -5 then +19.8 ms, taken as
	// jumps, the link lost with no slip at all): a clear reading off the track only becomes a
	// candidate; the jump needs the next cycle's clear reading off the same way, at least as far less
	// 1 ms, at most 10 ms further (a slip goes on one way), and the audio layer's report besides. Our
	// packet then moves by that second reading. Otherwise a reading is handled as before (the track,
	// the follow). P1_WIDEN_OFF=1: none of this.
	if (cs != CS_NONE && score >= 0.95 && cs_trk_n_ >= 3 && slip_hint_on() && (!master_ || master_follow_on()) && !std::getenv("P1_NO_CS_GATE")) {
		const double dj = double(at - ref) - cs_trk_off_, ms = cfg_.fs / 1000.0;
		if (std::fabs(dj) > 2 * ms && std::fabs(dj) <= double(samples(kRxSearchMaxS))) {
			const bool confirmed = jc_have_ && ref - jc_ref_ <= samples(1.6 * cyc_s()) && (dj > 0) == (jc_off_ > 0) &&
			                       std::fabs(dj) >= std::fabs(jc_off_) - ms && std::fabs(dj - jc_off_) <= 10 * ms;
			if (confirmed) {
				jc_have_ = false;
				const int64_t st = int64_t(std::llround(dj));
				next_first_bit_ += st; fl_.shift += double(st);
				cs_trk_last_ = ref;
				log("our capture slips: next packet moved " + std::to_string(double(st) / ms) + " ms (two control signals off the same way)");
				goto tracked;
			}
			jc_have_ = true; jc_off_ = dj; jc_ref_ = ref;
			log("control signal " + std::to_string(dj / ms) + " ms off the reply track while our capture slips: held for the next reading");
		} else jc_have_ = false;
	} else if (cs != CS_NONE && score >= 0.95) jc_have_ = false;
	if (cs != CS_NONE && !std::getenv("P1_NO_CS_GATE")) {
		const double d = double(at - ref);
		if (cs_trk_last_ < 0 || ref - cs_trk_last_ > samples(1.6 * cyc_s())) cs_trk_n_ = 0;
		cs_trk_last_ = ref;
		if (cs_trk_n_ >= 3 && score < 0.95 && std::fabs(d - cs_trk_off_) > double(samples(0.006))) {
			log("cs" + std::to_string(int(cs)) + " (" + std::to_string(score) + ") " + std::to_string((d - cs_trk_off_) / cfg_.fs * 1000.0) + " ms off the reply track: taken as nothing heard");
			cs = CS_NONE;
		} else if (score >= 0.95) {
			if (cs_trk_n_ < 3) { cs_trk_off_ = cs_trk_n_ ? (cs_trk_off_ * cs_trk_n_ + d) / (cs_trk_n_ + 1) : d; cs_trk_n_++; }
			else if (std::fabs(d - cs_trk_off_) <= double(samples(0.006))) { cs_trk_off_ += 0.25 * (d - cs_trk_off_); cs_trk_away_ = 0; }
			else if (++cs_trk_away_ >= 3) { cs_trk_off_ = d; cs_trk_away_ = 0; log("reply track moved"); }   // three clear ones elsewhere: the timing moved
		}
	} else if (cs == CS_NONE) cs_trk_last_ = ref;
	if (cs_trk_n_ >= 3) { cs_ref_off_ = cs_trk_off_; cs_ref_ok_ = true; }   // fix 228: kept across sending turns
	if ((!master_ || master_follow_on()) && score >= 0.95 && (cs == CS1 || cs == CS2 || cs == CS4)) next_first_bit_ += follow_step(at - ref, fl_, master_ ? "master" : "slave");
tracked:
	far_set(cs == CS_NONE || cs == CS3 ? 0 : cs == CS4 ? (last_was_cs4_ ? 2 : fast_ ? 2 | 8 : 1 | 4) : cs != last_rx_cs_ ? 1 : 2);
	if (cs == CS3) {                                // break-in: CS3 heads the IRS's BK packet
		state_ = ST_BK_RX;
		bk_hold_until_ = at + samples(cfg_.pdtimer_s);   // PDTimer: PDuplex's break-ins wait this long
		cho_req_ = false;                           // the link is taken: our changeover is done
		events_.push_back({at + samples(0.96 + 0.012), EV_BK_DONE, at});
		return;
	}
	if (cs == CS_NONE) {
		if (++misses_ > cfg_.timeout_cycles) { disconnect("timeout"); return; }
		build_packet(false);
		return;
	}
	misses_ = 0;
	if (cs == CS4) {
		// Fix 211: after a REJECT of our break-in, CS4 is the request letter for the 100 Bd copy
		// [spec]; the real reference unit answered every copy with CS4 for 80 s (bench p1-504: 0 of the
		// line got through, we never timed out). Every 4th CS4 in a row the break-in goes again
		// at 200 Bd instead, with the same text: a peer that never took the REJECT, or reads the
		// 200 Bd break-in, takes it; a CS4 to it REJECTs again. P1_NO_REJ_RETRY: off.
		if (rej_cs4_ > 0 && ++rej_total_ > cfg_.timeout_cycles) { disconnect("timeout"); return; }   // (misses_ is reset by every CS4)
		if (rej_cs4_ > 0 && !std::getenv("P1_NO_REJ_RETRY") && ++rej_cs4_ > 4) {
			rej_cs4_ = 0;
			fast_ = true; counter_ = 0; state_ = ST_BK_SENT;
			log("CS4 to the 100 Bd copy of our break-in 4 times: the break-in again at 200 Bd");
			send_bk(next_first_bit_, true);
			events_.push_back({next_first_bit_ + samples(cyc_s() - 0.01) - samples(cfg_.txd_s), EV_CS_WINDOW, next_first_bit_});
			return;
		}
		if (last_was_cs4_) { build_packet(false); return; }   // CS4 in a row = request
		last_was_cs4_ = true;
		if (!fast_) {                               // at 100 Bd: acknowledge + speed up
			fast_ = true;
			if (pending_qrt_) { disconnect("QRT acknowledged"); return; }
			// CS4 takes the place of the next letter: the first 200 Bd packet
			// is acknowledged with the letter from before the CS4; the other
			// letter ("an OK-CS different from the last CS before the CS4")
			// rejects the speed-up [spec: P1 description 1990,
			// "Geschwindigkeitserhoehung"]. The real reference unit: CS1, CS4, CS1
			// [bb-ext, 2026-10-20]. We took that CS1 as a request and repeated
			// the packet forever. P1_UP_OLD: the old way.
			if (!up_old()) { last_rx_cs_ = last_rx_cs_ == CS1 ? CS2 : CS1; up_wait_ = true; }
			log("speed up to 200 Bd");
			build_packet(true);
		} else {                                    // at 200 Bd: reject, speed down
			fast_ = false;
			header_ = 0x55;
			if (!std::getenv("P1_NO_SPEEDCHANGE")) last_rx_cs_ = CS4;   // the receiver acknowledges the first 100 Bd block with CS1 [spec]
			log("speed down to 100 Bd");
			rebuild_same();
		}
		return;
	}
	last_was_cs4_ = false; rej_cs4_ = 0; rej_total_ = 0;
	if (up_wait_) {                                 // the first letter after a speed-up
		up_wait_ = false;
		if (cs == last_rx_cs_) {                    // the OK-CS: rejected, the 200 Bd information again at 100 Bd [spec]
			far_set(2 | 8);
			fast_ = false;
			// Fix 245: the packet keeps its header. The reference unit has not taken it, and its
			// expected header changes only when it takes a packet; with 55 here its
			// memory ARQ refused every later copy of ours. P1_BK_HEADER_OLD=1: 55 (the old guess).
			if (std::getenv("P1_BK_HEADER_OLD")) header_ = 0x55;
			log("speed-up rejected: back to 100 Bd");
			rebuild_same();
			return;
		}
	}
	if (cs != last_rx_cs_) {                        // acknowledged
		last_rx_cs_ = cs;
		if (pending_qrt_) { disconnect("QRT acknowledged"); return; }
		build_packet(true);
	} else {
		build_packet(false);                        // request: repeat
	}
}

void P1Station::on_bk_done(int64_t at)
{
	if (state_ != ST_BK_RX) return;
	RxPacket p;
	if (find_packet(at - 4, at + 4, p, true) && p.bk) {
		txq_.erase(0, size_t(pending_used_)); pending_used_ = 0;   // our last packet was acknowledged
		state_ = ST_IRS; ack_cs_ = CS1; misses_ = 0; rx_up_wait_ = false;
		fr_ = Follow();                                 // fix 192: a receiving turn
		last_rx_counter_ = 0;
		rx_fast_ = p.baud == 200; rx_bad_ = rx_good100_ = 0;
		rx_ok_since_connect_ = true;
		deliver(p);
		log("break-in received, now receiving");
		send_cs(CS1, reply_time(at, true));
		const int64_t next = at + samples(cyc_s());
		events_.push_back({next + samples(0.96 + 0.012), EV_PACKET_DUE, next});
	} else {
		// Could not read it: ask for it again (CS2) in our receive-slot timing,
		// or (at 200 Bd, the second failure) reject it with CS4: the sender
		// then sends its text as an ordinary 100 Bd packet, header 55
		// [spec: "REJECT eines BK-Paketes"].
		if (++misses_ > cfg_.timeout_cycles) { disconnect("timeout"); return; }
		if (fast_ && ++rx_bad_ >= kRxDownAfter && !std::getenv("P1_NO_SPEEDCHANGE")) {
			rx_bad_ = 0; rx_good100_ = 0; rx_fast_ = false;
			log("break-in not read: asking for 100 Bd (CS4)");
			send_cs(CS4, reply_time(at, true));
			txq_.erase(0, size_t(pending_used_)); pending_used_ = 0;   // our last packet: acknowledged by its break-in
			state_ = ST_IRS; ack_cs_ = CS4; last_rx_counter_ = -1;      // the 100 Bd packet takes the break-in's counter (0); CS4 requests it again, CS1 acknowledges it
			const int64_t next = at + samples(cyc_s());
			events_.push_back({next + samples(0.96 + 0.012), EV_PACKET_DUE, next});
			return;
		}
		state_ = ST_BK_RX;
		send_cs(CS2, reply_time(at, true));
		const int64_t next = at + samples(cyc_s());
		events_.push_back({next + samples(0.96 + 0.012), EV_BK_DONE, next});
	}
}

void P1Station::on_packet_due(int64_t exp)
{
	if (state_ != ST_IRS) return;
	RxPacket p;
	const double w = rx_search_s();
	if (w > 0.010 && debug_) log("rx look widened to +-" + std::to_string(int(w * 1000)) + " ms (our capture slipped)");
	bool got = find_packet(exp - samples(w), exp + samples(w), p, false, true);
	// Fix 234: a packet found beyond the usual +-10 ms moves our grid, so it must also be the one due
	// or a repeat of the last (its counter): a random CRC pass in the wider look moves nothing.
	if (got && w > 0.010 && std::llabs(p.first - exp) > samples(0.010) && last_rx_counter_ >= 0 &&
	    (p.status & 3) != last_rx_counter_ && (p.status & 3) != ((last_rx_counter_ + 1) & 3)) got = false;
	if (got) {
		marq_clear();
		if (master_ && master_follow_on()) {           // fix 192: the grid our replies sit on
			const double cyc = cyc_s() * cfg_.fs, k = std::round((double(p.first) - double(grid0_)) / cyc);
			grid0_ += follow_step(int64_t(std::llround(double(p.first) - double(grid0_) - k * cyc)), fr_, "master grid");
		}
	}
	else got = marq_decode(exp, rx_fast_ ? 200 : 100, p);
	if (got) {
		misses_ = 0; rx_bad_ = 0; rx_fast_ = p.baud == 200;
		rx_ok_since_connect_ = true;
		const int cnt = p.status & 3;
		if (cnt != last_rx_counter_) {
			last_rx_counter_ = cnt;
			ack_cs_ = ack_cs_ == CS1 ? CS2 : CS1;
			deliver(p);
			// Speed-up [spec: P1 description 1990, "Geschwindigkeitserhoehung"]:
			// a good 100 Bd packet acknowledged with CS4 makes the sender go to
			// 200 Bd. CS4 stands in for this packet's letter (ack_cs_ stays
			// toggled), so the first 200 Bd packet gets the letter from before
			// the CS4; the other letter would reject the speed-up. Until then a
			// request is CS4 again. We sent the other letter, and a real reference unit
			// went back to 100 Bd every time. P1_UP_OLD: the old way.
			if (p.baud == 100 && !(p.status & 0xC0) && ++rx_good100_ >= rx_up_after_ && !want_qrt_ && txq_.empty() && !std::getenv("P1_NO_SPEEDCHANGE")) {
				if (up_old()) ack_cs_ = ack_cs_ == CS1 ? CS2 : CS1;   // (old: undo)
				else { rx_up_wait_ = true; rx_up_fail_ = 0; }
				rx_good100_ = 0; rx_up_try_ = true;
				log("asking for 200 Bd (CS4)");
				send_cs(CS4, reply_time(p.first, false));
				const int64_t next = p.first + samples(cyc_s());
				events_.push_back({next + samples(0.96 + 0.012), EV_PACKET_DUE, next});
				return;
			}
			if (p.baud == 200 && rx_up_try_) { rx_up_try_ = false; rx_up_after_ = kRxUpAfter; }   // the speed-up held
			rx_up_wait_ = false;
		} else {
			if (debug_) log("rx repeat status " + std::to_string(p.status) + " at " + std::to_string(p.baud) + " Bd");
			if (rx_up_wait_) {                      // the sender missed our CS4 and repeats at 100 Bd: CS4 again
				send_cs(CS4, reply_time(p.first, false));
				const int64_t next = p.first + samples(cyc_s());
				events_.push_back({next + samples(0.96 + 0.012), EV_PACKET_DUE, next});
				return;
			}
		}
		if (p.status & 0x80) {                      // QRT packet: acknowledge, then stand by
			send_cs(ack_cs_, reply_time(p.first, false));
			disconnect("QRT received");
			return;
		}
		if (p.status & 0x40) must_break_in_ = true;   // the sender handed us the link
		reply_as_irs(p.first, false);
	} else {
		if (debug_) log("rx nothing at " + std::to_string(rx_fast_ ? 200 : 100) + " Bd (miss " + std::to_string(misses_ + 1) + ")");
		if (++misses_ > cfg_.timeout_cycles) { disconnect("timeout"); return; }
		if (rx_up_wait_) {                          // no 200 Bd packet yet after our CS4
			const int64_t next = exp + samples(cyc_s());
			events_.push_back({next + samples(0.96 + 0.012), EV_PACKET_DUE, next});
			if (++rx_up_fail_ < kRxMaxTry) { send_cs(CS4, reply_time(exp, false)); return; }   // request [spec: CS4 in a row]
			// reject: the OK-CS different from the letter before the CS4 (ack_cs_), back to 100 Bd [spec]
			rx_up_wait_ = false; rx_fast_ = false; rx_up_try_ = false;
			rx_up_after_ = std::min(4 * kRxUpAfter, 2 * rx_up_after_);
			log("no 200 Bd packet after CS4: rejecting the speed-up");
			send_cs(ack_cs_, reply_time(exp, false));
			return;
		}
		// Speed-down [spec: "Geschwindigkeitsverminderung"]: CS4 after a bad
		// packet is a reject; the sender repeats its information at 100 Bd.
		// We never asked, so on a weak path a 200 Bd link stayed there and
		// timed out (6 m at 0 W, 2026-10-20). After 2 failures at 200 Bd; a
		// speed-up that failed doubles the wait before the next one.
		if (rx_fast_ && ++rx_bad_ >= kRxDownAfter && !std::getenv("P1_NO_SPEEDCHANGE")) {
			rx_bad_ = 0; rx_good100_ = 0; rx_fast_ = false;
			if (rx_up_try_) { rx_up_try_ = false; rx_up_after_ = std::min(4 * kRxUpAfter, 2 * rx_up_after_); }
			log("asking for 100 Bd (CS4)");
			// CS4 is now the request letter for the first 100 Bd block, which
			// CS1 acknowledges [spec]: failures repeat CS4. Our old letter was
			// taken as an acknowledgement and a packet was lost (emulated
			// firmware, noise 3000).
			ack_cs_ = CS4;
			send_cs(CS4, reply_time(exp, false));
		} else send_cs(ack_cs_, reply_time(exp, false));   // same CS again = request
		const int64_t next = exp + samples(cyc_s());
		events_.push_back({next + samples(0.96 + 0.012), EV_PACKET_DUE, next});
	}
}

void P1Station::deliver(const RxPacket &p)
{
	if (p.status & 0x80) return;                    // QRT packets are not printed
	const int mode = (p.status >> 2) & 3;
	if (debug_) {                                   // the packet's data bytes as received
		std::string h = "rx packet status " + std::to_string(p.status) + " data";
		char b[4];
		for (int i = 0; i < p.data_bits / 8; i++) { std::snprintf(b, sizeof b, " %02x", p.data[i]); h += b; }
		log(h);
	}
	std::string out;
	if (mode == 1 || mode == 2) {
		// Mode 2: Huffman with upper and lower case swapped (the reference unit data type 2 of
		// P2/P3, the protocol description 5.1). The reference V4.1r never sent it in P1,
		// upper-case text included (type 1 throughout, [bb-emu], fix 164); read anyway.
		BitReader r(p.data, size_t(p.data_bits));
		for (int c; (c = huffman_next(r)) >= 0;) if (c != IDLE_RS) out += char(mode == 2 && std::isalpha(c) ? c ^ 0x20 : c);
	} else {
		for (int i = 0; i < p.data_bits / 8; i++) if (p.data[i] != IDLE_RS) out += char(p.data[i]);
	}
	// The caller's first text is "<level><call>\r": as callee the firmware
	// does not print it and shows that call as the remote station (emulated
	// firmware, our P1 calls, 2026-10-20). Held until its CR (at 100 Bd it
	// may take two packets); not an identification after 16 characters.
	if (ident_pending_) {
		ident_buf_ += out; out.clear();
		const size_t cr = ident_buf_.find('\r');
		if (cr != std::string::npos && cr >= 2 && std::isdigit(uint8_t(ident_buf_[0]))) {
			std::string who = ident_buf_.substr(1, cr - 1);
			for (auto &ch : who) ch = char(std::toupper(uint8_t(ch)));
			remote_ = who;
			out = ident_buf_.substr(cr + 1);
			ident_pending_ = false; ident_buf_.clear();
		} else if (cr != std::string::npos || ident_buf_.size() > 16 || (!ident_buf_.empty() && !std::isdigit(uint8_t(ident_buf_[0])))) {
			out.swap(ident_buf_); ident_pending_ = false;
		}
	}
	if (!std::getenv("P1_NO_ESCAPE")) {             // the SI layer (compress.hpp SiFilter); may span packets
		std::string o;
		if (std::getenv("P1_OLD_ESCAPE")) {         // the old rule: 1C + anything below 0x61 printed as a plain FS
			for (char ch : out) {
				const uint8_t c = uint8_t(ch);
				if (esc_pending_) {
					esc_pending_ = false;
					if (c >= 0x61 && c <= 0x7F) { o += char(c - 0x60); continue; }
					o += char(0x1C);
				}
				if (c == 0x1C) { esc_pending_ = true; continue; }
				o += ch;
			}
		} else for (char ch : out) si_.feed(uint8_t(ch), o);
		out.swap(o);
	}
	last_rx_text_ = !out.empty();
	rx_ += out;
}

void P1Station::disconnect(const std::string &why)
{
	log("disconnected: " + why);
	why_ = why;
	if (cfg_.inline_status) rx_ += "*** DISCONNECTED (" + why + ")\r";
	state_ = cfg_.inline_status ? ST_LISTEN : ST_IDLE;
	events_.clear();
	txq_.clear(); want_qrt_ = false; pending_qrt_ = false; pending_used_ = 0;
	last_rx_counter_ = -1; rx_ok_since_connect_ = false; sent_text_ = false; must_break_in_ = false; rej_cs4_ = rej_total_ = 0;
	bk_req_ = cho_req_ = false; bk_hold_until_ = 0;
}

int16_t P1Station::modulate()
{
	while (!bursts_.empty() && bursts_.front().end + 2 < n_) bursts_.erase(bursts_.begin());
	const Burst *b = nullptr;
	for (const auto &x : bursts_) if (n_ >= x.onset && n_ < x.end) { b = &x; break; }
	ptt_ = b != nullptr;
	if (!b) { phase_ = 0; return 0; }
	int bit = 1;                                   // lead-in: mark of this polarity
	if (n_ >= b->first_bit) {
		double t = double(n_ - b->first_bit);
		for (const auto &s : b->segs) {
			const double len = s.bits.size() * spb(s.baud);
			if (t < len) { bit = s.bits[size_t(t / spb(s.baud))]; break; }
			t -= len;
		}
	}
	if (b->inv) bit ^= 1;
	const float f = (bit ? cfg_.tone_hi : cfg_.tone_lo) + cfg_.tx_shift_hz;
	phase_ += 2.0 * M_PI * f / cfg_.fs;
	if (phase_ > 2 * M_PI) phase_ -= 2 * M_PI;
	// 2 ms raised-cosine ramps at both ends.
	const double ramp = 0.002 * cfg_.fs;
	double g = 1.0;
	if (n_ - b->onset < ramp) g = 0.5 - 0.5 * std::cos(M_PI * (n_ - b->onset) / ramp);
	if (b->end - n_ < ramp) g = std::min(g, 0.5 - 0.5 * std::cos(M_PI * (b->end - n_) / ramp));
	return int16_t(cfg_.amplitude * g * std::sin(phase_));
}

}  // namespace fmodem
