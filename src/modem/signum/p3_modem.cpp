// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// The functions of p3_modem.hpp (out of the header, so that they build in parallel).
#include "p3_modem.hpp"

namespace fmodem::p3 {

void ls_axmy(double *r, const double *k, double u, int n)
{
	int j = 0;
	if (!ls_old()) {
		typedef double v2 __attribute__((vector_size(16)));
		const v2 uu = {u, u};
		for (; j + 2 <= n; j += 2) {
			v2 a, b;
			std::memcpy(&a, r + j, sizeof a); std::memcpy(&b, k + j, sizeof b);
			a -= uu * b;
			std::memcpy(r + j, &a, sizeof a);
		}
	}
	for (; j < n; j++) r[j] -= u * k[j];
}

void ls_chol(double *A, int N, int bw)
{
	for (int i = 0; i < N; i++) {
		double *ri = &A[size_t(i) * size_t(bw)];
		for (int k = std::max(0, i - bw + 1); k < i; k++) {
			const double u = A[size_t(k) * size_t(bw) + size_t(i - k)];
			if (u == 0) continue;
			const double *rk = &A[size_t(k) * size_t(bw)];
			ls_axmy(ri, rk + (i - k), u, std::min(N, k + bw) - i);
		}
		const double piv = std::sqrt(std::max(ri[0], 1e-12));
		for (int j = 0; j < bw && i + j < N; j++) ri[j] /= piv;
	}
}

std::vector<float> modulate(const std::vector<uint8_t> &words, double fs, bool odd, float amplitude, double lead_s)
{
	const double end = lead_s + (kAllPulses - 1) * kT + kPulseHalfS;
	std::vector<float> y(size_t(std::ceil(end * fs)) + 1, 0.f);
	int ph[kHdrCarriers];
	for (int k = 0; k < kHdrCarriers; k++) ph[k] = (k * k) % 16;   // Newman-like start (low crest factor)
	// Fast path (2026-10-22, fix 153; the Pi 4 took 24 ms per SL5 burst, and a burst made
	// late stalls the audio loop): the pulse is the same table at every position when T and
	// the lead are whole samples, and each carrier's phase advances by a fixed rotation per
	// sample. Same waveform to float rounding. P3_MOD_SLOW: the per-sample cos (test).
	static const bool slow = std::getenv("P3_MOD_SLOW") != nullptr;
	const double Tf = kT * fs, Lf = lead_s * fs;
	if (!slow && std::fabs(Tf - std::round(Tf)) < 1e-9 && std::fabs(Lf - std::round(Lf)) < 1e-9) {
		const long Tn = long(std::lround(Tf));
		const long a0 = long(std::ceil((lead_s - kPulseHalfS) * fs)), b0 = long(std::floor((lead_s + kPulseHalfS) * fs));
		std::vector<double> g(size_t(b0 - a0 + 1));
		for (long i = a0; i <= b0; i++) g[size_t(i - a0)] = amplitude * pulse(double(i) / fs - lead_s);
		for (int p = 0; p < kAllPulses; p++) {
			const SlotMap &m = slot_map(p, odd);
			const int nc = m.n;
			for (int j = 0; j < nc; j++) {
				const int k = m.k[j];
				ph[k] = (ph[k] - (words[size_t(word_index(p, j))] & 15)) & 15;
			}
			const long s0 = a0 + long(p) * Tn;
			const long i0 = std::max(0L, s0), i1 = std::min(long(y.size()) - 1, b0 + long(p) * Tn);
			for (int j = 0; j < nc; j++) {
				const int k = m.k[j];
				const double w = 2 * M_PI * carrier_hz(k), phi = 2 * M_PI * ph[k] / 16.0;
				std::complex<double> z = std::polar(1.0, w * double(i0) / fs + phi);
				const std::complex<double> rot = std::polar(1.0, w / fs);
				for (long i = i0; i <= i1; i++) { y[size_t(i)] += float(g[size_t(i - s0)] * z.real()); z *= rot; }
			}
		}
		return y;
	}
	for (int p = 0; p < kAllPulses; p++) {
		const SlotMap &m = slot_map(p, odd);
		const int nc = m.n;
		const double c = lead_s + p * kT;
		for (int j = 0; j < nc; j++) {
			const int k = m.k[j];
			ph[k] = (ph[k] - (words[size_t(word_index(p, j))] & 15)) & 15;
		}
		const long i0 = std::max(0L, long(std::ceil((c - kPulseHalfS) * fs)));
		const long i1 = std::min(long(y.size()) - 1, long(std::floor((c + kPulseHalfS) * fs)));
		for (int j = 0; j < nc; j++) {
			const int k = m.k[j];
			const double w = 2 * M_PI * carrier_hz(k), phi = 2 * M_PI * ph[k] / 16.0;
			for (long i = i0; i <= i1; i++) {
				const double t = double(i) / fs;
				y[size_t(i)] += amplitude * pulse(t - c) * float(std::cos(w * t + phi));
			}
		}
	}
	return y;
}

bool bk_check(const std::vector<uint8_t> &info, uint8_t *data, bool &more, int *type, bool *qrt, bool *over)
{
	if (info.size() < 6) return false;
	if (uint16_t(crc16_ccitt_reflected(info.data(), 4) ^ kBkCrcXor) != uint16_t(info[4] | info[5] << 8)) return false;
	const uint8_t st = uint8_t(info[3] ^ kBkStatus);
	if (st & ~(kBkMore | 0x1c | 0x40 | 0x80)) return false;   // 0x80: QRT with the last text [bb-ext: CMS FQ + disconnect]; 0x40 over
	if (qrt) *qrt = (st & 0x80) != 0;
	if (over) *over = (st & 0x40) != 0;
	data[0] = info[0]; data[1] = info[1]; data[2] = uint8_t(info[2] ^ 0x11);
	more = (st & kBkMore) != 0;
	if (type) *type = (st >> 2) & 7;
	return true;
}

const std::vector<double> & cs_pulse_table(float fs)
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
		for (int i = 0; i < n; i++) t[size_t(i)] = cs_pulse((i - 2 * T) / fs);
		cached = fs;
	}
	return t;
}

