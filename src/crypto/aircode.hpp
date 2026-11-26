// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// aircode.hpp: how the encryption layer's binary frames travel in the link's text stream ("air code
// v1"). Ciphertext is random bytes, and the modem's per-packet coding (Huffman / PMC, made for text)
// spends 2.1 air bytes per random byte on P3 and 1.6 on P2 (8-bit bytes go as escapes or ASCII
// packets); base64 would cost 1.37. Instead the bits are written as printable characters with a
// prefix code whose code lengths follow the modem's own Huffman table (a complete prefix code over
// 0x20-0x7E built from the weights 2^-h, h = that character's Huffman length; every length here is at
// most its h): a random bit stream read through it yields characters that the modem's Huffman coding
// sends back at about one air bit per bit, 1.02 air bytes per byte measured on P2 and P3.
// No control characters, no 0x1C-0x1E, nothing >= 0x7F.
//
// A frame is encoded on its own: its bits MSB first, the last character completed with zero bits; the
// reader knows the frame's length from its header and drops the padding. Changing the table changes
// the protocol: it is part of encryption protocol version 1.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace signum::crypto {

class AirCode {
public:
	// The code lengths for 0x20..0x7E (generated from the modem's Huffman type 1 table; Kraft sum 1).
	static constexpr uint8_t kLen[95] = {
	    2, 10, 11, 13, 12, 11, 11, 11, 9, 9, 11, 11, 7, 10, 7, 10,
	    8, 9, 9, 9, 9, 9, 9, 9, 9, 9, 10, 13, 12, 9, 11, 9,
	    12, 8, 8, 8, 7, 8, 8, 10, 9, 8, 9, 9, 9, 9, 9, 8,
	    8, 9, 8, 7, 8, 9, 9, 9, 12, 13, 8, 14, 13, 14, 14, 11,
	    14, 5, 7, 6, 5, 3, 7, 6, 6, 4, 10, 7, 6, 6, 4, 6,
	    8, 9, 4, 5, 5, 5, 8, 7, 9, 9, 7, 14, 14, 14, 14};
	static constexpr int kMaxLen = 14;

	AirCode()
	{
		// canonical code: by (length, character)
		int code = 0, k = 0;
		for (int l = 1; l <= kMaxLen; l++) {
			first_[size_t(l)] = code;
			index_[size_t(l)] = k;
			for (int c = 0; c < 95; c++)
				if (kLen[c] == l) { sorted_[size_t(k++)] = char(0x20 + c); code_[size_t(c)] = uint16_t(code++); }
			count_[size_t(l)] = k - index_[size_t(l)];
			code <<= 1;
		}
	}
	// bytes -> characters (the last character padded with zero bits)
	std::string encode(const uint8_t *p, size_t n) const
	{
		std::string o;
		int code = 0, len = 0;
		auto bit = [&](int b) {
			code = code << 1 | b; len++;
			const int i = code - first_[size_t(len)];
			if (i >= 0 && i < count_[size_t(len)]) { o += sorted_[size_t(index_[size_t(len)] + i)]; code = 0; len = 0; }
		};
		for (size_t i = 0; i < n; i++)
			for (int k = 7; k >= 0; k--) bit((p[i] >> k) & 1);
		while (len) bit(0);
		return o;
	}
	// One character's bits appended to `bits` (MSB first, one bit per byte of the vector); false: not
	// a character of the code.
	bool bits_of(char c, std::vector<uint8_t> &bits) const
	{
		const int i = int(uint8_t(c)) - 0x20;
		if (i < 0 || i >= 95) return false;
		for (int k = kLen[i] - 1; k >= 0; k--) bits.push_back(uint8_t((code_[size_t(i)] >> k) & 1));
		return true;
	}

private:
	std::array<int, kMaxLen + 2> first_{}, index_{}, count_{};
	std::array<char, 95> sorted_{};
	std::array<uint16_t, 95> code_{};
};

// Reads frames back from the character stream. Frame: type (1 byte), length (2 bytes, big-endian),
// then that many bytes; each frame starts on a new character.
class FrameReader {
public:
	explicit FrameReader(const AirCode &c) : code_(c) {}
	enum Result { NEED_MORE, FRAME, BAD };
	// Feed one character; FRAME when one is complete (type() and body()), BAD for a character outside
	// the code or a frame longer than max_len.
	Result feed(char c, size_t max_len)
	{
		if (!code_.bits_of(c, bits_)) return BAD;
		while (bits_.size() - pos_ >= 8 && have_ < need()) {
			uint8_t b = 0;
			for (int k = 0; k < 8; k++) b = uint8_t(b << 1 | bits_[pos_++]);
			buf_.push_back(b);
			have_++;
			if (have_ == 3 && size_t(buf_[1] << 8 | buf_[2]) > max_len) return BAD;
		}
		if (have_ >= 3 && have_ == need()) {
			type_ = buf_[0];
			body_.assign(buf_.begin() + 3, buf_.end());
			reset();                                       // the rest of this character's bits: padding
			return FRAME;
		}
		return NEED_MORE;
	}
	uint8_t type() const { return type_; }
	const std::vector<uint8_t> &body() const { return body_; }
	bool idle() const { return have_ == 0 && bits_.size() == pos_; }

private:
	size_t need() const { return have_ < 3 ? 3 : 3 + size_t(buf_[1] << 8 | buf_[2]); }
	void reset() { bits_.clear(); pos_ = 0; buf_.clear(); have_ = 0; }
	const AirCode &code_;
	std::vector<uint8_t> bits_, buf_, body_;
	size_t pos_ = 0, have_ = 0;
	uint8_t type_ = 0;
};

inline std::string frame_chars(const AirCode &c, uint8_t type, const std::vector<uint8_t> &body)
{
	std::vector<uint8_t> f;
	f.reserve(body.size() + 3);
	f.push_back(type); f.push_back(uint8_t(body.size() >> 8)); f.push_back(uint8_t(body.size()));
	f.insert(f.end(), body.begin(), body.end());
	return c.encode(f.data(), f.size());
}

}  // namespace signum::crypto
