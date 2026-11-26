// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// The functions of compress.hpp (out of the header, so that they build in parallel).
#include "compress.hpp"

namespace fmodem::text {

std::string decode(int type, const uint8_t *data, int nbytes, bool p2, int *n_idle)
{
	std::string out;
	int idle_dummy = 0;
	int &nidle = n_idle ? *n_idle : idle_dummy;
	nidle = 0;
	if (type == 0 || type == 3) {
		for (int i = 0; i < nbytes; i++) { if (data[i] != IDLE_RS) out += char(data[i]); else nidle++; }
		return out;
	}
	if (type >= 4 && !kPmcHave[(type >> 1) & 1]) return out;
	const int nbits = nbytes * 8;
	auto bit = [&](int p) { return (data[p >> 3] >> (p & 7)) & 1; };
	int pos = 0, prev = IDLE_RS;
	while (pos < nbits) {
		int found = -1, flen = 0;
		for (int ch = 0; ch < 128 && found < 0; ch++) {
			const Code c = code_for(type, prev, ch);
			if (!c.len || pos + c.len > nbits) continue;
			bool ok = true;
			for (int i = 0; i < c.len && ok; i++) ok = bit(pos + i) == ((c.bits >> i) & 1);
			if (ok) { found = ch; flen = c.len; }
		}
		if (found < 0) break;                          // the cut-off last idle code (or an unknown code)
		pos += flen;
		if (found == 0x7F && p2) { prev = 0x7F; out += char(0x7F); continue; }   // P2: DEL, no escape
		if (found == 0x7F) {                           // byte >= 0x7F: 0x7F, then its low 7 bits raw;
			if (pos + 7 > nbits) break;                // low bits 0: one more bit, 1 = 0x7F, 0 = 0x80
			int v = 0;
			for (int i = 0; i < 7; i++) v |= bit(pos + i) << i;
			pos += 7;
			if (!v) {
				if (pos + 1 > nbits) break;
				v = bit(pos) ? 0x7F : 0x80;
				pos += 1;
				prev = v;
			} else prev = 0x80 | v;
			out += char(prev);
			continue;
		}
		prev = found;
		if (found == IDLE_RS) { nidle++; continue; }
		out += char(swapped_type(type) ? swap_case(found) : found);
	}
	return out;
}

int encode(int type, const std::string &q, uint8_t *data, int nbytes, bool p2)
{
	std::memset(data, 0, size_t(nbytes));
	if (type == 0) {
		const int n = std::min<int>(nbytes, int(q.size()));
		for (int i = 0; i < nbytes; i++) data[i] = i < n ? uint8_t(q[size_t(i)]) : IDLE_RS;
		return n;
	}
	const int nbits = nbytes * 8;
	int pos = 0, prev = IDLE_RS, used = 0;
	auto put = [&](Code c) {
		for (int i = 0; i < c.len && pos < nbits; i++, pos++)
			if ((c.bits >> i) & 1) data[pos >> 3] |= uint8_t(1u << (pos & 7));
	};
	const bool swapped = swapped_type(type);
	// A character that does not fit is still written, cut off at the end of
	// the field, and not counted as sent: the firmware's own packets end so
	// (99 of 99 of its packets of one text re-encode bit-exact; idle there
	// made 3 differ in the last bits). Idle fills only after the queue runs
	// out. Fix 51: after a packet ending "…a" + idle bits "100" the emulated
	// firmware refused every later packet, at every level, until timeout.
	bool cut = false;
	for (char ch0 : q) {
		const int ch = swapped ? swap_case(uint8_t(ch0)) : uint8_t(ch0);
		if (ch >= 128 && p2) break;                    // P2: no 8-bit escape: the byte goes in an ASCII packet (choose)
		if (ch == 127 && p2) {                         // P2: DEL is the bare symbol
			const Code e = code_for(type, prev, 0x7F);
			if (!e.len) break;
			if (pos + e.len > nbits) { put(e); cut = true; break; }
			put(e); prev = ch; used++;
			continue;
		}
		if (ch >= 127) {                               // 0x7F escape + the low 7 bits; 0x7F and 0x80
			const Code e = code_for(type, prev, 0x7F); // take 8 bits: 0x80 resp. 0x00
			const Code v = ch == 0x7F ? Code{0x80, 8} : ch == 0x80 ? Code{0x00, 8} : Code{uint16_t(ch & 0x7F), 7};
			if (!e.len) break;
			if (pos + e.len + v.len > nbits) { put(e); put(v); cut = true; break; }
			put(e); put(v); prev = ch; used++;
			continue;
		}
		const Code c = code_for(type, prev, ch);
		if (!c.len) break;
		if (pos + c.len > nbits) { put(c); cut = true; break; }
		put(c); prev = ch; used++;
	}
	while (!cut && pos < nbits) {                      // idle fill, the last code cut off
		const Code c = code_for(type, prev, IDLE_RS);
		if (!c.len) break;
		put(c); prev = IDLE_RS;
	}
	return used;
}

int choose(const std::string &q, int nbytes, bool allow_pmc, bool p2, bool fit_at_size)
{
	if (p2 && !q.empty() && uint8_t(q[0]) >= 0x80) return 0;   // P2: an 8-bit byte next: an ASCII packet (as the firmware does)
	int up = 0, lo = 0;
	for (size_t i = 0; i < q.size() && i < size_t(nbytes) + 2; i++) {
		const uint8_t c = uint8_t(q[i]);
		if (c >= 'A' && c <= 'Z') up++;
		else if (c >= 'a' && c <= 'z') lo++;
	}
	const bool swap = lo < up;
	const int n = std::min(fit_at_size ? nbytes : nbytes + 1, 80);
	std::string buf(size_t(n), '\0');
	auto fit = [&](int t) {                            // all of q fits: n + 1, above any partial fit (PMC English wins, as with the reference unit)
		const int k = encode(t, q, reinterpret_cast<uint8_t *>(&buf[0]), n, p2);
		return k >= int(q.size()) ? n + 1 : k;
	};
	int pick = 0, best = n;
	if (fit(swap ? 2 : 1) > best) { pick = 1; best = fit(swap ? 2 : 1); }
	if (allow_pmc && kPmcHave[0] && fit(swap ? 5 : 4) > best) { pick = 2; best = fit(swap ? 5 : 4); }
	if (allow_pmc && kPmcHave[1] && fit(swap ? 7 : 6) >= best) pick = 3;
	return pick == 2 ? (swap ? 5 : 4) : pick == 3 ? (swap ? 7 : 6) : (swap ? 2 : 1);
}

}  // namespace fmodem::text
