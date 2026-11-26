// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// The functions of p2_unproto_rx.hpp (out of the header, so that they build in parallel).
#include "p2_unproto_rx.hpp"

namespace fmodem {

UnprotoPacket unproto_decode_cand(const UnprotoCand &c)
{
	UnprotoPacket out;
	double steps[72][2];
	const auto hf = p2::fit_header(c.hs);
	for (int k = 0; k < 72; k++) for (int q = 0; q < 2; q++) steps[k][q] = std::fmod(c.steps[k][q] - hf.bias + 32.0, 16.0);
	for (int t = 0; t <= 4; t++) {
		const int lv = t == 0 ? hf.level : t;
		if (lv < 1 || lv > 4 || (t > 0 && lv == hf.level)) continue;
		const auto r = p2::decode_steps(lv, steps);
		if (!r.ok) continue;
		const int n = p2::kDataBytes[lv];
		out.ok = true; out.level = lv; out.metric = r.metric; out.status = r.info[size_t(n)]; out.counter = out.status & 3;
		out.text = text::decode((out.status >> 2) & 7, r.info.data(), n, true);   // the data type: ASCII, Huffman or PMC, as in ARQ
		return out;
	}
	return out;
}

UnprotoPacket unproto_combine(const std::vector<UnprotoCand> &a, const std::vector<UnprotoCand> &b)
{
	auto mean = [](double x, double y) {
		const std::complex<double> s = std::polar(1.0, 2 * M_PI * x / 16.0) + std::polar(1.0, 2 * M_PI * y / 16.0);
		return std::fmod(16.0 * std::arg(s) / (2 * M_PI) + 32.0, 16.0);
	};
	for (const auto &ca : a)
		for (const auto &cb : b) {
			UnprotoCand m;
			for (int i = 0; i < 8; i++) for (int q = 0; q < 2; q++) m.hs[i][q] = mean(ca.hs[i][q], cb.hs[i][q]);
			for (int k = 0; k < 72; k++) for (int q = 0; q < 2; q++) m.steps[k][q] = mean(ca.steps[k][q], cb.steps[k][q]);
			const UnprotoPacket p = unproto_decode_cand(m);
			if (p.ok) return p;
		}
	return UnprotoPacket{};
}

UnprotoPacket unproto_decode_burst(const std::vector<float> &x, int fs, size_t b0, float lo, float hi, std::vector<UnprotoCand> *cands, UnprotoTiming *tm)
{
	UnprotoPacket out;
	const double T = 0.010 * fs;
	auto nibf = [](const std::complex<double> &a, const std::complex<double> &b) { return std::fmod(-16.0 * std::arg(b * std::conj(a)) / (2 * M_PI) + 32.0, 16.0); };
	// one alignment: the carriers' pulse trains from cA / cB (A = the carrier whose steps come first)
	auto try_at = [&](double cA, double cB, double fA, double fB, bool a_is_lo) -> bool {
		const auto z = p2::ls_pulses(x, fs, cA, cB, fA, fB, 84);
		if (z.size() < 168) return false;
		for (int skip = 7; skip <= 9; skip++) {
			UnprotoCand c;
			for (int k = 0; k < 72; k++) { c.steps[k][0] = nibf(z[size_t(skip + 1 + k)], z[size_t(skip + 2 + k)]); c.steps[k][1] = nibf(z[size_t(84 + skip + 1 + k)], z[size_t(84 + skip + 2 + k)]); }
			for (int i = 0; i < 8; i++) { c.hs[i][0] = nibf(z[size_t(skip + i - 7)], z[size_t(skip + i - 6)]); c.hs[i][1] = nibf(z[size_t(84 + skip + i - 7)], z[size_t(84 + skip + i - 6)]); }
			if (cands) cands->push_back(c);
			out = unproto_decode_cand(c);
			if (out.ok) {
				if (tm) { tm->got = true; tm->out_lo = (a_is_lo ? cA : cB) + skip * T; tm->out_hi = (a_is_lo ? cB : cA) + skip * T; }
				return true;
			}
		}
		return false;
	};
	// Fix 248: the timing the packets before it give first (the burst's own first pulses, below, are
	// found badly in noise), both carrier orders (the leading carrier alternates per copy), the
	// pulse grid as predicted and 1 ms either side; skip covers a pulse either way.
	// The leading carrier alternates per transmission, so the carriers' timings are tried swapped too
	// (first as tm->swap predicts: an odd number of transmissions on).
	if (tm && tm->pred) {
		static const double offs[3] = {0.0, -0.1, 0.1};
		for (int sw = 0; sw < 2; sw++)
			for (double o : offs)
				for (int order = 0; order < 2; order++) {
					const bool x2 = (sw != 0) != tm->swap;
					const double cl = (x2 ? tm->hi : tm->lo) - 8 * T + o * T, ch = (x2 ? tm->lo : tm->hi) - 8 * T + o * T;
					const bool ok = order ? try_at(ch, cl, hi, lo, false) : try_at(cl, ch, lo, hi, true);
					if (ok) { tm->by_pred = true; return out; }
				}
		out = UnprotoPacket{};
	}
	const int ntap = int(0.01 * fs) | 1;
	static thread_local std::vector<double> lp;
	static thread_local int lp_fs = 0;
	if (lp_fs != fs) {
		lp.assign(size_t(ntap), 0.0);
		double sum = 0;
		for (int n = 0; n < ntap; n++) {
			const double k = n - (ntap - 1) / 2.0, fc = 80.0 / fs;
			lp[size_t(n)] = (k == 0 ? 2 * fc : std::sin(2 * M_PI * fc * k) / (M_PI * k)) * (0.54 - 0.46 * std::cos(2 * M_PI * n / (ntap - 1)));
			sum += lp[size_t(n)];
		}
		for (auto &v : lp) v /= sum;
		lp_fs = fs;
	}
	auto pulses = [&](double fc, bool at_b0) {
		const size_t a = b0 > size_t(0.03 * fs) ? b0 - size_t(0.03 * fs) : 0, len = size_t(0.9 * fs);
		std::vector<std::complex<double>> bb(len), f(len);
		for (size_t n = 0; n < len && a + n < x.size(); n++) bb[n] = double(x[a + n]) * std::polar(1.0, -2 * M_PI * fc * double(a + n) / fs);
		for (size_t n = 0; n < len; n++) {
			std::complex<double> acc = 0;
			for (int k = 0; k < ntap; k++) { const long i = long(n) + k - ntap / 2; if (i >= 0 && i < long(len)) acc += lp[size_t(k)] * bb[size_t(i)]; }
			f[n] = acc;
		}
		int best_ph = 0; double best = -1;
		for (int ph = 0; ph < int(T); ph++) {
			double s = 0; int c = 0;
			for (double t = ph; t < len; t += T, c++) s += std::abs(f[size_t(t)]);
			if (s / c > best) { best = s / c; best_ph = ph; }
		}
		std::vector<std::complex<double>> p;
		for (double t = best_ph; t < len; t += T) p.push_back(f[size_t(t)]);
		double mx = 0; for (auto &v : p) mx = std::max(mx, std::abs(v));
		size_t st = 0; while (st < p.size() && std::abs(p[st]) < 0.4 * mx) st++;
		if (at_b0) st = size_t(std::max(0.0, std::ceil((double(b0) - 0.5 * T - double(a) - best_ph) / T)));   // fix 248: b0 is the burst's start
		if (st > p.size()) st = p.size();
		return std::make_pair(std::vector<std::complex<double>>(p.begin() + long(st), p.end()), double(a) + best_ph + double(st) * T);
	};
	const int nw = int(std::lround(1.6 * T));
	// Fix 248: b0 known to be the burst's start (the grid found by its energy, UnprotoRx::comb_): each
	// carrier's first pulse is the one at b0 (+- skip), then as before (the first pulse above 0.4 of
	// the strongest: in noise often a noise pulse before the burst)
	for (int m = (tm && tm->b0_start) ? 0 : 1; m < 2; m++) {
		const auto Q1 = pulses(lo - 12.5, m == 0), Q2 = pulses(hi - 12.5, m == 0);
		if (Q1.first.size() < 81 || Q2.first.size() < 81) continue;
		for (int order = 0; order < 2; order++) {
			const auto &QA = order ? Q2 : Q1, &QB = order ? Q1 : Q2;
			const double fA = order ? hi : lo, fB = order ? lo : hi;
			if (try_at(QA.second - T - nw / 2.0, QB.second - T - nw / 2.0, fA, fB, order == 0)) return out;
		}
	}
	return out;
}

UnprotoPacket unproto_decode_long(const std::vector<float> &x, int fs, size_t b0, float lo, float hi, const uint8_t (*lh)[2][8])
{
	UnprotoPacket out;
	const double T = 0.010 * fs;
	const int ntap = int(0.01 * fs) | 1;
	std::vector<double> lp(static_cast<size_t>(ntap));
	{ double sum = 0; for (int n = 0; n < ntap; n++) { const double k = n - (ntap - 1) / 2.0, fc = 80.0 / fs;
	  lp[size_t(n)] = (k == 0 ? 2 * fc : std::sin(2 * M_PI * fc * k) / (M_PI * k)) * (0.54 - 0.46 * std::cos(2 * M_PI * n / (ntap - 1))); sum += lp[size_t(n)]; }
	  for (auto &v : lp) v /= sum; }
	auto first_pulse = [&](double fc) {                    // the first strong pulse's centre (the first 0.3 s are enough)
		const size_t a = b0 > size_t(0.03 * fs) ? b0 - size_t(0.03 * fs) : 0, len = size_t(0.3 * fs);
		std::vector<std::complex<double>> bb(len), f(len);
		for (size_t n = 0; n < len && a + n < x.size(); n++) bb[n] = double(x[a + n]) * std::polar(1.0, -2 * M_PI * fc * double(a + n) / fs);
		for (size_t n = 0; n < len; n++) { std::complex<double> acc = 0; for (int k = 0; k < ntap; k++) { const long i = long(n) + k - ntap / 2; if (i >= 0 && i < long(len)) acc += lp[size_t(k)] * bb[size_t(i)]; } f[n] = acc; }
		int best_ph = 0; double best = -1;
		for (int ph = 0; ph < int(T); ph++) { double sm = 0; int c = 0; for (double t = ph; t < len; t += T, c++) sm += std::abs(f[size_t(t)]); if (sm / c > best) { best = sm / c; best_ph = ph; } }
		double mx = 0; std::vector<double> mg; for (double t = best_ph; t < len; t += T) { mg.push_back(std::abs(f[size_t(t)])); mx = std::max(mx, mg.back()); }
		size_t st = 0; while (st < mg.size() && mg[st] < 0.4 * mx) st++;
		return double(a) + best_ph + double(st) * T;
	};
	const double c1 = first_pulse(lo - 12.5), c2 = first_pulse(hi - 12.5);
	const int nw = int(std::lround(1.6 * T));
	auto nib = [](const std::complex<double> &a, const std::complex<double> &b) { return std::fmod(-16.0 * std::arg(b * std::conj(a)) / (2 * M_PI) + 32.0, 16.0); };
	const int N = 334;
	for (int order = 0; order < 2; order++) {
		const double cA = order ? c2 : c1, cB = order ? c1 : c2;
		const double fA = order ? hi : lo, fB = order ? lo : hi;
		const auto z = p2::ls_pulses(x, fs, cA - T - nw / 2.0, cB - T - nw / 2.0, fA, fB, N);
		if (int(z.size()) < 2 * N) continue;
		for (int skip = 7; skip <= 9; skip++) {
			double h[8][2];
			for (int i = 0; i < 8; i++) { h[i][0] = nib(z[size_t(skip + i - 7)], z[size_t(skip + i - 6)]); h[i][1] = nib(z[size_t(N + skip + i - 7)], z[size_t(N + skip + i - 6)]); }
			int lv_best = 0, par_best = 0; double bias_best = 0, rms_best = 99;
			for (int lv = 1; lv <= 4; lv++) for (int par = 0; par < 2; par++) {
				if (!lh[lv][par][0]) continue;
				std::complex<double> acc = 0; double dd[16];
				for (int i = 0; i < 8; i++) for (int c = 0; c < 2; c++) { const int want = c ? lh[lv][par][i] & 15 : lh[lv][par][i] >> 4; dd[2 * i + c] = h[i][c] - want; acc += std::polar(1.0, 2 * M_PI * dd[2 * i + c] / 16.0); }
				const double bias = 16.0 * std::arg(acc) / (2 * M_PI);
				double e2 = 0; for (double v : dd) { const double e = std::fmod(v - bias + 1608.0, 16.0) - 8.0; e2 += e * e; }
				const double rms = std::sqrt(e2 / 16);
				if (rms < rms_best) { rms_best = rms; lv_best = lv; par_best = par; bias_best = bias; }
			}
			if (!lv_best || rms_best > 1.5) continue;
			std::vector<double> sb(640); auto st = reinterpret_cast<double (*)[2]>(sb.data());
			for (int k = 0; k < 320; k++) { st[k][0] = std::fmod(nib(z[size_t(skip + 1 + k)], z[size_t(skip + 2 + k)]) - bias_best + 32.0, 16.0);
			                                 st[k][1] = std::fmod(nib(z[size_t(N + skip + 1 + k)], z[size_t(N + skip + 2 + k)]) - bias_best + 32.0, 16.0); }
			const auto r = p2::decode_steps(lv_best, st, 40);
			if (!r.ok) continue;
			const int n = p2::data_bytes(lv_best, 40);
			out.ok = true; out.level = lv_best; out.metric = r.metric; out.status = r.info[size_t(n)]; out.counter = out.status & 3; (void)par_best;
			out.text = text::decode((out.status >> 2) & 7, r.info.data(), n, true);
			return out;
		}
	}
	return out;
}

}  // namespace fmodem
