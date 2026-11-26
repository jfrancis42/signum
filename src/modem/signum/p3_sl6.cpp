// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// The functions of p3_sl6.hpp (out of the header, so that they build in parallel).
#include "p3_sl6.hpp"

namespace fmodem::p3 {

std::vector<uint8_t> sl6_encode(const uint8_t *data, uint8_t status)
{
	uint8_t info[kSl6Info];
	std::memcpy(info, data, kSl6Data);
	info[kSl6Data] = status;
	const uint16_t c = crc16_ccitt_reflected(info, kSl6Data + 1);
	info[kSl6Data + 1] = uint8_t(c);
	info[kSl6Data + 2] = uint8_t(c >> 8);
	std::vector<uint8_t> coded(kSl6Coded);
	unsigned reg = 0;
	for (int i = 0; i < kSl6Steps; i++) {
		const int b = i < kSl6InfoBits ? (info[i / 8] >> (i % 8)) & 1 : 0;
		reg = ((reg << 1) | unsigned(b)) & 0x7f;
		coded[size_t(2 * i)] = uint8_t(__builtin_popcount(reg & 0x6D) & 1);
		coded[size_t(2 * i + 1)] = uint8_t(__builtin_popcount(reg & 0x4F) & 1);
	}
	const auto &kp = sl6_kept_positions();
	const uint8_t *hdr = (status & 1) ? sl6::kHeaderOdd : sl6::kHeaderEven;
	std::vector<uint8_t> w(hdr, hdr + 162);
	for (int n = 0; n < kSl6Symbols; n++) {
		unsigned lab = 0;
		for (int b = 0; b < 2; b++) {
			const int i = 2 * n + b;
			lab |= unsigned(coded[size_t(kp[sl6::kPi[i]])] ^ sl6::kComp[i]) << b;
		}
		const unsigned s = lab ^ (lab >> 1);           // Gray label -> step index
		w.push_back(uint8_t(2 + 4 * s));
	}
	w.insert(w.end(), sl6::kTail, sl6::kTail + 18);
	return w;
}

Lv23Rx sl6_decode_hard(const double *steps)
{
	std::vector<int8_t> coded(kSl6Coded, -1);
	const auto &kp = sl6_kept_positions();
	for (int n = 0; n < kSl6Symbols; n++) {
		const int s = int(std::lround((steps[162 + n] - 2) / 4.0)) & 3;
		const unsigned lab = unsigned(s) ^ (unsigned(s) >> 1);
		for (int b = 0; b < 2; b++) {
			const int i = 2 * n + b;
			coded[size_t(kp[sl6::kPi[i]])] = int8_t(((lab >> b) & 1) ^ sl6::kComp[i]);
		}
	}
	constexpr int NS = 64;
	std::vector<int> pm(NS, 1 << 28), npm(NS);
	std::vector<uint8_t> dec(size_t(kSl6Steps) * NS), prev(size_t(kSl6Steps) * NS);
	pm[0] = 0;
	for (int i = 0; i < kSl6Steps; i++) {
		std::fill(npm.begin(), npm.end(), 1 << 30);
		const int lim = i < kSl6InfoBits ? 2 : 1;
		for (int s = 0; s < NS; s++) {
			if (pm[size_t(s)] >= 1 << 28) continue;
			for (int u = 0; u < lim; u++) {
				const unsigned reg = ((unsigned(s) << 1) | unsigned(u)) & 0x7f;
				const int ns = int(reg & 0x3f);
				int m = pm[size_t(s)];
				const int8_t c0 = coded[size_t(2 * i)], c1 = coded[size_t(2 * i + 1)];
				if (c0 >= 0) m += (__builtin_popcount(reg & 0x6D) & 1) != c0;
				if (c1 >= 0) m += (__builtin_popcount(reg & 0x4F) & 1) != c1;
				if (m < npm[size_t(ns)]) { npm[size_t(ns)] = m; dec[size_t(i) * NS + size_t(ns)] = uint8_t(u); prev[size_t(i) * NS + size_t(ns)] = uint8_t(s); }
			}
		}
		pm.swap(npm);
	}
	uint8_t info[kSl6Info] = {};
	int s = 0;
	for (int i = kSl6Steps - 1; i >= 0; i--) {
		const int u = dec[size_t(i) * NS + size_t(s)];
		if (i < kSl6InfoBits && u) info[i / 8] |= uint8_t(1 << (i % 8));
		s = prev[size_t(i) * NS + size_t(s)];
	}
	Lv23Rx r;
	r.ok = crc16_ccitt_reflected(info, kSl6Data + 1) == uint16_t(info[kSl6Data + 1] | info[kSl6Data + 2] << 8);
	r.data.assign(info, info + kSl6Data);
	r.status = info[kSl6Data];
	return r;
}

Lv23Rx sl6_decode(const double *steps, const double *wt)
{
	static const bool hard = std::getenv("P3_SL6_HARD") != nullptr;
	if (hard) return sl6_decode_hard(steps);
	std::vector<double> soft(kSl6Coded, 0.0);          // > 0: coded bit 1; 0: not sent
	const auto &kp = sl6_kept_positions();
	auto dist = [](double a, double b) { double d = vit::mod16(a - b + 64.0); return d > 8 ? 16 - d : d; };
	for (int n = 0; n < kSl6Symbols; n++) {
		const double x = steps[162 + n], w = wt ? wt[162 + n] : 1.0;
		double d[4];
		for (int q = 0; q < 4; q++) d[q] = dist(x, 2 + 4 * q);
		for (int b = 0; b < 2; b++) {
			const int i = 2 * n + b;
			double m0 = 1e9, m1 = 1e9;
			for (int q = 0; q < 4; q++) {
				const unsigned lab = unsigned(q) ^ (unsigned(q) >> 1);
				const int bit = int(((lab >> b) & 1) ^ unsigned(sl6::kComp[i]));
				double &m = bit ? m1 : m0;
				m = std::min(m, d[q]);
			}
			soft[size_t(kp[sl6::kPi[i]])] = (m0 - m1) * w;
		}
	}
	constexpr int NS = 64;
	std::vector<double> pm(NS, 1e30), npm(NS);
	std::vector<uint8_t> dec(size_t(kSl6Steps) * NS), prev(size_t(kSl6Steps) * NS);
	pm[0] = 0;
	// all 64 states per step at once (viterbi64.hpp; P3_VIT_OLD=1: the loop it replaces)
	static const bool vit_old = std::getenv("P3_VIT_OLD") != nullptr;
	static const vit::AcsTables<double, vit::Form::Sel> acs(vit::parity_mask(0x6D), vit::parity_mask(0x4F));
	for (int i = 0; i < kSl6Steps; i++) {
		const int lim = i < kSl6InfoBits ? 2 : 1;
		const double s0 = soft[size_t(2 * i)], s1 = soft[size_t(2 * i + 1)];
		if (!vit_old) { vit::acs_step(acs, pm.data(), npm.data(), &dec[size_t(i) * NS], s0, s1, true, lim > 1, 1e30, 1e29); pm.swap(npm); continue; }
		std::fill(npm.begin(), npm.end(), 1e30);
		for (int s = 0; s < NS; s++) {
			if (pm[size_t(s)] >= 1e29) continue;
			for (int u = 0; u < lim; u++) {
				const unsigned reg = ((unsigned(s) << 1) | unsigned(u)) & 0x7f;
				const int ns = int(reg & 0x3f);
				const double m = pm[size_t(s)] - ((__builtin_popcount(reg & 0x6D) & 1) ? s0 : -s0) - ((__builtin_popcount(reg & 0x4F) & 1) ? s1 : -s1);
				if (m < npm[size_t(ns)]) { npm[size_t(ns)] = m; dec[size_t(i) * NS + size_t(ns)] = uint8_t(u); prev[size_t(i) * NS + size_t(ns)] = uint8_t(s); }
			}
		}
		pm.swap(npm);
	}
	uint8_t info[kSl6Info] = {};
	int s = 0;
	for (int i = kSl6Steps - 1; i >= 0; i--) {
		const uint8_t cd = dec[size_t(i) * NS + size_t(s)];
		const int u = vit_old ? cd : vit::vit_u(cd, s);
		if (i < kSl6InfoBits && u) info[i / 8] |= uint8_t(1 << (i % 8));
		s = vit_old ? prev[size_t(i) * NS + size_t(s)] : vit::vit_prev(cd, s);
	}
	Lv23Rx r;
	r.ok = crc16_ccitt_reflected(info, kSl6Data + 1) == uint16_t(info[kSl6Data + 1] | info[kSl6Data + 2] << 8);
	r.data.assign(info, info + kSl6Data);
	r.status = info[kSl6Data];
	return r;
}

}  // namespace fmodem::p3
