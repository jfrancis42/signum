// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// P2Station member functions, part 8 of 8 (split out of p2_station.hpp so they build in parallel).
#include "p2_station.hpp"

namespace fmodem {

bool P2Station::cs_onset(double coarse, double &onset)
{
	const int64_t a = int64_t(coarse) - samples(0.030), b = int64_t(coarse) + samples(0.090);
	const int w = std::max(1, int(samples(0.0025)));
	if (a - w < 0 || b > n_ || n_ - (a - w) >= int64_t(kRing) - 2) return false;
	std::vector<double> e(size_t(b - a), 0.0);
	double run = 0;
	for (int64_t t = a - w; t < b; t++) {
		run += std::fabs(double(raw_[size_t(t % kRing)]));
		if (t - w >= a - w) run -= std::fabs(double(raw_[size_t((t - w) % kRing)]));
		if (t >= a) e[size_t(t - a)] = run / w;
	}
	const size_t nf = size_t(samples(0.008));      // floor: the first 8 ms (before the onset, ~8 ms before `coarse`)
	double fl = 0; for (size_t i = 0; i < nf; i++) fl += e[i]; fl /= double(nf);
	std::vector<double> body(e.begin() + long(samples(0.040)), e.end());   // 10..90 ms after `coarse`
	std::nth_element(body.begin(), body.begin() + long(body.size() / 2), body.end());
	const double pl = body[body.size() / 2];
	if (pl < 4.0 * fl + 1.0) return false;
	const double thr = fl + 0.5 * (pl - fl);
	for (size_t i = nf; i < size_t(samples(0.040)); i++)
		if (e[i - 1] < thr && e[i] >= thr) {
			onset = double(a) + double(i - 1) + (thr - e[i - 1]) / (e[i] - e[i - 1]);
			long_onset_lag_ = coarse - onset;
			return true;
		}
	return false;
}

void P2Station::long_follow(double off, bool good)
{
	long_reads_++;
	if (const char *e = std::getenv("P2_TEST_LONG_CS")) {
		for (const char *p = e; *p; ) {
			int k = 0; double ms = 0; int n = 0;
			if (std::sscanf(p, "%d:%lf%n", &k, &ms, &n) >= 2 && k == long_reads_) {
				const bool weak = p[n] == 'w';
				off += ms * cfg_.fs / 1000.0;
				if (weak) good = false;
				log("test: long-cycle reading " + std::to_string(k) + " moved " + std::to_string(ms) + " ms" + (weak ? ", weak" : ""));
			}
			const char *q = std::strchr(p, ',');
			if (!q) break;
			p = q + 1;
		}
	}
	const double was = long_cs_off_;
	double d = off - was, moved = 0;
	std::string note;
	if (std::getenv("P2_LONG_FOLLOW_OLD")) {
		// Fix 77: a move up to 4 ms followed at once, a larger one (< 20 ms) when the look
		// before measured the same one (within 2 ms).
		if (was > 0 && std::fabs(d) < double(samples(0.02))) {
			const bool small = std::fabs(d) <= double(samples(0.004)) || std::getenv("P2_LONG_FOLLOW_ANY");
			const bool again = long_jump_ != 0 && std::fabs(d - long_jump_) <= double(samples(0.002));
			if (small || again) { moved = d; next_start_ += int64_t(std::llround(d)); long_jump_ = 0; }
			else long_jump_ = d;
		} else { long_cs_off_ = off; long_jump_ = 0; }
		if (long_jump_ != 0) note = " (a jump: held until seen again)";
	} else {
		// Where the signal lands on our unmoved lattice: the reading plus every move of ours.
		const double place = off + long_moved_;
		static const double step = std::getenv("P2_LONG_STEP_MS") ? std::atof(std::getenv("P2_LONG_STEP_MS")) / 1000.0 : 0.001;
		// (fix 217: with the onset readings, ~0.1 ms where the old ones wandered +-1 ms, the
		// follow is tighter: 0.6 / 0.2. At 130 ppm (0.5 ms a cycle) 0.4 / 0.08 lagged and the
		// firmware failed our long packets: a 130 ppm run, we call and send, 1,762 -> 3,425; clean the same.)
		static const double kp_env = std::getenv("P2_LONG_GAIN") ? std::atof(std::getenv("P2_LONG_GAIN")) : -1;
		static const double ki_env = std::getenv("P2_LONG_RATE_GAIN") ? std::atof(std::getenv("P2_LONG_RATE_GAIN")) : -1;
		const double kp = kp_env >= 0 ? kp_env : long_onset_mode_ ? kLongGainOnset : 0.4;
		const double ki = ki_env >= 0 ? ki_env : long_onset_mode_ ? kLongRateGainOnset : 0.08;
		const double lim = step * cfg_.fs;
		double m = 0;
		if (!good) { m = long_rate_; note = " (weak reading: drift estimate only)"; }
		else if (was < 0) { long_cs_off_ = off; long_cand_n_ = 0; note = " (reference)"; }
		else if (std::fabs(d) <= double(samples(0.004))) {
			long_rate_ = std::max(-lim, std::min(lim, long_rate_ + ki * d));
			m = long_rate_ + kp * d;
			long_cand_n_ = 0;
		} else if (long_cand_n_ > 0 && std::fabs(place - long_cand_) <= double(samples(0.002))) {
			long_cs_off_ = (long_cand_ + place) / 2 - long_moved_; long_cand_n_ = 0;
			log("long cycle: reference re-taken " + std::to_string(d / cfg_.fs * 1000.0) + " ms away (seen twice; not followed)");
		} else {
			long_cand_ = place; long_cand_n_ = 1;
			note = " (off the reference: not followed)";
		}
		moved = double(std::llround(std::max(-lim, std::min(lim, m))));
		next_start_ += int64_t(moved);
	}
	long_moved_ += moved;
	// Fix 259 (P2 stall after a break-in): as caller our answers go on our own transmit lattice
	// (reply_at: grid_ + 0.6 s), which is where the callee looks for them after its break-in
	// (its view of our last packet + 1.85 s). The follow above moves our packets, so it moves that
	// lattice too. It did not: grid_ stayed where the link set it up, and after a long run of
	// long packets between two of our stations (the follow at its step limit, 82 ms in 16 KB on the
	// paced FIFO link) our P to the break-in went that far off; the callee never read it and
	// repeated its break-in until both timed out. P2_MASTER_GRID_OLD=1: the old lattice.
	static const bool grid_old = std::getenv("P2_MASTER_GRID_OLD") != nullptr;
	if (master_ && !grid_old) grid_ += int64_t(moved);
	if (debug_) log("long cycle: cs at +" + std::to_string(off / cfg_.fs * 1000.0) + " ms (ref " + std::to_string(was / cfg_.fs * 1000.0) + " ms), next start moved " +
	                std::to_string(moved / cfg_.fs * 1000.0) + " ms, " + std::to_string(long_moved_ / cfg_.fs * 1000.0) + " ms in all" + note);
}

double P2Station::callee_test_shift()
{
	cj_reads_++;
	const char *e = std::getenv("CALLEE_TEST_CS");   // "K:MS[,K:MS]": the K-th callee reading (from 1) MS ms off
	if (!e) return 0;
	for (const char *p = e; *p; ) {
		int k = 0; double ms = 0;
		if (std::sscanf(p, "%d:%lf", &k, &ms) == 2 && k == cj_reads_) { log("test: callee reading " + std::to_string(k) + " moved " + std::to_string(ms) + " ms"); return ms * cfg_.fs / 1000.0; }
		const char *q = std::strchr(p, ',');
		if (!q) break;
		p = q + 1;
	}
	return 0;
}

double P2Station::callee_jump(CalleeJump &J, double off, bool clear, const char *what)
{
	if (!clear) return 0;
	if (J.ref < 0) { J.ref = off; return 0; }
	const double d = off - J.ref, ms = cfg_.fs / 1000.0;
	auto say = [&](const std::string &t) { if (debug_) log(std::string("callee ") + what + ": cs " + std::to_string(d / ms) + " ms off the reference" + t); };
	if (std::fabs(d) < double(samples(0.0015))) { J.cand = false; J.alt_n = 0; return 0; }
	if (rt_agrees(d) && std::fabs(d) <= double(samples(kCalleeJumpMaxS))) {
		J.alt_n = 0;
		if (!J.cand) { J.cand = true; say(", as the round trip reported (" + std::to_string(rt_pend_ / ms) + " ms): waiting for the next reading"); return 0; }
		J.cand = false;
		rt_pend_ -= d;
		log(std::string("callee ") + what + ": our clock moved " + std::to_string(d / ms) + " ms (round trip reported, seen twice): followed");
		return d;
	}
	J.cand = false;
	if (J.alt_n > 0 && std::fabs(off - J.alt) <= ms) J.alt_n++; else { J.alt = off; J.alt_n = 1; }
	if (J.alt_n >= 3) { say(": seen 3 times, no round-trip report: reference re-taken, not followed"); J.ref = J.alt; J.alt_n = 0; return 0; }
	say(": no round-trip report agrees, not followed");
	return 0;
}

void P2Station::callee_follow(int64_t ref, bool have, double off, bool good, bool onset, bool jumped)
{
	if (master_ || callee_follow_off()) return;
	if (ref != sf_next_) { sf_ref_ = -1; sf_rate_ = 0; sf_moved_ = 0; }   // a new run of our packets
	if (jumped) { sf_next_ = next_start_; return; }
	const double lim = double(samples(kCalleeStepS)), d = off - sf_ref_;
	const double kp = onset ? kLongGainOnset : 0.4, ki = onset ? kLongRateGainOnset : 0.08;
	double m = 0; const char *note = "";
	if (!have || !good) { m = sf_rate_; note = have ? " (weak reading: drift estimate only)" : " (nothing read: drift estimate only)"; }
	else if (sf_ref_ < 0) { sf_ref_ = off; note = " (reference)"; }
	else if (std::fabs(d) <= double(samples(0.002))) {
		sf_rate_ = std::max(-lim, std::min(lim, sf_rate_ + ki * d));
		m = sf_rate_ + kp * d;
	} else { m = sf_rate_; note = " (off the reference: drift estimate only)"; }
	const int64_t mv = int64_t(std::llround(std::max(-lim, std::min(lim, m))));
	next_start_ += mv; sf_moved_ += double(mv);
	sf_next_ = next_start_;
	if (debug_ && (mv != 0 || *note))
		log("callee follow: cs at +" + (have ? std::to_string(off / cfg_.fs * 1000.0) : std::string("-")) + " ms (ref " + std::to_string(sf_ref_ / cfg_.fs * 1000.0) +
		    " ms), next start moved " + std::to_string(double(mv) / cfg_.fs * 1000.0) + " ms, " + std::to_string(sf_moved_ / cfg_.fs * 1000.0) + " ms in all" + note);
}

int P2Station::test_csfault(int cs)
{
	static const char *e = std::getenv("P2_TEST_CSFAULT");
	if (!e || tf_idx_ < 0) return cs;
	const int k = tf_idx_++;
	for (const char *p = e; *p; ) {
		int n = 0, kk = 0; char l = 0;
		if (std::sscanf(p, "U%d+%d=%c", &n, &kk, &l) == 3 && n == tf_ups_ && kk == k) {
			const char *q = std::strchr(p2arq::kCsName, l);
			const int f = (l == '-' || !q) ? int(p2arq::CS_NONE) : int(q - p2arq::kCsName);
			log(std::string("test: control signal ") + (cs >= 0 ? std::string(1, p2arq::kCsName[cs]) : "-") + " replaced by " + l);
			return f;
		}
		const char *c = std::strchr(p, ',');
		if (!c) break;
		p = c + 1;
	}
	return cs;
}

auto P2Station::bk_decode(int64_t start, bool retry, const Snap *sp, const TrkView *tv) -> BkRx
{
	BkRx r;
	r.p = find_packet(start, true, false, sp, bk_cc_ok_ && bk_cc_start_ == start ? bk_cc_ : nullptr);
	// Our first Q went out (break-in not decoded): the real reference unit then
	// sends its break-in data again as an ordinary packet, not as a
	// break-in (RF bench 2026-10-15; it also steps its level down while
	// we keep asking). Looking only for the break-in pattern again lost
	// the link every time.
	if (!r.p.ok && retry) { r.p = find_packet(start, false, false, sp, nullptr, tv); r.again = r.p.ok; r.retried = true; }
	return r;
}

void P2Station::on_bk_done(int64_t start, int arg)
{
	if (state_ != ST_ISS) return;
	const bool retry = arg & 1;
	BkRx b;
	bool late_used = false;
	if (cfg_.realtime && retry && bk_late_.valid() && bk_late_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
		BkRx l = bk_late_.get();                       // fix 53: the previous copy's decode, finished late
		if (l.retried) trk_seq_of(bk_late_slot_, true);   // fix 200: its packet look counted here, on the audio loop
		if (l.p.ok) { b = l; b.p.start += double(start - bk_late_slot_); late_used = true; log("P2: the break-in decoded late: used for its repeat"); }
	}
	if (late_used) {
	} else if (cfg_.realtime) {
		// Up to ~60 ms of decoding: on a worker, never on the audio loop
		// (see on_packet); looked at every 2 ms until our answer is due.
		if (bk_fut_slot_ != start || !bk_fut_.valid()) {
			bk_fut_slot_ = start;
			bk_fut_ = pool_.submit([this, start, retry, sp = snapshot_short(start), tv = trk_view(start, false)]() { return bk_decode(start, retry, &sp, &tv); });
		}
		if (bk_fut_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
			const int64_t step = samples(0.002);
			if (n_ + step < cs_at(start, false) - samples(0.003)) { schedule(n_ + step, EV_BK_DONE, start, arg | 2); return; }
			bk_late_ = std::move(bk_fut_); bk_late_slot_ = start;   // kept for the repeat (fix 53)
			if (cfg_.paced) { dstats_.late_bk++; load_.late_bk++; }
			if (debug_) log("P2: break-in decode not ready in time, counted as failed");
		} else { b = bk_fut_.get(); if (b.retried) trk_seq_of(start, true); }
	} else b = bk_decode(start, retry, nullptr);
	RxPacket &p = b.p;
	const bool again = b.again;
	if (debug_) log("P2 bk decode " + std::string(retry ? "repeat" : "first") + ": ok " + std::to_string(int(p.ok)) + " again " + std::to_string(int(again)) + " level " + std::to_string(p.level) + " path " + std::to_string(p.path) + " hdr_rms " + std::to_string(p.hdr_rms) + " llr_rms " + std::to_string(p.llr_rms) + " llr_level " + std::to_string(p.llr_level) + " start " + std::to_string((p.start - double(start)) / cfg_.fs * 1000.0) + " ms");
	if (p.ok) {
		txq_.erase(0, size_t(pending_used_)); pending_used_ = 0;
		const int n = again ? p2::data_bytes(p.level, 9) : p2::data_bytes(p.level, 7);
		deliver(p.info, n, p.info[size_t(n)]);
		if ((p.info[size_t(n)] & 0x80) && !std::getenv("P2_BK_NO_BK")) qrt_rx_ = true;   // fix 90: a break-in that asks QRT: its end packet follows
		level_ = p.level;
		rx_level_ = p.level;                        // fix 54: else a stale level 2 asked T of a sender at level 1
		rx_t_state_ = false;
		// fix 60: the new sender starts with short packets (long only after
		// our R). A stale rx_long_ from packets we received before our
		// sending turn made on_packet wait 2.5 s for a long one, and every
		// reply was due in the past (KB5LZK, 30 m P2, 2026-10-16 15:11)
		rx_long_ = false;
		if (again) afc_update(p.bias, afc_data_mod());
		log(again ? "break-in data received again, now receiving" : "break-in received, now receiving");
		peer_broke_in(start);
		if (qrt_peer_took_link()) return;
		state_ = ST_IRS;
		ack_phase_ = 1;                             // our ack below is P; the next new packet gets X
		last_rx_counter_ = again ? (p.info[size_t(n)] & 3) : 0;
		misses_ = 0;
		good_run_ = 0;
		p2_quick_s_ = true;                         // fix 217
		// Test: P2_TEST_MUTE_BK_ACK=N: our P to the first N break-ins received is not sent
		// (how the firmware repeats a break-in whose acknowledgement it missed).
		static int mute_bk_ack = std::getenv("P2_TEST_MUTE_BK_ACK") ? std::atoi(std::getenv("P2_TEST_MUTE_BK_ACK")) : 0;
		if (mute_bk_ack > 0) { mute_bk_ack--; log("test: our P to the break-in not sent"); }
		else send_cs(p2arq::CS_P, cs_at(int64_t(p.start), false));
		ack_phase_ = 0;
		last_ack_cs_ = p2arq::CS_P;
		rx_fails_ = 0;
		const int64_t next = int64_t(p.start) + samples(cyc_s());
		schedule(next + samples(0.86), EV_PACKET, next);
	} else {
		if (++misses_ > cfg_.timeout_cycles) { disconnect("timeout"); return; }
		// Fix 260: Q to the first four failed reads of a break-in, then T until one decodes, as the
		// reference unit does in our place (emulated pair, noise on the break-in's receiver, caller
		// and callee alike: Q Q Q Q T T ... P). The breaking-in side repeats its break-in at its
		// current level and steps down only on T: Q for ever left an L4 break-in that could not
		// decode at that SNR repeated until the timeout (on the air against the real unit, and on
		// the emulator from noise RMS 2500). P2_BK_RX_T_OLD=1: Q for ever.
		const int n = retry ? ++bk_rx_fails_ : (bk_rx_fails_ = 1);
		static const bool t_old = std::getenv("P2_BK_RX_T_OLD") != nullptr;
		const bool t = !p3_ && !t_old && n > kBkRxQ;
		if (t && n == kBkRxQ + 1) log("break-in not decoded " + std::to_string(kBkRxQ) + " times: answering T (asking for a lower level)");
		send_cs(t ? p2arq::CS_T : p2arq::CS_Q, cs_at(start, false));   // ask for it again (T: at a lower level)
		schedule(start + samples(cyc_s()) + samples(0.86), EV_BK_DONE, start + samples(cyc_s()), 1);
	}
}

void P2Station::handle(const Event &e)
{
	switch (e.type) {
	case EV_UNPROTO: on_unproto(e.ref); break;
	case EV_P1_CS: on_p1_cs(e.ref, e.arg); break;
	case EV_P1_PKT: on_p1_packet(e.ref); break;
	case EV_P1_AGAIN: on_p1_again(e.ref); break;
	case EV_REPLY: on_reply(e.ref, e.arg); break;
	case EV_BK_DONE: on_bk_done(e.ref, e.arg); break;
	case EV_PACKET: on_packet(e.ref, (e.arg & 1) != 0, (e.arg & 2) != 0); break;
	case EV_P2_SYNC: launch_p2_sync(e.ref); break;
	case EV_P2_LAUNCH:
		if (state_ != ST_IRS || p3_) break;
		if (e.arg == 1) { if (p2_fut_slot_ != e.ref || !p2_fut_.valid() || !p2_fut_long_) p2_launch(e.ref, true); }   // a long packet (fix 73)
		// short: also while a long packet is possible (cheap; replaced if the look
		// finds a long one). Skipping it then kept every short decode late after
		// the sender left long packets, so rx_long_ never cleared (loopback run).
		else if (p2_fut_slot_ != e.ref || !p2_fut_.valid()) p2_launch(e.ref, false);
		break;
	case EV_P3: on_p3(e.ref, e.arg); break;
	case EV_P3_LAUNCH: if (p3_) launch_p3(e.ref, e.arg / 1000.0); break;
	case EV_P3_SL1: if (state_ == ST_IRS) launch_sl1(e.ref); break;
	case EV_P3_SYNC: if (p3_) launch_p3_sync(e.ref, e.arg / 1000.0); break;
	case EV_BK_DATA: if (p3_) p3_bk_decode(e.ref, e.arg); break;
	case EV_BK_AGAIN: on_bk_again(e.ref); break;
	case EV_P3L_LAUNCH: if (p3_) launch_p3_long(e.ref); break;
	case EV_P3L_SYNC: if (p3_) launch_p3_long_sync(e.ref); break;
	case EV_P3L_PRE: if (p3_) advance_p3_long(e.ref); break;
	case EV_P2L_PRE: if (!p3_) advance_p2_long(e.ref); break;
	}
}

void P2Station::first_psk_parity(int64_t at)
{
	if (acc_first_ < 0 || std::getenv("P3_B1_NO_PARITY") || (!p3_ && std::getenv("P2_SWAP_START"))) return;
	const long k = std::lround(double(at - acc_first_) / double(samples(cyc_s())));
	const bool sw = k % 2 == 0;
	if (sw != psk_swap_ && debug_) log("first PSK burst " + std::to_string(k) + " cycles after our accept: carrier order " + (sw ? "high" : "low") + " first");
	psk_swap_ = sw;
	acc_first_ = -1;
}

void P2Station::p1_new(int64_t from)
{
	P1Config c;
	c.fs = cfg_.fs; c.tone_lo = cfg_.tone_lo; c.tone_hi = cfg_.tone_hi; c.txd_s = cfg_.txd_s; c.amplitude = cfg_.amplitude * fsk_scale();
	c.mycall = cfg_.mycall; c.timeout_cycles = cfg_.timeout_cycles; c.inline_status = false; c.rt_latency_s = cfg_.rt_latency_s; c.rx_mute_delay = cfg_.rx_mute_delay;
	c.pduplex = cfg_.pduplex; c.pdtimer_s = float(cfg_.pdtimer_s);
	// Fix 184: the offset the set-up's FSK measured (fsk_afc_add) moves the P1 link's receive
	// tones: the P1 station listened on the nominal tones and from 50 Hz decoded nothing (the
	// reference unit calls us in P1 at 50/60/80 Hz: 0 of 1,500 characters; 40 Hz passed).
	// From 10 Hz; P1_NO_RX_AFC: off.
	// And what we send keeps the shift our P2 station's bursts had (fix 145, from 40 Hz): the P1
	// station sent on the nominal tones, a jump of the whole offset after our CS1 / set-up packet.
	if (std::fabs(afc_hz()) >= 10.0 && !std::getenv("P1_NO_RX_AFC")) { c.rx_shift_hz = float(afc_hz()); log("P1: receive tones moved " + std::to_string(afc_hz()) + " Hz"); }
	if (tx_df_ != 0 && !std::getenv("P1_NO_TX_AFC")) { c.tx_shift_hz = float(tx_df_); log("P1: transmit moved " + std::to_string(tx_df_) + " Hz"); }
	p1_ = std::make_unique<P1Station>(c);
	p1_->set_debug(debug_);
	from = std::max<int64_t>(0, std::max(from, n_ - kRing + samples(0.5)));
	p1_->begin_at(from);
	for (int64_t t = from; t < n_; t++) p1_->tick(int16_t(std::max(-32767.0f, std::min(32767.0f, raw_[size_t(t % kRing)]))));
}

std::string P2Station::p1_caller_guess(const uint8_t *bytes) const
{
	const std::string body = text::decode((bytes[21] >> 2) & 3, bytes + 1, p1_slow_ ? 8 : 20);   // fix 180: 100 Bd set-up
	std::string who = body.substr(0, body.find('\r'));
	for (auto &ch : who) ch = char(std::toupper(uint8_t(ch)));
	if (!who.empty() && std::isdigit(uint8_t(who[0]))) who.erase(0, 1);
	if (!who.empty()) const_cast<P2Station *>(this)->remote_ = who;
	return remote_;
}

int16_t P2Station::p1_tick(int16_t rx)
{
	const int16_t y = p1_->tick(rx);
	ptt_ = p1_->ptt();
	rx_ += p1_->take_rx();
	if (debug_) log_ += p1_->take_log(); else p1_->take_log();
	if (!p1_up_seen_ && p1_->connected()) {        // our MYLevel 1 call was answered
		p1_up_seen_ = true; p1_link_ = true;
		remote_ = p1_->remote_call();
		link_status(true, "");
		log("P1 link set up with " + remote_);
	}
	if (!p1_->connected() && !p1_->calling()) {
		// ended: reported at once, but the P1 station plays out what it queued
		// first (the acknowledgement of the peer's QRT packet; destroyed with
		// it, the peer never heard it)
		if (!p1_closing_) { p1_closing_ = true; p1_report_end(p1_->end_reason().empty() ? "no answer" : p1_->end_reason()); }
		if (!p1_->tx_pending()) p1_end("");
	}
	return y;
}

void P2Station::link_status(bool up, const std::string &note)
{
	if (cfg_.inline_status) rx_ += (up ? "*** CONNECTED TO " + remote_ : std::string("*** DISCONNECTED")) + note + "\r";
	link_events_.push_back({up, remote_});
	link_note_ = up ? std::string() : note;
	if (up && state_ != ST_P1) p1_link_ = false;   // a P2/P3 link (link_plevel)
	if (up && !link_up_) p3_long_link_start();      // fix 241
	if (up && !link_up_) { pstats_ = PanelStats{}; pstats_.t_up = n_; dstats_ = DecodeStats{}; p3_cap_ = 6; cap_hold_ = 0; caps_ = 0; last_cap_at_ = -1; ev_at_[0] = ev_at_[1] = -1; late_sl1_logged_ = false; hunt_s_ = 0; hunt_base_ = 0; hunt_i_ = 0; deaf_ = 0; confirm_left_ = 0; probation_ = 0; hunt_k_ = 0; hunt_trial_ = false; hunt_backoff_ = 1; hunt_quiet_ = 0; fail_hist_ = 0; rep_hist_ = 0; hunt_noisy_ = false; ack_new_cs_ = -1; hunt_new_ = 0; hunt_fail_cs_ = -1; hunt_fail_seen_ = false; up_miss_run_ = 0; up_miss_steps_ = 0; early_seen_ = false; }   // fix 171: each link at the nominal answer time
	if (up && !link_up_) { p3l_hold_ = 0; p3l_holds_ = 0; }   // fix 236: the long-decode budget, per link
	link_up_ = up;
}

std::vector<float> P2Station::up_p1_burst()
{
	const int64_t lead = samples(1.0125 - 0.96), spb = samples(1.0 / up_baud_);
	std::vector<float> y(size_t(lead + int64_t(up_p1bits_.size()) * spb));
	for (size_t i = 0; i < y.size(); i++) {
		int bit = 1;
		if (int64_t(i) >= lead) bit = up_p1bits_[size_t((int64_t(i) - lead) / spb)];
		if (up_inv_) bit ^= 1;
		up_phase_ += 2.0 * M_PI * (bit ? cfg_.tone_hi : cfg_.tone_lo) / cfg_.fs;
		if (up_phase_ > 2 * M_PI) up_phase_ -= 2 * M_PI;
		y[i] = float(cfg_.amplitude * std::sin(up_phase_));
	}
	return y;
}

void P2Station::on_unproto(int64_t ref)
{
	if (state_ != ST_UNPROTO) return;
	if (up_p3_) { on_unproto_p3(ref); return; }
	if (up_rep_ == 0) {                              // a new packet
		if (want_qrt_ && txq_.empty() && !up_first_) { unproto_end("QRT"); return; }
		if (n_ - up_idle_since_ > samples(240)) { unproto_end("4 minutes idle: QRT"); return; }
		const size_t nd = up_p1_ ? size_t(up_baud_ == 100 ? 8 : 20) : up_long_ ? size_t(p2::data_bytes(up_level_, 40)) : size_t(p2::kDataBytes[up_level_]);
		std::vector<uint8_t> data(nd, 0x1e), info(nd + 3);
		uint8_t status = uint8_t(up_counter_ & 3);
		if (!up_first_ && !txq_.empty() && !up_p1_) {
			// Fix 239: P2 unproto packs its text as ARQ does (ASCII / Huffman / PMC, the
			// type in status bits 2-4): the emulated reference unit's U 3 puts 5-8 characters in each
			// 5-byte level-1 packet ("foxtrot ", "de de Tn", "x, juli"), ASCII alone 5; that cost
			// p2-fec3-we-send 90 characters against the reference unit pair's 128.
			const int type = cfg_.compress ? text::choose(txq_, int(nd), cfg_.allow_pmc, true, true) : 0;
			const int used = text::encode(type, txq_, data.data(), int(nd), true);
			txq_.erase(0, size_t(used));
			if (txq_.empty()) txq_ += rle_tx_.flush();     // a held run closed once the queue drains (as fill_pending)
			status |= uint8_t(0x20 | type << 2);
			up_idle_since_ = n_;
		} else if (!up_first_ && !txq_.empty()) {
			// P1: Huffman (type 1, status bits 2-3) when it carries more text than ASCII,
			// as P1Station::fill_data does in ARQ (a byte >= 0x80 ends the Huffman run). Fix 239:
			// ASCII alone, our U 1 gave the emulated reference unit 128 characters in p1-fec100-snr-3-we-send
			// where its own U 1 gives 174 (8 bytes a packet, about 11 Huffman characters).
			size_t hz = 0;
			while (hz < txq_.size() && hz < 4 * nd && uint8_t(txq_[hz]) < 0x80) hz++;
			std::vector<uint8_t> hd(nd);
			const int used_h = cfg_.compress && hz ? text::encode(1, txq_.substr(0, hz), hd.data(), int(nd), false) : 0;
			const size_t k = std::min(nd, txq_.size());
			if (size_t(used_h) > k) {
				data = hd; txq_.erase(0, size_t(used_h)); status |= 1 << 2;
			} else {
				for (size_t i = 0; i < k; i++) data[i] = uint8_t(txq_[i]);
				txq_.erase(0, k);
			}
			if (txq_.empty()) txq_ += rle_tx_.flush();
			status |= 0x20;
			up_idle_since_ = n_;
		}
		up_first_ = false;
		if (up_p1_) {                                  // header, data (ASCII), status = counter, CRC over data + status
			up_hdr_ = up_hdr_ == 0xAA ? 0x55 : 0xAA;
			uint8_t pkt[24];
			const int body = int(nd) + 1;
			pkt[0] = up_hdr_;
			for (size_t i = 0; i < nd; i++) pkt[1 + i] = data[i];
			pkt[body] = uint8_t(status & 0x0f);              // counter + data type (no "more" bit in P1)
			const uint16_t crc = crc16_ccitt_reflected(pkt + 1, size_t(body));
			pkt[body + 1] = uint8_t(crc); pkt[body + 2] = uint8_t(crc >> 8);
			up_p1bits_.clear();
			for (int i = 0; i < (body + 3) * 8; i++) up_p1bits_.push_back(uint8_t((pkt[i / 8] >> (i % 8)) & 1));   // LSB first
		} else if (up_long_) {
			info.assign(nd + 3, 0);
			p2::make_info(up_level_, data.data(), status, info.data(), 40);
			up_words_.assign(1, 0x6a);
			const auto &h = p2arq::kLongHeader[up_level_][up_counter_ & 1];
			up_words_.insert(up_words_.end(), h, h + 8);
			const auto d = p2::encode_data(up_level_, info.data(), 40);
			up_words_.insert(up_words_.end(), d.begin(), d.end());
			up_words_.push_back(p2arq::kLongTail[up_level_]);
		} else {
			p2::make_info(up_level_, data.data(), status, info.data());
			up_words_ = p2::encode(up_level_, info.data(), up_counter_);
		}
		up_counter_++;
	}
	const double cycle = up_p1_ ? 1.0125 : up_long_ ? 3.3875 : 0.8875;
	auto burst = up_p1_ ? up_p1_burst() : p2::modulate(up_words_, float(cfg_.fs), cfg_.tone_lo, cfg_.tone_hi, (up_slot_ % 2) == 1, p2_amp());   // P2: with its 20 ms lead, as p2_unproto
	if (up_p1_) up_inv_ = !up_inv_;                  // the next copy inverted
	TxBurst tb;
	tb.start = ref;
	tb.audio = std::move(burst);
	tx_.push_back(std::move(tb));
	up_slot_++;
	up_rep_ = (up_rep_ + 1) % up_reps_;
	schedule(ref + samples(cycle), EV_UNPROTO, ref + samples(cycle));
}

void P2Station::on_unproto_p3(int64_t ref)
{
	if (up_rep_ == 0) {
		// QRT (the firmware's U 33, ESC D: the text out, then one packet more with status bit 7,
		// sent `reps` times, then the end): the end packet, then the end
		if (up_qrt_sent_) { unproto_end("QRT"); return; }
		const bool qrt_now = want_qrt_ && txq_.empty() && !up_first_ && !p3u_qrt_old();
		if (want_qrt_ && txq_.empty() && !up_first_ && p3u_qrt_old()) { unproto_end("QRT"); return; }
		if (n_ - up_idle_since_ > samples(240)) { unproto_end("4 minutes idle: QRT"); return; }
		const int lv = up_level_;
		const int cap = p3u::payload(lv, up_long_);
		std::vector<uint8_t> data(size_t(cap), 0x1e);
		uint8_t status = uint8_t(up_counter_ & 3);
		if (!up_first_ && !txq_.empty()) {
			const int type = cfg_.compress ? text::choose(txq_, cap, cfg_.allow_pmc, false) : 0;
			const int used = text::encode(type, txq_, data.data(), cap, false);
			txq_.erase(0, size_t(used));
			if (txq_.empty()) txq_ += rle_tx_.flush();
			status |= uint8_t(type << 2 | (txq_.empty() ? 0 : 0x20));
			up_idle_since_ = n_;
		}
		up_first_ = false;
		if (qrt_now) { status |= 0x80; up_qrt_sent_ = true; }
		std::vector<uint8_t> w = p3u::words(lv, up_long_, data.data(), status);
		up_words_ = std::move(w);
		up_counter_++;
	}
	const bool odd = (up_slot_ % 2) == 1;
	const int lv = up_level_;
	const bool lng = up_long_;
	const float fs = cfg_.fs;
	// the slot (word 0 at slot + 0.8 T) 0.25 s after the event: in real time the burst is made on a worker
	// (a long packet: tens of ms on a small computer) and must be in before its start (the first burst
	// then goes 0.35 s after U; the reference unit's 0.30 s)
	const int64_t slot = ref + samples(0.25);
	int64_t start = slot;
	double lead = 0;
	float amp = 0;
	if (lv == 1) { start = slot - int64_t(std::lround(p2::preroll(cfg_.fs))); amp = p3_psk_amp(); }
	else { p3_place(p3_pulse0(slot, lv, lng), lv == 5 && !lng ? 0.02 : 0.03, start, lead); amp = (lv == 5 && !lng ? kP3DataGain : lv_gain(lv)) * p3_data_amp(); }
	auto make = [w = up_words_, lv, lng, fs, odd, amp, lead]() {
		if (lv == 1) return p3::modulate_cs(w, fs, odd, amp);
		return lv == 5 && !lng ? mod_p3(w, fs, odd, amp, lead) : mod_lv(lv, w, fs, odd, amp, lead, lng);
	};
	TxBurst tb;
	tb.start = start;
	// in real time made on a worker (a long packet's 3.4 s of audio: as queue_lv)
	if (cfg_.realtime) tb.fut = pool_.submit(make, true).share();
	else tb.audio = make();
	tx_push(std::move(tb));
	up_slot_++;
	up_rep_ = (up_rep_ + 1) % up_reps_;
	const double cycle = up_long_ ? 3.3875 : 0.8875;
	schedule(ref + samples(cycle), EV_UNPROTO, ref + samples(cycle));
}

void P2Station::disconnect(const std::string &why)
{
	log("disconnected: " + why);
	if (premod_hits_ + premod_misses_ > 0)
		log("TX audio made ahead for " + std::to_string(premod_hits_) + " of " + std::to_string(premod_hits_ + premod_misses_) + " data packets");
	premod_hits_ = premod_misses_ = 0;
	link_status(false, " (" + why + ")");
	disconnect_quiet();
}

void P2Station::disconnect_quiet()
{
	state_ = answer_ ? ST_LISTEN : ST_IDLE;
	events_.clear();
	tx_df_ = 0; tx_est_ = 0; tx_est_n_ = 0; tx_est_out_ = 0; tx_step_at_ = -(int64_t(1) << 40);   // the next link (or unproto) measures afresh
	txq_.clear(); want_qrt_ = false; pending_qrt_ = false; pending_used_ = 0; qrt_hold_ = 0; qrt_takes_ = 0; qrt_tx_since_take_ = false;
	bk_rep_watch_ = false; bk_rep_dedup_ = false; bk_last_info_.clear();   // fix 270
	last_rx_counter_ = -1; sent_text_ = false; must_break_in_ = false; bk_req_ = cho_req_ = false; bk_hold_until_ = 0; p3_long_ = false; rx_long_ = false; p2_quick_s_ = false; rx_last_fail_ = -(int64_t(1) << 40); rx_full_ = false; p3_last_full_ = false; p3_full_ok_ = true; last_pkt_full_ = false; p3_clean_run_ = 0; marq_clear(); led_marq_ = 0; p2_late_ = {}; late_slot_ = -1; p2_decided_ = -1; bk_late_ = {}; trk_clear(); cst_clear(); p3_backoff_ = 1;
	last_rx_cs_ = -1; last_tx_cs_ = -1; p3_a2_rx_t_ = false;
	p1_late_ref_ = p1_late_dec_ = p3_b1_prev_ = -1; p1_late_which_ = -1; fsk_late_logs_ = 0; psk_late_logs_ = 0; late_cs_logs_ = 0;
	qrt_rx_ = false; qrt_end_sent_ = false; p2_setup_watch_ = false;
	p3_ = false; p3_first_tx_ = true; p3_level_ = 5;
	bk_tx_.clear(); p3_greet_off_ = 0; greet_long_ = false; rle_ = text::RleExpander(); si_rx_.clear(); rle_tx_ = text::RleCompressor(); hdr_flip_ = 0; acc_first_ = -1; p1_slow_ = false; p1_hdr_ = 0xAA; p1_sd_ = false; p3_rx_long_ = false; p3_r_pending_ = false; p3_long_good_ = 0; p3l_slot_ = -1; p3_last_lv_ = -1; p3_last_data_.clear(); mq3_.clear(); mql_.clear();
	new_send_turn(); bk_ans_at_ = -1; a2_ans_at_ = -1; p3_rx_df_ = 0; p3_df_set_ = false; fs_prev_ = {}; fs_prev2_ = {}; fs_reseeds_ = 0; fs_anchor_ok_ = false; fs_fails_ = 0; fs_wait_ = kFsMinFails; fs_trial_ = false; led_ir_ = led_te_ = led_tx_type_ = 0; led_good_role_ = -1; led_trk_n_ = -1; led_rx_data_ = led_rx_idle_ = false;
	link_up_ = false; watch_since_ = -1; watch_p3_next_ = -1;
}

float P2Station::prog_sample(TxBurst &b, size_t i)
{
	TxProg &p = *b.prog;
	if (b.df == 0) {
		if (i >= p.ready()) prog_wait(b, i + 1);
		return p.y[i];
	}
	if (long(i) >= b.sdone) {
		const long n = long(p.y.size());
		const long e = std::min(n, std::max(b.sdone + kShiftChunk, long(i) + 1));
		const long need = std::min(n, e + kSsbReach);
		if (long(p.ready()) < need) prog_wait(b, size_t(need));
		ssb_shift_range(p.y.data(), n, b.sy.data(), b.sdone, e, b.df, b.start, cfg_.fs);
		b.sdone = e;
	}
	return b.sy[i];
}

void P2Station::prog_wait(TxBurst &b, size_t n)
{
	// the audio loop waits: a playback underrun is likely
	const auto w0 = std::chrono::steady_clock::now();
	const size_t had = b.prog->ready();
	b.prog->wait(n);
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count();
	tx_late_++;
	load_.tx_late++; load_.last_tx_wait_ms = ms;
	const int64_t at = n_ - b.start;
	log(at <= 0 ? "TX audio not ready at its start: waited " + std::to_string(ms) + " ms"
	            : "TX audio not ready " + std::to_string(1e3 * double(at) / cfg_.fs) + " ms into its burst: waited " + std::to_string(ms) + " ms");
	if (tx_prof() && b.tr) {
		const double t = now_ms();
		char m[200]; std::snprintf(m, sizeof m, "TX prof: pieces %s, queued %.1f ms of audio before its start, submitted %.1f ms ago, began %.1f ms ago, %zu samples made, %zu needed",
		    b.tr->hit ? "made ahead" : "made now", 1e3 * double(b.start - b.tr->queued_n) / cfg_.fs, t - b.tr->sub, b.tr->beg > 0 ? t - b.tr->beg : -1.0, had, n);
		log(m);
	}
}

int16_t P2Station::play()
{
	for (auto &b : tx_)
		if (b.fut.valid() && n_ >= b.start) {   // normally long done
			if (b.fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
				// the audio loop waits: a playback underrun is likely
				const auto w0 = std::chrono::steady_clock::now();
				b.fut.wait();
				tx_late_++;
				load_.tx_late++; load_.last_tx_wait_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count();
				log("TX audio not ready at its start: waited " + std::to_string(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count()) + " ms");
				if (tx_prof() && b.tr) {
					const double t = now_ms();
					char m[200]; std::snprintf(m, sizeof m, "TX prof: %s, queued %.1f ms of audio before its start, submitted %.1f ms ago, began %.1f ms ago, took %.1f ms at %d MHz",
					    b.tr->hit ? "made ahead" : "made now", 1e3 * double(b.start - b.tr->queued_n) / cfg_.fs, t - b.tr->sub, t - b.tr->beg, b.tr->end - b.tr->beg, b.tr->khz / 1000);
					log(m);
				}
			}
			else if (tx_prof() && b.tr) {
				const double t = now_ms();
				char m[200]; std::snprintf(m, sizeof m, "TX prof ok: %s, queued %.1f ms of audio before its start, submitted %.1f ms ago, began %.1f ms ago, took %.1f ms at %d MHz, ready %.1f ms ago",
				    b.tr->hit ? "made ahead" : "made now", 1e3 * double(b.start - b.tr->queued_n) / cfg_.fs, t - b.tr->sub, t - b.tr->beg, b.tr->end - b.tr->beg, b.tr->khz / 1000, t - b.tr->end);
				log(m);
			}
			b.audio = b.fut.get(); b.fut = {};
		}
	for (auto &b : tx_)                              // fix 145: the peer's offset, once per burst
		if (!b.shifted && !b.fut.valid() && n_ >= b.start) {
			// fix 223: the shift is chosen as a burst starts and kept to its end
			tx_shift_choose();
			if (b.prog) {                            // made in pieces: shifted as its pieces are played
				if (std::fabs(tx_df_) >= 0.5) { b.df = tx_df_; b.sy.assign(b.prog->y.size(), 0.0f); }
				b.shifted = true;
				if (tx_prof() && b.tr) {
					const double t = now_ms();
					char m[220]; std::snprintf(m, sizeof m, "TX prof pieces: %s, queued %.1f ms of audio before its start, submitted %.1f ms ago, began %.1f ms ago, %zu of %zu samples made at its start (%d MHz)%s",
					    b.tr->hit ? "made ahead" : "made now", 1e3 * double(b.start - b.tr->queued_n) / cfg_.fs, t - b.tr->sub, b.tr->beg > 0 ? t - b.tr->beg : -1.0,
					    b.prog->ready(), b.prog->y.size(), b.tr->khz / 1000, b.df != 0 ? ", shifted" : "");
					log(m);
				}
				continue;
			}
			if (std::fabs(tx_df_) >= 0.5) ssb_shift(b.audio, tx_df_, b.start, cfg_.fs);
			// Test hook: TX_TEST_STEP="S:HZ": every burst from S s of audio on moved HZ (a radio's
			// frequency step: a VFO jump or a retune during a link).
			static const char *step = std::getenv("TX_TEST_STEP");
			if (step) { const double at = std::atof(step); const char *c = std::strchr(step, ':'); const double hz = c ? std::atof(c + 1) : 0.0;
			            if (double(b.start) >= at * cfg_.fs && hz != 0.0) {
			                ssb_shift(b.audio, hz, b.start, cfg_.fs);
			                if (!tx_step_logged_) { tx_step_logged_ = true; log("TX step now: every burst moved " + std::to_string(hz) + " Hz (test hook TX_TEST_STEP)"); }
			            } }
			b.shifted = true;
		}
	while (!tx_.empty() && !tx_.front().fut.valid() && tx_.front().start + int64_t(tx_.front().len()) < n_) tx_.erase(tx_.begin());
	float y = 0;
	bool on = false;
	for (auto &b : tx_)
		if (n_ >= b.start && n_ < b.start + int64_t(b.len())) { y += b.prog ? prog_sample(b, size_t(n_ - b.start)) : b.audio[size_t(n_ - b.start)]; on = true; }
	// Unproto keeps the transmitter keyed between its bursts, as the reference unit does: the long
	// cycle's 82.5 ms gaps outlasted our unkey delay, and every burst then began with the
	// radio's TX switching (the real reference unit printed none of it; the reference unit has no radio)
	ptt_ = on || state_ == ST_UNPROTO;
	return int16_t(std::max(-32767.0f, std::min(32767.0f, y)));
}

bool P2Station::qrt_peer_took_link()
{
	if (!want_qrt_) return false;
	qrt_hold_ = kQrtHold;
	log("QRT pending: the peer took the link back, waiting for its QRT");
	if (qrt_rx_ || !qrt_tx_since_take_) return false;   // its break-in asks QRT itself (fix 90), or no QRT packet of ours since the last one
	qrt_tx_since_take_ = false;
	const int lim = qrt_takebacks_max();
	if (lim > 0 && ++qrt_takes_ >= lim) { disconnect("QRT taken back " + std::to_string(qrt_takes_) + " times"); return true; }
	return false;
}

void P2Station::note_margin(int level, int64_t deadline, int64_t done, bool p3)
{
	if (!cfg_.paced) return;
	const double ms = (double(wall_ns()) + double(deadline - n_) * 1e9 / cfg_.fs - double(done)) / 1e6;
	if (ms < kThinMs) { load_.thin++; load_.last_thin_ms = ms; }
	dstats_.n++; dstats_.last_ms = ms; dstats_.last_level = level; dstats_.sum_ms += ms;
	if (std::isnan(dstats_.min_ms) || ms < dstats_.min_ms) dstats_.min_ms = ms;
	dstats_push(float(ms));
	if (!p3 || !cfg_.backoff) return;
	if (ms < kThinMs) {
		const std::string why = "decode margin " + std::to_string(int(std::lround(ms))) + " ms at SL" + std::to_string(level);
		if (level >= p3_cap_ || backoff_evidence(0, why)) cap_to(level, why);
	} else if (throttled_) cap_to(level, "the computer is throttling");
	else relax();
}

void P2Station::note_late(int level, bool p3)
{
	if (!cfg_.paced) return;
	dstats_.late++; dstats_.last_ms = -1; dstats_.last_level = level;
	load_.late++; load_.last_late_level = level;
	dstats_push(-1.f);
	if (!(p3 && cfg_.backoff && level > 1)) return;
	if (level - 1 < p3_cap_ && !backoff_evidence(0, "decode late at SL" + std::to_string(level))) return;   // fix 264: one late decode is a hiccup
	// Fix 226: one late decode, one cap step. A late packet is counted as failed, so we send T and
	// the next decodes try more levels (up to 4 with SL1 beside them on 2 workers: 25-43 ms on the
	// Pi 4, ~24 ms allowed) and come late too: f223-7 went SL5 -> 4 -> 3 -> 2 -> 1 in 7.5 s on one
	// 225 ms hiccup and stayed at SL1 for 16 minutes. Lateness within kCapGraceS of the last cap
	// does not cap again; a computer that is too slow all along still steps down, one level per
	// kCapGraceS, and settles where it keeps up. P3_CAP_GRACE_OFF=1: as before.
	static const bool grace_off = std::getenv("P3_CAP_GRACE_OFF") != nullptr;
	if (!grace_off && last_cap_at_ >= 0 && n_ - last_cap_at_ < samples(kCapGraceS)) {
		log("P3: computer back-off: decode late at SL" + std::to_string(level) + " within " + std::to_string(int(kCapGraceS)) + " s of the last cap, not capped again");
		return;
	}
	cap_to(level - 1, "decode late at SL" + std::to_string(level));
}

bool P2Station::backoff_evidence(int kind, const std::string &what)
{
	if (backoff_old()) return true;
	const std::string w = what + (kind ? " (long packet)" : "");
	if (slip_hint_n_ >= 0 && n_ - slip_hint_n_ <= samples(kHiccupSlipS)) {
		load_.hiccups++;
		char b[16]; std::snprintf(b, sizeof b, "%.2f", double(n_ - slip_hint_n_) / cfg_.fs);
		log("P3: late decode treated as a hiccup (" + w + "; the audio loop stalled " + b + " s before: not counted, no back-off)");
		return false;
	}
	const int64_t prev = ev_at_[kind];
	ev_at_[kind] = n_;
	if (prev >= 0 && n_ - prev <= samples(kEvidenceS)) {
		log("P3: second late or thin decode within " + std::to_string(int(kEvidenceS)) + " s (" + w + "): the computer is not keeping up");
		return true;
	}
	load_.hiccups++;
	log("P3: late decode treated as a hiccup (" + w + "; the first in " + std::to_string(int(kEvidenceS)) + " s, no back-off unless a second late or thin decode follows)");
	return false;
}

void P2Station::cap_to(int lv, const std::string &why)
{
	if (lv >= p3_cap_) { cap_hold_ = std::max(cap_hold_, n_ + samples(cap_hold_s())); return; }
	load_.caps++;
	// fix 226: caps within kCapBurstS of the last count as one for the hold doubling (one
	// hiccup cost 120+240+480+960 s on f223-7)
	static const bool burst_off = std::getenv("P3_CAP_GRACE_OFF") != nullptr;
	const bool first = last_cap_at_ < 0;           // this link's first cap: say why it happens (once)
	if (burst_off || last_cap_at_ < 0 || n_ - last_cap_at_ >= samples(kCapBurstS)) caps_++;
	caps_ = std::max(caps_, 1);
	last_cap_at_ = n_;
	cap_hold_ = n_ + samples(cap_hold_s());
	p3_cap_ = std::max(1, lv);
	log("P3: computer back-off: " + why + ", not above SL" + std::to_string(p3_cap_) + " for " + std::to_string(int(cap_hold_s())) + " s at least" +
	    (first ? " (this computer is not keeping up: other load, a low CPU clock or no real-time priority)" : ""));
}

}  // namespace fmodem
