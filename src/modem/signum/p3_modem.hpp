// P3 2400 Bd level: 18/16-carrier DQPSK modulator and least-squares
// demodulator. C++17, measured from the reference firmware's
// audio (the protocol description section 4.3).
//
// Packet = 82 pulses, T = 10 ms, all carriers switching together:
//   pulses 0-8   header: 18 carriers, 480 + 120 k Hz, words 18p .. 18p+17
//                (the first two words are the 0a 0a lead)
//   pulses 9-81  data + tail: 16 carriers, 600 + 120 k Hz, words 162 + 16(p-9)
// Each word's low nibble is a phase step in 1/16 turn applied to its carrier:
// z_p = z_(p-1) * exp(-j 2 pi w / 16); a carrier that continues from the
// header into the data keeps its phase. Word slot -> carrier depends on the
// transmission: the first ("even") goes up in frequency, the repeat ("odd")
// uses a fixed permutation (kOdd16/kOdd18).
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "p2_codec.hpp"
#include "p3_codec.hpp"
#include "p3_pulse.hpp"

namespace fmodem::p3 {

// r[j] -= u * k[j], j = 0..n-1 (the banded Cholesky's row update and the forward substitution's
// column update): every element alone, so two at a time with 16-byte vectors is the same operation on
// each (a multiply and a subtract, fused or not as the compiler makes the scalar form: on ARM both are
// fused, on x86 neither). P3_LS_OLD=1: one at a time.
inline bool ls_old() { static const bool v = std::getenv("P3_LS_OLD") != nullptr; return v; }
void ls_axmy(double *r, const double *k, double u, int n);
// The banded Cholesky factor in place (A = U^T U, row i: A[i bw + (j - i)]): row by row, each row
// updated from the rows above it in ascending order (ls_axmy), then divided by its pivot.
void ls_chol(double *A, int N, int bw);

constexpr int kHdrPulses = 9, kHdrCarriers = 18, kAllPulses = 82;
constexpr int kHdrWords = kHdrPulses * kHdrCarriers;   // 162 (2 lead + 160)
constexpr double kT = 0.010;

// Carrier index k: frequency 480 + 120 k Hz, k = 0..17 (data uses 1..16).
inline constexpr int kOdd18[18] = {17, 16, 9, 10, 11, 12, 13, 14, 15, 2, 3, 4, 5, 6, 7, 8, 1, 0};
inline constexpr int kOdd16[16] = {16, 9, 10, 11, 12, 13, 14, 15, 2, 3, 4, 5, 6, 7, 8, 1};
inline double carrier_hz(int k) { return 480.0 + 120.0 * k; }
inline int pulse_carriers(int p) { return p < kHdrPulses ? kHdrCarriers : kCarriers; }
// The word slots' carriers of a pulse: n slots (18 in the header, 16 after), slot j on carrier k[j].
// One table per pulse kind and map, each with room for 18, so a loop over j < n stays inside it (GCC
// 14 at -O3 warned that slot_carrier's kOdd16[j] could be read at j = 16 in p3::modulate: it could
// not tie the loop's bound, pulse_carriers(p), to the branch on p; every caller keeps j below the
// pulse's carrier count, so no read was ever out of range. Same carriers as before for every p, j.)
struct SlotMap { int n; int k[kHdrCarriers]; };
inline constexpr SlotMap kSlotHdrEven = {kHdrCarriers, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17}};
inline constexpr SlotMap kSlotHdrOdd = {kHdrCarriers, {17, 16, 9, 10, 11, 12, 13, 14, 15, 2, 3, 4, 5, 6, 7, 8, 1, 0}};      // kOdd18
inline constexpr SlotMap kSlotDataEven = {kCarriers, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}};
inline constexpr SlotMap kSlotDataOdd = {kCarriers, {16, 9, 10, 11, 12, 13, 14, 15, 2, 3, 4, 5, 6, 7, 8, 1}};               // kOdd16
constexpr bool slot_maps_match()   // the same carriers as kOdd18 / kOdd16 and the plain order
{
	for (int j = 0; j < kHdrCarriers; j++) if (kSlotHdrEven.k[j] != j || kSlotHdrOdd.k[j] != kOdd18[j]) return false;
	for (int j = 0; j < kCarriers; j++) if (kSlotDataEven.k[j] != j + 1 || kSlotDataOdd.k[j] != kOdd16[j]) return false;
	return true;
}
static_assert(slot_maps_match(), "P3 slot maps");
inline const SlotMap &slot_map(int p, bool odd)
{
	return p < kHdrPulses ? (odd ? kSlotHdrOdd : kSlotHdrEven) : (odd ? kSlotDataOdd : kSlotDataEven);
}
// Carrier k of word slot j in pulse p (j < pulse_carriers(p)).
inline int slot_carrier(int p, int j, bool odd) { return slot_map(p, odd).k[j]; }
inline int word_index(int p, int j) { return p < kHdrPulses ? kHdrCarriers * p + j : kHdrWords + kCarriers * (p - kHdrPulses) + j; }