std::vector<float> modulate_cs(const std::vector<uint8_t> &words, float fs, bool swap, float amplitude)
{
	const double T = 0.010 * fs, st = kCsStagger * T, start0 = 1.2 * T;
	const size_t len = size_t(start0 + double(words.size()) * T + st + 3 * T) + 1;
	std::vector<float> y(len, 0.0f);
	for (int role = 0; role < 2; role++) {
		const double f = (role == 0) == !swap ? kCsLo : kCsHi;
		double ph = 0;
		for (size_t k = 0; k < words.size(); k++) {
			const int nib = role == 0 ? (words[k] >> 4) & 15 : words[k] & 15;
			ph -= 2 * M_PI * nib / 16.0;
			const double c = start0 + (role ? st : 0) + double(k) * T + 0.8 * T;
			for (long n = long(std::ceil(c - 2 * T)); n <= long(std::floor(c + 2 * T)); n++) {
				if (n < 0 || size_t(n) >= len) continue;
				y[size_t(n)] += float(amplitude * cs_pulse((double(n) - c) / fs) * std::cos(2 * M_PI * f * double(n) / fs + ph));
			}
		}
	}
	return y;
}

void sl1_make_info(const uint8_t *data, uint8_t status, uint8_t *info /*8*/)
{
	for (int i = 0; i < kSl1Data; i++) info[i] = data[i];
	info[kSl1Data] = uint8_t(status ^ 0x10);
	const uint16_t c = crc16_ccitt_reflected(info, kSl1Data + 1) ^ 0xA8A1;
	for (int i = 0; i < kSl1Data; i++) info[i] ^= kSl1Scr[i];
	info[kSl1Data + 1] = uint8_t(c); info[kSl1Data + 2] = uint8_t(c >> 8);
}

