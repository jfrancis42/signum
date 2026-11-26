// P3 packets, 2400 Bd level (16 carriers, DQPSK). C++17,
// recovered from the reference firmware's behaviour (the protocol description section 4).
//
//   info  = 212 data bytes + status (0x20|counter) + CRC-16 (X.25, LE)
//   coded = K=7 conv code (g0 0x6D, g1 0x4F, newest bit in the LSB),
//           8 zero tail bits, punctured to r=3/4 (keep coded j%6 in 1..4)
//   chan  = 2304 kept bits, interleaved by kPi, inverted by kComp, two per
//           symbol (Gray DQPSK), 72 data pulses x 16 carriers
//   words = 2 lead (0a 0a) + 10 header pulses + 72 data + 1 tail pulse,
//           16 per pulse (one per carrier slot, see kSlotHz)
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>

#include "crc16.hpp"
#include "p3_sl2400_tables.hpp"
#include "viterbi64.hpp"

namespace fmodem::p3 {

constexpr int kDataBytes = 212, kInfoBytes = 215, kInfoBits = kInfoBytes * 8;
constexpr int kSteps = kInfoBits + 8, kCoded = 2 * kSteps, kKept = 2304;
constexpr int kPulses = 83, kCarriers = 16, kDataPulse0 = 10, kDataPulses = 72;

inline int parity(unsigned v) { return __builtin_popcount(v) & 1; }
inline bool kept_pos(int j) { const int r = j % 6; return r >= 1 && r <= 4; }
inline int coded_of_kept(int k) { return 6 * (k / 4) + (k % 4) + 1; }

inline void make_info(const uint8_t *data, int counter, uint8_t *info, uint8_t status_hi = 0x20)
{
	std::memcpy(info, data, kDataBytes);
	info[kDataBytes] = uint8_t(status_hi | (counter & 3));
	const uint16_t c = crc16_ccitt_reflected(info, kDataBytes + 1);
	info[kDataBytes + 1] = uint8_t(c);
	info[kDataBytes + 2] = uint8_t(c >> 8);
}

// Information block -> 1330 words (low nibble = phase step in 1/16 turn).
// first: the first packet a station sends in a P3 ARQ link ends with 16x 'a'.
std::vector<uint8_t> encode(const uint8_t *info, int counter, bool first = false);

struct RxResult { bool ok = false; uint8_t info[kInfoBytes]{}; int metric = 0; double agree = 1; };   // agree: -path metric / sum |soft| (soft decodes; 1: every coded bit agrees)

// Data steps[72*16] (1/16 turn, any real value) -> info. Hard Viterbi with
// erasures for the punctured bits.
RxResult decode_steps_hard(const double *steps);

// Soft decisions (2026-10-18; the hard decoder above until then,
// P3_SL5_HARD=1 keeps it): per coded bit, the phase distance to the nearest
// symbol with that bit 0 minus that with it 1, scaled by the word's
// reliability wt (optional: |differential product| / mean). Punctured bits
// are erasures (0). metric: the path metric, rounded.
RxResult decode_steps(const double *steps, const double *wt = nullptr);

}  // namespace fmodem::p3
