// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// The functions of p3_lv23.hpp (out of the header, so that they build in parallel).
#include "p3_lv23.hpp"

namespace fmodem::p3 {

const Lv23 & lv23_cfg(int level)
{
	using namespace lv23;
	static const Lv23 L2{2, kData2, kHdr2, kDat2, kWords2, kStx2, kCrcx2, kScr2, kMap2, {kTmpl2_0, kTmpl2_1, kTmpl2_2, kTmpl2_3}};
	static const Lv23 L3{3, kData3, kHdr3, kDat3, kWords3, kStx3, kCrcx3, kScr3, kMap3, {kTmpl3_0, kTmpl3_1, kTmpl3_2, kTmpl3_3}};
	return level == 2 ? L2 : L3;
}

std::vector<uint8_t> lv23_encode(int level, const uint8_t *data, uint8_t status)
{
	const Lv23 &L = lv23_cfg(level);
	std::vector<uint8_t> blk(size_t(L.ndata + 3));
	for (int i = 0; i < L.ndata; i++) blk[size_t(i)] = data[i];
	const uint8_t st = uint8_t(status ^ L.stx);
	blk[size_t(L.ndata)] = st;
	const uint16_t c = uint16_t(crc16_ccitt_reflected(blk.data(), size_t(L.ndata + 1)) ^ L.crcx);
	for (int i = 0; i < L.ndata; i++) blk[size_t(i)] ^= L.scr[i];
	blk[size_t(L.ndata + 1)] = uint8_t(c); blk[size_t(L.ndata + 2)] = uint8_t(c >> 8);
	const int nbits = (L.ndata + 3) * 8;
	std::vector<uint8_t> coded(size_t(2 * (nbits + 8)));
	unsigned reg = 0;
	for (int i = 0; i < nbits + 8; i++) {
		const int b = i < nbits ? (blk[size_t(i / 8)] >> (i % 8)) & 1 : 0;
		reg = ((reg << 1) | unsigned(b)) & 0x7f;
		coded[size_t(2 * i)] = uint8_t(lv23_parity(reg & 0x6D));
		coded[size_t(2 * i + 1)] = uint8_t(lv23_parity(reg & 0x4F));
	}
	const uint8_t *t = L.tmpl[status & 3];
	std::vector<uint8_t> w(size_t(L.nwords));
	for (int j = 0; j < L.nwords; j++) {
		int s = t[j];
		if (j >= L.hdr && j < L.hdr + L.ndat) s = (s & 1) | (coded[L.map[j - L.hdr]] << 1);
		w[size_t(j)] = uint8_t(2 + 4 * s);
	}
	return w;
}

Lv23Rx lv23_decode(int level, const double *steps, int counter_hint, const double *wt)
{
	const Lv23 &L = lv23_cfg(level);
	Lv23Rx best;
	for (int ctr = 0; ctr < 4; ctr++) {
		if (counter_hint >= 0 && ctr != counter_hint) continue;
		const uint8_t *t = L.tmpl[ctr];
		const int nbits = (L.ndata + 3) * 8, NC = 2 * (nbits + 8);
		std::vector<double> soft(size_t(NC), 0.0);       // >0: coded bit 1
		for (int j = 0; j < L.ndat; j++) {
			const int b0 = t[L.hdr + j] & 1;
			const double v0 = 2 + 4 * b0, v1 = 2 + 4 * (b0 + 2);  // symbol bit1 = 0 / 1
			auto dist = [&](double a, double b) { double d = vit::mod16(a - b + 64.0); return d > 8 ? 16 - d : d; };
			const double s = steps[L.hdr + j];
			soft[L.map[j]] = (dist(s, v0) - dist(s, v1)) * (wt ? wt[L.hdr + j] : 1.0);
		}
		// Viterbi (soft, K=7)
		const int S = 64;
		std::vector<double> pm(S, 1e18), npm(S);
		pm[0] = 0;
		std::vector<uint8_t> dec(size_t(nbits + 8) * S), prv(size_t(nbits + 8) * S);
		// all 64 states per step at once (viterbi64.hpp; P3_VIT_OLD=1: the loop it replaces)
		static const bool vit_old = std::getenv("P3_VIT_OLD") != nullptr;
		static const vit::AcsTables<double, vit::Form::Mul> acs(vit::parity_mask(0x6D), vit::parity_mask(0x4F));
		for (int i = 0; i < nbits + 8; i++) {
			if (!vit_old) { vit::acs_step(acs, pm.data(), npm.data(), &dec[size_t(i) * S], soft[size_t(2 * i)], soft[size_t(2 * i + 1)], true, i < nbits, 1e18, 1e17); pm.swap(npm); continue; }
			std::fill(npm.begin(), npm.end(), 1e18);
			for (int s = 0; s < S; s++) {
				if (pm[size_t(s)] >= 1e17) continue;
				for (int u = 0; u < (i < nbits ? 2 : 1); u++) {
					const unsigned reg = ((unsigned(s) << 1) | unsigned(u)) & 0x7f;
					const int ns = int(reg & 0x3f);
					const int c0 = lv23_parity(reg & 0x6D), c1 = lv23_parity(reg & 0x4F);
					const double m = pm[size_t(s)] - (c0 ? 1 : -1) * soft[size_t(2 * i)] - (c1 ? 1 : -1) * soft[size_t(2 * i + 1)];
					if (m < npm[size_t(ns)]) { npm[size_t(ns)] = m; dec[size_t(i) * S + size_t(ns)] = uint8_t(u); prv[size_t(i) * S + size_t(ns)] = uint8_t(s); }
				}
			}
			pm.swap(npm);
		}
		std::vector<uint8_t> blk(size_t(L.ndata + 3), 0);
		int s = 0;
		for (int i = nbits + 8 - 1; i >= 0; i--) {
			const uint8_t cd = dec[size_t(i) * S + size_t(s)];
			const int u = vit_old ? cd : vit::vit_u(cd, s);
			if (i < nbits && u) blk[size_t(i / 8)] |= uint8_t(1 << (i % 8));
			s = vit_old ? prv[size_t(i) * S + size_t(s)] : vit::vit_prev(cd, s);
		}
		Lv23Rx r;
		{ double sa = 0; for (double v : soft) sa += std::fabs(v); r.agree = sa > 0 ? -pm[0] / sa : 0; }
		r.data.resize(size_t(L.ndata));
		for (int i = 0; i < L.ndata; i++) r.data[size_t(i)] = uint8_t(blk[size_t(i)] ^ L.scr[i]);
		std::vector<uint8_t> chk(r.data);
		chk.push_back(blk[size_t(L.ndata)]);
		const uint16_t c = uint16_t(crc16_ccitt_reflected(chk.data(), chk.size()) ^ L.crcx);
		r.ok = c == uint16_t(blk[size_t(L.ndata + 1)] | blk[size_t(L.ndata + 2)] << 8);
		r.status = uint8_t(blk[size_t(L.ndata)] ^ L.stx);
		if (r.ok && (r.status & 3) == ctr) return r;
		if (!best.ok && r.ok) best = r;
	}
	return best;
}

std::vector<uint8_t> lv4_encode(const uint8_t *data, uint8_t status)
{
	using namespace lv4;
	uint8_t blk[kData4 + 4];
	for (int i = 0; i < kData4; i++) blk[i] = data[i];
	const uint8_t st = uint8_t(status ^ kStx4);
	blk[kData4] = st;
	const uint16_t c = uint16_t(crc16_ccitt_reflected(blk, size_t(kData4 + 1)) ^ kCrcx4);
	for (int i = 0; i < kData4; i++) blk[i] ^= kScr4[i];
	blk[kData4 + 1] = uint8_t(c); blk[kData4 + 2] = uint8_t(c >> 8); blk[kData4 + 3] = 0xFF;
	std::vector<uint8_t> coded(2016);
	unsigned reg = 0x3F;
	for (int i = 0; i < (kData4 + 4) * 8; i++) {
		reg = ((reg << 1) | unsigned((blk[i / 8] >> (i % 8)) & 1)) & 0x7f;
		coded[size_t(2 * i)] = uint8_t(lv23_parity(reg & 0x6D));
		coded[size_t(2 * i + 1)] = uint8_t(lv23_parity(reg & 0x4F));
	}
	const uint8_t *t = (status & 3) == 0 ? kTmpl4_0 : (status & 3) == 1 ? kTmpl4_1 : (status & 3) == 2 ? kTmpl4_2 : kTmpl4_3;
	std::vector<uint8_t> w(static_cast<size_t>(kWords4));
	for (int j = 0; j < kWords4; j++) {
		int s = t[j];
		if (j >= kHdr4 && j < kHdr4 + kSym4) {
			const int i = j - kHdr4;
			const int g = coded[kMap4[2 * i]] | (coded[kMap4[2 * i + 1]] << 1);
			s = g ^ (g >> 1);
		}
		w[size_t(j)] = uint8_t(2 + 4 * s);
	}
	return w;
}

Lv23Rx lv4_decode(const double *steps, const double *wt)
{
	using namespace lv4;
	const int NI = (kData4 + 4) * 8, NC = 2 * NI;
	std::vector<double> soft(size_t(NC), 0.0);
	auto dist = [](double a, double b) { double d = vit::mod16(a - b + 64.0); return d > 8 ? 16 - d : d; };
	for (int i = 0; i < kSym4; i++) {
		const double x = steps[kHdr4 + i];
		double d[4];
		for (int s = 0; s < 4; s++) d[s] = dist(x, 2 + 4 * s);
		for (int b = 0; b < 2; b++) {
			double m0 = 1e9, m1 = 1e9;
			for (int s = 0; s < 4; s++) { const int g = s ^ (s >> 1); (((g >> b) & 1) ? m1 : m0) = std::min(((g >> b) & 1) ? m1 : m0, d[s]); }
			soft[kMap4[2 * i + b]] = (m0 - m1) * (wt ? wt[kHdr4 + i] : 1.0);
		}
	}
	const int S = 64;
	std::vector<double> pm(S, 1e18), npm(S);
	pm[0x3F] = 0;
	std::vector<uint8_t> dec(size_t(NI) * S), prv(size_t(NI) * S);
	// all 64 states per step at once (viterbi64.hpp; P3_VIT_OLD=1: the loop it replaces)
	static const bool vit_old = std::getenv("P3_VIT_OLD") != nullptr;
	static const vit::AcsTables<double, vit::Form::Mul> acs(vit::parity_mask(0x6D), vit::parity_mask(0x4F));
	for (int i = 0; i < NI; i++) {
		const bool tail = i >= NI - 8;
		if (!vit_old) { vit::acs_step(acs, pm.data(), npm.data(), &dec[size_t(i) * S], soft[size_t(2 * i)], soft[size_t(2 * i + 1)], !tail, true, 1e18, 1e17); pm.swap(npm); continue; }
		std::fill(npm.begin(), npm.end(), 1e18);
		for (int s = 0; s < S; s++) {
			if (pm[size_t(s)] >= 1e17) continue;
			for (int u = tail ? 1 : 0; u < 2; u++) {
				const unsigned reg = ((unsigned(s) << 1) | unsigned(u)) & 0x7f;
				const int ns = int(reg & 0x3f);
				const double m = pm[size_t(s)] - (lv23_parity(reg & 0x6D) ? 1 : -1) * soft[size_t(2 * i)] - (lv23_parity(reg & 0x4F) ? 1 : -1) * soft[size_t(2 * i + 1)];
				if (m < npm[size_t(ns)]) { npm[size_t(ns)] = m; dec[size_t(i) * S + size_t(ns)] = uint8_t(u); prv[size_t(i) * S + size_t(ns)] = uint8_t(s); }
			}
		}
		pm.swap(npm);
	}
	std::vector<uint8_t> blk(size_t(kData4 + 4), 0);
	int s = 0x3F;
	for (int i = NI - 1; i >= 0; i--) {
		const uint8_t cd = dec[size_t(i) * S + size_t(s)];
		if (vit_old ? cd : vit::vit_u(cd, s)) blk[size_t(i / 8)] |= uint8_t(1 << (i % 8));
		s = vit_old ? prv[size_t(i) * S + size_t(s)] : vit::vit_prev(cd, s);
	}
	Lv23Rx r;
	{ double sa = 0; for (double v : soft) sa += std::fabs(v); r.agree = sa > 0 ? -pm[0x3F] / sa : 0; }
	r.data.resize(size_t(kData4));
	for (int i = 0; i < kData4; i++) r.data[size_t(i)] = uint8_t(blk[size_t(i)] ^ kScr4[i]);
	std::vector<uint8_t> chk(r.data);
	chk.push_back(blk[size_t(kData4)]);
	r.ok = uint16_t(crc16_ccitt_reflected(chk.data(), chk.size()) ^ kCrcx4) == uint16_t(blk[size_t(kData4 + 1)] | blk[size_t(kData4 + 2)] << 8);
	r.status = uint8_t(blk[size_t(kData4)] ^ kStx4);
	return r;
}

}  // namespace fmodem::p3
