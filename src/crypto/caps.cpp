// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// The functions of caps.hpp (out of the header, so that they build in parallel).
#include "caps.hpp"

namespace signum::crypto {

Agreement agree(const Caps &caller, const Caps &callee, bool we_call)
{
	Agreement a;
	const Caps &me = we_call ? caller : callee, &peer = we_call ? callee : caller;
	const bool me_e = !me.enc.empty(), me_v = !me.ver.empty(), p_e = !peer.enc.empty(), p_v = !peer.ver.empty();
	if (me_e && p_e) {
		a.prot = Agreement::ENCRYPT;
		for (int v : caller.enc) if (std::find(callee.enc.begin(), callee.enc.end(), v) != callee.enc.end()) a.enc = std::max(a.enc, v);
		if (!a.enc) a.reason = "no common encryption version (caller " + caps_list(caller.enc) + ", callee " + caps_list(callee.enc) + ")";
		for (const auto &m : caller.comp) if (std::find(callee.comp.begin(), callee.comp.end(), m) != callee.comp.end()) { a.comp = m; break; }
	} else if (me_v && p_v) {
		a.prot = Agreement::VERIFY;
		for (int v : caller.ver) if (std::find(callee.ver.begin(), callee.ver.end(), v) != callee.ver.end()) a.enc = std::max(a.enc, v);
		if (!a.enc) a.reason = "no common identity-check version (caller " + caps_list(caller.ver) + ", callee " + caps_list(callee.ver) + ")";
		a.sign = caller.sign || callee.sign;
	} else if (me_e && p_v)
		a.reason = "the other station asks for identity verification only (verify, data in clear); this station requires encryption, which is never weakened: link ended";
	else if (me_v && p_e)
		a.reason = "the other station requires encryption; this station is set to identity verification only (verify), which never turns into encryption: link ended";
	else if (me_e)
		a.reason = "encryption is off at the other station";
	else if (me_v)
		a.reason = "the other station is a Signum without the modem-layer identity check on (verify off there, or a version before 0.5.2): modem-layer identity check unavailable; link ended";
	else
		a.reason = "this station asks for no protection";
	if (a.reason.empty()) {
		const bool both_static = caller.has_static && callee.has_static;
		const bool both_psk = !caller.psk_id.empty() && !callee.psk_id.empty();
		if (both_psk && caller.psk_id != callee.psk_id) a.reason = "the two stations' pre-shared keys differ (ids " + caller.psk_id + " and " + callee.psk_id + ")";
		else if (both_static) a.mode = both_psk ? Agreement::STATIC_PSK : Agreement::STATIC;
		else if (both_psk) a.mode = Agreement::PSK_ONLY;
		else if (!caller.psk_id.empty() || !callee.psk_id.empty())
			a.reason = std::string("only the ") + (caller.psk_id.empty() ? "callee" : "caller") + " has a pre-shared key for this link, and the two do not both have a key pair";
		else a.reason = "no common key material: neither a pre-shared key on both sides nor a key pair on both sides";
	}
	if (a.reason.empty()) {
		const std::string keys = a.mode == Agreement::PSK_ONLY ? "pre-shared key" : a.mode == Agreement::STATIC ? "station keys" : "station keys + pre-shared key";
		a.summary = a.prot == Agreement::ENCRYPT ? "encryption v" + std::to_string(a.enc) + ", compression " + a.comp + ", " + keys
		                                         : "identity check v" + std::to_string(a.enc) + (a.sign ? ", every record signed" : ", at link start only") + ", " + keys;
	}
	return a;
}

}  // namespace signum::crypto
