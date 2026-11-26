// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// The functions of p2_codec.hpp (out of the header, so that they build in parallel).
#include "p2_codec.hpp"

namespace fmodem::p2 {

int coded_index(int level, int g, int p, int car, int b, int ppr)
{
	switch (level) {
	case 1:
		if (ppr == 40) {                               // long: P2 level-1 long and P3 SL1 long share
			                                           // one coding
			const int c = p3::lng::level(1).coded[2 * (g * 40 + p) + car];
			return c >= 0 ? c : 2 * kInfoBits(1, 40) + 15;   // the one unresolved bit: a tail bit (0)
		}
		return ppr == 7 ? kSeq1bk[2 * p + car] + 2 * g : kSeq1[(2 * p + car) % 18] + 2 * g;
	case 2: return 32 * p + 8 * (3 - (2 * car + b)) + (g ^ 1);
	case 3: return 32 * (p + ppr * (g & 1)) + kOff3[g >> 1][car][b];
	default: return 28 * (p + ppr * (g & 3)) + kOff4[g >> 2][car][b];
	}
}

std::vector<uint8_t> encode_data(int level, const uint8_t *info, int ppr)
{
	const int nbits = kInfoBits(level, ppr), k = kBitsPerSym[level];
	std::vector<uint8_t> coded(size_t(2 * (nbits + 8)));
	conv_encode(info, nbits, coded.data());
	const unsigned comp = (level == 2) ? 3u : (level == 1 ? 1u : 0u);   // label complement mask
	const int base = (level <= 2) ? 2 : 0, step = 16 >> k;
	std::vector<uint8_t> w;
	for (int g = 0; g < 8; g++)
		for (int p = 0; p < ppr; p++) {
			int nib[2];
			for (int car = 0; car < 2; car++) {
				unsigned label = 0;
				for (int b = 0; b < k; b++) label |= unsigned(coded[size_t(coded_index(level, g, p, car, b, ppr))]) << b;
				label ^= comp;
				nib[car] = (base + step * int(gray_inv(label))) & 15;
			}
			w.push_back(uint8_t(nib[0] << 4 | nib[1]));
		}
	return w;
}

const std::vector<double> & pulse_table(float fs)
{
	// built on first use; decoders call this from several threads
	static std::mutex m;
	std::lock_guard<std::mutex> lk(m);
	static float cached = 0;
	static std::vector<double> t;
	if (cached != fs) {
		const double T = 0.010 * fs;
		const int n = int(std::lround(4 * T)) + 1;
		t.assign(size_t(n), 0.0);
		double area = 0;
		for (int i = 0; i < n; i++) { t[size_t(i)] = rrc1((i - 2 * T) / T); area += t[size_t(i)]; }
		for (auto &v : t) v *= T / area;
		cached = fs;
	}
	return t;
}

ModPlan modulate_plan(const std::vector<uint8_t> &words, float fs, float lo, float hi, bool swap,
                             float amplitude, float txd_s)
{
	ModPlan p;
	p.fs = fs;
	const double T = 0.010 * fs;
	p.T = T;
	p.stagger = (std::getenv("P2X_STAG48") ? 0.5 : 0.5078125) * T;   // 48.75 samples at 9600 Hz (measured; not exactly T/2)
	p.start0 = txd_s * fs + preroll(fs);
	p.len = size_t(p.start0 + words.size() * T + p.stagger + 3 * T) + 1;
	const auto &P = pulse_table(fs);
	p.norm = P[size_t(std::lround(2 * T))] / rrc1(0.0);   // table scaling, applied to exact rrc1 values
	p.A = 0.4 * amplitude;                            // steady carrier amplitude (the reference unit level at 4400-4700)
	for (int role = 0; role < 2; role++) {
		p.f[role] = (role == 0) ? (swap ? hi : lo) : (swap ? lo : hi);
		double ph = 0;
		p.ph[role].resize(words.size());
		for (size_t k = 0; k < words.size(); k++) {
			const int nib = role == 0 ? (words[k] >> 4) & 15 : words[k] & 15;
			ph -= 2 * M_PI * nib / 16.0;
			p.ph[role][k] = ph;
		}
	}
	// Onset as the reference unit's audio shows it: no precursor tails; the output opens
	// ~6.7 ms before the first pulse's centre with a short ramp (measured:
	// its signal jumps up between -61 and -13 samples of that centre).
	const double c0 = p.start0 + 0.8 * T;
	p.g0 = c0 - 0.0067 * fs; p.gr = 0.0033 * fs;
	p.gate = !std::getenv("P2X_NOGATE");
	return p;
}

void modulate_range(const ModPlan &p, float *y, size_t a, size_t b)
{
	const double T = p.T, start0 = p.start0, stagger = p.stagger, A = p.A, norm = p.norm;
	const float fs = p.fs;
	const size_t len = p.len;
	b = std::min(b, len);
	if (a >= b) return;
	for (int role = 0; role < 2; role++) {
		const double f = p.f[role];
		const std::vector<double> &phs = p.ph[role];
		for (size_t k = 0; k < phs.size(); k++) {
			const double ph = phs[k];
			const double centre = start0 + (role == 1 ? stagger : 0) + double(k) * T + 0.8 * T;
			const long n0 = long(std::ceil(centre - 2 * T)), n1 = long(std::floor(centre + 2 * T));
			if (n0 >= long(b)) break;                  // later pulses start later still
			if (n1 < long(a)) continue;
			for (long n = std::max(n0, long(a)); n <= n1 && n < long(b); n++) {
				if (n < 0 || size_t(n) >= len) continue;
				y[size_t(n)] += float(A * norm * rrc1((double(n) - centre) / T) * std::cos(2 * M_PI * f * double(n) / fs + ph));
			}
		}
	}
	const double g0 = p.g0, gr = p.gr;
	for (size_t n = a; p.gate && n < b && double(n) < g0 + gr; n++)
		y[n] *= double(n) < g0 ? 0.0f : float(0.5 - 0.5 * std::cos(M_PI * (double(n) - g0) / gr));
}

std::vector<std::complex<double>> ls_pulses_core(const std::vector<float> &x, double fs, double cA, double cB,
                                                        double fA, double fB, int n, const std::vector<double> *ptab,
                                                        LsPre *fill, long avail)
{
	const double T = 0.010 * fs;
	const auto &Ptab = ptab ? *ptab : pulse_table(float(fs));   // same layout: centre 2T, 4T+1 samples
	const int half = int(std::lround(2 * T));
	const int U = 4 * n;                                // real unknowns: (re, im) x 2 carriers x n
	// The normal matrix is symmetric and banded: pulses overlap +-2T on each
	// carrier, B staggered. Only its lower band is kept (row r, column c,
	// r - c <= bw): the Cholesky solve reads nothing else. Entries further
	// out are dropped (a dense U x U matrix was 14 MB for a long packet).
	const int bw = 4 * 7, W = bw + 1;
	std::vector<double> N(size_t(U) * size_t(W), 0.0), rhs(size_t(U), 0.0);
	auto at = [&](int r, int c) -> double & { return N[size_t(r) * size_t(W) + size_t(r - c)]; };
	// Pulse k on a carrier: centre at c + k*T + 0.8T, extent +-2T.
	const long lo = long(std::floor(std::min(cA, cB) + 0.8 * T - 2 * T)) - 1;
	const long hi = long(std::ceil(std::max(cA, cB) + n * T + 0.8 * T + 2 * T)) + 1;
	// The normal equations are sums over samples: long bursts are split into
	// sample ranges summed on several cores (par.hpp), then added up. Short
	// ones (or one core) are summed in one pass, in sample order.
	const long t0 = std::max(0L, lo), t1 = std::min(hi, long(x.size()));
	// Short packets too (2026-10-22, the Pi 4: 84 pulses summed on one core took 10 ms of a
	// 31 ms reply window); P2_LS_MIN_SPLIT sets the smallest burst split (default 40 pulses).
	static const int min_split = std::getenv("P2_LS_MIN_SPLIT") ? std::atoi(std::getenv("P2_LS_MIN_SPLIT")) : 40;
	const int parts = n >= min_split ? std::min(par::ways(), 8) : 1;
	std::vector<std::vector<double>> Np(size_t(parts - 1)), rp(size_t(parts - 1));
	// the sums of earlier ranges, made while the packet arrived (LsPre): only for exactly this fit
	LsPre *pre = !fill && !ptab && ls_pre_tls && ls_pre_tls->matches(x.size(), fs, cA, cB, fA, fB, n, parts) ? ls_pre_tls : nullptr;
	// range `part` summed from sample `from` (its start a, or a block boundary of it where an earlier
	// call stopped: Nq, rq hold the sums so far) up to `to`
	auto accumulate_span = [&](int part, std::vector<double> &Nq, std::vector<double> &rq, long from, long to) {
		auto atq = [&](int r, int c) -> double & { return Nq[size_t(r) * size_t(W) + size_t(r - c)]; };
		const long a = t0 + (t1 - t0) * part / parts;
		const long b = to;
		std::vector<std::pair<int, double>> act;
		// Speed (no change in the result beyond rounding): the carrier phasor
		// is rotated sample by sample (exact at the start of each block of
		// 256), and a pulse's table index is t minus a per-pulse constant
		// (round(t - x) = t - round(x) for integer t).
		std::complex<double> rot[2], ph[2];
		for (int car = 0; car < 2; car++) rot[car] = std::polar(1.0, 2 * M_PI * (car ? fB : fA) / fs);
		auto pulse_off = [&](int car, long k) { return long(std::lround((car ? cB : cA) + 0.8 * T + double(k) * T)) - half; };
	// The fast path (2026-10-22, for the Pi 4's Cortex-A72: the per-pair branch and the
	// vector of pairs cost more than the arithmetic): the active unknowns of a sample in
	// fixed arrays, in ascending order (a merge of the two carriers' lists). The band still
	// bounds the pairs: with the carriers a pulse or more apart (the decoder's slipped
	// timings) they span more than bw, and those pairs are dropped as before; in ascending
	// order they are the first of each row (a first version added them past the band: noisy
	// links failed). The same sums in the same order per entry; P2_LS_SLOW: the old loop (test).
	static const bool slow = std::getenv("P2_LS_SLOW") != nullptr;
	if (!slow) {
		for (long t = from; t < b; t++) {
			if (t == from || ((t - a) & 255) == 0)
				for (int car = 0; car < 2; car++) ph[car] = std::polar(1.0, 2 * M_PI * (car ? fB : fA) * double(t) / fs);
			int ui[2][12], m[2] = {0, 0};
			double uv[2][12];
			for (int car = 0; car < 2; car++) {
				const double c0 = (car ? cB : cA) + 0.8 * T;
				const long k1 = long(std::floor((double(t) - c0 + 2 * T) / T)), k0 = k1 - 4;
				const double cs = ph[car].real(), sn = ph[car].imag();
				for (long k = std::max(0L, k0); k <= k1 && k < n; k++) {
					const long i = t - pulse_off(car, k);
					if (i < 0 || i >= long(Ptab.size())) continue;
					const double env = Ptab[size_t(i)];
					const int u = 4 * int(k) + 2 * car;
					ui[car][m[car]] = u; uv[car][m[car]++] = env * cs;
					ui[car][m[car]] = u + 1; uv[car][m[car]++] = -env * sn;
				}
				ph[car] *= rot[car];
			}
			int idx[24]; double val[24]; int na = 0;
			for (int i0 = 0, i1 = 0; i0 < m[0] || i1 < m[1];) {   // merge: ascending unknown index
				if (i1 >= m[1] || (i0 < m[0] && ui[0][i0] < ui[1][i1])) { idx[na] = ui[0][i0]; val[na++] = uv[0][i0++]; }
				else { idx[na] = ui[1][i1]; val[na++] = uv[1][i1++]; }
			}
			const double xt = double(x[size_t(t)]);
			for (int p = 0, q0 = 0; p < na; p++) {
				const int r = idx[p];
				const double vr = val[p];
				rq[size_t(r)] += vr * xt;
				double *row = &Nq[size_t(r) * size_t(W)];
				while (r - idx[q0] > bw) q0++;            // pairs outside the band (idx ascending)
				for (int q = q0; q <= p; q++) row[r - idx[q]] += vr * val[q];
			}
		}
		return;
	}
	for (long t = from; t < b; t++) {
		act.clear();
		if (t == from || ((t - a) & 255) == 0)
			for (int car = 0; car < 2; car++) ph[car] = std::polar(1.0, 2 * M_PI * (car ? fB : fA) * double(t) / fs);
		for (int car = 0; car < 2; car++) {
			const double c0 = (car ? cB : cA) + 0.8 * T;
			const long k1 = long(std::floor((double(t) - c0 + 2 * T) / T)), k0 = k1 - 4;
			const double cs = ph[car].real(), sn = ph[car].imag();
			for (long k = k0; k <= k1; k++) {
				if (k < 0 || k >= n) continue;
				const long i = t - pulse_off(car, k);
				if (i < 0 || i >= long(Ptab.size())) continue;
				const double env = Ptab[size_t(i)];
				const int u = 4 * int(k) + 2 * car;
				act.emplace_back(u, env * cs);
				act.emplace_back(u + 1, -env * sn);
			}
			ph[car] *= rot[car];
		}
		for (auto &u : act) {
			rq[size_t(u.first)] += u.second * double(x[size_t(t)]);
			for (auto &v : act)
				if (v.first <= u.first && u.first - v.first <= bw) atq(u.first, v.first) += u.second * v.second;
		}
	}
	};
	auto range_of = [&](int part, long &a, long &b) { a = t0 + (t1 - t0) * part / parts; b = t0 + (t1 - t0) * (part + 1) / parts; };
	// fill: the span of each range to sum now (from -1: the whole range, as a fit); the arrays it adds to
	// are loaded before. NOTE: on ARM, GCC fuses multiply-adds, and which ones it fuses here depends on
	// the code around this loop: a test hook's static variable in this function changed the fit's last
	// bits on the Pi 4. Check the decode results bit for bit on an ARM computer after any change to
	// this function.
	std::vector<long> span_from(size_t(parts), -1), span_to(size_t(parts), -1);
	auto accumulate = [&](int part) {
		std::vector<double> &Nq = part == 0 ? N : Np[size_t(part - 1)];
		std::vector<double> &rq = part == 0 ? rhs : rp[size_t(part - 1)];
		long a, b;
		range_of(part, a, b);
		long from = a, to = b;
		if (span_from[size_t(part)] >= 0) { from = span_from[size_t(part)]; to = span_to[size_t(part)]; }
		else {
			const long got = pre ? pre->take(part, Nq, rq) : -1;
			if (got >= 0) { from = got; pre->taken += got - a; }
			else if (part) { Nq.assign(N.size(), 0.0); rq.assign(rhs.size(), 0.0); }
		}
		accumulate_span(part, Nq, rq, from, to);
	};
	if (fill) {                                        // the samples that are in, summed and kept
		{
			std::lock_guard<std::mutex> lk(fill->m);
			if (fill->parts == 0) {
				fill->xsize = x.size(); fill->fs = fs; fill->cA = cA; fill->cB = cB; fill->fA = fA; fill->fB = fB; fill->n = n; fill->parts = parts;
				fill->N.assign(size_t(parts), {}); fill->r.assign(size_t(parts), {}); fill->upto.assign(size_t(parts), -1);
			} else if (fill->parts != parts || !fill->same(x.size(), fs, cA, cB, fA, fB, n)) return {};
		}
		for (int part = 0; part < parts; part++) {
			std::vector<double> &Nq = part == 0 ? N : Np[size_t(part - 1)];
			std::vector<double> &rq = part == 0 ? rhs : rp[size_t(part - 1)];
			long a, b;
			range_of(part, a, b);
			long from;
			{
				std::lock_guard<std::mutex> lk(fill->m);
				from = fill->upto[size_t(part)];
				if (from >= b) continue;                   // all summed
				if (from >= 0) { Nq = fill->N[size_t(part)]; rq = fill->r[size_t(part)]; }
			}
			if (from < 0) { from = a; Nq.assign(N.size(), 0.0); rq.assign(rhs.size(), 0.0); }
			const long to = avail >= b ? b : a + ((avail - a) / 256) * 256;   // a block boundary short of the range's end
			if (to <= from) break;
			span_from[size_t(part)] = from; span_to[size_t(part)] = to;
			accumulate(part);
			std::lock_guard<std::mutex> lk(fill->m);
			fill->N[size_t(part)] = std::move(Nq); fill->r[size_t(part)] = std::move(rq); fill->upto[size_t(part)] = to;
			Nq.assign(size_t(U) * size_t(W), 0.0); rq.assign(size_t(U), 0.0);   // (moved out)
			if (to < b) break;
		}
		return {};
	}
	par::parallel_for(parts, accumulate);
	for (int q = 0; q + 1 < parts; q++) {
		for (size_t i = 0; i < N.size(); i++) N[i] += Np[size_t(q)][i];
		for (size_t i = 0; i < rhs.size(); i++) rhs[i] += rp[size_t(q)][i];
	}
	for (int i = 0; i < U; i++) at(i, i) += 1e-6;
	// Cholesky solve (banded).
	for (int j = 0; j < U; j++) {
		double d = at(j, j);
		for (int k = std::max(0, j - bw); k < j; k++) d -= at(j, k) * at(j, k);
		d = std::sqrt(std::max(d, 1e-12));
		at(j, j) = d;
		for (int i = j + 1; i < std::min(U, j + bw + 1); i++) {
			double v = at(i, j);
			for (int k = std::max(0, i - bw); k < j; k++) v -= at(i, k) * at(j, k);
			at(i, j) = v / d;
		}
	}
	for (int i = 0; i < U; i++) {
		double v = rhs[size_t(i)];
		for (int k = std::max(0, i - bw); k < i; k++) v -= at(i, k) * rhs[size_t(k)];
		rhs[size_t(i)] = v / at(i, i);
	}
	for (int i = U - 1; i >= 0; i--) {
		double v = rhs[size_t(i)];
		for (int k = i + 1; k < std::min(U, i + bw + 1); k++) v -= at(k, i) * rhs[size_t(k)];
		rhs[size_t(i)] = v / at(i, i);
	}
	std::vector<std::complex<double>> z(size_t(2 * n));
	for (int k = 0; k < n; k++)
		for (int car = 0; car < 2; car++)
			z[size_t(car * n + k)] = {rhs[size_t(4 * k + 2 * car)], rhs[size_t(4 * k + 2 * car + 1)]};
	return z;
}

int viterbi_ref(const uint8_t *coded, const uint8_t *erased, int nbits, std::vector<uint8_t> &out)
{
	constexpr int NS = 256;
	const int steps = nbits + 8;
	std::vector<int> pm(NS, 1 << 28), npm(NS);
	std::vector<uint8_t> dec(size_t(steps) * NS);
	std::vector<uint8_t> prev(size_t(steps) * NS);
	pm[0] = 0;
	for (int i = 0; i < steps; i++) {
		std::fill(npm.begin(), npm.end(), 1 << 30);
		const int lim = i < nbits ? 2 : 1;               // tail bits are zero
		for (int s = 0; s < NS; s++) {
			if (pm[size_t(s)] >= 1 << 28) continue;
			for (int u = 0; u < lim; u++) {
				const unsigned reg = ((unsigned(s) << 1) | unsigned(u)) & 0x1ff;
				const int ns = int(reg & 0xff);
				int m = pm[size_t(s)];
				if (!erased[2 * i]) m += parity(reg & 0x171) != coded[2 * i];
				if (!erased[2 * i + 1]) m += parity(reg & 0x1EB) != coded[2 * i + 1];
				if (m < npm[size_t(ns)]) { npm[size_t(ns)] = m; dec[size_t(i) * NS + size_t(ns)] = uint8_t(u); prev[size_t(i) * NS + size_t(ns)] = uint8_t(s); }
			}
		}
		pm.swap(npm);
	}
	out.assign(size_t((nbits + 7) / 8), 0);
	int s = 0;
	const int metric = pm[0];
	for (int i = steps - 1; i >= 0; i--) {
		const int u = dec[size_t(i) * NS + size_t(s)];
		if (i < nbits && u) out[size_t(i / 8)] |= uint8_t(1 << (i % 8));
		s = prev[size_t(i) * NS + size_t(s)];
	}
	return metric;
}

int viterbi_soft_ref(const float *llr, int nbits, std::vector<uint8_t> &out)
{
	constexpr int NS = 256;
	const int steps = nbits + 8;
	constexpr float kNone = -1e30f;
	std::vector<float> pm(NS, kNone), npm(NS);
	std::vector<uint8_t> dec(size_t(steps) * NS);
	std::vector<uint8_t> prev(size_t(steps) * NS);
	pm[0] = 0;
	for (int i = 0; i < steps; i++) {
		std::fill(npm.begin(), npm.end(), kNone);
		const int lim = i < nbits ? 2 : 1;               // tail bits are zero
		const float a = llr[2 * i], b = llr[2 * i + 1];
		for (int st = 0; st < NS; st++) {
			if (pm[size_t(st)] <= kNone) continue;
			for (int u = 0; u < lim; u++) {
				const unsigned reg = ((unsigned(st) << 1) | unsigned(u)) & 0x1ff;
				const int ns = int(reg & 0xff);
				const float m = pm[size_t(st)] + (parity(reg & 0x171) ? -a : a) + (parity(reg & 0x1EB) ? -b : b);
				if (m > npm[size_t(ns)]) { npm[size_t(ns)] = m; dec[size_t(i) * NS + size_t(ns)] = uint8_t(u); prev[size_t(i) * NS + size_t(ns)] = uint8_t(st); }
			}
		}
		pm.swap(npm);
	}
	out.assign(size_t((nbits + 7) / 8), 0);
	int st = 0, bad = 0;
	std::vector<int> path(static_cast<size_t>(steps));
	for (int i = steps - 1; i >= 0; i--) {
		const int u = dec[size_t(i) * NS + size_t(st)];
		if (i < nbits && u) out[size_t(i / 8)] |= uint8_t(1 << (i % 8));
		const unsigned reg = ((unsigned(prev[size_t(i) * NS + size_t(st)]) << 1) | unsigned(u)) & 0x1ff;
		if (llr[2 * i] != 0 && (llr[2 * i] < 0) != bool(parity(reg & 0x171))) bad++;
		if (llr[2 * i + 1] != 0 && (llr[2 * i + 1] < 0) != bool(parity(reg & 0x1EB))) bad++;
		st = prev[size_t(i) * NS + size_t(st)];
	}
	return bad;
}

int viterbi(const uint8_t *coded, const uint8_t *erased, int nbits, std::vector<uint8_t> &out)
{
	// Unreachable states hold kU and grow by at most 2 a step (never near a reachable metric, which
	// stays under 2 * steps), so no test is needed: a reachable predecessor always wins against an
	// unreachable one, and the choice made between two unreachable ones is never traced back.
	constexpr int NS = 256, kU = 1 << 28;
	const int steps = nbits + 8;
	const uint8_t *T = conv_out_table();
	int pa[NS], pb[NS], *pm = pa, *npm = pb;
	for (int &v : pa) v = kU;
	pm[0] = 0;
	std::vector<uint64_t> ch(size_t(steps) * 4, 0);   // per step: bit ns set = from (ns>>1)|0x80
	for (int i = 0; i < steps; i++) {
		int bm[4];
		for (int o = 0; o < 4; o++) bm[o] = (!erased[2 * i] && (o & 1) != coded[2 * i]) + (!erased[2 * i + 1] && (o >> 1) != coded[2 * i + 1]);
		// butterflies: s0 = j and s1 = j|0x80 feed 2j (u 0) and 2j+1 (u 1); both generators have
		// their first and last taps, so T[2j+1] = T[2j|0x100] = T[2j]^3 and T[2j+1|0x100] = T[2j]
		uint8_t f[NS];
		for (int j = 0; j < NS / 2; j++) {
			const int o = T[2 * j], b0 = bm[o], b1 = bm[o ^ 3];
			const int p0 = pm[j], p1 = pm[j | 0x80];
			const int m00 = p0 + b0, m10 = p1 + b1, m01 = p0 + b1, m11 = p1 + b0;
			f[2 * j] = m10 < m00; npm[2 * j] = m10 < m00 ? m10 : m00;
			f[2 * j + 1] = m11 < m01; npm[2 * j + 1] = m11 < m01 ? m11 : m01;
		}
		pack_choices(f, &ch[size_t(i) * 4]);
		if (i >= nbits) for (int ns = 1; ns < NS; ns += 2) npm[ns] = kU;   // tail bits are zero
		std::swap(pm, npm);
	}
	out.assign(size_t((nbits + 7) / 8), 0);
	int s = 0;
	const int metric = pm[0];
	for (int i = steps - 1; i >= 0; i--) {
		const int u = s & 1;
		if (i < nbits && u) out[size_t(i / 8)] |= uint8_t(1 << (i % 8));
		const bool f1 = (ch[size_t(i) * 4 + size_t(s >> 6)] >> (s & 63)) & 1;
		s = (s >> 1) | (f1 ? 0x80 : 0);
	}
	return metric;
}

int viterbi_soft(const float *llr, int nbits, std::vector<uint8_t> &out)
{
	constexpr int NS = 256;
	constexpr float kNone = -1e30f;
	const int steps = nbits + 8;
	const uint8_t *T = conv_out_table();
	float pa[NS], pb[NS], *pm = pa, *npm = pb;
	for (float &v : pa) v = kNone;
	pm[0] = 0;
	// As the hard one: kNone absorbs any step metric (|llr| is far below its precision), so an
	// unreachable state stays exactly kNone and loses against any reachable one.
	std::vector<uint64_t> ch(size_t(steps) * 4, 0);
	for (int i = 0; i < steps; i++) {
		const float a = llr[2 * i], b = llr[2 * i + 1];
		const float xa[2] = {a, -a}, xb[2] = {b, -b};
		uint64_t *c = &ch[size_t(i) * 4];
		for (int w = 0; w < 4; w++) {
			uint64_t bits = 0;
			for (int k = 0; k < 64; k++) {
				const int ns = 64 * w + k, s0 = ns >> 1, o0 = T[ns], o1 = T[ns | 0x100];
				const float m0 = pm[s0] + xa[o0 & 1] + xb[o0 >> 1], m1 = pm[s0 | 0x80] + xa[o1 & 1] + xb[o1 >> 1];
				const bool f1 = m1 > m0;
				npm[ns] = f1 ? m1 : m0;
				bits |= uint64_t(f1) << k;
			}
			c[w] = bits;
		}
		if (i >= nbits) for (int ns = 1; ns < NS; ns += 2) npm[ns] = kNone;   // tail bits are zero
		std::swap(pm, npm);
	}
	out.assign(size_t((nbits + 7) / 8), 0);
	int st = 0, bad = 0;
	for (int i = steps - 1; i >= 0; i--) {
		const int u = st & 1;
		if (i < nbits && u) out[size_t(i / 8)] |= uint8_t(1 << (i % 8));
		const bool f1 = (ch[size_t(i) * 4 + size_t(st >> 6)] >> (st & 63)) & 1;
		const unsigned reg = unsigned(st) | (f1 ? 0x100u : 0u);
		if (llr[2 * i] != 0 && (llr[2 * i] < 0) != bool(parity(reg & 0x171))) bad++;
		if (llr[2 * i + 1] != 0 && (llr[2 * i + 1] < 0) != bool(parity(reg & 0x1EB))) bad++;
		st = (st >> 1) | (f1 ? 0x80 : 0);
	}
	return bad;
}

void psk_soft(double v, int k, unsigned comp, float *out)
{
	const int M = 1 << k;
	const double step = 16.0 / M;
	double best[8][2];
	for (int b = 0; b < k; b++) best[b][0] = best[b][1] = 1e9;
	for (int sidx = 0; sidx < M; sidx++) {
		double d = std::fmod(v - sidx * step + 1608.0, 16.0) - 8.0;
		d = std::fabs(std::fmod(d + 24.0, 16.0) - 8.0);
		const unsigned label = gray(unsigned(sidx)) ^ comp;
		for (int b = 0; b < k; b++) { double &x = best[b][(label >> b) & 1]; if (d * d < x) x = d * d; }
	}
	for (int b = 0; b < k; b++) out[b] = float(best[b][1] - best[b][0]);
}

std::vector<float> level_llr(int level, const double (*steps)[2], int ppr, const double (*wt)[2])
{
	const int nbits = kInfoBits(level, ppr), k = kBitsPerSym[level];
	const int L = 2 * (nbits + 8);
	std::vector<float> llr(size_t(L), 0.0f);
	const unsigned comp = (level == 2) ? 3u : (level == 1 ? 1u : 0u);
	const int base = (level <= 2) ? 2 : 0;
	for (int g = 0; g < 8; g++)
		for (int p = 0; p < ppr; p++)
			for (int car = 0; car < 2; car++) {
				float sb[4];
				psk_soft(steps[ppr * g + p][car] - base, k, comp, sb);
				const float w = wt ? float(wt[ppr * g + p][car]) : 1.0f;   // the step's amplitude weight
				for (int b = 0; b < k; b++) {
					const size_t j = size_t(coded_index(level, g, p, car, b, ppr));
					if (j < size_t(L)) llr[j] = w * sb[b];
				}
			}
	return llr;
}

RxResult decode_steps(int level, const double (*steps)[2], int ppr)
{
	if (soft_decoding()) {
		const int nbits = kInfoBits(level, ppr), k = kBitsPerSym[level];
		const int L = 2 * (nbits + 8);
		std::vector<float> llr(size_t(L), 0.0f);
		const unsigned comp = (level == 2) ? 3u : (level == 1 ? 1u : 0u);
		const int base = (level <= 2) ? 2 : 0;
		for (int g = 0; g < 8; g++)
			for (int p = 0; p < ppr; p++)
				for (int car = 0; car < 2; car++) {
					float sb[4];
					psk_soft(steps[ppr * g + p][car] - base, k, comp, sb);
					for (int b = 0; b < k; b++) {
						const size_t j = size_t(coded_index(level, g, p, car, b, ppr));
						if (j < size_t(L)) llr[j] = sb[b];
					}
				}
		RxResult r;
		r.level = level;
		r.metric = viterbi_soft(llr.data(), nbits, r.info);
		const int n = data_bytes(level, ppr);
		r.ok = crc16_ccitt_reflected(r.info.data(), size_t(n + 1)) == uint16_t(r.info[size_t(n + 1)] | r.info[size_t(n + 2)] << 8);
		if (std::getenv("P2_SOFT_CMP")) {                // experiment: count hard vs soft outcomes
			static std::mutex mu; static int cnt[2][2];
			std::vector<uint8_t> coded(size_t(L), 0), erased(size_t(L), 1), hi;
			for (int j = 0; j < L; j++) if (llr[size_t(j)] != 0) { erased[size_t(j)] = 0; coded[size_t(j)] = llr[size_t(j)] < 0; }
			viterbi(coded.data(), erased.data(), nbits, hi);
			const bool hok = crc16_ccitt_reflected(hi.data(), size_t(n + 1)) == uint16_t(hi[size_t(n + 1)] | hi[size_t(n + 2)] << 8);
			std::lock_guard<std::mutex> lk(mu);
			cnt[hok][r.ok]++;
			std::fprintf(stderr, "[p2] L%d ppr %d hard/soft ok: 00 %d 01 %d 10 %d 11 %d\n", level, ppr, cnt[0][0], cnt[0][1], cnt[1][0], cnt[1][1]);
		}
		return r;
	}
	const int nbits = kInfoBits(level, ppr), k = kBitsPerSym[level];
	const int L = 2 * (nbits + 8);
	std::vector<uint8_t> coded(size_t(L), 0), erased(size_t(L), 1);
	const unsigned comp = (level == 2) ? 3u : (level == 1 ? 1u : 0u);
	const int base = (level <= 2) ? 2 : 0, M = 1 << k;
	const double step = 16.0 / M;
	for (int g = 0; g < 8; g++)
		for (int p = 0; p < ppr; p++)
			for (int car = 0; car < 2; car++) {
				const int s = int(std::lround((steps[ppr * g + p][car] - base) / step)) & (M - 1);
				const unsigned label = gray(unsigned(s)) ^ comp;
				for (int b = 0; b < k; b++) {
					const size_t j = size_t(coded_index(level, g, p, car, b, ppr));
					if (j >= size_t(L)) continue;
					coded[j] = uint8_t((label >> b) & 1);
					erased[j] = 0;
				}
			}
	RxResult r;
	r.level = level;
	r.metric = viterbi(coded.data(), erased.data(), nbits, r.info);
	const int n = data_bytes(level, ppr);
	r.ok = crc16_ccitt_reflected(r.info.data(), size_t(n + 1)) == uint16_t(r.info[size_t(n + 1)] | r.info[size_t(n + 2)] << 8);
	return r;
}

HeaderFit fit_header(const double h[8][2], int only_level)
{
	HeaderFit best;
	for (int lv = 1; lv <= 4; lv++) {
		if (only_level && lv != only_level) continue;
		for (int par = 0; par < 2; par++) {
			std::complex<double> acc = 0;
			double d[16];
			for (int i = 0; i < 8; i++)
				for (int car = 0; car < 2; car++) {
					const int want = car ? kHeader[lv][par][i + 1] & 15 : kHeader[lv][par][i + 1] >> 4;
					d[2 * i + car] = h[i][car] - want;
					acc += std::polar(1.0, 2 * M_PI * d[2 * i + car] / 16.0);
				}
			const double bias = 16.0 * std::arg(acc) / (2 * M_PI);
			double e2 = 0;
			for (double v : d) { double e = std::fmod(v - bias + 1608.0, 16.0) - 8.0; e2 += e * e; }
			const double rms = std::sqrt(e2 / 16);
			if (rms < best.rms) best = {lv, par, bias, rms};
		}
	}
	return best;
}

}  // namespace fmodem::p2