bool sl1_check(const uint8_t *rx, uint8_t *data, uint8_t &status)
{
	uint8_t b[kSl1Data + 1];
	for (int i = 0; i < kSl1Data; i++) b[i] = uint8_t(rx[i] ^ kSl1Scr[i]);
	b[kSl1Data] = rx[kSl1Data];
	const uint16_t c = crc16_ccitt_reflected(b, kSl1Data + 1) ^ 0xA8A1;
	for (int i = 0; i < kSl1Data; i++) data[i] = b[i];
	status = uint8_t(rx[kSl1Data] ^ 0x10);
	return c == uint16_t(rx[kSl1Data + 1] | rx[kSl1Data + 2] << 8);
}

Demod demod_slow(const float *x, long n, double fs, double t0, int p0, int p1, double df)
{
	Demod d; d.p0 = p0; d.p1 = p1;
	struct Col { long i0; std::vector<double> v; };
	std::vector<Col> cols;
	for (int p = p0; p < p1; p++) {
		d.col0.push_back(int(cols.size()));
		const double c = t0 + p * kT * fs;
		const long i0 = std::max(0L, long(std::ceil(c - kPulseHalfS * fs)));
		const long i1 = std::min(n - 1, long(std::floor(c + kPulseHalfS * fs)));
		const int nc = pulse_carriers(p);
		for (int kk = 0; kk < nc; kk++) {
			const int k = nc == kHdrCarriers ? kk : kk + 1;
			const double w = 2 * M_PI * (carrier_hz(k) + df) / fs;
			Col a{i0, {}}, b{i0, {}};
			for (long i = i0; i <= i1; i++) {
				const double g = pulse((double(i) - c) / fs);
				a.v.push_back(g * std::cos(w * double(i)));
				b.v.push_back(-g * std::sin(w * double(i)));
			}
			cols.push_back(std::move(a)); cols.push_back(std::move(b));
		}
	}
	const int N = int(cols.size());
	// bandwidth: columns of the next two pulses
	int bw = 0;
	for (int q = 0; q + 1 < int(d.col0.size()); q++) {
		const int e = q + 3 < int(d.col0.size()) ? d.col0[size_t(q + 3)] : N;
		bw = std::max(bw, e - d.col0[size_t(q)]);
	}
	if (bw == 0) bw = N;
	auto dot = [&](const Col &a, const Col &b) {
		const long s = std::max(a.i0, b.i0), e = std::min(a.i0 + long(a.v.size()), b.i0 + long(b.v.size()));
		double acc = 0;
		for (long i = s; i < e; i++) acc += a.v[size_t(i - a.i0)] * b.v[size_t(i - b.i0)];
		return acc;
	};
	// banded storage: L[i][j-i] for j in [i, i+bw)
	std::vector<double> A(size_t(N) * size_t(bw), 0.0), rhs(size_t(N), 0.0);
	for (int i = 0; i < N; i++) {
		for (int j = i; j < std::min(N, i + bw); j++) A[size_t(i) * size_t(bw) + size_t(j - i)] = dot(cols[size_t(i)], cols[size_t(j)]);
		double acc = 0;
		for (size_t m = 0; m < cols[size_t(i)].v.size(); m++) acc += cols[size_t(i)].v[m] * x[cols[size_t(i)].i0 + long(m)];
		rhs[size_t(i)] = acc;
		A[size_t(i) * size_t(bw)] *= 1 + 1e-9;   // tiny ridge
	}
	// banded Cholesky: A = U^T U, U upper with bandwidth bw (stored in place)
	ls_chol(&A[0], N, bw);
	std::vector<double> y(rhs);
	for (int i = 0; i < N; i++) {                 // U^T y = rhs
		for (int k = std::max(0, i - bw + 1); k < i; k++) y[size_t(i)] -= A[size_t(k) * size_t(bw) + size_t(i - k)] * y[size_t(k)];
		y[size_t(i)] /= A[size_t(i) * size_t(bw)];
	}
	for (int i = N - 1; i >= 0; i--) {            // U a = y
		for (int j = i + 1; j < std::min(N, i + bw); j++) y[size_t(i)] -= A[size_t(i) * size_t(bw) + size_t(j - i)] * y[size_t(j)];
		y[size_t(i)] /= A[size_t(i) * size_t(bw)];
	}
	d.z.resize(size_t(N / 2));
	for (int c = 0; c < N / 2; c++) d.z[size_t(c)] = {y[size_t(2 * c)], y[size_t(2 * c + 1)]};
	// residual over the modelled span
	const long s = cols.front().i0, e = cols.back().i0 + long(cols.back().v.size());
	std::vector<double> m(size_t(e - s), 0.0);
	for (int i = 0; i < N; i++)
		for (size_t q = 0; q < cols[size_t(i)].v.size(); q++) m[size_t(cols[size_t(i)].i0 - s) + q] += y[size_t(i)] * cols[size_t(i)].v[q];
	double rr = 0, xx = 0;
	for (long i = s; i < e; i++) { const double r = x[i] - m[size_t(i - s)]; rr += r * r; xx += double(x[i]) * x[i]; }
	d.resid = xx > 0 ? std::sqrt(rr / xx) : 1;
	return d;
}

