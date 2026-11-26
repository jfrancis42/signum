// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// The functions of p3_levels.hpp (out of the header, so that they build in parallel).
#include "p3_levels.hpp"

namespace fmodem::p3 {

const Layout & layout(int level)
{
	static const Layout L2 = {2, true, 3, 1, {{82, 6, {3, 5, 7, 10, 12, 14}, {10, 12, 14, 3, 5, 7}}, {}}};
	static const Layout L3 = {3, false, 99, 1, {{82, 14, P3_D14_EVEN, P3_D14_ODD}, {}}};
	static const Layout L4 = {4, false, 99, 2, {{9, 16, P3_D16_EVEN, P3_D16_ODD}, {73, 14, P3_D14_EVEN, P3_D14_ODD}}};
	static const Layout L5 = {5, false, 99, 2, {{9, 18, P3_D18_EVEN, P3_D18_ODD}, {73, 16, P3_D16_EVEN, P3_D16_ODD}}};
	static const Layout L6 = {6, false, 99, 1, {{82, 18, P3_D18_EVEN, P3_D18_ODD}, {}}};   // all 18 carriers (black-box, SL6 captures)
	switch (level) { case 2: return L2; case 3: return L3; case 4: return L4; case 6: return L6; default: return L5; }
}

const Layout & layout_long(int level)
{
	static const Layout L2 = {2, true, 3, 1, {{330, 6, {3, 5, 7, 10, 12, 14}, {10, 12, 14, 3, 5, 7}}, {}}};
	static const Layout L3 = {3, false, 99, 1, {{330, 14, P3_D14_EVEN, P3_D14_ODD}, {}}};
	static const Layout L4 = {4, false, 99, 2, {{9, 16, P3_D16_EVEN, P3_D16_ODD}, {321, 14, P3_D14_EVEN, P3_D14_ODD}}};
	static const Layout L5 = {5, false, 99, 2, {{9, 18, P3_D18_EVEN, P3_D18_ODD}, {321, 16, P3_D16_EVEN, P3_D16_ODD}}};
	static const Layout L6 = {6, false, 99, 1, {{330, 18, P3_D18_EVEN, P3_D18_ODD}, {}}};
	switch (level) { case 2: return L2; case 3: return L3; case 4: return L4; case 6: return L6; default: return L5; }
}

std::vector<float> modulate_lv(int level, const std::vector<uint8_t> &words, double fs, bool odd, float amplitude, double lead_s, bool lng)
{
	const Layout &L = lng ? layout_long(level) : layout(level);
	const int np = L.npulses();
	const double half = lv_half(L), end = lead_s + (np - 1 + kStagger) * kT + half;
	std::vector<float> y(size_t(std::ceil(end * fs)) + 1, 0.f);
	int ph[18];
	for (int k = 0; k < 18; k++) ph[k] = (k * k) % 16;
	// Fast path as p3::modulate (fix 153): one pulse table per slot kind (leading or
	// staggered) when T and the lead are whole samples, carriers by phase rotation.
	static const bool slow = std::getenv("P3_MOD_SLOW") != nullptr;
	const double Tf = kT * fs, Lf = lead_s * fs;
	if (!slow && std::fabs(Tf - std::round(Tf)) < 1e-9 && std::fabs(Lf - std::round(Lf)) < 1e-9) {
		const long Tn = long(std::lround(Tf));
		long a0[2], b0[2]; std::vector<double> g[2];
		for (int lg = 0; lg < 2; lg++) {
			const double c0 = lead_s + (lg ? kStagger * kT : 0.0);
			a0[lg] = long(std::ceil((c0 - half) * fs)); b0[lg] = long(std::floor((c0 + half) * fs));
			g[lg].resize(size_t(b0[lg] - a0[lg] + 1));
			for (long i = a0[lg]; i <= b0[lg]; i++) g[lg][size_t(i - a0[lg])] = double(amplitude) * lv_pulse(L, double(i) / fs - c0);
		}
		for (int p = 0; p < np; p++) {
			const int nc = L.carriers(p);
			for (int j = 0; j < nc; j++) {
				const int k = L.carrier(p, j, odd);
				ph[k] = (ph[k] - (words[size_t(L.word_index(p, j))] & 15)) & 15;
				const int lg = L.lagging(j) ? 1 : 0;
				const long s0 = a0[lg] + long(p) * Tn;
				const long i0 = std::max(0L, s0), i1 = std::min(long(y.size()) - 1, b0[lg] + long(p) * Tn);
				const double w = 2 * M_PI * carrier_hz(k), phi = 2 * M_PI * ph[k] / 16.0;
				std::complex<double> z = std::polar(1.0, w * double(i0) / fs + phi);
				const std::complex<double> rot = std::polar(1.0, w / fs);
				for (long i = i0; i <= i1; i++) { y[size_t(i)] += float(g[lg][size_t(i - s0)] * z.real()); z *= rot; }
			}
		}
		return y;
	}
	for (int p = 0; p < np; p++) {
		const int nc = L.carriers(p);
		for (int j = 0; j < nc; j++) {
			const int k = L.carrier(p, j, odd);
			ph[k] = (ph[k] - (words[size_t(L.word_index(p, j))] & 15)) & 15;
			const double c = lead_s + p * kT + (L.lagging(j) ? kStagger * kT : 0.0);
			const long i0 = std::max(0L, long(std::ceil((c - half) * fs)));
			const long i1 = std::min(long(y.size()) - 1, long(std::floor((c + half) * fs)));
			const double w = 2 * M_PI * carrier_hz(k), phi = 2 * M_PI * ph[k] / 16.0;
			for (long i = i0; i <= i1; i++) {
				const double t = double(i) / fs;
				y[size_t(i)] += float(amplitude * lv_pulse(L, t - c) * std::cos(w * t + phi));
			}
		}
	}
	return y;
}

std::optional<std::vector<std::vector<std::complex<double>>>> demod_lv_fast(const Layout &L, int np, const float *x, long n, double fs, double t0, bool odd, double df, double *resid)
{
	static const bool slow = std::getenv("P3_LV_SLOW") != nullptr;
	if (slow) return std::nullopt;
	const double Tf = kT * fs;
	const long Tn = std::lround(Tf);
	if (std::fabs(Tf - double(Tn)) > 1e-9 || np <= 0) return std::nullopt;
	const double half = lv_half(L) * fs;
	long I[2];
	std::vector<double> g[2];
	for (int l = 0; l < 2; l++) {
		const double c = t0 + (l ? kStagger * Tf : 0.0);
		I[l] = long(std::ceil(c - half));
		const long e = long(std::floor(c + half));
		for (long i = I[l]; i <= e; i++) g[l].push_back(lv_pulse(L, (double(i) - c) / fs));
		if (I[l] < 0 || e + long(np - 1) * Tn > n - 1) return std::nullopt;
	}
	struct Col { int p, l, k; long i0; double w; };
	std::vector<Col> cols;
	for (int p = 0; p < np; p++)
		for (int j = 0; j < L.carriers(p); j++) {
			const int l = L.lagging(j) ? 1 : 0, k = L.carrier(p, j, odd);
			cols.push_back({p, l, k, I[l] + long(p) * Tn, 2 * M_PI * (carrier_hz(k) + df) / fs});
		}
	const int NC = int(cols.size()), N = 2 * NC;
	auto len = [&](const Col &c) { return long(g[c.l].size()); };
	int bw = 2;
	for (int a = 0; a < NC; a++)
		for (int b = a + 1; b < NC && cols[size_t(b)].i0 < cols[size_t(a)].i0 + len(cols[size_t(a)]); b++) bw = std::max(bw, 2 * (b - a) + 2);
	// table sums over the overlap of template la (at 0) and lb (at delta)
	std::unordered_map<long long, std::complex<double>> tab;
	auto tsum = [&](int la, int lb, long delta, double w, long long key) {
		const auto it = tab.find(key);
		if (it != tab.end()) return it->second;
		const auto &ga = g[la], &gb = g[lb];
		const long m0 = std::max(0L, delta), m1 = std::min(long(ga.size()), delta + long(gb.size()));
		std::complex<double> acc = 0, ph = std::polar(1.0, -w * double(m0));
		const std::complex<double> rot = std::polar(1.0, -w);
		for (long m = m0; m < m1; m++) { acc += ga[size_t(m)] * gb[size_t(m - delta)] * ph; ph *= rot; }
		tab.emplace(key, acc);
		return acc;
	};
	const double w120 = 2 * M_PI * 120.0 / fs;
	std::vector<double> A(size_t(N) * size_t(bw), 0.0), rhs(size_t(N), 0.0);
	for (int a = 0; a < NC; a++) {
		const Col &ca = cols[size_t(a)];
		for (int b = a; b < NC && cols[size_t(b)].i0 < ca.i0 + len(ca); b++) {
			const Col &cb = cols[size_t(b)];
			const long delta = cb.i0 - ca.i0;
			const long long dp = (delta - (I[cb.l] - I[ca.l])) / Tn;
			const long long base = ((long long)(ca.l * 2 + cb.l) * 256 + (dp + 128)) * 128;
			const int dk = ca.k - cb.k, ks = ca.k + cb.k;
			const double wd = w120 * dk, ws = ca.w + cb.w;
			const std::complex<double> S1 = std::polar(1.0, -wd * double(ca.i0)) * tsum(ca.l, cb.l, delta, wd, base + (dk + 40));
			const std::complex<double> S2 = std::polar(1.0, -ws * double(ca.i0)) * tsum(ca.l, cb.l, delta, ws, base + 80 + ks);
			const size_t r0 = size_t(2 * a) * size_t(bw), r1 = size_t(2 * a + 1) * size_t(bw);
			const int d0 = 2 * (b - a);
			A[r0 + size_t(d0)] = 0.5 * (S1.real() + S2.real());          // Re u . Re u'
			A[r0 + size_t(d0 + 1)] = 0.5 * (S2.imag() - S1.imag());      // Re u . Im u'
			if (b > a) A[r1 + size_t(d0 - 1)] = 0.5 * (S2.imag() + S1.imag());   // Im u . Re u'
			A[r1 + size_t(d0)] = 0.5 * (S1.real() - S2.real());          // Im u . Im u'
		}
		std::complex<double> z = 0, ph = std::polar(1.0, -ca.w * double(ca.i0));
		const std::complex<double> rot = std::polar(1.0, -ca.w);
		const auto &ga = g[ca.l];
		for (size_t m = 0; m < ga.size(); m++) { z += ga[m] * double(x[ca.i0 + long(m)]) * ph; ph *= rot; }
		rhs[size_t(2 * a)] = z.real(); rhs[size_t(2 * a + 1)] = z.imag();
	}
	for (int i = 0; i < N; i++) A[size_t(i) * size_t(bw)] *= 1 + 1e-9;
	ls_chol(&A[0], N, bw);
	std::vector<double> y(rhs);
	for (int i = 0; i < N; i++) {
		for (int k = std::max(0, i - bw + 1); k < i; k++) y[size_t(i)] -= A[size_t(k) * size_t(bw) + size_t(i - k)] * y[size_t(k)];
		y[size_t(i)] /= A[size_t(i) * size_t(bw)];
	}
	for (int i = N - 1; i >= 0; i--) {
		for (int j = i + 1; j < std::min(N, i + bw); j++) y[size_t(i)] -= A[size_t(i) * size_t(bw) + size_t(j - i)] * y[size_t(j)];
		y[size_t(i)] /= A[size_t(i) * size_t(bw)];
	}
	std::vector<std::vector<std::complex<double>>> z(static_cast<size_t>(np));
	for (int c = 0; c < NC; c++) z[size_t(cols[size_t(c)].p)].push_back({y[size_t(2 * c)], y[size_t(2 * c + 1)]});
	if (resid) {
		const long s = cols.front().i0, e = cols.back().i0 + len(cols.back());
		std::vector<double> m(size_t(e - s), 0.0);
		for (int c = 0; c < NC; c++) {
			const Col &cc = cols[size_t(c)];
			const std::complex<double> yc(y[size_t(2 * c)], -y[size_t(2 * c + 1)]);
			std::complex<double> ph = std::polar(1.0, -cc.w * double(cc.i0));
			const std::complex<double> rot = std::polar(1.0, -cc.w);
			const auto &gc = g[cc.l];
			for (size_t q = 0; q < gc.size(); q++) { m[size_t(cc.i0 - s) + q] += gc[q] * (yc * ph).real(); ph *= rot; }
		}
		double rr = 0, xx = 0;
		for (long i = s; i < e; i++) { const double r = x[i] - m[size_t(i - s)]; rr += r * r; xx += double(x[i]) * x[i]; }
		*resid = xx > 0 ? std::sqrt(rr / xx) : 1;
	}
	return z;
}

std::vector<std::vector<std::complex<double>>> demod_lv(int level, const float *x, long n, double fs, double t0, bool odd, double df, double *resid, int p1, bool lng)
{
	const Layout &L = lng ? layout_long(level) : layout(level);
	const int np = p1 > 0 ? std::min(p1, L.npulses()) : L.npulses();
	if (auto f = demod_lv_fast(L, np, x, n, fs, t0, odd, df, resid)) return std::move(*f);
	struct Col { long i0; std::vector<double> v; int p, j; };
	std::vector<Col> cols;
	const double half = lv_half(L);
	for (int p = 0; p < np; p++)
		for (int j = 0; j < L.carriers(p); j++) {
			const double c = t0 + (p * kT + (L.lagging(j) ? kStagger * kT : 0.0)) * fs;
			const long i0 = std::max(0L, long(std::ceil(c - half * fs))), i1 = std::min(n - 1, long(std::floor(c + half * fs)));
			const double w = 2 * M_PI * (carrier_hz(L.carrier(p, j, odd)) + df) / fs;
			Col a{i0, {}, p, j}, b{i0, {}, p, j};
			for (long i = i0; i <= i1; i++) {
				const double g = lv_pulse(L, (double(i) - c) / fs);
				a.v.push_back(g * std::cos(w * double(i)));
				b.v.push_back(-g * std::sin(w * double(i)));
			}
			cols.push_back(std::move(a)); cols.push_back(std::move(b));
		}
	const int N = int(cols.size());
	// bandwidth: columns overlapping in time (sorted by i0 already per pulse)
	int bw = 1;
	for (int i = 0; i < N; i++)
		for (int j = i + 1; j < N && cols[size_t(j)].i0 < cols[size_t(i)].i0 + long(cols[size_t(i)].v.size()); j++) bw = std::max(bw, j - i + 1);
	auto dot = [&](const Col &a, const Col &b) {
		const long s = std::max(a.i0, b.i0), e = std::min(a.i0 + long(a.v.size()), b.i0 + long(b.v.size()));
		double acc = 0;
		for (long i = s; i < e; i++) acc += a.v[size_t(i - a.i0)] * b.v[size_t(i - b.i0)];
		return acc;
	};
	std::vector<double> A(size_t(N) * size_t(bw), 0.0), rhs(size_t(N), 0.0);
	for (int i = 0; i < N; i++) {
		for (int j = i; j < std::min(N, i + bw); j++) A[size_t(i) * size_t(bw) + size_t(j - i)] = dot(cols[size_t(i)], cols[size_t(j)]);
		double acc = 0;
		for (size_t m = 0; m < cols[size_t(i)].v.size(); m++) acc += cols[size_t(i)].v[m] * x[cols[size_t(i)].i0 + long(m)];
		rhs[size_t(i)] = acc;
		A[size_t(i) * size_t(bw)] *= 1 + 1e-9;
	}
	ls_chol(&A[0], N, bw);
	std::vector<double> y(rhs);
	for (int i = 0; i < N; i++) {
		for (int k = std::max(0, i - bw + 1); k < i; k++) y[size_t(i)] -= A[size_t(k) * size_t(bw) + size_t(i - k)] * y[size_t(k)];
		y[size_t(i)] /= A[size_t(i) * size_t(bw)];
	}
	for (int i = N - 1; i >= 0; i--) {
		for (int j = i + 1; j < std::min(N, i + bw); j++) y[size_t(i)] -= A[size_t(i) * size_t(bw) + size_t(j - i)] * y[size_t(j)];
		y[size_t(i)] /= A[size_t(i) * size_t(bw)];
	}
	std::vector<std::vector<std::complex<double>>> z(static_cast<size_t>(np));
	for (int c = 0; c < N / 2; c++) z[size_t(cols[size_t(2 * c)].p)].push_back({y[size_t(2 * c)], y[size_t(2 * c + 1)]});
	if (resid) {
		const long s = cols.front().i0, e = cols.back().i0 + long(cols.back().v.size());
		std::vector<double> m(size_t(e - s), 0.0);
		for (int i = 0; i < N; i++) for (size_t q = 0; q < cols[size_t(i)].v.size(); q++) m[size_t(cols[size_t(i)].i0 - s) + q] += y[size_t(i)] * cols[size_t(i)].v[q];
		double rr = 0, xx = 0;
		for (long i = s; i < e; i++) { const double r = x[i] - m[size_t(i - s)]; rr += r * r; xx += double(x[i]) * x[i]; }
		*resid = xx > 0 ? std::sqrt(rr / xx) : 1;
	}
	return z;
}

double lv_step(const std::vector<std::vector<std::complex<double>>> &z, const Layout &L, int p, int j, bool odd)
{
	// the same carrier in the previous pulse (it may sit in another slot)
	const int k = L.carrier(p, j, odd);
	int jp = -1;
	for (int q = 0; q < L.carriers(p - 1); q++) if (L.carrier(p - 1, q, odd) == k) jp = q;
	if (jp < 0) return -1;
	double s = -std::arg(z[size_t(p)][size_t(j)] / z[size_t(p - 1)][size_t(jp)]) / (2 * M_PI) * 16;
	return s < 0 ? s + 16 : s;
}

FlatPlan demod_flat_plan(int level, long n, double fs, double t0, double df, int p1, bool lng)
{
	FlatPlan P; P.level = level; P.p1 = p1; P.lng = lng; P.fs = fs; P.t0 = t0; P.df = df;

	const Layout &L = lng ? layout_long(level) : layout(level);
	const int Tn = int(std::lround(kT * fs));
	const int np = p1 > 0 ? p1 : L.npulses();
	const long base = long(std::ceil(t0 - kPulseHalfS * fs));
	const int Lg = int(long(std::floor(t0 + kPulseHalfS * fs)) - base + 1);
	if (base < 0 || base + long(np - 1) * Tn + Lg > n) return P;
	std::vector<double> g(static_cast<size_t>(Lg));
	for (int m = 0; m < Lg; m++) g[size_t(m)] = pulse((double(base + m) - t0) / fs);
	constexpr int NK = 18;
	std::complex<double> Um[3][2 * NK - 1], Up[3][2 * NK - 1];
	for (int dd = 0; dd < 3; dd++)
		for (int D = 0; D < 2 * NK - 1; D++) {
			const double wm = 2 * M_PI * 120.0 * (D - (NK - 1)) / fs, wp = 2 * M_PI * (carrier_hz(0) * 2 + 120.0 * D + 2 * df) / fs;
			std::complex<double> am = 0, ap = 0, pm = std::polar(1.0, wm * dd * Tn), pp = std::polar(1.0, wp * dd * Tn);
			const std::complex<double> rm = std::polar(1.0, wm), rp = std::polar(1.0, wp);
			for (int m = dd * Tn; m < Lg; m++) { const double gg = g[size_t(m)] * g[size_t(m - dd * Tn)]; am += gg * pm; ap += gg * pp; pm *= rm; pp *= rp; }
			Um[dd][D] = am; Up[dd][D] = ap;
		}
	struct C { int r, k; };
	std::vector<C> cols;
	std::vector<int> col0;
	for (int r = 0; r < np; r++) {
		col0.push_back(int(2 * cols.size()));
		int ks[18], nk = L.carriers(r);
		for (int j = 0; j < nk; j++) ks[j] = L.carrier(r, j, false);
		std::sort(ks, ks + nk);
		for (int j = 0; j < nk; j++) cols.push_back({r, ks[j]});
	}
	const int NC = int(cols.size()), N = 2 * NC;
	int bw = 0;
	for (size_t q = 0; q < col0.size(); q++) bw = std::max(bw, (q + 3 < col0.size() ? col0[q + 3] : N) - col0[q]);
	std::vector<double> A(size_t(N) * size_t(bw), 0.0);
	std::vector<std::complex<double>> Pm(size_t((2 * NK - 1) * np)), Pp(size_t((2 * NK - 1) * np));
	for (int D = 0; D < 2 * NK - 1; D++) {
		const double wm = 2 * M_PI * 120.0 * (D - (NK - 1)) / fs, wp = 2 * M_PI * (carrier_hz(0) * 2 + 120.0 * D + 2 * df) / fs;
		std::complex<double> em = std::polar(1.0, wm * double(base)), ep = std::polar(1.0, wp * double(base));
		const std::complex<double> sm = std::polar(1.0, wm * Tn), sp = std::polar(1.0, wp * Tn);
		for (int r = 0; r < np; r++) { Pm[size_t(D * np + r)] = em; Pp[size_t(D * np + r)] = ep; em *= sm; ep *= sp; }
	}
	for (int c1 = 0; c1 < NC; c1++) {
		const auto &a = cols[size_t(c1)];
		for (int c2 = c1; c2 < NC; c2++) {
			const auto &b = cols[size_t(c2)];
			const int dd = b.r - a.r;
			if (dd > 2) break;
			const int Dm = a.k - b.k + NK - 1, Dp = a.k + b.k;
			const std::complex<double> Sm = Pm[size_t(Dm * np + a.r)] * Um[dd][Dm], Sp = Pp[size_t(Dp * np + a.r)] * Up[dd][Dp];
			const double aa = 0.5 * (Sm.real() + Sp.real()), bb = 0.5 * (Sm.real() - Sp.real());
			const double ab = -0.5 * Sp.imag() + 0.5 * Sm.imag(), ba = -0.5 * Sp.imag() - 0.5 * Sm.imag();
			const int i = 2 * c1, j = 2 * c2;
			auto put = [&](int r, int q, double v) { if (q >= r && q - r < bw) A[size_t(r) * size_t(bw) + size_t(q - r)] = v; };
			put(i, j, aa); put(i + 1, j + 1, bb); put(i, j + 1, ab); put(i + 1, j, ba);
			if (c1 == c2) put(i, i + 1, ab);
		}
	}
	for (int i = 0; i < N; i++) A[size_t(i) * size_t(bw)] *= 1 + 1e-9;
	ls_chol(&A[0], N, bw);
	P.ok = true; P.Tn = Tn; P.np = np; P.Lg = Lg; P.NC = NC; P.N = N; P.bw = bw; P.base = base;
	P.g = std::move(g); P.A = std::move(A);
	for (const auto &c : cols) { P.col_r.push_back(c.r); P.col_k.push_back(c.k); }
	return P;
}

void demod_flat_advance(const FlatPlan &P, const float *x, long n_avail, FlatProgress &g)
{
	if (!P.ok || ls_slow()) return;
	const int NC = P.NC, N = P.N, bw = P.bw, Tn = P.Tn, Lg = P.Lg;
	const long base = P.base;
	if (g.y.empty()) g.y.assign(size_t(N), 0.0);
	int c = g.cols;
	while (c < NC && base + long(P.col_r[size_t(c)]) * Tn + Lg <= n_avail) c++;
	if (c > g.cols) {
		const double fs = P.fs, df = P.df;
		auto w_of = [&](int k) { return 2 * M_PI * (carrier_hz(k) + df) / fs; };
		ls_rhs_rows(P.col_r.data() + g.cols, P.col_k.data() + g.cols, c - g.cols, base, Tn, Lg, P.g.data(), x, w_of, g.y.data() + 2 * size_t(g.cols));
		g.cols = c;
	}
	const int k1 = g.cols == NC ? N : std::min(N, std::max(g.k, 2 * g.cols - bw + 1));
	ls_forward(P.A.data(), N, bw, g.y.data(), g.k, k1);
	g.k = k1;
}

std::vector<std::array<std::complex<double>, 18>> demod_flat_run(const FlatPlan &P, const float *x, long n, double *resid, FlatProgress *prog)
{
	const int np = P.p1 > 0 ? P.p1 : (P.lng ? layout_long(P.level) : layout(P.level)).npulses();
	std::vector<std::array<std::complex<double>, 18>> out(static_cast<size_t>(np));
	if (!P.ok || P.base + long(P.np - 1) * P.Tn + P.Lg > n) { if (resid) *resid = 1; return out; }
	if (prog && !resid && !ls_slow()) {               // finish a run made in pieces (P3_LONG_PRE_OFF=1 in the station: none)
		demod_flat_advance(P, x, n, *prog);
		std::vector<double> y(prog->y);
		const int N = P.N, bw = P.bw;
		const double *A = P.A.data();
		ls_backward(A, N, bw, y.data());
		for (int c = 0; c < P.NC; c++) out[size_t(P.col_r[size_t(c)])][size_t(P.col_k[size_t(c)])] = {y[size_t(2 * c)], y[size_t(2 * c + 1)]};
		return out;
	}
	const double fs = P.fs, df = P.df;
	const int Tn = P.Tn, Lg = P.Lg, NC = P.NC, N = P.N, bw = P.bw;
	const long base = P.base;
	const std::vector<double> &g = P.g, &A = P.A;
	struct C { int r, k; };
	std::vector<C> cols(static_cast<size_t>(NC));
	for (int c = 0; c < NC; c++) cols[size_t(c)] = {P.col_r[size_t(c)], P.col_k[size_t(c)]};
	std::vector<double> rhs(size_t(N), 0.0);
	auto w_of = [&](int k) { return 2 * M_PI * (carrier_hz(k) + df) / fs; };
	static const bool slow = std::getenv("P3_LS_SLOW") != nullptr;   // test: the forms before fix 216b (p3_modem.hpp ls_rhs_rows)
	if (!slow) ls_rhs_rows(P.col_r.data(), P.col_k.data(), NC, base, Tn, Lg, g.data(), x, w_of, rhs.data());
	else for (int c = 0; c < NC; c++) {
		const long s0 = base + long(cols[size_t(c)].r) * Tn;
		const double w = w_of(cols[size_t(c)].k);
		std::complex<double> ph = std::polar(1.0, -w * double(s0)), rot = std::polar(1.0, -w), z = 0;
		for (int m = 0; m < Lg; m++) { z += g[size_t(m)] * double(x[s0 + m]) * ph; ph *= rot; }
		rhs[size_t(2 * c)] = z.real(); rhs[size_t(2 * c + 1)] = z.imag();
	}
	std::vector<double> y(rhs);
	if (!slow) ls_forward(A.data(), N, bw, y.data());
	else for (int i = 0; i < N; i++) { for (int k = std::max(0, i - bw + 1); k < i; k++) y[size_t(i)] -= A[size_t(k) * size_t(bw) + size_t(i - k)] * y[size_t(k)]; y[size_t(i)] /= A[size_t(i) * size_t(bw)]; }
	ls_backward(A.data(), N, bw, y.data());
	for (int c = 0; c < NC; c++) out[size_t(cols[size_t(c)].r)][size_t(cols[size_t(c)].k)] = {y[size_t(2 * c)], y[size_t(2 * c + 1)]};
	if (resid) {
		const long e = base + long(np - 1) * Tn + Lg;
		std::vector<double> mdl(size_t(e - base), 0.0);
		if (!slow) ls_model_rows(P.col_r.data(), P.col_k.data(), NC, base, Tn, Lg, g.data(), w_of, y.data(), mdl.data());
		else for (int c = 0; c < NC; c++) {
			const long s0 = base + long(cols[size_t(c)].r) * Tn;
			const double w = w_of(cols[size_t(c)].k);
			const std::complex<double> zc(y[size_t(2 * c)], y[size_t(2 * c + 1)]);
			std::complex<double> ph = std::polar(1.0, w * double(s0)), rot = std::polar(1.0, w);
			for (int m = 0; m < Lg; m++) { mdl[size_t(s0 - base + m)] += g[size_t(m)] * (zc * ph).real(); ph *= rot; }
		}
		double rr = 0, xx = 0;
		for (long i = base; i < e; i++) { const double r = x[i] - mdl[size_t(i - base)]; rr += r * r; xx += double(x[i]) * x[i]; }
		*resid = xx > 0 ? std::sqrt(rr / xx) : 1;
	}
	return out;
}

}  // namespace fmodem::p3
