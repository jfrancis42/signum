// P3 speed levels 2 and 3: packet coding (C++17, measured
// black-box; the protocol description section 4.2). Waveforms: p3_levels.hpp.
//
//   info  = data (23 / 59 bytes, 0x1e fill) ^ scrambler, status ^ kStx,
//           CRC-16 X.25 over (plain data, status as sent) ^ kCrcx, LE
//   coded = K=7 conv code (0x6D / 0x4F), 8 zero tail bits, rate 1/2
//   words = per-counter template; data symbol j's bit 1 = coded[kMap[j]]
//           (bit 0 is a fixed quarter-turn offset per position: DBPSK)
// Plain status = 0x20 (more data follows) | counter (first data packet 0).
// The firmware strips a leading space from a packet's text (we never send
// one; on receive the text is taken as decoded).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "crc16.hpp"
#include "p3_lv23_tables.hpp"
#include "p3_lv4_tables.hpp"
#include "viterbi64.hpp"

namespace fmodem::p3 {

struct Lv23 {
	int level, ndata, hdr, ndat, nwords;
	uint8_t stx; uint16_t crcx;
	const uint8_t *scr; const uint16_t *map; const uint8_t *tmpl[4];
};
const Lv23 &lv23_cfg(int level);

inline int lv23_parity(unsigned v) { return __builtin_popcount(v) & 1; }

// data (ndata bytes, already padded) + plain status -> packet words.
std::vector<uint8_t> lv23_encode(int level, const uint8_t *data, uint8_t status);

struct Lv23Rx { bool ok = false; std::vector<uint8_t> data; uint8_t status = 0; int metric = 0; double agree = 1; };   // agree: as p3::RxResult

// steps[j] = received phase step (1/16 turn, real) for every word position.
// Soft decision per data symbol between its two allowed values.
Lv23Rx lv23_decode(int level, const double *steps, int counter_hint = -1, const double *wt = nullptr);   // wt: optional per-word reliability


// ---- speed level 4 ------------------------------------------------------------
std::vector<uint8_t> lv4_encode(const uint8_t *data, uint8_t status);
Lv23Rx lv4_decode(const double *steps, const double *wt = nullptr);   // wt: optional per-word reliability

}  // namespace fmodem::p3