DemodPlan demod_plan(long n, double fs, double t0, int p0, int p1, double df)
{
	DemodPlan P; P.fs = fs; P.t0 = t0; P.df = df; P.p0 = p0; P.p1 = p1;
	const int Tn = int(std::lround(kT * fs));
	const int np = p1 - p0;
	const double c0 = t0 + double(p0) * Tn;
	const long base = long(std::ceil(c0 - kPulseHalfS * fs));
	const int L = int(long(std::floor(c0 + kPulseHalfS * fs)) - base + 1);
	if (std::fabs(kT * fs - Tn) > 1e-9 || base < 0 || base + long(np - 1) * Tn + L > n || np < 1) return P;
	P.ok = true; P.Tn = Tn; P.np = np; P.base = base; P.L = L;
	std::vector<double> &g = P.g;
	g.resize(static_cast<size_t>(L));
	for (int m = 0; m < L; m++) g[size_t(m)] = pulse((double(base + m) - c0) / fs);
	// U_d for d = 0..2, W = 2 pi 120 D / fs (D = k1-k2, -17..17) and
	// W = 2 pi (f_k1 + f_k2 + 2 df) / fs (k1+k2 = 0..34).
	constexpr int NK = kHdrCarriers;
	std::complex<double> Um[3][2 * NK - 1], Up[3][2 * NK - 1];
	for (int dd = 0; dd < 3; dd++)
		for (int D = 0; D < 2 * NK - 1; D++) {
			const double wm = 2 * M_PI * 120.0 * (D - (NK - 1)) / fs;
			const double wp = 2 * M_PI * (carrier_hz(0) * 2 + 120.0 * D + 2 * df) / fs;
			std::complex<double> am = 0, ap = 0;
			std::complex<double> pm = std::polar(1.0, wm * dd * Tn), pp = std::polar(1.0, wp * dd * Tn);
			const std::complex<double> rm = std::polar(1.0, wm), rp = std::polar(1.0, wp);
			for (int m = dd * Tn; m < L; m++) {
				const double gg = g[size_t(m)] * g[size_t(m - dd * Tn)];
				am += gg * pm; ap += gg * pp;
				pm *= rm; pp *= rp;
			}
			Um[dd][D] = am; Up[dd][D] = ap;
		}
	// columns
	for (int r = 0; r < np; r++) {
		P.col0.push_back(int(2 * P.col_r.size()));
		const int nc = pulse_carriers(p0 + r);
		for (int kk = 0; kk < nc; kk++) { P.col_r.push_back(r); P.col_k.push_back(nc == kHdrCarriers ? kk : kk + 1); }
	}
	const int NC = int(P.col_r.size()), N = 2 * NC;
	int bw = 0;
	for (size_t q = 0; q < P.col0.size(); q++) {
		const int e = q + 3 < P.col0.size() ? P.col0[q + 3] : N;
		bw = std::max(bw, e - P.col0[q]);
	}
	P.NC = NC; P.N = N; P.bw = bw;
	std::vector<double> &A = P.A;
	A.assign(size_t(N) * size_t(bw), 0.0);
	// phase factors e^{jW s0} per (difference/sum index, pulse)
	std::vector<std::complex<double>> Pm(size_t((2 * NK - 1) * np)), Pp(size_t((2 * NK - 1) * np));
	for (int D = 0; D < 2 * NK - 1; D++) {
		const double wm = 2 * M_PI * 120.0 * (D - (NK - 1)) / fs;
		const double wp = 2 * M_PI * (carrier_hz(0) * 2 + 120.0 * D + 2 * df) / fs;
		std::complex<double> em = std::polar(1.0, wm * double(base)), ep = std::polar(1.0, wp * double(base));
		const std::complex<double> sm = std::polar(1.0, wm * Tn), sp = std::polar(1.0, wp * Tn);
		for (int r = 0; r < np; r++) { Pm[size_t(D * np + r)] = em; Pp[size_t(D * np + r)] = ep; em *= sm; ep *= sp; }
	}
	// normal matrix (upper band)
	for (int c1 = 0; c1 < NC; c1++) {
		const int ar = P.col_r[size_t(c1)], ak = P.col_k[size_t(c1)];
		for (int c2 = c1; c2 < NC; c2++) {
			const int br = P.col_r[size_t(c2)], bk = P.col_k[size_t(c2)];
			const int dd = br - ar;
			if (dd > 2) break;
			const int Dm = ak - bk + NK - 1, Dp = ak + bk;
			const std::complex<double> Sm = Pm[size_t(Dm * np + ar)] * Um[dd][Dm];
			const std::complex<double> Sp = Pp[size_t(Dp * np + ar)] * Up[dd][Dp];
			const double aa = 0.5 * (Sm.real() + Sp.real()), bb = 0.5 * (Sm.real() - Sp.real());
			const double ab = -0.5 * Sp.imag() + 0.5 * Sm.imag(), ba = -0.5 * Sp.imag() - 0.5 * Sm.imag();
			const int i = 2 * c1, j = 2 * c2;
			auto put = [&](int r, int q, double v) { if (q >= r && q - r < bw) A[size_t(r) * size_t(bw) + size_t(q - r)] = v; };
			put(i, j, aa); put(i + 1, j + 1, bb); put(i, j + 1, ab); put(i + 1, j, ba);
			if (c1 == c2) put(i, i + 1, ab);
		}
	}
	for (int i = 0; i < N; i++) A[size_t(i) * size_t(bw)] *= 1 + 1e-9;
	ls_chol(&A[0], N, bw);                            // banded Cholesky (as demod_slow)
	return P;
}

