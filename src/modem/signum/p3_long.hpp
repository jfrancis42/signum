// P3 long-cycle packets (3.75 s cycle), speed levels 1-6.
// The coding chain matches the reference firmware V.4.1r exactly.
//
// SL2-6 chain (the same for short packets; only E/I/P differ):
//   info  = payload + status + CRC-16 (X.25 over payload + status, LE) + 0x00, E bytes
//           (bytes after the 0x00 never reach the channel)
//   scramble: byte 2k ^= T[k] >> 8, byte 2k+1 ^= T[k] & 0xFF, T = the CRC-16 byte table
//           (0x8408), k taken mod 256 (the index is a byte); the trailing 0x00
//           is sent unscrambled
//   encode: K=7, input LSB first into a 7-bit state (newest bit in the LSB); per step
//           parity(state & 0x4F) then parity(state & 0x6D); puncture mask per level
//           (SL2-4 none, SL5 0xD, SL6 0x1B366: shifted out LSB first, the top set bit
//           ends the period); kept bits packed LSB first into bytes
//   interleave: the first I coded bytes, as 8I bits; input bit n goes to output bit p,
//           p starting at 8I - P, then p -= P while p - P >= 0, else the next column
//           (start + 1, start + 2, ...)
//   channel bit k = output bit k (byte k >> 3, bit k & 7); 1 bit per data word at
//           SL2/3, 2 at SL4-6 (word = kQpsk[(byte >> bit) & 3])
// SL5 long sends 32 channel bits more than the interleaver produces: the reference unit
// sends the packet's first 4 payload bytes there, in the clear (seen in its long packets).
// We send the same.
// SL1 long is the P2 level-1 long packet (K=9), unchanged from the measured model.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "crc16.hpp"
#include "p3_long_tables.hpp"
#include "viterbi64.hpp"

namespace fmodem::p3 {

inline int long_payload(int level) { return level >= 1 && level <= 6 ? lng::level(level).payload : 0; }

namespace lchain {

inline const std::array<uint16_t, 256> &crc_table()
{
	static const std::array<uint16_t, 256> t = [] {
		std::array<uint16_t, 256> r{};
		for (unsigned i = 0; i < 256; i++) {
			unsigned c = i;
			for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0x8408u : c >> 1;
			r[i] = uint16_t(c);
		}
		return r;
	}();
	return t;
}

// Encoder input bytes E, interleaver bytes I, interleaver step P [level 2..6][long]
struct Chain { int E, I, P; };
inline Chain chain(int level, bool lng)
{
	static const Chain t[5][2] = {
		{{30, 54, 35}, {122, 240, 181}},
		{{66, 126, 69}, {282, 560, 321}},
		{{128, 252, 250}, {562, 1120, 1122}},
		{{218, 288, 563}, {962, 1280, 2407}},
		{{290, 324, 909}, {1282, 1440, 3876}},
	};
	return t[level - 2][lng ? 1 : 0];
}

// K=7 encoder with puncturing; returns the kept bits packed LSB first
// (a trailing partial byte is dropped, as the reference unit does).
std::vector<uint8_t> encode(const std::vector<uint8_t> &in, int level);

// Column interleaver over the first I bytes.
std::vector<uint8_t> interleave(const std::vector<uint8_t> &in, int I, int P);

// payload + status -> channel bits (as bytes, LSB first), plus the SL5-long tail.
std::vector<uint8_t> channel(int level, bool lng, const uint8_t *data, int payload, uint8_t status);

}  // namespace lchain

inline constexpr uint8_t kQpsk[4] = {0x0A, 0x06, 0x0E, 0x02};

// Fix 256: SL1 long, channel bit 36 (word 27's high nibble) carries coded bit 593 uninverted, not
// 592 inverted (the measured table has 592 twice, here and at channel bit 76; 593 continues its
// row: channel bits 30, 32, 34, 38 carry 497, 529, 561, 625). Checked on the emulated reference unit's U 36:
// with the table our words for 10 of its 14 packets differed from the words it sent, in that
// bit alone. P3_L1_CODED_OLD=1: the table as it was.
inline bool l1_old() { static const bool old = std::getenv("P3_L1_CODED_OLD") != nullptr; return old; }
inline int l1_coded(int i) { return i == 36 && !l1_old() ? 593 : lng::level(1).coded[i]; }
inline int l1_comp(int i) { return i == 36 && !l1_old() ? 0 : lng::level(1).comp[i]; }

// payload bytes + plain status -> the packet's words (counter = status & 3).
std::vector<uint8_t> long_encode(int level, const uint8_t *data, uint8_t status);

struct LongRx { bool ok = false; std::vector<uint8_t> data; uint8_t status = 0; double metric = 0; };
// SL1 long receive (2026-10-18): the inverse of long_encode's level-1 branch.
// nib[2 * w] / nib[2 * w + 1] = the received phase steps (1/16 turn, real) of
// word w's high / low nibble (its two carriers). Each data word carries two
// coded bits in its nibbles' bit 3 (a half turn on top of the template's
// low 3 bits); the K=9 code (g0, g1) and the coded-bit map are the tables'.
LongRx long1_decode(const double *nib);

// Receive (2026-10-17): the inverse of long_encode for SL2-6. steps[w] = the
// received phase step (1/16 turn, real) of every word of the long layout
// (layout_long word order). Soft channel bits -> the interleaver inverted ->
// K=7 Viterbi through the level's puncturing (coded bits past the
// interleaver's I bytes are erasures; the encoder input after CRC + 0x00 is
// known zero) -> descramble -> CRC.
// wt (optional): a reliability per word (e.g. the magnitude of its
// differential product, mean ~1); each word's soft bits are scaled by it
// (2026-10-18: phase distance alone weighs a faded or noise-buried word like
// a clean one).
LongRx long_decode(int level, const double *steps, const double *wt = nullptr);

}  // namespace fmodem::p3
