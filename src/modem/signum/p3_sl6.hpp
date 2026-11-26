// P3 speed level 6 (18 carriers, DQPSK, rate 8/9). C++17,
// recovered black-box from the reference firmware's unproto mode 35 packets
// (the protocol description section 4.2, the provenance record "P3 SL6").
//
//   info  = 284 data bytes + status + CRC-16 (X.25 over data + status, LE)
//   coded = K=7 conv code (g0 0x6D, g1 0x4F, newest bit in the LSB), 8 zero
//           tail bits, punctured to r=8/9: of the unpunctured stream
//           (index j = 2 x step + generator) keep j % 16 in {0,3,4,7,8,9,12,13,14}
//   chan  = 2592 kept bits, interleaved by sl6::kPi, inverted by sl6::kComp,
//           two per symbol (Gray DQPSK, step = 2 + 4 s), 72 pulses x 18 carriers
//   words = 162 header (2 lead + 160, by counter parity) + 1296 data + 18 tail;
//           layout(6): 82 pulses x 18 carriers 480..2520 Hz
// Measured status: 0x20 | counter. A constant XOR on the status or CRC would
// be invisible in unproto data (it folds into kComp); the status bits that
// vary in ARQ (type, over, QRT) are therefore not verified against the
// firmware yet.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>

#include "crc16.hpp"
#include "p3_lv23.hpp"
#include "p3_sl6_tables.hpp"
#include "viterbi64.hpp"

namespace fmodem::p3 {

constexpr int kSl6Data = 284, kSl6Info = kSl6Data + 3, kSl6InfoBits = kSl6Info * 8;
constexpr int kSl6Steps = kSl6InfoBits + 8, kSl6Coded = 2 * kSl6Steps, kSl6Kept = 2592, kSl6Symbols = 72 * 18;

inline bool sl6_kept(int j) { const int r = j % 16; return r == 0 || r == 3 || r == 4 || r == 7 || r == 8 || r == 9 || r == 12 || r == 13 || r == 14; }
inline const std::vector<int> &sl6_kept_positions()
{
	static const std::vector<int> v = [] { std::vector<int> p; for (int j = 0; j < kSl6Coded; j++) if (sl6_kept(j)) p.push_back(j); return p; }();
	return v;
}

// Plain data (284 bytes) + plain status -> 1476 words.
std::vector<uint8_t> sl6_encode(const uint8_t *data, uint8_t status);

// Phase steps of all 1476 words (1/16 turn, real) -> data + status. Hard
// Viterbi with erasures for the punctured positions; CRC decides.
Lv23Rx sl6_decode_hard(const double *steps);

// Soft decisions (2026-10-18; the hard-decision decoder above was used until
// then, P3_SL6_HARD=1 keeps it): per coded bit, the phase distance to the
// nearest symbol with that bit 0 minus that with it 1, scaled by the word's
// reliability wt (optional, e.g. |differential product| / mean).
Lv23Rx sl6_decode(const double *steps, const double *wt = nullptr);

}  // namespace fmodem::p3