void ls_forward(const double *A, int N, int bw, double *y, int k0, int k1)
{
	// (columns k0..k1-1 only: the same operations on each y[i], in the same order, when the columns are
	// done in pieces, k0 ascending; column k needs y up to k + bw - 1)
	if (k1 < 0) k1 = N;
	for (int k = k0; k < k1; k++) {
		y[k] /= A[size_t(k) * size_t(bw)];
		const double yk = y[k];
		const double *ak = &A[size_t(k) * size_t(bw)];
		const int e = std::min(N, k + bw);
		ls_axmy(y + k + 1, ak + 1, yk, e - k - 1);
	}
}

Demod demod_run(const DemodPlan &P, const float *x, long n)
{
	if (!P.ok || P.base + long(P.np - 1) * P.Tn + P.L > n) return demod_slow(x, n, P.fs, P.t0, P.p0, P.p1, P.df);
	Demod d; d.p0 = P.p0; d.p1 = P.p1; d.col0 = P.col0;
	const double fs = P.fs, df = P.df;
	const int Tn = P.Tn, L = P.L, NC = P.NC, N = P.N, bw = P.bw;
	const long base = P.base;
	const std::vector<double> &g = P.g, &A = P.A;
	auto w_of = [&](int k) { return 2 * M_PI * (carrier_hz(k) + df) / fs; };
	// right-hand side: z = sum g x e^{-jwn} -> (a, b) = (Re z, Im z)
	std::vector<double> rhs(size_t(N), 0.0);
	static const bool slow = std::getenv("P3_LS_SLOW") != nullptr;   // test: the forms before fix 216b
	if (!slow) ls_rhs_rows(P.col_r.data(), P.col_k.data(), NC, base, Tn, L, g.data(), x, w_of, rhs.data());
	else for (int c = 0; c < NC; c++) {
		const long s0 = base + long(P.col_r[size_t(c)]) * Tn;
		const double w = w_of(P.col_k[size_t(c)]);
		std::complex<double> ph = std::polar(1.0, -w * double(s0)), rot = std::polar(1.0, -w), z = 0;
		for (int m = 0; m < L; m++) { z += g[size_t(m)] * double(x[s0 + m]) * ph; ph *= rot; }
		rhs[size_t(2 * c)] = z.real(); rhs[size_t(2 * c + 1)] = z.imag();
	}
	std::vector<double> y(rhs);
	if (!slow) ls_forward(A.data(), N, bw, y.data());
	else for (int i = 0; i < N; i++) {
		for (int k = std::max(0, i - bw + 1); k < i; k++) y[size_t(i)] -= A[size_t(k) * size_t(bw) + size_t(i - k)] * y[size_t(k)];
		y[size_t(i)] /= A[size_t(i) * size_t(bw)];
	}
	for (int i = N - 1; i >= 0; i--) {
		for (int j = i + 1; j < std::min(N, i + bw); j++) y[size_t(i)] -= A[size_t(i) * size_t(bw) + size_t(j - i)] * y[size_t(j)];
		y[size_t(i)] /= A[size_t(i) * size_t(bw)];
	}
	d.z.resize(size_t(NC));
	for (int c = 0; c < NC; c++) d.z[size_t(c)] = {y[size_t(2 * c)], y[size_t(2 * c + 1)]};
	// residual over the modelled span
	const long e = base + long(P.np - 1) * Tn + L;
	std::vector<double> mdl(size_t(e - base), 0.0);
	if (!slow) ls_model_rows(P.col_r.data(), P.col_k.data(), NC, base, Tn, L, g.data(), w_of, y.data(), mdl.data());
	else for (int c = 0; c < NC; c++) {
		const long s0 = base + long(P.col_r[size_t(c)]) * Tn;
		const double w = w_of(P.col_k[size_t(c)]);
		const std::complex<double> zc(y[size_t(2 * c)], y[size_t(2 * c + 1)]);   // a g cos + b (-g sin) = g Re(z e^{jwn})
		std::complex<double> ph = std::polar(1.0, w * double(s0)), rot = std::polar(1.0, w);
		for (int m = 0; m < L; m++) { mdl[size_t(s0 - base + m)] += g[size_t(m)] * (zc * ph).real(); ph *= rot; }
	}
	double rr = 0, xx = 0;
	for (long i = base; i < e; i++) { const double r = x[i] - mdl[size_t(i - base)]; rr += r * r; xx += double(x[i]) * x[i]; }
	d.resid = xx > 0 ? std::sqrt(rr / xx) : 1;
	return d;
}

