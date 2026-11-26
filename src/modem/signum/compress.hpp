// P2/P3 online text compression (the data type in status bits 2-4).
//
//   0 ASCII 8-bit, 1 Huffman, 2 Huffman with upper/lower case swapped,
//   4/5 PMC German normal/swapped, 6/7 PMC English normal/swapped
//   [spec: P2 protocol fig. 1, P3 protocol s.6-7]. The Huffman
//   table is P1's [spec: P3 s.7, ARRL P1 s.7]; verified on
//   the firmware's MODE 1 packets [bb-ext]. Bits go LSB-first through the
//   data field; unused space is filled with idle (0x1e) codes, the last one
//   cut off. PMC: after one of 16 "context" characters the next character
//   uses that context's own code table, after any other character the
//   Huffman table [spec]; the tables are measured black-box (docs/
//   the provenance record) and supplied by pmc_tables.inc when available. In the
//   compressed types a byte >= 0x80 is the symbol 0x7F followed by its low 7
//   bits, LSB first, and DEL (0x7F) is the symbol 0x7F + 0 [bb-ext: B2F
//   binary blocks in PMC packets; host-mode probes].
#pragma once

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "p1_huffman.hpp"

namespace fmodem::text {

struct Code { uint16_t bits = 0; uint8_t len = 0; };   // first-sent bit in bit 0

// PMC: kPmcContexts[lang][k] is the k-th context character; kPmc[lang][k][sym].
// lang 0 = German (types 4/5), 1 = English (types 6/7). len 0 = unknown.
#if __has_include("pmc_tables.inc")
#include "pmc_tables.inc"
#else
constexpr int kPmcCount = 0;
inline constexpr char kPmcContexts[2][1] = {{0}, {0}};
inline constexpr Code kPmc[2][1][128] = {};
inline constexpr bool kPmcHave[2] = {false, false};
#endif

inline int pmc_context(int lang, int ch)
{
	for (int k = 0; k < kPmcCount; k++) if (uint8_t(kPmcContexts[lang][k]) == ch) return k;
	return -1;
}
inline Code huff(int ch) { const auto &e = kHuff[ch & 0x7f]; return Code{e.code, e.len}; }
inline Code code_for(int type, int prev, int ch)
{
	if (type >= 4) {
		const int lang = (type >> 1) & 1, k = pmc_context(lang, prev);
		if (k >= 0) return kPmc[lang][k][ch & 0x7f];
	}
	return huff(ch);
}
inline int swap_case(int c) { return std::isalpha(c) ? c ^ 0x20 : c; }
inline bool swapped_type(int type) { return type == 2 || type == 5 || type == 7; }

// Decode a data field of `nbytes` with the given data type (0-7).
// p2: a P2 packet. P2 has no 8-bit escape: the symbol 0x7F is DEL and
// nothing follows it [bb-emu: our P2 text with 0x80, 0xE9 or DEL in a PMC
// packet came out of the emulated firmware as DEL + garbage from that point;
// bb-ext: the real reference unit's P2 sent its 0x80 text as an ASCII packet, its P3
// compressed it with the escape, 2026-10-21]. P3 has the escape (below).
// n_idle (optional): the number of idle (0x1e) characters decoded, i.e. the packet's fill
// (a packet without fill was full).
std::string decode(int type, const uint8_t *data, int nbytes, bool p2 = false, int *n_idle = nullptr);

// Byte-stream escapes, applied before any compression:
// - run-length encoding [spec: P3 protocol s.7]: 0x1D, the byte, a
//   count n (1..0x60; n < 0x20 sent as n + 0x60), e.g. "AAAAAAAA" = 1D 41 68.
//   The reference unit sends a repeat at the end of its queued text this way
//   ("ee" -> "e", then 1D 65 61 in the next packet), and a literal 0x1D as
//   1D 1D 61 [bb-ext: binary host-mode transfer of all 256 byte values].
// - 0x1C + (b + 0x60) for b in {0E 0F 10 14 15 16 17 1C 1E 1F} [bb-ext, same
//   transfer: e.g. the idle byte 0x1E travels as 1C 7E]; every other byte
//   travels unchanged.
// Stateful: a sequence may span packets.
inline bool must_escape_1c(uint8_t b)
{
	return b == 0x0E || b == 0x0F || b == 0x10 || (b >= 0x14 && b <= 0x17) || b == 0x1C || b == 0x1E || b == 0x1F;
}
// The system-information (SI) layer on the receive side, the byte sink every received
// character goes through after the run-length expander (its state cleared at every connect),
// for P1, -2 and -3 alike:
// - 0x1C starts an SI and is dropped;
// - the byte after it: >= 0x61 -> the control character b & 0x1F, delivered (the escapes above);
//   'D' (0x44) -> the reference unit's position report (its GPS feature): letters 'a'.. mapped through
//   "7XX60XXX2XX9X1XXX3548 $" into a 14-byte store, nothing printed, ended by a space;
//   any other byte -> skipped, with everything after it up to and including the next space;
// - the state survives packet boundaries.
// Nothing of an SI ever reaches the terminal.
// Signum's own SI (function code 'X', the peer capability record, peer_caps.hpp)
// is skipped the same way, and its body kept with its position in the output, for the
// host layer to read (P2Station::take_si); nothing else changes.
struct SiFilter {
	int state = 0;                                 // 0 plain, 1 after 0x1C, 2 skipping to a space, 3 position report, 4 our 'X' SI
	std::string position;                          // the last position report's characters
	std::string ext;                               // the 'X' SI being read (its body after the 'X')
	struct Ext { size_t pos; std::string body; };  // pos: the output's length when it ended
	std::vector<Ext> ext_done;                     // finished 'X' SIs (at most kExtMax kept until taken)
	static constexpr size_t kExtMax = 16, kExtLen = 160;
	void feed(uint8_t c, std::string &out)
	{
		switch (state) {
		case 0: if (c == 0x1C) state = 1; else out += char(c); break;
		case 1:
			if (c == 0x44) { state = 3; position.clear(); }
			else if (c >= 0x61) { out += char(c & 0x1F); state = 0; }
			else if (c == 'X') { state = 4; ext.clear(); }
			else state = 2;
			break;
		case 2: if (c == 0x20) state = 0; break;
		case 4:
			if (c == 0x20) { state = 0; if (ext_done.size() < kExtMax) ext_done.push_back({out.size(), ext}); break; }
			if (ext.size() < kExtLen) ext += char(c);
			break;
		default:
			if (c == 0x20) { state = 0; break; }
			if (position.size() < 14) {
				static const char kMap[] = "7XX60XXX2XX9X1XXX3548 $";
				const int i = int(c) - 0x61;
				position += (i >= 0 && i < int(sizeof kMap - 1)) ? kMap[i] : '?';
			}
			break;
		}
	}
};

// The run-length expander (when RLe is on): 1D, the byte, the count
// (>= 0x61: count - 0x60, else the count itself), each expanded byte then through the SI layer.
struct RleExpander {
	int state = 0;
	uint8_t code = 0;
	SiFilter si;
	std::string feed(const std::string &in)
	{
		std::string out;
		for (char c0 : in) {
			const uint8_t c = uint8_t(c0);
			switch (state) {
			case 0: if (c == 0x1D) state = 1; else si.feed(c, out); break;
			case 1: code = c; state = 2; break;
			default: {
				static const bool old_count = std::getenv("RLE_OLD_COUNT") != nullptr;   // the old rule: 0x60 -> 0
				const int n = old_count ? (c >= 0x60 ? c - 0x60 : c) : (c >= 0x61 ? c - 0x60 : c);
				for (int k = 0; k < n; k++) si.feed(code, out);
				state = 0; break;
			}
			}
		}
		return out;
	}
};
inline std::string rle_escape(const std::string &q)
{
	std::string o;
	for (char c : q) {
		const uint8_t b = uint8_t(c);
		if (b == 0x1D) o += "\x1d\x1d\x61";
		else if (must_escape_1c(b)) { o += '\x1c'; o += char(b + 0x60); }
		else o += c;
	}
	return o;
}

// The run-length compressor on the send side, byte for byte as the reference unit's. A repeat of the last byte starts a run (the first
// byte has already gone out). When the run ends it is sent literally if it
// has fewer than 4 repeats (12 for spaces), else as 1D ch n; a run is cut at
// 0x60 repeats; a literal 0x1D always travels as a run. flush() is called
// when the send queue runs dry and, like the reference unit, sends runs of fewer
// than 4 repeats as 1D ch n and longer ones literally.
// Output bytes outside a 1D triplet still get the 0x1C escapes.
struct RleCompressor {
	int last = -1, count = 0;
	bool in_run = false;
	int run = 0;
	static void lit(std::string &o, uint8_t b)
	{
		if (must_escape_1c(b)) { o += '\x1c'; o += char(b + 0x60); }
		else o += char(b);
	}
	static void triplet(std::string &o, int ch, int n)
	{
		o += '\x1d'; o += char(ch); o += char(n < 0x20 ? n + 0x60 : n);
	}
	void start_run(int ch) { in_run = true; run = ch; count = 1; }
	std::string feed(const std::string &in)
	{
		std::string o;
		for (char c0 : in) {
			const int c = uint8_t(c0);
			if (in_run) {
				if (c == run) {
					if (++count == 0x60) { triplet(o, run, count); in_run = false; count = 0; }
					continue;
				}
				in_run = false;
				const bool rle = run == 0x1D || (run == 0x20 ? count >= 12 : count >= 4);
				if (rle) triplet(o, run, count);
				else for (int k = 0; k < count; k++) lit(o, uint8_t(run));
				count = 0;
				last = c;
				if (c == 0x1D) { start_run(0x1D); continue; }
				lit(o, uint8_t(c));
				continue;
			}
			if (c == 0x1D) { last = 0x1D; start_run(0x1D); continue; }
			if (c == last) { start_run(c); continue; }
			count = 0; last = c;
			lit(o, uint8_t(c));
		}
		return o;
	}
	std::string flush()
	{
		std::string o;
		if (!in_run) return o;
		in_run = false;
		if (count < 4) triplet(o, run, count);
		else for (int k = 0; k < count; k++) lit(o, uint8_t(run));
		count = 0;
		return o;
	}
};

// Encode as much of `q` as fits into `nbytes` with data type `type`;
// returns the number of characters consumed (data filled with idle codes).
int encode(int type, const std::string &q, uint8_t *data, int nbytes, bool p2 = false);

// The reference unit's choice of data type, with compression on:
//  - "swapped" when the next nbytes+2 characters hold more upper- than
//    lower-case letters;
//  - trial-pack Huffman, PMC German and PMC English into min(nbytes + 1, 80)
//    bytes; the most characters wins against an ASCII baseline of that many
//    bytes, a tie with English goes to English; when all the queued text
//    fits, every compressed type counts 3000, so PMC English wins;
//  - ASCII is never sent: a win for ASCII sends Huffman.
// fit_at_size (fix 239): the types are compared on the packet's own size; P2 unproto does so
// (the emulated reference unit's U 3 packets of one text: 18 of 18 re-chosen alike, "x, juli" type 6 and
// "3 RST=" type 2 at 5 bytes, where the comparison at 6 bytes picks types 4 and 1).
int choose(const std::string &q, int nbytes, bool allow_pmc = true, bool p2 = false, bool fit_at_size = false);

}  // namespace fmodem::text
