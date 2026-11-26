// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// The functions of p3_long.hpp (out of the header, so that they build in parallel).
#include "p3_long.hpp"

namespace fmodem::p3::lchain {

std::vector<uint8_t> encode(const std::vector<uint8_t> &in, int level)
{
	const uint32_t mask = level == 5 ? 0xDu : level == 6 ? 0x1B366u : 0u;
	uint32_t a = mask;
	auto keep = [&]() -> bool {
		if (!mask) return true;
		for (;;) {
			const bool c = a & 1u;
			a >>= 1;
			if (a) return c;
			a = mask;
		}
	};
	std::vector<uint8_t> out;
	unsigned st = 0, acc = 0, nacc = 0;
	for (uint8_t b : in)
		for (int i = 0; i < 8; i++) {
			st = ((st << 1) | ((b >> i) & 1u)) & 0x7F;
			const unsigned t = unsigned(__builtin_popcount(st & 0x4F) & 1) | unsigned(__builtin_popcount(st & 0x6D) & 1) << 1;
			for (int k = 0; k < 2; k++)
				if (keep()) {
					acc |= ((t >> k) & 1u) << nacc;
					if (++nacc == 8) { out.push_back(uint8_t(acc)); acc = 0; nacc = 0; }
				}
		}
	return out;
}

std::vector<uint8_t> interleave(const std::vector<uint8_t> &in, int I, int P)
{
	std::vector<uint8_t> out(size_t(I), 0);
	int col = 8 * I - P, p = col;
	for (int n = 0; n < 8 * I; n++) {
		const unsigned bit = size_t(n >> 3) < in.size() ? (in[size_t(n >> 3)] >> (n & 7)) & 1u : 0u;
		if (bit) out[size_t(p >> 3)] |= uint8_t(1u << (p & 7));
		if (p - P >= 0) p -= P;
		else p = ++col;
	}
	return out;
}

std::vector<uint8_t> channel(int level, bool lng, const uint8_t *data, int payload, uint8_t status)
{
	const Chain c = chain(level, lng);
	std::vector<uint8_t> info(size_t(c.E), 0);
	std::memcpy(info.data(), data, size_t(payload));
	info[size_t(payload)] = status;
	const uint16_t crc = crc16_ccitt_reflected(info.data(), size_t(payload) + 1);
	info[size_t(payload) + 1] = uint8_t(crc);
	info[size_t(payload) + 2] = uint8_t(crc >> 8);
	const auto &T = crc_table();
	for (int k = 0; 2 * k + 1 < payload + 3 + 1; k++) {
		if (2 * k < payload + 3) info[size_t(2 * k)] ^= uint8_t(T[size_t(k & 0xFF)] >> 8);
		if (2 * k + 1 < payload + 3) info[size_t(2 * k + 1)] ^= uint8_t(T[size_t(k & 0xFF)]);
	}
	info[size_t(payload) + 3] = 0;                   // sent as 0x00
	auto out = interleave(encode(info, level), c.I, c.P);
	for (int k = 0; k < 4; k++) out.push_back(data[k]);   // read past the buffer (SL5 long)
	return out;
}

}  // namespace fmodem::p3::lchain