HeaderScore score_header(const Demod &d)
{
	HeaderScore best;
	for (int odd = 0; odd < 2; odd++)
		for (int par = 0; par < 2; par++) {
			const auto w = header_words(par);
			double c = 0, s = 0; int n = 0;
			for (int p = 1; p < kHdrPulses; p++)
				for (int j = 0; j < kHdrCarriers; j++) {
					const double e = 2 * M_PI * (step(d, p, j, odd) - w[size_t(word_index(p, j))]) / 16;
					c += std::cos(e); s += std::sin(e); n++;
				}
			// remove the common rotation (frequency offset) before scoring
			const double bias = std::atan2(s, c);
			double sc = 0;
			for (int p = 1; p < kHdrPulses; p++)
				for (int j = 0; j < kHdrCarriers; j++)
					sc += std::cos(2 * M_PI * (step(d, p, j, odd) - w[size_t(word_index(p, j))]) / 16 - bias);
			sc /= n;
			if (sc > best.score) best = {sc, par, odd != 0, bias / (2 * M_PI) * 16};
		}
	return best;
}

Sync5 sync_at(const float *x, long n, double fs, double t0, double search_ms)
{
	Sync5 s; s.tb = t0;
	// Fix 219: beyond +-4 ms (re-acquiring after misses) a coarser grid, refined at the old one.
	const double cs = search_ms > 4.0 ? 4 * std::floor(search_ms / 4.0) : 4;
	const double cw = std::floor(search_ms * 9.6 / cs) * cs;
	for (double dt = -cw; dt <= cw + 0.01; dt += cs) {
		const auto d = demod(x, n, fs, t0 + dt * fs / 9600, 0, kHdrPulses + 1);
		const auto h = score_header(d);
		if (h.score > s.hb.score) { s.hb = h; s.tb = t0 + dt * fs / 9600; }
	}
	if (cs > 4) {
		const double c = s.tb;
		for (double dt = -cs; dt <= cs + 0.01; dt += 4) {
			if (dt == 0) continue;
			const auto h = score_header(demod(x, n, fs, c + dt * fs / 9600, 0, kHdrPulses + 1));
			if (h.score > s.hb.score) { s.hb = h; s.tb = c + dt * fs / 9600; }
		}
	}
	const double fw = search_ms < 1 ? 1.5 : 3;
	for (double dt = -fw; dt <= fw + 0.01; dt += 0.5) {
		const double t = s.tb + dt * fs / 9600;
		if (dt == 0) continue;
		const auto h = score_header(demod(x, n, fs, t, 0, kHdrPulses + 1));
		if (h.score > s.hb.score) { s.hb = h; s.tb = t; }
	}
	return s;
}

