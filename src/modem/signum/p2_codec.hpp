// P2 short-cycle (unproto) packets, speed levels 1-4. C++17.
// Recovered from the reference firmware's behaviour: host-port pulse
// words for known text (GF(2) linear-algebra fit, every one of 1,521 captured
// packets reproduced bit-exactly) and the transmitted audio (pulse-shape
// least-squares fit). See the protocol description section 3.3.
//
//   info   = N data bytes + status + CRC-16 (X.25 reflected, little-endian)
//            N = 5 / 14 / 32 / 59 for levels 1-4 (DBPSK, DQPSK, 8DPSK, 16DPSK)
//   coded  = K=9 r=1/2 conv code (g0 0x171, g1 0x1EB, newest bit in the LSB),
//            8 zero tail bits; level 3 punctured to r=2/3, level 4 to r=7/8
//   chan   = per-level block interleaver; symbol label = Gray code of the
//            phase step index; level 2 complements the label bits
//   pulses = 9 header (level, counter parity) + 72 data + 1 tail per carrier,
//            nibble A on the leading carrier, B half a pulse later
#pragma once

#include "par.hpp"
#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>

#include "p3_long_tables.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "crc16.hpp"

namespace fmodem::p2 {

inline constexpr int kBitsPerSym[5] = {0, 1, 2, 3, 4};
// Data pulses per carrier = 8 rows x ppr pulses: ppr 9 = short packet (72
// pulses, 1.25 s cycle), ppr 40 = long packet (320 pulses, ARQ only).
constexpr int data_bytes(int level, int ppr = 9)
{
	return level == 1 ? ppr - 4 : level == 2 ? 2 * ppr - 4 : level == 3 ? 4 * ppr - 4 : 7 * ppr - 4;
}
inline constexpr int kDataBytes[5] = {0, data_bytes(1), data_bytes(2), data_bytes(3), data_bytes(4)};   // 5/14/32/59
constexpr int kInfoBits(int level, int ppr = 9) { return (data_bytes(level, ppr) + 3) * 8; }   // + status + CRC

// Header words (A nibble high, B nibble low) by level and counter parity.
inline constexpr uint8_t kHeader[5][2][9] = {
	{},
	{{0x6a, 0x22, 0x6a, 0x62, 0xa2, 0xa6, 0x62, 0xea, 0xa6}, {0x6a, 0x62, 0x6a, 0xaa, 0x22, 0x6e, 0xaa, 0x2a, 0x22}},
	{{0x6a, 0x62, 0xee, 0x2e, 0x2a, 0x62, 0x6a, 0xee, 0xaa}, {0x6a, 0x22, 0xa6, 0xa6, 0x2a, 0x2e, 0xe2, 0xe6, 0xaa}},
	{{0x6a, 0x2a, 0xa2, 0x6a, 0x62, 0x2a, 0x6e, 0x2e, 0xea}, {0x6a, 0xe2, 0x62, 0x6a, 0xea, 0xee, 0x6e, 0xae, 0x62}},
	{{0x6a, 0xe2, 0xe6, 0xee, 0xe2, 0xe2, 0xae, 0x6a, 0xea}, {0x6a, 0xa2, 0xae, 0x66, 0xe2, 0xae, 0x26, 0x62, 0xea}},
};
inline constexpr uint8_t kTail[5] = {0, 0xaa, 0xaa, 0x48, 0x19};

// Level 1: channel bit 18g+t carries coded bit kSeq1[t] + 2g.
inline constexpr int kSeq1[18] = {17, 1, 49, 33, 81, 65, 113, 97, 0, 129, 32, 16, 64, 48, 96, 80, 128, 112};
// Level 1 with 7 pulses per row (break-ins): channel bit 14g+t carries
// kSeq1bk[t] + 2g. Measured on 48 P3 break-ins (B1/A2) of the firmware, all
// decoding with Viterbi metric 0; NOT kSeq1 with the entries >= 112 removed
// (0 and 97 change places).
inline constexpr int kSeq1bk[14] = {17, 1, 49, 33, 81, 65, 0, 97, 32, 16, 64, 48, 96, 80};
// Levels 3/4: per-pulse coded-bit offsets [row class][carrier][label bit].
inline constexpr int kOff3[4][2][3] = {{{17, 20, 26}, {1, 4, 10}}, {{16, 22, 29}, {0, 6, 13}},
                                {{18, 25, 28}, {2, 9, 12}}, {{21, 24, 30}, {5, 8, 14}}};
inline constexpr int kOff4[2][2][4] = {{{15, 16, 20, 24}, {1, 2, 6, 10}}, {{14, 18, 23, 26}, {0, 4, 9, 12}}};

inline int parity(unsigned v) { return __builtin_popcount(v) & 1; }
inline unsigned gray(unsigned s) { return s ^ (s >> 1); }
inline unsigned gray_inv(unsigned g) { unsigned s = 0; for (; g; g >>= 1) s ^= g; return s; }

// Index into the UNPUNCTURED r=1/2 coded stream for data pulse 9g+p (g 0..7,
// p 0..8), carrier car (0 = A), label bit b (LSB first).
int coded_index(int level, int g, int p, int car, int b, int ppr = 9);

inline void conv_encode(const uint8_t *info, int nbits, uint8_t *coded)
{
	unsigned reg = 0;
	for (int i = 0; i < nbits + 8; i++) {
		const int b = i < nbits ? (info[i / 8] >> (i % 8)) & 1 : 0;
		reg = ((reg << 1) | unsigned(b)) & 0x1ff;
		coded[2 * i] = uint8_t(parity(reg & 0x171));
		coded[2 * i + 1] = uint8_t(parity(reg & 0x1EB));
	}
}

// Data pulse words for an information block (data_bytes(level, ppr) + 3
// bytes): 8 rows x ppr pulses.
std::vector<uint8_t> encode_data(int level, const uint8_t *info, int ppr = 9);

// Short packet: information block -> 82 pulse words (low bytes).
inline std::vector<uint8_t> encode(int level, const uint8_t *info, int counter)
{
	std::vector<uint8_t> w(kHeader[level][counter & 1], kHeader[level][counter & 1] + 9);
	const auto d = encode_data(level, info, 9);
	w.insert(w.end(), d.begin(), d.end());
	w.push_back(kTail[level]);
	return w;
}

inline void make_info(int level, const uint8_t *data, uint8_t status, uint8_t *info, int ppr = 9)
{
	const int n = data_bytes(level, ppr);
	std::memcpy(info, data, size_t(n));
	info[n] = status;
	const uint16_t c = crc16_ccitt_reflected(info, size_t(n + 1));
	info[n + 1] = uint8_t(c);
	info[n + 2] = uint8_t(c >> 8);
}

// The reference pulse, measured from the reference unit's transmitted audio: a root-raised-
// cosine with roll-off 1 for symbol period T = 10 ms, truncated at +-2T, at
// the nominal tone with constant phase. It matches the measured pulse to
// 0.95 %; with the 48.75-sample stagger below
// a whole reference-unit burst is reproduced to 0.8 % after its first 80 ms
// (the earlier 1.6T Hann approximation: 14 %, too coarse for the reference unit 16DPSK
// receiver). The burst onset (first ~50 ms) still differs (unexplained). Its centre lies 0.8T after the pulse's nominal start, so
// its leading tail begins 1.2T before it (kPreroll).
inline double rrc1(double x)                       // RRC, roll-off 1, symbol 1
{
	if (std::fabs(x) < 1e-9) return 4.0 / M_PI;
	if (std::fabs(std::fabs(x) - 0.25) < 1e-9)
		return (1 / std::sqrt(2.0)) * ((1 + 2 / M_PI) * std::sin(M_PI / 4) + (1 - 2 / M_PI) * std::cos(M_PI / 4));
	return 4 * x * std::cos(2 * M_PI * x) / (M_PI * x * (1 - 16 * x * x));
}
// Pulse table at sample rate fs: index i <-> time (i - 2T) from the centre,
// scaled so that a steady carrier (equal phases) has amplitude 1.
const std::vector<double> &pulse_table(float fs);
inline double preroll(float fs) { return 1.2 * 0.010 * fs; }

// Modulate pulse words. Each carrier's pulse k has constant phase at the
// nominal tone, stepped by -nibble/16 turn per pulse. Nibble A leads on lo
// (hi when swap); nibble B is half a pulse later. Pulse 0's nominal start is
// at txd_s + preroll(fs) into the returned audio.
//
// The burst can be made in pieces (ModPlan, modulate_range; scs-modem TODO: the one-core sender): a
// long packet's 3.4 s of audio took 33 ms on a Raspberry Pi 4 (73 ms at its governor's 700 MHz), made
// after the peer's answer ~40-55 ms before the packet's start, so the audio loop waited for it. In
// pieces its first 50 ms are ready in ~1 ms and the rest follows far ahead of the audio. Every sample
// is the same sum of the same terms in the same order (carrier A's pulses in order, then carrier B's,
// then the onset gate): the same numbers as made whole (modulate() is the one range [0, len)).
struct ModPlan {
	float fs = 0;
	double T = 0, stagger = 0, start0 = 0, norm = 0, A = 0, g0 = 0, gr = 0;
	size_t len = 0;
	bool gate = true;
	double f[2] = {0, 0};
	std::vector<double> ph[2];                       // each pulse's phase, per carrier (role)
};
ModPlan modulate_plan(const std::vector<uint8_t> &words, float fs, float lo, float hi, bool swap,
                             float amplitude, float txd_s = 0.020f);
// samples [a, b) of the burst into y (y holds the whole burst, zeroed; a range is made once, in order)
void modulate_range(const ModPlan &p, float *y, size_t a, size_t b);
inline std::vector<float> modulate(const std::vector<uint8_t> &words, float fs, float lo, float hi, bool swap,
                                   float amplitude, float txd_s = 0.020f)
{
	const ModPlan p = modulate_plan(words, fs, lo, hi, swap, amplitude, txd_s);
	std::vector<float> y(p.len, 0.0f);
	modulate_range(p, y.data(), 0, p.len);
	return y;
}

// ---- receiver -------------------------------------------------------------
// Joint least-squares estimate of every pulse's complex amplitude on both
// carriers, using the known pulse shape (pulse_table). Pulses overlap,
// so per-pulse filtering leaves inter-symbol interference that 16DPSK cannot
// tolerate; the joint fit removes it exactly. cA/cB: sample index of the
// first pulse's START on each carrier; n pulses per carrier. Returns 2n
// amplitudes (A then B), phase relative to the nominal tone.
//
// A long packet's normal equations in pieces (LsPre, scs-modem TODO o): the sums are made over
// `parts` sample ranges, each in sample order, and then added in order (below). The samples of a range
// already in can be summed while the packet still arrives (ls_pulses_pre, on a worker), by the same
// loop into the same zeroed arrays, stopped at a 256-sample block boundary (where the loop sets its
// carrier phasors afresh); ls_pulses then goes on from there instead of starting the range again.
// The same additions in the same order: the same numbers, so the same fit. Taken only when every
// input is the same (window length, timing, tones, pulse count, ranges); the decoding thread names it
// in ls_pre_tls. Real time only (the station).
struct LsPre {
	size_t xsize = 0; double fs = 0, cA = 0, cB = 0, fA = 0, fB = 0; int n = 0, parts = 0;
	std::vector<std::vector<double>> N, r;           // per range: its sums so far
	std::vector<long> upto;                          // per range: summed up to this sample (-1: nothing)
	mutable std::mutex m;
	bool same(size_t xs, double fs_, double a, double b, double fa, double fb, int n_) const
	{
		return xs == xsize && fs_ == fs && a == cA && b == cB && fa == fA && fb == fB && n_ == n;
	}
	bool matches(size_t xs, double fs_, double a, double b, double fa, double fb, int n_, int parts_) const
	{
		std::lock_guard<std::mutex> lk(m);
		return parts == parts_ && same(xs, fs_, a, b, fa, fb, n_);
	}
	// range `part`'s sums so far into Nq, rq; returns where they end (-1: none)
	long take(int part, std::vector<double> &Nq, std::vector<double> &rq) const
	{
		std::lock_guard<std::mutex> lk(m);
		if (part < 0 || part >= int(upto.size()) || upto[size_t(part)] < 0) return -1;
		Nq = N[size_t(part)]; rq = r[size_t(part)];
		return upto[size_t(part)];
	}
	std::atomic<long> taken{0};                      // samples a decode found summed (for the log)
};
inline thread_local LsPre *ls_pre_tls = nullptr;
// fill: sum (on this thread) every range of fill not yet summed whose samples x[0, avail) hold, and
// store it; nothing is solved and nothing returned. Otherwise the fit (ls_pulses).
std::vector<std::complex<double>> ls_pulses_core(const std::vector<float> &x, double fs, double cA, double cB,
                                                        double fA, double fB, int n, const std::vector<double> *ptab,
                                                        LsPre *fill, long avail);
inline std::vector<std::complex<double>> ls_pulses(const std::vector<float> &x, double fs, double cA, double cB,
                                                   double fA, double fB, int n, const std::vector<double> *ptab = nullptr)
{
	return ls_pulses_core(x, fs, cA, cB, fA, fB, n, ptab, nullptr, 0);
}
// The ranges of that fit whose samples are in (x[0, avail)), summed into `pre` (see LsPre).
inline void ls_pulses_pre(LsPre &pre, const std::vector<float> &x, double fs, double cA, double cB, double fA, double fB, int n, long avail)
{
	ls_pulses_core(x, fs, cA, cB, fA, fB, n, nullptr, &pre, avail);
}

struct RxResult { bool ok = false; int level = 0; std::vector<uint8_t> info; int metric = 0; };

// The reference decoders (the straightforward form; the fast ones below match them bit for bit).
// Hard-decision Viterbi over nbits+8 steps; erased[] marks punctured bits.
int viterbi_ref(const uint8_t *coded, const uint8_t *erased, int nbits, std::vector<uint8_t> &out);

// Soft-decision Viterbi: llr[j] > 0 favours coded bit 0, 0 = erased.
// Returns the decided path's hard disagreements (as the hard Viterbi).
int viterbi_soft_ref(const float *llr, int nbits, std::vector<uint8_t> &out);

// The fast decoders (fix 216): the same survivors, metrics and tie-breaks as the reference ones
// above, bit for bit. Each state ns has the two predecessors ns>>1 and
// (ns>>1)|0x80 with the input bit ns&1, so a step stores one choice bit per state (the reference
// kept a byte for the input and one for the predecessor) and the branch outputs come from a table.
// The reference visited the predecessors in ascending order with a strict comparison: the lower
// one wins a tie. On the Pi 4 a level-4 long packet's decode was ~3 of these (hard, again at the
// header's bias, the soft one), each the bulk of a 20 ms decode, against a ~27 ms answer budget.
inline const uint8_t *conv_out_table()
{
	static const auto t = [] { std::array<uint8_t, 512> v{}; for (unsigned r = 0; r < 512; r++) v[r] = uint8_t(parity(r & 0x171) | parity(r & 0x1EB) << 1); return v; }();
	return t.data();
}
inline void pack_choices(const uint8_t *f, uint64_t *c)
{
	for (int w = 0; w < 4; w++) { uint64_t v = 0; for (int k = 0; k < 64; k++) v |= uint64_t(f[64 * w + k]) << k; c[w] = v; }
}
int viterbi(const uint8_t *coded, const uint8_t *erased, int nbits, std::vector<uint8_t> &out);
// Soft-decision Viterbi: llr[j] > 0 favours coded bit 0, 0 = erased.
// Returns the decided path's hard disagreements (as the hard Viterbi).
int viterbi_soft(const float *llr, int nbits, std::vector<uint8_t> &out);

// Soft value of each Gray-coded bit of an M-PSK step: for bit b, the squared
// distance (in 1/16 turn) to the nearest symbol whose label has b = 1 minus
// that to the nearest with b = 0 (> 0 favours 0). `v` is the step minus the
// level's base, in 1/16 turn.
void psk_soft(double v, int k, unsigned comp, float *out);
inline bool soft_decoding()   // experiment
{
	static const bool on = [] { const bool v = std::getenv("P2_SOFT") != nullptr; if (v) std::fprintf(stderr, "[p2] soft-decision decoding\n"); return v; }();
	return on;
}

// Soft values of a packet's coded bits (index as coded_index; 0 = punctured).
std::vector<float> level_llr(int level, const double (*steps)[2], int ppr = 9, const double (*wt)[2] = nullptr);
// Decode soft values (one packet's, or the sum over its repeats: memory ARQ).
inline RxResult decode_llr(int level, const std::vector<float> &llr, int ppr = 9)
{
	RxResult r;
	r.level = level;
	r.metric = viterbi_soft(llr.data(), kInfoBits(level, ppr), r.info);
	const int n = data_bytes(level, ppr);
	r.ok = crc16_ccitt_reflected(r.info.data(), size_t(n + 1)) == uint16_t(r.info[size_t(n + 1)] | r.info[size_t(n + 2)] << 8);
	return r;
}

// steps[8*ppr][2]: received phase step per data pulse and carrier (A, B), in
// 1/16 turn relative to the nominal tone (any real value).
RxResult decode_steps(int level, const double (*steps)[2], int ppr = 9);

// Identify level and counter parity from the 8 header steps (words 1..8,
// [i][carrier] in 1/16 turn), estimating the per-pulse step bias that a
// frequency offset adds (6.25 Hz = 1/16 turn per pulse, which is a whole
// 16DPSK symbol, so it must come from the known header, not the data).
struct HeaderFit { int level = 0, parity = 0; double bias = 0, rms = 99; };
HeaderFit fit_header(const double h[8][2], int only_level = 0);

}  // namespace fmodem::p2