// Pulse value at time t (seconds from its centre).
inline float pulse(double t)
{
	const double u = std::fabs(t) * 9600.0;
	const int i = int(u);
	if (i >= kPulseHalf9600) return 0.f;
	const double f = u - i;
	return float(kPulse9600[i] * (1 - f) + kPulse9600[i + 1] * f);
}
constexpr double kPulseHalfS = kPulseHalf9600 / 9600.0;

// 1330 words -> audio. Pulse p is centred at lead_s + p T. amplitude is per
// carrier (the firmware's is ~500 on a 16-bit scale).
std::vector<float> modulate(const std::vector<uint8_t> &words, double fs, bool odd, float amplitude, double lead_s = 0.02);

// ---- two-carrier bursts inside a P3 link -------------------------------------
// CS, the set-up packets and the QRT end packet are P2-style word streams
// (high nibble carrier A, low nibble carrier B, B staggered ~0.52 T) but on
// 1080/1920 Hz with the narrow kCsPulse9600 pulse. Layout as p2::modulate
// with txd 0: word k of carrier A centred at 1.2 T + 0.8 T + k T.
constexpr double kCsLo = 1080.0, kCsHi = 1920.0, kCsStagger = 49.5 / 96.0;
// P1 answer to a P3 set-up packet (status 0x31), as p2arq::kP2Accept.
constexpr uint16_t kP3Accept = 0x59A;            // (0xA65 in the other polarity)
// Fixed set-up bursts (emulated firmware, identical for any callsigns):
// caller's first PSK burst, callee's reply, caller's second burst.
inline constexpr uint8_t kSetupA1[82] = {
	0xaa, 0x22, 0x6a, 0x62, 0xa2, 0xa6, 0x62, 0xea, 0xa6, 0x2a, 0x2a, 0xa2, 0xa2, 0xaa, 0xa2, 0xaa,
	0xaa, 0xaa, 0x22, 0x2a, 0x2a, 0xaa, 0x22, 0xaa, 0xa2, 0x22, 0xa2, 0x2a, 0xa2, 0xaa, 0x22, 0x2a,
	0x22, 0x2a, 0x22, 0xaa, 0xaa, 0x22, 0x2a, 0x22, 0x22, 0xaa, 0xa2, 0x2a, 0xa2, 0xa2, 0xa2, 0x2a,
	0xaa, 0x22, 0xaa, 0xaa, 0xaa, 0x2a, 0xaa, 0x2a, 0xa2, 0xaa, 0x2a, 0xa2, 0x2a, 0xa2, 0xa2, 0xaa,
	0xa2, 0xaa, 0xa2, 0xaa, 0x2a, 0xa2, 0x2a, 0xaa, 0x22, 0x22, 0x2a, 0xaa, 0x2a, 0x22, 0x22, 0xaa,
	0xa2, 0x2a,
};
inline constexpr uint8_t kSetupB1[82] = {
	0xaa, 0x22, 0xaa, 0x22, 0xaa, 0xaa, 0x22, 0xaa, 0x22, 0x22, 0x22, 0xaa, 0x22, 0xaa, 0x22, 0xaa,
	0xaa, 0x22, 0xaa, 0x22, 0xaa, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0xaa, 0x22, 0xa2, 0xa2, 0xaa,
	0xa2, 0x22, 0x22, 0xa2, 0x22, 0xaa, 0x22, 0xa2, 0x2a, 0xaa, 0x2a, 0x22, 0xaa, 0x22, 0x22, 0x2a,
	0x22, 0x22, 0x22, 0x2a, 0xaa, 0x22, 0xaa, 0xaa, 0x22, 0x22, 0x2a, 0xaa, 0x2a, 0x22, 0x22, 0x2a,
	0x2a, 0xaa, 0x22, 0x2a, 0xa2, 0xa2, 0xa2, 0xa2, 0x2a, 0x22, 0xaa, 0xa2, 0xaa, 0xaa, 0x2a, 0xaa,
	0xa2, 0xaa,
};
inline constexpr uint8_t kSetupA2[82] = {
	0xaa, 0x22, 0xaa, 0x22, 0xaa, 0xaa, 0x22, 0xaa, 0x22, 0x22, 0x22, 0xaa, 0x22, 0xaa, 0x22, 0xaa,
	0xaa, 0x22, 0xaa, 0x22, 0xaa, 0x22, 0x22, 0x22, 0x22, 0x2a, 0x2a, 0xaa, 0xaa, 0xa2, 0x2a, 0x22,
	0x22, 0xaa, 0xaa, 0x2a, 0xaa, 0x2a, 0xa2, 0x2a, 0xa2, 0x2a, 0x2a, 0x22, 0xa2, 0x22, 0xaa, 0x22,
	0x2a, 0x2a, 0xaa, 0xaa, 0x22, 0xa2, 0x22, 0x2a, 0x22, 0xaa, 0x22, 0x22, 0xaa, 0x2a, 0xa2, 0x2a,
	0xa2, 0xa2, 0xa2, 0xaa, 0xa2, 0xaa, 0xaa, 0x2a, 0x22, 0xa2, 0x22, 0xa2, 0xa2, 0x2a, 0x22, 0x2a,
	0xa2, 0x22,
};