RxPacket finish_at(const float *x, long n, double fs, const Sync5 &s, std::vector<std::complex<double>> *dd, const DemodPlan *plan)
{
	RxPacket out;
	const HeaderScore &hb = s.hb; const double tb = s.tb;
	// frequency offset: bias b (1/16 turn per T) -> b / (16 T) Hz
	const double df = hb.bias / (16 * kT);
	// a plan made while the packet arrived (fix 152), if it is for this timing and offset
	const bool planned = plan && plan->ok && plan->t0 == tb && plan->df == df && plan->fs == fs && plan->p0 == 0 && plan->p1 == kAllPulses;
	const auto d = planned ? demod_run(*plan, x, n) : demod(x, n, fs, tb, 0, kAllPulses, df);
	const auto h2 = score_header(d);
	if (dd) {                                          // memory ARQ: this copy's products, bias removed (same demodulation)
		const std::complex<double> rot = std::polar(1.0, 2 * M_PI * h2.bias / 16.0);
		dd->assign(size_t(kDataPulses * kCarriers), {0, 0});
		for (int p = kHdrPulses; p < kHdrPulses + kDataPulses; p++)
			for (int j = 0; j < kCarriers; j++) {
				const int k = slot_carrier(p, j, hb.odd);
				(*dd)[size_t((p - kHdrPulses) * kCarriers + j)] = d.at(p, k) * std::conj(d.at(p - 1, k)) * rot;
			}
	}
	std::vector<double> st(size_t(kDataPulses * kCarriers)), wt(st.size(), 1.0);
	double wm = 0;
	for (int p = kHdrPulses; p < kHdrPulses + kDataPulses; p++)
		for (int j = 0; j < kCarriers; j++) {
			st[size_t((p - kHdrPulses) * kCarriers + j)] = step(d, p, j, hb.odd) - h2.bias;
			const int k = slot_carrier(p, j, hb.odd);
			wm += wt[size_t((p - kHdrPulses) * kCarriers + j)] = std::abs(d.at(p, k)) * std::abs(d.at(p - 1, k));
		}
	// soft bits weighted by |differential product| (2026-10-18); P3_NO_WT=1: off
	static const bool use_wt = std::getenv("P3_NO_WT") == nullptr;
	if (wm > 0) for (auto &v : wt) v /= wm / double(wt.size());
	out.r = decode_steps(st.data(), use_wt && wm > 0 ? wt.data() : nullptr);
	out.ok = out.r.ok; out.t0 = tb; out.odd = hb.odd; out.parity = hb.parity; out.hscore = h2.score; out.resid = d.resid; out.df = df;
	return out;
}

