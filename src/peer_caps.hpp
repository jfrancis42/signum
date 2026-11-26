// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// peer_caps.hpp: the Signum peer capability record. It
// travels as a system-information sequence of the P1/P2/P3 byte stream with the unassigned function
// code 'X' (0x1C 'X' body 0x20): the reference unit skips such a sequence silently, our SI layer hands
// it to the host layer (compress.hpp SiFilter, P2Station::take_si/send_si). Sent on P2/P3 links only.
// The peer marker (peer_id.hpp) is this record with the product and version only; the protection
// layer (built without SIGNUM_NO_CRYPTO) adds its fields. This file is in every build.
//
// Body (after the 'X'): "FE" + the record format version (one digit, now 1) + fields, each ';' + a tag
// letter + its value. Characters 0x21-0x7E only, never ';' inside a value, never a space, 0x1C-0x1E or
// 0x7F+; at most kCapsMaxBody characters. Unknown tags are skipped; a record with a higher format digit
// is read the same way (a new format keeps the tags it does not change). Tags of format 1:
//   p  product ("Signum")
//   v  software version ("0.5.0"): for the log, never a switch
//   e  encryption: the protocol versions this station will run, comma-separated ("1"); present only
//      while encryption is on
//   a  identity verification without encryption ("verify"): the protocol versions this station will
//      run ("1"); present only while verify is on (0.5.2 and later; older Signums skip it)
//   s  "1": this station asks for an authentication tag on every record (verify mode only)
//   k  "s" when this station has a static key pair (for IK / XX)
//   q  the id of the pre-shared key it would use for this peer, when it has one
//   c  compression for the encryption layer's records, in this station's order of preference,
//      comma-separated: "zen1" = zstd with the dictionary en1; "none" is always possible
// The meaning of e/a/s/k/q/c and the agreement on them: crypto/caps.hpp.
// No OS calls (the core may run on a small controller).
#pragma once
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace signum {

constexpr size_t kCapsMaxBody = 120;

struct Caps {
	int format = 1;
	std::string product, version;
	std::vector<int> enc;                 // e
	std::vector<int> ver;                 // a
	bool sign = false;                    // s
	bool has_static = false;              // k contains 's'
	std::string psk_id;                   // q
	std::vector<std::string> comp;        // c
	bool valid = false;
};

inline std::string caps_list(const std::vector<int> &v)
{
	std::string s;
	for (size_t i = 0; i < v.size(); i++) s += (i ? "," : "") + std::to_string(v[i]);
	return s;
}

std::string caps_body(const Caps &c);

inline std::vector<std::string> split_list(const std::string &v)
{
	std::vector<std::string> r;
	size_t a = 0;
	while (a <= v.size()) { size_t b = v.find(',', a); if (b == std::string::npos) b = v.size(); if (b > a) r.push_back(v.substr(a, b - a)); a = b + 1; }
	return r;
}

// The body of an 'X' SI as the SI layer kept it (after the 'X'). valid=false: not ours or garbled.
Caps parse_caps(const std::string &b);

}  // namespace signum
