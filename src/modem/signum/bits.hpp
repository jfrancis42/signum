// Fixed-capacity bit buffers. The protocol sends the least significant bit of
// each byte first, so bit i of the stream is bit (i % 8) of byte i / 8.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace fmodem {

template <std::size_t Bytes>
class BitWriter {
public:
	// Append the low `n` bits of `v`, bit 0 first. Returns false when full.
	bool put(uint32_t v, int n)
	{
		if (pos_ + std::size_t(n) > Bytes * 8) return false;
		for (int i = 0; i < n; i++, pos_++) {
			if (v >> i & 1) buf_[pos_ / 8] |= uint8_t(1u << (pos_ % 8));
		}
		return true;
	}
	std::size_t bits() const { return pos_; }
	std::size_t room() const { return Bytes * 8 - pos_; }
	const std::array<uint8_t, Bytes> &bytes() const { return buf_; }

private:
	std::array<uint8_t, Bytes> buf_{};
	std::size_t pos_ = 0;
};

class BitReader {
public:
	BitReader(const uint8_t *data, std::size_t nbits) : data_(data), n_(nbits) {}
	bool empty() const { return pos_ >= n_; }
	std::size_t pos() const { return pos_; }
	int get() { int b = data_[pos_ / 8] >> (pos_ % 8) & 1; pos_++; return b; }

private:
	const uint8_t *data_;
	std::size_t n_, pos_ = 0;
};

}  // namespace fmodem