std::vector<std::complex<double>> sl5_diffs(const float *x, long n, double fs, const Sync5 &s)
{
	const HeaderScore &hb = s.hb;
	const double df = hb.bias / (16 * kT);
	const auto d = demod(x, n, fs, s.tb, 0, kAllPulses, df);
	const auto h2 = score_header(d);
	const std::complex<double> rot = std::polar(1.0, 2 * M_PI * h2.bias / 16.0);
	std::vector<std::complex<double>> out(size_t(kDataPulses * kCarriers));
	for (int p = kHdrPulses; p < kHdrPulses + kDataPulses; p++)
		for (int j = 0; j < kCarriers; j++) {
			const int k = slot_carrier(p, j, hb.odd);
			out[size_t((p - kHdrPulses) * kCarriers + j)] = d.at(p, k) * std::conj(d.at(p - 1, k)) * rot;
		}
	return out;
}

RxResult sl5_decode_diffs(const std::vector<std::complex<double>> &dd)
{
	static const bool use_wt = std::getenv("P3_NO_WT") == nullptr;
	std::vector<double> st(dd.size()), wt(dd.size(), 1.0);
	double wm = 0;
	for (size_t i = 0; i < dd.size(); i++) { const double v = -std::arg(dd[i]) / (2 * M_PI) * 16; st[i] = v < 0 ? v + 16 : v; wm += wt[i] = std::abs(dd[i]); }
	if (wm > 0) for (auto &v : wt) v /= wm / double(wt.size());
	return decode_steps(st.data(), use_wt && wm > 0 ? wt.data() : nullptr);
}

std::vector<RxPacket> receive(const std::vector<float> &x, double fs)
{
	std::vector<RxPacket> pk;
	const long n = long(x.size()), win = long(kT * fs);
	if (n < win * 4) return pk;
	std::vector<double> e(size_t(n), 0.0);
	double acc = 0, peak = 0;
	for (long i = 0; i < n; i++) {
		acc += double(x[size_t(i)]) * x[size_t(i)];
		if (i >= win) acc -= double(x[size_t(i - win)]) * x[size_t(i - win)];
		e[size_t(i)] = std::sqrt(std::max(acc, 0.0) / double(win));
		peak = std::max(peak, e[size_t(i)]);
	}
	const double th = peak * 0.25;
	for (long i = 1; i < n; i++) {
		if (!(e[size_t(i)] >= th && e[size_t(i - 1)] < th)) continue;
		long j = i;
		while (j < n && e[size_t(j)] >= th * 0.5) j++;
		if ((j - i) < long(0.7 * fs)) { i = j; continue; }
		// the 25 % edge of the trailing-window rms falls near pulse 0's centre
		const double t0 = double(i);
		RxPacket best;
		for (int dp = -1; dp <= 1 && !best.ok; dp++) {
			const auto r = decode_at(x.data(), n, fs, t0 + dp * kT * fs);
			if (r.ok || r.hscore > best.hscore) best = r;
		}
		pk.push_back(best);
		i = j;
	}
	return pk;
}

}  // namespace fmodem::p3