namespace fmodem::p3 {

std::vector<uint8_t> long_encode(int level, const uint8_t *data, uint8_t status)
{
	const lng::Level &L = lng::level(level);
	std::vector<uint8_t> w(L.tmpl[status & 1], L.tmpl[status & 1] + L.nwords);
	if (level == 1) {                                // P2 level-1 long (K=9), measured model
		std::vector<uint8_t> info(size_t(L.payload) + 3);
		std::memcpy(info.data(), data, size_t(L.payload));
		info[size_t(L.payload)] = status;
		const uint16_t c = crc16_ccitt_reflected(info.data(), size_t(L.payload) + 1);
		info[size_t(L.payload) + 1] = uint8_t(c);
		info[size_t(L.payload) + 2] = uint8_t(c >> 8);
		int maxidx = 0;
		for (int i = 0; i < L.nbits; i++) if (l1_coded(i) > maxidx) maxidx = l1_coded(i);
		const int nsteps = maxidx / 2 + 2, ninfo = int(info.size()) * 8;
		std::vector<uint8_t> coded(size_t(2 * nsteps));
		const unsigned mask = (1u << L.kbits) - 1;
		unsigned reg = 0;
		for (int i = 0; i < nsteps; i++) {
			const unsigned b = i < ninfo ? (info[size_t(i / 8)] >> (i % 8)) & 1u : 0u;
			reg = ((reg << 1) | b) & mask;
			coded[size_t(2 * i)] = uint8_t(__builtin_popcount(reg & L.g0) & 1);
			coded[size_t(2 * i + 1)] = uint8_t(__builtin_popcount(reg & L.g1) & 1);
		}
		auto chbit = [&](int i) -> unsigned { const int ci = l1_coded(i); return unsigned((ci >= 0 ? coded[size_t(ci)] : 0) ^ l1_comp(i)); };
		for (int j = 0; j < L.ndata; j++) {
			uint8_t &x = w[L.pos[j]];
			x = uint8_t((x & ~0x88u) | chbit(2 * j) << 7 | chbit(2 * j + 1) << 3);
		}
		return w;
	}
	// Words per header pulse / per data pulse (header: 9 pulses; data: 320 pulses,
	// at SL5 one more: the leak, p3_long.hpp).
	static const int kHdr[7] = {0, 0, 6, 14, 16, 18, 18}, kNc[7] = {0, 0, 6, 14, 14, 16, 18};
	const auto ch = lchain::channel(level, true, data, L.payload, status);
	auto bit = [&](int k) -> unsigned { return (ch[size_t(k >> 3)] >> (k & 7)) & 1u; };
	const int nc = kNc[level], first = 9 * kHdr[level];
	const int npulse = level == 5 ? 321 : 320;       // SL5 long: one pulse too many (the leak)
	for (int j = 0; j < npulse * nc; j++) {
		uint8_t &x = w[size_t(first + j)];
		if (level <= 3) x = uint8_t(0x02 | bit(j) << 3);
		else x = kQpsk[bit(2 * j) | bit(2 * j + 1) << 1];
	}
	return w;
}

LongRx long1_decode(const double *nib)
{
	LongRx r;
	const lng::Level &L = lng::level(1);
	auto dist = [](double a, double b) { double d = vit::mod16(a - b + 64.0); return d > 8 ? 16 - d : d; };
	int maxidx = 0;
	for (int i = 0; i < L.nbits; i++) if (l1_coded(i) > maxidx) maxidx = l1_coded(i);
	const int nsteps = maxidx / 2 + 2, ninfo = (L.payload + 3) * 8;
	std::vector<float> soft(size_t(2 * nsteps), 0.f);     // > 0: coded bit 1
	for (int j = 0; j < L.ndata; j++) {
		const int w = L.pos[j];
		const uint8_t t = L.tmpl[0][w];                   // the template's low 3 bits per nibble (both parities alike there)
		for (int b = 0; b < 2; b++) {
			const int i = 2 * j + b;
			if (l1_coded(i) < 0) continue;
			const double v = nib[2 * w + b];
			const int base = b == 0 ? (t >> 4) & 7 : t & 7;
			float s = float(dist(v, base) - dist(v, base + 8));   // > 0: nibble bit 3 set
			if (l1_comp(i)) s = -s;
			soft[size_t(l1_coded(i))] += s;
		}
	}
	const unsigned mask = (1u << L.kbits) - 1;
	const int S = 1 << (L.kbits - 1);
	std::vector<float> pm(static_cast<size_t>(S), 1e30f), npm(static_cast<size_t>(S));
	pm[0] = 0;
	std::vector<uint32_t> prv(size_t(nsteps) * size_t(S));
	for (int i = 0; i < nsteps; i++) {
		std::fill(npm.begin(), npm.end(), 1e30f);
		const int umax = i < ninfo ? 2 : 1;
		for (int st = 0; st < S; st++) {
			if (pm[size_t(st)] >= 1e29f) continue;
			for (int u = 0; u < umax; u++) {
				const unsigned reg = ((unsigned(st) << 1) | unsigned(u)) & mask;
				const int ns = int(reg & unsigned(S - 1));
				const float m = pm[size_t(st)] - ((__builtin_popcount(reg & L.g0) & 1) ? soft[size_t(2 * i)] : -soft[size_t(2 * i)])
				                - ((__builtin_popcount(reg & L.g1) & 1) ? soft[size_t(2 * i + 1)] : -soft[size_t(2 * i + 1)]);
				if (m < npm[size_t(ns)]) { npm[size_t(ns)] = m; prv[size_t(i) * size_t(S) + size_t(ns)] = uint32_t(st) << 1 | uint32_t(u); }
			}
		}
		pm.swap(npm);
	}
	std::vector<uint8_t> info(size_t(L.payload + 3), 0);
	int st = 0;
	for (int i = nsteps - 1; i >= 0; i--) {
		const uint32_t v = prv[size_t(i) * size_t(S) + size_t(st)];
		if ((v & 1) && i < ninfo) info[size_t(i >> 3)] |= uint8_t(1u << (i & 7));
		st = int(v >> 1);
	}
	const uint16_t crc = crc16_ccitt_reflected(info.data(), size_t(L.payload) + 1);
	r.ok = crc == uint16_t(info[size_t(L.payload) + 1] | info[size_t(L.payload) + 2] << 8);
	r.status = info[size_t(L.payload)];
	r.data.assign(info.begin(), info.begin() + L.payload);
	return r;
}

LongRx long_decode(int level, const double *steps, const double *wt)
{
	LongRx r;
	if (level < 2 || level > 6) return r;
	const lng::Level &L = lng::level(level);
	const lchain::Chain c = lchain::chain(level, true);
	static const int kHdr[7] = {0, 0, 6, 14, 16, 18, 18}, kNc[7] = {0, 0, 6, 14, 14, 16, 18};
	const int first = 9 * kHdr[level], nb = 8 * c.I;
	auto dist = [](double a, double b) { double d = vit::mod16(a - b + 64.0); return d > 8 ? 16 - d : d; };
	// soft channel bits, > 0: bit 1
	std::vector<float> ch(size_t(nb), 0.f);
	if (level <= 3) {
		for (int k = 0; k < nb; k++) { const double v = steps[first + k]; ch[size_t(k)] = float((dist(v, 2) - dist(v, 10)) * (wt ? wt[first + k] : 1.0)); }
	} else {
		(void)kNc;
		for (int j = 0; 2 * j + 1 < nb; j++) {
			const double v = steps[first + j];
			double d[4];
			for (int q = 0; q < 4; q++) d[q] = dist(v, double(kQpsk[q]));
			for (int b = 0; b < 2; b++) {
				double m0 = 1e9, m1 = 1e9;
				for (int q = 0; q < 4; q++) { double &m = ((q >> b) & 1) ? m1 : m0; m = std::min(m, d[q]); }
				ch[size_t(2 * j + b)] = float((m0 - m1) * (wt ? wt[first + j] : 1.0));
			}
		}
	}
	// interleaver: coded bit n went to channel bit p(n)
	std::vector<float> coded(size_t(nb), 0.f);
	{
		int col = nb - c.P, p = col;
		for (int n = 0; n < nb; n++) {
			coded[size_t(n)] = ch[size_t(p)];
			if (p - c.P >= 0) p -= c.P;
			else p = ++col;
		}
	}
	// puncturing: which coded index each (step, output) took (-1: not sent)
	const int NI = 8 * c.E;
	const int known0 = 8 * (L.payload + 3);          // input bits from here on are zero
	std::vector<int> idx(size_t(2 * NI), -1);
	{
		const uint32_t mask = level == 5 ? 0xDu : level == 6 ? 0x1B366u : 0u;
		uint32_t a = mask;
		int kept = 0;
		for (int i = 0; i < 2 * NI; i++) {
			bool keep = true;
			if (mask) {
				for (;;) { const bool cbit = a & 1u; a >>= 1; if (a) { keep = cbit; break; } a = mask; }
			}
			if (keep) { idx[size_t(i)] = kept < nb ? kept : -1; kept++; }
		}
	}
	// K=7 Viterbi, all 64 states per step at once (viterbi64.hpp; P3_VIT_OLD=1: the loop it replaces)
	constexpr int S = 64;
	std::vector<float> pm(S, 1e30f), npm(S);
	pm[0] = 0;
	std::vector<uint8_t> prv(size_t(NI) * S);
	static const bool vit_old = std::getenv("P3_VIT_OLD") != nullptr;
	static const vit::AcsTables<float, vit::Form::Sel> acs(vit::parity_mask(0x4F), vit::parity_mask(0x6D));
	for (int i = 0; i < NI; i++) {
		const float s0 = idx[size_t(2 * i)] >= 0 ? coded[size_t(idx[size_t(2 * i)])] : 0.f;
		const float s1 = idx[size_t(2 * i + 1)] >= 0 ? coded[size_t(idx[size_t(2 * i + 1)])] : 0.f;
		const int umax = i < known0 ? 2 : 1;
		if (!vit_old) { vit::acs_step(acs, pm.data(), npm.data(), &prv[size_t(i) * S], s0, s1, true, umax > 1, 1e30f, 1e29f); pm.swap(npm); continue; }
		std::fill(npm.begin(), npm.end(), 1e30f);
		for (int st = 0; st < S; st++) {
			if (pm[size_t(st)] >= 1e29f) continue;
			for (int u = 0; u < umax; u++) {
				const unsigned reg = ((unsigned(st) << 1) | unsigned(u)) & 0x7F;
				const int ns = int(reg & 0x3F);
				const float m = pm[size_t(st)] - ((__builtin_popcount(reg & 0x4F) & 1) ? s0 : -s0) - ((__builtin_popcount(reg & 0x6D) & 1) ? s1 : -s1);
				if (m < npm[size_t(ns)]) { npm[size_t(ns)] = m; prv[size_t(i) * S + size_t(ns)] = uint8_t(st << 1 | u); }
			}
		}
		pm.swap(npm);
	}
	std::vector<uint8_t> info(size_t(c.E), 0);
	int st = 0;
	r.metric = pm[0];
	for (int i = NI - 1; i >= 0; i--) {
		const uint8_t v = prv[size_t(i) * S + size_t(st)];
		const int u = vit_old ? (v & 1) : vit::vit_u(v, st);
		if (u) info[size_t(i >> 3)] |= uint8_t(1u << (i & 7));
		st = vit_old ? (v >> 1) : vit::vit_prev(v, st);
	}
	const int n3 = L.payload + 3;
	const auto &T = lchain::crc_table();
	for (int k = 0; 2 * k + 1 < n3 + 1; k++) {
		if (2 * k < n3) info[size_t(2 * k)] ^= uint8_t(T[size_t(k & 0xFF)] >> 8);
		if (2 * k + 1 < n3) info[size_t(2 * k + 1)] ^= uint8_t(T[size_t(k & 0xFF)]);
	}
	const uint16_t crc = crc16_ccitt_reflected(info.data(), size_t(L.payload) + 1);
	r.ok = crc == uint16_t(info[size_t(L.payload) + 1] | info[size_t(L.payload) + 2] << 8);
	r.status = info[size_t(L.payload)];
	r.data.assign(info.begin(), info.begin() + L.payload);
	return r;
}

}  // namespace fmodem::p3
