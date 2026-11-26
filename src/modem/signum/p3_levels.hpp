// P3 speed levels 2-6: per-level carrier layouts, modulator and a
// generic least-squares demodulator (words <-> audio). Measured black-box
// from the reference firmware's DAC audio with every word known (the protocol description
// section 4.2). SL1 is the P2-style two-carrier format (p3_modem.hpp
// modulate_cs + p2_codec level 1); SL5 also has its own tuned code in
// p3_modem.hpp.
//
// Words: one per (pulse, slot); low nibble = phase step in 1/16 turn,
// z_p = z_(p-1) e^{-j 2 pi w / 16}. Carrier k = 480 + 120 k Hz.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <unordered_map>
#include <vector>

#include "p3_modem.hpp"

namespace fmodem::p3 {

struct Section { int npulses, n; int even[18]; int odd[18]; };
struct Layout {
	int level = 0;
	bool narrow = false;            // CS pulse (SL2) instead of the flat-top pulse
	int lag_from = 99;              // slots >= lag_from are staggered by kStagger (SL2)
	int nsec = 0;
	Section sec[2]{};
	int npulses() const { int s = 0; for (int i = 0; i < nsec; i++) s += sec[i].npulses; return s; }
	int nwords() const { int s = 0; for (int i = 0; i < nsec; i++) s += sec[i].npulses * sec[i].n; return s; }
	const Section &section(int p, int &first) const
	{
		first = 0;
		for (int i = 0; i < nsec; i++) { if (p < first + sec[i].npulses) return sec[i]; first += sec[i].npulses; }
		return sec[nsec - 1];
	}
	int carriers(int p) const { int f; return section(p, f).n; }
	int carrier(int p, int j, bool odd) const { int f; const auto &s = section(p, f); return odd ? s.odd[j] : s.even[j]; }
	int word_index(int p, int j) const
	{
		int idx = 0, first = 0;
		for (int i = 0; i < nsec; i++) {
			if (p < first + sec[i].npulses) return idx + (p - first) * sec[i].n + j;
			idx += sec[i].npulses * sec[i].n; first += sec[i].npulses;
		}
		return -1;
	}
	bool lagging(int j) const { return j >= lag_from; }
};
constexpr double kStagger = 48.75 / 96.0;          // fraction of T (as P2)

// 14 carriers 720..2280 (k 2..15); repeat: half rotation.
#define P3_D14_EVEN {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}
#define P3_D14_ODD {9, 10, 11, 12, 13, 14, 15, 2, 3, 4, 5, 6, 7, 8}
#define P3_D16_EVEN {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}
#define P3_D16_ODD {16, 9, 10, 11, 12, 13, 14, 15, 2, 3, 4, 5, 6, 7, 8, 1}
#define P3_D18_EVEN {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17}
#define P3_D18_ODD {17, 16, 9, 10, 11, 12, 13, 14, 15, 2, 3, 4, 5, 6, 7, 8, 1, 0}

const Layout &layout(int level);
// Long-cycle packets (330 pulses): the same carriers per level, 320 data
// pulses instead of 72 (word counts measured on unproto modes 37-41).
const Layout &layout_long(int level);

inline double lv_pulse(const Layout &L, double t) { return L.narrow ? cs_pulse(t) : double(pulse(t)); }
inline double lv_half(const Layout &L) { return L.narrow ? 2 * kT : kPulseHalfS; }

// Words -> audio; pulse p (leading slots) centred at lead_s + p T.
std::vector<float> modulate_lv(int level, const std::vector<uint8_t> &words, double fs, bool odd, float amplitude, double lead_s = 0.03, bool lng = false);

// demod_lv's normal matrix without building its columns (speed, for the
// Raspberry Pi; 2026-10-19). A column is u(i) = g(i - I) e^{-j w i}, the real
// pair (Re u, Im u); g is one of two pulse templates (leading or staggered
// slots: the pulse period is a whole number of samples), so every inner
// product is a table sum over the template overlap times a phase factor:
// sum u u'* = e^{-j(w-w')I} T(dk), sum u u' = e^{-j(w+w')I} T(ks). Same
// columns, same order, same factorisation as demod_lv up to rounding.
// Declines (nullopt) when a column would be cut at the buffer's ends or the
// period is not whole; P3_LV_SLOW forces the old path.
std::optional<std::vector<std::vector<std::complex<double>>>> demod_lv_fast(const Layout &L, int np, const float *x, long n, double fs, double t0, bool odd, double df, double *resid);

// Generic joint LS demodulation (any layout, per-carrier offsets): returns
// z[p][slot] for pulses 0..np-1 (slot order of `odd`), t0 = centre of pulse
// 0's leading slots (samples).
std::vector<std::vector<std::complex<double>>> demod_lv(int level, const float *x, long n, double fs, double t0, bool odd, double df = 0, double *resid = nullptr, int p1 = -1, bool lng = false);

// Phase steps (1/16 turn, word values) from demod_lv output: steps[p][j], p >= 1.
double lv_step(const std::vector<std::vector<std::complex<double>>> &z, const Layout &L, int p, int j, bool odd);

}  // namespace fmodem::p3

