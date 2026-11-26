// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// P2Station member functions, part 5 of 8 (split out of p2_station.hpp so they build in parallel).
#include "p2_station.hpp"

namespace fmodem {

void P2Station::dedup_repack(std::string &dec)
{
	static const bool off = std::getenv("P2_NO_REPACK_DEDUP") != nullptr;
	const bool cand = repack_cand_;
	repack_cand_ = false;
	if (off || dec.empty()) return;
	auto common = [](const std::string &a, const std::string &b) { size_t k = 0; while (k < a.size() && k < b.size() && a[k] == b[k]) k++; return k; };
	if (cand && !last_dec_.empty()) {
		const size_t m = std::min(dec.size(), last_dec_.size());
		if (m >= 4 && common(dec, last_dec_) == m) { resend_ = last_dec_; log("P2: a repacked resend of the last packet (" + std::to_string(last_dec_.size()) + " characters we have)"); }
	}
	const std::string orig = dec;
	if (!resend_.empty()) {
		const size_t m = std::min(dec.size(), resend_.size()), k = common(dec, resend_);
		if (k == m) { dec.erase(0, k); resend_.erase(0, k); log("P2: " + std::to_string(k) + " characters already delivered, dropped"); }
		else resend_.clear();
	}
	last_dec_ = orig;
}

void P2Station::repack_seen(const uint8_t *info, int nbytes, int status, int lvkey)
{
	static const bool off = std::getenv("P2_NO_REPACK_DEDUP") != nullptr;
	if (off || last_dec_.empty()) return;
	const std::string dec = text::decode((status >> 2) & 7, info, nbytes, !p3_);
	const size_t m = std::min(dec.size(), last_dec_.size());
	size_t k = 0;
	while (k < m && dec[k] == last_dec_[k]) k++;
	if (m < 4 || k != m) return;
	p3_note_rx(lvkey, info, nbytes);                  // a further copy at this level: not new
	repack_cand_ = true;
	deliver(info, nbytes, status);                    // fix 172 drops what we have
}

void P2Station::deliver(const uint8_t *info, int nbytes, int status)
{
	const int type = status < 0 ? 0 : (status >> 2) & 7;
	if (status >= 0 && nbytes > 0) last_rx_type_ = type;
	if (debug_ || (type >= 3 && !text::kPmcHave[(type >> 1) & 1])) {
		std::string h;
		for (int i = 0; i < nbytes; i++) { char b[4]; std::snprintf(b, sizeof b, "%02x", info[i]); h += b; }
		log("rx data type " + std::to_string(type) + " status " + std::to_string(status) + " " + h);
	}
	int n_idle = 0;
	std::string dec = text::decode(type, info, nbytes, !p3_, &n_idle);   // PMC without tables yields nothing (logged above)
	if (status >= 0) rx_full_ = n_idle == 0 && !dec.empty();   // fix 217: the packet held no fill
	if (status >= 0) dedup_repack(dec);                 // fix 172
	// Front panel: data = any character decoded; idle = idle fill. Compressed
	// idle is coded bits (PMC idle packets are 0f8f87..., not 0x1e), so for
	// those take a packet without "more" (the sender's queue ran dry, the
	// rest padded) as holding idle.
	if (!dec.empty()) led_rx_data_ = true;
	if (dec.empty() || (type == 0 ? std::memchr(info, 0x1e, size_t(nbytes)) != nullptr : status >= 0 && !(status & 0x20))) led_rx_idle_ = true;
	{
		const size_t base = rx_.size();
		rx_ += rle_.feed(dec);
		for (auto &e : rle_.si.ext_done) if (si_rx_.size() < 16) si_rx_.push_back({base + e.pos, std::move(e.body)});   // our 'X' SIs (host layer)
		rle_.si.ext_done.clear();
	}
	pstats_.rx_bytes += dec.size();
}

bool P2Station::fsk_offset_hz(int64_t a, int64_t b, double &hz) const
{
	a = std::max<int64_t>(a, std::max<int64_t>(0, n_ - (kRing - 2)));
	b = std::min<int64_t>(b, n_);
	const int n = int(b - a);
	if (n < int(samples(0.08))) return false;
	std::vector<double> x(static_cast<size_t>(n));
	for (int k = 0; k < n; k++) x[size_t(k)] = raw_[size_t((a + k) % kRing)] * (0.5 - 0.5 * std::cos(2 * M_PI * (k + 0.5) / n));
	auto pw = [&](double f) {
		const std::complex<double> w = std::polar(1.0, -2 * M_PI * f / cfg_.fs);
		std::complex<double> ph(1, 0), acc(0, 0);
		for (int k = 0; k < n; k++) { acc += x[size_t(k)] * ph; ph *= w; if ((k & 1023) == 1023) ph /= std::abs(ph); }
		return std::norm(acc);
	};
	auto S = [&](double d) { return pw(cfg_.tone_lo + d) + pw(cfg_.tone_hi + d); };
	// Fix 202: the offsets' powers are computed over the compute pool (each as before, then
	// taken in order: the same result); one by one this was ~11 ms of the tick that answers a
	// set-up packet (and the call's: the same on the call), on the x86 build machine.
	auto S_all = [&](const std::vector<double> &ds) {
		std::vector<double> v(ds.size());
		const int parts = std::max(1, std::min(par::ways(), int(ds.size())));
		par::parallel_for(parts, [&](int w) {
			for (size_t i = ds.size() * size_t(w) / size_t(parts); i < ds.size() * size_t(w + 1) / size_t(parts); i++) v[i] = S(ds[i]);
		});
		return v;
	};
	double best = -1, bd = 0;
	// fix 185: around the offset the call / reply was found at (p1_df_), +-90 Hz (was around 0)
	const double c0 = std::getenv("P1_AFC_CENTER0") ? 0.0 : std::round(p1_df_ / 2) * 2;
	std::vector<double> ds;
	for (double d = c0 - 90; d <= c0 + 90; d += 2) ds.push_back(d);
	const std::vector<double> sc = S_all(ds);
	for (size_t i = 0; i < ds.size(); i++) if (sc[i] > best) { best = sc[i]; bd = ds[i]; }
	std::vector<double> srt(sc);
	std::nth_element(srt.begin(), srt.begin() + long(srt.size() / 2), srt.end());
	const double med = srt[srt.size() / 2];
	if (!(best > 4.0 * med)) return false;
	double fb = bd, fv = best;
	std::vector<double> df;
	for (double d = bd - 2; d <= bd + 2; d += 0.25) df.push_back(d);
	const std::vector<double> sf = S_all(df);
	for (size_t i = 0; i < df.size(); i++) if (sf[i] > fv) { fv = sf[i]; fb = df[i]; }
	hz = fb;
	return true;
}

void P2Station::tx_est_feed(double hz, const char *what)
{
	if (tx_est_n_ == 0) { tx_est_ = hz; tx_est_n_ = 1; return; }
	// one reading far from the average (a wrong unwrap, a noise hit) is not taken; four in a row are
	if (std::fabs(hz - tx_est_) > 8.0 && ++tx_est_out_ < 4) {
		if (debug_ && std::getenv("TX_FOLLOW_DEBUG")) log(std::string("tx follow: ") + what + " " + std::to_string(hz) + " Hz far from " + std::to_string(tx_est_) + ": not taken");
		return;
	}
	if (tx_est_out_ >= 4) tx_est_ = hz;
	tx_est_out_ = 0;
	tx_est_ += 0.25 * (hz - tx_est_);
	tx_est_n_++;
}

double P2Station::track_step_hz(double err_hz, bool p3)
{
	const double e = std::max(-kTxFollowMaxHz, std::min(kTxFollowMaxHz, err_hz)), ae = std::fabs(e);
	if (ae < (p3 ? 1.5 : 6.0)) return 0;
	const double step = std::max(0.15, std::min(3.0, (p3 ? 0.05 : 0.025) * ae));
	return e < 0 ? -step : step;
}

void P2Station::tx_shift_choose()
{
	const double t = tx_target();
	if (tx_follow_old() || !tx_target_follows()) {
		if (std::fabs(t - tx_df_) >= 0.5) { if (debug_) log("transmit shift " + std::to_string(t) + " Hz (was " + std::to_string(tx_df_) + ")"); tx_df_ = t; }
		return;
	}
	if (n_ - tx_step_at_ < int64_t(0.9 * cyc_s() * cfg_.fs)) return;
	const double u = track_step_hz(t - tx_df_, p3_);
	if (u == 0) return;
	tx_step_at_ = n_;
	const double was = tx_df_;
	tx_df_ += u;
	if (debug_) log("transmit shift " + std::to_string(tx_df_) + " Hz (was " + std::to_string(was) + ", " + std::to_string(u) + " Hz toward " + std::to_string(t) + ")");
}

double P2Station::tx_target() const
{
	if (!tx_afc_on()) return 0;
	const double fsk = fsk_afc_w_ > 0 ? fsk_afc_sum_ / fsk_afc_w_ : 0.0;
	if (std::fabs(fsk) >= tx_afc_min()) return fsk;                 // fix 145, every role
	if (!master_ || !tx_follow_on() || tx_est_n_ == 0) return 0;
	const double v = std::max(-kTxFollowMaxHz, std::min(kTxFollowMaxHz, tx_est_));
	return std::fabs(v) >= 1.0 ? v : 0.0;
}

void P2Station::fsk_afc_add(int64_t a, int64_t b, const char *what)
{
	if (!fsk_afc_on()) return;
	double hz = 0;
	if (!fsk_offset_hz(a, b, hz)) { if (debug_) log(std::string("fsk afc: no clear peak (") + what + ")"); return; }
	// Fix 207: a measurement of a reply to our call or set-up packet is not taken if it lies more
	// than 45 Hz from the tones the reply was read at (p1_df_, once a grid offset or an earlier
	// measurement has set them; a real reply measures within ~1 Hz of an earlier measurement); nor,
	// once 2 measurements are in, more than 25 Hz from their mean. (As callee the first
	// measurement, on the call, is taken as before: fix 185's 100 Hz call read on the nominal
	// tones.) In noise (rx noise 3,000-3,500, we call at +80 Hz) the replies measured 79.5,
	// 81.0, then 127.75 (78.5, then 127.25): the mean went to 96 (103) Hz, the P1 link was
	// handed over 16 (23) Hz off: 66 (0) of 600 characters.
	// P1_AFC_NO_OUTLIER=1: every measurement taken (the old way).
	if (!std::getenv("P1_AFC_NO_OUTLIER") && ((std::strcmp(what, "P1 reply") == 0 && (p1_df_ != 0 || fsk_afc_n_ >= 1) && std::fabs(hz - p1_df_) > 45.0) || (fsk_afc_n_ >= 2 && std::fabs(hz - fsk_afc_sum_ / fsk_afc_w_) > 25.0))) {
		log("fsk afc " + std::to_string(hz) + " Hz (" + what + "): read at " + std::to_string(p1_df_) + " Hz, " + (fsk_afc_n_ ? std::to_string(fsk_afc_sum_ / fsk_afc_w_) + " Hz so far" : std::string("none so far")) + ": not taken");
		return;
	}
	fsk_afc_n_++;
	const double w = double(b - a) / cfg_.fs;
	fsk_afc_sum_ += w * hz; fsk_afc_w_ += w;
	afc_ = (fsk_afc_sum_ / fsk_afc_w_) / kAfcHzPerUnit;
	tx_est_ = fsk_afc_sum_ / fsk_afc_w_; tx_est_n_ = 1; tx_est_out_ = 0;   // fix 223: the FSK measurement seeds it
	if (const double t = tx_target(); t != tx_df_) { tx_df_ = t; log("transmit shift " + std::to_string(tx_df_) + " Hz (set-up FSK)"); }
	if (!std::getenv("P1_ACQ_NO_TRACK") && (p1_df_ != 0 || std::fabs(fsk_afc_sum_ / fsk_afc_w_) >= 30.0)) p1_df_ = fsk_afc_sum_ / fsk_afc_w_;   // fix 185
	log("fsk afc " + std::to_string(hz) + " Hz (" + what + "), now " + std::to_string(afc_hz()) + " Hz");
}

void P2Station::on_p1_again(int64_t exp)
{
	if (state_ != ST_IRS || p3_) return;
	uint8_t bytes[24];
	int64_t t1 = 0;
	if (p1_setup_at(exp - samples(0.0135), t1, bytes) && (bytes[21] & 0x10)) {
		log("P2: the caller sent its set-up packet again: P2-accept again");
		events_.clear();
		queue_accept(t1 + samples(kP1Reply - cfg_.rt_latency_s + test_call_answer_s()), p2arq::kP2Accept);
		acc_first_ = tx_.back().start;                // fix 222: the set-up packet read again: the grid restarts from this accept
		enter_psk_irs(t1 - samples(0.0225) + samples(cyc_s() + 0.036));
		p2_setup_watch_ = true;
		return;
	}
	if (++misses_ > cfg_.timeout_cycles) { disconnect("timeout"); return; }
	if (!std::getenv("P2_SETUP_NAK")) { p2_accept_again(exp, "no set-up packet read"); return; }   // fix 218
	const int64_t next = exp + samples(cyc_s());
	schedule(next + samples(0.86), EV_PACKET, next);
}

void P2Station::enter_psk_irs(int64_t expected_start)
{
	state_ = ST_IRS;
	level_ = 2; long_ = false;
	psk_swap_ = std::getenv("P2_SWAP_START") != nullptr;   // experiment hook (normally false: start low)
	ack_phase_ = 1;                                 // first acknowledgement is P
	last_ack_cs_ = p2arq::CS_Q;                     // "nothing yet" = not the P that is due
	rx_fails_ = 0;
	last_rx_counter_ = -1;
	misses_ = 0;
	good_run_ = 0;
	schedule(expected_start + samples(0.86), EV_PACKET, expected_start);
}

int P2Station::p3_search_us(bool ok) const
{
	if (p3_drift_off()) return ok ? 0 : 2000;
	if (ok) return p3_drift_on_ ? 2000 : 0;
	static const bool widen_off = std::getenv("P3_WIDEN_OFF") != nullptr;   // test: (2) off
	static const bool widen_always = std::getenv("P3_WIDEN_ALWAYS") != nullptr;   // test: without the audio layer's word
	if (widen_off || (!widen_always && (slip_hint_n_ < 0 || n_ - slip_hint_n_ > samples(kSlipHintS)))) return 2000;
	return std::min(kP3SearchMaxUs, 2000 + 5000 * std::max(0, misses_ - 1));
}

void P2Station::p3_drift_note(double err, int cycles)
{
	if (p3_drift_off() || cycles < 1) return;
	const double ms = cfg_.fs / 1000.0, e = err / cycles;
	if (!p3_drift_on_) {
		const bool two = std::fabs(e) >= ms && std::fabs(p3_drift_prev_) >= ms && (e > 0) == (p3_drift_prev_ > 0);
		const bool jump = cycles >= 2 && std::fabs(err) >= 3 * ms;
		if (two || jump) {
			p3_drift_on_ = true; p3_drift_quiet_ = 0;
			p3_drift_ = std::max(-4 * ms, std::min(4 * ms, two ? 0.5 * (e + p3_drift_prev_) : e));
			log("P3: our clock drifts against the sender's: " + std::to_string(p3_drift_ / ms) + " ms per cycle (followed)");
		}
	} else {
		p3_drift_ = std::max(-4 * ms, std::min(4 * ms, p3_drift_ + 0.5 * e));
		if (std::fabs(p3_drift_) < 0.3 * ms && std::fabs(err) < 0.5 * ms) {
			if (++p3_drift_quiet_ >= 3) { p3_drift_reset(); log("P3: the clock drift has stopped"); return; }
		} else p3_drift_quiet_ = 0;
	}
	p3_drift_prev_ = e;
}

void P2Station::schedule_p3_rx(int64_t next, bool ok)
{
	// (In long mode the short look stays: the sender may go back to short
	// packets by itself; a failed short look is then silent.)
	watch_p3_next_ = next;
	schedule(next + samples(kP3Launch), EV_P3_LAUNCH, next, p3_search_us(ok));
	schedule(next + samples(0.88), EV_PACKET, next);
	if (p3_rx_long_ || p3_r_pending_) {              // (a pending switch either way: both looks)
		schedule(next + samples(kP3LongIn), EV_P3L_LAUNCH, next);
		schedule(next + samples(kP3LongIn + 0.004), EV_PACKET, next, 1);
	}
}

void P2Station::link_watch()
{
	static const bool off = std::getenv("P2_WATCHDOG_OFF") != nullptr;
	if (off) return;
	bool live = ptt_ || !tx_.empty();
	for (const Event &e : events_) {
		if (live) break;
		switch (e.type) {
		case EV_P1_CS: case EV_P1_PKT: case EV_P1_AGAIN: case EV_REPLY: case EV_BK_DONE: case EV_PACKET:
		case EV_P3: case EV_BK_DATA: case EV_BK_AGAIN: case EV_UNPROTO: live = true; break;
		default: break;
		}
	}
	if (live) { watch_since_ = -1; return; }
	if (watch_since_ < 0) { watch_since_ = n_; return; }
	const int64_t cyc = samples(cyc_s());
	if (n_ - watch_since_ < samples((p3_rx_long_ || p3_r_pending_ ? lcyc_s() : cyc_s()) + 0.5)) return;
	watch_fires_++;
	if (p3_ && state_ == ST_IRS && watch_p3_next_ >= 0) {
		int64_t next = watch_p3_next_ + cyc;
		int lost = 1;
		while (next < n_) { next += cyc; lost++; }
		log("link stalled: no look scheduled for " + std::to_string(double(n_ - watch_since_) / cfg_.fs) + " s as P3 receiver: looks made again at the next slot (" +
		    std::to_string(lost) + " cycle(s) counted failed)");
		misses_ += lost;
		watch_since_ = -1;
		if (misses_ > cfg_.timeout_cycles) { disconnect("timeout"); return; }
		schedule_p3_rx(next, false);
		return;
	}
	log("link stalled: nothing scheduled for " + std::to_string(double(n_ - watch_since_) / cfg_.fs) + " s (state " + std::to_string(int(state_)) + "): counted as a failed cycle");
	watch_since_ += cyc;
	if (++misses_ > cfg_.timeout_cycles) disconnect("timeout");
}

void P2Station::long_products(int lv, const std::vector<float> &x, double fs, double t, bool both, bool first_odd,
	                          std::vector<std::complex<double>> &d0, std::vector<std::complex<double>> &d1)
{
	const p3::Layout &L = p3::layout_long(lv);
	auto fill_wide = [&](const std::vector<std::array<std::complex<double>, 18>> &zf, bool odd, std::vector<std::complex<double>> &d) {
		d.assign(size_t(L.nwords()), std::complex<double>(0, 0));
		for (int pp = 1; pp < L.npulses(); pp++)
			for (int j = 0; j < L.carriers(pp); j++) {
				const int k = L.carrier(pp, j, odd);
				d[size_t(L.word_index(pp, j))] = zf[size_t(pp)][size_t(k)] * std::conj(zf[size_t(pp - 1)][size_t(k)]);
			}
	};
	auto fill_narrow = [&](bool odd, std::vector<std::complex<double>> &d) {
		d.assign(size_t(L.nwords()), std::complex<double>(0, 0));
		const auto z = p3::demod_lv(lv, x.data(), long(x.size()), fs, t, odd, 0, nullptr, -1, true);
		for (int pp = 1; pp < L.npulses(); pp++)
			for (int j = 0; j < L.carriers(pp); j++) {
				const int k = L.carrier(pp, j, odd);
				int jp = -1;
				for (int q = 0; q < L.carriers(pp - 1); q++) if (L.carrier(pp - 1, q, odd) == k) jp = q;
				if (jp >= 0) d[size_t(L.word_index(pp, j))] = z[size_t(pp)][size_t(j)] * std::conj(z[size_t(pp - 1)][size_t(jp)]);
			}
	};
	if (!L.narrow) {
		const auto zf = p3::demod_flat(lv, x.data(), long(x.size()), fs, t, 0, nullptr, -1, true);
		if (both || !first_odd) fill_wide(zf, false, d0);
		if (both || first_odd) fill_wide(zf, true, d1);
	} else {
		if (both || !first_odd) fill_narrow(false, d0);
		if (both || first_odd) fill_narrow(true, d1);
	}
}

void P2Station::long_products_planned(const p3::FlatPlan &P, const std::vector<float> &x,
	                                  std::vector<std::complex<double>> &d0, std::vector<std::complex<double>> &d1, p3::FlatProgress *prog)
{
	const p3::Layout &L = p3::layout_long(P.level);
	const auto zf = p3::demod_flat_run(P, x.data(), long(x.size()), nullptr, prog);
	for (int m = 0; m < 2; m++) {
		auto &d = m ? d1 : d0;
		d.assign(size_t(L.nwords()), std::complex<double>(0, 0));
		for (int pp = 1; pp < L.npulses(); pp++)
			for (int j = 0; j < L.carriers(pp); j++) {
				const int k = L.carrier(pp, j, m != 0);
				d[size_t(L.word_index(pp, j))] = zf[size_t(pp)][size_t(k)] * std::conj(zf[size_t(pp - 1)][size_t(k)]);
			}
	}
}

p3::LongRx P2Station::long_decode_d(int lv, const std::vector<std::complex<double>> &d)
{
	std::vector<double> st(d.size(), 0.0), wt(d.size(), 1.0);
	double m = 0; int n = 0;
	for (size_t w = 0; w < d.size(); w++) {
		const double v = -std::arg(d[w]) / (2 * M_PI) * 16; st[w] = v < 0 ? v + 16 : v;
		const double a = std::abs(d[w]); if (a > 0) { m += a; n++; }
	}
	if (n) for (size_t w = 0; w < d.size(); w++) wt[w] = std::abs(d[w]) / (m / n);
	return p3::long_decode(lv, st.data(), n ? wt.data() : nullptr);
}

double P2Station::long_phase_err(int lv, const std::vector<std::complex<double>> &d)
{
	static const int kHdr[7] = {0, 0, 6, 14, 16, 18, 18};
	const double m = lv <= 3 ? 8.0 : 4.0;
	double e2 = 0; int n = 0;
	for (size_t w = size_t(9 * kHdr[lv]); w < d.size(); w++) {
		if (std::abs(d[w]) <= 0) continue;
		double v = -std::arg(d[w]) / (2 * M_PI) * 16 - 2.0;
		v = std::fmod(v + 64.0, m); if (v > m / 2) v -= m;
		e2 += v * v; n++;
	}
	return n ? std::sqrt(e2 / n) : -1;
}

double P2Station::long_debias(int lv, std::vector<std::complex<double>> &d)
{
	static const bool off = std::getenv("P3_LONG_BIAS_OFF") != nullptr;
	if (off || lv < 2 || lv > 6 || d.empty()) return 0;
	static const int kHdr[7] = {0, 0, 6, 14, 16, 18, 18};
	const int P = lv <= 3 ? 2 : 4;
	std::complex<double> acc(0, 0);
	for (size_t w = size_t(9 * kHdr[lv]); w < d.size(); w++) {
		const double a = std::abs(d[w]);
		if (a <= 0) continue;
		acc += std::pow(d[w] / a, P) * a;
	}
	if (std::abs(acc) <= 0) return 0;
	const double m = 16.0 / P;                     // grid spacing (1/16 turn)
	double b = -(std::arg(acc) + M_PI * P / 4.0) * 16.0 / (2 * M_PI * P);
	b = std::fmod(b + 64.0 * m + m / 2, m) - m / 2;
	const std::complex<double> rot = std::polar(1.0, 2 * M_PI * b / 16.0);
	for (auto &v : d) v *= rot;
	return b;
}

auto P2Station::decode_p3_long_lv(int64_t slot, const std::vector<float> &x, int64_t a, int lv, const Snap &sp,
	                         int mq_lv, int mq_cnt, const std::vector<MqLong> *mq_store, const P3LSync *pre) -> P3Long
{
	P3Long out;
	const double fs = cfg_.fs, base = double(slot) + 0.8 * T_ - double(a);
	if (lv == 1) {
		// SL1 long: P2-style two carriers, 330 words; each word's two
		// nibble steps from the pulse amplitudes (d.lo/d.hi start one pulse
		// before pulse 0), both carrier orders and +-2 pulses tried
		// (p3::long1_decode, the CRC decides). (The P2 long decoder,
		// tried first, never decoded the firmware's SL1 long packets.)
		const Demod d = demod(slot - samples(0.04), slot + samples(3.36), 334, &sp);
		if (!d.ok) return out;
		const int nw = p3::lng::level(1).nwords;
		std::vector<double> nb(size_t(2 * nw));
		for (int sw = 0; sw < 2; sw++)
			for (int o = 0; o >= -2 && o <= 2; o = o >= 0 ? -o - 1 : -o) {
				const auto &A = sw ? d.hi : d.lo, &B = sw ? d.lo : d.hi;
				bool fits = true;
				for (int w = 0; w < nw && fits; w++) {
					const int i = w + 1 + o;
					if (i < 1 || i >= int(A.size()) || i >= int(B.size())) { fits = false; break; }
					nb[size_t(2 * w)] = nib(A[size_t(i - 1)], A[size_t(i)]);
					nb[size_t(2 * w + 1)] = nib(B[size_t(i - 1)], B[size_t(i)]);
				}
				if (!fits) continue;
				const p3::LongRx r = p3::long1_decode(nb.data());
				if (r.ok) { out.ok = true; out.level = 1; out.data = r.data; out.status = r.status; return out; }
			}
		return out;
	}
	const p3::LvSync sy = pre ? pre->s : p3::sync_level(lv, x.data(), long(x.size()), fs, base, 3.0);
	const p3::Layout &L = p3::layout_long(lv);
	const bool mq = lv == mq_lv && !std::getenv("P3_NO_MARQ");
	// wide levels: both carrier maps tried (one demodulation); narrow: the sync's map
	std::vector<std::complex<double>> d0, d1;
	static const bool prof = std::getenv("P3_PROF") != nullptr;
	const auto t0 = std::chrono::steady_clock::now();
	const bool planned = !L.narrow && pre && pre->plan && pre->plan->ok && pre->plan->t0 == sy.t && pre->plan->df == 0 && pre->plan->lng && pre->plan->level == lv;
	if (planned) {
		if (pre->prog) { std::lock_guard<std::mutex> lk(pre->prog->m); long_products_planned(*pre->plan, x, d0, d1, &pre->prog->g); }
		else long_products_planned(*pre->plan, x, d0, d1);
	}
	else long_products(lv, x, fs, sy.t, !L.narrow, sy.odd, d0, d1);
	if (prof) std::fprintf(stderr, "P3 long level %d: demodulation %.1f ms (planned %d)\n", lv, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), int(planned));
	const double b0 = long_debias(lv, d0), b1 = long_debias(lv, d1);   // fix 241
	// Wide levels: both maps' Viterbis side by side (the compute pool), the sync's map taken first as
	// before: the same decodes, the other map's ready at once when the first fails (on the Raspberry
	// Pi 4 a failed long SL6 decode with memory ARQ was four Viterbis in a row, 117 ms before the
	// faster Viterbi). P3_LONG_PAR_OFF=1: one after the other, as before.
	static const bool par_off = std::getenv("P3_LONG_PAR_OFF") != nullptr;
	p3::LongRx rs[2];
	const bool par2 = !par_off && !L.narrow && !d0.empty() && !d1.empty();
	if (par2) par::parallel_for(2, [&](int oi) { const bool odd = oi ? !sy.odd : sy.odd; rs[oi] = long_decode_d(lv, odd ? d1 : d0); });
	for (int oi = 0; oi < 2; oi++) {
		const bool odd = oi ? !sy.odd : sy.odd;
		const auto &d = odd ? d1 : d0;
		if (d.empty()) continue;
		const p3::LongRx r = par2 ? std::move(rs[oi]) : long_decode_d(lv, d);
		if (r.ok) { out.ok = true; out.level = lv; out.data = r.data; out.status = r.status; out.perr = long_phase_err(lv, d); out.bias = odd ? b1 : b0; return out; }
		if (L.narrow) break;                       // the sync's map only
	}
	// memory ARQ: this copy's products (both maps), combined with the stored ones
	if (mq) {
		if (L.narrow) { long_products(lv, x, fs, sy.t, false, !sy.odd, d0, d1); long_debias(lv, sy.odd ? d0 : d1); }   // the other map, now (fix 241: its bias off)
		out.mq_level = lv;
		out.d0 = std::move(d0); out.d1 = std::move(d1);
		std::vector<const MqLong *> stc;
		if (mq_store) for (const auto &c : *mq_store) if (c.level == lv) stc.push_back(&c);
		// both combinations side by side (as the maps above), taken in order
		p3::LongRx mr[2];
		auto combo = [&](int m0) {
			auto sum = m0 ? out.d1 : out.d0;
			for (size_t c = 0; c < stc.size(); c++) {
				const bool odd = ((stc.size() - c) & 1) ? !m0 : m0;   // one map flip per transmission back
				const auto &d = odd ? stc[c]->d1 : stc[c]->d0;
				for (size_t w = 0; w < sum.size() && w < d.size(); w++) sum[w] += d[w];
			}
			mr[m0] = long_decode_d(lv, sum);
		};
		const bool mpar = !par_off && !stc.empty();
		if (mpar) par::parallel_for(2, combo);
		for (int m0 = 0; m0 < 2 && !stc.empty(); m0++) {
			if (!mpar) combo(m0);
			const p3::LongRx &r = mr[m0];
			if (r.ok && (mq_cnt < 0 || (r.status & 3) == mq_cnt)) {
				out.ok = true; out.level = lv; out.data = r.data; out.status = r.status; out.marq = int(stc.size()) + 1;
				return out;
			}
		}
	}
	return out;
}