// Break-in bursts (B1, A2 and a changeover break-in), 82 words: 0xaa, the P2
// break-in pattern (24 words), 56 data pulses, a tail word. The data pulses
// are P2 level-1 coding with 7 pulses per row (K=9, p2::kSeq1bk) of 3 data
// bytes + status + CRC: data byte 2 ^ 0x11 (the SL1 scrambler), status 0x89
// (| 0x20: more text follows), CRC = X.25(those 4 bytes as sent) ^ 0x87A3,
// low byte first. Recovered from 48 firmware break-ins carrying random
// texts; kSetupB1 is "\rPT" + more (tail 0xaa), kSetupA2 is idle (0x1e x3,
// tail 0x22) - bk_words() reproduces both word for word.
constexpr int kBkData = 3;
constexpr uint8_t kBkStatus = 0x89, kBkMore = 0x20;
constexpr uint16_t kBkCrcXor = 0x87A3;
inline void bk_info(const uint8_t *data, bool more, uint8_t info[6])
{
	info[0] = data[0]; info[1] = data[1]; info[2] = uint8_t(data[2] ^ 0x11);
	info[3] = uint8_t(kBkStatus | (more ? kBkMore : 0));
	const uint16_t c = uint16_t(crc16_ccitt_reflected(info, 4) ^ kBkCrcXor);
	info[4] = uint8_t(c); info[5] = uint8_t(c >> 8);
}
inline std::vector<uint8_t> bk_words(const uint8_t *data, bool more, uint8_t tail)
{
	uint8_t info[6];
	bk_info(data, more, info);
	std::vector<uint8_t> w(kSetupA2, kSetupA2 + 25);
	const auto d = p2::encode_data(1, info, 7);
	w.insert(w.end(), d.begin(), d.end());
	w.push_back(tail);
	return w;
}
// Decoded break-in information block -> data (3 bytes), the more flag and
// the data type (status bits 2-4, as in packets: the status sent is
// 0x89 ^ type << 2, e.g. 0x95 = PMC English swapped [bb-ext, MODE 2]),
// QRT (0x80) and "over" (0x40: with PDuplex 1 the firmware breaks in with
// its text and hands the link straight back, as its packets' over bit does;
// status 0xd1 = 0x89 ^ 0x40 ^ type 6, emulator and real reference unit, fix 134
// [bb-ext]); false if the CRC fails.
bool bk_check(const std::vector<uint8_t> &info, uint8_t *data, bool &more, int *type = nullptr, bool *qrt = nullptr, bool *over = nullptr);

