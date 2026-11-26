// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// caps.hpp: what two Signums' capability records (the record itself: src/peer_caps.hpp) agree on for
// the protection layer (link_crypto.hpp). Sent on P2/P3 links only, and only while a protection is on.
//
// The protection a station asks for is in its record: encryption ('e', the protocol versions) or
// identity verification without encryption, "verify" ('a', the protocol versions; 's1' when it also
// asks for a tag on every record). Version 1 of both: Noise NNpsk0 / IK / IKpsk2 / XX / XXpsk3 with
// 25519, AESGCM, SHA256; frames in the air code v1 (aircode.hpp); records as in link_crypto.hpp.
// Negotiation (docs/md/encryption.md "Two Signums with different settings"):
//   - both ask for encryption: the highest common version (as before verify existed);
//   - both ask for verify: the highest common verify version; every record is signed when EITHER asks
//     for it (the stronger request wins: a tag never weakens anything and needs no acknowledgement);
//   - one asks for encryption, the other for verify: the link is ended. Encryption is never turned on
//     for a station that did not ask for it (it needs that station's NOT-AMATEUR acknowledgement), and
//     an encrypting station never accepts less;
//   - one asks for nothing (off, the reference unit, an older Signum without verify): ended.
// Compression ('c'): the caller's first choice that the callee also lists (encryption only). Both ends
// compute the same from the same two records; no extra round trip. The two records go into the Noise
// prologue, so a station in the middle that strips or changes one makes the handshake fail (downgrade
// protection).
#pragma once
#include <algorithm>
#include <string>
#include <vector>

#include "peer_caps.hpp"

namespace signum::crypto {

using signum::Caps;
using signum::caps_body;
using signum::parse_caps;
using signum::split_list;
constexpr size_t kMaxBody = kCapsMaxBody;

// What two records agree on. reason: why the protection cannot run (empty when it can).
struct Agreement {
	enum Prot { NONE, ENCRYPT, VERIFY } prot = NONE;
	int enc = 0;                          // the agreed protocol version (of e or a); 0: none
	bool sign = false;                    // verify: a tag on every record
	std::string comp = "none";
	enum Mode { KNONE, PSK_ONLY, STATIC, STATIC_PSK } mode = KNONE;
	std::string reason;
	std::string summary;                  // "encryption v1, compression zen1, keys + PSK"
};

// caller/callee: the two records (the caller's preference decides compression). we_call: this station
// is the caller (the reasons speak of "the other station").
Agreement agree(const Caps &caller, const Caps &callee, bool we_call = true);

}  // namespace signum::crypto
