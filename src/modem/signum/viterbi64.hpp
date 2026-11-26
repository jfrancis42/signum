// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// One step of the 64-state (K=7) Viterbi decoders of P3 (short SL2-SL6, long SL2-SL6), done for all
// states at once with 16-byte vectors (NEON on ARM, SSE2 on x86; GCC vector extensions, no OS calls,
// no heap). Bit for bit the decoders' former loops:
//
//   for st = 0..63, if !(pm[st] >= skip): for each allowed u: reg = (st << 1 | u) & 0x7F, ns = reg & 0x3F,
//     m = pm[st] - ta(reg) - tb(reg); if (m < npm[ns]) take it        (npm[] starting at `init`)
//
// The next state ns has u = ns & 1 and two predecessors, st0 = ns >> 1 (reg = ns) and st1 = st0 | 32
// (reg = ns | 64), met in that order by the loop above (st ascending), so ns takes st0 when its metric
// is below `init` and then st1 when below that: the same comparisons, in the same order, on the same
// values. Both generator masks of every decoder here have bit 6 set, so st1's branch outputs are st0's
// complemented. ta(reg) is the decoder's own form, kept exactly:
//   Sel: (parity ? s0 : -s0)            Mul: (parity ? 1 : -1) * s0  (the lv23 / lv4 decoders)
// Per state the step returns a code instead of the decoder's arrays: 0 nothing taken (those arrays
// kept their zero), 1 st0, 2 st1; vit_prev() / vit_u() turn it back into what they held.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace fmodem::vit {

// std::fmod(v, 16.0) for the decoders' phase distances, without the library call: for 0 <= v < 1e15,
// v * 0.0625 is the exact quotient (a power of two), its floor the exact integer part, and
// v - 16 floor(v / 16) the exact remainder, which fmod returns and which is representable, so the
// subtraction (fused or not) gives it exactly. Anything else (negative, huge, NaN): fmod itself.
// P3_VIT_OLD=1: fmod always.
inline double mod16(double v)
{
	static const bool old = std::getenv("P3_VIT_OLD") != nullptr;
	if (!old && v >= 0 && v < 1e15) return v - 16.0 * std::floor(v * 0.0625);
	return std::fmod(v, 16.0);
}

enum class Form { Sel, Mul };

// parity masks over the 64 next states, for st0's branch (reg = ns)
constexpr uint64_t parity_mask(unsigned poly)
{
	uint64_t m = 0;
	for (unsigned ns = 0; ns < 64; ns++) if (__builtin_popcount(ns & poly) & 1) m |= uint64_t(1) << ns;
	return m;
}

inline int vit_prev(uint8_t code, int ns) { return code == 0 ? 0 : code == 1 ? (ns >> 1) : ((ns >> 1) | 32); }
inline int vit_u(uint8_t code, int ns) { return code == 0 ? 0 : (ns & 1); }

template <class T> struct VecOf;
template <> struct VecOf<float> { typedef float v __attribute__((vector_size(16))); typedef int32_t m __attribute__((vector_size(16))); static constexpr int W = 4; };
template <> struct VecOf<double> { typedef double v __attribute__((vector_size(16))); typedef int64_t m __attribute__((vector_size(16))); static constexpr int W = 2; };

// The scalar form (reference for the vector one; used where vectors do not help).
template <class T, Form F>
inline void acs_scalar(const T *pm, T *npm, uint8_t *code, T s0, T s1, uint64_t pa, uint64_t pb, bool u0, bool u1, T init, T skip)
{
	for (int ns = 0; ns < 64; ns++) {
		const int u = ns & 1;
		T cur = init; uint8_t c = 0;
		if (u ? u1 : u0) {
			const bool a = (pa >> ns) & 1, b = (pb >> ns) & 1;
			const int st0 = ns >> 1, st1 = st0 | 32;
			T ta0, tb0, ta1, tb1;
			if (F == Form::Sel) { ta0 = a ? s0 : -s0; tb0 = b ? s1 : -s1; ta1 = !a ? s0 : -s0; tb1 = !b ? s1 : -s1; }
			else { ta0 = T(a ? 1 : -1) * s0; tb0 = T(b ? 1 : -1) * s1; ta1 = T(!a ? 1 : -1) * s0; tb1 = T(!b ? 1 : -1) * s1; }
			if (!(pm[st0] >= skip)) { const T m = pm[st0] - ta0 - tb0; if (m < cur) { cur = m; c = 1; } }
			if (!(pm[st1] >= skip)) { const T m = pm[st1] - ta1 - tb1; if (m < cur) { cur = m; c = 2; } }
		}
		npm[ns] = cur; code[ns] = c;
	}
}

