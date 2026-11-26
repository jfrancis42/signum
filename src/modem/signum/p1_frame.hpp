// P1 packet framing and validation (portable C++17, no heap).
//
// A packet, LSB first: header(8) + data(N) + status(8) + CRC(16).
// N = 64 at 100 baud (96-bit packet) or 160 at 200 baud (192-bit). Header is
// 0x55, inverted to 0xAA on new-information packets; the FSK shift polarity
// also inverts each packet, so a receiver tries both bit polarities. The CRC
// (crc16.hpp) covers data+status, header excluded.
#pragma once

#include <cstddef>
#include <cstdint>

#include "crc16.hpp"

namespace fmodem {

struct P1Packet {
	int bit_offset;        // where it was found in the bit buffer
	bool inverted;         // bits were inverted (opposite shift polarity)
	uint8_t header;        // 0x55 or 0xAA
	uint8_t status;        // packet counter (0-1), data type (2-4), BK (6), QRT (7)
	int data_bits;         // 64 or 160
	uint8_t data[20];      // packed data field, LSB first (<=160 bits = 20 bytes)
};

inline int p1_packet_bits(int baud) { return baud == 100 ? 96 : 192; }
inline int p1_data_bits(int baud) { return baud == 100 ? 64 : 160; }

// Pack `n` bits (0/1 bytes, LSB first) starting at `bits[off]` into `out`.
inline void pack_lsb(const uint8_t *bits, int off, int n, uint8_t *out)
{
	for (int i = 0; i < (n + 7) / 8; i++) out[i] = 0;
	for (int i = 0; i < n; i++)
		if (bits[off + i]) out[i / 8] |= uint8_t(1u << (i % 8));
}

// Test one position/polarity; fill `pkt` and return true on a header+CRC match.
inline bool p1_try_packet(const uint8_t *bits, int off, int total, int baud,
                          bool invert, P1Packet &pkt)
{
	const int n = p1_packet_bits(baud), db = p1_data_bits(baud);
	if (off + n > total) return false;
	uint8_t tmp[24];
	for (int i = 0; i < n; i++) tmp[i / 8] = 0;
	for (int i = 0; i < n; i++) {
		int b = bits[off + i] & 1;
		if (invert) b ^= 1;
		if (b) tmp[i / 8] |= uint8_t(1u << (i % 8));
	}
	const uint8_t hdr = tmp[0];
	if (hdr != 0x55 && hdr != 0xAA) return false;
	// body = data + status (bytes 1 .. 1+db/8), CRC = next 2 bytes.
	const int body_bytes = (db + 8) / 8;
	const uint16_t crc_rx = uint16_t(tmp[1 + body_bytes] | (tmp[1 + body_bytes + 1] << 8));
	if (crc16_ccitt_reflected(tmp + 1, body_bytes) != crc_rx) return false;

	pkt.bit_offset = off;
	pkt.inverted = invert;
	pkt.header = hdr;
	pkt.data_bits = db;
	for (int i = 0; i < (db + 7) / 8; i++) pkt.data[i] = tmp[1 + i];
	pkt.status = tmp[1 + db / 8];
	return true;
}

// Status byte fields.
inline int p1_counter(const P1Packet &p) { return p.status & 3; }
inline int p1_datatype(const P1Packet &p) { return (p.status >> 2) & 7; }   // 0=ASCII8, 1=Huffman
inline bool p1_qrt(const P1Packet &p) { return p.status & 0x80; }

}  // namespace fmodem