inline double cs_pulse(double t)        // t in seconds from the centre
{
	const double u = std::fabs(t) * 9600.0;
	const int i = int(u);
	if (i >= kCsPulseHalf9600) return 0.0;
	const double f = u - i;
	return kCsPulse9600[i] * (1 - f) + kCsPulse9600[i + 1] * f;
}
// Table in p2::pulse_table's layout (centre 2T, 4T+1 samples), for ls_pulses.
const std::vector<double> &cs_pulse_table(float fs);
// swap: carrier A on the high tone. amplitude = peak of one carrier.
std::vector<float> modulate_cs(const std::vector<uint8_t> &words, float fs, bool swap, float amplitude);

// ---- speed level 1 (unproto mode 30) -----------------------------------------
// P2 level-1 coding (p2_codec, 5 data bytes) on the 1080/1920 Hz pair with
// the CS pulse, plus a fixed scrambler (68/68 firmware packets re-encode):
// data bytes 2..4 ^= 11 89 23, the status is sent ^ 0x10, and the X.25 CRC
// (over data + status as sent, data unscrambled) is sent ^ 0xA8A1.
constexpr int kSl1Data = 5;
inline constexpr uint8_t kSl1Scr[kSl1Data] = {0x00, 0x00, 0x11, 0x89, 0x23};
void sl1_make_info(const uint8_t *data, uint8_t status, uint8_t *info /*8*/);
// Received (Viterbi-decoded) 8 info bytes -> plain data + status; CRC ok?
bool sl1_check(const uint8_t *rx, uint8_t *data, uint8_t &status);

// ---- least-squares demodulation ------------------------------------------
//
// Model: x(t) = sum_p sum_k Re(z_pk e^{j w_k t}) g(t - c_p) over pulses
// p0..p1-1; z solved jointly. Columns are ordered by pulse, so the normal
// matrix is banded (pulses overlap their two neighbours each side).

struct Demod {
	int p0 = 0, p1 = 0;
	std::vector<int> col0;                       // first column of pulse p - p0
	std::vector<std::complex<double>> z;         // [col/2]
	double resid = 1;                            // relative residual
	std::complex<double> at(int p, int k) const  // carrier k of pulse p
	{
		const int nc = pulse_carriers(p), kk = nc == kHdrCarriers ? k : k - 1;
		return z[size_t(col0[size_t(p - p0)] / 2 + kk)];
	}
};

// t0 = centre of pulse 0 in samples (fractional). x is sampled at fs.
Demod demod_slow(const float *x, long n, double fs, double t0, int p0, int p1, double df = 0);