// Per-lane constants of one decoder (its two generator masks), made once.
template <class T, Form F>
struct AcsTables {
	typedef typename VecOf<T>::v V; typedef typename VecOf<T>::m M;
	static constexpr int W = VecOf<T>::W, NB = 32 / W;
	// [u][block]: per lane (q = block * W + lane, ns = 2q + u) the sign choice of st0's branch outputs
	M ma[2][NB], mb[2][NB];
	uint64_t pa = 0, pb = 0;
	AcsTables(uint64_t pa_, uint64_t pb_) : pa(pa_), pb(pb_)
	{
		for (int u = 0; u < 2; u++)
			for (int k = 0; k < NB; k++)
				for (int l = 0; l < W; l++) {
					const int ns = 2 * (k * W + l) + u;
					ma[u][k][l] = ((pa >> ns) & 1) ? -1 : 0;
					mb[u][k][l] = ((pb >> ns) & 1) ? -1 : 0;
				}
	}
};

template <class V, class M> inline V vsel(M m, V a, V b) { return (V)((m & (M)a) | (~m & (M)b)); }

// Interleave two vectors of states (even, odd) into memory, and their codes as bytes.
template <class T> struct inter;
template <> struct inter<float> {
	typedef VecOf<float>::v V; typedef VecOf<float>::m M;
	typedef uint8_t B8 __attribute__((vector_size(8)));
	static void store(float *p, uint8_t *c, V e, V o, M ce, M co)
	{
		const V a = __builtin_shufflevector(e, o, 0, 4, 1, 5), b = __builtin_shufflevector(e, o, 2, 6, 3, 7);
		std::memcpy(p, &a, sizeof a); std::memcpy(p + 4, &b, sizeof b);
		typedef uint8_t B16 __attribute__((vector_size(16)));
		const B16 be = (B16)ce, bo = (B16)co;            // codes 0..2: the low byte of each lane
		const B8 cc = __builtin_shufflevector(be, bo, 0, 16, 4, 20, 8, 24, 12, 28);
		std::memcpy(c, &cc, 8);
	}
};
template <> struct inter<double> {
	typedef VecOf<double>::v V; typedef VecOf<double>::m M;
	static void store(double *p, uint8_t *c, V e, V o, M ce, M co)
	{
		const V a = __builtin_shufflevector(e, o, 0, 2), b = __builtin_shufflevector(e, o, 1, 3);
		std::memcpy(p, &a, sizeof a); std::memcpy(p + 2, &b, sizeof b);
		c[0] = uint8_t(ce[0]); c[1] = uint8_t(co[0]); c[2] = uint8_t(ce[1]); c[3] = uint8_t(co[1]);
	}
};

// One step for all 64 states. pm, npm: 64 values each (16-byte aligned not required).
template <class T, Form F>
inline void acs_step(const AcsTables<T, F> &tb, const T *pm, T *npm, uint8_t *code, T s0, T s1, bool u0, bool u1, T init, T skip)
{
	typedef typename VecOf<T>::v V; typedef typename VecOf<T>::m M;
	constexpr int W = VecOf<T>::W, NB = 32 / W;
	V vs0, vs1, vn0, vn1, vinit, vskip, vone, vmone;
	for (int l = 0; l < W; l++) { vs0[l] = s0; vs1[l] = s1; vn0[l] = -s0; vn1[l] = -s1; vinit[l] = init; vskip[l] = skip; vone[l] = T(1); vmone[l] = T(-1); }
	// st1's branch outputs are st0's complemented: the sign flipped (Sel), the factor negated (Mul)
	auto one = [&](const V &p0, const V &p1, const M &ok0, const M &ok1, const M &a, const M &b, V &res, M &cd) {
		V ta0, tb0, ta1, tb1;
		if (F == Form::Sel) { ta0 = vsel(a, vs0, vn0); tb0 = vsel(b, vs1, vn1); ta1 = vsel(a, vn0, vs0); tb1 = vsel(b, vn1, vs1); }
		else { ta0 = vsel(a, vone, vmone) * vs0; tb0 = vsel(b, vone, vmone) * vs1; ta1 = vsel(a, vmone, vone) * vs0; tb1 = vsel(b, vmone, vone) * vs1; }
		const V m0 = p0 - ta0 - tb0, m1 = p1 - ta1 - tb1;
		const M t0 = ok0 & (M)(m0 < vinit);
		const V c0 = vsel(t0, m0, vinit);
		const M t1 = ok1 & (M)(m1 < c0);
		res = vsel(t1, m1, c0);
		cd = (t1 & 2) | (~t1 & t0 & 1);
	};
	for (int k = 0; k < NB; k++) {
		V p0, p1;
		std::memcpy(&p0, pm + k * W, sizeof p0);
		std::memcpy(&p1, pm + 32 + k * W, sizeof p1);
		const M ok0 = ~(M)(p0 >= vskip), ok1 = ~(M)(p1 >= vskip);
		V re = vinit, ro = vinit; M ce = M{}, co = M{};
		if (u0) one(p0, p1, ok0, ok1, tb.ma[0][k], tb.mb[0][k], re, ce);
		if (u1) one(p0, p1, ok0, ok1, tb.ma[1][k], tb.mb[1][k], ro, co);
		// states 2q (u = 0) and 2q + 1 (u = 1) side by side again
		inter<T>::store(npm + 2 * k * W, code + 2 * k * W, re, ro, ce, co);
	}
}

}  // namespace fmodem::vit
