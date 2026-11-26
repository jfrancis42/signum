// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// The functions of p3_codec.hpp (out of the header, so that they build in parallel).
#include "p3_codec.hpp"

namespace fmodem::p3 {

std::vector<uint8_t> encode(const uint8_t *info, int counter, bool first)
{
	std::vector<uint8_t> coded(kCoded);
	unsigned reg = 0;
	for (int i = 0; i < kSteps; i++) {
		const int b = i < kInfoBits ? (info[i / 8] >> (i % 8)) & 1 : 0;
		reg = ((reg << 1) | unsigned(b)) & 0x7f;
		coded[size_t(2 * i)] = uint8_t(parity(reg & 0x6D));
		coded[size_t(2 * i + 1)] = uint8_t(parity(reg & 0x4F));
	}
	std::vector<uint8_t> kept;
	for (int j = 0; j < kCoded; j++) if (kept_pos(j)) kept.push_back(coded[size_t(j)]);
	std::vector<uint8_t> w{0x0a, 0x0a};
	const uint8_t *hdr = (counter & 1) ? sl2400::kHeaderOdd : sl2400::kHeaderEven;
	w.insert(w.end(), hdr, hdr + 160);
	for (int n = 0; n < kDataPulses * kCarriers; n++) {
		unsigned lab = 0;
		for (int b = 0; b < 2; b++) {
			const int i = 2 * n + b;
			lab |= unsigned(kept[sl2400::kPi[i]] ^ sl2400::kComp[i]) << b;
		}
		unsigned s = lab;                                // Gray label -> step index
		s ^= s >> 1;
		w.push_back(uint8_t(2 + 4 * s));
	}
	if (first) w.insert(w.end(), 16, uint8_t(0x0a));
	else w.insert(w.end(), sl2400::kTailPulse, sl2400::kTailPulse + 16);
	return w;
}

RxResult decode_steps_hard(const double *steps)
{
	std::vector<int8_t> coded(kCoded, -1);             // -1 = erased
	for (int n = 0; n < kDataPulses * kCarriers; n++) {
		const int s = int(std::lround((steps[n] - 2) / 4.0)) & 3;
		const unsigned lab = unsigned(s) ^ (unsigned(s) >> 1);
		for (int b = 0; b < 2; b++) {
			const int i = 2 * n + b;
			coded[size_t(coded_of_kept(sl2400::kPi[i]))] = int8_t(((lab >> b) & 1) ^ sl2400::kComp[i]);
		}
	}
	constexpr int NS = 64;
	std::vector<int> pm(NS, 1 << 28), npm(NS);
	std::vector<uint8_t> dec(size_t(kSteps) * NS), prev(size_t(kSteps) * NS);
	pm[0] = 0;
	for (int i = 0; i < kSteps; i++) {
		std::fill(npm.begin(), npm.end(), 1 << 30);
		const int lim = i < kInfoBits ? 2 : 1;
		for (int s = 0; s < NS; s++) {
			if (pm[size_t(s)] >= 1 << 28) continue;
			for (int u = 0; u < lim; u++) {
				const unsigned reg = ((unsigned(s) << 1) | unsigned(u)) & 0x7f;
				const int ns = int(reg & 0x3f);
				int m = pm[size_t(s)];
				const int8_t c0 = coded[size_t(2 * i)], c1 = coded[size_t(2 * i + 1)];
				if (c0 >= 0) m += parity(reg & 0x6D) != c0;
				if (c1 >= 0) m += parity(reg & 0x4F) != c1;
				if (m < npm[size_t(ns)]) { npm[size_t(ns)] = m; dec[size_t(i) * NS + size_t(ns)] = uint8_t(u); prev[size_t(i) * NS + size_t(ns)] = uint8_t(s); }
			}
		}
		pm.swap(npm);
	}
	RxResult r;
	int s = 0;
	r.metric = pm[0];
	for (int i = kSteps - 1; i >= 0; i--) {
		const int u = dec[size_t(i) * NS + size_t(s)];
		if (i < kInfoBits && u) r.info[i / 8] |= uint8_t(1 << (i % 8));
		s = prev[size_t(i) * NS + size_t(s)];
	}
	r.ok = crc16_ccitt_reflected(r.info, kDataBytes + 1) == uint16_t(r.info[kDataBytes + 1] | r.info[kDataBytes + 2] << 8);
	return r;
}

RxResult decode_steps(const double *steps, const double *wt)
{
	static const bool hard = std::getenv("P3_SL5_HARD") != nullptr;
	if (hard) return decode_steps_hard(steps);
	std::vector<double> soft(kCoded, 0.0);             // > 0: coded bit 1
	auto dist = [](double a, double b) { double d = vit::mod16(a - b + 64.0); return d > 8 ? 16 - d : d; };
	for (int n = 0; n < kDataPulses * kCarriers; n++) {
		const double x = steps[n], w = wt ? wt[n] : 1.0;
		double d[4];
		for (int q = 0; q < 4; q++) d[q] = dist(x, 2 + 4 * q);
		for (int b = 0; b < 2; b++) {
			const int i = 2 * n + b;
			double m0 = 1e9, m1 = 1e9;
			for (int q = 0; q < 4; q++) {
				const unsigned lab = unsigned(q) ^ (unsigned(q) >> 1);
				const int bit = int(((lab >> b) & 1) ^ unsigned(sl2400::kComp[i]));
				double &m = bit ? m1 : m0;
				m = std::min(m, d[q]);
			}
			soft[size_t(coded_of_kept(sl2400::kPi[i]))] = (m0 - m1) * w;
		}
	}
	constexpr int NS = 64;
	std::vector<double> pm(NS, 1e30), npm(NS);
	std::vector<uint8_t> dec(size_t(kSteps) * NS), prev(size_t(kSteps) * NS);
	pm[0] = 0;
	// all 64 states per step at once (viterbi64.hpp; P3_VIT_OLD=1: the loop it replaces)
	static const bool vit_old = std::getenv("P3_VIT_OLD") != nullptr;
	static const vit::AcsTables<double, vit::Form::Sel> acs(vit::parity_mask(0x6D), vit::parity_mask(0x4F));
	for (int i = 0; i < kSteps; i++) {
		const int lim = i < kInfoBits ? 2 : 1;
		const double s0 = soft[size_t(2 * i)], s1 = soft[size_t(2 * i + 1)];
		if (!vit_old) { vit::acs_step(acs, pm.data(), npm.data(), &dec[size_t(i) * NS], s0, s1, true, lim > 1, 1e30, 1e29); pm.swap(npm); continue; }
		std::fill(npm.begin(), npm.end(), 1e30);
		for (int s = 0; s < NS; s++) {
			if (pm[size_t(s)] >= 1e29) continue;
			for (int u = 0; u < lim; u++) {
				const unsigned reg = ((unsigned(s) << 1) | unsigned(u)) & 0x7f;
				const int ns = int(reg & 0x3f);
				const double m = pm[size_t(s)] - (parity(reg & 0x6D) ? s0 : -s0) - (parity(reg & 0x4F) ? s1 : -s1);
				if (m < npm[size_t(ns)]) { npm[size_t(ns)] = m; dec[size_t(i) * NS + size_t(ns)] = uint8_t(u); prev[size_t(i) * NS + size_t(ns)] = uint8_t(s); }
			}
		}
		pm.swap(npm);
	}
	RxResult r;
	int s = 0;
	r.metric = int(std::lround(pm[0]));
	{ double sa = 0; for (double v : soft) sa += std::fabs(v); r.agree = sa > 0 ? -pm[0] / sa : 0; }
	for (int i = kSteps - 1; i >= 0; i--) {
		const uint8_t cd = dec[size_t(i) * NS + size_t(s)];
		const int u = vit_old ? cd : vit::vit_u(cd, s);
		if (i < kInfoBits && u) r.info[i / 8] |= uint8_t(1 << (i % 8));
		s = vit_old ? prev[size_t(i) * NS + size_t(s)] : vit::vit_prev(cd, s);
	}
	r.ok = crc16_ccitt_reflected(r.info, kDataBytes + 1) == uint16_t(r.info[kDataBytes + 1] | r.info[kDataBytes + 2] << 8);
	return r;
}

}  // namespace fmodem::p3
