// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// P2Station member functions, part 2 of 8 (split out of p2_station.hpp so they build in parallel).
#include "p2_station.hpp"

namespace fmodem {

void P2Station::send_cs(int cs, int64_t start)
{
	// test hook (fix 168): from P2_TEST_FORCE_AT s on, the letters of P2_TEST_FORCE_CS in turn
	static const char *force = std::getenv("P2_TEST_FORCE_CS");
	static const double force_at = std::getenv("P2_TEST_FORCE_AT") ? std::atof(std::getenv("P2_TEST_FORCE_AT")) : 0.0;
	static size_t force_i = 0;
	static const size_t force_n = std::getenv("P2_TEST_FORCE_N") ? size_t(std::atoi(std::getenv("P2_TEST_FORCE_N"))) : size_t(-1);   // how many
	static const char *force3 = std::getenv("P2_TEST_FORCE3_CS");   // the same in P3 (data phase)
	const char *fl = p3_ ? force3 : force;
	if (fl && *fl && state_ == ST_IRS && double(n_) / cfg_.fs >= force_at && force_i < force_n) {
		const char *q = std::strchr(p2arq::kCsName, fl[force_i++ % std::strlen(fl)]);
		if (q) cs = int(q - p2arq::kCsName);
	}
	// Fix 203: an answer whose time passed more than 20 ms ago is not sent (it would go out at
	// once, at no time the sender listens). P2_SEND_PAST_CS=1: as before (test).
	static const bool past_ok = std::getenv("P2_SEND_PAST_CS") != nullptr;
	if (!p3_ && !past_ok && start < n_ - samples(0.02)) {
		log(std::string("send CS ") + p2arq::kCsName[cs] + ": its time passed " + std::to_string(double(n_ - start) / cfg_.fs * 1000.0) + " ms ago, not sent");
		last_tx_cs_ = cs;
		psk_swap_ = !psk_swap_;
		return;
	}
	std::vector<uint8_t> w{0x22};
	for (const char *p = p2arq::kCsPattern[cs]; *p; p++) w.push_back(*p == 'a' ? 0xaa : 0x22);
	const bool lead_hi = psk_swap_;
	start = queue_psk(start, w, p3_);
	last_tx_cs_ = cs;
	if (cs >= 0 && cs < 5) pstats_.cs_tx[size_t(cs)]++;
	if (debug_) log(std::string("send CS ") + p2arq::kCsName[cs] + " at " + std::to_string(double(start - n_) / cfg_.fs * 1000.0) + " ms from now" + (p3_ ? "" : lead_hi ? " lead hi" : " lead lo"));
}

int64_t P2Station::reply_at(int64_t rx_start, bool after_long) const
{
	const int64_t nominal = rx_start + (after_long ? samples(lcyc_s() - cyc_s()) : 0) + reply_delay_mine();
	if (!master_) return nominal;
	const double cyc = cyc_s() * cfg_.fs, lat0 = double(grid_) + (0.6 + cfg_.master_adjust_s) * cfg_.fs;
	const double k = std::round((double(nominal) - lat0) / cyc);
	return int64_t(std::llround(lat0 + k * cyc));
}

bool P2Station::timing_of(const std::vector<float> &x, int64_t from, double &c_lo, double &c_hi, double fshift) const
{
	// Pulse timing from the low-passed baseband of each carrier. The
	// filtering (the costly part) runs as four jobs, two carriers x two
	// halves of the window, on several cores (par.hpp); each output
	// sample is computed exactly as in one pass.
	const int ntap = int(0.01 * cfg_.fs) | 1;
	std::vector<double> lp(static_cast<size_t>(ntap));
	{
		const double c = 80.0 / cfg_.fs;
		double sum = 0;
		for (int k = 0; k < ntap; k++) {
			const double m = k - (ntap - 1) / 2.0;
			lp[size_t(k)] = (m == 0 ? 2 * c : std::sin(2 * M_PI * c * m) / (M_PI * m)) * (0.54 - 0.46 * std::cos(2 * M_PI * k / (ntap - 1)));
			sum += lp[size_t(k)];
		}
		for (auto &v : lp) v /= sum;
	}
	const double flo = p3_ ? p3::kCsLo : cfg_.tone_lo, fhi = p3_ ? p3::kCsHi : cfg_.tone_hi;
	const double fcs[2] = {flo - (p3_ ? 0 : 12.5) + fshift, fhi - (p3_ ? 0 : 12.5) + fshift};
	std::vector<std::complex<double>> fl[2] = {std::vector<std::complex<double>>(x.size()), std::vector<std::complex<double>>(x.size())};
	const size_t NX = x.size();
	const int halves = NX > 4000 ? 2 : 1;
	par::parallel_for(2 * halves, [&](int job) {
		const int car = job / halves, h = job % halves;
		const long a = long(NX) * h / halves, b = long(NX) * (h + 1) / halves;
		const long ba = std::max(0L, a - ntap / 2), bb_end = std::min(long(NX), b + ntap / 2 + 1);
		std::vector<std::complex<double>> bb(size_t(bb_end - ba));
		for (long n = ba; n < bb_end; n++) bb[size_t(n - ba)] = double(x[size_t(n)]) * std::polar(1.0, -2 * M_PI * fcs[car] * double(from + int64_t(n)) / cfg_.fs);
		for (long n = a; n < b; n++) {
			std::complex<double> acc = 0;
			for (int k = 0; k < ntap; k++) {
				const long i = n + k - ntap / 2;
				if (i >= 0 && i < long(NX)) acc += lp[size_t(k)] * bb[size_t(i - ba)];
			}
			fl[car][size_t(n)] = acc;
		}
	});
	auto first_centre = [&](const std::vector<std::complex<double>> &f, double &centre) {
		int best_ph = 0; double best = -1;
		for (int ph = 0; ph < int(T_); ph++) {
			double s = 0; int cnt = 0;
			for (double t = ph; t < double(x.size()); t += T_, cnt++) s += std::abs(f[size_t(t)]);
			if (cnt && s / cnt > best) { best = s / cnt; best_ph = ph; }
		}
		double mx = 0;
		for (double t = best_ph; t < double(x.size()); t += T_) mx = std::max(mx, std::abs(f[size_t(t)]));
		for (double t = best_ph; t < double(x.size()); t += T_)
			if (std::abs(f[size_t(t)]) >= 0.4 * mx) { centre = double(from) + t; return true; }
		return false;
	};
	return first_centre(fl[0], c_lo) && first_centre(fl[1], c_hi);
}

auto P2Station::demod(int64_t from, int64_t to, int npulses, const Snap *sp, const double *cc, double fshift) const -> Demod
{
	Demod d;
	if (to <= from || (!sp && n_ - from >= kRing - 2)) return d;
	std::vector<float> x(size_t(to - from));
	for (int64_t t = from; t < to; t++) x[size_t(t - from)] = sp ? sp->at(t) : t < n_ ? raw_[size_t(t % kRing)] : 0.0f;
	double e = 0; for (float v : x) e += double(v) * v;
	if (e / double(x.size()) < 1e2) return d;      // nothing there
	const double flo = (p3_ ? p3::kCsLo : cfg_.tone_lo) + fshift, fhi = (p3_ ? p3::kCsHi : cfg_.tone_hi) + fshift;
	double c_lo = 0, c_hi = 0;
	if (cc) { c_lo = cc[0]; c_hi = cc[1]; }     // timing found earlier (p2_sync)
	else if (!timing_of(x, from, c_lo, c_hi, timing_shift(fshift))) return d;
	d.start = std::min(c_lo, c_hi) - nw_ / 2.0;
	d.c[0] = c_lo; d.c[1] = c_hi;
	const LsArgs la = ls_args(from, to, c_lo, c_hi, flo, fhi, npulses);
	std::vector<float> y(la.ysize);
	const int64_t base = la.base;
	for (size_t k = 0; k < y.size(); k++) {
		const int64_t t = base + int64_t(k);
		y[k] = sp ? sp->at(t) : (t >= 0 && t < n_ && n_ - t < kRing - 1) ? raw_[size_t(t % kRing)] : 0.0f;
	}
	const auto z = p2::ls_pulses(y, cfg_.fs, la.cA, la.cB, la.fA, la.fB, la.n, p3_ ? &p3::cs_pulse_table(cfg_.fs) : nullptr);
	d.lo.assign(z.begin(), z.begin() + npulses + 2);
	d.hi.assign(z.begin() + npulses + 2, z.end());
	d.ok = true;
	return d;
}

double P2Station::cs_shift_hz(double afc) const
{
	static const bool off2 = std::getenv("P2_CS_NO_DEMOD_AFC") != nullptr, off3 = std::getenv("P3_CS_NO_DEMOD_AFC") != nullptr;
	static const double mn = std::getenv("P2_CS_AFC_MIN") ? std::atof(std::getenv("P2_CS_AFC_MIN")) : 1.0;
	const double hz = afc * kAfcHzPerUnit;
	return (!(p3_ ? off3 : off2) && std::fabs(hz) > mn) ? hz : 0.0;
}

auto P2Station::demod_cs(int64_t from, int64_t to, int npulses, const Snap *sp, const double *cc, double afc) const -> Demod
{
	const double fs_hz = cs_shift_hz(afc);
	Demod d = demod(from, to, npulses, sp, cc, fs_hz);
	if (d.ok && fs_hz != 0.0)
		for (int car = 0; car < 2; car++) {
			auto &Z = car ? d.hi : d.lo;
			for (size_t i = 0; i < Z.size(); i++) Z[i] *= std::polar(1.0, 2 * M_PI * fs_hz * (d.c[car] + (double(i) - 1.0) * T_) / cfg_.fs);
		}
	return d;
}

auto P2Station::slipped(const Demod &d, int v, int64_t from, int64_t to, int npulses, const Snap *sp) const -> Demod
{
	const int e = d.c[0] <= d.c[1] ? 0 : 1;
	double c2[2] = {d.c[0], d.c[1]};
	if (v == 0) c2[e] += T_; else c2[1 - e] -= T_;
	static const bool redemod = std::getenv("P2_SLIP_REDEMOD") != nullptr;
	if (redemod) return demod(from, to, npulses, sp, c2);   // (test; nominal tones)
	Demod d2 = d;
	d2.c[0] = c2[0]; d2.c[1] = c2[1];
	d2.start = std::min(c2[0], c2[1]) - nw_ / 2.0;
	if (v == 0) { auto &Z = e ? d2.hi : d2.lo; Z.erase(Z.begin()); Z.push_back({0, 0}); }
	else { auto &Z = e ? d2.lo : d2.hi; Z.insert(Z.begin(), {0, 0}); Z.pop_back(); }
	return d2;
}

auto P2Station::slip_by(const Demod &d, int k) const -> Demod
{
	const int e = d.c[0] <= d.c[1] ? 0 : 1;
	Demod r = d;
	auto &Z = e ? r.hi : r.lo;
	const size_t n = std::min(Z.size(), size_t(k));
	r.c[e] += double(k) * T_;
	Z.erase(Z.begin(), Z.begin() + long(n));
	Z.insert(Z.end(), n, {0, 0});
	r.start = std::min(r.c[0], r.c[1]) - nw_ / 2.0;
	return r;
}

void P2Station::step_weights(const Demod &d, bool swap, int o, int first, int n, double (*w)[2])
{
	const auto &A = swap ? d.hi : d.lo, &B = swap ? d.lo : d.hi;
	std::vector<double> all;
	for (int k = 0; k < n; k++)
		for (int c = 0; c < 2; c++) {
			const auto &Z = c ? B : A;
			const size_t i = size_t(o + first + k + 1);
			w[k][c] = (i >= 1 && i < Z.size()) ? std::abs(Z[i - 1]) * std::abs(Z[i]) : 0.0;
			all.push_back(w[k][c]);
		}
	std::nth_element(all.begin(), all.begin() + long(all.size() / 2), all.end());
	const double med = std::max(1e-12, all[all.size() / 2]);
	for (int k = 0; k < n; k++) for (int c = 0; c < 2; c++) w[k][c] /= med;
}

double P2Station::pattern_bias(const std::vector<std::array<double, 2>> &st, const char *pat, int len, double prior)
{
	std::complex<double> acc = 0;
	for (int i = 0; i < len && i < int(st.size()); i++)
		for (int c = 0; c < 2; c++) {
			const double want = pat[i] == 'a' ? 10.0 : 2.0;
			acc += std::polar(1.0, 2 * M_PI * (st[size_t(i)][size_t(c)] - prior - want) / 16.0);
		}
	const double b = 16.0 * std::arg(acc) / (2 * M_PI);
	return prior + (std::fabs(b) <= 4.0 ? b : 0.0);
}

double P2Station::pattern_score(const std::vector<std::array<double, 2>> &st, const char *pat, double prior, double *bias_out)
{
	const int len = int(std::strlen(pat));
	const double bias = pattern_bias(st, pat, len, prior);
	if (bias_out) *bias_out = bias;
	double good = 0;
	for (int i = 0; i < len && i < int(st.size()); i++)
		for (int c = 0; c < 2; c++) {
			const double want = pat[i] == 'a' ? 10.0 : 2.0;
			double e = std::fabs(std::fmod(st[size_t(i)][size_t(c)] - bias - want + 24.0, 16.0) - 8.0);   // 0..8
			good += e < 2.0 ? 1.0 : 0.0;
		}
	return good / (2.0 * len);
}

int P2Station::find_psk_cs_raw(int64_t exp, double &start, int track)
{
	// Fix 62: the classifier P2 uses (tracked timing, soft decision); the
	// hard score alone missed most control signals below ~-5 dB.
	// long cycle: the receiver keeps its own 3.75 s grid (drifting ~±45 ms
	// against ours), so its timing is not predictable from the short cycle
	return find_psk_cs_apply(exp, classify_reply(exp, false, nullptr, afc_, track ? cs_pred(exp, track == 2 ? 1 : 0) : CsPred{}), start, track);
}

int P2Station::find_psk_cs_apply(int64_t exp, ReplyRx r, double &start, int track)
{
	r.ctx = track != 0;
	start = r.start;
	look_soft_cs_ = r.soft_cs; look_soft_ = r.soft; look_margin_ = r.margin;   // fix 210: the last look's soft reading
	look_hard_cs_ = r.cs; look_score_ = r.score;     // fix 231c, 235
	int cs = p2arq::CS_NONE;
	if (r.cross_cs >= 0) {                          // fix 169 (see classify_reply)
		if (debug_) log(std::string("cs: tracked ") + p2arq::kCsName[r.soft_cs] + " against untracked " + p2arq::kCsName[r.cross_cs] + ": doubtful, taken as nothing heard");
		return cs;
	}
	if (r.score >= 0.8 && (r.soft_cs < 0 || r.soft_cs == r.cs || !cs_soft_ok(r))) {
		// Not during P3 set-up: the real reference unit answered our B1 with R over and
		// over and each R's estimate pulled the tracked offset ~11 Hz further
		// (-7 to -75 Hz in a minute, RF bench 2026-10-15).
		const bool on = cs_hard_on_track(r);
		cs_track(r, false);
		if (on) { if (state_ != ST_P3SETUP) afc_update(r.bias, 8.0); cs = r.cs; }
		else if (debug_) log("cs: hard detection off the track, not taken");
	} else if (cs_soft_ok(r)) { cs_track(r, true); cs = r.soft_cs; start = r.soft_start; }
	else if (!track && r.soft_cs >= 0 && r.soft >= 0.55 && r.margin >= 0.40 && !std::getenv("P2_NO_CSPAIR")) {
		// Fix 68: an untracked look (P3 set-up, changeovers) takes a soft
		// letter only when the previous look gave the same one (single looks:
		// ~1 % wrong, 1e-4 false in noise; the answer repeats every cycle
		// until taken). KB5LZK 80 m answered our A1 with P at soft 0.55-0.78,
		// hard under 0.8, for 40 s: set-up timeout (2026-10-16 19:10).
		if (r.soft_cs == su_cs_ && exp - su_exp_ <= samples(4.0)) {
			cs = r.soft_cs; start = r.soft_start; su_cs_ = -1;
			if (debug_) log("cs: soft letter taken on the second agreeing look");
		} else { su_cs_ = r.soft_cs; su_exp_ = exp; }
	}
	if (cs >= 0 && cs == r.cs) su_cs_ = -1;
	if (debug_ && r.wide) log("cs: carriers re-aligned by " + std::to_string(r.wide) + " pulses (fix 196)");
	if (r.heard_steps > 0 && late_cs_logs_++ < 3) log(std::string("cs ") + p2arq::kCsName[r.cs] + " read from the first " + std::to_string(r.heard_steps) + " of its 21 steps, the rest not heard by our look (a long audio path; fix 257)");
	if (debug_) log(std::string("cs ") + (cs >= 0 ? std::string(1, p2arq::kCsName[cs]) : "-") + " score " + std::to_string(r.score)
	                + " soft " + (r.soft_cs >= 0 ? std::string(1, p2arq::kCsName[r.soft_cs]) : "-") + " " + std::to_string(r.soft) + " margin " + std::to_string(r.margin));
	return cs;
}

void P2Station::afc_update(double b, double mod)
{
	const double c = b + mod * std::round((afc_ - b) / mod);
	afc_ += 0.5 * (c - afc_);
	led_trk_n_ = n_;
	tx_est_feed(afc_hz(), "afc");
	if (debug_) log("afc " + std::to_string(afc_hz()) + " Hz" + (std::getenv("AFC_DEBUG") ? " (measured " + std::to_string(b) + " mod " + std::to_string(mod) + ")" : std::string()));
}

void P2Station::shift_audio_part(const std::vector<float> &a, double hz, double fs, float *out, long i0)
{
	constexpr int N = 127;
	static const std::vector<double> h = [] {
		std::vector<double> v(N, 0.0);
		for (int k = 0; k < N; k++) {
			const int m = k - N / 2;
			v[size_t(k)] = (m % 2) ? 2.0 / (M_PI * m) * (0.54 - 0.46 * std::cos(2 * M_PI * k / (N - 1))) : 0.0;
		}
		return v;
	}();
	const long n = long(a.size());
	for (long i = i0; i < n; i++) {
		double im = 0;
		for (int k = 0; k < N; k++) {
			const long j = i + N / 2 - k;
			if (j >= 0 && j < n) im += h[size_t(k)] * a[size_t(j)];
		}
		const double ph = 2 * M_PI * hz * double(i) / fs;
		out[size_t(i)] = float(a[size_t(i)] * std::cos(ph) - im * std::sin(ph));
	}
}

void P2Station::shift_cached(std::vector<float> &x, double df, double fs, int64_t a, long heard, ShiftCache &c)
{
	static const bool old = std::getenv("P3_SHIFT_CACHE_OLD") != nullptr;
	if (old) { shift_audio(x, df, fs); return; }
	const long n = long(x.size());
	std::vector<float> out(x.size());
	long start = 0;
	if (c.a == a && c.df == df) start = std::max(0L, std::min({c.valid, n - 63, heard - 63}));
	if (start > 0) std::memcpy(out.data(), c.y.data(), size_t(start) * sizeof(float));
	shift_audio_part(x, df, fs, out.data(), start);
	const long valid = std::max(0L, std::min(n, heard) - 63);
	if (c.a != a || c.df != df || valid > c.valid) { c.a = a; c.df = df; c.valid = valid; c.y.assign(out.begin(), out.begin() + valid); }
	x.swap(out);
}

double P2Station::bk_rel_s() const
{
	static const bool old = std::getenv("P2_BK_AT_OLD") != nullptr;
	static const double tsh = std::getenv("P2_TEST_BK_SHIFT_MS") ? std::atof(std::getenv("P2_TEST_BK_SHIFT_MS")) / 1000.0 : 0.0;
	if (master_) return cfg_.master_bk_adjust_s;
	if (old) return cfg_.callee_bk_adjust_s + tsh;
	return cs_ref_s() + kBkAfterCs + tsh;
}

int64_t P2Station::bk_data_shift()
{
	if (const char *e = std::getenv("P2_DATA_SHIFT_MS")) { log(std::string("test: our packets after the break-in at ") + e + " ms"); return samples(std::atof(e) / 1000.0); }
	if (std::getenv("P2_BK_DATA_OLD")) return 0;
	static const double lead = std::getenv("P2_BK_DATA_LEAD_MS") ? std::atof(std::getenv("P2_BK_DATA_LEAD_MS")) / 1000.0 : kBkDataLead;
	const double s = -(lead + double(cfg_.rt_latency_s) + double(cfg_.reply_adjust_s));
	if (debug_) log("our packets after the break-in " + std::to_string(s * 1000.0) + " ms from the reply lattice (fix 201)");
	return samples(s);
}

int64_t P2Station::reply_check_at(int64_t ref, bool lng) const
{
	const int64_t usual = ref + (lng ? samples(lcyc_s() - cyc_s()) : 0) + reply_delay_theirs() + samples(0.30);
	if (p3_) {
		static const bool old = std::getenv("P3_DATA_LOOK_OLD") != nullptr;
		const int64_t cap = p3_look_deadline(ref + samples(lng ? lcyc_s() : cyc_s()));
		int64_t at = usual;
		// Fix 243: our round trip longer than the default path's (rt_latency > 0): the control
		// signal reaches us that much later, so the look waits that much longer, up to just before
		// our next packet (with the default path, rt_latency <= 0: unchanged).
		if (cfg_.realtime && cfg_.rt_latency_s > 0.0f && !std::getenv("P3_LONG_PATH_LOOK_OFF"))
			at = std::max(usual, std::min(usual + samples(cfg_.rt_latency_s), cap));
		// Fix 265 (TODO ff 3, n4a10): after a long packet the look waits until 15 ms before the decision is due.
		// The receiver's break-in burst there (824 ms) starts where its control signal would, and its 24-word
		// header must be in: on the air A looked 3.68 s after its long packet's start, had heard 272 ms of B's
		// break-in (18 words) and read "cs -"; B's break-in went unseen for 87 s (B's set-up timeout), A sent
		// on until killed. The recorded audio cut where A looked: no break-in; cut 25 ms later: the break-in.
		// P3_LONG_LOOK_OLD=1: as before.
		static const bool long_old = std::getenv("P3_LONG_LOOK_OLD") != nullptr;
		if (lng && !long_old) at = std::max(at, cap - samples(0.015));
		return old ? at : std::min(at, cap);
	}
	if (std::getenv("P2_LATE_REPLY_LOOK")) return usual;
	const int64_t next = ref + samples(lng ? lcyc_s() : cyc_s());
	return std::min(usual, p2_irs_look_old() ? next - samples(0.005) : p2_look_deadline(next));
}

auto P2Station::bk_look(int64_t exp, const CsPred &pred, const Snap *sp, double afc, int64_t heard) const -> BkLook
{
	BkLook b;
	// fix 63 (P3): the break-in at the tracked timing, scored softly (as P2's
	// classify_reply); a control signal there scores ~-0.14 against it
	if (pred.ok) {
		const Demod dp = demod_cs(exp - samples(0.04), exp + samples(0.30), 26, sp, pred.c[pred.first], afc);
		if (dp.ok) {
			const int ot = std::max(0, std::min(2, 1 + int(std::lround((pred.start - dp.start) / T_))));
			double bias = afc;
			const double sb = bk_soft(dp, ot - 1, afc, &bias);
			std::vector<std::array<double, 2>> st;
			steps_of(dp, false, ot, 21, st);
			double cs_best = -1;
			for (int c = 0; c < p2arq::CS_COUNT; c++) cs_best = std::max(cs_best, pattern_soft(st, p2arq::kCsPattern[c], afc));
			if (sb >= bk_soft_min() && sb - std::max(0.0, cs_best) >= 0.25) {
				b.hit = 1; b.start = dp.start + (ot - 1) * T_; b.bias = bias; b.soft = sb; b.c[0] = dp.c[0]; b.c[1] = dp.c[1];
				// Fix 266 (TODO ff, fourth on-air session): a weak tracked reading (soft 0.60-0.68) is the break-in read
				// with the carriers the control-signal track predicted, one of them in the signal's lead-in: before
				// each such break-in the reference unit's control signals had been read with their carriers 2.5-5.5
				// pulses apart (fix 196's lead-in), against 0.5 for the good ones. Its start came out 15-20 ms early,
				// our answer went that much early (u1-1, u1-2: 922-928 ms after the break-in's onset, against
				// 941-943 ms for every answer the unit took), the unit did not take it and repeated its break-in
				// every cycle until the link ended; we never decoded a packet after it (fourth session 4 of 4,
				// third session 15 of 15 such readings). The burst's own demodulation (its carriers' own first
				// pulses, fix 196's slip by 2-4 pulses where they lie that far apart) reads the same audio at
				// 0.99. So below kBkWeak the burst's own reading, if clean (>= 0.9) and better, gives the start.
				// kBkWeak 0.75: the failing readings were 0.60-0.68; between two of our stations the tracked
				// readings of 0.78-0.92 were all answered and decoded (fourth session, 22 of 22), so they stay.
				// P3_BK_OWN_OLD=1: the tracked reading as it is; P3_BK_OWN_BELOW=x: the threshold (0.75).
				static const bool own_old = std::getenv("P3_BK_OWN_OLD") != nullptr;
				static const double kBkWeak = std::getenv("P3_BK_OWN_BELOW") ? std::atof(std::getenv("P3_BK_OWN_BELOW")) : 0.75;
				constexpr double kBkClean = 0.9;
				if (!own_old && sb < kBkWeak) {
					const Demod d0 = demod_cs(exp - samples(0.04), exp + samples(0.30), 26, sp, nullptr, afc);
					b.own_rej = -1;                                       // looked; -1: its own demodulation failed
					if (d0.ok) {
						const double lag = std::fabs(d0.c[1] - d0.c[0]) / T_;
						b.own_lag = lag;
						double best = -1, best_bias = afc, best_start = 0, bc[2] = {0, 0}; int best_k = 0;
						for (int k = 0; k <= 4; k++) {
							if (k == 1 || (k >= 2 && std::fabs(lag - k) > 1.0)) continue;
							const Demod d = k ? slip_by(d0, k) : d0;
							for (int o = -1; o <= 1; o++) {
								double bi = afc;
								const double s0 = bk_soft(d, o, afc, &bi);
								if (s0 > best) { best = s0; best_bias = bi; best_start = d.start + o * T_; bc[0] = d.c[0]; bc[1] = d.c[1]; best_k = k; }
							}
						}
						b.own_rej = best;
						if (best >= kBkClean && best > sb) {
							b.own_soft = best; b.own_moved = (best_start - b.start) / T_ * 10.0; b.own_slip = best_k;
							b.start = best_start; b.bias = best_bias; b.c[0] = bc[0]; b.c[1] = bc[1];
						}
					}
					// Fix 270: fix 266 kept the weak tracked reading (its own soft reading not clean): the pattern
					// matched hard on the burst's own demodulation gives the start, if it matches within kBkRepWin.
					// On the air (u5-4 107.6 s, soft 0.69, kept): the hard match read it 16 ms after the tracked
					// start, where the peer's repeat and its packets then came. P3_BK_REPEAT_OLD=1: not tried.
					if (b.own_soft < 0 && !bk_rep_old() && d0.ok) {
						double hs = 0, hb = afc;
						if (bk_hard(d0, exp, sp, afc, heard, hs, &hb) && std::fabs(hs - b.start) <= kBkRepWin * cfg_.fs) {
							b.own_hard = (hs - b.start) / cfg_.fs * 1000.0;
							b.start = hs; b.bias = hb;   // its data decode finds its own carriers (as after a hard hit)
						}
					}
				}
				return b;
			}
		}
	}
	const Demod d0 = demod_cs(exp - samples(0.04), exp + samples(0.30), 26, sp, nullptr, afc);
	if (!d0.ok) return b;
	double start = 0;
	if (bk_hard(d0, exp, sp, afc, heard, start, &b.bias)) { b.hit = 2; b.start = start; }
	return b;
}

bool P2Station::bk_hard(const Demod &d0, int64_t exp, const Snap *sp, double afc, int64_t heard, double &start, double *bias) const
{
	bool hit = bk_match(d0, start, afc, heard, bias);
	// one carrier's first pulse found a pulse early (see find_packet)
	const int e = d0.c[0] <= d0.c[1] ? 0 : 1;
	for (int v = 0; v < 2 && !hit; v++) {
		double c2[2] = {d0.c[0], d0.c[1]};
		if (v == 0) c2[e] += T_; else c2[1 - e] -= T_;
		const Demod d2 = demod_cs(exp - samples(0.04), exp + samples(0.30), 26, sp, c2, afc);
		hit = d2.ok && bk_match(d2, start, afc, heard, bias);
	}
	return hit;
}

bool P2Station::looks_like_bk_apply(int64_t exp, const CsPred &pred, const BkLook &b, double &start)
{
	if (b.hit == 1) {
		start = b.start;
		bk_bias_ = b.bias;
		bk_cc_ok_ = b.own_hard <= -999; bk_cc_[0] = b.c[0]; bk_cc_[1] = b.c[1]; bk_cc_start_ = int64_t(start);
		if (debug_) log("P3: break-in seen (tracked, soft " + std::to_string(b.soft) + ")");
		if (b.own_soft >= 0) {                     // fix 266 (logged always: it moves our answer)
			char m[200];
			std::snprintf(m, sizeof m, "P3: break-in read weakly at the tracked timing (soft %.2f): its own timing reads it at %.2f%s, start moved %+.1f ms (fix 266)",
			              b.soft, b.own_soft, b.own_slip ? (", carriers re-aligned by " + std::to_string(b.own_slip) + " pulses").c_str() : "", b.own_moved);
			log(m);
		} else if (b.own_hard > -999) {            // fix 270: the hard match took over
			char m[200];
			std::snprintf(m, sizeof m, "P3: break-in read weakly at the tracked timing (soft %.2f), its own soft timing %s: its pattern matched hard, start moved %+.1f ms (fix 270)",
			              b.soft, b.own_rej < 0 ? "not demodulated" : ("reads " + std::to_string(b.own_rej).substr(0, 5)).c_str(), b.own_hard);
			log(m);
		} else if (b.own_rej > -2) {               // fix 266 looked but kept the tracked reading (logged: the on-air u5-4/u5-5 cases)
			char m[200];
			std::snprintf(m, sizeof m, "P3: break-in read weakly at the tracked timing (soft %.2f): its own timing %s (carriers %.1f pulses apart): kept (fix 266)",
			              b.soft, b.own_rej < 0 ? "not demodulated" : ("reads " + std::to_string(b.own_rej).substr(0, 5)).c_str(), b.own_lag);
			log(m);
		}
		return true;
	}
	bk_cc_ok_ = false;
	if (b.hit != 2) return false;
	start = b.start;
	bk_bias_ = b.bias;
	// Fix 74: a hard break-in off the tracked reply timing is taken only when
	// the look before had one at the same offset (the peer's timing moved);
	// alone it is refused (counted as a missed signal: a real break-in is
	// repeated). KC0TPS (40 m, 2026-10-17 02:35, a weak link) gave a match
	// 27.5 ms late; its data never decoded, nor did anything after it.
	if (pred.ok && !cstk_[0].turn_new && !std::getenv("P2_NO_BKGATE")) {
		const double off = start - pred.start;
		if (std::fabs(off) > 1.0 * T_) {
			const bool pair = bk_off_exp_ >= 0 && exp - bk_off_exp_ == samples(cyc_s()) && std::fabs(off - bk_off_) <= 1.0 * T_;
			bk_off_exp_ = exp; bk_off_ = off;
			log("P3: break-in " + std::to_string(off / cfg_.fs * 1000.0) + " ms off the track" + (pair ? ", second in a row: taken" : ": refused"));
			if (!pair) return false;
		}
	}
	return true;
}

bool P2Station::bk_match(const Demod &d, double &start, double afc, int64_t heard, double *bias_out) const
{
	// The first pulses may be lost to our own turnaround: match whatever
	// part of the 24-pulse pattern is there (at least 16 pulses). So may the
	// last ones, not received yet: the ISS decides before its next packet,
	// and on air the round trip put an RMS's break-in ~0.11 s later than
	// the firmware's own (N5TW, 2026-10-15: its last ~5 pulses were still
	// to come, read as zeros, 37/48 matched, 36 break-ins missed). Pulse
	// i of a carrier is centred at d.c + (i - 1) T; skip any not yet in.
	// Only pattern pulses 1-20 are scored, so pulses 21-24 (the last to
	// arrive) are never needed.
	constexpr int kBkScored = 20;
	auto in_yet = [&](int i, bool swap) {
		return d.c[swap ? 1 : 0] + (i - 1) * T_ + nw_ / 2.0 <= double(heard) && d.c[swap ? 0 : 1] + (i - 1) * T_ + nw_ / 2.0 <= double(heard);
	};
	for (int swap = 0; swap < 2; swap++)
		for (int o = -6; o <= 2; o++) {
			const auto &A = swap ? d.hi : d.lo, &B = swap ? d.lo : d.hi;
			int good = 0, seen = 0;
			// common offset (radio frequency offset, see pattern_score), +-4/16
			std::complex<double> acc = 0;
			for (int w = 1; w <= kBkScored; w++) {
				const int i1 = 1 + o + w;
				if (i1 < 1 || i1 >= int(A.size()) || i1 >= int(B.size()) || !in_yet(i1, swap)) continue;
				for (int c = 0; c < 2; c++) {
					const auto &Z = c ? B : A;
					const double want = p2arq::kBkPattern[w - 1] == 'a' ? 10.0 : 2.0;
					acc += std::polar(1.0, 2 * M_PI * (nib(Z[size_t(i1 - 1)], Z[size_t(i1)]) - afc - want) / 16.0);
				}
			}
			double bias = 16.0 * std::arg(acc) / (2 * M_PI);
			// The +-4/16 limit (control signals: a pattern and its complement are
			// half a turn apart) is not needed here: no control-signal code is the
			// break-in pattern's complement. KC0TPS (30 m, 2026-10-15) was ~32 Hz
			// off (-3.5/16 per step); its break-ins' estimates crossed -4, were
			// zeroed, and none matched (fix 45).
			if (std::fabs(bias) > 7.0) bias = 0;
			bias += afc;
			for (int w = 1; w <= kBkScored; w++) {
				const int i1 = 1 + o + w;
				if (i1 < 1 || i1 >= int(A.size()) || i1 >= int(B.size()) || !in_yet(i1, swap)) continue;
				for (int c = 0; c < 2; c++) {
					const auto &Z = c ? B : A;
					const double v = nib(Z[size_t(i1 - 1)], Z[size_t(i1)]);
					const double want = p2arq::kBkPattern[w - 1] == 'a' ? 10.0 : 2.0;
					good += std::fabs(std::fmod(v - bias - want + 24.0, 16.0) - 8.0) < 2.0;
					seen++;
				}
			}
			if (seen >= 32 && good >= int(0.85 * seen)) { start = d.start + o * T_; (bias_out ? *bias_out : bk_bias_) = bias; return true; }
		}
	return false;
}

double P2Station::packet_fshift(bool bk_only) const
{
	static const bool off = std::getenv("P2_NO_DEMOD_AFC") != nullptr;
	// Fix 147: P2 break-ins shifted too. Demodulated at the nominal tones, the firmware's
	// break-in 60 Hz off never decoded (answered Q to the end of the link; 40 Hz still did).
	static const bool bk_unshifted = std::getenv("P2_BK_NO_DEMOD_AFC") != nullptr;
	return (demod_afc_on() && (!bk_only || !bk_unshifted) && !off && std::fabs(afc_hz()) > 1.0) ? afc_hz() : 0.0;
}

auto P2Station::find_packet(int64_t exp, bool bk_only, bool allow_long, const Snap *sp, const double *cc, const TrkView *tv) -> RxPacket
{
	const double fs_hz = packet_fshift(bk_only);
	RxPacket r = find_packet_at(exp, bk_only, allow_long, sp, cc, fs_hz, tv);
	if (fs_hz != 0.0 && r.ok) r.bias += afc_;
	// fix 212: the track's refinement computed here when on a worker (a snapshot), off the audio loop
	if (sp && r.ok && !bk_only && r.has_c && r.swap >= 0) r.has_rc = trk_refine(r, r.rc, sp);
	return r;
}

auto P2Station::find_packet_at(int64_t exp, bool bk_only, bool allow_long, const Snap *sp, const double *cc, double fshift, const TrkView *tv) -> RxPacket
{
	RxPacket r;
	const int maxp = allow_long ? 332 : 84;
	TrkView tk;                                       // the track for this transmission (fix 200: tv, from the launch)
	if (!bk_only) tk = tv ? *tv : trk_view(exp);
	const bool tracked = !bk_only && tk.on;
	const int want_swap_ = tracked ? tk.swap : marq_swap_;   // the carrier order the next failed copy is taken in (memory ARQ)
	// The six candidates (carrier order x word-0 position) are tried at
	// once on several cores (par.hpp); the first that decodes, in the
	// order swap 0 o 0..2, swap 1 o 0..2, wins, as in one pass.
	// One search over the candidates of a demodulation; hdr: some
	// candidate had a header.
	auto search = [&](const Demod &d, bool &hdr) -> RxPacket {
		const int no = 3;
		std::vector<RxPacket> cand(size_t(2 * no));
		std::vector<char> hdr_seen(size_t(2 * no), 0);
		auto try_cand = [&](int idx) {
			const int swap = idx / no, o = idx % no;
			RxPacket &r = cand[idx];
			std::vector<std::array<double, 2>> st;
			// Header: level and parity (short) or a long header.
			steps_of(d, swap, o, 8, st);
			double h[8][2];
			for (int i = 0; i < 8; i++) { h[i][0] = st[size_t(i)][0]; h[i][1] = st[size_t(i)][1]; }
			const auto hf = p2::fit_header(h);
			// Long headers: own bias fit, compared with the short-header fit.
			int lv_long = 0;
			double long_bias = 0, long_rms = 99;
			for (int lv = 1; lv <= 4 && allow_long; lv++) {
				if (!long_known(lv)) continue;
				for (int par = 0; par < 2; par++) {
					std::complex<double> acc = 0;
					double dd[16];
					for (int i = 0; i < 8; i++)
						for (int c = 0; c < 2; c++) {
							const int want = c ? p2arq::kLongHeader[lv][par][i] & 15 : p2arq::kLongHeader[lv][par][i] >> 4;
							dd[2 * i + c] = h[i][c] - want;
							acc += std::polar(1.0, 2 * M_PI * dd[2 * i + c] / 16.0);
						}
					const double bias = 16.0 * std::arg(acc) / (2 * M_PI);
					double e2 = 0;
					for (double v : dd) { const double e = std::fmod(v - bias + 1608.0, 16.0) - 8.0; e2 += e * e; }
					const double rms = std::sqrt(e2 / 16);
					if (rms < long_rms) { long_rms = rms; long_bias = bias; lv_long = lv; }
				}
			}
			static const double long_max = std::getenv("P2_LONG_RMS") ? std::atof(std::getenv("P2_LONG_RMS")) : 1.0;   // test
			if (!(long_rms < long_max && long_rms < hf.rms)) lv_long = 0;
			// Long packets (2026-10-20, TODO A: firmware -> firmware P2 links, our
			// decoder on the same noisy audio, a bench tool): at noise 4000
			// the firmware took 96 % of L2 long packets first time, we 18-29 %.
			// - The bias came from the 8 header words only; the 320 data steps
			//   (M-th power) give it far better, the header (or, without one, the
			//   AFC frame: residual ~0) only picking the alias: with the true
			//   timing 25 % -> 100 %.
			// - A long packet whose header did not fit (rms < 1.0, as fix 52 for
			//   short ones) is decoded at the expected level, at the expected
			//   alignment only, a decode as bad as random input's refused
			//   (kLongMaxMetric: its 1st percentile; good decodes 42-107 at L2).
			// - Its soft values go to memory ARQ (the firmware needed 2-3 copies at
			//   noise 3000-5000 where we had no long combining at all).
			// P2_LONG_OLD: the header-only decode, as before.
			static const bool long_old = std::getenv("P2_LONG_OLD") != nullptr;
			const double want_st = double(exp) + (tracked ? tk.dt : 0.0);
			const bool at_exp = std::fabs(d.start + (o - 1) * T_ - want_st) <= (tracked ? 0.6 : 1.6) * T_;
			const int lvx = lv_long ? lv_long : (!long_old && allow_long && !bk_only && long_known(level_) && at_exp ? level_ : 0);
			if (lvx) {
				if (lv_long) hdr_seen[idx] = 1;
				steps_of(d, swap, o, 329, st);
				std::vector<double> sbuf(640), wbuf(640);
				auto s = reinterpret_cast<double (*)[2]>(sbuf.data());
				auto ww = reinterpret_cast<double (*)[2]>(wbuf.data());
				// s: the data steps less `bias`. Fix 216: filled without a decode where only the steps
				// are needed (the soft decode, memory ARQ); each refill was a full hard Viterbi decode,
				// two of them on the way to the soft decode (Pi 4: ~65 ms against a ~27 ms budget).
				static const bool refill_old = std::getenv("P2_LONG_REFILL_OLD") != nullptr;   // test: as before
				auto fill_s = [&](double bias) {
					for (int k = 0; k < 320; k++) for (int c = 0; c < 2; c++) s[k][c] = std::fmod(st[size_t(8 + k)][size_t(c)] - bias + 32, 16);
				};
				auto decode_with = [&](double bias) { fill_s(bias); return p2::decode_steps(lvx, s, 40); };
				auto refill = [&](double bias) { if (refill_old) decode_with(bias); else fill_s(bias); };
				double bias = long_bias;
				if (!long_old) {                               // the bias from the data, the alias nearest the header's (or 0)
					const int M = 1 << p2::kBitsPerSym[lvx], base = lvx <= 2 ? 2 : 0;
					std::complex<double> acc = 0;
					for (int k = 0; k < 320; k++) for (int c = 0; c < 2; c++) acc += std::polar(1.0, 2 * M_PI * M * (st[size_t(8 + k)][size_t(c)] - base) / 16.0);
					const double step = 16.0 / M, ref = lv_long ? long_bias : 0.0;
					bias = 16.0 * std::arg(acc) / (2 * M_PI * M);
					bias += step * std::round((ref - bias) / step);
				}
				auto res = decode_with(bias);
				if (!res.ok && lv_long && !long_old) { const auto r2 = decode_with(long_bias); if (r2.ok) { res = r2; bias = long_bias; } else refill(bias); }
				// The amplitude-weighted soft decode when the hard one fails (steps in
				// a fade or a noise burst count less; as memory ARQ already decodes).
				// Single long copies, firmware ⇄ firmware links (a bench tool):
				// CCIR moderate + noise 1000 / 2000 / 3000, L3 first copies 81 → 96,
				// 34 → 78, 17 → 44 % (the firmware 85 / 41 / 17); white noise 5000,
				// L2 10 → 79 % (the firmware 3 %). Without a header a soft decode is
				// refused at the random input's 1st percentile (kLongMaxSoft).
				// P2_NO_WSOFT: hard decisions only (test).
				static constexpr int kLongMaxSoft[5] = {0, 103, 211, 208, 100};
				bool soft = false;
				if (!res.ok && !long_old && wsoft_on()) {
					if (refill_old) decode_with(bias);         // (s already holds the steps with this bias)
					step_weights(d, swap, o, 8, 320, ww);
					const auto r4 = p2::decode_llr(lvx, p2::level_llr(lvx, s, 40, ww), 40);
					if (r4.ok && (lv_long || r4.metric < kLongMaxSoft[lvx] || std::getenv("P2_HL_ANY"))) { res = r4; soft = true; }
				}
				static constexpr int kLongMaxMetric[5] = {0, 76, 152, 131, 54};
				if (res.ok && (lv_long || soft || res.metric < kLongMaxMetric[lvx] || std::getenv("P2_HL_ANY"))) {
					r.ok = true; r.lng = true; r.level = lvx; r.info = res.info; r.start = d.start + (o - 1) * T_; r.bias = bias; r.swap = swap; r.c[0] = d.c[0]; r.c[1] = d.c[1]; r.has_c = true; r.metric = res.metric;
					if (!lv_long) hdr_seen[idx] = 1;
					if (std::getenv("HL_DEBUG") && !lv_long) std::fprintf(stderr, "HL long swap %d o %d level %d metric %d\n", swap, o, lvx, res.metric);
					return;
				}
				if (marq_on() && !long_old && lvx == level_ && at_exp) {   // memory ARQ: this copy's soft values (s holds the data-bias steps)
					step_weights(d, swap, o, 8, 320, ww);
					r.llr = p2::level_llr(lvx, s, 40, std::getenv("P2_MARQ_NOWT") ? nullptr : ww); r.llr_level = lvx; r.llr_parity = -1; r.llr_rms = lv_long ? long_rms : 2.0;
					r.lng = true; r.start = d.start + (o - 1) * T_; r.bias = bias; r.swap = swap;
				}
				if (lv_long) return;                           // a long header: not a short packet
			}
			if (marq_on() && !r.lng) {                        // memory ARQ: this candidate's soft values at the expected level, header or not
				const int want = p3_ ? 1 : level_;
				const auto hw = p2::fit_header(h, want);
				std::vector<std::array<double, 2>> sw;
				steps_of(d, swap, o, 80, sw);
				double s2[72][2], w2[72][2];
				for (int k = 0; k < 72; k++) { s2[k][0] = std::fmod(sw[size_t(8 + k)][0] - hw.bias + 32, 16); s2[k][1] = std::fmod(sw[size_t(8 + k)][1] - hw.bias + 32, 16); }
				step_weights(d, swap, o, 8, 72, w2);
				r.llr = p2::level_llr(want, s2, 9, std::getenv("P2_MARQ_NOWT") ? nullptr : w2); r.llr_level = want; r.llr_parity = hw.parity; r.llr_rms = hw.rms;
				r.start = d.start + (o - 1) * T_; r.bias = hw.bias; r.swap = swap;
			}
			// Header fit limit (rms, 1/16 turn): 2.0 since fix 52 (was 1.0: at
			// low SNR most SL1 packets were rejected here undecoded; the CRC
			// decides). P2_HDR_RMS overrides (test).
			static const double hdr_max = std::getenv("P2_HDR_RMS") ? std::atof(std::getenv("P2_HDR_RMS")) : 2.0;
			if (hf.rms > hdr_max) return;     // no header: maybe its start was lost (see below)
			hdr_seen[idx] = 1;
			steps_of(d, swap, o, 80, st);
			double s[72][2];
			for (int k = 0; k < 72; k++) { s[k][0] = std::fmod(st[size_t(8 + k)][0] - hf.bias + 32, 16); s[k][1] = std::fmod(st[size_t(8 + k)][1] - hf.bias + 32, 16); }
			auto res = p2::decode_steps(hf.level, s, 9);
			if (!res.ok && !p3_ && wsoft_on()) {            // the amplitude-weighted soft decode (fix 105): L2 at -7.2 dB 40 → 84 %, L3 at -0.1 dB 62 → 100 % (sl1_sens)
				double w2[72][2];
				step_weights(d, swap, o, 8, 72, w2);
				const auto rs = p2::decode_llr(hf.level, p2::level_llr(hf.level, s, 9, w2), 9);
				// A genuine header over noisy data passes the header gate, so each
				// failing packet's soft decode is one more 2^-16 chance of a false
				// CRC pass: refused at the random input's 1st percentile of the soft
				// metric (the monitor, noise 2000: an L4 idle packet decoded to
				// garbage at metric 27, the limit 21).
				static constexpr int kShortMaxSoft[5] = {0, 22, 45, 44, 21};
				if (rs.ok && (rs.metric < kShortMaxSoft[hf.level] || std::getenv("P2_HL_ANY"))) res = rs;
			}
			std::vector<uint8_t> inf = res.info;
			if (std::getenv("P2_FP_DEBUG") && !p3_) std::fprintf(stderr, "P2 cand exp %.3f swap %d o %d level %d par %d rms %.2f bias %.2f metric %d ok %d\n", exp / cfg_.fs, swap, o, hf.level, hf.parity, hf.rms, hf.bias, int(res.metric), int(res.ok));   // experiment
			if (std::getenv("SL1_DEBUG") && p3_) { std::vector<uint8_t> t = inf; std::fprintf(stderr, "SL1 cand exp %.3f swap %d o %d level %d par %d rms %.2f bias %.2f ok %d\n", exp / cfg_.fs, swap, o, hf.level, hf.parity, hf.rms, hf.bias, int(hf.level == 1 && sl1_plain(t))); }   // experiment
			if (p3_ ? (hf.level == 1 && sl1_plain(inf)) : res.ok) {
				// a short packet: lng may still be set by the long attempt's memory-ARQ
				// copy (fix 102), and its status was then read as a long packet's
				// (garbage text from a short packet in a long-packet wait; found by
				// the monitor, 2026-10-20)
				r.ok = true; r.lng = false; r.level = hf.level; r.info = inf; r.start = d.start + (o - 1) * T_; r.bias = hf.bias; r.swap = swap; r.llr.clear(); r.metric = res.metric; r.hdr_rms = hf.rms; r.c[0] = d.c[0]; r.c[1] = d.c[1]; r.has_c = true;
				if (std::getenv("HDR_DEBUG")) std::fprintf(stderr, "HDR %s level %d parity %d status 0x%02x at %.3f\n", p3_ ? "P3" : "P2", hf.level, hf.parity,
				                                       inf.size() > 5 ? inf[p3_ ? size_t(p3::kSl1Data) : size_t(p2::data_bytes(hf.level, 9))] : 0, r.start / cfg_.fs);
				return;
			}
		};
		// Fix 151: with the timing tracked, the predicted candidate (the expected carrier
		// order, word 0 nearest the expected start) alone first; the six only when it fails.
		// All six cost two rounds on four cores and every one of them a full decode: a long
		// packet's search took 12-23 ms on the Pi 4 against a ~31 ms reply window. A packet
		// decodes at one candidate only in practice; P2_SEARCH_ALL: the six at once (test).
		static const bool search_all = std::getenv("P2_SEARCH_ALL") != nullptr;
		int first = -1;
		if (tracked && want_swap_ >= 0 && !search_all) {
			const double want = double(exp) + tk.dt;
			int bo = 0; double bd = 1e18;
			for (int o = 0; o < no; o++) { const double dd = std::fabs(d.start + (o - 1) * T_ - want); if (dd < bd) { bd = dd; bo = o; } }
			first = want_swap_ * no + bo;
			try_cand(first);
			if (cand[size_t(first)].ok) { cand[size_t(first)].path = 1; return cand[size_t(first)]; }
		}
		par::parallel_for(2 * no, [&](int idx) { if (idx != first) try_cand(idx); });
		for (auto &c : cand)
			if (c.ok) { c.path = 2; return c; }
		for (char hsn : hdr_seen) if (hsn) hdr = true;
		// memory ARQ: the failed candidate at the expected timing (closest
		// start to exp), the last good packet's carrier order first, then
		// the better header fit
		RxPacket f;
		double fd = 1e18;
		for (auto &c : cand) {
			if (c.llr.empty()) continue;
			const double dd = std::fabs(c.start - double(exp)) + (want_swap_ >= 0 && c.swap != want_swap_ ? 1e6 : 0) + c.llr_rms;
			if (dd < fd) { fd = dd; f = c; }
		}
		return f;
	};
	// Headerless: right after our own transmission the radio's receive
	// recovery can swallow the first pulses of the reply packet (seen on
	// IC-7300s). The data (words 9..80) is still there: decode it at the
	// link's level (P3: SL1) with the offset estimated from the data
	// (M-th power), and let the CRC decide. Only at the link level: the
	// other four made a failed decode take ~45-50 ms on the RF bench
	// (KB2PCN-5, 40 m, 2026-10-16: 100 of 103 packets dropped).
	// Only alignments within `win` of the expected start (every good
	// headerless decode in white noise was at it): 16 alignments made
	// random CRC passes, 19 an hour in pure noise (~1 per 4,000 decodes,
	// 16 x 2^-16); and a decode needing as many corrections as random
	// input does is refused (kHlMaxMetric: the random metric's 5th
	// percentile; a bench tool, 2026-10-16).
	auto headerless = [&](const Demod &dd_in, double want_start, double win, RxPacket &out) -> bool {
		static constexpr int kHlMaxMetric[5] = {0, 19, 36, 31, 13};
		const int lv = p3_ ? 1 : level_;
		const int k = p2::kBitsPerSym[lv], M = 1 << k, base = lv <= 2 ? 2 : 0;
		// The carrier order fixes the pairing: nibble A's carrier leads B's
		// by half a pulse. The timing pairs each carrier's first pulse, so
		// when the leading carrier's first pulse is lost (a cut start) its
		// first pulse is a word later and appears to lag: as found, swap 0
		// and 1 were one order paired right and the other paired a pulse
		// off, which never decodes. Each order is now paired with A
		// leading (slipped: the same number of trials, so the same
		// false-decode rate). A's cut packets (P2, 2026-10-19) decoded at
		// metric 0 this way and at no alignment before. P2_HL_NOSLIP: the old pairing.
		static const bool noslip = std::getenv("P2_HL_NOSLIP") != nullptr;
		for (int swap = 0; swap < 2; swap++) {
			const bool a_lags = dd_in.c[swap] > dd_in.c[1 - swap];
			Demod slip;
			if (a_lags && !noslip) slip = slipped(dd_in, 0, exp - samples(0.04), exp + samples(allow_long ? 3.36 : 0.86), maxp, sp);
			const Demod &dd = a_lags && !noslip ? slip : dd_in;
			for (int o = -6; o <= 1; o++) {      // z index of word 0 = 1 + o
				if (std::fabs(dd.start + o * T_ - want_start) > win) continue;
				const auto &A = swap ? dd.hi : dd.lo, &B = swap ? dd.lo : dd.hi;
				double s[72][2];
				std::complex<double> acc = 0;
				bool ok = true;
				for (int j = 0; j < 72 && ok; j++)
					for (int c = 0; c < 2; c++) {
						const auto &Z = c ? B : A;
						const int i1 = 1 + o + 9 + j, i0 = i1 - 1;
						if (i0 < 0 || i1 >= int(Z.size())) { ok = false; break; }
						s[j][c] = nib(Z[size_t(i0)], Z[size_t(i1)]);
						acc += std::polar(1.0, 2 * M_PI * M * (s[j][c] - base) / 16.0);
					}
				if (!ok) continue;
				const double bias = 16.0 * std::arg(acc) / (2 * M_PI * M);
				for (auto &v : s) for (double &x : v) x = std::fmod(x - bias + 32.0, 16.0);
				const auto res = p2::decode_steps(lv, s, 9);
				std::vector<uint8_t> inf = res.info;
				if (std::getenv("HL_DEBUG") && std::atoi(std::getenv("HL_DEBUG")) > 1) std::fprintf(stderr, "HLc exp %.3f lv %d swap %d o %d start-want %.1f dstart-exp %.1f c %.1f/%.1f bias %.2f metric %d ok %d\n", exp / cfg_.fs, lv, swap, o, dd.start + o * T_ - want_start, dd.start - double(exp), dd.c[0] - double(exp), dd.c[1] - double(exp), bias, int(res.metric), int(res.ok));   // experiment
				if (!(p3_ ? (lv == 1 && sl1_plain(inf)) : res.ok)) continue;
				if (res.metric >= kHlMaxMetric[lv] && !std::getenv("P2_HL_ANY")) continue;
				out.ok = true; out.level = lv; out.info = inf; out.start = dd.start + o * T_; out.swap = swap; out.metric = res.metric; out.bias = bias;
				out.c[0] = dd.c[0]; out.c[1] = dd.c[1]; out.has_c = true;
				if (std::getenv("HL_DEBUG")) std::fprintf(stderr, "HL swap %d o %d start-exp %.1f metric %d\n", swap, o, out.start - double(exp), res.metric);
				return true;
			}
		}
		return false;
	};
	RxPacket fail;                                    // best failed candidate (memory ARQ)
	const double want_start = double(exp) + (tracked ? tk.dt : 0.0);
	if (tracked) {
		// the tracked timing, in the expected carrier order
		double c2[2];
		tracked_c2(exp, tk, cc, c2);
		const auto tq0 = std::chrono::steady_clock::now();
		const Demod dp = demod(exp - samples(0.04), exp + samples(allow_long ? 3.36 : 0.86), maxp, sp, c2, fshift);
		const auto tq1 = std::chrono::steady_clock::now();
		if (dp.ok) {
			bool h = false;
			RxPacket p = search(dp, h);
			static const bool prof = std::getenv("P2_PROF") != nullptr;   // every decode's two stages (slow CPUs: the Pi 4)
			if ((allow_long && std::getenv("P2_TIME_LONG")) || prof) std::fprintf(stderr, "%s decode: demod %.1f ms, search %.1f ms, ok %d lng %d\n", allow_long ? "long" : "short",
				std::chrono::duration<double, std::milli>(tq1 - tq0).count(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tq1).count(), int(p.ok), int(p.lng));
			if (p.ok) { p.pred = want_swap_; return p; }
			fail = p;
			RxPacket hl;
			if (headerless(dp, want_start, 0.6 * T_, hl)) { hl.path = 3; hl.pred = want_swap_; return hl; }
		}
	}
	// Acquisition (no track yet): the expected start with the modulation's
	// own carrier lead and stagger, both carrier orders, before the
	// packet's own timing, which at low SNR is rarely right: the first good
	// decode then set the track, and a timing ~1 ms off stayed (a tracked
	// decode keeps the timing it was given; sl1_sens -8.9 dB seed 1: 13 of
	// the first 23 packets repeated, 88 % first copies; 97 % with this).
	// Lead and stagger as the track learns them on clean packets: lead 77
	// samples at 9600 Hz; stagger 48.75 (ours) or 58 (the emulated
	// firmware's data packets) [bb-ext]. P2_NO_PRIOR: off (test).
	static const bool prior_off = std::getenv("P2_NO_PRIOR") != nullptr;
	// All four are tried and the best header fit kept: the first that
	// decoded set the track, and our stagger decodes the TNC's clean
	// packets too, ~1 ms off for the rest of the link (emulated firmware,
	// 60 Hz: 4,637 → 3,953 characters until this).
	if (!tracked && !bk_only && !prior_off && track_on()) {
		RxPacket best;
		for (int k = 0; k < 4; k++) {
			const int sw = k & 1;
			const double lead = double(exp) + 77.0 * cfg_.fs / 9600.0, lag = lead + (k < 2 ? 58.0 : 48.75) * cfg_.fs / 9600.0;
			const double c2[2] = {sw ? lag : lead, sw ? lead : lag};
			const Demod dp = demod(exp - samples(0.04), exp + samples(allow_long ? 3.36 : 0.86), maxp, sp, c2, fshift);
			if (!dp.ok) continue;
			bool h = false;
			RxPacket p = search(dp, h);
			if (p.ok) { if (!best.ok || p.hdr_rms < best.hdr_rms) best = p; continue; }
			if (fail.llr.empty() || (!p.llr.empty() && p.llr_rms < fail.llr_rms)) fail = p;
		}
		if (best.ok) { best.path = 4; return best; }
	}
	// the packet's own timing (timing_of, or cc from its first 0.3 s): only
	// when the tracked timing did not decode (it costs a fit)
	const Demod d = demod(exp - samples(0.04), exp + samples(allow_long ? 3.36 : 0.86), maxp, sp, cc, fshift);
	if (!d.ok) return tracked ? fail : r;
	if (!bk_only) {
		bool hdr = false;
		RxPacket p = search(d, hdr);
		if (p.ok) { p.path = 5; p.pred = tracked ? want_swap_ : -1; return p; }
		if (fail.llr.empty()) fail = p;
		// No header at any candidate: on the RF bench one carrier's first
		// pulse was sometimes found a pulse early (energy there passed the
		// 40 % threshold), so the carriers were a pulse apart and nothing
		// fitted (a third of the real reference unit's packets). Retry with the
		// earlier carrier a pulse later, then the later one a pulse earlier.
		if (!hdr) {
			for (int v = 0; v < 2; v++) {
				const Demod d2 = slipped(d, v, exp - samples(0.04), exp + samples(allow_long ? 3.36 : 0.86), maxp, sp);
				if (!d2.ok) continue;
				bool h2 = false;
				p = search(d2, h2);
				if (p.ok) { p.path = 6; p.pred = tracked ? want_swap_ : -1; return p; }
				if (!tracked && p.llr_rms < fail.llr_rms) fail = p;
			}
		}
	}
	std::vector<std::array<double, 2>> st;
	// Break-in packet: find the 24-pulse pattern (its first pulses may be
	// lost to our transmit-to-receive turnaround), take the frequency-
	// offset bias from the known pattern, then decode the 56 data pulses.
	auto bk_search = [&](const Demod &d) -> bool {
		for (int swap = 0; swap < 2; swap++)
			for (int o = -6; o <= 2; o++) {
				const auto &A = swap ? d.hi : d.lo, &B = swap ? d.lo : d.hi;
				auto stp = [&](const std::vector<std::complex<double>> &Z, int w) -> double {   // step of word w
					const int i1 = 1 + o + w;
					if (i1 < 1 || i1 >= int(Z.size())) return -1;
					return nib(Z[size_t(i1 - 1)], Z[size_t(i1)]);
				};
				std::complex<double> acc = 0;
				int good = 0, seen = 0;
				for (int w = 1; w <= 24; w++)
					for (int c = 0; c < 2; c++) {
						const double v = stp(c ? B : A, w);
						if (v < 0) continue;
						const double want = p2arq::kBkPattern[w - 1] == 'a' ? 10.0 : 2.0;
						acc += std::polar(1.0, 2 * M_PI * (v - want) / 16.0);
						seen++;
					}
				if (seen < 20) continue;
				const double bias = 16.0 * std::arg(acc) / (2 * M_PI);
				for (int w = 1; w <= 24; w++)
					for (int c = 0; c < 2; c++) {
						const double v = stp(c ? B : A, w);
						if (v < 0) continue;
						const double want = p2arq::kBkPattern[w - 1] == 'a' ? 10.0 : 2.0;
						good += std::fabs(std::fmod(v - bias - want + 24.0, 16.0) - 8.0) < 2.0;
					}
				// fix 63: a break-in found at the tracked timing (cc given) is
				// decoded at its expected alignment whatever the pattern's
				// hard agreement (the gate failed most at low SNR); the CRC decides
				const bool at_exp = bk_only && cc && std::lround((double(exp) - d.start) / T_) == o;
				if (good < int(0.8 * seen) && !at_exp) continue;
				double s7[56][2];
				bool ok = true;
				for (int k = 0; k < 56 && ok; k++)
					for (int c = 0; c < 2; c++) {
						const double v = stp(c ? B : A, 25 + k);
						if (v < 0) { ok = false; break; }
						s7[k][c] = std::fmod(v - bias + 32.0, 16.0);
					}
				if (!ok) continue;
				if (p3_) {                               // P3 break-in: level-1 coding, own CRC/status
					const auto res = p2::decode_steps(1, s7, 7);
					uint8_t d3[p3::kBkData];
					bool more = false;
					if (debug_) {
						std::string w;
						for (int k = -24; k < 60; k++) {
							const double v = stp(A, 25 + k), u = stp(B, 25 + k);
							if (v < 0 || u < 0) { w += " --"; continue; }
							char q[8]; std::snprintf(q, sizeof q, " %x%x", int(std::lround(std::fmod(v - bias + 32.0, 16.0))) & 15, int(std::lround(std::fmod(u - bias + 32.0, 16.0))) & 15);
							w += q;
						}
						log("bk words" + w);
						char t[96];
						std::snprintf(t, sizeof t, "bk swap %d o %d metric %d info %02x %02x %02x %02x %02x %02x", swap, o, res.metric,
						              res.info[0], res.info[1], res.info[2], res.info[3], res.info[4], res.info[5]);
						log(t);
					}
					int type = 0;
					bool qrt = false, over = false;
					if (!p3::bk_check(res.info, d3, more, &type, &qrt, &over)) continue;
					r.ok = true; r.bk = true; r.level = 1; r.info.assign(d3, d3 + p3::kBkData); r.start = d.start + o * T_; r.bias = bias;
					r.bkc[0] = d.c[0] + o * T_; r.bkc[1] = d.c[1] + o * T_; r.has_bkc = true;
					r.info.push_back(uint8_t(type << 2 | (over ? 0x40 : 0) | (qrt ? 0x80 : 0)));   // pseudo status: data type, over, QRT
					return true;
				}
				for (int t = 0; t <= 4; t++) {
					const int lv = t == 0 ? level_ : t;
					if (t > 0 && lv == level_) continue;
					const auto res = p2::decode_steps(lv, s7, 7);
					if (!res.ok) continue;
					r.ok = true; r.bk = true; r.level = lv; r.info = res.info; r.start = d.start + o * T_; r.bias = bias;
					return true;
				}
			}
		return false;
	};
	if (bk_only) {
		if (bk_search(d)) return r;
		// The carrier slip of the short-packet search above: looks_like_bk
		// may have found the pattern only with one carrier moved a pulse
		// (RF bench: the real reference unit's break-in after our long packets);
		// decoding with the unmoved timing then always failed.
		for (int v = 0; v < 2; v++) {
			const Demod d2 = slipped(d, v, exp - samples(0.04), exp + samples(0.86), maxp, sp);
			if (d2.ok && bk_search(d2)) return r;
		}
	}
	if (!bk_only) {
		RxPacket hl;
		if (headerless(d, want_start, tracked ? 0.6 * T_ : 1.6 * T_, hl)) { hl.path = 7; hl.pred = tracked ? want_swap_ : -1; return hl; }
	}
	return r.ok ? r : fail;
}

void P2Station::marq_ahead(RxPacket &q, const MarqSnap &m)
{
	if (q.ok || q.llr.empty() || q.bk || m.n <= 0 || q.llr_level != m.level || m.sum.size() != q.llr.size() || m.n >= kMarqRestart) return;
	std::vector<float> sum(m.sum);
	const float w = marq_weight(q);
	for (size_t i = 0; i < sum.size(); i++) sum[i] += w * q.llr[i];
	const auto res = p2::decode_llr(m.level, sum, llr_ppr(m.level, sum.size()));
	q.mq_pre = true; q.mq_pre_ok = res.ok; q.mq_ver = m.ver; q.mq_info = res.info; q.mq_metric = res.metric;
}

void P2Station::trk_retime(const double *cc, double *c2) const
{
	if (!cc) return;
	const double d0 = cc[0] - c2[0], d1 = cc[1] - c2[1];
	static const bool each = std::getenv("TRK_EACH") != nullptr;
	if (each) std::fprintf(stderr, "trk early timing against the track %.2f / %.2f samples\n", d0, d1);
	if (trk_retime_gain() == 0.0) return;
	if (std::fabs(d0) > 0.3 * T_ || std::fabs(d1) > 0.3 * T_ || std::fabs(d0 - d1) > 0.15 * T_) return;
	const double dm = trk_retime_gain() * 0.5 * (d0 + d1);
	c2[0] += dm; c2[1] += dm;
	if (each) std::fprintf(stderr, "trk retime %.2f samples\n", dm);
}

bool P2Station::trk_refine(const RxPacket &q, double c[2], const Snap *sp) const
{
	static const bool off = std::getenv("P2_NO_TRK_REFINE") != nullptr;
	const auto t_begin = std::chrono::steady_clock::now();
	if (off || q.lng || q.info.empty() || q.level < 1 || q.level > 4) return false;
	std::vector<uint8_t> words;
	if (p3_) {
		if (q.level != 1 || q.info.size() < size_t(p3::kSl1Data + 1)) return false;
		uint8_t inf8[8];
		p3::sl1_make_info(q.info.data(), q.info[size_t(p3::kSl1Data)], inf8);
		words = p2::encode_data(1, inf8, 9);
	} else {
		if (q.info.size() < size_t(p2::data_bytes(q.level, 9) + 3)) return false;
		words = p2::encode_data(q.level, q.info.data(), 9);
	}
	const double h = 0.125 * T_;
	const double fsh = (demod_afc_on() && std::fabs(afc_hz()) > 1.0 && !std::getenv("P2_NO_DEMOD_AFC")) ? afc_hz() : 0.0;
	const double fq[2] = {(p3_ ? p3::kCsLo : cfg_.tone_lo) + fsh, (p3_ ? p3::kCsHi : cfg_.tone_hi) + fsh};
	const auto &tab = p3_ ? p3::cs_pulse_table(cfg_.fs) : p2::pulse_table(cfg_.fs);
	const int half = int(std::lround(2 * T_));
	const int o = int(std::lround((q.start - (std::min(q.c[0], q.c[1]) - nw_ / 2.0)) / T_)) + 1;
	double m[3][2];
	for (int car = 0; car < 2; car++) {
		const bool is_a = (car == 0) == (q.swap == 0);
		const int64_t ta = int64_t(std::floor(q.c[car] - h + double(o + 7) * T_)) - half - 2;
		const int64_t tb = int64_t(std::ceil(q.c[car] + h + double(o + 79) * T_)) + half + 2;
		if (sp ? (ta < sp->t0 || tb > sp->end + half) : (ta < 1 || tb >= n_ || n_ - ta >= kRing - 2)) return false;
		std::vector<std::complex<double>> bb(size_t(tb - ta));
		const double w0 = -2 * M_PI * fq[car] / cfg_.fs;
		const std::complex<double> rot = std::polar(1.0, w0);
		std::complex<double> ph = std::polar(1.0, w0 * double(ta % 96000));
		for (int64_t t = ta; t < tb; t++) { bb[size_t(t - ta)] = double(sp ? sp->at(t) : raw_[size_t(t % kRing)]) * ph; ph *= rot; }
		for (int k = 0; k < 3; k++) {
			const double dd = (k - 1) * h;
			std::complex<double> prev = 0, acc = 0;
			for (int w = 8; w <= 80; w++) {
				const double ctr = q.c[car] + dd + double(o + w - 1) * T_;
				const int64_t t0 = int64_t(std::lround(ctr)) - half;
				std::complex<double> z = 0;
				for (size_t i = 0; i < tab.size(); i++) z += tab[i] * bb[size_t(t0 + int64_t(i) - ta)];
				if (w > 8) {
					const int want = is_a ? words[size_t(w - 9)] >> 4 : words[size_t(w - 9)] & 15;
					acc += std::abs(prev) * std::abs(z) * std::polar(1.0, 2 * M_PI * (nib(prev, z) - want) / 16.0);
				}
				prev = z;
			}
			m[k][car] = std::abs(acc);
		}
	}
	for (int car = 0; car < 2; car++) {
		const double a = m[0][car], b = m[1][car], cc = m[2][car], den = a - 2 * b + cc;
		double e = 0;
		if (den < 0) e = std::max(-1.0, std::min(1.0, 0.5 * (a - cc) / den)) * h;
		else e = a > cc ? -h : cc > a ? h : 0;
		c[car] = q.c[car] + e;
	}
	if (std::getenv("TRK_EACH")) std::fprintf(stderr, "trk refine lo %+.2f hi %+.2f (o %d lvl %d) %.0f us\n", c[0] - q.c[0], c[1] - q.c[1], o, q.level,
	                                         std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t_begin).count());
	return true;
}

}  // namespace fmodem