// Same result as demod_slow, fast: every pulse is the same table shifted by
// a whole number of samples (T = 10 ms is an integer at 9600/48000 Hz), so
// each normal-matrix entry is a precomputed sum
//   U_d(W) = sum_m g(m) g(m - dT) e^{jWm}
// times a phase, for the difference/sum frequency W of the two carriers.
// The data-independent part of demod (2026-10-22, fix 152): the pulse table, the
// columns, the normal matrix and its banded Cholesky factor depend only on the
// timing (t0, p0, p1), the offset df and fs; only the right-hand side needs the
// samples. Planned while a packet is still arriving (the station's early timing
// search), the solve at its end is a few ms: on a Pi 4 the factor was 26 of the
// 37 ms of an SL5 demodulation, against a ~32 ms reply window.
struct DemodPlan {
	bool ok = false;                                  // false: demod_slow at run time
	double fs = 0, t0 = 0, df = 0; int p0 = 0, p1 = 0;
	int Tn = 0, np = 0, L = 0, NC = 0, N = 0, bw = 0; long base = 0;
	std::vector<double> g, A;                        // A: the factor (upper band)
	std::vector<int> col0, col_r, col_k;
};
DemodPlan demod_plan(long n, double fs, double t0, int p0, int p1, double df = 0);
// The sample-dependent part: right-hand side, the two triangular solves, the residual.
// The least-squares pulse fits' common steps (demod_run below, p3_levels.hpp demod_flat_run), a pulse
// row at a time (columns ordered by row, as the plans make them; a row's carriers share its samples).
// Per column the same operations in the same order as the one-column-at-a-time form with
// std::complex they replace (z += g x ph; ph *= rot), written out so a row's carriers run side by side
// (fix 216b, the Pi 4: bit for bit the same).
template <class WOf>
inline void ls_rhs_rows(const int *col_r, const int *col_k, int NC, long base, int Tn, int L, const double *g, const float *x, WOf w_of, double *rhs)
{
	constexpr int K = 18;
	double pr[K], pi[K], rr[K], ri[K], zr[K], zi[K];
	for (int c0 = 0; c0 < NC;) {
		int c1 = c0;
		while (c1 < NC && col_r[c1] == col_r[c0] && c1 - c0 < K) c1++;
		const int nk = c1 - c0;
		const long s0 = base + long(col_r[c0]) * Tn;
		for (int j = 0; j < nk; j++) {
			const double w = w_of(col_k[c0 + j]);
			const std::complex<double> ph = std::polar(1.0, -w * double(s0)), rot = std::polar(1.0, -w);
			pr[j] = ph.real(); pi[j] = ph.imag(); rr[j] = rot.real(); ri[j] = rot.imag(); zr[j] = 0; zi[j] = 0;
		}
		for (int m = 0; m < L; m++) {
			const double gx = g[m] * double(x[s0 + m]);
			for (int j = 0; j < nk; j++) {
				zr[j] += gx * pr[j]; zi[j] += gx * pi[j];
				const double a = pr[j] * rr[j] - pi[j] * ri[j], b = pr[j] * ri[j] + pi[j] * rr[j];
				pr[j] = a; pi[j] = b;
			}
		}
		for (int j = 0; j < nk; j++) { rhs[2 * (c0 + j)] = zr[j]; rhs[2 * (c0 + j) + 1] = zi[j]; }
		c0 = c1;
	}
}
// Forward substitution with the banded Cholesky factor (row i: A[i*bw + (j-i)]), by columns: each y[i]
// gets the same subtractions in the same order (k ascending, then its division) as by rows, but the
// updates of one column are independent.
void ls_forward(const double *A, int N, int bw, double *y, int k0 = 0, int k1 = -1);
// The fitted model over the span (for the residual): mdl[t - base] += g Re(z e^{jwt}), the columns'
// contributions to each sample in column order, as before.
template <class WOf>
inline void ls_model_rows(const int *col_r, const int *col_k, int NC, long base, int Tn, int L, const double *g, WOf w_of, const double *y, double *mdl)
{
	constexpr int K = 18;
	double pr[K], pi[K], rr[K], ri[K], zr[K], zi[K];
	for (int c0 = 0; c0 < NC;) {
		int c1 = c0;
		while (c1 < NC && col_r[c1] == col_r[c0] && c1 - c0 < K) c1++;
		const int nk = c1 - c0;
		const long s0 = base + long(col_r[c0]) * Tn;
		for (int j = 0; j < nk; j++) {
			const double w = w_of(col_k[c0 + j]);
			const std::complex<double> ph = std::polar(1.0, w * double(s0)), rot = std::polar(1.0, w);
			pr[j] = ph.real(); pi[j] = ph.imag(); rr[j] = rot.real(); ri[j] = rot.imag(); zr[j] = y[2 * (c0 + j)]; zi[j] = y[2 * (c0 + j) + 1];
		}
		for (int m = 0; m < L; m++) {
			double &t = mdl[s0 - base + m];
			for (int j = 0; j < nk; j++) {
				t += g[m] * (zr[j] * pr[j] - zi[j] * pi[j]);
				const double a = pr[j] * rr[j] - pi[j] * ri[j], b = pr[j] * ri[j] + pi[j] * rr[j];
				pr[j] = a; pi[j] = b;
			}
		}
		c0 = c1;
	}
}
Demod demod_run(const DemodPlan &P, const float *x, long n);
inline Demod demod(const float *x, long n, double fs, double t0, int p0, int p1, double df = 0)
{
	return demod_run(demod_plan(n, fs, t0, p0, p1, df), x, n);
}

