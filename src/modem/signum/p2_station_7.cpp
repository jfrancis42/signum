// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// P2Station member functions, part 7 of 8 (split out of p2_station.hpp so they build in parallel).
#include "p2_station.hpp"

namespace fmodem {

double P2Station::pattern_soft(const std::vector<std::array<double, 2>> &st, const char *pat, double prior)
{
	const int len = int(std::strlen(pat));
	const double bias = pattern_bias(st, pat, len, prior);
	double acc = 0; int n = 0;
	for (int i = 0; i < len && i < int(st.size()); i++)
		for (int c = 0; c < 2; c++) {
			const double want = pat[i] == 'a' ? 10.0 : 2.0;
			acc += std::cos(2 * M_PI * (st[size_t(i)][size_t(c)] - bias - want) / 16.0); n++;
		}
	return n ? acc / n : -1;
}

double P2Station::bk_soft(const Demod &d, int o, double prior, double *bias_out) const
{
	constexpr int kBkScored = 20;
	std::complex<double> acc = 0;
	for (int w = 1; w <= kBkScored; w++) {
		const int i1 = 1 + o + w;
		if (i1 < 1 || i1 >= int(d.lo.size()) || i1 >= int(d.hi.size())) return -1;
		for (int c = 0; c < 2; c++) {
			const auto &Z = c ? d.hi : d.lo;
			const double want = p2arq::kBkPattern[w - 1] == 'a' ? 10.0 : 2.0;
			acc += std::polar(1.0, 2 * M_PI * (nib(Z[size_t(i1 - 1)], Z[size_t(i1)]) - prior - want) / 16.0);
		}
	}
	double bias = 16.0 * std::arg(acc) / (2 * M_PI);
	if (std::fabs(bias) > 7.0) bias = 0;
	bias += prior;
	if (bias_out) *bias_out = bias;
	double sum = 0; int n = 0;
	for (int w = 1; w <= kBkScored; w++) {
		const int i1 = 1 + o + w;
		for (int c = 0; c < 2; c++) {
			const auto &Z = c ? d.hi : d.lo;
			const double want = p2arq::kBkPattern[w - 1] == 'a' ? 10.0 : 2.0;
			sum += std::cos(2 * M_PI * (nib(Z[size_t(i1 - 1)], Z[size_t(i1)]) - bias - want) / 16.0); n++;
		}
	}
	return n ? sum / n : -1;
}

auto P2Station::cs_pred(int64_t exp, int k) -> CsPred
{
	CsPred p;
	CsTrk &T = cstk_[k];
	p.trk = k;
	if (exp != T.last_exp) { T.last_exp = exp; T.calls++; }
	// long cycles: off by default (fix 72: better detection, no throughput gain;
	// P2_LONGTRACK=1 turns it on)
	static const bool long_on = std::getenv("P2_LONGTRACK") != nullptr;
	if (!T.ok || std::getenv("P2_NO_CSTRACK") || (k == 1 && !long_on)) return p;
	p.ok = true;
	const double lead = double(exp) + T.dt + T.lead, lag = lead + T.stag;
	p.c[0][0] = lead; p.c[0][1] = lag;               // swap 0: the low carrier leads
	p.c[1][0] = lag;  p.c[1][1] = lead;
	p.first = T.swap ^ (T.calls & 1);                // the leading carrier alternates per transmission
	p.start = double(exp) + T.dt;
	return p;
}

void P2Station::cs_track(const ReplyRx &r, bool soft)
{
	if (r.bk || (soft ? !r.has_c : !r.has_hc)) return;
	// Fix 71: only trackable looks teach the track, the first lock too: set-up
	// replies have other timings (a lock taken from the A2 answer put every
	// data-phase look in the wrong place; emulator, noise 6000 both ways: 18 s
	// without a detection). Fix 72: each kind of cycle its own track.
	if (!r.ctx) return;
	CsTrk &T = cstk_[r.trk];
	const double st = soft ? r.soft_start : r.start;
	const double *c = soft ? r.c : r.hc;               // the timing of the decision taken
	const double dt = st - double(r.exp), lead = std::min(c[0], c[1]) - st, stag = std::fabs(c[1] - c[0]);
	const int sw = c[1] < c[0] ? 1 : 0;
	const bool off = T.ok && (std::fabs(dt - T.dt) > 0.6 * T_ || std::fabs(stag - T.stag) > 0.3 * T_);
	if (std::getenv("P2_CSTRACK_LOG")) log("cs track " + std::to_string(r.trk) + (soft ? " soft" : " hard") + ": dt " + std::to_string(dt / cfg_.fs * 1e3) + " ms (track " +
	                                       std::to_string(T.dt / cfg_.fs * 1e3) + "), lead " + std::to_string(lead / cfg_.fs * 1e3) + ", stag " + std::to_string(stag / cfg_.fs * 1e3) +
	                                       (r.tracked_look ? " tracked" : "") + (off ? " OFF" : ""));
	const bool turn = T.turn_new;
	T.turn_new = false;
	if (off && !turn) {
		// a detection away from the track moves it only when a second one
		// agrees (one false detection in noise must not lose the lock)
		const bool agree = T.jump_ok && std::fabs(dt - T.jump_dt) < 0.3 * T_ && std::fabs(stag - T.jump_stag) < 0.3 * T_;
		T.jump_ok = true; T.jump_dt = dt; T.jump_stag = stag;
		if (!agree) return;
	}
	T.jump_ok = false;
	if (!T.ok || off) { T.dt = dt; T.lead = lead; T.stag = stag; }
	else { T.dt += 0.3 * (dt - T.dt); T.lead += 0.3 * (lead - T.lead); T.stag += 0.3 * (stag - T.stag); }
	T.swap = sw; T.calls = 0; T.ok = true;
}

auto P2Station::classify_reply(int64_t exp, bool want_bk, const Snap *sp, double afc, CsPred pred) const -> ReplyRx
{
	ReplyRx r;
	r.bias = afc;
	r.exp = exp;
	r.tracked_look = pred.ok;
	r.trk = pred.trk;
	const int64_t from = exp - samples(0.04), to = exp + samples(0.30);
	std::vector<std::array<double, 2>> sst;
	// soft scoring of one demodulation (all three word-0 positions)
	auto soft_score = [&](const Demod &dd, bool tracked) {
		// tracked: only the word-0 position of the predicted start
		const int ot = tracked ? std::max(0, std::min(2, 1 + int(std::lround((pred.start - dd.start) / T_)))) : 0;
		for (int o = tracked ? ot : 0; o <= (tracked ? ot : 2); o++) {
			steps_of(dd, false, o, 21, sst);
			double best = -2, second = -2; int bc = -1;
			for (int c = 0; c < p2arq::CS_COUNT; c++) {
				const double v = pattern_soft(sst, p2arq::kCsPattern[c], afc);
				if (v > best) { second = best; best = v; bc = c; } else if (v > second) second = v;
			}
			if (best > r.soft || (tracked && !r.soft_tracked)) {
				if (!tracked && r.soft_tracked) continue;          // a tracked look outranks the others
				r.soft = best; r.margin = best - second; r.soft_cs = bc; r.soft_tracked = tracked;
				r.soft_bias = pattern_bias(sst, p2arq::kCsPattern[bc], int(std::strlen(p2arq::kCsPattern[bc])), afc);
				r.soft_start = dd.start + (o - 1) * T_; r.c[0] = dd.c[0]; r.c[1] = dd.c[1]; r.has_c = true;
			}
		}
	};
	// Also when a break-in may come (we are the sender): at the tracked
	// position the break-in pattern scores ~-0.14 against every letter
	// (+0.43, margin 0.19, a pulse off), far below acceptance, so a
	// break-in falls through to bk_match below.
	if (pred.ok) {
		for (int k = 0; k < 1; k++) {                    // the predicted carrier order only (cost on the audio loop, P3)
			const int sw = k ? 1 - pred.first : pred.first;
			const Demod dp = demod_cs(from, to, 26, sp, pred.c[sw], afc);
			if (!dp.ok) continue;
			soft_score(dp, true);
			if (want_bk && !std::getenv("P2_NO_BKTRACK")) {
				// fix 63: the break-in at the tracked timing (word 0 one index
				// before the control signal's: bk_match's o counts from z 1)
				const int ot = std::max(0, std::min(2, 1 + int(std::lround((pred.start - dp.start) / T_))));
				const double sb = bk_soft(dp, ot - 1, afc);
				r.bk_soft = sb;
				if (sb >= bk_soft_min() && sb - std::max(0.0, r.soft) >= 0.25) {
					r.bk = true; r.bk_tracked = true; r.start = dp.start + (ot - 1) * T_;
					r.c[0] = dp.c[0]; r.c[1] = dp.c[1]; r.has_c = true;
					return r;
				}
			}
			if (cs_soft_ok(r) && !std::getenv("P2_CS_NO_CROSS")) {
				// Fix 169 (E93 check): the tracked look reads the 21 steps at the
				// predicted word positions only, so a control signal a pulse or two
				// off the track is read there as another letter at soft 0.50-0.56,
				// margin 0.27-0.36 (P one pulse late -> S, S two pulses late -> P:
				// a repeat request after our speed-up taken as its acknowledgement,
				// the clean side's E93 loss). An untracked look (free timing, every
				// word position) reads such a burst right. When it reads another
				// letter clearly (hard >= 0.8, or soft >= 0.55 with margin >= 0.30),
				// the letter is doubtful: nothing heard (we repeat; repeating is
				// safe, a wrong acknowledgement loses a packet). P2_CS_NO_CROSS: off.
				const Demod dx = demod_cs(from, to, 26, sp, nullptr, afc);
				if (dx.ok) {
					int hc = -1, sc = -1; double hs = 0, sb = -2, s2 = -2;
					std::vector<std::array<double, 2>> sx;
					for (int o = 0; o <= 2; o++) {
						steps_of(dx, false, o, 21, sx);
						for (int c = 0; c < p2arq::CS_COUNT; c++) {
							const double h = pattern_score(sx, p2arq::kCsPattern[c], afc);
							if (h > hs) { hs = h; hc = c; }
							const double v = pattern_soft(sx, p2arq::kCsPattern[c], afc);
							if (v > sb) { if (sc != c) s2 = sb; sb = v; sc = c; } else if (v > s2 && c != sc) s2 = v;
						}
					}
					static const double xs = std::getenv("P2_CROSS_SOFT") ? std::atof(std::getenv("P2_CROSS_SOFT")) : 0.55, xm = std::getenv("P2_CROSS_MARGIN") ? std::atof(std::getenv("P2_CROSS_MARGIN")) : 0.30, xh = std::getenv("P2_CROSS_HARD") ? std::atof(std::getenv("P2_CROSS_HARD")) : 0.8;
					const int other = hs >= xh ? hc : (sb >= xs && sb - s2 >= xm ? sc : -1);
					if (other >= 0 && other != r.soft_cs) { r.cross_cs = other; return r; }
				}
			}
			if (cs_soft_ok(r)) {
				r.cs = r.soft_cs; r.score = 1.0; r.start = r.soft_start; r.bias = pattern_bias_of(dp, r);
				// Fix 135: the demodulation above runs at the predicted carrier timing, so it
				// measures none: the track froze at its first lock, and a reply drifting
				// against it (the reference firmware as caller and receiver: -1.4 ms a cycle)
				// scored lower each cycle until a look failed and the lock was taken again
				// (soft 0.99 -> 0.45 every ~22 s), and the caller's break-in and packets
				// after it were looked for in the wrong place. A free demodulation measures
				// where the carriers are; within half a pulse it moves the track.
				if (!std::getenv("P2_CSTRACK_STILL")) {
					const Demod df = demod_cs(from, to, 26, sp, nullptr, afc);
					const double dc = df.ok ? 0.5 * ((df.c[0] - pred.c[sw][0]) + (df.c[1] - pred.c[sw][1])) : 1e9;
					if (std::fabs(dc) < 0.5 * T_) { r.start += dc; r.hc[0] = df.c[0]; r.hc[1] = df.c[1]; r.has_hc = true; }
				}
				return r;
			}
		}
	}
	const Demod d0 = demod_cs(from, to, 26, sp, nullptr, afc);
	if (!d0.ok) return r;
	soft_score(d0, false);
	Demod dv[2];
	bool have[2] = {false, false};
	const int e = d0.c[0] <= d0.c[1] ? 0 : 1;
	(void)e;
	auto variant = [&](int v) -> const Demod & {
		if (!have[v]) { dv[v] = slipped(d0, v, from, to, 26, sp); have[v] = true; soft_score(dv[v], false); }
		return dv[v];
	};
	if (want_bk) {
		double st = 0;
		if (bk_match(d0, st)) { r.bk = true; r.start = st; return r; }
		for (int v = 0; v < 2; v++) {
			const Demod &d2 = variant(v);
			if (d2.ok && bk_match(d2, st)) { r.bk = true; r.start = st; return r; }
		}
	}
	std::vector<std::array<double, 2>> st;
	auto score = [&](const Demod &dd) {
		for (int o = 0; o <= 2; o++) {
			steps_of(dd, false, o, 21, st);
			for (int c = 0; c < p2arq::CS_COUNT; c++) {
				double b = 0;
				const double sc = pattern_score(st, p2arq::kCsPattern[c], afc, &b);
				if (sc > r.score) { r.score = sc; r.cs = c; r.start = dd.start + (o - 1) * T_; r.bias = b; r.hc[0] = dd.c[0]; r.hc[1] = dd.c[1]; r.has_hc = true; }
			}
		}
	};
	score(d0);
	for (int v = 0; v < 2 && r.score < 0.8 && !cs_soft_ok(r); v++) {
		const Demod &d2 = variant(v);
		if (d2.ok) score(d2);
	}
	// Fix 196: the carriers re-aligned by up to 4 pulses (the clean tree's P2_CS_SLACK, its
	// PROVENANCE H42, ported). Each carrier's grid starts at its own first pulse of >= 0.4 of its
	// maximum (timing_of) and the fits above let the two grids differ by one pulse at most. The
	// real reference unit's control signals on the RF bench start with ~4 pulses at 20-50 % of full level
	// before word 0 on both carriers (the reference unit's do not: the radio path, or the unit's
	// own output stage); in ~20 % of them one carrier's first pulse was found 2-4 pulses early in
	// that lead-in, its steps compared with the other carrier's 2-4 words off, and the letter read
	// weak or wrong (an S after a speed-up read P 0.71: 600 characters lost) [bb-clean]. So, when
	// nothing above reads clearly, the earlier carrier's grid is moved k = 2..4 pulses later where
	// that leaves the carriers' first pulses within a pulse of each other, and such a reading is
	// taken from 0.9 up (and only over a weaker one). P2 only. P2_CS_SLACK=N (0 or 1: off),
	// P2_CS_WIDE_MIN (0.9).
	static const int cs_slack = std::getenv("P2_CS_SLACK") ? std::atoi(std::getenv("P2_CS_SLACK")) : 4;
	static const double wide_min = std::getenv("P2_CS_WIDE_MIN") ? std::atof(std::getenv("P2_CS_WIDE_MIN")) : 0.9;
	// Fix 265: P3 too. On the air the reference unit's control signal answering our break-in (the first after
	// its own sending) read "cs - 0.64" in 8 of 9 P-answer cases (s2s-pcall a.2: the two carriers' first pulses
	// found 25 ms apart, the high one in the lead-in): its T was missed twice, the unit's next P ("one level
	// lower, again") was then taken for "send at SL1", and the link stalled with T every cycle (TODO ff, fix 265).
	// The same audio re-aligned by k pulses reads T 1.000 / 0.99. P3_CS_SLACK_OLD=1: P2 only, as before.
	static const bool p3_slack = !std::getenv("P3_CS_SLACK_OLD");
	if ((!p3_ || p3_slack) && cs_slack >= 2 && r.score < 0.8 && !cs_soft_ok(r)) {
		const double lag = std::fabs(d0.c[1] - d0.c[0]) / T_;   // the later carrier's first pulse after the earlier one's
		const ReplyRx keep = r;
		r.score = 0;
		for (int k = 2; k <= cs_slack; k++) {
			if (std::fabs(lag - k) > 1.0) continue;
			const double before = r.score;
			score(slip_by(d0, k));
			if (r.score > before) r.wide = k;
		}
		if (r.score < wide_min || r.score <= keep.score) r = keep;
	}
	return r;
}

bool P2Station::reply_rx(int64_t exp, int64_t ref, int arg, bool want_bk, int64_t deadline, ReplyRx &out)
{
	// Inline when the answer is already due at the first look: as callee
	// sending data, the caller's control signal (+0.971 s) and its window
	// end after our next packet's start (+1.25 s); the classification is
	// done at once, as before (on a worker it was never in time: every
	// reply counted as nothing heard, RF bench 2026-10-15).
	const CsPred pred = cs_pred(exp, (arg & 3) == 1 ? 1 : 0);   // long cycle: its own track (fix 72)
	if (!cfg_.realtime || (!(arg & 8) && n_ + samples(0.002) >= deadline)) { out = classify_reply(exp, want_bk, nullptr, afc_, pred); out.ctx = true; return true; }
	if (rr_slot_ != exp || !rr_fut_.valid()) {
		rr_slot_ = exp;
		const int64_t from = exp - samples(0.04) - int64_t(2 * T_), to = exp + samples(0.30) + int64_t(2 * T_) + 1;
		rr_fut_ = pool_.submit([this, exp, want_bk, a = afc_, pred, sp = snapshot(from, to)]() { return classify_reply(exp, want_bk, &sp, a, pred); });
	}
	if (rr_fut_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
		const int64_t step = samples(0.002);
		if (n_ + step < deadline) { schedule(n_ + step, EV_REPLY, ref, arg | 8); return false; }
		rr_fut_ = {};
		if (debug_) log("P2: reply not classified in time, counted as nothing heard");
		out = ReplyRx{};
		return true;
	}
	out = rr_fut_.get();
	out.ctx = true;
	return true;
}

bool P2Station::p3_look(int64_t exp, bool want_bk, bool bk_track, int track, int64_t deadline, EvType ev, int64_t ref, int arg)
{
	if (p3k_exp_ != exp || p3k_ev_ != int(ev) || !p3k_fut_.valid()) {
		p3k_exp_ = exp; p3k_ev_ = int(ev);
		// the timing predictions as the inline look takes them, in its order (cs_pred counts looks)
		p3k_pbk_ = want_bk && bk_track && !std::getenv("P2_NO_BKTRACK") ? cs_pred(exp) : CsPred{};
		p3k_trk_k_ = track == 2 ? 1 : 0;
		p3k_trk_exp_ = cstk_[p3k_trk_k_].last_exp; p3k_trk_calls_ = cstk_[p3k_trk_k_].calls;
		p3k_pcs_ = track ? cs_pred(exp, p3k_trk_k_) : CsPred{};
		const bool lp = late_cs_heard();
		const int64_t from = exp - samples(0.04) - int64_t(2 * T_), to = std::max(exp + samples(0.30) + int64_t(2 * T_) + 1, lp ? n_ : int64_t(0));
		p3k_fut_ = pool_.submit([this, exp, want_bk, lp, a = afc_, heard = n_, pbk = p3k_pbk_, pcs = p3k_pcs_, sp = snapshot(from, to)]() {
			P3Look l;
			if (want_bk) l.bk = bk_look(exp, pbk, &sp, a, heard);
			if (l.bk.hit == 0) l.cs = classify_reply(exp, false, &sp, a, pcs);
			if (lp && l.bk.hit == 0 && l.cs.cross_cs < 0 && !(l.cs.cs >= 0 && l.cs.score >= 0.9) && !cs_soft_ok(l.cs)) heard_cs(exp, &sp, a, heard, l.cs);   // fix 257
			return l;
		}, true);
	}
	if (p3k_fut_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
		const int64_t step = samples(0.002);
		if (n_ + step < deadline) { schedule(n_ + step, ev, ref, arg); return false; }
		load_.look_wait++;
		if (debug_) log("P3: control-signal look not done by our next burst: waiting for it");
	}
	p3k_res_ = p3k_fut_.get();
	p3k_fut_ = {}; p3k_exp_ = -1; p3k_ev_ = -1;
	return true;
}

void P2Station::heard_cs(int64_t exp, const Snap *sp, double afc, int64_t heard, ReplyRx &r) const
{
	const double heard_to = double(std::min<int64_t>(heard, sp->end));
	const Demod d = demod_cs(exp - samples(0.04), int64_t(heard_to), 26, sp, nullptr, afc);
	if (!d.ok) return;
	int best_c = -1, best_good = 0, best_n = 0, second = 0, best_o = 0; double best_bias = afc;
	std::vector<std::array<double, 2>> st;
	for (int o = 0; o <= 2; o++) {
		steps_of(d, false, o, 21, st);
		int nh = 0;                                   // steps whose two pulses (both carriers) are heard
		for (int w = 1; w <= 21; w++) {
			const int i = o + w;                      // pulses i-1, i; pulse k's centre d.c + (k - 1) T, 2 T either side
			if (d.c[0] + (i + 1) * T_ > heard_to || d.c[1] + (i + 1) * T_ > heard_to || i >= int(d.lo.size()) || i >= int(d.hi.size())) break;
			nh = w;
		}
		if (nh < kCsHeardMin) continue;
		std::vector<std::array<double, 2>> sh(st.begin(), st.begin() + nh);
		int g[p2arq::CS_COUNT]; double bs[p2arq::CS_COUNT];
		for (int c = 0; c < p2arq::CS_COUNT; c++) {
			bs[c] = pattern_bias(sh, p2arq::kCsPattern[c], nh, afc);
			g[c] = 0;
			for (int k = 0; k < nh; k++) {
				const double want = p2arq::kCsPattern[c][k] == 'a' ? 10.0 : 2.0;
				int ok = 0;
				for (int cc = 0; cc < 2; cc++) ok += std::fabs(std::fmod(sh[size_t(k)][size_t(cc)] - bs[c] - want + 24.0, 16.0) - 8.0) < 2.0;
				g[c] += ok == 2;
			}
		}
		int bc = 0;
		for (int c = 1; c < p2arq::CS_COUNT; c++) if (g[c] > g[bc]) bc = c;
		int sec = 0;
		for (int c = 0; c < p2arq::CS_COUNT; c++) if (c != bc) sec = std::max(sec, g[c]);
		if (best_c < 0 || g[bc] * best_n > best_good * nh || (g[bc] * best_n == best_good * nh && nh > best_n)) {
			best_c = bc; best_good = g[bc]; best_n = nh; second = sec; best_o = o; best_bias = bs[bc];
		}
	}
	if (best_c < 0 || 10 * (best_n - best_good) > best_n || best_good - second < 4) return;
	r.cs = best_c; r.score = double(best_good) / best_n; r.start = d.start + (best_o - 1) * T_; r.bias = best_bias;
	r.c[0] = d.c[0]; r.c[1] = d.c[1]; r.has_c = true; r.heard_steps = best_n;
}

int64_t P2Station::p3_tx_earliest(int64_t slot) const
{
	static const bool old = std::getenv("P3_LOOK_LEAD_OLD") != nullptr;
	int64_t e = slot - int64_t(std::lround(p2::preroll(cfg_.fs)));
	if (old) return e;
	for (int lv = 2; lv <= 6; lv++) {               // (the next level is not known at a break-in's or B1's look: all of them)
		int64_t st; double ld;
		const bool lng = p3_long_ && lv == p3_level_ && p3::long_payload(lv) > 0;
		p3_place(p3_pulse0(slot, lv, lng), (lv == 5 && !lng) ? 0.02 : 0.03, st, ld);
		e = std::min(e, st);
	}
	return e;
}

int P2Station::reply_cs(const ReplyRx &r)
{
	if (r.cs >= 0 && r.cs < 5) pstats_.cs_rx[size_t(r.cs)]++;
	if (debug_) log(std::string("cs ") + (r.cs >= 0 ? std::string(1, p2arq::kCsName[r.cs]) : "-") + " score " + std::to_string(r.score)
	                + " soft " + (r.soft_cs >= 0 ? std::string(1, p2arq::kCsName[r.soft_cs]) : "-") + " " + std::to_string(r.soft) + " margin " + std::to_string(r.margin));
	if (debug_ && r.wide) log("cs: carriers re-aligned by " + std::to_string(r.wide) + " pulses (fix 196)");
	if (r.heard_steps > 0 && late_cs_logs_++ < 3) log(std::string("cs ") + p2arq::kCsName[r.cs] + " read from the first " + std::to_string(r.heard_steps) + " of its 21 steps, the rest not heard by our look (a long audio path; fix 257)");
	if (r.cross_cs >= 0) {                          // fix 169: tracked and untracked looks disagree
		log(std::string("cs: tracked ") + p2arq::kCsName[r.soft_cs] + " (soft " + std::to_string(r.soft) + ") against untracked " + p2arq::kCsName[r.cross_cs] + ": doubtful, taken as nothing heard");
		return int(p2arq::CS_NONE);
	}
	if (r.score >= 0.8 && (r.soft_cs < 0 || r.soft_cs == r.cs || !cs_soft_ok(r))) {
		const bool on = cs_hard_on_track(r);
		cs_track(r, false);
		if (on) { afc_update(r.bias, 8.0); return r.cs; }
		if (debug_) log("cs: hard detection off the track, not taken");
		return int(p2arq::CS_NONE);
	}
	if (cs_soft_ok(r)) { cs_track(r, true); return r.soft_cs; }
	return int(p2arq::CS_NONE);
}

void P2Station::on_reply(int64_t ref, int arg)
{
	fs_poll();                                     // a frequency search finished since (2026-11-14)
	const int kind = arg & 3;
	int64_t exp = ref + (kind == 1 ? samples(lcyc_s() - cyc_s()) : 0) + reply_delay_theirs();
	// After our break-in as callee, the caller answers on its own lattice
	// (its packet slot + 1.25 s + 0.6 s), as it does as ISS: expect it
	// there, counted from what we heard of its packet. Counting from our
	// own transmission instead put the window 46 ms late on the RF bench
	// (audio path delay), past the reply's reference pulse.
	if (state_ == ST_BK_SENT && !master_ && !p3_ && bk_lattice_ >= 0) exp = bk_lattice_ + samples(cyc_s() + 0.6);
	if (state_ == ST_BK_SENT) {
		int cs;
		if (p3_ && p3_look_async()) {               // fix 250
			if (!p3_look(exp, false, false, 0, p3_look_deadline(ref + samples(cyc_s())), EV_REPLY, ref, arg | 8)) return;
			double start = 0; cs = p3_look_cs(exp, start, 0);
		}
		else if (p3_) { double start = 0; cs = find_psk_cs(exp, start); }
		else {
			ReplyRx rr;
			// Fix 90: our break-in answered by a break-in, P2 (fix 44 for P3).
			// A sender whose own QRT is under way does not hand over: it
			// answers our break-in, in its control-signal slot, with its own
			// (emulator: the firmware's host typed D just as our FQ came, its
			// break-in every cycle, ours every cycle, until the run ended).
			// Receive it as the sender does; its QRT follows.
			const bool want_bk = !std::getenv("P2_BK_NO_BK");
			if (!reply_rx(exp, ref, arg, want_bk, ref + samples(cyc_s()) - samples(0.003), rr)) return;
			if (rr.bk) {
				log("our break-in answered by a break-in");
				txq_.erase(0, size_t(pending_used_)); pending_used_ = 0;   // the firmware printed our break-in's characters
				state_ = ST_ISS;                    // on_bk_done takes a break-in from here (to ST_IRS)
				misses_ = 0;
				grid_ = ref;                        // it answers on our break-in's lattice: our replies follow it
				bk_cc_ok_ = rr.bk_tracked; bk_cc_[0] = rr.c[0]; bk_cc_[1] = rr.c[1]; bk_cc_start_ = int64_t(rr.start);
				schedule(int64_t(rr.start) + samples(std::getenv("P2_NO_EARLY") ? 0.86 : kP2Launch), EV_BK_DONE, int64_t(rr.start));
				return;
			}
			cs = reply_cs(rr);
		}
		// Test: P2_TEST_BK_MISS=N: the first N answers to our break-ins taken as nothing heard
		// (the break-in acknowledged only on the N+1-th try, as on the bench 2026-11-10).
		static int bk_miss = std::getenv("P2_TEST_BK_MISS") ? std::atoi(std::getenv("P2_TEST_BK_MISS")) : 0;
		if (bk_miss > 0 && cs != p2arq::CS_NONE) { bk_miss--; log(std::string("test: answer ") + p2arq::kCsName[cs] + " to our break-in taken as nothing heard"); cs = p2arq::CS_NONE; }
		// the front panel's far-end lamps (measurement only): P takes the break-in, Q / T ask again, T lower
		far_set(cs == p2arq::CS_P ? FAR_OK : cs == p2arq::CS_T ? FAR_REPEAT | (level_ > 1 ? FAR_SLOWER : 0) : cs == p2arq::CS_NONE ? 0 : FAR_REPEAT);
		if (cs == p2arq::CS_P) {                    // only P acknowledges a break-in (Q = send it again)
			txq_.erase(0, size_t(pending_used_)); pending_used_ = 0;
			state_ = ST_ISS; new_send_turn(); misses_ = 0;
			last_rx_cs_ = cs;
			expect_ack_ = 1;                        // P acknowledged the break-in; Q is next
			counter_ = 0; hdr_flip_ = 0;
			long_ = false;
			next_start_ = ref + samples(cyc_s());       // our packets follow our break-in's lattice
			grid_ = ref;
			if (!master_ && !p3_) { const int64_t d = bk_data_shift(); next_start_ += d; grid_ += d; }   // fix 201
			log("break-in acknowledged, now sending");
			build_packet(true);
		} else if ((cs == p2arq::CS_Q || cs == p2arq::CS_T) && !master_ && !p3_ && !std::getenv("P2_BK_AT_OLD") && !std::getenv("P2_TEST_BK_REPEAT")) {
			// Fix 215: Q (or T) to our break-in: the caller has taken it and its data did not decode
			// (the reference unit: Q until the break-in's data, counter 0, has come in; then P;
			// fix 201). It now reads us as its sender, in the window of our packets
			// after a break-in, and a repeated break-in burst is read as a packet that failed: the
			// bench's rb13-1-2 and the reference unit answered Q Q Q Q, then T for ever. So we send: the
			// break-in's text again as an ordinary packet with counter 0 (new to the caller),
			// acknowledged by P; Q asks again. T: one level lower first.
			// P2_TEST_BK_REPEAT (test, fix 260): the break-in repeated instead, as the reference unit does.
			if (cs == p2arq::CS_T && level_ > 1) { level_--; log("break-in: speed down to level " + std::to_string(level_)); }
			pending_used_ = 0;
			state_ = ST_ISS; new_send_turn(); misses_ = 0;
			last_rx_cs_ = cs;
			expect_ack_ = 0;                        // its letter is Q: our counter-0 packet turns it into P
			counter_ = 3; hdr_flip_ = 0;            // build_packet: counter 0
			long_ = false;
			next_start_ = ref + samples(cyc_s()) + bk_data_shift();
			grid_ = ref + bk_data_shift();
			log(std::string("break-in answered ") + p2arq::kCsName[cs] + ": taken, its data not read; now sending it as a packet (fix 215)");
			build_packet(true);
		} else {
			if (++misses_ > cfg_.timeout_cycles) { disconnect("timeout"); return; }
			if (cs == p2arq::CS_T && level_ > 1) { level_--; log("break-in: speed down to level " + std::to_string(level_)); }
			if (bk_lattice_ >= 0) bk_lattice_ += samples(cyc_s());
			if (!master_ && !p3_ && !std::getenv("P2_BK_AT_OLD")) send_bk(ref + samples(cyc_s()) + samples(bk_rel_s()));   // fix 215: in the same place
			else send_bk(ref + samples(cyc_s()));
			schedule(bk_check_at(ref + samples(cyc_s())), EV_REPLY, ref + samples(cyc_s()), 2);
		}
		return;
	}
	if (state_ != ST_ISS) return;
	double start = 0;
	ReplyRx rr;
	if (!p3_) {
		if (!reply_rx(exp, ref, arg, true, p2_look_deadline(ref + samples(kind == 1 ? lcyc_s() : cyc_s())), rr)) return;
		if (rr.bk) {
			bk_cc_ok_ = rr.bk_tracked; bk_cc_[0] = rr.c[0]; bk_cc_[1] = rr.c[1]; bk_cc_start_ = int64_t(rr.start);
			if (debug_) log(std::string("break-in seen") + (rr.bk_tracked ? " (tracked, soft " + std::to_string(rr.bk_soft) + ")" : ""));
			schedule(int64_t(rr.start) + samples(std::getenv("P2_NO_EARLY") ? 0.86 : kP2Launch), EV_BK_DONE, int64_t(rr.start));   // fix 73: as soon as it is in
			return;
		}
	}
	const bool p3a = p3_ && p3_look_async();         // fix 250: the look on a worker
	if (p3a && !p3_look(exp, true, kind != 1, kind == 1 ? 2 : 1, p3_look_deadline(ref + samples(kind == 1 ? lcyc_s() : cyc_s())), EV_REPLY, ref, arg | 8)) return;
	if (p3_ && (p3a ? p3_look_bk(exp, start) : looks_like_bk(exp, start, kind != 1))) {   // P3 break-in: the other side takes the link
		// Test hook (fix 270): P3_TEST_BK_EARLY_MS=x: the first P3_TEST_BK_EARLY_N (1) peer break-ins we take
		// are read x ms early, as the weak tracked readings on the air (u5-4, u5-5: 15-20 ms): our answer goes
		// that much early and our receive lattice is that much off.
		if (const char *e = std::getenv("P3_TEST_BK_EARLY_MS")) {
			static int n = 0;
			const int nmax = std::getenv("P3_TEST_BK_EARLY_N") ? std::atoi(std::getenv("P3_TEST_BK_EARLY_N")) : 1;
			if (n++ < nmax) { start -= std::atof(e) * cfg_.fs / 1000.0; log("test: break-in read " + std::string(e) + " ms early"); }
		}
		take_p3_bk(start);
		return;
	}
	int cs;
	if (p3a) cs = p3_look_cs(exp, start, kind == 1 ? 2 : 1);
	else if (p3_) cs = find_psk_cs(exp, start, kind == 1 ? 2 : 1);
	else { cs = reply_cs(rr); start = rr.start; }
	cs = test_csfault(cs);
	next_start_ = ref + samples(kind == 1 ? lcyc_s() : cyc_s());
	// Long cycle: the receiver keeps its own 3.75 s grid (slightly fast
	// against ours, ~2 samples per cycle) and does not follow us; the
	// firmware's sender follows the receiver (its period jitters around
	// 3.7498 s). Follow it: shift our next start by the change in where
	// its control signal lands relative to our packet (long_follow, fix 209).
	// Fix 231c: as callee, first the jump test (callee_jump): a jump is followed only when the audio
	// layer reported our round trip moving by that much and the next clear reading agrees; such a
	// reading then steers nothing else. CALLEE_TEST_CS shifts readings (test).
	double tsh = 0; bool cj_moved = false;
	if (kind == 0 && ref != sf_next_) cj_short_ = CalleeJump();
	if (kind != 1) cj_long_ = CalleeJump();
	if (!master_ && !callee_follow_off() && cs != p2arq::CS_NONE && kind != 2) {
		tsh = callee_test_shift();
		const bool clear = p3_ ? p3_cs_clear(cs) : long_read_good(rr);
		const double m = callee_jump(kind == 1 ? cj_long_ : cj_short_, start - double(ref) + tsh, clear, kind == 1 ? "long cycle" : "short cycle");
		if (m != 0) { next_start_ += int64_t(std::llround(m)); cj_moved = true; long_cand_n_ = 0; }
	}
	if (kind == 1 && cs != p2arq::CS_NONE && !cj_moved) {
		// Fix 217: in P2 the reading is the control signal's energy onset (cs_onset).
		static const bool onset_off = std::getenv("P2_LONG_ONSET") && std::atoi(std::getenv("P2_LONG_ONSET")) == 0;
		double on = 0;
		if (p3_ || onset_off) long_follow(start - double(ref) + tsh, p3_ || long_read_good(rr));
		else if (cs_onset(start, on)) { long_onset_mode_ = true; long_follow(on - double(ref) + tsh, long_read_good(rr)); }
		else long_follow(start - double(ref) - long_onset_lag_ + tsh, false);
	}
	else if (kind != 1) { long_cs_off_ = -1; long_jump_ = 0; long_cand_n_ = 0; long_rate_ = 0; }
	if (kind == 0 && !master_) {                     // fix 231: as callee our short-cycle packets follow the caller's signals
		double on = 0;
		const bool have = cs != p2arq::CS_NONE;
		if (cj_moved) callee_follow(ref, have, 0, false, false, true);
		else if (p3_) callee_follow(ref, have, start - double(ref) + tsh, have && p3_cs_clear(cs), false);
		else if (have && long_read_good(rr) && cs_onset(start, on)) callee_follow(ref, true, on - double(ref) + tsh, true, true);
		else callee_follow(ref, have, 0, false, false);
	}
	if (cs == p2arq::CS_NONE) {
		p3_clean_run_ = 0;                          // fix 217
		fs_submit(exp, exp - samples(0.03), exp + samples(0.25), 1);   // the frequency search (2026-11-14)
		led_ir_ = 0; led_te_ = 2; pk_count(1);      // front panel: no acknowledgement decoded
		far_set(0);                                 // (the far-end lamps: nothing heard)
		// Fix 76: our QRT end packet met only silence: the peer has closed
		// (it acknowledged our QRT packet). Repeating it until the link
		// timeout kept Pat waiting ~50 s, until it aborted (DD) and
		// panicked (KB0TNG, 20 m, 2026-10-17 10:32: R to our QRT packet,
		// then nothing). A peer that did not decode the end packet asks for
		// it again, which is not silence.
		if (qrt_end_sent_ && misses_ + 1 >= kQrtEndSilent && !std::getenv("P2_QRT_WAIT")) { disconnect("QRT: end packet unanswered, peer silent"); return; }
		if (++misses_ > cfg_.timeout_cycles) { disconnect("timeout"); return; }
		transmit_pending();
		return;
	}
	misses_ = 0;
	// The reference unit as receiver follows the same rules for T, S and the
	// acknowledgements in P2 and P3; so do these.
	int &lv = p3_ ? p3_level_ : level_;
	const int top = p3_ ? 6 : 4;
	auto level_changed = [&]() { if (!p3_ && long_ && !long_ok(level_)) long_ = false; };
	// After our break-in was answered with T or S (fix 48): the receiver
	// answers our first good packet with Q; the same letter again
	// is a repeat request and P means "one level lower, again" (the firmware's
	// sender). Reading KB5LZK's S
	// after its S answer as "ack + faster" dropped 59 characters of Pat's
	// login reply (30 m, 2026-10-15 19:24: the CMS asked "Login" again).
	if (p3_ans_cs_ >= 0) {
		const int ans = p3_ans_cs_;
		if (cs == ans) {
			led_ir_ = 2; led_te_ = 0; pk_count(2);
			far_set(FAR_REPEAT);
			// Fix 265: T after a T answer is a repeat request, but the receiver's failures go on counting
			// (T and one level lower after 5, again after 5 more), and from its 5th T it
			// is at a lower level. Repeating SL5 for ever on T (on air s2s-pcall e.3: 80 cycles, link lost)
			// never met it. So the 5th T in a row lowers us one level, as the T rule below then goes on.
			// P3_ANS_T_OLD=1: the answer's letter always a repeat request.
			static const bool ans_old = std::getenv("P3_ANS_T_OLD") != nullptr;
			if (!ans_old && cs == p2arq::CS_T && ++p3_ans_rep_ >= kP3TDown && lv > 1) {
				p3_ans_cs_ = -1; p3_ans_rep_ = 0;
				p3_up_fails_ = -1; p3_clean_run_ = 0;
				expect_ack_ = 0; last_rx_cs_ = p2arq::CS_T;
				lv--; level_changed();
				hdr_flip_ = counter_ & 1;
				p3_t_run_ = 1;
				far_set(FAR_REPEAT | FAR_SLOWER);
				log("P3: T " + std::to_string(kP3TDown) + " times after the break-in's T answer: speed down to level " + std::to_string(lv) + " (fix 265)");
				rebuild_same();
				return;
			}
			if (p3_ans_rep_ == 1) log(std::string("P3: ") + p2arq::kCsName[cs] + " after the answer " + p2arq::kCsName[ans] + ": a repeat request, level " + std::to_string(lv) + " kept (fix 48/267)");
			transmit_pending(); return;
		}
		p3_ans_cs_ = -1; p3_ans_rep_ = 0;
		if (cs == p2arq::CS_P && lv > 1) {
			lv--; level_changed();
			hdr_flip_ = counter_ & 1;               // a speed-down: the header toggle restarts
			expect_ack_ = 1; last_rx_cs_ = p2arq::CS_P;   // after P the next acknowledgement is Q
			far_set(FAR_REPEAT | FAR_SLOWER);
			log("P3: P after the break-in answer, down to level " + std::to_string(lv));
			rebuild_same();
			return;
		}
		if (cs == p2arq::CS_Q) { expect_ack_ = 1; last_rx_cs_ = p2arq::CS_P; }   // Q acknowledges (the check below)
	}
	if (cs == p2arq::CS_T) p3_guess_rep_ = -1;     // fix 276: the receiver is above level 1: the T rule takes over
	if (cs == p2arq::CS_T) {
		// The receiver sends T after 5 failures and lowers its own level once;
		// further T's are repeat requests, and it lowers again after 5 more
		// (the firmware's sender does the same).
		// Dropping on every T ran ahead of it and the link never recovered.
		p3_up_fails_ = -1; p3_clean_run_ = 0;
		expect_ack_ = 0; last_rx_cs_ = p2arq::CS_T;  // the next acknowledgement is P
		if (lv > 1 && (p3_t_run_ == 0 || p3_t_run_ >= kP3TDown) && !(p3_ && p3_test_skew_step())) {
			lv--; level_changed();
			hdr_flip_ = counter_ & 1;               // the sender's header toggle restarts (0xAA state)
			log(std::string(p3_ ? "P3: " : "") + "speed down to level " + std::to_string(lv));
			p3_t_run_ = 1;
			far_set(FAR_REPEAT | FAR_SLOWER);
			rebuild_same();
		} else {
			p3_t_run_++;
			far_set(FAR_REPEAT);
			transmit_pending();
		}
		return;
	}
	// The receiver acknowledges with Q if the control signal it last sent was
	// P, else with P: after T, R or S the next acknowledgement is P;
	// the same letter as last time is a repeat request. R and S acknowledge
	// and carry a request. After our speed-up on S it answers a good packet at
	// the new level with the signal it sent before the S, unchanged.
	const bool after_s = p3_up_fails_ >= 0;
	const bool ack = after_s ? cs == p3_pre_s_
	               : (cs == p2arq::CS_R || cs == p2arq::CS_S ? cs != last_rx_cs_
	                                                        : (cs == p2arq::CS_P ? 0 : 1) == expect_ack_);
	if (!ack) {                                    // repeat request
		p3_clean_run_ = 0;                          // fix 217
		led_ir_ = 2; led_te_ = 0; pk_count(2);      // front panel: asked to repeat (only the red shows)
		far_set(FAR_REPEAT);
		expect_ack_ = cs == p2arq::CS_P ? 1 : 0; last_rx_cs_ = cs;
		// Fix 276: the level fix 265 guessed (a P after unread answers: "the missed answer's less one",
		// T assumed when nothing was read) answered by P again and again. A receiver above level 1 sends
		// T at its 5th failure in a row and lowers its level (our receiver the same,
		// kP3TDown); at level 1 it has nothing lower and repeats its last letter for ever. So P repeated
		// kP3TDown times with no T means the receiver is at level 1: its unread answers were P (its own
		// SL1 answer, repeated to our break-in sent again), not T or S. Go to level 1 and send the same
		// information there; the header toggle is kept (the receiver changed nothing). A reference
		// unit sender reads its answers, so it never has to guess: repeating SL4
		// for ever met the receiver's P 71 times (on air 2026-11-23 17:44, 80 m, link 22) until the timeout.
		// P3_BK_GUESS_OLD=1: the old way (the guessed level kept).
		static const bool guess_old = std::getenv("P3_BK_GUESS_OLD") != nullptr;
		if (p3_guess_rep_ >= 0 && cs == p2arq::CS_P && !after_s && lv > 1 && !guess_old && ++p3_guess_rep_ >= kP3TDown) {
			const int from = lv;
			p3_guess_rep_ = -1; p3_up_fails_ = -1; p3_t_run_ = 0;
			lv = 1; p3_long_ = false; level_changed();
			far_set(FAR_REPEAT | FAR_SLOWER);
			log("P3: P " + std::to_string(kP3TDown) + " times at the guessed level " + std::to_string(from) +
			    " with no T: the receiver is at level 1 (its unread answers were P), down to level 1 (fix 276)");
			rebuild_same();
			return;
		}
		// After our speed-up: S again = the new level failed once; the receiver
		// stays there for a second try. After 2 failures it drops back
		// and answers with its saved signal toggled, distinguishable from the saved signal that acknowledges.
		if (after_s && cs == p2arq::CS_S) { p3_up_fails_++; transmit_pending(); return; }
		// The same R again while our switch of packet length is pending: the
		// receiver keeps showing R while it fails (it does not send T then)
		// and drops its own level; the firmware's sender drops one level
		// once the R has repeated for more than 4 cycles.
		if (cs == p2arq::CS_R && r_pending_ && ++r_run_ > 4 && lv > 1) {
			lv--; level_changed(); r_run_ = 0;
			hdr_flip_ = counter_ & 1;
			far_set(FAR_REPEAT | FAR_SLOWER);
			log(std::string(p3_ ? "P3: " : "") + "R repeated, speed down to level " + std::to_string(lv));
			rebuild_same();
			return;
		}
		if (after_s && cs == (p3_pre_s_ == p2arq::CS_P ? p2arq::CS_Q : p2arq::CS_P) && lv > 1) {
			lv--; level_changed(); p3_up_fails_ = -1;
			hdr_flip_ = counter_ & 1;               // a speed-down: the header toggle restarts (fix 11; missing here, on air 2026-10-15 the RMS then sent T for 10 cycles)
			far_set(FAR_REPEAT | FAR_SLOWER);
			log(std::string(p3_ ? "P3: " : "") + "speed-up not taken, back to level " + std::to_string(lv));
			rebuild_same();
			return;
		}
		p3_up_fails_ = -1;
		transmit_pending();
		return;
	}
	p3_t_run_ = 0;
	p3_up_fails_ = -1;
	p3_guess_rep_ = -1;                              // fix 276: the guessed level acknowledged
	p3_clean_run_++;                                 // fix 217
	led_good_role_ = 1;                            // front panel: an acknowledgement as sender (ends CHO)
	pk_count(0);
	far_set(FAR_OK | (cs == p2arq::CS_S && lv < top ? FAR_FASTER : 0) | (cs == p2arq::CS_R ? FAR_LENGTH : 0));
	if (cs == p2arq::CS_S) p3_pre_s_ = last_rx_cs_;   // the receiver's signal before this S
	expect_ack_ = cs == p2arq::CS_P ? 1 : 0;
	last_rx_cs_ = cs;
	if (qrt_end_sent_) { disconnect("QRT acknowledged"); return; }
	if (pending_qrt_) { send_qrt_end(); return; }
	// S = acknowledge + up one level. The receiver raises its own level as it
	// sends S, so we follow it even above our max_level (that only limits
	// what we ask for as receiver); not following lost data.
	if (cs == p2arq::CS_S && lv < top) {
		lv++; level_changed();
		p3_up_fails_ = 0;
		tf_ups_++; tf_idx_ = 0;
		log(std::string(p3_ ? "P3: " : "") + "speed up to level " + std::to_string(lv) + (p3_ && p3_long_ ? " (long)" : ""));
	}
	r_pending_ = cs == p2arq::CS_R; r_run_ = 0;     // a switch of packet length is pending until a plain ack
	if (cs == p2arq::CS_R && !p3_) { long_ = !long_ && long_ok(level_); log(long_ ? "long packets" : "short packets"); }
	if (cs == p2arq::CS_R && p3_ && !std::getenv("P3_NO_LONG")) {   // P3: R = toggle long-cycle packets (the protocol description 4.1)
		p3_long_ = !p3_long_ && p3::long_payload(p3_level_) > 0;
		log(std::string("P3: ") + (p3_long_ ? "long" : "short") + " packets at SL" + std::to_string(p3_level_));
	}
	build_packet(true);
}

}  // namespace fmodem
