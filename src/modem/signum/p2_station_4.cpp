// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// P2Station member functions, part 4 of 8 (split out of p2_station.hpp so they build in parallel).
#include "p2_station.hpp"

namespace fmodem {

bool P2Station::a1_nomatch_decoded(const RxPacket &q, int64_t ref)
{
	if (!(q.ok && q.level == 1 && !q.bk)) return false;
	const int64_t a1 = int64_t(std::llround(q.start));
	log("P3: A1 decoded without a header match, " + std::to_string(double(a1 - ref) / cfg_.fs * 1000.0) + " ms from the expected slot, status " + std::to_string(q.info[size_t(p3::kSl1Data)]));
	deliver(q.info, p3::kSl1Data, q.info[size_t(p3::kSl1Data)]);
	bk_tx_.clear(); b1_reset();
	int64_t b1 = a1 + samples(kP3CalleeReply - cfg_.rt_latency_s);
	const int64_t dl = b1 - int64_t(std::lround(p2::preroll(cfg_.fs))) - samples(0.003);
	if (!setup_late_old() && n_ > dl) {
		log("P3: A1 decoded " + std::to_string(int(std::lround(1000.0 * double(n_ - dl) / cfg_.fs))) +
		    " ms after our B1 was due: B1 answers the caller's next A1, on time (fix 271)");
		b1 += samples(cyc_s());                     // (its carrier order from the accept's grid: first_psk_parity)
	}
	queue_words(b1, my_bk(true), 82);
	schedule(callee_look_at(my_start_), EV_P3, my_start_, 11);
	return true;
}

void P2Station::a1_none(int64_t ref)
{
	const int64_t exp = ref - samples(0.0135), cyc = samples(cyc_s());
	if (!std::getenv("P3_SETUP_B1")) {
		if (++misses_ > cfg_.timeout_cycles) { disconnect("P3 set-up timeout"); return; }
		log("P3: no A1 and no set-up packet: P3-accept again");
		queue_accept(exp + samples(kP1Reply - cfg_.rt_latency_s + test_call_answer_s()), p3::kP3Accept);
		schedule(ref + cyc + samples(0.86), EV_P3, ref + cyc, 10);
		return;
	}
	const int64_t a1 = ref;                          // as before: B1 on the expected slot
	schedule(a1 + samples(0.86), EV_BK_DATA, a1, 3);
	bk_tx_.clear(); b1_reset();
	queue_words(a1 + samples(kP3CalleeReply - cfg_.rt_latency_s), my_bk(true), 82);
	schedule(callee_look_at(my_start_), EV_P3, my_start_, 11);
}

void P2Station::greet_scan_next(const char *why)
{
	static const int kScan[5] = {1, 2, 3, 4, 5};
	if (greet_scan_ < 0) log(std::string("P3: set-up: ") + why + ": trying every level in turn (fix 210)");
	greet_scan_ = (greet_scan_ + 1) % 5;
	p3_level_ = kScan[greet_scan_]; greet_long_ = false;
	fb_[0].alive = fb_[1].alive = false; greet_ep_cs_ = -1;
}

void P2Station::fb_letter(int cs, int sent, bool informative)
{
	for (int k = 0; k < 2; k++) {
		FbModel &m = fb_[k];
		if (!m.alive || cs < 0) continue;
		if (informative && m.lvl == sent && (fb_[0].alive && fb_[1].alive)) { m.alive = false; log(std::string("P3: fall-back from ") + (k ? "S" : "T") + " ruled out"); continue; }
		if (cs == p2arq::CS_T && (m.t_run++ % 5) == 0 && m.lvl > m.floor) m.lvl--;
	}
	if (fb_[0].alive && fb_[1].alive) { if (informative && cs >= 0) fb_next_ ^= 1; }   // both possible: the other one next
	else fb_next_ = fb_[0].alive ? 0 : 1;
	p3_level_ = fb_[fb_next_].lvl;
}

double P2Station::p3_tone_power(int64_t from, int64_t to, const int *ks, int nk) const
{
	const int64_t blk = samples(0.01);
	double sum = 0; int nb = 0;
	for (int i = 0; i < nk; i++) {
		const double w = 2 * M_PI * (480.0 + 120.0 * ks[i]) / cfg_.fs;
		const std::complex<double> step = std::polar(1.0, -w);
		for (int64_t b = from; b + blk <= to; b += blk) {
			std::complex<double> ph = std::polar(1.0, -w * double(b % int64_t(cfg_.fs))), acc = 0;
			for (int64_t t = b; t < b + blk; t++, ph *= step)
				acc += double((t >= 0 && t < n_ && n_ - t < kRing - 1) ? raw_[size_t(t % kRing)] : 0.0f) * ph;
			sum += std::norm(acc) / double(blk * blk); nb++;
		}
	}
	return nb ? sum / nb : 0.0;
}

bool P2Station::p3_bk_peer_packet(int64_t exp)
{
	static const bool old = std::getenv("P3_BK_PK_OLD") != nullptr;
	if (old || p3_bk_ref_far_ <= 0) return false;
	const int64_t a = exp - samples(0.04), b = exp + samples(0.20);
	const double far = p3_tone_power(a, b, kP3FarTones, 4), cs = p3_tone_power(a, b, kP3CsTones, 2);
	const bool pk = far >= 0.25 * p3_bk_ref_far_ && far >= 0.4 * cs;
	if (debug_ || pk)
		log("P3: look after our break-in: far tones " + std::to_string(far / std::max(cs, 1e-9)) + " of the control-signal tones, " +
		    std::to_string(far / p3_bk_ref_far_) + " of the peer's last packet" +
		    (pk ? ": the peer's data packet, our break-in not taken (fix 277)" : ""));
	return pk;
}

void P2Station::take_setup_a2(int64_t a2)
{
	const int64_t cyc = samples(cyc_s());
	misses_ = 0;
	if (!early_old()) {
		greet_taken();                              // A2 takes our greeting (what it carried of our queued text)
		// A changeover key pressed while our first turn went out is spent with its "over", as on the
		// reference unit (emulated: a Ctrl-Y typed then is lost; the text waits for the next one).
		cho_req_ = false;
	}
	p3_bk_rx(a2, "A2");
	// Our answer sets the caller's level: T = 5, P = 1 (cfg p3_level).
	int a2cs = level_letter(cfg_.p3_level);                       // experiment hook: P3_A2_CS=P|Q|R|S|T
	if (const char *e = std::getenv("P3_A2_CS")) { const char *q = std::strchr(p2arq::kCsName, e[0]); if (q) a2cs = int(q - p2arq::kCsName); }
	// Fix 229: the reference unit as callee answers the A2 by how cleanly it came through, as a
	// changeover break-in: T (SL5) when clean, S (SL3) at 3 dB in the emulator's pair runs, else its
	// alternation letter. We answered T always, so at 3 dB the reference unit sent SL5 that needed
	// two copies a packet. Our reading is fix 42's (bk_error), taken when the A2's data has decoded (0.86 s)
	// if the answer is due at least 10 ms later; otherwise, or if the A2 did not decode,
	// the configured letter as before. Never above it. P3_A2_FIXED=1: always configured.
	const int64_t a2at = a2 + samples(kP3CalleeReply - cfg_.rt_latency_s);
	static const bool a2_fixed = std::getenv("P3_A2_FIXED") != nullptr || std::getenv("P3_A2_CS") != nullptr;
	if (!a2_fixed && a2at - (a2 + samples(0.86)) >= samples(0.010)) { a2_ans_at_ = a2at; a2_ans_cap_ = a2cs; }
	else { send_cs(a2cs, a2at); a2_ans_at_ = -1; }
	p3_rx_level_ = letter_level(a2cs);
	p3_run_ = 1; p3_q_ = -1;                        // fix 229: our good-packet run cleared on the burst, then counts it
	state_ = ST_IRS;
	ack_phase_ = 0;                                 // first acknowledgement Q
	last_ack_cs_ = p2arq::CS_T;
	p3_a2_rx_t_ = true;                             // fix 258: that Q plain (no R or S on the first packet)
	last_rx_counter_ = -1;
	rx_fails_ = 0; good_run_ = 0;
	schedule(a2 + cyc + samples(kP3Launch), EV_P3_LAUNCH, a2 + cyc, 2000);
	schedule(a2 + cyc + samples(0.88), EV_PACKET, a2 + cyc);
	log("P3: receiving");
}

bool P2Station::match_a2(int64_t exp, double &st)
{
	static int nomatch_n = 0;                     // test hook (fix 235): P3_TEST_A2_NOMATCH=N, our first N A2 header matches fail
	if (const char *e = std::getenv("P3_TEST_A2_NOMATCH"); e && nomatch_n < std::atoi(e)) { nomatch_n++; if (debug_) log("P3: test: A2 header match made to fail"); return false; }
	const bool ok = match_words(exp, p3::kSetupA2, 24, st);
	a2_partial_ = !ok && mw_partial_;
	a2_partial_at_ = a2_partial_ ? mw_partial_at_ : -1;
	return ok;
}

bool P2Station::a2_quiet(int64_t exp)
{
	// Fix 257: also an A2 whose header words heard so far all fit but began too late for the look
	// (a long audio path: the caller's A2 went whole, late). P3_LATE_A2_OFF=1: score only.
	static const bool late_off = std::getenv("P3_LATE_A2_OFF") != nullptr;
	const bool late_a2 = !late_off && cfg_.realtime && a2_partial_;
	if (std::getenv("P3_NO_A2_WATCH") || !greet_sent_ || (look_score_ < 0.60 && !late_a2)) return false;
	if (a2_quiet_at_ >= 0 && std::llabs(exp - a2_quiet_at_ - samples(2 * cyc_s())) < samples(0.1)) return false;   // the cycle after a silent one: sent
	a2_quiet_at_ = exp;
	a2_late_at_ = late_a2 && a2_partial_at_ > exp ? a2_partial_at_ : -1;   // fix 257: read where it began
	log("P3: a burst where A2 belongs, not read (score " + std::to_string(look_score_) + (late_a2 ? ", its words heard so far fit: late" : "") + "): silent one cycle to decode it (fix 235)");
	schedule(std::max((a2_late_at_ >= 0 ? a2_late_at_ : exp) + samples(0.86), n_), EV_P3, exp, 15);
	return true;
}

void P2Station::a2_decode(int64_t exp)
{
	if (state_ != ST_P3SETUP) return;
	RxPacket q;
	const int64_t dx = a2_late_at_ >= 0 ? a2_late_at_ : exp;   // fix 257: a late A2 read where it began
	if (cfg_.realtime) {
		if (a2d_slot_ != dx) {
			a2d_slot_ = dx;
			a2d_fut_ = pool_.submit([this, dx, sp = snapshot_short(dx)]() { return find_packet(dx, true, false, &sp); });
		}
		if (a2d_fut_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
			if (n_ < dx + samples(cyc_s() - 0.10)) { schedule(n_ + samples(0.005), EV_P3, exp, 15); return; }
			a2d_fut_.wait();                            // (the repeat's answer is still in time)
		}
		q = a2d_fut_.get(); a2d_slot_ = -1;
	} else q = find_packet(exp, true, false);
	a2_late_at_ = -1;
	const int64_t cyc = samples(cyc_s());
	if (q.ok && q.bk) {
		const int64_t a2 = int64_t(std::llround(q.start));
		const bool now = n_ < a2 + samples(kP3CalleeReply - cfg_.rt_latency_s - 0.005);
		log("P3: A2 decoded without a header match, " + std::to_string(double(a2 - exp) / cfg_.fs * 1000.0) + " ms from the expected slot: " +
		    (now ? "answered" : "its repeat answered") + " (fix 235)");
		if (!now) psk_swap_ = !psk_swap_;               // one more cycle without a burst
		take_setup_a2(now ? a2 : a2 + cyc);
		return;
	}
	if (debug_) log("P3: no A2 in the slot (decode): the greeting again");
	psk_swap_ = !psk_swap_;                             // the silent cycle
	const int64_t g = exp - samples(master_seen_s()) + 2 * cyc;
	send_greeting(g);
	my_start_ = g;
	const int64_t a2ref = g + (greet_long_ ? samples(lcyc_s() - cyc_s()) : 0);
	schedule(callee_look_at(a2ref), EV_P3, a2ref, 12);
}

void P2Station::send_greeting(int64_t slot)
{
	greet_sent_ = true;
	const std::string g = greet_txt_.substr(std::min(p3_greet_off_, greet_txt_.size()));   // B1 carried the start
	if (const char *e = std::getenv("P3_TEST_GREET_LEVEL")) {   // test hook (fix 210): the greetings' levels, "4" or "4,4,2,1" (the last repeats)
		static int k = 0; int i = 0; const char *q = e;
		while (i < k && std::strchr(q, ',')) { q = std::strchr(q, ',') + 1; i++; }
		k++; p3_level_ = std::atoi(q);
		if (q[0] && q[1] == 'L') greet_long_ = true;   // "1L": SL1 long
	}
	// What it carries: as before when the text fits as it is (the connect text alone always does);
	// else packed as a data packet is (the reference unit's greeting with queued text: Huffman), with
	// no "over" when text is left (status 0x23 | type: counter 3, 0x20; the reference unit 0x27).
	const bool lng1 = greet_long_ && p3_level_ == 1;
	const int cap = lng1 ? p3::long_payload(1) : (p3_level_ >= 2 && p3_level_ <= 4) ? p3::level_data_bytes(p3_level_) : p3_level_ == 1 ? p3::kSl1Data : p3::kDataBytes;
	std::vector<uint8_t> data(size_t(cap), 0x1e);
	int type = 0;
	if (g.size() <= size_t(cap) || greet_q_ == 0) {   // (the connect text alone: cut at the packet's size, as before)
		for (size_t i = 0; i < g.size() && i < size_t(cap); i++) data[i] = uint8_t(g[i]);
		greet_used_ = std::min(g.size(), size_t(cap));
	} else {
		type = cfg_.compress ? text::choose(g, cap, cfg_.allow_pmc, false) : 0;
		greet_used_ = size_t(std::max(0, text::encode(type, g, data.data(), cap, false)));
	}
	greet_more_ = greet_q_ > 0 && greet_used_ < g.size();
	const uint8_t st = uint8_t((greet_more_ ? 0x23 : 0x63) | type << 2);
	if (debug_) log("P3: greeting at level " + std::to_string(p3_level_) + (greet_long_ ? " (long)" : "") +
	                (g.size() > cfg_.p3_greeting.size() - std::min(p3_greet_off_, cfg_.p3_greeting.size()) ? ", with " + std::to_string(greet_q_) + " queued characters" + (greet_more_ ? ", more to send (no over)" : "") : ""));
	if (lng1) {
		queue_psk(slot, p3::long_encode(1, data.data(), st));
	} else if (p3_level_ >= 2 && p3_level_ <= 4) {
		queue_lv(slot, p3_level_, p3::encode_level(p3_level_, data.data(), st));
	} else if (p3_level_ == 1) {                     // SL1: 5 bytes only
		uint8_t info[8];
		p3::sl1_make_info(data.data(), st, info);
		queue_psk(slot, p2::encode(1, info, 3));
	} else {
		uint8_t info[p3::kInfoBytes];
		p3::make_info(data.data(), 3, info, uint8_t(st & ~3));   // counter 3, 0x20, OVER (or not), the type
		queue_p3(slot, p3::encode(info, 3, true));
	}
	p3_first_tx_ = false;
	my_start_ = slot;
}

double P2Station::bk_error(int64_t start, const double *cc) const
{
	constexpr int kSteps = 80;                      // pattern (24) + data (56)
	const Demod d = demod(start - samples(0.04), start + samples(0.86), kSteps + 2, nullptr, cc);
	if (!d.ok) return 1e9;
	auto median = [](std::vector<double> v) {
		std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
		return std::max(1e-9, v[v.size() / 2]);
	};
	// common phase offset: steps folded to one DBPSK point (doubling the angle)
	std::complex<double> acc = 0;
	// step (1/16 turns), the smaller of its pulses against its carrier's median, index
	struct Step { double v, g; size_t i; };
	std::vector<Step> st;
	for (int c = 0; c < 2; c++) {
		const auto &Z = c ? d.hi : d.lo;
		std::vector<double> a;
		for (size_t i = 1; i < Z.size(); i++) a.push_back(std::abs(Z[i]));
		if (a.empty()) return 1e9;
		const double cm = median(a);
		for (size_t i = 2; i < Z.size(); i++) {
			const double v = nib(Z[i - 1], Z[i]);
			acc += std::polar(1.0, 2 * 2 * M_PI * (v - 2.0) / 16.0);   // points 2 and 10 fold together
			st.push_back({v, std::min(std::abs(Z[i - 1]), std::abs(Z[i])) / cm, i});
		}
	}
	const double bias = 16.0 * std::arg(acc) / (2 * 2 * M_PI);
	double sum = 0; int n = 0;
	for (const auto &x : st) {
		// with the decode's timing Z[1] is the burst's reference pulse: steps 2..81 are its 80 words
		if (cc ? (x.i < 2 || x.i > size_t(kSteps + 1)) : x.g < 0.25) continue;
		const double e = std::fmod(x.v - bias - 2.0 + 32.0, 8.0);
		const double err = std::min(e, 8.0 - e);           // sixteenths off the DBPSK point, 0..4
		sum += std::min(err, 4.0 - err) * 22.5;            // degrees off the nearest quadrant point, 0..45
		n++;
	}
	if (!n) return 1e9;
	// the carriers' levels over the burst's pulses (1..81 with the decode's timing)
	double cmed[2];
	for (int c = 0; c < 2; c++) {
		const auto &Z = c ? d.hi : d.lo;
		std::vector<double> a;
		for (size_t i = 1; i < Z.size() && (!cc || i <= size_t(kSteps + 1)); i++) a.push_back(std::abs(Z[i]));
		cmed[c] = median(a);
	}
	const double imb = 20.0 * std::log10(std::max(cmed[0], cmed[1]) / std::min(cmed[0], cmed[1]));
	return sum / n + kBkImbDeg * imb;
}

void P2Station::p3_bk_decode(int64_t start, int kind)
{
	static const char *const names[3] = {"B1", "A2", "break-in"};
	if (kind == 3) {                                // the caller's A1: an SL1 packet
		const RxPacket q = find_packet(start, false, false);
		if (debug_) log(std::string("P3: A1 ") + (q.ok ? "decoded, level " + std::to_string(q.level) + " status " + std::to_string(q.ok ? q.info[size_t(p3::kSl1Data)] : 0) : "NOT decoded"));
		if (q.ok && q.level == 1) deliver(q.info, p3::kSl1Data, q.info[size_t(p3::kSl1Data)]);
		return;
	}
	RxPacket p;
	if (cfg_.realtime && kind == 2 && !std::getenv("P3_BK_SYNC_DECODE")) {
		// Fix 83: on a worker (it blocked the audio loop 10-40 ms), polled.
		if (bkd_slot_ != start) {
			bkd_slot_ = start;
			const bool cc_ok = bk_cc_ok_ && bk_cc_start_ == start;
			const double c0 = bk_cc_[0], c1 = bk_cc_[1];
			bkd_fut_ = pool_.submit([this, start, cc_ok, c0, c1, sp = snapshot_short(start)]() {
				double cc[2] = {c0, c1};
				return find_packet(start, true, false, &sp, cc_ok ? cc : nullptr);
			});
		}
		if (bkd_fut_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
			const int64_t step = samples(0.002);
			if (bk_ans_at_ >= 0 && n_ + step < bk_ans_at_ - samples(0.003)) { schedule(n_ + step, EV_BK_DATA, start, kind); return; }
			if (bk_ans_at_ >= 0) {                 // the answer is due: as configured
				if (cfg_.paced) { dstats_.late_bk++; load_.late_bk++; }
				log("P3: break-in decode late, answering " + std::string(1, p2arq::kCsName[bk_ans_cap_]));
				if (bk_ans_at_ > n_) send_cs(bk_ans_cap_, bk_ans_at_);
				arm_bk_answer(bk_ans_cap_);
				bk_ans_at_ = -1;
			}
			schedule(n_ + samples(0.010), EV_BK_DATA, start, kind);   // its data still delivered when done
			return;
		}
		p = bkd_fut_.get(); bkd_slot_ = -1;
	} else p = find_packet(start, true, false, nullptr, bk_cc_ok_ && bk_cc_start_ == start ? bk_cc_ : nullptr);
	// Fix 83: a break-in whose data did not decode is answered with Q, the
	// repeat request (the reference unit as receiver: Q, "break-in not
	// taken"; emulator: our break-in with its data spoilt was answered Q
	// and taken when repeated). Answering with a level letter acknowledged
	// it and its characters were lost (K0NTS 40 m, 2026-10-17 20:05: the
	// banner's first word).
	if (const char *f = std::getenv("P3_TEST_A2_FAIL"); f && kind == 1) { static int n = 0; if (n++ < std::atoi(f)) { p.ok = false; log("test: A2 decode failed"); } }   // fix 213b test hook
	if (const char *f = std::getenv("P3_BK_RX_FAIL"); f && kind == 2 && bk_rxfail_n_ < std::atoi(f)) { p.ok = false; bk_rxfail_n_++; log("test: break-in decode failed"); }
	// Fix 268: the same for the caller's A2. The reference unit (emulator) answers an A2 whose data did not
	// decode with Q and takes it when repeated; our T there acknowledged it and its characters were lost
	// (on the air 2026-11-19 u5-3: the A2's "~~f" missing, the sync handshake never recognised, link
	// stalled; emulator P3_TEST_A2_FAIL=1: "ECHO F" missing). P3_A2_NO_Q=1: the configured letter as before.
	if (kind == 1 && a2_ans_at_ >= 0 && !p.ok && !std::getenv("P3_A2_NO_Q")) {
		log("P3: A2 data NOT decoded: answering Q (repeat request, fix 268)");
		bk_wait_q(start, a2_ans_at_);
		a2_ans_at_ = -1;
		return;
	}
	if (kind == 2 && bk_ans_at_ >= 0 && !p.ok && !std::getenv("P3_BK_NO_Q")) {
		log("P3: break-in data NOT decoded: answering Q (repeat request)");
		bk_wait_q(start, bk_ans_at_);
		bk_ans_at_ = -1;
		return;
	}
	// A decoded B1/A2/break-in measured the offset over its pattern (known
	// steps, the full +-8/16 range): the AFC learns nothing else in set-up
	// (fix 32). Without it a peer 20 Hz off linked (B1 decoded) but its
	// first SL5 packet never decoded (emulator --chan-offset 20; fix 49).
	if (p.ok && p.bk) {
		// Unwrapped around the AFC: the pattern measures +-8/16 (+-50 Hz) only;
		// a peer 60 Hz off wrapped to +47 Hz, the wrong sign (emulator).
		const double bu = p.bias + 16.0 * std::round((afc_ - p.bias) / 16.0);
		afc_update(p.bias, 16.0);
		// The same measurement in Hz for the P3 packet decoders (SL5's own
		// estimate = 6.25 x this bias, emulator +-10 Hz). Their header-based
		// estimate is ambiguous beyond ~+-12.5 Hz (QPSK): at 20 Hz the first
		// SL5 packet never decoded. Shift their audio by it instead (fix 49).
		// The set-up's FSK measurement when there is one: the pattern's
		// estimate is off by ~7 Hz at 60 Hz (demodulated at the nominal tones).
		// Fix 265 (TODO ff 2): a break-in later in the link keeps the shift the decodes have tracked since.
		// The set-up's FSK measurement is from the link's first seconds: on 6 m the radios drift (on the air
		// 2026-11-18 the reference unit's control signals came at -13 Hz at set-up, -9.5 at 37 s, -7.3 at
		// 43 s), and a station whose caller shifts its transmit after the set-up (two Signums) hears the
		// set-up's FSK tens of Hz from the PSK that follows (n4a10: 8.75 Hz re-imposed at every break-in
		// while the decodes had tracked to 1.66 Hz; then every try "back to 8.75"). Only the set-up's
		// bursts (B1/A2, before any data packet decoded) take it. P3_BK_SHIFT_OLD=1: at every break-in.
		static const bool bk_shift_old = std::getenv("P3_BK_SHIFT_OLD") != nullptr;
		if (kind == 2 && fs_anchor_ok_ && !bk_shift_old) {
			if (debug_) log("P3: break-in offset " + std::to_string(6.25 * bu) + " Hz by its pattern; the receive shift kept at the decodes' " + std::to_string(p3_rx_df_) + " Hz (fix 265)");
		} else {
			p3_rx_df_ = fsk_afc_w_ > 0 ? -(fsk_afc_sum_ / fsk_afc_w_) : 6.25 * bu;
			p3_df_set_ = true;
			if (debug_) log("P3: break-in offset " + std::to_string(p3_rx_df_) + " Hz (P3 receive shift)");
		}
	}
	if (kind == 1 && a2_ans_at_ >= 0) {             // fix 229: our answer to the A2
		int letter = a2_ans_cap_;
		if (p.ok) {
			const double q = bk_error(start, p.has_bkc ? p.bkc : nullptr);
			// In heavier noise the reference unit answers its alternation letter (SL1). We stop at S: one
			// faded A2 put us at SL1 for the link (p3-poor seed 2: phase error 23.2 degrees, P, 289 characters against
			// 1,435 with T). P3_A2_SL1=1: P from kBkErrS, as the reference unit.
			static const bool a2_sl1 = std::getenv("P3_A2_SL1") != nullptr;
			const int by_q = q < kBkErrT ? p2arq::CS_T : (q < kBkErrS || !a2_sl1) ? p2arq::CS_S : p2arq::CS_P;
			if (letter_level(by_q) < letter_level(letter)) letter = by_q;   // never above our configured level
			char m[96];
			std::snprintf(m, sizeof m, "P3: A2 phase error %.1f deg, answering %c", q, p2arq::kCsName[letter]);
			log(m);
		}
		if (a2_ans_at_ > n_) send_cs(letter, a2_ans_at_);
		// The reference unit answers A2 as it answers a break-in (the speed-change
		// episode): after S a failure is answered S again, the second one P
		// with SL2 expected (fix 88's view from the sender's side). T keeps the old state.
		if (letter != a2_ans_cap_) {
			if (letter == p2arq::CS_S) arm_bk_answer(letter);
			else { p3_rx_level_ = letter_level(letter); rx_up_pending_ = false; }
			last_ack_cs_ = letter;
		}
		a2_ans_at_ = -1;
	}
	if (kind == 2 && bk_ans_at_ >= 0) {             // our answer to the break-in (fix 42)
		int letter = bk_ans_cap_;
		if (p.ok) {
			const double q = bk_error(start, p.has_bkc ? p.bkc : nullptr);
			const int by_q = q < kBkErrT ? p2arq::CS_T : q < kBkErrS ? p2arq::CS_S : p2arq::CS_P;
			if (letter_level(by_q) < letter_level(letter)) letter = by_q;   // never above our configured level
			char m[96];
			std::snprintf(m, sizeof m, "P3: break-in phase error %.1f deg, answering %c", q, p2arq::kCsName[letter]);
			log(m);
		}
		if (bk_ans_at_ > n_) send_cs(letter, bk_ans_at_);
		arm_bk_answer(letter);
		bk_ans_at_ = -1;
	}
	// Fix 270: a repeat of the peer's break-in re-taken: its data was delivered with the first copy.
	const bool dup = p.ok && kind == 2 && bk_rep_dedup_ && p.info.size() > size_t(p3::kBkData) &&
	                 bk_last_info_.size() == size_t(p3::kBkData) + 1 && std::equal(bk_last_info_.begin(), bk_last_info_.end(), p.info.begin());
	if (kind == 2) bk_rep_dedup_ = false;
	if (dup) log("P3: the repeated break-in's data is the one delivered already: not delivered again (fix 270)");
	else if (p.ok) deliver(p.info, p3::kBkData, p.info[size_t(p3::kBkData)] & 0x1c);
	if (p.ok && kind == 2 && p.info.size() > size_t(p3::kBkData)) bk_last_info_.assign(p.info.begin(), p.info.begin() + p3::kBkData + 1);
	if (p.ok && (p.info[size_t(p3::kBkData)] & 0x80) && kind == 2 && state_ == ST_IRS) {
		qrt_rx_ = true;                             // the break-in also asks for QRT: the end packet follows
		log("P3: break-in carries QRT");
	}
	// Fix 134: a break-in with "over" (PDuplex 1: its text, and the link handed straight back):
	// as for a packet's over bit (fix 50), we take the link back at once. Refused before, it
	// was answered Q for ever (emulator and real reference unit with PDuplex 1, our QRT pending).
	if (p.ok && (p.info[size_t(p3::kBkData)] & 0xc0) == 0x40 && kind == 2 && state_ == ST_IRS) {
		must_break_in_ = true;
		log("P3: break-in carries over: the link back to us");
	}
	if (debug_ || !p.ok) log(std::string("P3: ") + names[kind] + (p.ok ? " data decoded" : " data NOT decoded"));
}

const uint8_t * P2Station::my_bk(bool b1)
{
	if (bk_tx_.empty()) {
		uint8_t d[p3::kBkData] = {0x1e, 0x1e, 0x1e};
		bool more;
		if (b1) {
			greet_txt_ = cfg_.p3_greeting; greet_q_ = 0; greet_used_ = 0; greet_more_ = false;
			if (!early_old()) { greet_txt_ += txq_; greet_q_ = txq_.size(); }   // what is queued now goes in our first turn
			p3_greet_off_ = std::min<size_t>(p3::kBkData, greet_txt_.size());
			for (size_t i = 0; i < p3_greet_off_; i++) d[i] = uint8_t(greet_txt_[i]);
			more = greet_txt_.size() > p3_greet_off_;
		} else {
			pending_used_ = int(std::min<size_t>(p3::kBkData, txq_.size()));
			for (int i = 0; i < pending_used_; i++) d[i] = uint8_t(txq_[size_t(i)]);
			more = txq_.size() > size_t(pending_used_);
			if (pending_used_ > 0) sent_text_ = true;
		}
		bk_tx_ = p3::bk_words(d, more, b1 ? 0xaa : 0x22);
	}
	return bk_tx_.data();
}

int64_t P2Station::queue_words(int64_t at, const uint8_t *w, int n)
{
	std::vector<uint8_t> words(w, w + n);
	// Test hook: P3_BK_SPOIL=N spoils the data of our first N changeover
	// break-in transmissions (pattern intact, CRC fails at the peer).
	if (const char *sp = std::getenv("P3_BK_SPOIL"); sp && !bk_tx_.empty() && w == bk_tx_.data() && state_ != ST_P3SETUP && bk_spoil_n_ < std::atoi(sp)) {
		for (int i = 40; i < n; i += 3) words[size_t(i)] ^= 0x88;
		bk_spoil_n_++;
		log("test: break-in spoilt");
	}
	at = queue_psk(at, words, true);
	my_start_ = at;
	// Test hook: P3_REPLAY_B1=FILE.raw:SLOT sends a recorded B1 instead.
	const char *rp = std::getenv("P3_REPLAY_B1");
	if (rp && w == p3::kSetupB1) {
		std::string spec(rp);
		const size_t c = spec.rfind(':');
		if (FILE *f = std::fopen(spec.substr(0, c).c_str(), "rb")) {
			const double s0 = std::atof(spec.c_str() + c + 1);
			const long pre = 480;
			std::fseek(f, long(std::floor(s0) - pre) * 2, SEEK_SET);
			std::vector<int16_t> b(static_cast<size_t>(9600));
			const size_t got = std::fread(b.data(), 2, b.size(), f);
			std::fclose(f);
			tx_.back().audio.assign(b.begin(), b.begin() + long(got));
			tx_.back().start = at - pre;
			log("B1 replaced by the recording");
		}
	}
	return at;
}

void P2Station::start_p3_master(int64_t ref)
{
	p3_ = true; p3_first_tx_ = true; level_ = 1; long_ = false; psk_swap_ = false;
	state_ = ST_P3SETUP; misses_ = 0; p3_drift_reset();
	link_status(true, " (P3)");
	log("P3 link set up");
	next_start_ = ref + samples(1.261 + (cyc_s() - 1.25) + cfg_.first_psk_adjust_s);
	grid_ = next_start_;
	if (p1_sd_) hdr_flip_ ^= 1;                    // fix 82: the toggle restarts after the speed-down
	const auto a1 = setup_a1();
	queue_words(next_start_, a1.data(), int(a1.size()));
	schedule(my_start_ + samples(kP3CalleeReply + p3_b1_look()), EV_P3, my_start_, 0);
}

void P2Station::new_send_turn()
{
	cstk_[0].turn_new = cstk_[1].turn_new = true;   // fix 67: the peer's reply timing may move with the turn (KB2PCN-8 80 m: a 0.95 R refused as off the track)
	mq3_clear();
	p3_up_fails_ = -1; p3_t_run_ = 0; r_pending_ = false; r_run_ = 0; long_cs_off_ = -1; long_jump_ = 0; long_cand_n_ = 0; long_rate_ = 0; p3_ans_cs_ = -1;
	p3_guess_rep_ = -1;                             // fix 276
	must_break_in_ = false;                         // an "over" heard before this turn is spent (fix 50)
	bk_req_ = false;                                // decision a: our break-in key is spent once we send
	// Fix 146: our P2 receiver's speed-up / long request ends with our receiving turn. Left
	// pending, it was found "taken" in the next one (the new sender at a higher level than we
	// had asked it up from) and that turn's first new packet answered P, the letter from
	// before the S, for Q: the firmware repeated it for the rest of the link. Seen at 40-70
	// Hz, where the firmware answers our call with CS4 and the link starts at level 1 [bb-emu].
	if (!std::getenv("P2_UP_ACROSS_TURNS")) { p2_up_pending_ = false; p2_up_fails_ = 0; p2_up_bad_ = 0; }
}

void P2Station::arm_bk_answer(int letter)
{
	p3_rx_level_ = letter_level(letter);
	if (letter == p2arq::CS_T || letter == p2arq::CS_S) {
		rx_up_pending_ = true; rx_up_fails_ = 0; rx_up_cs_ = letter; rx_pre_s_ = p2arq::CS_Q;
		rx_up_from_ = std::max(1, p3_rx_level_ - 1);   // SL4 after T, SL2 after S
	} else {
		rx_up_pending_ = false;
		// Fix 276: after a P answer our repeat request is P, the letter we sent (as after the A2's,
		// which goes down the same path); take_p3_bk left T there. A failed first packet was then
		// answered T and the next good one Q, which a sender that read the T (it expects P after T)
		// takes for a repeat request, and its repeat, decoded again, got Q again: the link stuck
		// (lockstep, p3_bk_guess_test). The reference receiver repeats its P (on air 2026-11-23 17:44,
		// 80 m: P 71 times to our failing packets). P3_BK_P_REPEAT_OLD=1: T as before.
		static const bool p_rep_old = std::getenv("P3_BK_P_REPEAT_OLD") != nullptr;
		if (letter == p2arq::CS_P && !p_rep_old) last_ack_cs_ = p2arq::CS_P;
	}
}

void P2Station::bk_wait_q(int64_t bk, int64_t q_at)
{
	const int64_t cyc = samples(cyc_s());
	send_cs(p2arq::CS_Q, q_at);
	bk_q_at_ = q_at;
	events_.erase(std::remove_if(events_.begin(), events_.end(), [&](const Event &e) {
		return e.ref == bk + cyc && (e.type == EV_PACKET || e.type == EV_P3_LAUNCH || e.type == EV_P3_SYNC || e.type == EV_P3_SL1);
	}), events_.end());
	if (master_) my_start_ += cyc;                 // its answer lattice moves on one cycle
	schedule(bk + cyc + samples(0.30), EV_BK_AGAIN, bk + cyc);
}

void P2Station::on_bk_again(int64_t exp)
{
	if (!p3_ || state_ != ST_IRS) return;
	double st = 0;
	if (looks_like_bk(exp, st)) { log("P3: break-in repeated"); take_p3_bk(st, -1, !bk_rep_old()); return; }
	if (++misses_ > cfg_.timeout_cycles) { disconnect("timeout"); return; }
	if (debug_) log("P3: no repeat of the break-in, Q again");
	const int64_t cyc = samples(cyc_s());
	bk_q_at_ += cyc;
	send_cs(p2arq::CS_Q, bk_q_at_);
	if (master_) my_start_ += cyc;
	schedule(exp + cyc + samples(0.30), EV_BK_AGAIN, exp + cyc);
}

bool P2Station::p3_bk_repeat(int64_t exp)
{
	if (!bk_rep_watch_ || bk_rep_old() || state_ != ST_IRS || bk_rep_n_ >= kBkRepMax) return false;
	double st = 0;
	CsPred none;
	const BkLook b = bk_look(exp, none, nullptr, afc_, n_);
	if (b.hit != 2) return false;
	st = b.start;
	const double off = (st - double(exp)) / cfg_.fs;
	if (std::fabs(off) > kBkRepWin) {
		if (debug_) log("P3: a break-in at " + std::to_string(off * 1000.0) + " ms from our lattice: too far for the peer's repeat, not taken");
		return false;
	}
	const int64_t cyc = samples(cyc_s());
	const int64_t k = std::llround(double(exp - bk_rep_first_) / double(cyc));
	char m[200];
	std::snprintf(m, sizeof m, "P3: the peer's break-in again (our answer not taken): its own timing %+.1f ms from our lattice; taken there, answering again (fix 270)", off * 1000.0);
	log(m);
	bk_bias_ = b.bias; bk_cc_ok_ = false;
	const int saved_misses = misses_;
	bk_rep_n_++;
	take_p3_bk(st, master_ ? bk_rep_at0_ + k * cyc : -1, true);
	misses_ = saved_misses + 1;                  // a repeat is not progress: the timeout still counts
	return true;
}

void P2Station::take_p3_bk(double start, int64_t at_override, bool repeat)
{
	const int64_t bk = int64_t(start), cyc = samples(cyc_s());
	// Its pattern measured the offset (known steps: no half-turn ambiguity);
	// the AFC learns nothing else during set-up (fix 32). KC0TPS ~32 Hz off.
	afc_update(bk_bias_, 16.0);
	const int letter = level_letter(cfg_.p3_level);
	// The caller answers a break-in 1.85 s after its own last packet
	// (0.964 s after the break-in) and its lattice moves with it.
	int64_t at;
	// After one of our long-cycle packets the break-in comes 2.5 s later (fix 46).
	if (at_override >= 0) { at = at_override; if (master_) grid_ = at - samples(0.6); }   // fix 270: a repeat re-taken
	else if (master_) { at = my_start_ + samples((p3_long_ ? lcyc_s() - cyc_s() : 0.0) + cyc_s() + 0.6); grid_ = at - samples(0.6); }
	else at = bk + samples(kP3CalleeReply - cfg_.rt_latency_s);
	// fix 270: a repeat (re-taken here, or after our Q) keeps what the first copy delivered
	if (!repeat) { bk_rep_n_ = 0; bk_last_info_.clear(); }
	bk_rep_watch_ = true; bk_rep_first_ = bk; bk_rep_at0_ = at; bk_rep_dedup_ = repeat && !bk_last_info_.empty();
	// The letter sets the new sender's level. The reference unit picks it by
	// how clean the break-in was (fix 42), so when the answer
	// is due after that decode (bk + 0.86 s, 10 ms to spare) it is
	// chosen there (p3_bk_decode); otherwise as configured.
	// (Fix 83: in real time the decode runs on a worker and is polled up to
	// 3 ms before the answer; a late one falls back to the configured letter.)
	const int64_t spare = cfg_.realtime && !std::getenv("P3_BK_SYNC_DECODE") ? samples(0.004) : samples(0.010);
	if (at - (bk + samples(0.86)) >= spare && !std::getenv("P3_BK_FIXED_LETTER")) { bk_ans_at_ = at; bk_ans_cap_ = letter; }
	else { send_cs(letter, at); bk_ans_at_ = -1; }
	p3_bk_rx(bk, "break-in");
	p3_run_ = 1; p3_q_ = -1;                        // fix 229: as after A2
	txq_.erase(0, size_t(pending_used_)); pending_used_ = 0;   // the break-in acknowledges our last packet
	state_ = ST_IRS; misses_ = 0;
	ack_phase_ = 0; last_ack_cs_ = p2arq::CS_T; last_rx_counter_ = -1; rx_fails_ = 0; good_run_ = 0;
	rx_t_state_ = false; p3_a2_rx_t_ = false;
	// Fix 274: the new sender starts on short packets (break-in + 1.25 s, counter 3; our own sender does
	// the same unless the answer is R, which we never give here). Our receiver's long/short state from our
	// last turn as receiver must not survive the changeover. On the air (2026-11-20 18:24, 80 m) we had
	// asked for short packets with R; the short packet that completed it was the one we broke in on, so
	// the pending flag was never cleared (only a new packet answered plainly clears it). The peer broke
	// back in later: with the flag up, our failed look at its first SL1 packet waited for a long look that
	// was never scheduled: no answer, nothing scheduled, the watchdog 4.3 s later (9 cycles counted
	// failed), then both looks on long timing (one answer per 3.75 s) for the rest of the link, which
	// timed out 220 s later. P3_BK_RX_SHORT_OLD=1: the state kept as before.
	static const bool bk_rx_short_old = std::getenv("P3_BK_RX_SHORT_OLD") != nullptr;
	if (!bk_rx_short_old) {
		if (p3_rx_long_ || p3_r_pending_) log("P3: receiving after the break-in: short packets (our pending long/short switch dropped, fix 274)");
		p3_rx_long_ = false; p3_r_pending_ = false; p3_long_good_ = 0;
	}
	arm_bk_answer(letter);
	sent_text_ = false;
	schedule(bk + cyc + samples(kP3Launch), EV_P3_LAUNCH, bk + cyc, 2000);
	schedule(bk + cyc + samples(0.88), EV_PACKET, bk + cyc);
	log("P3: break-in received, now receiving");
	peer_broke_in(bk);
	qrt_peer_took_link();
}

void P2Station::on_p3(int64_t ref, int step)
{
	if (state_ != ST_P3SETUP) return;
	const int64_t cyc = samples(cyc_s());
	auto miss = [&]() { if (++misses_ > cfg_.timeout_cycles) { disconnect("P3 set-up timeout"); return true; } return false; };
	double st = 0;
	switch (step) {
	case 0: {
		// Fix 243 (a long audio path): the callee's B1 to our previous A1, read whole now. Its B1
		// comes ~0.15 s after the slot (~0.11 s with the default path, fix 235's notes) plus what our
		// own round trip adds; past ~80 ms of round trip too little of it is in by our look, which
		// must come before our next A1 (each A1 then went again, unanswered). The callee answers
		// every A1 that decodes with B1 at the same place, so a B1 read whole one cycle late tells
		// where the B1 to the A1 we just sent lies: answered there (our letter at its reply instant,
		// its data read when it is in). P3_LATE_B1_OFF=1: the old way.
		bool late_b1 = false;
		{
			static const bool late_off = std::getenv("P3_LATE_B1_OFF") != nullptr;
			const int64_t prev = p3_b1_prev_;
			p3_b1_prev_ = ref;
			double st0 = 0;
			if (!late_off && cfg_.realtime && prev >= 0 && ref - prev == cyc
			    && match_words(prev + samples(kP3CalleeReply), p3::kSetupB1, 24, st0,
			                   std::min(prev + samples(kP3CalleeReply + 0.50), ref - int64_t(std::lround(p2::preroll(cfg_.fs))) + cfg_.rx_mute_delay))) {   // (heard up to our A1's blanking)
				st = st0 + double(cyc); late_b1 = true;
				log("P3: B1 to our previous A1 read after our look (a long audio path): answering the B1 to this one, " + std::to_string(int(std::lround(1000.0 * (st0 - double(prev)) / cfg_.fs - 1000.0 * kP3CalleeReply))) + " ms after its slot");
			}
		}
		if (late_b1 || match_words(ref + samples(kP3CalleeReply), p3::kSetupB1, 24, st)) {
			misses_ = 0; p3_b1_prev_ = -1;
			p3_bk_rx(int64_t(st), "B1");
			p3_rx_level_ = letter_level(level_letter(cfg_.p3_level));
			send_cs(level_letter(cfg_.p3_level), reply_at(int64_t(st), false));
			su_fail_ = 0; su_letter_ = level_letter(cfg_.p3_level);
			schedule(int64_t(st) + cyc + samples(kP3Launch), EV_P3_LAUNCH, int64_t(st) + cyc, 2000);
			schedule(int64_t(st) + cyc + samples(p3_su_look()), EV_P3, int64_t(st) + cyc, 1);
		} else if (const int cs = find_psk_cs(ref + samples(kP3CalleeReply), st);
		           !std::getenv("P3_A1_NO_CS") && (cs == p2arq::CS_P || cs == p2arq::CS_S || cs == p2arq::CS_T)) {
			// A callee with nothing to send (an RMS: its modem in host mode,
			// no connect text) acknowledges A1 with a control signal instead
			// of breaking in with B1 (KO0OOO on the air, 2026-10-15: P to
			// every A1 for 90 s while we waited for B1). We are the sender:
			// go on at the level the signal asks for (as the callee does
			// after its B1 is answered); the RMS breaks in with its text.
			log(std::string("P3: A1 answered with ") + p2arq::kCsName[cs] + " (no break-in), sending at level " + std::to_string(letter_level(cs)));
			misses_ = 0;
			p3_level_ = letter_level(cs);
			state_ = ST_ISS; new_send_turn();
			counter_ = 0; expect_ack_ = 1; last_rx_cs_ = cs;
			// As fix 88 (A2): an S answer starts the receiver's speed-up
			// episode, P after two failures = it dropped one level [inf: the
			// same rule; this path is not reachable on the reference unit,
			// whose callee breaks in with B1]. P3_A2_LAST_T=1: off.
			if (cs == p2arq::CS_S && !std::getenv("P3_A2_LAST_T")) { p3_up_fails_ = 0; p3_pre_s_ = p2arq::CS_Q; }
			p3_first_tx_ = false; qrt_end_sent_ = false;
			next_start_ = ref + cyc;
			build_packet(true);
		} else if (!miss()) {
			const auto a1 = setup_a1();
			queue_words(ref + cyc, a1.data(), int(a1.size()));
			schedule(my_start_ + samples(kP3CalleeReply + p3_b1_look()), EV_P3, my_start_, 0);
		}
		return;
	}
	case 1: {
		// Fix 79: in real time the look follows the decode (polled every
		// 2 ms) up to our answer, never giving up before the old 0.92 s look:
		// that look came ~60 ms after our answer was due on the air (KB0TNG
		// 2026-10-17: "send CS P at -61 ms", half the signal cut off).
		if (cfg_.realtime && !std::getenv("P3_LATE_LOOK")) {
			const bool sl1 = p3_rx_level_ == 1;
			const bool ready = sl1 ? (sl1_fut_.valid() && sl1_slot_ == ref && sl1_fut_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
			                       : (p3_fut_.valid() && p3_fut_slot_ == ref && p3_fut_.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
			const int64_t step = samples(0.002);
			if (!ready && n_ + step < std::max(reply_at(ref, false) - samples(0.003), ref + samples(0.92))) { schedule(n_ + step, EV_P3, ref, 1); return; }
		}
		p3::RxPacket r;
		if (p3_rx_level_ == 1) {                       // SL1 greeting: two-carrier receiver
			RxPacket q;
			if (cfg_.realtime && sl1_fut_.valid() && sl1_slot_ == ref && sl1_fut_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) { q = sl1_fut_.get(); sl1_fut_ = {}; }
			else q = find_packet(ref, false, false);
			if (q.ok && q.level == 1) {
				r.ok = true; std::memset(r.r.info, 0x1e, sizeof r.r.info);
				for (int i = 0; i < p3::kSl1Data; i++) r.r.info[i] = q.info[size_t(i)];
				r.r.info[p3::kDataBytes] = q.info[size_t(p3::kSl1Data)];
				r.t0 = q.start + 0.8 * T_ + kP3Off * cfg_.fs;
			}
		} else r = find_p3(ref, 2.0);
		if (r.ok) {
			misses_ = 0;
			deliver(r.r.info, p3::level_data_bytes(p3_rx_level_), r.r.info[p3::kDataBytes]);
			if (!early_old() && !(r.r.info[p3::kDataBytes] & 0x40)) {
				// The callee's first P3 packet without "over": its first turn goes on with the text it had
				// queued (the reference unit's callee, emulated: status 0x27, its whole text, then over).
				// The reference unit's caller acknowledges it with Q and receives; we did the same as for an
				// "over" (A2: we took the link and the rest of its text waited for its operator's Ctrl-Y).
				const int64_t s0 = p3_slot(r);
				send_cs(p2arq::CS_Q, reply_at(s0, false));
				log("P3: the callee's first packet has no over: receiving its text (Q)");
				p3_run_ = 1; p3_q_ = -1;
				state_ = ST_IRS;
				ack_phase_ = 1; last_ack_cs_ = p2arq::CS_Q;   // Q given: the next new packet P
				last_rx_counter_ = 3; rx_fails_ = 0; good_run_ = 0; rx_t_state_ = false;
				schedule(s0 + cyc + samples(kP3Launch), EV_P3_LAUNCH, s0 + cyc, 2000);
				schedule(s0 + cyc + samples(0.88), EV_PACKET, s0 + cyc);
				return;
			}
			bk_tx_.clear();
			const int64_t at = queue_words(reply_at(p3_slot(r), false), my_bk(false), 82);   // (fix 257: where it went)
			schedule(caller_look_at(at), EV_P3, at, 2);
		} else if (!miss()) {
			// Fix 69: the receiver's rule after its answer to a break-in
			// (protocol notes §4.12): the same letter on a failure; after 2
			// failures P and one level lower (SL4 after T, SL2 after S); then
			// as any receiver, T and one level lower after every 5 more. We
			// sent T for ever and kept expecting SL5 while the callee stepped
			// down (emulator, noise 3000 from the start: 45 T, link lost).
			++su_fail_;
			if (std::getenv("P3_SU_ALWAYS_T")) su_letter_ = p2arq::CS_T;
			else if (su_fail_ == 2 && p3_rx_level_ > 1) {
				p3_rx_level_ = p3_rx_level_ == 5 ? 4 : p3_rx_level_ == 3 ? 2 : p3_rx_level_ - 1;
				su_letter_ = p2arq::CS_P;
				log("P3: set-up: no packet after our answer, P, now expecting level " + std::to_string(p3_rx_level_));
			} else if (su_fail_ > 2 && (su_fail_ - 2) % kP3TDown == 0 && p3_rx_level_ > 1) {
				p3_rx_level_--;
				su_letter_ = p2arq::CS_T;
				log("P3: set-up: still no packet, T, now expecting level " + std::to_string(p3_rx_level_));
			}
			send_cs(su_letter_, reply_at(ref, false));          // the same CS = send it again
			schedule(ref + cyc + samples(kP3Launch), EV_P3_LAUNCH, ref + cyc, 2000);
			schedule(ref + cyc + samples(p3_su_look()), EV_P3, ref + cyc, 1);
		}
		return;
	}
	case 2: {
		const int64_t a2x = ref + samples(kP3CalleeReply);
		if (p3_look_async() && !p3_look(a2x, false, false, 0, p3_look_deadline(ref + cyc), EV_P3, ref, step)) return;   // fix 250
		const int cs = p3_look_async() ? p3_look_cs(a2x, st, 0) : find_psk_cs(a2x, st);
		// The answer to A2 sets our first level: T = 5 (2400 Bd), S = 3,
		// P = 1 (we lack 2-4: S -> 1).
		// Fix 268: Q is the receiver's repeat request (its A2 data did not decode), not a level: the reference
		// unit (emulator, our A2 spoilt) answered Q and kept answering Q to every packet we sent after it, nothing
		// printed. Our A2 goes again, unchanged. P3_A2_Q_OLD=1: Q taken as level 5 as before.
		static const bool a2_q_old = std::getenv("P3_A2_Q_OLD") != nullptr;
		if (cs == p2arq::CS_Q && !a2_q_old) log("P3: A2 answered with Q (not taken): our A2 again (fix 268)");
		if (cs == p2arq::CS_T || cs == p2arq::CS_S || cs == p2arq::CS_P || (cs == p2arq::CS_Q && a2_q_old) || cs == p2arq::CS_R) {
			p3_level_ = letter_level(cs);
			if (const char *e = std::getenv("P3_TX_LEVEL")) p3_level_ = std::atoi(e);   // experiment hook: force our sending level
			// Fix 268: R is SL1 long, as for a break-in's answer (the reference unit answered our repeated A2 with R
			// in the emulator; sent short, nothing was taken). P3_A2_Q_OLD=1: short as before.
			if (cs == p2arq::CS_R && !a2_q_old && !std::getenv("P3_NO_LONG") && p3::long_payload(1) > 0) p3_long_ = true;
			log("P3: A2 answered with " + std::string(1, p2arq::kCsName[cs]) + ", level " + std::to_string(p3_level_) + (p3_long_ ? " (long)" : ""));
			state_ = ST_ISS; new_send_turn(); misses_ = 0;
			counter_ = 2;                                   // the first P3 packet is counter 3
			expect_ack_ = 1;                                // then Q, P, Q ...
			if (cs == p2arq::CS_R && !a2_q_old) expect_ack_ = 0;   // fix 268: after R the acknowledgement is P
			// The receiver's last letter is its A2 answer: the same letter
			// again is a repeat request (2026-10-19: with T hard-coded, an
			// A2 answered with S followed by S for our first packet read as
			// "acknowledged + speed up"; under fading that packet was lost,
			// 101 characters missing at the firmware). P3_A2_LAST_T=1: old.
			last_rx_cs_ = std::getenv("P3_A2_LAST_T") ? p2arq::CS_T : cs;
			// A2 answered with S starts a speed-up episode at the reference
			// unit as receiver, as our S does: a failed packet is
			// answered S again, after 2 failures it drops one level and
			// answers P, a good packet Q (as after A2 = T). Emulator, CCIR
			// moderate fading: the reference unit failed our first SL3 packet twice, dropped to SL2
			// and answered P; we took the P as an acknowledgement and the
			// packet (101 characters) was lost (2026-10-19).
			if (cs == p2arq::CS_S && !std::getenv("P3_A2_LAST_T")) { p3_up_fails_ = 0; p3_pre_s_ = p2arq::CS_Q; }
			// Fix 267: A2 answered with T, then T for our first packet: a repeat request, as after a
			// break-in answered T (fix 48/265: the 5th T in a row lowers us one level, P lowers one level,
			// Q acknowledges). Emulator (the emulated reference unit): as sender it repeats the same SL5
			// packet on that T and takes the receiver's Q that follows as the acknowledgement; as receiver
			// it answered our SL4 rebuild with P and dropped the packet's characters (83 lost, acknowledged).
			// Speeding down on that T deadlocked two Signums on the air (f2a5, f2b2: Q read as a repeat
			// request for 30 cycles). P3_A2_T_OLD=1: as before (the T rule: one level down at once).
			if (cs == p2arq::CS_T && !std::getenv("P3_A2_T_OLD") && !std::getenv("P3_A2_LAST_T")) { p3_ans_cs_ = cs; p3_ans_rep_ = 0; }
			next_start_ = ref + cyc;
			log("P3: sending");
			build_packet(true);
		} else if (!miss()) {
			queue_words(ref + cyc, my_bk(false), 82);
			schedule(caller_look_at(my_start_), EV_P3, my_start_, 2);
		}
		return;
	}
	case 20: {                                      // our break-in: the letter sets our level
		// Our break-in answered by a break-in: the other side keeps (or takes
		// back) the link, e.g. the RMS ending it as we break in to QRT after
		// the CMS's FQ (K0NTS, 40 m, 2026-10-15 17:39: its break-in every
		// cycle, ours repeated until Pat gave up). Receive it (fix 44).
		const int64_t bkx = ref + samples(master_ ? kP3CalleeReply : master_seen_s());
		const bool la = p3_look_async();            // fix 250
		if (la && !p3_look(bkx, true, false, 0, p3_look_deadline(ref + cyc), EV_P3, ref, step)) return;
		if (la ? p3_look_bk(bkx, st) : looks_like_bk(bkx, st)) {
			log("P3: our break-in answered by a break-in");
			my_start_ = ref;                        // its answer lattice counts from our burst
			pending_used_ = 0; bk_tx_.clear();      // our break-in's characters were not taken: keep them queued
			take_p3_bk(st);
			return;
		}
		int cs = la ? p3_look_cs(bkx, st, 0) : find_psk_cs(bkx, st);
		// Test hook (fix 265): P3_TEST_BK_MISS=N, the first N answers to our P3 break-ins taken as nothing heard
		// (the on-air misread of the reference unit's first answer).
		static int p3_bk_miss = std::getenv("P3_TEST_BK_MISS") ? std::atoi(std::getenv("P3_TEST_BK_MISS")) : 0;
		if (p3_bk_miss > 0 && cs != p2arq::CS_NONE) { p3_bk_miss--; log(std::string("test: answer ") + p2arq::kCsName[cs] + " to our break-in taken as nothing heard"); cs = p2arq::CS_NONE; }
		// The reference unit as receiver answers a break-in with T (SL5) or S (SL3)
		// when it was received well, else with its alternation letter,
		// P (SL1), or R (SL1, long). Its Q is
		// a repeat request (the break-in not taken), not "SL5".
		// Sending SL5 on N5TW's Q (30 m,
		// 2026-10-15) got Q for 40 s and no transfer. Acknowledgements
		// then: after T/S and P, Q; after R, P.
		// Fix 265: an answer we did not read. The receiver took our first break-in and answered it (T or S);
		// our break-in sent again is then a failed packet at that level, answered with the same letter again,
		// and its P that follows means "one level lower, again" (as after the answer, fix 48 above), not
		// "SL1": emulator, our first two answers taken as nothing heard (P3_TEST_BK_MISS=2): T T P, and only
		// level 4 was acknowledged (Q P Q P; SL1 got P P T T T ... for ever, the on-air stall of s2s-pcall
		// a, a.2, d, e.3); S S P: level 2 (R P Q ...). So a P after a missed answer sends at the missed
		// letter's level less one: what the first missed look read (hard >= 0.6, else soft >= 0.3), T or S; T when
		// none read anything usable; a P read there is the receiver's own P (SL1, as before; emulator T T P P:
		// level 4 acknowledged, S S P P: level 2). P3_BK_P_OLD=1: SL1 always.
		bool lv_guessed = false;                    // fix 276: the level below is fix 265's guess, not a letter read
		auto bk_letter_level = [&](int c) {
			if (c == p2arq::CS_P && p3_bk_tries_ > 0 && !std::getenv("P3_BK_P_OLD")) {
				const int g = p3_bk_guess_;
				const int lv = g == p2arq::CS_P ? 1 : g == p2arq::CS_S ? 2 : 4;
				log(std::string("P3: P after ") + std::to_string(p3_bk_tries_) + " unread answer" + (p3_bk_tries_ > 1 ? "s" : "") + " to our break-in (read there: " +
				    (g >= 0 ? std::string(1, p2arq::kCsName[g]) : std::string("nothing")) + "): level " + std::to_string(lv) + (lv > 1 ? ", the missed answer's less one (fix 265)" : " (fix 265)"));
				lv_guessed = lv > 1;
				return lv;
			}
			return c == p2arq::CS_T ? 5 : c == p2arq::CS_S ? 3 : 1;
		};
		const bool peer_pk = cs == p2arq::CS_NONE && p3_bk_peer_packet(bkx);   // fix 277: not an answer: our break-in not taken
		if (cs == p2arq::CS_NONE && !peer_pk) {
			const int g = look_hard_cs_ >= 0 && look_score_ >= 0.6 ? look_hard_cs_ : look_soft_cs_ >= 0 && look_soft_ >= 0.3 ? look_soft_cs_ : -1;
			if (p3_bk_guess_ < 0 && (g == p2arq::CS_T || g == p2arq::CS_S || g == p2arq::CS_P)) p3_bk_guess_ = g;   // the first usable one: the answer itself (later P's are its "lower again")
		}
		if (cs == p2arq::CS_T || cs == p2arq::CS_S || cs == p2arq::CS_P || cs == p2arq::CS_R) {
			p3_level_ = bk_letter_level(cs);
			p3_long_ = cs == p2arq::CS_R && !std::getenv("P3_NO_LONG") && p3::long_payload(1) > 0;
			log("P3: break-in answered with " + std::string(1, p2arq::kCsName[cs]) + ", sending at level " + std::to_string(p3_level_) + (p3_long_ ? " (long)" : ""));
			state_ = ST_ISS; new_send_turn(); misses_ = 0;
			if (lv_guessed) p3_guess_rep_ = 0;      // fix 276
			counter_ = 2;
			expect_ack_ = cs == p2arq::CS_R ? 0 : 1;
			last_rx_cs_ = cs == p2arq::CS_P || cs == p2arq::CS_R ? cs : p2arq::CS_T;
			if (cs == p2arq::CS_T || cs == p2arq::CS_S) { p3_ans_cs_ = cs; p3_ans_rep_ = 0; }
			p3_first_tx_ = true; qrt_end_sent_ = false;
			next_start_ = ref + cyc;
			build_packet(true);
		} else if (!miss()) {
			if (cs == p2arq::CS_Q) log("P3: break-in answered with Q (repeat request), sending it again");
			// fix 265: a Q is "not taken": its next answer is a first one; fix 277: so is the peer's data packet
			if (cs == p2arq::CS_NONE && !peer_pk) p3_bk_tries_++; else { p3_bk_tries_ = 0; p3_bk_guess_ = -1; }
			queue_words(ref + cyc, my_bk(false), 82);
			premod_after_bk();                      // fix 254
			schedule(master_ ? caller_look_at(ref + cyc) : callee_look_at(ref + cyc), EV_P3, ref + cyc, 20);
		}
		return;
	}
	case 10: {
		int64_t a1 = ref;
		static int a1_nomatch_n = 0;                  // test hook (fix 213c): P3_TEST_A1_NOMATCH=N, our first N A1 header matches fail
		const bool a1_nomatch = std::getenv("P3_TEST_A1_NOMATCH") && a1_nomatch_n++ < std::atoi(std::getenv("P3_TEST_A1_NOMATCH"));
		if (!a1_nomatch && match_words(ref, p3::kSetupA1, 9, st)) a1 = int64_t(st);   // the SL1 header (its data is the caller's text)
		else {
			if (debug_) log("P3: A1 not matched");
			// TODO 0e: the caller may not have heard our P3-accept and send its
			// P1 set-up packet again in this slot (its first bit 13.5 ms before
			// the A1 slot). The firmware through CCIR poor fading repeated it
			// for 90 s while we, past it, answered B1 to nothing. Look for it
			// once it is in (case 13), before our B1 is due.
			// P3_NO_SETUP_AGAIN: the old way (test).
			if (!std::getenv("P3_NO_SETUP_AGAIN")) { schedule(ref + samples(0.955), EV_P3, ref, 13); return; }
		}
		// Fix 218: B1 only for an A1 that decodes. A header match alone is no confirmation:
		// through CCIR poor fading the A1 header matched in the reference unit's repeated P1 set-up
		// packet (its caller had lost our P3-accept), we sent B1, and then B1 every cycle to
		// a caller still at level 1 (a compatibility run, CCIR poor, P3). The decode is made
		// once the packet is in (step 14); a failed one goes on as no A1 (step 13: the set-up
		// packet again, else P3-accept again). P3_SETUP_B1=1: the old way.
		if (!std::getenv("P3_SETUP_B1") && !std::getenv("P3_A1_CS")) {
			p3_a1_at_ = a1;
			schedule(std::max(a1 + samples(a1_async() ? 0.84 : 0.86), n_), EV_P3, ref, 14);   // (fix 252: on a worker from 0.84 s)
			return;
		}
		schedule(a1 + samples(0.86), EV_BK_DATA, a1, 3);
		// Experiment hook P3_A1_CS=P|Q|R|S|T: answer A1 with that control
		// signal instead of the B1 break-in, as some Winlink RMS do (on the
		// air 2026-10-15: K0NTS answers T, KO0OOO P or S),
		// to see what the reference V4.1r caller does with each.
		if (const char *e = std::getenv("P3_A1_CS")) {
			const char *q = std::strchr(p2arq::kCsName, e[0]);
			const int a1cs = q ? int(q - p2arq::kCsName) : p2arq::CS_T;
			send_cs(a1cs, a1 + samples(kP3CalleeReply - cfg_.rt_latency_s));
			log(std::string("P3: A1 answered with ") + p2arq::kCsName[a1cs] + " (experiment), receiving at level " + std::to_string(letter_level(a1cs)));
			p3_rx_level_ = letter_level(a1cs);
			state_ = ST_IRS;
			ack_phase_ = 0; last_ack_cs_ = p2arq::CS_T; last_rx_counter_ = -1; rx_fails_ = 0; good_run_ = 0;
			schedule(a1 + cyc + samples(kP3Launch), EV_P3_LAUNCH, a1 + cyc, 2000);
			schedule(a1 + cyc + samples(0.88), EV_PACKET, a1 + cyc);
			return;
		}
		bk_tx_.clear(); b1_reset();
		queue_words(a1 + samples(kP3CalleeReply - cfg_.rt_latency_s), my_bk(true), 82);
		schedule(callee_look_at(my_start_), EV_P3, my_start_, 11);
		return;
	}
	case 15:                                         // fix 235: the A2 slot `ref` decoded as a break-in
		a2_decode(ref);
		return;
	case 14: {                                       // fix 218: the A1 whose header matched at p3_a1_at_ (slot `ref`)
		const int64_t a1 = p3_a1_at_;
		int64_t b1 = a1 + samples(kP3CalleeReply - cfg_.rt_latency_s);
		RxPacket q;
		if (a1_async()) {                            // fix 252: decoded on a worker
			const int64_t dl = b1 - int64_t(std::lround(p2::preroll(cfg_.fs))) - samples(0.003);
			if (a1d_slot_ != a1 || !a1d_fut_.valid()) { a1d_slot_ = a1; a1d_late_ = false; a1d_fut_ = submit_sl1(a1); }
			if (a1d_fut_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
				if (!a1d_late_ && n_ + samples(0.002) >= dl) {
					a1d_late_ = true;
					log("P3: A1 decode not done by our B1: if it decodes, B1 answers the caller's next A1 (fix 252)");
				}
				schedule(n_ + samples(0.002), EV_P3, ref, 14);
				return;
			}
			q = a1d_fut_.get(); a1d_fut_ = {}; a1d_slot_ = -1;
			if (a1d_late_ || n_ > dl) b1 += samples(cyc_s());   // the caller sends A1 again on its lattice when no B1 came
		} else q = find_packet(a1, false, false);
		if (q.ok && q.level == 1) {
			if (debug_) log("P3: A1 decoded, status " + std::to_string(q.info[size_t(p3::kSl1Data)]));
			deliver(q.info, p3::kSl1Data, q.info[size_t(p3::kSl1Data)]);
			bk_tx_.clear(); b1_reset();
			queue_words(b1, my_bk(true), 82);
			schedule(callee_look_at(my_start_), EV_P3, my_start_, 11);
			return;
		}
		log("P3: A1 header but no good A1: not confirmed");
		schedule(std::max(ref + samples(0.955), n_), EV_P3, ref, 13);
		return;
	}
	case 13: {                                       // A1 not found at `ref`: the caller's P1 set-up packet again?
		const int64_t exp = ref - samples(0.0135);
		uint8_t bytes[24];
		int64_t t1 = 0;
		if (p1_setup_at(exp, t1, bytes) && (bytes[21] & 0x20) && cfg_.allow_p3) {
			log("P3: the caller sent its set-up packet again: P3-accept again");
			events_.clear();
			queue_accept(t1 + samples(kP1Reply - cfg_.rt_latency_s + test_call_answer_s()), p3::kP3Accept);
			acc_first_ = tx_.back().start;                // fix 222: the set-up packet read again: the grid restarts from this accept
			schedule(t1 - samples(0.0225) + samples(cyc_s() + 0.036) + samples(0.86), EV_P3, t1 - samples(0.0225) + samples(cyc_s() + 0.036), 10);
			return;
		}
		// Fix 218: neither A1 nor the set-up packet. The switch to P3 is not confirmed
		// until the caller's A1 is taken: the reference unit as callee answers every slot
		// without it with its P3-accept again, also a slot it heard nothing in, and never sends
		// B1 to a caller that may still be at level 1 (reference-unit pairs through the channel relay
		// with replies cut, 2026-11-11: its accept cut twice, then a third, then once more for
		// the caller's first A1; the caller's repeated set-up packet cut: P3-accept again into
		// the silence). Our B1 instead (on the expected slot, for ever) left a caller that had
		// lost our accept in a fade repeating its set-up packet at level 1 for the whole link,
		// our B1 covering the end of every copy (a compatibility run, CCIR poor, P3: 0 of 2,421).
		// The accept goes at the reply time of the set-up packet the slot would hold.
		// P3_SETUP_B1=1: the old way (B1 on the expected slot).
		// Fix 213c: the A1 decoded where its header did not match. match_words looks for the
		// set-up words from 40 ms before to 20 ms after the expected slot; on the bench the
		// the reference unit's first A1 arrived outside that (f216-15: "A1 not matched",
		// then the same packet decoded by find_packet, whose timing comes from the burst), so
		// fix 218 took a sent A1 for none and answered it with P3-accept again: the reference unit never
		// answered the B1 that followed (the same run: 90 s, P3 set-up timeout). A good
		// SL1 decode of the slot is the caller's A1: B1 at its own start. P3_A1_NO_DECODE: off.
		// Fix 271 (on the air u1-3, u6-6, the third session's e: 3 of ~34 unit calls): this look comes at the
		// slot + 0.955 s, 70 ms after B1's instant (+0.8848 s less our round trip), so that B1 went out
		// 85 ms late: whole (fix 257) or, since fix 269, cut. The reference unit answered the late whole B1
		// on its own lattice: its answer began under our late B1 and the part heard was no letter, for 50 s
		// (measured from the recordings: no timing or offset reads it). And the decode itself ran on the
		// audio loop (45 ms: a playback underrun). Now the decode runs on a worker (case 16) and a B1
		// whose instant has passed answers the caller's next A1, on time, as fix 252 does for a matched A1
		// decoded late (the caller sends its A1 again on its lattice when no B1 came).
		// P3_SETUP_LATE_OLD=1: as before (decoded here, B1 at its own instant whatever the time).
		if (!std::getenv("P3_A1_NO_DECODE")) {
			if (!setup_late_old() && cfg_.realtime && pool_.size() > 0) {
				a1n_slot_ = ref;
				a1n_fut_ = pool_.submit([this, ref, sp = snapshot_short(ref)]() { return find_packet(ref, false, false, &sp); });
				schedule(n_ + samples(0.002), EV_P3, ref, 16);
				return;
			}
			if (a1_nomatch_decoded(find_packet(ref, false, false), ref)) return;
		}
		a1_none(ref);
		return;
	}
	case 16: {                                       // fix 271: the A1 decoded on a worker without a header match (case 13)
		if (a1n_slot_ != ref || !a1n_fut_.valid()) return;
		if (a1n_fut_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { schedule(n_ + samples(0.002), EV_P3, ref, 16); return; }
		const RxPacket q = a1n_fut_.get(); a1n_fut_ = {}; a1n_slot_ = -1;
		if (state_ != ST_P3SETUP) return;
		if (!a1_nomatch_decoded(q, ref)) a1_none(ref);
		return;
	}
	case 11: {
		// Fix 235: B1 again after a greeting (fix 210's soft-answer path): the caller may have taken
		// the greeting and be repeating its A2, which we did not match (f232-2: 35 B1 into its A2
		// for 46 s). Its repeat, decoded the cycle before, or matched here, is answered.
		if (greet_sent_ && !std::getenv("P3_NO_A2_WATCH") && match_a2(ref + samples(master_seen_s()), st)) {
			log("P3: A2 after B1 again: the greeting had been taken (fix 235)");
			take_setup_a2(int64_t(st));
			return;
		}
		int cs = find_psk_cs(ref + samples(master_seen_s()), st);
		// Test hooks (fix 210): P3_TEST_B1_ANS_MISS=N: the caller's first N answers to B1 taken as
		// nothing heard; P3_TEST_B1_ANS_WEAK=N: read as no letter with a soft reading no better
		// than 0.50 / margin 0.50 (RF bench rb8-4: hard 0.67-0.76, soft T 0.36-0.53).
		if (const char *e = std::getenv("P3_TEST_B1_ANS_MISS")) { static int n = 0; if (n++ < std::atoi(e)) { if (debug_) log("P3: test: B1 answer ignored"); cs = -1; look_soft_cs_ = -1; } }
		if (const char *e = std::getenv("P3_TEST_B1_FAKE_T")) { static int n = 0; if (n++ < std::atoi(e) && cs < 0) { if (debug_) log("P3: test: soft T made up"); look_soft_cs_ = p2arq::CS_T; look_soft_ = 0.6; look_margin_ = 0.6; } }
		if (const char *e = std::getenv("P3_TEST_B1_ANS_WEAK")) { static int n = 0; if (n++ < std::atoi(e)) { if (debug_) log("P3: test: B1 answer weak"); cs = -1; look_soft_ = std::min(look_soft_, 0.50); look_margin_ = std::min(look_margin_, 0.50); } }
		// The caller acknowledges B1 with a speed request (T from the
		// firmware's own distorted signal; S/P from ours). We only have the
		// 2400 Bd level so far: accept any of them and send at that level.
		// A poor B1 is answered with the caller's alternation letter (fix 42:
		// SL1); from the reference firmware that is R (a P3 caller 30 Hz off:
		// R every cycle, and we resent B1 until the set-up timed out).
		// P3_B1_NO_R: the old refusal (test).
		if (cs < 0 && a2_quiet(ref + samples(master_seen_s()))) return;   // fix 235 (only once a greeting went)
		static const bool no_r = std::getenv("P3_B1_NO_R") != nullptr;
		// Fix 210 (RF bench rb8-4: the real reference unit's T to our first B1 read at hard 0.67 /
		// soft T 0.36, the next at soft T 0.53; we sent B1 twice more, its fall-back P was
		// taken as "level 1", and the set-up timed out). The caller is silent until it has
		// taken a B1, so a soft letter means it took it, and every B1 sent after that is a
		// failed packet to it: after two, it answers P and expects one level lower (SL4 after
		// T, SL2 after S; emulator, P3_TEST_B1_ANS_MISS). So: (1) a look with no letter but a
		// clear soft T, S or R is taken as the answer (greeting sent, not B1; if the caller
		// was in fact still waiting, the next look is silent and B1 goes again); (2) a P after
		// our B1 went again is that fall-back, not SL1. P3_B1_NO_GUESS=1: the old way.
		static const bool no_guess = std::getenv("P3_B1_NO_GUESS") != nullptr;
		const bool soft_ans = look_soft_cs_ == p2arq::CS_T || look_soft_cs_ == p2arq::CS_S || (look_soft_cs_ == p2arq::CS_R && !no_r);
		if (cs < 0 && soft_ans && look_soft_ >= 0.30 && look_margin_ >= 0.20) b1_soft_ = look_soft_cs_;
		bool guess = false;
		if (cs < 0 && !no_guess && soft_ans && look_soft_ >= 0.45 && look_margin_ >= 0.40) {
			cs = look_soft_cs_; guess = true;
			log(std::string("P3: B1 answer read as soft ") + p2arq::kCsName[cs] + " only: taken (fix 210)");
		}
		if (cs == p2arq::CS_P && b1_resends_ > 0 && !no_guess) {
			const int path = b1_soft_ == p2arq::CS_S ? p2arq::CS_S : p2arq::CS_T;
			misses_ = 0;
			greet_ep_cs_ = -1; greet_fails_ = 0; greet_t_run_ = 0; greet_long_ = false;
			// no soft letter: after T (SL4) or after S (SL2), tried in turn
			fb_start(b1_soft_ < 0 || path == p2arq::CS_T, b1_soft_ < 0 || path == p2arq::CS_S);
			greet_skip_p_ = true;                 // a greeting right after a B1 its answer repeated can fail at any level (emulator)
			log("P3: B1 answered with P after " + std::to_string(b1_resends_) + " B1 again: its fall-back from " + p2arq::kCsName[path] + ", sending at level " + std::to_string(p3_level_) + " (fix 210)");
			const int64_t slot = ref + cyc;
			send_greeting(slot);
			my_start_ = slot;
			schedule(callee_look_at(slot), EV_P3, slot, 12);
		} else if (cs == p2arq::CS_T || cs == p2arq::CS_S || cs == p2arq::CS_P || (cs == p2arq::CS_R && !no_r)) {
			log(std::string("P3: B1 answered with ") + p2arq::kCsName[cs] + ", sending at level " + std::to_string(letter_level(cs)));
			misses_ = 0;
			p3_level_ = letter_level(cs);
			greet_ep_cs_ = cs; greet_fails_ = 0; greet_t_run_ = 0;
			fb_[0].alive = fb_[1].alive = false; greet_guess_ = guess; greet_scan_ = -1;
			// R: SL1 long, as after a data-phase break-in. The firmware callee
			// sends its greeting as one 3.32 s burst on the 3.75 s cycle and the
			// caller's A2 follows a long cycle later (emulated firmware pair
			// 30 Hz apart, 2026-10-20) [bb-ext].
			greet_long_ = cs == p2arq::CS_R && !no_r && p3::long_payload(1) > 0;
			const int64_t slot = ref + cyc;
			send_greeting(slot);
			my_start_ = slot;
			const int64_t a2ref = slot + (greet_long_ ? samples(lcyc_s() - cyc_s()) : 0);   // A2's lattice: the end of our (long) cycle
			schedule(callee_look_at(a2ref), EV_P3, a2ref, 12);
		} else if (!miss()) {
			b1_resends_++;
			queue_words(ref + cyc, my_bk(true), 82);
			schedule(callee_look_at(my_start_), EV_P3, my_start_, 11);
		}
		return;
	}
	case 12: {
		if (match_a2(ref + samples(master_seen_s()), st)) {
			take_setup_a2(int64_t(st));
		} else {
			// No A2: the caller did not take our greeting. Its letters follow
			// the speed-change episode its B1 answer started (as fix 88): the
			// same letter = failed once at the level it set; P after two
			// failures = it dropped one level; later T = step down further
			// (first T at once, then every 6th, as in the data phase). Until
			// 2026-10-19 we resent at the B1 level for ever (emulator,
			// Watterson fading seed 11: it answered T, P, P, P, T, T ... and
			// the set-up timed out). P3_GREET_NO_STEP=1: old.
			double st2 = 0;
			const int cs2 = find_psk_cs(ref + samples(master_seen_s()), st2);
			if (debug_) log(std::string("P3: set-up step 12: no A2, control signal ") + (cs2 >= 0 ? std::string(1, p2arq::kCsName[cs2]) : std::string("-")));
			if (greet_more_ && cs2 == p2arq::CS_Q) {
				// Our greeting carried queued text and had no "over": the caller (the reference unit) takes it
				// with Q, not A2 (emulated, scs-modem TODO v). We go on sending, as its callee does, and hand over
				// by ourselves once the queue has run dry (its callee's first turn needs no Ctrl-Y).
				greet_taken();
				log("P3: greeting taken (Q): our first turn goes on with the queued text, over when it has gone");
				state_ = ST_ISS; new_send_turn(); misses_ = 0;
				counter_ = 3;                               // the greeting was counter 3: next 0
				expect_ack_ = 0; last_rx_cs_ = p2arq::CS_Q;  // Q took it: P next
				cho_req_ = true;
				p3_long_ = greet_long_;
				p3_first_tx_ = false; qrt_end_sent_ = false;
				next_start_ = ref + cyc;
				build_packet(true);
				return;
			}
			if (cs2 < 0 && a2_quiet(ref + samples(master_seen_s()))) return;   // fix 235
			// Fix 210: the B1 answer was a soft reading only and the caller is silent: it was
			// still waiting for B1 (it is silent until it takes one). Send B1 again.
			const bool was_guess = greet_guess_, skip_p = greet_skip_p_;
			greet_guess_ = false; greet_skip_p_ = false;
			if (was_guess && cs2 < 0 && !(look_soft_cs_ == greet_ep_cs_ && look_soft_ >= 0.30)) {
				log("P3: no letter after a greeting sent on a soft B1 answer: B1 again (fix 210)");
				if (!miss()) {
					b1_resends_ = 0; b1_soft_ = -1;
					queue_words(ref + cyc, my_bk(true), 82);
					schedule(callee_look_at(my_start_), EV_P3, my_start_, 11);
				}
				return;
			}
			static const bool old_step = std::getenv("P3_GREET_OLD_STEP") != nullptr;   // fix 210: the old rules (test)
			if (!std::getenv("P3_GREET_NO_STEP") && old_step && p3_level_ > 1) {
				if (greet_ep_cs_ >= 0 && cs2 == greet_ep_cs_) {
					greet_fails_++;                         // the episode: failed at its level
				} else if (greet_ep_cs_ >= 0 && cs2 == p2arq::CS_P && greet_fails_ >= 1) {
					p3_level_--; greet_ep_cs_ = -1;         // it dropped one level
					log("P3: greeting not taken, now sending at level " + std::to_string(p3_level_));
				} else if (greet_ep_cs_ < 0 && cs2 == p2arq::CS_T) {
					if (greet_t_run_++ % 6 == 0) { p3_level_--; log("P3: greeting: T, now sending at level " + std::to_string(p3_level_)); }
				}
			} else if (!std::getenv("P3_GREET_NO_STEP") && !old_step) {
				// Fix 210, the caller's receiver after its T or S answer (emulator, every
				// level forced): a failed packet is answered
				// with the same letter; the second with P, and it then expects one level
				// lower (SL4 after T, SL2 after S); failures there are answered P, P, then T:
				// at the first T it expects one level lower again, then one lower every 5th
				// T. After T it never went below SL2 (SL1 never taken in 60 s); after S, SL1
				// at the first T. A B1 sent again after it took one counts as a failure, and
				// so can the greeting right after it (then the P comes one cycle early).
				// Before: P was a fall-back only after a repeat of the same letter, and with
				// the episode letter set no T stepped down (the reference unit: T taken, B1 sent
				// again: T, P, P, T ... at SL5 for ever).
				if (greet_scan_ >= 0) {
					if (cs2 >= 0) greet_scan_next("");     // still not taken: the next level
				} else if ((greet_ep_cs_ == p2arq::CS_T || greet_ep_cs_ == p2arq::CS_S) && cs2 == greet_ep_cs_) {
					// the same letter twice after its answer: no fall-back P came, so that letter was
					// not its answer to our B1 (a T run already going: the reference unit, our B1 sent 6 times)
					if (++greet_fails_ >= 2) greet_scan_next("the same letter again, no fall-back");
				} else if ((greet_ep_cs_ == p2arq::CS_T || greet_ep_cs_ == p2arq::CS_S) && cs2 == p2arq::CS_P) {
					const bool t = greet_ep_cs_ == p2arq::CS_T;
					greet_ep_cs_ = -1; greet_t_run_ = 0;
					fb_start(t, !t);
					log("P3: greeting not taken (P: its fall-back from " + std::string(t ? "T" : "S") + "), now sending at level " + std::to_string(p3_level_));
				} else if (greet_ep_cs_ < 0 && fb_on()) {
					const int sent = p3_level_;
					fb_letter(cs2, sent, !skip_p);
					// (One model left and still refused at its level: kept, as a noisy channel
					// gives the same letters; no sweep here.)
					if (p3_level_ != sent || debug_) log("P3: greeting not taken (" + std::string(cs2 >= 0 ? std::string(1, p2arq::kCsName[cs2]) : "-") + "), now sending at level " + std::to_string(p3_level_) + (fb_[0].alive && fb_[1].alive ? " (after T or S: both tried)" : ""));
				} else if (greet_ep_cs_ < 0 && cs2 == p2arq::CS_T && p3_level_ > 1) {
					if (greet_t_run_++ % 5 == 0) { p3_level_--; log("P3: greeting: T, now sending at level " + std::to_string(p3_level_)); }
				}
			}
			if (!miss()) {
				send_greeting(ref + cyc);                   // our P3 packet again (long: ref is the end of our long cycle)
				my_start_ = ref + cyc;
				const int64_t a2ref = my_start_ + (greet_long_ ? samples(lcyc_s() - cyc_s()) : 0);
				schedule(callee_look_at(a2ref), EV_P3, a2ref, 12);
			}
		}
		return;
	}
	}
}

}  // namespace fmodem
