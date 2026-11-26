// Packet CRC: CRC-16/CCITT in the bit-reflected (X.25/HDLC) form,
// preset 0xFFFF, result inverted, transmitted low byte first.
//
// Sources: the 1990 P1 description says "CRC nach CCITT-Norm" over the
// packet from the data field on (header excluded); the reflected form,
// preset and inversion follow the hf implementation (T. Sailer, used as a
// factual reference only). Verified black-box: off-air P1 packets and
// every P2/P3 packet class of the reference firmware (some P3 classes XOR the
// result with a per-class constant; see the protocol description).
#pragma once

#include <cstddef>
#include <cstdint>

namespace fmodem {

constexpr uint16_t crc16_ccitt_reflected(const uint8_t *data, std::size_t len, uint16_t preset = 0xffff)
{
	uint16_t crc = preset;
	for (std::size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int b = 0; b < 8; b++) crc = (crc & 1) ? uint16_t((crc >> 1) ^ 0x8408) : uint16_t(crc >> 1);
	}
	return uint16_t(~crc);
}

}  // namespace fmodem