// Phase step (1/16 turn) into pulse p on word slot j: the word value the
// transmitter used, as a real number.
inline double step(const Demod &d, int p, int j, bool odd)
{
	const int k = slot_carrier(p, j, odd);
	const auto a = d.at(p - 1, k), b = d.at(p, k);
	double s = -std::arg(b / a) / (2 * M_PI) * 16;
	return s < 0 ? s + 16 : s;
}

// Known header words for a counter parity (lead + kHeader*).
inline std::vector<uint8_t> header_words(int parity)
{
	std::vector<uint8_t> w{0x0a, 0x0a};
	const uint8_t *h = parity ? sl2400::kHeaderOdd : sl2400::kHeaderEven;
	w.insert(w.end(), h, h + 160);
	return w;
}

struct HeaderScore { double score = -1; int parity = 0; bool odd = false; double bias = 0; };

// Score pulses 1..8 of a demodulated header against both parities and maps.
// score = mean cos of the step error (1 = perfect); bias = mean error (1/16 turn).
HeaderScore score_header(const Demod &d);

struct RxPacket { bool ok = false; double t0 = 0; bool odd = false; int parity = 0; double hscore = 0, resid = 1, df = 0; RxResult r;
	std::vector<uint8_t> lvdata; int lvstatus = -1;   // lvdata/lvstatus: levels other than 5 (data may exceed r.info)
	// memory ARQ (2026-10-18): this copy's per-word differential products
	// z_p conj(z_(p-1)) at the expected level, for both carrier maps (SL5:
	// one, its map known from the header, bias removed); marq = decoded only
	// from copies combined
	int mq_level = 0; std::vector<std::complex<double>> mq_d0, mq_d1; int marq = 0; double agree = 1; };   // agree: the decode's (p3::RxResult)

// Timing and frequency from the header alone (pulses 0..kHdrPulses): needs
// only the first ~0.1 s of the packet, so a real-time receiver can run it
// while the rest of the packet is still arriving.
struct Sync5 { HeaderScore hb; double tb = 0; };
Sync5 sync_at(const float *x, long n, double fs, double t0, double search_ms = 5.0);

// Demodulate the whole packet at the sync_at() timing, Viterbi.
RxPacket finish_at(const float *x, long n, double fs, const Sync5 &s, std::vector<std::complex<double>> *dd = nullptr, const DemodPlan *plan = nullptr);

// SL5 memory ARQ: the data words' differential products of one copy (its
// own map and bias removed), and a decode from summed products.
std::vector<std::complex<double>> sl5_diffs(const float *x, long n, double fs, const Sync5 &s);
RxResult sl5_decode_diffs(const std::vector<std::complex<double>> &dd);

// Decode one packet whose pulse 0 centre is near t0 (samples): refine timing
// on the header, estimate the frequency offset, demodulate, Viterbi.
// search_ms: half-width of the coarse timing search (5 ms default; an ARQ
// link that already knows the peer's timing can pass ~0 for a fast decode).
inline RxPacket decode_at(const float *x, long n, double fs, double t0, double search_ms = 5.0)
{
	return finish_at(x, n, fs, sync_at(x, n, fs, t0, search_ms));
}

// Find packets in a recording: energy bursts of about a packet's length,
// then decode_at() from the burst's leading edge.
std::vector<RxPacket> receive(const std::vector<float> &x, double fs);

}  // namespace fmodem::p3