namespace fmodem::p3 {

// Fast LS demodulation for layouts whose carriers all switch together
// (SL3, SL4, SL5 sections): same structured normal matrix as p3::demod.
// Returns z[p][k] (k = carrier index 0..17, zero where unused).
// Plan / run split as p3::demod (fix 152): everything but the right-hand side
// depends only on the timing, the offset and the layout.
struct FlatPlan {
	bool ok = false; int level = 0, p1 = -1; bool lng = false; double fs = 0, t0 = 0, df = 0;
	int Tn = 0, np = 0, Lg = 0, NC = 0, N = 0, bw = 0; long base = 0;
	std::vector<double> g, A; std::vector<int> col_r, col_k;
};
FlatPlan demod_flat_plan(int level, long n, double fs, double t0, double df = 0, int p1 = -1, bool lng = false);
// The run in pieces while the packet arrives (a long packet: 3.3 s): the right-hand side of every
// pulse whose samples are all in (x[0..n_avail)), and the forward substitution as far as those rows
// allow. The same operations per column and per y[i], in the same order, as demod_flat_run in one go
// (columns are independent; the forward substitution is by columns, k ascending), so finishing a
// FlatProgress in demod_flat_run gives bit for bit the same result, provided x[i] is the same for every
// i used before (the caller's part: the same window start and the same audio).
struct FlatProgress { int cols = 0, k = 0; std::vector<double> y; };
// The back substitution (rows, j ascending: as it always was).
inline void ls_backward(const double *A, int N, int bw, double *y)
{
	for (int i = N - 1; i >= 0; i--) { for (int j = i + 1; j < std::min(N, i + bw); j++) y[size_t(i)] -= A[size_t(i) * size_t(bw) + size_t(j - i)] * y[size_t(j)]; y[size_t(i)] /= A[size_t(i) * size_t(bw)]; }
}
inline bool ls_slow() { static const bool slow = std::getenv("P3_LS_SLOW") != nullptr; return slow; }   // test: the forms before fix 216b
void demod_flat_advance(const FlatPlan &P, const float *x, long n_avail, FlatProgress &g);
std::vector<std::array<std::complex<double>, 18>> demod_flat_run(const FlatPlan &P, const float *x, long n, double *resid = nullptr, FlatProgress *prog = nullptr);
inline std::vector<std::array<std::complex<double>, 18>> demod_flat(int level, const float *x, long n, double fs, double t0, double df = 0, double *resid = nullptr, int p1 = -1, bool lng = false)
{
	return demod_flat_run(demod_flat_plan(level, n, fs, t0, df, p1, lng), x, n, resid);
}

}  // namespace fmodem::p3