auto P2Station::p3_long_pick(std::vector<P3Long> &rs) -> P3Long
{
	P3Long out;
	for (auto &r : rs) {
		if (r.mq_level) { out.mq_level = r.mq_level; out.d0 = std::move(r.d0); out.d1 = std::move(r.d1); }
		if (r.ok) { out.ok = true; out.level = r.level; out.data = std::move(r.data); out.status = r.status; out.marq = r.marq; out.perr = r.perr; out.bias = r.bias; return out; }
	}
	return out;
}

std::vector<int> P2Station::p3_long_levels(int64_t slot, bool *trimmed) const
{
	std::vector<int> lvs{p3_rx_level_};
	if (p3_rx_level_ > 1) lvs.push_back(p3_rx_level_ - 1);   // the sender steps down after our T
	if (lvs.size() > 1 && misses_ == 0 && !rx_t_state_ && p3l_last_ok_ == p3_rx_level_ && cpu_fb_at(slot)) { lvs.resize(1); if (trimmed) *trimmed = true; }   // CPU fallback (p3_fb_steady)
	return lvs;
}

void P2Station::launch_p3_long_sync(int64_t slot)
{
	if (p3_long_early_off()) return;
	p3_df_seed();
	const int64_t a = slot - samples(0.06), b = slot + samples(kP3Sync);
	const long n_long = long(std::max<int64_t>(slot + samples(kP3LongIn + 0.05) - a, 1));   // launch_p3_long's window
	const auto x = std::make_shared<const std::vector<float>>(p3_window(a, b));
	const double fs = cfg_.fs, base = double(slot) + 0.8 * T_ - double(a);
	p3l_early_ = P3LEarly{};
	p3l_early_.slot = slot; p3l_early_.df = p3_rx_df_;
	static const bool prof = std::getenv("P3_PROF") != nullptr;
	const auto w0 = std::chrono::steady_clock::now();
	for (int lv : p3_long_levels(slot)) {
		if (lv < 2 || lv > 6) continue;
		p3l_early_.f.emplace_back(lv, pool_.submit([x, lv, fs, base, n_long, w0]() {
			const auto s0 = std::chrono::steady_clock::now();
			P3LSync r;
			r.s = p3::sync_level(lv, x->data(), long(x->size()), fs, base, 3.0);
			if (!p3::layout_long(lv).narrow) r.plan = std::make_shared<const p3::FlatPlan>(p3::demod_flat_plan(lv, n_long, fs, r.s.t, 0, -1, true));
			if (prof) std::fprintf(stderr, "P3 long early sync: level %d: started %.1f ms after launch, took %.1f ms\n", lv,
			                       std::chrono::duration<double, std::milli>(s0 - w0).count(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count());
			return r;
		}).share());
		if (!p3_long_pre_off() && !p3::layout_long(lv).narrow) p3l_early_.prog.emplace_back(lv, std::make_shared<P3LProg>());
	}
}

void P2Station::advance_p3_long(int64_t slot)
{
	if (p3_long_pre_off() || p3l_early_.slot != slot || p3l_early_.df != p3_rx_df_ || p3l_early_.prog.empty()) return;
	const int64_t a = slot - samples(0.06), b = std::min(n_, slot + samples(kP3LongIn + 0.05));
	if (b - a <= 64) return;
	const auto x = std::make_shared<const std::vector<float>>(p3_window(a, b));
	const long n_avail = long(x->size()) - 64;
	for (const auto &pg : p3l_early_.prog) {
		std::shared_future<P3LSync> sf;
		for (const auto &e : p3l_early_.f) if (e.first == pg.first) sf = e.second;
		if (!sf.valid()) continue;
		const auto prog = pg.second;
		pool_.submit([x, n_avail, sf, prog]() {
			const P3LSync &s = sf.get();
			if (!s.plan || !s.plan->ok) return;
			std::lock_guard<std::mutex> lk(prog->m);
			p3::demod_flat_advance(*s.plan, x->data(), n_avail, prog->g);
		});
	}
}

void P2Station::launch_p3_long(int64_t slot)
{
	if (p3l_slot_ == slot && p3l_fut_.valid()) return;
	p3_df_seed();
	const int64_t a = slot - samples(0.06), b = slot + samples(kP3LongIn + 0.05);
	bool trimmed = false;
	const std::vector<int> lvs = p3_long_levels(slot, &trimmed);
	if (trimmed) fb_note(0, load_.fb_p3_levels);
	const auto x = std::make_shared<const std::vector<float>>(p3_window(a, b));
	const auto sp = std::make_shared<const Snap>(snapshot(slot - samples(0.04) - int64_t(2 * T_), slot + samples(3.36) + int64_t(2 * T_) + 1));
	p3l_slot_ = slot;
	const int mq_lv = p3_rx_level_, mq_cnt = (last_rx_counter_ + 1) & 3;
	const auto store = std::make_shared<const std::vector<MqLong>>(mql_);
	auto done = p3l_done_ = std::make_shared<std::atomic<int64_t>>(0);
	// the early searches, if for this slot and the same receive shift (the same audio)
	P3LEarly es;
	if (p3l_early_.slot == slot) es = std::move(p3l_early_);
	p3l_early_ = P3LEarly{};
	const bool es_use = es.slot == slot && es.df == p3_rx_df_;
	static const bool prof = std::getenv("P3_PROF") != nullptr;
	const auto w0 = std::chrono::steady_clock::now();
	std::vector<std::shared_future<P3Long>> fin;
	for (int lv : lvs) {
		std::shared_future<P3LSync> sf;
		std::shared_ptr<P3LProg> prog;
		if (es_use) for (const auto &e : es.f) if (e.first == lv) sf = e.second;
		if (es_use) for (const auto &e : es.prog) if (e.first == lv) prog = e.second;
		auto job = [this, slot, x, a, lv, sp, mq_lv, mq_cnt, store, sf, prog, w0]() {
			const auto q0 = std::chrono::steady_clock::now();
			P3LSync pre;
			if (sf.valid()) { pre = sf.get(); pre.prog = prog; }
			const auto q1 = std::chrono::steady_clock::now();
			P3Long r = decode_p3_long_lv(slot, *x, a, lv, *sp, mq_lv, mq_cnt, store.get(), sf.valid() ? &pre : nullptr);
			if (prof) std::fprintf(stderr, "P3 long level %d: started %.1f ms after launch, sync wait %.1f ms, early %d, decode %.1f ms, ok %d\n", lv,
			                       std::chrono::duration<double, std::milli>(q0 - w0).count(), std::chrono::duration<double, std::milli>(q1 - q0).count(), int(sf.valid()),
			                       std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - q1).count(), int(r.ok));
			return r;
		};
		if (cfg_.realtime) fin.push_back(pool_.submit(job).share());
		else {                                       // lockstep: in turn, up to the first that decodes
			if (!fin.empty() && fin.back().get().ok) break;
			std::promise<P3Long> pr; pr.set_value(job()); fin.push_back(pr.get_future().share());
		}
	}
	const bool dbg = debug_;
	auto pick = [fin, dbg, w0, done]() {
		std::vector<P3Long> rs;
		for (const auto &f : fin) { rs.push_back(f.get()); if (rs.back().ok) break; }   // in order; the first that decodes
		P3Long out = p3_long_pick(rs);
		if (dbg) std::fprintf(stderr, "P3 long worker: levels %zu took %.1f ms\n", fin.size(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count());
		done->store(wall_ns());
		return out;
	};
	if (cfg_.realtime) p3l_fut_ = pool_.submit(pick);
	else { std::promise<P3Long> pr; pr.set_value(pick()); p3l_fut_ = pr.get_future(); }
}

void P2Station::p3_long_lower(int lv, const std::string &why)
{
	lv = std::max(0, lv);
	if (lv >= p3l_ceil_) return;
	load_.fallbacks++;
	p3l_ceil_ = lv; p3l_room_n_ = 0; p3l_room_bad_ = false;
	if (p3l_raised_) { p3l_need_ = std::min(p3l_need_ * 2, 640); p3l_raised_ = false; }
	log("P3: long packets: " + why + (lv ? ", not asked for above SL" + std::to_string(lv) : ", not asked for any more") + " on this computer");
}

void P2Station::p3_long_measured(int lv, double ms)
{
	if (ms < kThinMs) {
		const std::string why = "long decode margin " + std::to_string(int(std::lround(ms))) + " ms at SL" + std::to_string(lv);
		if (lv >= p3l_ceil_ || backoff_evidence(1, why)) p3_long_lower(lv, why);
		return;
	}
	if (lv == p3l_ceil_) { if (ms >= 2 * kThinMs) p3l_room_n_++; else p3l_room_bad_ = true; }
}

void P2Station::p3_long_link_start()
{
	if (!p3_long_adaptive()) return;
	p3l_raised_ = false;
	if (p3l_ceil_ < 6 && p3l_room_n_ >= p3l_need_ && !p3l_room_bad_) {
		p3l_ceil_++; p3l_raised_ = true;
		log("P3: long packets: tried again up to SL" + std::to_string(p3l_ceil_) + " (the last link's long decodes kept up)");
	} else if (p3l_ceil_ < 6) log("P3: long packets asked for up to SL" + std::to_string(p3l_ceil_) + " on this computer (measured)");
	p3l_room_n_ = 0; p3l_room_bad_ = false; p3l_up_ = false;
}

void P2Station::on_p3_long(int64_t exp)
{
	if (p3l_slot_ != exp || !p3l_fut_.valid()) launch_p3_long(exp);
	const int64_t long_deadline = std::max(reply_at(exp, true) - samples(0.003), exp + samples(kP3LongIn + 0.03));
	if (cfg_.realtime && p3l_fut_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
		const int64_t step = samples(0.002);
		if (n_ + step < long_deadline) { schedule(n_ + step, EV_PACKET, exp, 1 | 2); return; }
		if (p3_long_adaptive()) { note_late(p3_rx_level_, false); if (p3_rx_level_ - 1 >= p3l_ceil_ || backoff_evidence(1, "long decode late at SL" + std::to_string(p3_rx_level_))) p3_long_lower(p3_rx_level_ - 1, "long decode late at SL" + std::to_string(p3_rx_level_)); }   // fix 241 (264: on evidence)
		else if (p3_long_budget_off()) note_late(p3_rx_level_, true);
		else { note_late(p3_rx_level_, false); p3_long_hold("long decode late at SL" + std::to_string(p3_rx_level_)); }
		log("P3: long decode not ready in time, counted as failed");
		p3l_fut_ = {}; p3l_slot_ = -1;                  // (a pool future: dropping it does not block)
		// Fix 251: a late long decode is a failed packet to the frequency search too. It was searched
		// only when a decode came back failed: on a Raspberry Pi 4 after a 50 Hz step every other
		// long decode was late, so readings came 7.5 s apart, by then ~10 Hz apart (the caller's
		// transmit follow walking, fix 246), never two agreeing: no re-measure, the link down to SL1
		// (loopback, both stations on the Pi). SIGNUM_FSTEP_LATE_OLD=1: not searched, as before.
		static const bool late_old = std::getenv("SIGNUM_FSTEP_LATE_OLD") != nullptr;
		if (!late_old) fs_submit(exp, exp - samples(0.02), exp + samples(1.3), 0);
		if (++misses_ > cfg_.timeout_cycles) { disconnect("timeout"); return; }
		reply_as_irs(exp, true, false, false);
		return;
	}
	const P3Long r = p3l_fut_.get();
	p3l_slot_ = -1;
	if (cfg_.realtime && p3l_done_ && p3l_done_->load()) {
		if (p3_long_adaptive()) {                     // fix 241
			note_margin(p3_rx_level_, long_deadline, p3l_done_->load(), false);
			if (cfg_.paced) p3_long_measured(p3_rx_level_, dstats_.last_ms);
		} else if (p3_long_budget_off()) note_margin(p3_rx_level_, long_deadline, p3l_done_->load(), true);
		else {
			note_margin(p3_rx_level_, long_deadline, p3l_done_->load(), false);
			if (cfg_.paced && cfg_.backoff && dstats_.last_ms < kThinMs) p3_long_hold("long decode margin " + std::to_string(int(std::lround(dstats_.last_ms))) + " ms at SL" + std::to_string(p3_rx_level_));
		}
	}
	// We asked for short packets (R pending). The short look answers a short packet it decoded (and
	// then drops this look); a failed short look leaves the answer to this one, since a long packet
	// may still be on the air. So reaching here, the short look failed. A failed long decode as well
	// was returned on unanswered (the comment said "the short look answered": it had not), and
	// nothing scheduled the next looks: the receiver went silent for good, no answer, no timeout,
	// until the sender timed out (bench, the reference unit sending 12,000, runs cv-1 and cv-4,
	// 2026-11-16: the first short packet after our R lost). Now a failed packet like any other:
	// counted, answered with the repeat request (R again: the switch is still pending) at the long
	// answer time (= the third short cycle's: the sender, if it took R, repeats its short packet
	// there), and both looks at the next slot. P3_R_FAIL_OLD=1: as before (silent).
	const bool r_short_pending = !p3_rx_long_;
	if (r_short_pending) {
		static const bool r_fail_old = std::getenv("P3_R_FAIL_OLD") != nullptr;
		if (!r.ok && r_fail_old) return;
		if (r.ok) {
			log("P3: R (short) not taken, the sender stayed on long packets");
			p3_rx_long_ = true; p3_r_pending_ = false;
		} else if (debug_) log("P3: R (short) pending: neither look decoded, answered as a failed packet");
	}
	if (!r.ok) {
		if (debug_) log("rx P3 long failed");
		fs_submit(exp, exp - samples(0.02), exp + samples(1.3), 0);   // the frequency search (2026-11-14)
		if (++misses_ > cfg_.timeout_cycles) { disconnect("timeout"); return; }
		if (!r_short_pending && r.mq_level && r.mq_level == p3_rx_level_) {   // (not a copy of short packets)
			mql_.push_back({r.mq_level, r.d0, r.d1});
			if (mql_.size() > kMqlMax) mql_.erase(mql_.begin());
		}
		const int lv_before = p3_rx_level_;
		reply_as_irs(exp, true, false, false);
		if (p3_rx_level_ != lv_before) mql_.clear();
		return;
	}
	misses_ = 0;
	p3l_last_ok_ = r.level;                      // the CPU fallback's steady state (p3_long_levels)
	marq_clear();
	mql_.clear();
	if (r.marq) log("P3: memory ARQ: long packet decoded at SL" + std::to_string(r.level) + " from " + std::to_string(r.marq) + " copies");
	const uint8_t status = r.status;
	const int cnt = status & 3;
	const bool fresh = p3_fresh(cnt, 10 + r.level, r.data.data(), int(r.data.size()));
	rx_more_ = p3_rx_lots(status, r.level, true);
	if (status & 0x80) { if (std::getenv("P2_TEST_TAKEBACK")) must_break_in_ = true; else qrt_rx_ = true; }   // test: take the link back on a QRT, as an RMS did (fix 91)
	if (fresh) {
		repack_cand_ = cnt == last_rx_counter_; last_rx_counter_ = cnt; p3_note_rx(10 + r.level, r.data.data(), int(r.data.size()));
		if ((status & 0x40) && !(status & 0x80)) must_break_in_ = true;   // fix 50
		if (!(status & 0x80)) deliver(r.data.data(), int(r.data.size()), status);
	}
	if (r.level != p3_rx_level_) { p3_rx_level_ = r.level; log("P3: receiving level " + std::to_string(p3_rx_level_) + " (long)"); }
	p3_q_ = r.marq ? -1 : r.perr; p3_q_lv_ = r.level; p3_q_long_ = true;   // fix 229
	// Fix 241: the receive shift follows long packets too (as the short ones', fix 70: half the
	// residual each packet), so a drifting peer stays inside the bias removal's reach (+-2 steps of
	// 1/16 turn per pulse at SL4-6: +-12.5 Hz). P3_LONG_DF_OFF=1: not followed.
	static const bool ldf_off = std::getenv("P3_LONG_DF_OFF") != nullptr || std::getenv("P3_LONG_BIAS_OFF") != nullptr;
	const double ldf = r.bias / (16 * p3::kT);
	if (!ldf_off && !r.marq && r.level >= 2 && std::fabs(ldf) < 12.0) { p3_rx_df_ += 0.5 * ldf; tx_est_feed(-p3_rx_df_, "P3 long packet"); }
	fs_decoded(r.level);                            // fix 261/263: a shift a decode confirmed
	if (debug_) log("rx P3 long status " + std::to_string(status) + " level " + std::to_string(r.level) + " (" + std::to_string(r.data.size()) + " bytes) perr " + std::to_string(r.perr) + " bias " + std::to_string(r.bias) + " shift " + std::to_string(p3_rx_df_));
	reply_as_irs(exp, true, fresh, true);
}

}  // namespace fmodem
