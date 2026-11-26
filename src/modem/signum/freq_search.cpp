// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// The functions of freq_search.hpp (out of the header, so that they build in parallel).
#include "freq_search.hpp"

namespace fmodem::fsearch {

void fft(std::complex<double> *a, int n)
{
	for (int i = 1, j = 0; i < n; i++) {
		int bit = n >> 1;
		for (; j & bit; bit >>= 1) j ^= bit;
		j ^= bit;
		if (i < j) std::swap(a[i], a[j]);
	}
	for (int len = 2; len <= n; len <<= 1) {
		const double ang = -2 * M_PI / len, wr0 = std::cos(ang), wi0 = std::sin(ang);
		for (int i = 0; i < n; i += len) {
			double wr = 1, wi = 0;
			for (int j = 0; j < len / 2; j++) {
				// written out: std::complex's operator* checks for NaN (a library call per product)
				std::complex<double> &p = a[i + j], &q = a[i + j + len / 2];
				const double vr = q.real() * wr - q.imag() * wi, vi = q.real() * wi + q.imag() * wr;
				const double ur = p.real(), ui = p.imag();
				p = {ur + vr, ui + vi}; q = {ur - vr, ui - vi};
				const double t = wr * wr0 - wi * wi0; wi = wr * wi0 + wi * wr0; wr = t;
			}
		}
	}
}

Result burst_offset(const float *x, long n, double fs, const double *carriers, const double *weights, int nc, const PulsePower &pw,
                           double centre, double range)
{
	Result r;
	if (n < 256 || nc < 1 || fs <= 0) return r;
	// Welch power spectrum: Hann segments of kN, half overlap; a shorter burst is one zero-padded segment
	static thread_local double S[kN / 2 + 1];
	static thread_local std::complex<double> a[kN];
	for (double &v : S) v = 0;
	const long seg = std::min<long>(kN, n), hop = seg / 2;
	int nseg = 0;
	for (long s0 = 0; s0 + seg <= n; s0 += hop) {
		for (int i = 0; i < kN; i++) a[i] = 0;
		for (long i = 0; i < seg; i++) a[i] = double(x[s0 + i]) * (0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / seg));
		fft(a, kN);
		for (int k = 0; k <= kN / 2; k++) S[k] += a[k].real() * a[k].real() + a[k].imag() * a[k].imag();
		nseg++;
		if (seg < kN) break;
	}
	if (nseg == 0) return r;
	const double df = fs / kN;
	double fmin = 1e9, fmax = -1e9;
	for (int c = 0; c < nc; c++) { fmin = std::min(fmin, carriers[c]); fmax = std::max(fmax, carriers[c]); }
	const double lo = std::max(df, fmin + centre - range - kMaxW), hi = std::min(fs / 2 - df, fmax + centre + range + kMaxW);
	const int k0 = int(std::ceil(lo / df)), k1 = int(std::floor(hi / df));
	if (k1 - k0 < 8) return r;
	// in-band power (within 60 Hz of the carriers at the centre) against the rest of the band
	double pin = 0, pout = 0; int nin = 0, nout = 0;
	double mean = 0;
	for (int k = k0; k <= k1; k++) {
		mean += S[k];
		const double f = k * df - centre;
		bool in = false;
		for (int c = 0; c < nc && !in; c++) in = std::fabs(f - carriers[c]) < 60.0;
		if (in) { pin += S[k]; nin++; } else { pout += S[k]; nout++; }
	}
	mean /= double(k1 - k0 + 1);
	r.snr_db = (nin && nout && pout > 0) ? 10 * std::log10((pin / nin) / (pout / nout)) : 0.0;
	// the comb at the nominal carriers on a 0.5 Hz grid, 0..fs/2
	constexpr int kTMax = 2 * 6000 + 2;
	static thread_local double T[kTMax];
	const int nt = std::min(kTMax, int(fs) + 1);              // 0.5 Hz steps up to fs/2
	for (int i = 0; i < nt; i++) T[i] = 0;
	for (int q = 0; q < nc; q++) {
		const int a0 = std::max(0, int(std::ceil(2 * (carriers[q] - kMaxW)))), a1 = std::min(nt - 1, int(std::floor(2 * (carriers[q] + kMaxW))));
		const double wq = weights ? weights[q] : 1.0;
		for (int i = a0; i <= a1; i++) T[i] += wq * pw.at(0.5 * i - carriers[q]);
	}
	auto tmpl = [&](double f) {
		const double u = 2 * f;
		if (u < 0 || u >= nt - 1) return 0.0;
		const int i = int(u);
		const double w = u - i;
		return T[i] * (1 - w) + T[i + 1] * w;
	};
	// correlation of the mean-removed spectrum with the comb, offsets centre +- range on 0.5 Hz
	constexpr int kMaxSteps = 2 * 2 * 100 + 1;
	double c[kMaxSteps];
	const int ns = std::min(kMaxSteps, int(2 * 2 * range) + 1);
	for (int s = 0; s < ns; s++) {
		const double off = centre - range + 0.5 * s;
		double acc = 0;
		for (int k = k0; k <= k1; k++) acc += (S[k] - mean) * tmpl(k * df - off);
		c[s] = acc;
	}
	int best = 0;
	for (int s = 1; s < ns; s++) if (c[s] > c[best]) best = s;
	double fine = 0;
	if (best > 0 && best < ns - 1) {
		const double y0 = c[best - 1], y1 = c[best], y2 = c[best + 1], den = y0 - 2 * y1 + y2;
		if (den < 0) fine = std::max(-0.5, std::min(0.5, 0.5 * (y0 - y2) / den));
	}
	r.hz = centre - range + 0.5 * (best + fine);
	// the match: correlation coefficient of the spectrum with the comb at the offset found
	{
		double st = 0, ss = 0, tt = 0, tm = 0;
		for (int k = k0; k <= k1; k++) tm += tmpl(k * df - r.hz);
		tm /= double(k1 - k0 + 1);
		for (int k = k0; k <= k1; k++) { const double sv = S[k] - mean, tv = tmpl(k * df - r.hz) - tm; st += sv * tv; ss += sv * sv; tt += tv * tv; }
		r.match = (ss > 0 && tt > 0) ? st / std::sqrt(ss * tt) : 0.0;
	}
	// the comb match (fix 261): spectrum and comb at hz, each less its moving average over one comb
	// period (120 Hz), correlated where the whole average is inside the band searched
	{
		static thread_local double A[kN / 2 + 1], B[kN / 2 + 1];
		const int n = k1 - k0 + 1, h = std::max(1, int(std::lround(60.0 / df)));
		for (int i = 0; i < n; i++) { A[i] = S[k0 + i]; B[i] = tmpl((k0 + i) * df - r.hz); }
		double ra = 0, rb = 0;                           // running sums over [i - h, i + h]
		for (int j = 0; j < 2 * h + 1 && j < n; j++) { ra += A[j]; rb += B[j]; }
		double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0; int m = 0;
		for (int i = h; i + h < n; i++) {
			if (i > h) { ra += A[i + h] - A[i - h - 1]; rb += B[i + h] - B[i - h - 1]; }
			const double a = A[i] - ra / (2 * h + 1), b = B[i] - rb / (2 * h + 1);
			sa += a; sb += b; saa += a * a; sbb += b * b; sab += a * b; m++;
		}
		if (m > 2) {
			const double ca = saa - sa * sa / m, cb = sbb - sb * sb / m, cab = sab - sa * sb / m;
			r.comb = (ca > 0 && cb > 0) ? cab / std::sqrt(ca * cb) : 0.0;
		}
	}
	// a peak at the edge of the search is not a measurement
	r.ok = best > 0 && best < ns - 1 && c[best] > 0;
	return r;
}

}  // namespace fmodem::fsearch
