// P1 source coding: Huffman (1990 PT table) and 8-bit ASCII.
// Portable C++17, no heap. Bits are consumed LSB-first (first on air = bit 0).
#pragma once

#include <cstdint>

#include "bits.hpp"

namespace fmodem {

// kHuff[ascii] = { code, length }, first-transmitted bit in bit 0.
struct HuffEntry { uint16_t code; uint8_t len; };
inline constexpr HuffEntry kHuff[128] = {
#include "p1_huffman_table.inc"
};

// Decode one Huffman symbol from the reader; returns ASCII, or -1 if the next
// bits don't complete a code before `maxlen` (15) bits or the stream ends.
inline int huffman_next(BitReader &r)
{
	uint16_t acc = 0;
	for (int n = 1; n <= 15; n++) {
		if (r.empty()) return -1;
		acc |= uint16_t(r.get() << (n - 1));
		for (const auto &e : kHuff)
			if (e.len == n && e.code == acc) return int(&e - kHuff);
	}
	return -1;
}

constexpr uint8_t IDLE_RS = 0x1E;   // ASCII RS

}  // namespace fmodem
