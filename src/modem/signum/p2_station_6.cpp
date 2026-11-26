// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// P2Station member functions, part 6 of 8 (split out of p2_station.hpp so they build in parallel).
#include "p2_station.hpp"

namespace fmodem {

double P2Station::cs_ref_s() const
{
	static const bool old = std::getenv("P2_CS_REF_OLD") != nullptr;
	static const double bias = std::getenv("P2_CS_BIAS_MS") ? std::atof(std::getenv("P2_CS_BIAS_MS")) / 1000.0 : kP2CsBias;
	if (old || master_ || p3_) return 0.0;
	return bias - (double(cfg_.rt_latency_s) + double(cfg_.reply_adjust_s));
}

void P2Station::p2_up_result(bool taken)
{
	if (taken) { up_miss_run_ = 0; early_seen_ = false; return; }
	const int n = up_miss_step();
	if (n <= 0 || p3_ || hunt_old() || hunt_k_ > 0 || up_miss_steps_ >= kUpMissMax || ++up_miss_run_ < n) return;
	up_miss_run_ = 0; up_miss_steps_++; early_seen_ = true;
	hunt_base_ = std::min(0.02, hunt_base_ + up_miss_late()); hunt_s_ = hunt_base_;
	p2_backoff_[0] = p2_backoff_[1] = 1;
	log("P2: requests not taken " + std::to_string(n) + " times while acknowledgements work: our answers now " + std::to_string(hunt_s_ * 1e3) + " ms from nominal (later)");
}

int64_t P2Station::test_cs_shift() const
{
	static std::vector<std::pair<double, double>> v = [] {
		std::vector<std::pair<double, double>> r;
		if (const char *e = std::getenv("P2_TEST_CS_SHIFT"))
			for (const char *p = e; p && *p; p = std::strchr(p, ',') ? std::strchr(p, ',') + 1 : nullptr) {
				double a, b; if (std::sscanf(p, "%lf:%lf", &a, &b) == 2) r.push_back({a, b / 1000.0});
			}
		return r;
	}();
	double sh = 0;
	for (const auto &x : v) if (double(n_) / cfg_.fs >= x.first) sh = x.second;
	return samples(sh);
}

void P2Station::p2_hunt_update(bool ok, bool new_info)
{
	static const bool off = std::getenv("P2_NO_HUNT") != nullptr;
	if (off) return;
	if (!hunt_old()) {                                // fix 208
		fail_hist_ = ((fail_hist_ << 1) | (ok ? 0u : 1u)) & 0xffu;
		if (new_info) {
			// Fix 208b: kHuntKeep new packets in a row (fix 208: 2), one of them moved on by the letter that
			// was due when the hunt began. sb208-14 (bench, P1 answer -7 ms): +4 ms (the wrong side) kept
			// on two new packets, the stall back 60 s later. The letter the sender just read is the last
			// P/Q we sent (last_ack_cs_, still the previous answer's here).
			if (hunt_k_ > 0) {
				hunt_trial_ = true; ++hunt_new_;
				if (last_ack_cs_ == hunt_fail_cs_ || hunt_fail_cs_ < 0) hunt_fail_seen_ = true;
				if (hunt_new_ >= hunt_keep() && hunt_fail_seen_) {   // kept
					hunt_base_ = hunt_s_; hunt_k_ = 0; hunt_trial_ = false; hunt_backoff_ = 1; hunt_quiet_ = 0; hunt_new_ = 0; early_seen_ = false;
					log("P2: answer time " + std::to_string(hunt_s_ * 1e3) + " ms kept (new packets)");
				} else if (debug_) log("P2: a new packet with our answers at " + std::to_string(hunt_s_ * 1e3) + " ms (on trial, " + std::to_string(hunt_new_) + ")");
			} else if (++hunt_quiet_ >= 24) hunt_backoff_ = 1;
			rep_hist_ <<= 1;
			deaf_ = 0;
			return;
		}
		if (!ok) return;                              // a failed decode says nothing about our answers
		if (hunt_k_ == 0 && p2_up_pending_) return;   // repeats while our S/R is pending are fix 170's (2 at most)
		if (++deaf_ == 1) hunt_noisy_ = fail_hist_ != 0 || __builtin_popcount(rep_hist_ & 0xffffu) >= 2;   // this run's start: the 16 answers before it
		rep_hist_ = (rep_hist_ << 1) | 1u;
		if (hunt_trial_) {                            // on trial and the same packet again: on with the hunt
			hunt_trial_ = false; hunt_new_ = 0; hunt_fail_seen_ = false; deaf_ = hunt_k_;
		} else if (hunt_k_ == 0) {
			const int k = (hunt_noisy_ ? hunt_after_noisy() : hunt_after_clean()) * hunt_backoff_;
			if (deaf_ < k) return;
			hunt_k_ = k; hunt_i_ = 0; hunt_quiet_ = 0; hunt_new_ = 0; hunt_fail_seen_ = false; up_miss_run_ = 0;
			hunt_fail_cs_ = ack_new_cs_ == p2arq::CS_P || ack_new_cs_ == p2arq::CS_Q ? ack_new_cs_ : -1;
			// Fix 208b: early first whatever the due letter (P2_HUNT_DUE_DIR=1: fix 208, late first when P is
			// due). The stalls that happen are late ones (the bench's history, the firmware's dead side:
			// late it reads P and not Q, and its window then moves away); sb208-14 had P due and stepped
			// late first. An early stall shows as requests not taken, handled by p2_up_result.
			hunt_dir_ = (hunt_due_dir() || early_seen_) && ack_new_cs_ == p2arq::CS_P ? 1 : -1;
			log(std::string("P2: the same packet ") + std::to_string(deaf_) + " times (" + (hunt_noisy_ ? "repeats or failed decodes lately" : "clean before") +
			    "), " + (ack_new_cs_ == p2arq::CS_P || ack_new_cs_ == p2arq::CS_Q ? std::string(1, p2arq::kCsName[ack_new_cs_]) : std::string("?")) + " due: hunting " + (hunt_dir_ < 0 ? "early" : "late") + " first");
		} else if ((deaf_ - hunt_k_) % kHuntEach) return;
		static const double mag[] = {0.004, 0.004, 0.008, 0.008, 0.012, 0.012};
		const int i = hunt_i_++;
		if (i >= 6) {                                 // every offset tried: back to the base, the next hunt after twice the repeats
			hunt_s_ = hunt_base_; hunt_k_ = 0; deaf_ = 0; hunt_trial_ = false; hunt_new_ = 0; hunt_backoff_ = std::min(hunt_backoff_ * 2, 4);
			log("P2: no answer time found: back to " + std::to_string(hunt_s_ * 1e3) + " ms from nominal; the next hunt after " + std::to_string(hunt_after_clean() * hunt_backoff_) + "+ repeats");
			return;
		}
		const double off_s = (i % 2 == 0 ? 1 : -1) * hunt_dir_ * mag[i];
		hunt_s_ = std::max(-0.02, std::min(0.02, hunt_base_ + off_s));
		log("P2: the same packet " + std::to_string(deaf_) + " times: our answers now " + std::to_string(hunt_s_ * 1e3) + " ms from nominal");
		return;
	}
	static const double steps[] = {0.0, -0.004, 0.004, -0.008, 0.008, -0.012, 0.012};
	static const bool recentre = std::getenv("P2_HUNT_RECENTRE") != nullptr;
	if (recentre) {                                   // fix 205, bench option
		if (new_info) {
			if (deaf_ >= kHuntAfter) {               // hunting: an offset got a new packet; on probation
				probation_ = 1;
				if (debug_) log("P2: a new packet with our answers at " + std::to_string(hunt_s_ * 1e3) + " ms (on probation)");
			} else if (probation_ > 0 && ++probation_ >= kConfirm) {
				// confirmed: the edge we were beyond lies within one step behind this offset
				const double d = hunt_s_ - hunt_base_, x = hunt_s_;
				if (d != 0) hunt_base_ = std::max(-0.02, std::min(0.02, x + (d > 0 ? kRecentre : -kRecentre)));
				hunt_s_ = hunt_base_; hunt_i_ = 0; probation_ = 0;
				log("P2: our answers at " + std::to_string(x * 1e3) + " ms confirmed: now " + std::to_string(hunt_s_ * 1e3) + " ms from nominal (re-centred)");
			}
			deaf_ = 0;
			return;
		}
		if (!ok) return;
		++deaf_;
		if (probation_ > 0 && deaf_ >= 2) {          // on probation: twice the same packet again
			probation_ = 0; deaf_ = kHuntAfter;
		} else if (deaf_ < kHuntAfter || (deaf_ - kHuntAfter) % kHuntEach) return;
		hunt_i_ = (hunt_i_ + 1) % 7;
		hunt_s_ = hunt_base_ + steps[hunt_i_];
		log("P2: the same packet " + std::to_string(deaf_) + " times: our answers now " + std::to_string(hunt_s_ * 1e3) + " ms from nominal");
		return;
	}
	auto step = [&](const char *why) {
		hunt_i_ = (hunt_i_ + 1) % 7;
		hunt_s_ = steps[hunt_i_];
		log(std::string("P2: ") + why + ": our answers now " + std::to_string(hunt_s_ * 1e3) + " ms from nominal");
	};
	if (new_info) {
		// a new packet after hunting: keep the offset, but on probation for 12 packets (near the
		// edge the sender still reads some of our letters)
		if (deaf_ >= kHuntAfter) { confirm_left_ = 12; if (debug_) log("P2: answer time " + std::to_string(hunt_s_ * 1e3) + " ms kept (a new packet)"); }
		else if (confirm_left_ > 0) confirm_left_--;
		deaf_ = 0;
		return;
	}
	if (!ok) return;                                  // a failed decode says nothing about our answers
	++deaf_;
	if (confirm_left_ > 0 && deaf_ >= 2) {            // on probation: twice the same packet again
		confirm_left_ = 0; deaf_ = kHuntAfter; step("the kept answer time failed"); return;
	}
	if (deaf_ < kHuntAfter || (deaf_ - kHuntAfter) % kHuntEach) return;
	step(("the same packet " + std::to_string(deaf_) + " times").c_str());
}

void P2Station::p2_tone_anchor()
{
	static const bool off = std::getenv("P2_TONE_TOGGLE_ONLY") != nullptr;
	// test hook: P2_TEST_SKIP_TOGGLE=T: once, at the first answer after T s, our toggle one step off
	// (as a cycle in which we sent nothing); the anchor below must undo it
	static double skip_at = std::getenv("P2_TEST_SKIP_TOGGLE") ? std::atof(std::getenv("P2_TEST_SKIP_TOGGLE")) : -1;
	if (skip_at >= 0 && double(n_) / cfg_.fs >= skip_at) { skip_at = -1; psk_swap_ = !psk_swap_; if (debug_) log("P2: test: toggle skipped"); }
	static const double slip_at = std::getenv("P2_TEST_TONE_SLIP") ? std::atof(std::getenv("P2_TEST_TONE_SLIP")) : -1;
	const bool want_slip = slip_at >= 0 && double(n_) / cfg_.fs >= slip_at;
	if (ans_swap_ < 0) return;
	const bool want = (ans_swap_ != 0) != master_;  // called: same carrier; caller: the other
	const bool use = want != want_slip;
	ans_swap_ = -1;
	if (psk_swap_ == use) return;
	tone_slips_++;
	if (debug_) log(std::string("P2: answer's leading carrier ") + (off && !want_slip ? "would be " : "") + "set from the packet (" + std::to_string(tone_slips_) + ")" + (want_slip ? " (test slip)" : ""));
	if (!off || want_slip) psk_swap_ = use;
}

void P2Station::reply_as_irs(int64_t start, bool was_long, bool new_info, bool ok)
{
	if (!ok) rx_last_fail_ = n_;                    // fix 217: rx_clean()
	// front panel, receiver: the result of this packet check
	if (!ok) { led_ir_ = 0; led_te_ = 2; pk_count(1); }
	else if (!new_info) { led_ir_ = 2; led_te_ = 0; pk_count(2); }
	else { led_ir_ = led_rx_idle_ || !led_rx_data_ ? 1 : 0; led_te_ = led_rx_data_ ? 1 : 0; pk_count(0); }
	if (ok) led_good_role_ = 0;
	led_rx_data_ = led_rx_idle_ = false;
	if (!p3_) p2_hunt_update(ok, new_info);
	const int64_t at = reply_at(start, was_long) + (p3_ ? 0 : samples(hunt_s_) + test_cs_shift());
	const int64_t at_cs = at + samples(cs_ref_s());   // fix 208: our P2 control signal (callee: on the P1 answer's reference); break-ins stay at `at`
	const int64_t next = start + samples(was_long ? lcyc_s() : cyc_s()) + (p3_ && !was_long ? p3_drift_step() : 0) +
	                     trk_drift_step(was_long ? lcyc_s() / cyc_s() : 1.0);   // fix 247: the peer's drift (P2)
	if (p3_) {                                       // P3: P/Q acks; break-in when we have something to send
		// Fix 229: our run of good packets, as the reference unit's requests follow one: +1 on every
		// good decode, cleared on a failure and on a break-in, NOT on our own S or R.
		if (ok) { if (p3_run_ < 1000) p3_run_++; } else p3_run_ = 0;
		// Fix 50: the sender's "over" bit (0x40) hands us the link; the
		// firmware breaks in on it even with nothing to send (emulator,
		// 2026-10-16: our idle packet with over, the firmware typed
		// nothing, it broke in at once and sent idle without over).
		// KB2PCN-8 (80 m, 2026-10-15) sent idle with over for 3 minutes
		// after its banner; we kept acknowledging and the link died.
		const bool qrt_bk = qrt_bk_now();
		// Test hook (fix 274): break in on the short packet that completes our R back to short packets,
		// when text is waiting (as on the air 2026-11-20 18:24): the switch's pending flag then outlives it.
		static const bool bk_at_r = std::getenv("P3_TEST_BK_AT_R") != nullptr;
		if (bk_at_r && ok && new_info && !was_long && p3_r_pending_ && !p3_rx_long_ && !txq_.empty() && !bk_req_) { bk_req_ = true; log("test: break-in on the packet completing our R"); }
		if (ok && !qrt_rx_ && ((must_break_in_ && !qrt_hold_on_over()) || bk_req_ || auto_break_in() || qrt_bk)) {
			must_break_in_ = false;
			bk_tx_.clear();
			const int64_t bat = queue_words(at, my_bk(false), 82);   // our first 3 characters; the rest follows in P3 packets (fix 257: bat, where it went)
			premod_after_bk();                       // fix 254
			if (master_) grid_ = bat - samples(0.6); // our packets follow our break-in's lattice
			state_ = ST_P3SETUP; misses_ = 0;
			p3_bk_tries_ = 0; p3_bk_guess_ = -1;    // fix 265
			// fix 277: the far tones of the packet just decoded (level 2 or more), the measure for a packet in our looks
			p3_bk_ref_far_ = p3_rx_level_ >= 2 ? p3_tone_power(start + samples(0.1), start + samples(0.6), kP3FarTones, 4) : -1;
			schedule(master_ ? caller_look_at(bat) : callee_look_at(bat), EV_P3, bat, 20);
			log("P3: break-in");
			return;
		}
		int cs;
		static int forced_t = std::getenv("P3_DATA_T") ? std::atoi(std::getenv("P3_DATA_T")) : 0;   // experiment hook
		static int forced_s = std::getenv("P3_DATA_S") ? std::atoi(std::getenv("P3_DATA_S")) : 0;   // experiment hook: S as the ack
		if (forced_s > 0 && new_info && ok && good_run_ >= 0) {
			forced_s--; cs = p2arq::CS_S; ack_phase_ = 1; last_ack_cs_ = cs;   // after S the next plain ack is P
			if (p3_rx_level_ < 6) p3_rx_level_++;
			log("P3: sending S (experiment), expecting level " + std::to_string(p3_rx_level_));
			send_cs(cs, at); schedule_p3_rx(next, false); return;
		}
		if (forced_t > 0) { forced_t--; cs = p2arq::CS_T; send_cs(cs, at); schedule_p3_rx(next, false); return; }
		static const char *after_s = std::getenv("P3_AFTER_S");   // experiment hook: the first ack after our S
		static bool s_sent = false;
		if (forced_s == 0 && std::getenv("P3_DATA_S") && !s_sent && new_info && after_s) {
			s_sent = true;
			const char *q = std::strchr(p2arq::kCsName, after_s[0]);
			cs = q ? int(q - p2arq::kCsName) : p2arq::CS_P;
			last_ack_cs_ = cs; rx_fails_ = 0;
			log(std::string("P3: first ack after S = ") + after_s[0] + " (experiment)");
			send_cs(cs, at); schedule_p3_rx(next, false); return;
		}
		// The firmware's receiver: after 5 failures
		// it sends T and lowers its own level; it keeps sending T while the
		// failures go on and lowers again after 5 more; the first
		// acknowledgement after T is P. `modem/` sent T after every second
		// failure and kept toggling P/Q from before.
		// Speed-up: after
		// MAXUp - 2 = 2 good packets while the sender has more data we send S
		// and raise our level at once. A good packet at the new level is
		// answered with the signal we sent before the S; a failure with S
		// again; a second failure drops back and answers with that signal
		// toggled.
		if (rx_up_pending_) {
			if (new_info) {
				cs = rx_pre_s_; ack_phase_ = cs == p2arq::CS_Q ? 1 : 0;
				rx_up_pending_ = false; rx_fails_ = 0; rx_good_ = 0;
				p3_backoff_ = 1;
			} else if (++rx_up_fails_ < 2) cs = rx_up_cs_;
			else {
				// Fix 81: back to the level the request was made from. A packet
				// decoded at the old level meanwhile had already reset our level,
				// and the decrement then gave level 0 (K0NTS 40 m, 2026-10-17
				// 20:06: memory ARQ, SL1 only, off for the next failures).
				p3_rx_level_ = std::getenv("P3_UP_DECREMENT") ? p3_rx_level_ - 1 : rx_up_from_; rx_up_pending_ = false; rx_good_ = 0;
				cs = rx_pre_s_ == p2arq::CS_P ? p2arq::CS_Q : p2arq::CS_P;
				ack_phase_ = cs == p2arq::CS_Q ? 1 : 0;
				// fix 65: back off (as P2): each speed-up not taken doubles the good
				// packets before the next request (max 16 x)
				if (!std::getenv("P3_NO_BACKOFF")) p3_backoff_ = std::min(p3_backoff_ * 2, 16);
				log("P3: speed-up not taken, back to level " + std::to_string(p3_rx_level_) + ", next request after " + std::to_string(kP3SpeedUp * p3_backoff_) + " good packets");
			}
			if (cs != rx_up_cs_) last_ack_cs_ = cs;
			send_cs(cs, at);
			schedule_p3_rx(next, ok);
			return;
		}
		// P3 long packets (2026-10-17): R toggles the sender between short
		// and long packets (as our sender does on R). We ask for long when the
		// sender has more and two good new packets came in a row (not at SL4:
		// the firmware's receiver does not ask there), and for short again when
		// a long packet says the sender has no more. Only where S cannot help:
		// at SL6, or once a speed-up was not taken (one level up is worth more
		// than long packets: emulator, firmware sending 12,000 characters,
		// long SL5 238 vs short SL6 247 characters/s). R acknowledges the packet;
		// while the switch is pending a failure is answered R again, and the
		// first plain acknowledgement after it is P.
		// (fix 217: the packet that completes a switch is acknowledged plainly; the firmware's
		// receiver skips S and R on it, and its sender reads R right after R as a
		// repeat request: our R back to short on the first long packet repeated it for ever)
		bool r_just = false;
		if (p3_r_pending_ && new_info) { p3_r_pending_ = false; r_just = true; }
		// Fix 258: the first packet after our A2 answer T completes that answer's speed setting too:
		// acknowledged plainly (Q), no R or S on it, as after a break-in answered T (arm_bk_answer:
		// its Q), and as the reference unit's receiver does (emulated V4.1r: Q on the first packet
		// after its T, then S or R). Our R there was taken by the reference unit's sender, but it still
		// waited for that Q: our P after the R (the first acknowledgement after R) made it go one level
		// down and send the same again, and every later P read as a repeat request; neither side counted
		// a failure, so the link stayed up for good with nothing moving (the SDK's emulator test, long
		// packets asked for). P3_A2_FIRST_OLD=1: as before.
		static const bool a2_first_old = std::getenv("P3_A2_FIRST_OLD") != nullptr;
		if (new_info && p3_a2_rx_t_ && last_ack_cs_ == p2arq::CS_T && !a2_first_old) r_just = true;
		// a good path only: kP3LongAfter good new packets in a row (long
		// packets lose 3.75 s per failure; in steady noise they did worse)
		if (ok && new_info) p3_long_good_++; else p3_long_good_ = 0;
		// Fix 217: as the firmware's receiver, R as soon as the sender's
		// "much to send" flag is up (no run of good packets first), and R back to short packets
		// when it is down. The flag was read inverted before (more_bit), so this never ran on
		// the firmware's packets; it was off by default as well. P3_LONG_OLD=1: the old rule
		// (kP3LongAfter good packets, no return to short unless P3_LONG_LEAVE).
		static const bool long_old = std::getenv("P3_LONG_OLD") != nullptr;
		const int long_after = long_old ? kP3LongAfter : 0;
		const bool leave = long_old ? std::getenv("P3_LONG_LEAVE") != nullptr : true;
		// Fix 217, beyond the firmware: a full packet (no fill) also asks for long packets, once
		// until the flag is seen up again after we went back to short ones (the reference unit's sender
		// raises the flag only with plenty queued: one short SL6 cycle later; with a
		// long cycle every 3.75 s that cycle decides how many long packets fit a link). Back to
		// short packets only when the flag is down and the packet had fill. Off by default since the
		// defaults audit (owner rule: the reference unit's behaviour unless decided otherwise; its R
		// reads the flag alone). P3_LONG_FULL=1: on.
		static const bool full_on = !long_old && std::getenv("P3_LONG_FULL") && std::atoi(std::getenv("P3_LONG_FULL")) > 0;
		if (rx_more_) p3_full_ok_ = true;
		static const double long_max_df = std::getenv("P3_LONG_MAX_DF") ? std::atof(std::getenv("P3_LONG_MAX_DF")) : 20.0;
		const bool df_ok = std::max(std::fabs(p3_rx_df_), std::fabs(afc_hz())) <= long_max_df;   // as more_bit: not with an offset
		// Fix 217b: not in real time unless P3_LONG_RT=1. Our P3 long decode does not fit its answer
		// time on the build VM (a real-time loopback fell to SL1 by computer back-off, the long decodes
		// late, 2026-11-11), let alone on the Pi 4; the lockstep runs (emulator, compatibility
		// set) have no deadline. Gate to be replaced by a measured long-decode budget.
		// Fix 241: adaptive by default (p3_long_adaptive above): the ceiling instead of the gate.
		const bool l_adapt = p3_long_adaptive();
		const int l_ceil = l_adapt ? p3l_ceil_ : 6;
		if (p3_rx_level_ < l_ceil) p3l_up_ = false;
		const bool l_room = !l_adapt || (p3_rx_level_ <= l_ceil && !(p3l_up_ && p3_rx_level_ >= l_ceil));
		const bool rt_ok = !cfg_.realtime || (p3_long_rt_env() > 0 ? p3_long_budget_ok() : p3_long_rt_env() == 0 ? false : l_room);
		const bool want_long = (rx_more_ || (full_on && rx_full_ && p3_full_ok_)) && df_ok && rt_ok && (long_old || rx_clean());
		const bool want_short = ((!rx_more_ && (!full_on || !rx_full_)) || !p3_long_budget_ok() || (l_adapt && p3_rx_level_ > l_ceil)) && !r_just;
		// Fix 224: the reference unit's order as receiver: S when
		// MAXUp - 2 good packets came AND the fill count is 0 (still the previous packet's when it
		// decides, fix 217), else R when the sender's "much to send" flag is up. We asked S on any 2 good
		// packets, idle ones included, and R only at SL6 (or after a refused speed-up): at 3 dB the
		// reference-unit pair goes SL3 -> R -> long SL3 -> S -> long SL4 with no failure; we went S, S to short
		// SL5 and every packet needed two copies (p3-snr3: 2,150 against 3,217). Still missing: the
		// reference unit also asks S only on packets it reads cleanly enough; as fix 217's
		// Viterbi-metric gate in P2, ours is the decode residual (p3_resid_, the average of the
		// clean decodes at this level, fix 173): S only at most kP3SResid. Not calibrated against
		// the reference unit [inf]: 0.47 lies between what we measure at 9 dB (SL3-SL6 0.33-0.44, where S pays)
		// and at 3 dB (0.53-0.56, where the pair never leaves SL3/SL4). SL1 has no residual: as
		// before. P3_S_FILL_OLD=1: the old rules; P3_S_RESID=x: the limit.
		static const bool s_order_off = std::getenv("P3_S_FILL_OLD") != nullptr;
		static const double s_resid = std::getenv("P3_S_RESID") ? std::atof(std::getenv("P3_S_RESID")) : kP3SResid;
		const bool prev_full = p3_last_full_;
		if (new_info) p3_last_full_ = rx_full_;
		// Fix 229: S also needs THIS packet to have come through cleanly enough for the level above:
		// its fit residual (short) or its phase-step rms (long, long_phase_err) at most kP3SQual. In
		// the reference pair runs (compat channel, white noise) the reference unit stops asking S at:
		// SL3 short 2.5 dB, SL3 long 2.3 dB, SL4 long 6.6 dB, SL5 long 14.8 dB; SL4 / SL5 short ~5.3 /
		// ~13.2 dB [inf: worked out from the long ones, not measured].
		// Each limit is our reading at that noise in the same channel. SL2 is not reached by the
		// reference pair: fix 224's 0.47 kept [inf]. P3_S_QUAL_OLD=1: fix 224's smoothed-residual gate.
		static const bool q_old = std::getenv("P3_S_QUAL_OLD") != nullptr || std::getenv("P3_S_RESID") != nullptr;
		bool s_quality;
		if (q_old) s_quality = p3_rx_level_ < 2 || (p3_resid_n_ >= 2 && p3_resid_ <= s_resid);
		else if (p3_rx_level_ < 2) s_quality = true;
		else if (p3_q_lv_ != p3_rx_level_ || p3_q_ < 0) s_quality = false;            // no reading of this level's packet
		else s_quality = p3_q_ <= kP3SQual[p3_q_long_ ? 1 : 0][std::min(6, p3_rx_level_)];
		// Fix 229: the reference unit asks P3 S only with the sender's "much to send" flag up (the flag
		// R reads) and after MAXUp - 2 good packets, counted across its own S and R: after S it asks
		// again on the packet after the one that completes the switch (pair, 9 dB: S on the second
		// long SL3 and on the second long SL4 packet; we asked one packet later).
		// P3_S_RULE_OLD=1: fix 224's count and no flag.
		static const bool s_rule_old = std::getenv("P3_S_RULE_OLD") != nullptr;
		const bool s_count = s_rule_old ? rx_good_ + 1 >= kP3SpeedUp * p3_backoff_ : p3_run_ >= kP3SpeedUp * p3_backoff_;
		const bool s_flag = s_rule_old || rx_more_;
		const bool s_due = !s_order_off && new_info && !rx_t_state_ && !r_just && s_count && s_flag && prev_full && s_quality &&
		                   p3_rx_level_ < std::min(6, p3_cap_) && cfg_.request_speedup;
		// Fix 229: the reference unit asks R whatever the signal quality; fix 173's clean-path gate
		// (smoothed residual <= 0.36) kept us on short SL3 at 3 dB where the reference unit pair goes long.
		// P3_LONG_GATE=1: fix 173's gate back.
		// (fix 229: the reference unit asks R at SL4 too; ours stood for the flag read inverted there)
		static const bool sl4_r_old = std::getenv("P3_SL4_XOR_OLD") != nullptr;
		// fix 241: at the long ceiling a speed-up due is asked as R first (then S on short packets)
		const bool climb_short = l_adapt && p3_rx_long_ && p3_rx_level_ >= l_ceil && s_due;
		static const bool long_gate = std::getenv("P3_LONG_GATE") != nullptr || std::getenv("P3_S_RULE_OLD") != nullptr;
		if (cfg_.p3_request_long && ok && new_info && !rx_t_state_ && !rx_up_pending_ && !p3_r_pending_ && !qrt_rx_ && (p3_rx_long_ || !long_gate || p3_long_clean()) &&
		    ((!p3_rx_long_ && want_long && !r_just && (p3_rx_level_ != 4 || !sl4_r_old) && p3::long_payload(p3_rx_level_) > 0 &&
		      (s_order_off ? (p3_rx_level_ >= std::min(6, p3_cap_) || p3_backoff_ > 1 || !cfg_.request_speedup || std::getenv("P3_LONG_FIRST")) : !s_due) &&
		      p3_long_good_ >= long_after) || (p3_rx_long_ && (want_short || climb_short) && leave))) {
			if (p3_rx_long_ && !rx_more_) p3_full_ok_ = false;   // back to short: the full trigger waits for the flag
			if (p3_rx_long_ && climb_short && !want_short) { p3l_up_ = true; log("P3: long packets: at SL" + std::to_string(p3_rx_level_) + ", this computer's long ceiling: short packets to climb"); }
			p3_rx_long_ = !p3_rx_long_; p3_r_pending_ = true; p3_long_good_ = 0;
			ack_phase_ = 1; last_ack_cs_ = p2arq::CS_R; rx_fails_ = 0; rx_good_ = 0;
			log(std::string("P3: asking for ") + (p3_rx_long_ ? "long" : "short") + " packets (R) at level " + std::to_string(p3_rx_level_));
			send_cs(p2arq::CS_R, at);
			schedule_p3_rx(next, true);
			return;
		}
		// (fix 217: S no longer waits for the sender's "much to send" flag; it was read inverted,
		// so S came only while the sender had little queued, e.g. on idle packets)
		static const bool s_more = std::getenv("P3_S_NEEDS_MORE") != nullptr;
		if (new_info && !rx_t_state_ && !climb_short && (s_order_off || s_rule_old ? ++rx_good_ >= kP3SpeedUp * p3_backoff_ : (++rx_good_, s_due)) && (rx_more_ || !s_more) && p3_rx_level_ < std::min(6, p3_cap_) && cfg_.request_speedup &&
		    (s_order_off || (prev_full && !r_just && s_quality))) {
			rx_pre_s_ = last_ack_cs_; rx_up_pending_ = true; rx_up_fails_ = 0; rx_good_ = 0; rx_up_cs_ = p2arq::CS_S;
			rx_up_from_ = p3_rx_level_;
			p3_rx_level_++;
			log("P3: asking for a higher level, now expecting " + std::to_string(p3_rx_level_));
			mq3_clear();
			send_cs(p2arq::CS_S, at);
			schedule_p3_rx(next, true);
			return;
		}
		// Fix 219: a packet above the level our T asked for: the sender never read the T (it steps
		// down on the first T it reads, fix 17), so the last letter it read is our last
		// plain acknowledgement, and the firmware's sender takes only a letter other than that one
		// (fix 168). After the T state's P (below) to a new packet whose ack
		// was due as Q, it repeated that packet for ever and we answered every repeat P again
		// (a slip test channel, 36/30/10 ms: our T's went out where we expected the packets, 20 ms off,
		// while the drift lost them; f216b-1 ended the same way: status 35, P, P, P). Now the
		// T state ends there and the letters alternate as if the T had not been sent.
		static const bool tun_off = std::getenv("P3_TUNTAKEN_OFF") != nullptr;   // test: off
		if (ok && rx_t_state_ && p3_t_untaken_ && !p3_drift_off() && !tun_off) { rx_t_state_ = false; log("P3: our T was not taken (the sender is at level " + std::to_string(p3_rx_level_) + ")"); }
		p3_t_untaken_ = false;
		if (!new_info) rx_good_ = 0;
		if (new_info) {
			if (rx_t_state_) { ack_phase_ = 0; rx_t_state_ = false; }
			else ack_phase_ ^= 1;
			cs = ack_phase_ ? p2arq::CS_Q : p2arq::CS_P; rx_fails_ = 0;
		} else if (ok && rx_t_state_ && !std::getenv("P2_T_NEW_ONLY")) {
			// fix 132 (as P2): after our T a good repeat of a packet we had is
			// answered P too, else the sender at its lowest level repeats it for ever
			ack_phase_ = 0; rx_t_state_ = false; rx_fails_ = 0;
			cs = p2arq::CS_P;
		} else if (!ok && (++rx_fails_ >= kP3TDown || p3_rx_level_ > p3_cap_) && p3_rx_level_ > 1) {
			// (back-off: a failed packet above the cap asks down at once)
			cs = p2arq::CS_T;
			rx_fails_ = 0; rx_t_state_ = true;
			p3_rx_level_--;
			log("P3: asking for a lower level, now expecting " + std::to_string(p3_rx_level_) + (p3_rx_level_ + 1 > p3_cap_ && rx_fails_ == 0 ? " (computer back-off)" : ""));
		} else cs = rx_t_state_ ? p2arq::CS_T : last_ack_cs_;   // repeat request (T while in the T state)
		if (cs != p2arq::CS_T) last_ack_cs_ = cs;
		send_cs(cs, at);
		schedule_p3_rx(next, ok);
		return;
	}
	p2_tone_anchor();
	const bool qrt_bk = qrt_bk_now();
	if (ok && !qrt_rx_ && ((must_break_in_ && !qrt_hold_on_over()) || bk_req_ || auto_break_in() || qrt_bk)) {
		must_break_in_ = false;
		// The caller's break-in goes slightly after its grid slot (the reference unit
		// samples it on a fixed raster); its later packets follow it.
		send_bk(at + samples(bk_rel_s()));     // fix 215: callee, before our control-signal instant
		bk_lattice_ = start + (was_long ? samples(lcyc_s() - cyc_s()) : 0);   // the caller's packet we answered (what we heard)
		state_ = ST_BK_SENT;
		schedule(bk_check_at(at), EV_REPLY, at, 2);
		return;
	}
	int cs;
	// After our S (the reference unit's receiver rule, in P2 as in
	// P3; fix 16): a new packet at a higher level is answered with the
	// signal we sent before the S; a repeat or a failure (the sender did
	// not go up, e.g. it is at its MYLevel) gets S once more, a second
	// drops back and answers with that signal toggled. Without the drop
	// back, S and the sender's repeats went on for ever (RF bench, the
	// real reference unit at MYLevel 2).
	// Fix 131: failed decodes while our request is pending are counted too.
	// They were not ("ask again, uncounted"): noise starting just as we asked
	// for level 4 left us sending S for the rest of the link; S again is a
	// repeat request, so the sender never stepped down and our T never came
	// (regress.sh p2-rx-stepdown: 48 characters, 911 at --max-level 3).
	// After two in a row the request is dropped (with the backoff) and the
	// failure handled as any other below: T after repeated failures.
	if (p2_up_pending_ && !ok && ++p2_up_bad_ >= 2 && !std::getenv("P2_UP_UNCOUNTED")) {
		p2_up_pending_ = false;
		// A dropped R may have been taken: the sender may be sending long packets we cannot
		// decode, so long stays possible (each look's power test decides) until a decode says
		// which. Without it we looked for short packets while the sender sent long ones at
		// every lower level, and the link timed out (our station pairs, noise 4,000 from 20 s:
		// 210 characters, then nothing). From modem/ (2026-10-31, its third P2 receiver fix).
		if (p2_req_cs_ == p2arq::CS_R && !std::getenv("P2_DROPPED_R_SHORT")) rx_long_ = true;
		int &b = p2_backoff_[p2_req_cs_ == p2arq::CS_S ? 0 : 1];
		b = std::min(b * 2, 16);
		log(std::string("P2: ") + (p2_req_cs_ == p2arq::CS_S ? "speed-up" : "long packets") + " dropped: decodes failing");
	}
	if (p2_up_pending_ && ok) p2_up_bad_ = 0;
	if (p2_up_pending_) {
		const bool taken = p2_req_cs_ == p2arq::CS_S ? rx_level_ > p2_up_from_ : rx_long_;
		// Fix 170: a new packet at the old level means the sender read our request as an
		// acknowledgement, i.e. as the other letter than the one it had read before (a letter that
		// differs from the last one read acknowledges). After it takes S it expects
		// the letter it read before the S, unchanged, and reads the other one as "the receiver fell
		// back: one level down, the same packet again". So that letter
		// is now the other one. We kept the old one: the sender took our next S, we answered its
		// first packet at the new level with the stale letter, it went back down and resent that
		// packet, and our repeat requests (the same letter) were repeat requests to it too:
		// the same packet until the link timed out (emulator: our S read as Q, 0 characters).
		// P2_UP_STALE_PRE=1: old.
		if (new_info && !taken && !std::getenv("P2_UP_STALE_PRE")) {
			p2_pre_s_ = p2_pre_s_ == p2arq::CS_P ? p2arq::CS_Q : p2arq::CS_P;
			log(std::string("P2: ") + p2arq::kCsName[p2_req_cs_] + " read as an acknowledgement (a new packet at the old level); letter before it now " + p2arq::kCsName[p2_pre_s_]);
		}
		if (new_info && taken) {
			cs = p2_pre_s_; ack_phase_ = cs == p2arq::CS_Q ? 1 : 0;
			p2_up_pending_ = false;
			p2_up_result(true);                       // fix 208b
			p2_backoff_[p2_req_cs_ == p2arq::CS_S ? 0 : 1] = 1;
		} else if (!ok || ++p2_up_fails_ < 2) cs = p2_req_cs_;   // a failed decode: ask again, uncounted (see modem/; not the firmware rule)
		else {
			// Not taken (twice): answer as if we had not asked, and ask again
			// only after twice as many good packets (up to 16 x). The real
			// reference unit on the RF bench ignored both our S and our R: without
			// this the link repeated the same packet for ever.
			p2_up_pending_ = false;
			cs = p2_pre_s_ == p2arq::CS_P ? p2arq::CS_Q : p2arq::CS_P;
			ack_phase_ = cs == p2arq::CS_Q ? 1 : 0;
			int &b = p2_backoff_[p2_req_cs_ == p2arq::CS_S ? 0 : 1];
			b = std::min(b * 2, 16);
			log(std::string("P2: ") + (p2_req_cs_ == p2arq::CS_S ? "speed-up" : "long packets") + " not taken, next request after " + std::to_string(kP2SpeedUp * b) + " good packets");
			p2_up_result(false);                      // fix 208b: may step our answers later and reset b
		}
		if (ok) rx_fails_ = 0;
		if (new_info) rep_same_ = 0;
		static const bool up_old2 = std::getenv("P2_UP_OLD") != nullptr;
		if (up_old2 || p2_req_cs_ != p2arq::CS_R || !ok || !rx_clean()) good_run_ = 0;   // fix 217: R does not restart the run (clean link)
		if (cs != p2_req_cs_) last_ack_cs_ = cs;
		if (new_info) ack_new_cs_ = cs == p2arq::CS_P || cs == p2arq::CS_Q ? cs : -1;   // fix 208
		send_cs(cs, at_cs);
		schedule(next + samples(0.86), EV_PACKET, next);
		return;
	}
	// Fix 56 (the firmware's receiver rule, as P3): after our T, failures
	// are answered with T again and the first good new packet with P.
	// Repeating our last letter (P) after a T read as an acknowledgement:
	// KB5LZK (30 m, 2026-10-16 10:43) skipped a packet and then repeated
	// the next one for ever (our P's, out of step, were repeat requests).
	static const bool t_every2 = std::getenv("P2_RX_T_EVERY2") != nullptr;
	// Fix 132: a good decode of a packet we already had counts here too. After
	// our T the sender repeats its unacknowledged packet at the lower level and
	// expects P; answering it T (only "new" packets left the T state) was a
	// deadlock at level 1: 121 T's, the same packet every cycle (p2-rx-stepdown
	// at level 4, 2026-10-21). Not delivered twice: delivery follows `new_info`.
	static const bool t_new_only = std::getenv("P2_T_NEW_ONLY") != nullptr;
	if ((t_new_only ? new_info : ok) && rx_t_state_) {
		rx_t_state_ = false;
		ack_phase_ = 0;
		cs = p2arq::CS_P;
	} else if (new_info) {
		ack_phase_ ^= 1;
		// Fix 217 (the reference unit as receiver, emulated reference-unit pairs, clean): R on the first new
		// packet whose "more" bit is up, with no run of good packets first (the pair: R to the
		// first text packet; we waited for 4); the run of good packets is not reset by R, so S
		// follows on the second long packet (as if it counted every good packet and cleared
		// the count on S only); and after the other end's break-in the first new packet, when it
		// decoded cleanly, is answered S at once (the pair: S to the callee's first banner
		// packet; we sent 4 short packets at level 2, so our break-in and everything after it
		// ran a level lower). P2_UP_OLD=1: the old rules.
		static const bool up_old = std::getenv("P2_UP_OLD") != nullptr;
		static const bool no_quick = std::getenv("P2_NO_QUICK_S") != nullptr;
		const bool quick_s = !up_old && !no_quick && p2_quick_s_ && ack_phase_ == 1 && cfg_.request_speedup && rx_level_ < cfg_.max_level && !rx_long_ &&
		                     last_rx_metric_ >= 0 && last_rx_metric_ <= kP2QuickSMetric && last_rx_marq_ == 0 && p2_backoff_[0] == 1;
		p2_quick_s_ = false;
		if (ack_phase_ == 0) cs = p2arq::CS_P;
		else if (quick_s) {
			p2_pre_s_ = p2arq::CS_P;
			p2_up_pending_ = true; p2_up_fails_ = 0; p2_up_bad_ = 0; p2_up_from_ = rx_level_; p2_req_cs_ = p2arq::CS_S;
			cs = p2arq::CS_S; good_run_ = 0;
			log("P2: speed-up at once after the break-in (clean decode)");
		}
		else if (cfg_.request_long && rx_more_ != rx_long_ && (rx_long_ || long_ok(rx_level_)) && (rx_long_ || (!up_old && p2_backoff_[1] == 1 && rx_clean()) || good_run_ >= kP2SpeedUp * p2_backoff_[1])) {
			// long while data is queued (back to short when it is not);
			// asking for long is followed like S (taken or fall back)
			cs = p2arq::CS_R;
			if (!rx_long_) {
				p2_pre_s_ = ack_phase_ ? p2arq::CS_P : p2arq::CS_Q;
				p2_up_pending_ = true; p2_up_fails_ = 0; p2_up_bad_ = 0; p2_req_cs_ = p2arq::CS_R;
				if (up_old || !rx_clean()) good_run_ = 0;
			}
		}
		else if (cfg_.request_speedup && good_run_ >= kP2SpeedUp * p2_backoff_[0] && rx_level_ < cfg_.max_level &&
		         (up_old || std::max(last_rx_metric_, prev_rx_metric_) <= (rx_long_ ? kP2SMetricLong : kP2SMetric))) {
			// S after MAXUp (4) good packets in P2 (P3: 4 - 2)
			p2_pre_s_ = last_ack_cs_ == p2arq::CS_S ? p2arq::CS_P : (ack_phase_ ? p2arq::CS_P : p2arq::CS_Q);
			p2_up_pending_ = true; p2_up_fails_ = 0; p2_up_bad_ = 0; p2_up_from_ = rx_level_; p2_req_cs_ = p2arq::CS_S;
			cs = p2arq::CS_S; good_run_ = 0;
		}
		else cs = p2arq::CS_Q;
	} else if (!ok && (!rx_t_state_ || t_every2) && ++rx_fails_ >= p2_rx_tdown() && rx_level_ > 1) {
		cs = p2arq::CS_T;                            // repeated failures: ask for a lower level
		rx_fails_ = 0;
		rx_t_run_ = 1;
		rx_level_--;
		// Memory ARQ and the headerless decode expect level_: it follows our
		// T, as it follows every good packet. It stayed at the old level, so
		// after the sender stepped down they summed and decoded at a level
		// it no longer sent, for the rest of the link (2026-10-19, P2 after
		// the callee's break-in). P2_RX_LEVEL_STAYS: the old behaviour.
		if (!std::getenv("P2_RX_LEVEL_STAYS")) { level_ = rx_level_; if (debug_) log("T: now expecting level " + std::to_string(rx_level_)); }
		// Fix 54/56: after our T the sender's next expected acknowledgement
		// is P (KB2PCN-8 took our Q there as a repeat request); until then
		// failures get T again, not our last letter.
		rx_t_state_ = true;
	} else {
		cs = rx_t_state_ ? p2arq::CS_T : last_ack_cs_;   // repeat request (T while in the T state)
		// Fix 170: a packet we already hold, repeated while we keep answering it with the same
		// letter: if the sender's last letter read is that same one (we disagree about whether it
		// was acknowledged), each answer is a repeat request to it and neither side ever moves.
		// After kRepeatProbe such repeats we answer once with the other letter (to the sender an
		// acknowledgement of the packet we hold); if it was only missing our answers, the probe
		// costs one cycle. P2_NO_REPEAT_PROBE=1: off.
		// Fix 208: while hunting (the new hunt), the letter that acknowledges the held packet every cycle
		// (the one the sender must read to move on), the other letter once every kRepeatProbe cycles as
		// fix 170's probe. Fix 171 alternated them every cycle; with the sender failing to read our
		// letters in noise (emulator, --tx-noise 12000, our decodes clean) that never moved it on: the
		// same packet 40 cycles running, at offsets 0 and +-4 ms too, where holding one letter (no hunt)
		// let packets through within a few cycles.
		if (ok && !new_info && cs != p2arq::CS_T && (cs == p2arq::CS_P || cs == p2arq::CS_Q) && hunting() && !hunt_old() &&
		    (ack_new_cs_ == p2arq::CS_P || ack_new_cs_ == p2arq::CS_Q) && !std::getenv("P2_NO_REPEAT_PROBE")) {
			const bool probe = ++rep_same_ >= kRepeatProbe;
			if (probe) rep_same_ = 0;
			cs = probe ? (ack_new_cs_ == p2arq::CS_P ? p2arq::CS_Q : p2arq::CS_P) : ack_new_cs_;
			ack_phase_ = cs == p2arq::CS_Q ? 1 : 0;
			if (probe || debug_) log(std::string("P2: the same packet again: answered ") + p2arq::kCsName[cs] + (probe ? " (hunting, probe)" : " (hunting)"));
		} else if (ok && !new_info && cs != p2arq::CS_T && (cs == p2arq::CS_P || cs == p2arq::CS_Q) && !std::getenv("P2_NO_REPEAT_PROBE")) {
			if (++rep_same_ >= kRepeatProbe || (hunting() && hunt_old())) {   // fix 171: every cycle while hunting (P2_HUNT_OLD=1)
				rep_same_ = 0;
				cs = cs == p2arq::CS_P ? p2arq::CS_Q : p2arq::CS_P;
				ack_phase_ = cs == p2arq::CS_Q ? 1 : 0;
				log(std::string("P2: the same packet again: answered ") + p2arq::kCsName[cs] + (hunting() ? " (hunting)" : " once"));
			}
		}
		// The sender lowers on the first T of a run and then on every 5th
		// further one (its T handling, kP3TDown); the level we expect
		// (memory ARQ, the headerless decode) follows the same count. We
		// lowered every 2 failures, a level ahead of the sender: lockstep,
		// B sent L2 for 5 cycles while A expected L1 (2026-10-19).
		// P2_RX_T_EVERY2: the old rule.
		if (cs == p2arq::CS_T && !t_every2 && ++rx_t_run_ > kP3TDown && rx_level_ > 1) {
			rx_level_--; level_ = rx_level_; rx_t_run_ = 1;
			if (debug_) log("T: now expecting level " + std::to_string(rx_level_));
		}
	}
	if (ok) rx_fails_ = 0;
	if (new_info || cs == p2arq::CS_T) rep_same_ = 0;
	if (cs != p2arq::CS_T) last_ack_cs_ = cs;
	if (new_info) ack_new_cs_ = cs == p2arq::CS_P || cs == p2arq::CS_Q ? cs : -1;   // fix 208: the letter the sender must read to move on
	send_cs(cs, at_cs);
	schedule(next + samples(0.86), EV_PACKET, next);
}

void P2Station::p2_late_poll()
{
	if (!cfg_.realtime || !p2_late_.valid() || p2_late_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
	late_ = p2_late_.get();
	late_slot_ = p2_late_reuse_ && p2_late_slot_ >= p2_decided_ ? p2_late_slot_ : -1;   // for the repeat only while no later slot was answered
	// Fix 149: a late decode still measured its own packet's timing: it teaches the
	// track (at its own slot). Without that, a CPU that decodes late every time (the
	// Pi 4: 43 ms against a ~31 ms window) never locked the track, so every decode
	// stayed on the four-demodulation acquisition path and stayed late.
	if (late_.ok && !std::getenv("P2_LATE_NO_TRACK")) trk_update(late_, p2_late_slot_);
	if (debug_) log(std::string("P2: the late decode is done: ") + (late_.ok ? std::string(late_.lng ? "long" : "short") + " level " + std::to_string(late_.level) + ", " + kPathName[late_.path] : "failed"));
}

void P2Station::p2_launch(int64_t exp, bool waited_long)
{
	p2_late_poll();                                   // fix 200: a late decode done by now teaches the track first
	if (debug_) log(std::string("P2: decode launched (") + (waited_long ? "long" : "short") + ") " + std::to_string(double(n_ - exp) / cfg_.fs) + " s after the packet's start");
	p2_fut_slot_ = exp; p2_fut_long_ = waited_long;
	p2_launch_wall_ = std::chrono::steady_clock::now();
	const bool have_tm = p2_sync_fut_.valid() && p2_sync_slot_ == exp;
	const int64_t from = exp - samples(0.04) - int64_t(2 * T_), to = exp + samples(waited_long ? 3.36 : 0.86) + int64_t(2 * T_) + 1;
	std::shared_future<P2Timing> sf = have_tm ? p2_sync_fut_ : std::shared_future<P2Timing>();
	const auto look = p2_launch_wall_;
	const bool dbg = debug_;
	MarqSnap ms;
	if (marq_on() && marq_n_ > 0) { ms.sum = marq_sum_; ms.level = marq_level_; ms.n = marq_n_; ms.ver = marq_ver_; }
	auto done = p2_done_ = std::make_shared<std::atomic<int64_t>>(0);
	auto sok = p2_short_ok_ = std::make_shared<std::atomic<bool>>(false);   // fix 203
	// test only (fix 200): a slow decode. P2_TEST_DECODE_DELAY_MS=D more per decode launched from
	// P2_TEST_DELAY_AT s on (default 0), P2_TEST_DELAY_N of them (default all), long looks only
	// with P2_TEST_DELAY_LONG=1; P2_TEST_SLOW_PATH_MS=D: D ms more for every decode that did
	// not come from the predicted tracked candidate (a CPU on which only that path is in time,
	// as the Pi 4 at level 4 long: ~20 ms against 55-140 ms).
	static const int t_dly = std::getenv("P2_TEST_DECODE_DELAY_MS") ? std::atoi(std::getenv("P2_TEST_DECODE_DELAY_MS")) : 0;
	static const double t_at = std::getenv("P2_TEST_DELAY_AT") ? std::atof(std::getenv("P2_TEST_DELAY_AT")) : 0.0;
	static int t_left = std::getenv("P2_TEST_DELAY_N") ? std::atoi(std::getenv("P2_TEST_DELAY_N")) : -1;
	static const bool t_long = std::getenv("P2_TEST_DELAY_LONG") != nullptr;
	static const int t_slow = std::getenv("P2_TEST_SLOW_PATH_MS") ? std::atoi(std::getenv("P2_TEST_SLOW_PATH_MS")) : 0;
	int dly = 0;
	if (t_dly > 0 && double(n_) / cfg_.fs >= t_at && t_left != 0 && (waited_long || !t_long)) {
		dly = t_dly; if (t_left > 0) t_left--;
		if (dbg) log("P2: test: this decode delayed " + std::to_string(dly) + " ms");
	}
	const TrkView tv = trk_view(exp);                 // fix 200: on the audio loop
	std::shared_ptr<p2::LsPre> lpre = waited_long && p2l_pre_slot_ == exp ? p2l_pre_ : nullptr;   // TODO o
	p2_fut_ = pool_.submit([this, exp, waited_long, sf, sp = snapshot(from, to), look, dbg, ms = std::move(ms), done, dly, tv, sok, lpre]() {
		using clk = std::chrono::steady_clock;
		const auto w0 = clk::now();
		P2Timing t;
		if (sf.valid()) t = sf.get();
		const auto w1 = clk::now();
		p2::ls_pre_tls = lpre.get();
		RxPacket r = find_packet(exp, false, waited_long, &sp, t.ok ? t.c : nullptr, &tv);
		p2::ls_pre_tls = nullptr;
		// test hook: P2_TEST_FIT_US=U, a long packet's fit as slow as
		// on one Pi 4 core: U microseconds per 256 of its samples summed now (not while it arrived)
		static const int fit_us = std::getenv("P2_TEST_FIT_US") ? std::atoi(std::getenv("P2_TEST_FIT_US")) : 0;
		if (fit_us > 0 && waited_long) {
			const double left = std::max(0.0, 3.40 * cfg_.fs - (lpre ? double(lpre->taken.load()) : 0.0));
			std::this_thread::sleep_for(std::chrono::microseconds(int64_t(fit_us * left / 256.0)));
		}
		if (dbg && lpre) std::fprintf(stderr, "P2 long decode: %.2f s of its fit's samples summed while it arrived\n", double(lpre->taken.load()) / cfg_.fs);
		if (!p3_) marq_ahead(r, ms);
		const int d2 = dly + (t_slow > 0 && r.path != 1 ? t_slow : 0);
		if (d2 > 0) std::this_thread::sleep_for(std::chrono::milliseconds(d2));
		if (dbg) std::fprintf(stderr, "P2 worker: start %.1f ms after the launch, timing wait %.1f ms, decode %.1f ms, ok %d, timing %d, path %s, swap %d pred %d, start-exp %.1f\n",
		                      std::chrono::duration<double, std::milli>(w0 - look).count(), std::chrono::duration<double, std::milli>(w1 - w0).count(),
		                      std::chrono::duration<double, std::milli>(clk::now() - w1).count(), int(r.ok), int(t.ok), kPathName[r.ok ? r.path : 0], r.swap, r.pred, r.ok ? r.start - double(exp) : 0.0);
		if (r.ok && !r.lng && !waited_long) sok->store(true);
		done->store(wall_ns());
		return r;
	});
}

void P2Station::advance_p2_long(int64_t exp)
{
	if (state_ != ST_IRS || p3_ || p2l_pre_slot_ != exp || !p2l_pre_) return;
	const TrkView tk = trk_view(exp);
	if (!tk.on) return;                              // the acquisition path: several timings, not this one
	P2Timing tm;
	if (p2_sync_fut_.valid() && p2_sync_slot_ == exp) {
		if (p2_sync_fut_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
		tm = p2_sync_fut_.get();
	}
	double c2[2];
	tracked_c2(exp, tk, tm.ok ? tm.c : nullptr, c2);
	const double fsh = packet_fshift(false);
	const LsArgs la = ls_args(exp - samples(0.04), exp + samples(3.36), c2[0], c2[1], cfg_.tone_lo + fsh, cfg_.tone_hi + fsh, 332);
	const long avail = long(n_ - la.base);
	if (avail <= 0) return;
	auto pre = p2l_pre_;
	const double fs = cfg_.fs;
	pool_.submit([pre, la, avail, fs, sp = snapshot(la.base, n_)]() {
		std::vector<float> y(la.ysize);
		for (size_t k = 0; k < y.size(); k++) y[k] = sp.at(la.base + int64_t(k));
		p2::ls_pulses_pre(*pre, y, fs, la.cA, la.cB, la.fA, la.fB, la.n, avail);
	});
}

void P2Station::launch_p2_sync(int64_t exp)
{
	const int64_t from = exp - samples(0.04), to = exp + samples(kP2Sync);
	p2_sync_slot_ = exp;
	// fix 206: the timing mixed at the tones find_packet demodulates at
	static const bool no_dafc = std::getenv("P2_NO_DEMOD_AFC") != nullptr;
	const double fsh = timing_shift((demod_afc_on() && !no_dafc && std::fabs(afc_hz()) > 1.0) ? afc_hz() : 0.0);
	p2_sync_fut_ = pool_.submit([this, from, fsh, sp = snapshot(from, to)]() {
		P2Timing r;
		std::vector<float> x(sp.v);
		double e = 0; for (float v : x) e += double(v) * v;
		if (x.empty() || e / double(x.size()) < 1e2) return r;
		r.ok = timing_of(x, from, r.c[0], r.c[1], fsh);
		return r;
	}).share();
}

void P2Station::on_packet(int64_t exp, bool waited_long, bool relook)
{
	fs_poll();                                     // a frequency search finished since (2026-11-14)
	if (state_ != ST_IRS) return;
	if (p3_ && qrt_rx_ && n_ < exp + samples(0.88)) { schedule(exp + samples(0.88), EV_PACKET, exp, (waited_long ? 1 : 0) | (relook ? 2 : 0)); return; }   // fix 79: the end-packet test reads the whole packet
	if (p3_ && waited_long) { on_p3_long(exp); return; }
	// Still on the air 0.86 s after its start: a long packet (3.3 s).
	// Fix 55: only once we asked for long packets (R): the sender sends
	// them only then. The power test alone (an absolute 1e5, ~316 rms)
	// took every packet for long on the air, where the band noise is
	// ~2,700 rms (the reference unit has none, the bench receiver ~280): we
	// waited 2.5 s more and every reply was due 2.5 s in the past, never
	// sent (KB5LZK 30 m and KB2PCN-8 80 m, P2, 2026-10-16).
	// TODO 0e (P2 side): before the first good packet a burst still on 0.86 s
	// into the slot, 6 dB over the gap before it, is the caller's set-up
	// packet again, not a short PSK packet (long ones come only after our R):
	// the firmware through CCIR poor fading repeated it for the whole link
	// while we waited for PSK. P2_NO_SETUP_AGAIN: the old way (test).
	if (!p3_ && p2_setup_watch_ && !waited_long && !relook && !std::getenv("P2_NO_SETUP_AGAIN")) {
		const double late = window_power(exp + samples(0.835), exp + samples(0.858)), gap = window_power(exp - samples(0.12), exp - samples(0.05));
		if (late > 4.0 * std::max(gap, 1e2)) {
			// Only if the packet does not decode (below): a real reference unit's first
			// PSK packets were sometimes still on the air there, and good
			// decodes were thrown away every other cycle; our answers then
			// came on a broken lattice and the link failed (6 m bench,
			// 2026-10-20: 3 of 17 runs with the TNC calling).
			// P2_SETUP_AGAIN_FIRST: the old order (test).
			if (debug_) log("P2: a burst still on at +0.86 s before the first packet: the set-up packet again?");
			if (std::getenv("P2_SETUP_AGAIN_FIRST")) { schedule(exp + samples(0.955), EV_P1_AGAIN, exp); return; }
			setup_again_exp_ = exp;
		}
	}
	const bool long_possible = rx_long_ || (p2_up_pending_ && p2_req_cs_ == p2arq::CS_R);
	// Fix 198: the power still on the air is also compared with the packet's own (0.05-0.75 s): a
	// short packet has ended by now (0.83 s). The absolute 1e5 alone is under a radio's band noise
	// between bursts (the clean side's RF bench: ~2e5), so once long packets were possible every
	// short packet was waited for as long: after our R to go short on a long link the real reference unit
	// was on the short cycle at once and we heard (and answered) only every third copy, nothing
	// printed for the rest of the link [bb-clean H31, its P2_SHORT_IN_LONG part (a), ported].
	// Still on the air at a quarter of the packet's power or more: long, as before.
	// P2_SHORT_IN_LONG=0: the absolute test only.
	static const bool short_in_long = !std::getenv("P2_SHORT_IN_LONG") || std::atoi(std::getenv("P2_SHORT_IN_LONG")) > 0;
	const double look_pw = recent_power(0.015);
	const bool short_by_power = short_in_long && !p3_ && !waited_long && !relook && long_possible && look_pw > 1e5 &&
	                            look_pw < 0.25 * window_power(exp + samples(0.05), exp + samples(0.75));
	if (debug_ && short_by_power) log("P2: still on the air at +0.86 s at under a quarter of the packet's power: a short packet");
	// Fix 203: the short look's own decode, launched at 0.845 s and done in a few ms, decides first:
	// a short packet that decoded (header and CRC) is one, whatever the power still on the air.
	// It was set aside for the long look (real reference unit, a radio's band noise over the absolute
	// test and fix 198 not yet in): the long look, 2.5 s later, decoded the short packet after its
	// answer was due, counted a failure and sent T, three slots in a row (L3 to L1).
	// P2_NO_SHORT_FIRST=1: off (test).
	static const bool short_first_off = std::getenv("P2_NO_SHORT_FIRST") != nullptr;
	const bool short_decoded = !short_first_off && cfg_.realtime && !p3_ && !waited_long && !relook && long_possible && p2_fut_slot_ == exp &&
	                           !p2_fut_long_ && p2_fut_.valid() && p2_short_ok_ && p2_short_ok_->load();
	if (debug_ && short_decoded && look_pw > 1e5 && !short_by_power) log("P2: a short packet decoded at the short look: not waiting for a long one");
	if (!p3_ && !waited_long && !relook && long_possible && look_pw > 1e5 && !short_by_power && !short_decoded) {
		schedule(exp + samples(3.36), EV_PACKET, exp, 1);
		// fix 73: the long packet's decode as soon as it is in, too (its reply was
		// due ~1 ms after the look; N5TW 40 m, 2026-10-17 00:13: all late)
		if (cfg_.realtime && !std::getenv("P2_NO_EARLY"))
			schedule(exp + samples(kP2Launch + 2.5) + (trk_ok_ ? int64_t(std::max(0.0, trk_dt_)) : 0), EV_P2_LAUNCH, exp, 1);
		if (p2_long_pre_on()) {                     // its fit in pieces while it arrives (TODO o)
			p2l_pre_ = std::make_shared<p2::LsPre>(); p2l_pre_slot_ = exp;
			for (double at : kP2LPre) schedule(exp + samples(at), EV_P2L_PRE, exp);
		}
		return;
	}
	if (qrt_rx_) {                                  // QRT acknowledged: expect the end packet
		const Demod d = demod_cs(exp - samples(0.04), exp + samples(0.86), 84, nullptr, nullptr, afc_);
		std::vector<std::array<double, 2>> st;
		bool end = false;
		// Fix 66: the alternating body scored softly (mean cos of each step's
		// distance from a half turn after the previous one: bias-free, ~0 in
		// noise over ~118 terms), at the tracked timing first. The hard test
		// below missed KB5LZK's end packets (30 m, 2026-10-16 18:14) and the
		// link ran on until Pat's DD.
		auto alt_soft = [&](const Demod &dd) {
			double best = -1;
			for (int swap = 0; swap < 2; swap++)
				for (int o = -6; o <= 2; o++) {
					const auto &A = swap ? dd.hi : dd.lo, &B = swap ? dd.lo : dd.hi;
					double prev[2] = {0, 0}, sum = 0; int n = 0;
					for (int j = 0; j < 60; j++) {
						const int i1 = 1 + o + 12 + j;
						if (i1 < 1 || i1 >= int(A.size()) || i1 >= int(B.size())) break;
						const double v[2] = {nib(A[size_t(i1 - 1)], A[size_t(i1)]), nib(B[size_t(i1 - 1)], B[size_t(i1)])};
						if (j > 0) for (int c = 0; c < 2; c++) { sum += std::cos(2 * M_PI * (v[c] - prev[c] - 8.0) / 16.0); n++; }
						prev[0] = v[0]; prev[1] = v[1];
					}
					if (n > 80) best = std::max(best, sum / n);
				}
			return best;
		};
		if (!std::getenv("P2_NO_ENDSOFT")) {
			double sc = -1;
			if (trk_ok_ && track_on()) {
				const double lead = double(exp) + trk_dt_ + trk_lead_, lag = lead + trk_stag_;
				for (int sw = 0; sw < 2 && sc < 0.45; sw++) {
					const double c2[2] = {sw ? lag : lead, sw ? lead : lag};
					const Demod dp = demod_cs(exp - samples(0.04), exp + samples(0.86), 84, nullptr, c2, afc_);
					if (dp.ok) sc = std::max(sc, alt_soft(dp));
				}
			}
			if (sc < 0.45 && d.ok) sc = std::max(sc, alt_soft(d));
			if (debug_) log("end packet soft score " + std::to_string(sc));
			end = sc >= 0.45;
		}
		// The alternating body, wherever it starts (the first pulses may
		// have been lost to our own transmit-to-receive turnaround).
		for (int swap = 0; swap < 2 && !end && d.ok; swap++)
			for (int o = -6; o <= 2 && !end; o++) {
				const auto &A = swap ? d.hi : d.lo;
				std::vector<std::array<double, 2>> body;
				for (int j = 0; j < 60; j++) {
					const int i1 = 1 + o + 12 + j;
					if (i1 < 1 || i1 >= int(A.size())) break;
					const auto &B = swap ? d.lo : d.hi;
					body.push_back({nib(A[size_t(i1 - 1)], A[size_t(i1)]), nib(B[size_t(i1 - 1)], B[size_t(i1)])});
				}
				// alternating 1/0: every step is a half turn (8/16) away from
				// the previous, i.e. pattern "a2a2..." or "2a2a..." with any bias.
				int good = 0;
				for (size_t j = 1; j < body.size(); j++)
					for (int c = 0; c < 2; c++) {
						const double dd = std::fabs(std::fmod(body[j][size_t(c)] - body[j - 1][size_t(c)] + 24.0, 16.0) - 8.0);   // 0..8, want 8
						good += dd > 6.0;
					}
				end = body.size() > 40 && good >= int(0.8 * 2 * (body.size() - 1));
			}
		if (end) {
			ack_phase_ ^= 1;
			send_cs(ack_phase_ ? p2arq::CS_Q : p2arq::CS_P, cs_at(exp, false));
			link_status(false, " (QRT received)");
			log("QRT received");
			const auto keep = tx_;
			disconnect_quiet();
			tx_ = keep;
			return;
		}
	}
	if (p3_) {
		// Never block the audio loop on the decode: if it is not done yet,
		// look again every 10 ms until shortly before our answer is due,
		// then count the packet as failed (a blocking wait here caused
		// playback underruns on the RF bench).
		reap_stale_decodes();
		// our answer starts at reply_at (callee P3: 0.900 s after the packet's
		// start, only ~20 ms after this first look), so wait up to just before it
		const int64_t step = samples(0.002);
		// fix 79: never give up before the old first look (0.88 s): a late
		// answer (its start cut off) with the right letter beats a failure
		const int64_t give_up = std::max(reply_at(exp, false) - samples(0.003), exp + samples(0.88));
		const bool can_wait = n_ + step < give_up;
		if (p3_res_slot_ != exp) {             // first look at this packet
			bool late = false;
			if (cfg_.realtime && p3_fut_.valid() && p3_fut_slot_ == exp &&
			    p3_fut_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
				if (can_wait) { schedule(n_ + step, EV_PACKET, exp, (waited_long ? 1 : 0) | 2); return; }
				stale_futs_.push_back(std::move(p3_fut_));
				late = true;
				note_late(p3_rx_level_, true);
				if (p3_times_) { late_watch_ = p3_times_; late_watch_dl_ = wall_ns(); late_watch_lv_ = p3_rx_level_; late_watch_done_ = p3_done_; }
				if (debug_) log("P3: decode not ready in time, counted as failed (" + std::to_string(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - p3_launch_wall_).count()) + " ms since launch)");
			}
			const bool measured = cfg_.realtime && !late && p3_fut_.valid() && p3_fut_slot_ == exp && p3_done_;
			const auto done_at = p3_done_;
			p3_res_ = late ? p3::RxPacket{} : find_p3(exp, 2.0);   // normally launched already (EV_P3_LAUNCH)
			if (measured && done_at->load()) note_margin(p3_res_.ok && p3_res_.hscore >= 1 ? int(p3_res_.hscore) : p3_rx_level_, give_up, done_at->load(), true);
			p3_res_late_ = late;
			p3_res_slot_ = exp;
			if (debug_ && !late) log("P3: decode collected " + std::to_string(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - p3_launch_wall_).count()) + " ms after launch");
		}
		p3::RxPacket r = p3_res_;
		// A decode whose soft values agree poorly with its own codeword is a chance CRC pass:
		// every packet is tried at several levels, maps and templates, dozens of CRC checks
		// (the clean session's E31, 2026-11-02: noise decoded as an SL3 packet with "over",
		// the link lost). agree = -path metric / sum |soft| (1: every coded bit agrees).
		// P3_AGREE_MIN at the expected level, P3_AGREE_MIN_OTHER at another (calibration below).
		if (r.ok && r.hscore >= 2) {
			static const double agree_min = std::getenv("P3_AGREE_MIN") ? std::atof(std::getenv("P3_AGREE_MIN")) : 0.0;
			static const double agree_other = std::getenv("P3_AGREE_MIN_OTHER") ? std::atof(std::getenv("P3_AGREE_MIN_OTHER")) : 0.0;
			const int lv = int(r.hscore);
			const double ag = lv == 5 ? r.r.agree : r.agree;
			if (std::getenv("P3_AGREE_LOG")) log("rx P3 agree " + std::to_string(ag) + " SL" + std::to_string(lv) + (lv == p3_rx_level_ ? " expected" : " other") + (r.marq ? " marq" : ""));
			if (ag < (lv != p3_rx_level_ ? agree_other : agree_min)) { log("P3: decode at SL" + std::to_string(lv) + " rejected, agreement " + std::to_string(ag)); r.ok = false; }
		}
		if (r.ok) { marq_clear(); mq3_clear(); }       // a good packet at a P3 level ends any combining
		if (r.ok && r.marq && p3_res_slot_ == exp) {
			log("P3: memory ARQ: decoded at SL" + std::to_string(int(r.hscore)) + " from " + std::to_string(r.marq) + " copies");
			led_marq_ = 1;                                 // front panel MARQ-OK
		}
		const bool late = p3_res_late_;
		// Speed level 1 (two-carrier P2 L1 + scrambler)? Tried after every
		// failed packet (cheap insurance should our level tracking be off).
		// In real time the decode runs on a worker (on the audio loop it
		// stalled playback for every failed packet).
		// A late main decode no longer throws away an SL1 try that is done (fix 242, a Signum
		// change; no effect on what is sent). Expecting SL1 the main decode is the SL5 try (a
		// speed-up taken), the dearest of all: on a starved computer it came late on every packet,
		// each packet counted as failed without a look at its SL1 decode, already done on its
		// worker, and the link timed out at SL1 (two Signums on the ALSA loopback, 2026-11-13:
		// SL5 -> SL1 by computer back-off, then nothing decoded for 89 s). The late SL1 try is
		// only taken if ready: no new decode, no wait. P3_LATE_SL1_OLD=1: as before.
		static const bool late_sl1_old = std::getenv("P3_LATE_SL1_OLD") != nullptr;
		if (!r.ok && late && !late_sl1_old && cfg_.realtime && sl1_slot_ == exp && sl1_fut_.valid() &&
		    sl1_fut_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
			RxPacket q = sl1_fut_.get();
			if (p3_rx_level_ == 1) q = marq(q, exp);
			else if (q.ok) marq_clear();
			if (q.ok && q.level == 1) {
				r.ok = true;
				std::memset(r.r.info, 0x1e, sizeof r.r.info);
				for (int i = 0; i < p3::kSl1Data; i++) r.r.info[i] = q.info[size_t(i)];
				r.r.info[p3::kDataBytes] = q.info[p3::kSl1Data];
				r.t0 = q.start + 0.8 * T_ + kP3Off * cfg_.fs;
				r.hscore = 1;
				load_.sl1_rescue++;
				if (!late_sl1_logged_) { late_sl1_logged_ = true; log("P3: main decode late, the packet taken from its SL1 decode (the computer is too slow for the other levels' tries)"); }
			}
		}
		if (!r.ok && !late) {
			RxPacket q;
			if (cfg_.realtime) {
				if (sl1_slot_ != exp) {
					sl1_slot_ = exp;
					sl1_fut_ = submit_sl1(exp);
				}
				if (sl1_fut_.valid() && sl1_fut_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
					if (can_wait) { schedule(n_ + step, EV_PACKET, exp, (waited_long ? 1 : 0) | 2); return; }
					sl1_fut_ = {};                     // a pool future: dropping it does not block
					if (cfg_.paced) dstats_.late_sl1++;
					if (debug_) log("P3: SL1 decode not ready in time, counted as failed");
				} else if (sl1_fut_.valid()) q = sl1_fut_.get();
			} else q = find_packet(exp, false, false);
			if (p3_rx_level_ == 1) q = marq(q, exp);     // memory ARQ over failed SL1 copies (not while expecting another level)
			else if (q.ok) marq_clear();
			if (q.ok && q.level == 1) {
				r.ok = true;
				std::memset(r.r.info, 0x1e, sizeof r.r.info);
				for (int i = 0; i < p3::kSl1Data; i++) r.r.info[i] = q.info[size_t(i)];
				r.r.info[p3::kDataBytes] = q.info[p3::kSl1Data];
				r.t0 = q.start + 0.8 * T_ + kP3Off * cfg_.fs;   // so p3_slot() gives its slot
				r.hscore = 1;
			}
		}
		if (p3_rx_long_) {
			if (!r.ok) return;                         // a long packet is still on the air: its own look answers
			log(p3_r_pending_ ? "P3: R not taken, the sender stayed on short packets" : "P3: the sender went back to short packets");
			p3_rx_long_ = false; p3_r_pending_ = false;
			drop_long_looks(exp);
		} else if (p3_r_pending_) {                 // we asked for short packets: a long one may be on the air
			if (!r.ok) return;
			drop_long_looks(exp);
		}
		if (r.ok) {
			bk_rep_watch_ = false; bk_last_info_.clear();  // fix 270: a packet decoded: the peer took our answer
			p3_drift_note(double(p3_slot(r) - exp), misses_ + 1);   // fix 219 (before misses_ is cleared)
			p3_last_ok_level_ = int(r.hscore);
			p3_t_untaken_ = rx_t_state_ && int(r.hscore) > p3_rx_level_ && int(r.hscore) <= 6;   // fix 219 (before the level is taken)
			misses_ = 0;
			const uint8_t status = r.r.info[p3::kDataBytes];
			const int cnt = status & 3;
			const uint8_t *pd = r.lvdata.empty() ? r.r.info : r.lvdata.data();
			const int pn = r.lvdata.empty() ? p3::level_data_bytes(int(r.hscore)) : int(r.lvdata.size());
			const bool fresh = p3_fresh(cnt, int(r.hscore), pd, pn);
			rx_more_ = p3_rx_lots(status, int(r.hscore), false);
			if (status & 0x80) { if (std::getenv("P2_TEST_TAKEBACK")) must_break_in_ = true; else qrt_rx_ = true; }   // test: take the link back on a QRT, as an RMS did (fix 91)
			if (fresh) {
				repack_cand_ = cnt == last_rx_counter_; last_rx_counter_ = cnt; p3_note_rx(int(r.hscore), pd, pn);
				if ((status & 0x40) && !(status & 0x80)) must_break_in_ = true;   // fix 50
				if (!(status & 0x80)) {
					if (!r.lvdata.empty()) deliver(r.lvdata.data(), int(r.lvdata.size()), status);
					else deliver(r.r.info, p3::level_data_bytes(int(r.hscore)), status);
				}
			}
			if (r.hscore >= 1 && r.hscore <= 6 && int(r.hscore) != p3_rx_level_) { p3_rx_level_ = int(r.hscore); log("P3: receiving level " + std::to_string(p3_rx_level_)); p3_resid_n_ = 0; }
			if (int(r.hscore) >= 2 && !r.marq) { p3_resid_ = p3_resid_n_ ? 0.7 * p3_resid_ + 0.3 * r.resid : r.resid; p3_resid_n_++; }   // fix 173
			p3_q_ = r.marq ? -1 : r.resid; p3_q_lv_ = int(r.hscore); p3_q_long_ = false;   // fix 229 (memory ARQ: no reading)
			// residual after our shift (fix 49; fix 70: every level, not SL5
			// only: SL4 fails 10 Hz off, SL3 20 Hz, and between break-ins nothing
			// else kept the shift current on a drifting link)
			if (std::fabs(r.df) < 12.0 && (int(r.hscore) == 5 || !std::getenv("P3_DF_SL5_ONLY"))) { p3_rx_df_ += 0.5 * r.df; tx_est_feed(-p3_rx_df_, "P3 packet"); }
			fs_decoded(int(r.hscore));                    // fix 261/263: a shift a decode confirmed
			if (debug_ && std::getenv("P3_DF_DEBUG")) log("P3 df " + std::to_string(r.df) + " shift now " + std::to_string(p3_rx_df_) + " SL" + std::to_string(int(r.hscore)));
			if (debug_ && std::getenv("P3_LOG_DATA")) {      // dev: the data of every decoded packet, new or not
				std::string h; char hb[4];
				const int nb = r.lvdata.empty() ? p3::level_data_bytes(int(r.hscore)) : int(r.lvdata.size());
				for (int i = 0; i < std::min(nb, 12); i++) { std::snprintf(hb, sizeof hb, "%02x", r.lvdata.empty() ? r.r.info[i] : r.lvdata[size_t(i)]); h += hb; }
				log("rx P3 data " + h + (fresh ? " (new)" : " (repeat)"));
			}
			if (p3_since_fail_ < (1 << 20)) p3_since_fail_++;
			if (debug_) log("rx P3 status " + std::to_string(status) + " level " + std::to_string(p3_rx_level_) + " decoded at SL" + std::to_string(int(r.hscore)) + " resid " + std::to_string(r.resid) +
			                " slot error " + std::to_string(double(p3_slot(r) - exp) / cfg_.fs * 1000.0) + " ms" +
			                (int(r.hscore) == 5 ? " df " + std::to_string(r.df) + " Hz" : std::string()));
			reply_as_irs(p3_slot(r), false, fresh, true);
		} else {
			if (debug_) log("rx P3 failed hdr " + std::to_string(r.hscore));
			p3_since_fail_ = 0;
			fs_submit(exp, exp - samples(0.02), exp + samples(0.86), 0);   // the frequency search (2026-11-14)
			if (++misses_ > cfg_.timeout_cycles) { disconnect("timeout"); return; }
			if (p3_bk_repeat(exp)) return;                 // fix 270: the peer's break-in again
			// memory ARQ: keep this copy (at the expected level) for the repeat
			if (r.mq_level && r.mq_level == p3_rx_level_) {
				mq3_.push_back({r.mq_level, r.mq_d0, r.mq_d1});
				if (mq3_.size() > kMq3Max) mq3_.erase(mq3_.begin());
			}
			const int lv_before = p3_rx_level_;
			reply_as_irs(exp, false, false, false);
			// A new expected level: the sender re-packs (copies of another level
			// would be filtered out anyway). Not on every failure in our T
			// state (until 2026-10-18): between its step-downs (the first T,
			// then every 6th) the sender repeats the same packet at the same
			// level, and those copies are what combining needs (emulator,
			// noise 2500: no SL4 packet decoded in 6 tries before).
			if (p3_rx_level_ != lv_before && !std::getenv("P3_MQ_CLEAR_ON_T")) mq3_clear();
			else if (std::getenv("P3_MQ_CLEAR_ON_T") && (p3_rx_level_ != lv_before || rx_t_state_)) mq3_clear();   // test: the old rule
		}
		return;
	}
	RxPacket p;
	{
		// the early timing, if there is one for this packet
		P2Timing tm;
		const bool have_tm = p2_sync_fut_.valid() && p2_sync_slot_ == exp;
		// Fix 53: the decode of the previous copy, finished after its reply
		// was due (we asked for a repeat), serves for this repeat: as caller
		// our reply instant leaves the decoder only ~10-40 ms after a packet
		// on the air (our ~50 ms audio round trip plus the path's ~35 ms),
		// so slow decodes failed every cycle (KB2PCN-8, 80 m, 2026-10-16;
		// reproduced in real time over the ALSA loopback with +40 ms).
		// Only after a plain repeat request (P/Q): after T/S/R the sender
		// re-packs the information differently.
		bool late_used = false;
		p2_late_poll();
		// The late copy serves the slot after its own, after a plain repeat request, at the
		// level expected. Its carrier order is not this copy's (the sender alternates them
		// every cycle): our answer's is left to the toggle (fix 168), as after memory ARQ.
		const bool late_ok = cfg_.realtime && late_slot_ >= 0 && late_.ok && exp > late_slot_ && late_.level == level_
		                     && (last_tx_cs_ == p2arq::CS_P || last_tx_cs_ == p2arq::CS_Q);
		auto take_late = [&](const char *what) {
			p = late_; p.start += double(exp - late_slot_); p.has_c = false; p.swap = -1; late_used = true;
			log(what);
		};
		if (late_ok && !late_.lng && !waited_long) take_late("P2: the previous copy decoded late: used for this repeat");
		// Fix 200: a long packet's too. After P/Q the sender repeats the packet unchanged; the
		// repeat is decoded as usual and the late copy taken only when that decode is not in
		// time or fails. Before, a late long decode was never used: one slow long decode, and
		// every repeat of that packet had to decode in time again (Pi 4, real reference unit, L4 long:
		// the repeats decoded too, 55-65 ms, and were late too: T to level 1, 60 s without a
		// packet). P2_NO_LATE_LONG=1: off (test).
		static const bool late_long_off = std::getenv("P2_NO_LATE_LONG") != nullptr;
		const bool late_long_ok = late_ok && late_.lng && waited_long && !late_long_off;
		if (late_used) {
		} else if (cfg_.realtime) {
			const int64_t step = samples(0.002);
			const bool can_wait = n_ + step < cs_deadline(exp, waited_long);
			if (p2_fut_slot_ != exp || !p2_fut_.valid() || p2_fut_long_ != waited_long) p2_launch(exp, waited_long);
			(void)have_tm;
			if (p2_fut_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
				if (can_wait) { schedule(n_ + step, EV_PACKET, exp, (waited_long ? 1 : 0) | 2); return; }
				p2_late_ = std::move(p2_fut_); p2_late_slot_ = exp;   // kept: it may serve for the repeat (fix 53)
				p2_late_reuse_ = !late_long_ok;               // not when this slot is answered from the late copy: the next one is a new packet
				note_late(rx_level_, false);
				if (late_long_ok) take_late("P2: decode not ready in time; the previous copy, decoded late, used for this repeat (long)");
				else if (debug_) log("P2: decode not ready in time, counted as failed");
			} else {
				p = p2_fut_.get();
				if (p2_done_ && p2_done_->load()) note_margin(p.ok ? p.level : rx_level_, cs_deadline(exp, waited_long), p2_done_->load(), false);
				if (debug_) log("P2: decode collected " + std::to_string(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - p2_launch_wall_).count()) + " ms after the look");
				if (!p.ok && late_long_ok) take_late("P2: this copy failed; the previous copy, decoded late, used for this repeat (long)");
			}
		} else {
			if (have_tm) tm = p2_sync_fut_.get();
			p = find_packet(exp, false, waited_long, nullptr, tm.ok ? tm.c : nullptr);
		}
	}
	// Fix 203: a short packet found by the long look. Its answer was due 2.5 s ago and the sender,
	// having heard nothing since, repeats it every short cycle: nothing is answered for the slots
	// already past (the answer went out 2.5 s late and the slots after it, already past too,
	// were each counted a failure: "send CS P at -2495 ms", then T, and the sender repacked a
	// packet we had printed but it never saw acknowledged; real reference unit, rb2-5). The packet is not
	// delivered either (its repeat is, and acknowledged in its slot): the look moves to the next
	// short-cycle slot still ahead, the slots between counted as the sender's transmissions.
	// P2_NO_LATE_SHORT_SKIP=1: as before (test).
	static const bool skip_off = std::getenv("P2_NO_LATE_SHORT_SKIP") != nullptr;
	if (!skip_off && !p3_ && waited_long && p.ok && !p.lng && cs_at(int64_t(p.start), false) + samples(hunt_s_) < n_ + samples(0.004)) {
		const int64_t cyc = samples(cyc_s());
		int64_t nx = int64_t(p.start) + cyc;
		int skipped = 0;
		while (nx + samples(0.86) <= n_) { trk_seq_of(nx, true); nx += cyc; skipped++; }
		trk_update(p, exp);
		trk_reframe(int64_t(p.start) - exp, exp);      // fix 247
		rx_long_ = false;
		p2_decided_ = std::max(p2_decided_, exp);
		if (late_slot_ >= 0 && exp > late_slot_) late_slot_ = -1;
		log("P2: the long look found a short packet, its answer long past: not answered; " + std::to_string(skipped) + " slot(s) passed, next look in " +
		    std::to_string(double(nx + samples(0.86) - n_) / cfg_.fs) + " s");
		schedule(nx + samples(0.86), EV_PACKET, nx);
		return;
	}
	if (late_slot_ >= 0 && exp > late_slot_) late_slot_ = -1;   // fix 200: the late copy served (or not) the slot after it
	p2_decided_ = std::max(p2_decided_, exp);
	p = marq(p, exp);                                  // memory ARQ over failed copies
	if (p.ok && p2_setup_watch_ && !p3_) {           // test hook (fix 222): P2_TEST_FIRST_MISS=N, the caller's first N good packets taken as failed
		static int fm = 0;
		if (const char *e = std::getenv("P2_TEST_FIRST_MISS"); e && fm < std::atoi(e)) { fm++; p.ok = false; log("test: the caller's first packet taken as failed"); }
	}
	if (!p.ok && setup_again_exp_ == exp && p2_setup_watch_) { setup_again_exp_ = -1; schedule(exp + samples(0.955), EV_P1_AGAIN, exp); return; }
	if (p.ok) {
		p2_setup_watch_ = false;
		afc_update(p.bias, afc_data_mod());
		misses_ = 0;
		rx_level_ = p.level;
		level_ = p.level;
		const int n = p2::data_bytes(p.level, p.lng ? 40 : 9);
		const uint8_t status = p.info[size_t(n)];
		const int cnt = status & 3;
		const bool fresh = p3_fresh(cnt, p.level + (p.lng ? 10 : 0), p.info.data(), n);   // fix 84, also in P2 (122 copies of one new L1 packet answered T)
		rx_more_ = (status & 0x20) != 0;
		rx_long_ = p.lng;
		if (status & 0x80) { if (std::getenv("P2_TEST_TAKEBACK")) must_break_in_ = true; else qrt_rx_ = true; }   // test: take the link back on a QRT, as an RMS did (fix 91)
		if (fresh) {
			repack_cand_ = cnt == last_rx_counter_; last_rx_counter_ = cnt; p3_note_rx(p.level + (p.lng ? 10 : 0), p.info.data(), n);
			if (!(status & 0x80)) deliver(p.info, n, status);
			good_run_++;
			if (status & 0x40) must_break_in_ = true;
		} else if (cnt == last_rx_counter_ && p.level + (p.lng ? 10 : 0) != p3_last_lv_ && !(status & 0x80)) repack_seen(p.info.data(), n, status, p.level + (p.lng ? 10 : 0));
		if (debug_) log("rx L" + std::to_string(p.level) + (p.lng ? " long" : "") + " status " + std::to_string(status) + (p.swap >= 0 ? (p.swap ? " lead hi" : " lead lo") : "") + " metric " + std::to_string(p.metric) + (p.marq_n ? " marq " + std::to_string(p.marq_n) : ""));
		ans_swap_ = p.marq_n == 0 ? p.swap : -1;      // fix 168: the carrier order of the packet we answer
		prev_rx_metric_ = last_rx_metric_;
		last_rx_metric_ = p.marq_n ? 99 : p.metric; last_rx_marq_ = p.marq_n;   // fix 217 (memory ARQ: not clean)
		trk_reframe(int64_t(p.start) - exp, exp);      // fix 247: the next slot is counted from this start
		reply_as_irs(int64_t(p.start), p.lng, fresh, true);
	} else {
		good_run_ = 0;
		ans_swap_ = -1;
		p2_quick_s_ = false;                        // fix 217: only the first packet after the break-in
		if (!p3_ && !p2_setup_watch_) fs_submit(exp, exp - samples(0.02), exp + samples(waited_long ? 1.3 : 0.86), 0);   // the frequency search (2026-11-14)
		if (++misses_ > cfg_.timeout_cycles) { disconnect("timeout"); return; }
		// Fix 218: before the caller's first good packet the switch to P2 is not
		// confirmed; the reference unit as callee answers such a slot with its P2-accept
		// again, not a PSK control signal (as in P3, on_p3 case 13). P2_SETUP_NAK=1: the old way.
		if (p2_setup_watch_ && !waited_long && !std::getenv("P2_SETUP_NAK")) { p2_accept_again(exp, "no good first packet"); return; }
		// Packet missing or bad: repeat our last CS (in our slot for it:
		// after a long wait the long packet's slot, fix 55).
		reply_as_irs(exp, waited_long, false, false);
	}
}

}  // namespace fmodem
