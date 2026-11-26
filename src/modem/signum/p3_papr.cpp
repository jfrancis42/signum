// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// The functions of p3_papr.hpp (out of the header, so that they build in parallel).
#include "p3_papr.hpp"

namespace fmodem::p3 {

std::vector<float> papr_taps(double fs)
{
	const int n = 129, m = n / 2;
	const double f1 = 300.0 / fs, f2 = 2700.0 / fs;
	std::vector<float> h(static_cast<size_t>(n), 0.f);
	for (int i = 0; i < n; i++) {
		const int k = i - m;
		const double lp2 = k == 0 ? 2 * f2 : std::sin(2 * M_PI * f2 * k) / (M_PI * k);
		const double lp1 = k == 0 ? 2 * f1 : std::sin(2 * M_PI * f1 * k) / (M_PI * k);
		const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (n - 1)) + 0.08 * std::cos(4 * M_PI * i / (n - 1));
		h[size_t(i)] = float((lp2 - lp1) * w);
	}
	return h;
}

std::vector<float> papr_reduce(const std::vector<float> &x, double fs, float clip, int iterations, double peak)
{
	if (clip <= 0 || x.empty()) return x;
	double pk = 0;
	for (float v : x) pk = std::max(pk, double(std::fabs(v)));
	if (pk <= 0) return x;
	if (peak <= 0) peak = pk;
	const auto h = papr_taps(fs);
	const long n = long(x.size()), m = long(h.size()) / 2;
	std::vector<float> y = x, c(static_cast<size_t>(n));
	for (int it = 0; it < iterations; it++) {
		double s = 0; long cnt = 0, lo = n, hi = -1;
		double ypk = 0;
		for (float v : y) ypk = std::max(ypk, double(std::fabs(v)));
		for (long i = 0; i < n; i++)
			if (std::fabs(y[size_t(i)]) > 0.01 * ypk) { s += double(y[size_t(i)]) * y[size_t(i)]; cnt++; lo = std::min(lo, i); hi = i; }
		if (cnt == 0) break;
		const float a = float(clip * std::sqrt(s / double(cnt)));
		for (long i = 0; i < n; i++) c[size_t(i)] = std::max(-a, std::min(a, y[size_t(i)]));
		// band-limit (only around the burst: the rest stays zero)
		const long b0 = std::max(0L, lo - m), b1 = std::min(n - 1, hi + m);
		std::fill(y.begin(), y.end(), 0.f);
		for (long i = b0; i <= b1; i++) {
			double acc = 0;
			const long k0 = std::max(0L, i - m), k1 = std::min(n - 1, i + m);
			for (long k = k0; k <= k1; k++) acc += double(h[size_t(m + i - k)]) * c[size_t(k)];
			y[size_t(i)] = float(acc);
		}
	}
	double ypk = 0;
	for (float v : y) ypk = std::max(ypk, double(std::fabs(v)));
	if (ypk > 0) for (auto &v : y) v = float(v * (peak / ypk));
	return y;
}

}  // namespace fmodem::p3
