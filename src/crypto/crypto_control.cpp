// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// CryptoControl member functions (out of crypto_control.hpp, so that they build in parallel).
#include "crypto_control.hpp"

namespace signum::crypto {

CryptoControl::CryptoControl()
{
	link.key_received = [this](const std::string &call, const Key32 &pub) {
		if (mycall && upper(call) == upper(mycall())) return;      // our own block echoed
		const Peer *p = keys.peer(call);
		if (p && p->pub == pub) return;                          // already known
		const std::string e = keys.note_received(call, pub);
		if (link.log) link.log("a public key for " + upper(call) + " was received over the air (fingerprint " + fingerprint(pub) + "): NOT trusted until you accept it (CRYPTO ACCEPT " + upper(call) + ") after comparing the fingerprint by another channel" + (e.empty() ? "" : " (not saved: " + e + ")"));
		if (link.changed) link.changed();
	};
}

std::string CryptoControl::enable(bool on, const std::string &ack)
{
	const bool given = upper(ack) == kAck;
	if (on && !given && !acked()) return std::string("refused: turning encryption on needs the acknowledgement ") + kAck + ". " + kLegalWarning;
	if (on) { const std::string le = keys.ensure_lock(); if (!le.empty()) return "refused: " + le; }
	const bool was_verify = cfg.verify;
	const std::string e = link.set_on(on);
	if (!e.empty()) return e;
	if (link.log) link.log(on ? std::string("encryption ON. ") + kLegalWarning + " " + kNotCompatible : std::string(was_verify ? "identity check (verify) off" : "encryption off"));
	if (on && given && store_ack && !acked()) {
		const std::string se = store_ack(true);
		if (!se.empty() && link.log) link.log("the acknowledgement was not stored (" + se + "): it will be asked again next time");
	}
	return "";
}

std::string CryptoControl::enable_verify()
{
	if (cfg.verify) return "";
	{ const std::string le = keys.ensure_lock(); if (!le.empty()) return "refused: " + le; }
	const std::string e = link.set_mode("verify");
	if (!e.empty()) return e;
	if (link.log) link.log(std::string("identity check ON (verify): links to other Signums check the other station's key at link start, then carry the data in clear") +
	                       (cfg.sign ? " with a tag on every record" : "") + "; a link where the check cannot be made is ended");
	return "";
}

std::string CryptoControl::verify_text() const
{
	return std::string("identity check (verify) ON: every P2/P3 link checks the other Signum's key at link start (trust ") + trust_name(cfg.vtrust) + "), then the data goes in clear" +
	       (cfg.sign ? ", every record with an authentication tag" : ", checked at link start only (a takeover after the check is not detected; CRYPTO SIGN ON adds a tag to every record)") +
	       "; a link where the check cannot be made is ended";
}

std::string CryptoControl::sign_text(bool on)
{
	return on ? "sign on: in verify mode every record carries a 16-byte authentication tag (data still in clear); a bad tag ends the link. With encryption on every record is authenticated anyway: no extra effect"
	          : "sign off (default): in verify mode the identity is checked at link start only, then the data goes in clear with no overhead";
}

std::string CryptoControl::withdraw()
{
	if (cfg.on) { const std::string e = enable(false, ""); if (!e.empty()) return e; }
	if (!acked()) return "";
	if (!store_ack) return "";
	const std::string e = store_ack(false);
	if (e.empty() && link.log) link.log("the NOT-AMATEUR acknowledgement was withdrawn: turning encryption on asks for it again");
	return e;
}

std::string CryptoControl::verify(const std::string &call, bool v, const std::string &fp)
{
	const Peer *p = keys.peer(call);
	if (!p || fp.empty() || !v) return keys.verify_peer(call, v);
	if (norm_fp(fingerprint(p->pub)) != norm_fp(fp))
		return "the key stored for " + upper(call) + " (" + fingerprint(p->pub) + ") is not the key you are verifying (" + fp + "): nothing was marked verified" +
		       (link.changed_key().set ? ". To trust the key it presented, replace the stored key first (Replace stored key..., CRYPTO PEER REPLACE " + link.changed_key().stored_as + ")" : "");
	const Key32 k = p->pub;
	return keys.verify_peer(call, v, &k);
}

std::string CryptoControl::replace(const std::string &call, const std::string &confirm, bool verified)
{
	const auto &ck = link.changed_key();
	if (!ck.set || (upper(call) != ck.stored_as && upper(call) != upper(ck.call)))
		return "no changed key to replace for " + upper(call) + " (only after a link failed because " + upper(call) + " presented a key other than the stored one)";
	const std::string nf = norm_fp(fingerprint(ck.new_pub)), c = norm_fp(confirm);
	if (c.size() < 4 || nf.compare(0, c.size(), c) != 0)
		return "to replace " + ck.stored_as + "'s stored key " + fingerprint(ck.old_pub) + " with the presented " + fingerprint(ck.new_pub) + ", confirm with the new fingerprint (at least its first 4 digits): CRYPTO PEER REPLACE " +
		       ck.stored_as + " " + nf.substr(0, 4) + " [VERIFIED]";
	const std::string name = ck.stored_as;
	const Key32 oldk = ck.old_pub, k = ck.new_pub;
	const std::string e = keys.add_peer(name, k, verified);
	if (!e.empty()) return e;
	link.clear_changed_key();
	if (link.log) link.log(name + "'s stored key replaced: " + fingerprint(oldk) + " -> " + fingerprint(k) + (verified ? " (verified)" : " (NOT verified)"));
	return "";
}

std::string CryptoControl::command_(const std::string &arg_in)
{
	std::string arg = arg_in;
	while (!arg.empty() && arg.back() == ' ') arg.pop_back();
	std::vector<std::string> w;
	{
		size_t i = 0;
		while (i < arg.size()) {
			while (i < arg.size() && arg[i] == ' ') i++;
			size_t j = arg.find(' ', i);
			if (j == std::string::npos) j = arg.size();
			if (j > i) w.push_back(arg.substr(i, j - i));
			i = j;
		}
	}
	const std::string s0 = w.size() > 0 ? upper(w[0]) : "STATUS", s1 = w.size() > 1 ? upper(w[1]) : "", s2 = w.size() > 2 ? w[2] : "";
	auto ok = [](const std::string &t) { return "*** CRYPTO: " + t + "\r\n"; };
	auto err = [](const std::string &t) { return "*** ERROR: CRYPTO: " + t + "\r\n"; };
	auto res = [&](const std::string &e, const std::string &good) { return e.empty() ? ok(good) : err(e); };
	auto crlf = [](std::string t) { std::string o; for (char c : t) { if (c == '\n') o += "\r\n"; else o += c; } return o; };
	if (s0 == "STATUS") {
		std::string t = ok(std::string("encryption ") + (cfg.on ? "ON" : cfg.verify ? "off, identity check (verify) ON" : "off") + ", sign " + (cfg.sign ? "on" : "off") + ", trust " + trust_name(cfg.trust) + " (encryption) / " + trust_name(cfg.vtrust) + " (verify), wait " + std::to_string(int(cfg.wait_s)) + " s, compression " + (cfg.compress ? "on" : "off") + ", keys in " + keys.dir());
		t += ok("link: " + link.status_line());
		t += ok(keys.has_key() ? "this station's key: " + fingerprint(keys.me().pub) : std::string("this station has no key pair (CRYPTO KEYGEN)"));
		t += ok(std::to_string(keys.peers().size()) + " known stations, " + std::to_string(keys.received().size()) + " keys received over the air waiting, " + std::to_string(keys.psks().size()) + " pre-shared keys");
		if (!keys.locked() && !keys.lock_error().empty()) t += err(keys.lock_error());
		if (link.changed_key().set) { const auto &ck = link.changed_key(); t += ok(ck.stored_as + " presented a key other than the stored one: stored " + fingerprint(ck.old_pub) + ", presented " + fingerprint(ck.new_pub) + " (CRYPTO PEER REPLACE " + ck.stored_as + " " + norm_fp(fingerprint(ck.new_pub)).substr(0, 4) + " [VERIFIED] after confirming it with its operator)"); }
		return t;
	}
	if (s0 == "ON") {
		if (s1 != kAck && !acked()) return ok(std::string(kLegalWarning)) + ok(kNotCompatible) + ok(kNoPlainFallback) + ok(std::string("to turn it on, type: CRYPTO ON ") + kAck);
		const std::string e = enable(true, s1);
		return e.empty() ? ok(std::string("encryption ON. ") + kLegalWarning) + ok(kNotCompatible) : err(e);
	}
	if (s0 == "OFF") { const bool v = cfg.verify; return res(enable(false, ""), v ? "identity check (verify) off" : "encryption off"); }
	if (s0 == "VERIFY") {
		if (!s1.empty()) return err("VERIFY (no argument; the trust policy: TRUST VERIFY STORED|TOFU|VERIFIED)");
		return res(enable_verify(), verify_text());
	}
	if (s0 == "SIGN") {
		if (s1 != "ON" && s1 != "OFF") return err("SIGN ON or SIGN OFF");
		cfg.sign = s1 == "ON";
		if (link.changed) link.changed();
		return ok(sign_text(cfg.sign) + " (from the next link)");
	}
	if (s0 == "ACK") {
		if (s1 == "OFF") { const bool had = acked(); const std::string e = withdraw(); return res(e, had ? "acknowledgement withdrawn, encryption off: turning it on asks for NOT-AMATEUR again" : "no acknowledgement was stored; encryption off"); }
		if (!s1.empty()) return err("ACK or ACK OFF");
		return ok(acked() ? "the NOT-AMATEUR acknowledgement is stored in the settings file: CRYPTO ON works without it (CRYPTO ACK OFF withdraws it)" : "no acknowledgement stored: the first CRYPTO ON needs NOT-AMATEUR");
	}
	if (s0 == "TRUST") {
		// TRUST policy: encryption's (as before); TRUST ENCRYPT policy the same; TRUST VERIFY policy: verify's.
		const bool forv = s1 == "VERIFY", fore = s1 == "ENCRYPT";
		const std::string pv = forv || fore ? s2 : s1;
		Trust t;
		if (!parse_trust(pv, t)) return err("TRUST [ENCRYPT|VERIFY] STORED|TOFU|VERIFIED");
		(forv ? cfg.vtrust : cfg.trust) = t;
		if (link.changed) link.changed();
		const char *how = t == Trust::VERIFIED ? "verified: only keys you verified" : t == Trust::TOFU ? "tofu: a new station's key is stored unverified at first contact" : "stored: only a key already stored here (verified or not)";
		return ok(std::string("trust for ") + (forv ? "verify" : "encryption") + ": " + how);
	}
	if (s0 == "COMPRESS") {
		if (s1 != "ON" && s1 != "OFF") return err("COMPRESS ON or COMPRESS OFF");
		cfg.compress = s1 == "ON";
		if (link.changed) link.changed();
		return ok(std::string("compression ") + (cfg.compress ? "on: records are compressed (zstd) when the other Signum offers it too" : "off: records go uncompressed") + " (from the next link)");
	}
	if (s0 == "WAIT") {
		const int v = std::atoi(s1.c_str());
		if (v < 10 || v > 600) return err("WAIT 10-600 (seconds)");
		cfg.wait_s = v;
		return ok("wait " + std::to_string(v) + " s");
	}
	if (s0 == "KEYGEN") {
		const std::string e = keys.generate(s1 == "FORCE");
		return e.empty() ? ok("new key pair, fingerprint " + fingerprint(keys.me().pub) + " (CRYPTO MYKEY shows the public key to give to others)") : err(e);
	}
	if (s0 == "MYKEY") {
		if (!keys.has_key()) return err("no key pair (CRYPTO KEYGEN)");
		if (s1 == "PRIVATE") return ok("SECRET: anyone with this block can pose as this station") + crlf(keys.private_block());
		return crlf(keys.public_block(mycall ? mycall() : ""));
	}
	if (s0 == "SENDKEY") {
		if (!keys.has_key()) return err("no key pair (CRYPTO KEYGEN)");
		if (link_is_up && !link_is_up()) return err("no link: SENDKEY sends this station's public key to the station at the other end of the link");
		link.host_send(keys.public_block(mycall ? mycall() : "", "\r"));
		return ok(std::string("public key queued for the link") + (cfg.on ? " (inside the encrypted session)" : " (in clear: a public key is not secret)"));
	}
	if (s0 == "PEERS") {
		std::string t;
		for (auto &p : keys.peers()) t += ok(p.first + "  " + fingerprint(p.second.pub) + (p.second.verified ? "  verified" : "  NOT verified"));
		return t.empty() ? ok("no known stations") : t;
	}
	if (s0 == "PEER") {
		const std::string call = upper(s2);
		if (s1 == "VERIFY" || s1 == "UNVERIFY") {
			const Peer *p = keys.peer(call);
			const std::string fp = p ? fingerprint(p->pub) : "";
			const auto &ck = link.changed_key();
			const std::string note = s1 == "VERIFY" && ck.set && ck.stored_as == call ? " (the STORED key " + fp + "; the key " + call + " presented on the last link, " + fingerprint(ck.new_pub) + ", is not stored: CRYPTO PEER REPLACE " + call + " to trust that one instead)" : "";
			return res(keys.verify_peer(call, s1 == "VERIFY"), call + (s1 == "VERIFY" ? " verified, fingerprint " + fp + note : " no longer verified"));
		}
		if (s1 == "REPLACE") {
			const std::string e = replace(call, w.size() > 3 ? w[3] : "", w.size() > 4 && upper(w[4]) == "VERIFIED");
			return e.empty() ? ok(call + "'s stored key replaced with the key it presented, " + fingerprint(keys.peer(call)->pub) + (keys.peer(call)->verified ? ", verified" : ", NOT verified")) : err(e);
		}
		if (s1 == "DELETE") return res(keys.delete_peer(call), call + "'s key deleted");
		if (s1 == "ADD") {
			Key32 k;
			if (w.size() < 4 || !unb64_32(w[3], k)) return err("PEER ADD CALL KEY [VERIFIED] (KEY: the 44 base64 characters of its Key line)");
			const bool v = w.size() > 4 && upper(w[4]) == "VERIFIED";
			return res(keys.add_peer(call, k, v), call + " added, fingerprint " + fingerprint(k) + (v ? ", verified" : ", NOT verified"));
		}
		if (s1 == "SHOW") { const Peer *p = keys.peer(call); return p ? crlf(make_block({"PUBLIC KEY", call, p->pub})) : err("no known key for " + call); }
		return err("PEER ADD|VERIFY|UNVERIFY|DELETE|SHOW CALL, PEER REPLACE CALL NEWFP [VERIFIED]");
	}
	if (s0 == "RECEIVED") {
		std::string t;
		for (auto &p : keys.received()) t += ok(p.first + "  " + fingerprint(p.second.pub) + "  (received over the air, not trusted)");
		return t.empty() ? ok("no keys received over the air") : t;
	}
	if (s0 == "ACCEPT") return res(keys.accept_received(s1, w.size() > 2 && upper(w[2]) == "VERIFIED"), s1 + "'s received key accepted" + (w.size() > 2 && upper(w[2]) == "VERIFIED" ? " and verified" : ", NOT verified"));
	if (s0 == "REJECT") return res(keys.reject_received(s1), s1 + "'s received key dropped");
	if (s0 == "IMPORT") {
		if (s1 != "FILE" || w.size() < 3) return err("IMPORT FILE PATH [FORCE] (a file holding key blocks)");
		std::string t;
		FILE *f = std::fopen(arg.substr(arg.find(w[2])).c_str(), "r");
		if (!f) return err("cannot read " + w[2]);
		char b[4096]; size_t n;
		while ((n = std::fread(b, 1, sizeof b, f)) > 0 && t.size() < (1u << 20)) t.append(b, n);
		std::fclose(f);
		const std::string r = import_text(t, "", false);
		wipe(&t[0], t.size());
		return r.compare(0, 5, "ERROR") == 0 ? err(r.substr(7)) : ok(r);
	}
	if (s0 == "PSK") {
		const std::string name = upper(s2);
		if (s1.empty() || s1 == "LIST") {
			std::string t;
			for (auto &p : keys.psks()) t += ok("pre-shared key " + p.first + "  id " + psk_id(p.second));
			return t.empty() ? ok("no pre-shared keys") : t;
		}
		if (s1 == "GEN") {
			Key32 k;
			if (!random_bytes(k.data(), 32)) return err("the random generator failed");
			const std::string e = keys.set_psk(name, k);
			return e.empty() ? ok("pre-shared key " + name + " made, id " + psk_id(k) + " (CRYPTO PSK SHOW " + name + " to export it)") : err(e);
		}
		if (s1 == "SET") {
			Key32 k;
			if (w.size() < 4 || !unb64_32(w[3], k)) return err("PSK SET NAME KEY (KEY: 44 base64 characters)");
			return res(keys.set_psk(name, k), "pre-shared key " + name + " set, id " + psk_id(k));
		}
		if (s1 == "PHRASE") {
			const size_t at = w.size() > 3 ? arg.find(w[3], arg.find(w[2]) + w[2].size()) : std::string::npos;
			if (at == std::string::npos) return err("PSK PHRASE NAME the passphrase (at least 20 characters)");
			const std::string phrase = arg.substr(at);
			if (phrase.size() < 20) return err("a passphrase of at least 20 characters (better: PSK GEN)");
			Key32 k;
			if (!psk_from_phrase(phrase, k)) return err("the key derivation failed");
			return res(keys.set_psk(name, k), "pre-shared key " + name + " from the passphrase, id " + psk_id(k));
		}
		if (s1 == "DELETE") return res(keys.delete_psk(name), "pre-shared key " + name + " deleted");
		if (s1 == "SHOW") {
			for (auto &p : keys.psks()) if (p.first == name) return ok("SECRET: anyone with this block can read this traffic") + crlf(make_block({"PRE-SHARED KEY", name, p.second}));
			return err("no pre-shared key named " + name);
		}
		return err("PSK [LIST] | GEN NAME | SET NAME KEY | PHRASE NAME words... | SHOW NAME | DELETE NAME (NAME: a call sign, or * for every station)");
	}
	return err("STATUS | ON " + std::string(kAck) + " | VERIFY | OFF | SIGN ON|OFF | ACK OFF | TRUST [ENCRYPT|VERIFY] STORED|TOFU|VERIFIED | COMPRESS ON|OFF | WAIT S | KEYGEN [FORCE] | MYKEY [PRIVATE] | SENDKEY | PEERS | PEER ... (ADD, VERIFY, UNVERIFY, DELETE, SHOW, REPLACE) | RECEIVED | ACCEPT CALL [VERIFIED] | REJECT CALL | IMPORT FILE PATH | PSK ...");
}

std::string CryptoControl::import_text(const std::string &text, const std::string &call, bool force)
{
	std::string perr;
	const auto bl = parse_blocks(text, &perr);
	if (bl.empty()) return "ERROR: " + (perr.empty() ? std::string("no SIGNUM key block found") : perr);
	std::string r;
	for (const auto &b : bl) {
		std::string e;
		if (b.kind == "PUBLIC KEY") {
			const std::string c = b.name.empty() ? upper(call) : b.name;
			if (c.empty()) e = "a public key block without a Call line (give the call)";
			else { e = keys.add_peer(c, b.key, false); if (e.empty()) r += "public key of " + c + " (" + fingerprint(b.key) + ", NOT verified); "; }
		} else if (b.kind == "PRIVATE KEY") {
			if (keys.has_key() && !force) e = "this station already has a key pair (import the private key with force to replace it)";
			else { e = keys.set_private(b.key); if (e.empty()) r += "this station's key pair (" + fingerprint(keys.me().pub) + "); "; }
		} else {
			if (b.name.empty()) e = "a pre-shared key block without a Name line";
			else { e = keys.set_psk(b.name, b.key); if (e.empty()) r += "pre-shared key " + b.name + " (id " + psk_id(b.key) + "); "; }
		}
		if (!e.empty()) return "ERROR: " + e + (r.empty() ? "" : " (already imported: " + r + ")");
	}
	return "imported: " + r;
}

std::string CryptoControl::status_json() const
{
	auto q = [](const std::string &s) { return jsonm::quote(s); };
	std::string j = "{\"available\":true,\"on\":" + std::string(cfg.on ? "true" : "false") + ",\"protection\":" + q(cfg.mode_name()) + ",\"sign\":" + (cfg.sign ? "true" : "false") +
	                ",\"verify_trust\":" + q(trust_name(cfg.vtrust)) + ",\"ack\":" + q(kAck) + ",\"ack_stored\":" + (acked() ? "true" : "false") + ",\"warning\":" + q(kLegalWarning) +
	                ",\"not_compatible\":" + q(kNotCompatible) + ",\"no_plain\":" + q(kNoPlainFallback) +
	                ",\"trust\":" + q(trust_name(cfg.trust)) + ",\"compress\":" + (cfg.compress ? "true" : "false") + ",\"wait_s\":" + std::to_string(int(cfg.wait_s)) + ",\"key_dir\":" + q(keys.dir()) + ",\"key_dir_locked\":" + (keys.locked() ? "true" : "false") + ",\"key_dir_error\":" + q(keys.locked() ? "" : keys.lock_error()) +
	                ",\"have_key\":" + (keys.has_key() ? "true" : "false") + ",\"fingerprint\":" + q(keys.has_key() ? fingerprint(keys.me().pub) : "") +
	                ",\"public_block\":" + q(keys.has_key() ? keys.public_block(mycall ? mycall() : "") : "");
	j += ",\"peers\":[";
	bool first = true;
	for (auto &p : keys.peers()) { j += (first ? "" : ",") + std::string("{\"call\":") + q(p.first) + ",\"fingerprint\":" + q(fingerprint(p.second.pub)) + ",\"verified\":" + (p.second.verified ? "true" : "false") + ",\"since\":" + std::to_string(p.second.since) + "}"; first = false; }
	j += "],\"received\":[";
	first = true;
	for (auto &p : keys.received()) { j += (first ? "" : ",") + std::string("{\"call\":") + q(p.first) + ",\"fingerprint\":" + q(fingerprint(p.second.pub)) + ",\"when\":" + std::to_string(p.second.since) + "}"; first = false; }
	j += "],\"psks\":[";
	first = true;
	for (auto &p : keys.psks()) { j += (first ? "" : ",") + std::string("{\"name\":") + q(p.first) + ",\"id\":" + q(psk_id(p.second)) + "}"; first = false; }
	j += "],\"link\":" + link.status_json() + "}";
	return j;
}

std::string CryptoControl::request_(const jsonm::Value &v)
{
	const std::string op = v.str("op");
	auto q = [](const std::string &s) { return jsonm::quote(s); };
	auto done = [&](const std::string &e, const std::string &good, const std::string &extra = "") {
		return "\"ok\":" + std::string(e.empty() ? "true" : "false") + ",\"text\":" + q(e.empty() ? good : e) + extra;
	};
	auto flag = [&](const char *k) { const jsonm::Value *x = v.get(k); return x && x->kind == jsonm::Value::BOOL && x->b; };
	const std::string call = upper(v.str("call")), name = upper(v.str("name"));
	if (op == "status" || op.empty()) return done("", "", ",\"crypto\":" + status_json());
	if (op == "set") {
		std::string e;
		if (const jsonm::Value *x = v.get("on")) if (x->kind == jsonm::Value::BOOL && x->b != cfg.on) e = enable(x->b, v.str("ack"));
		if (e.empty() && v.has("protection")) {
			const std::string m = v.str("protection");
			if (m == "on") { if (!cfg.on) e = enable(true, v.str("ack")); }
			else if (m == "verify") e = enable_verify();
			else if (m == "off") { if (cfg.active()) e = enable(false, ""); }
			else e = "protection: off, verify or on";
		}
		if (e.empty() && v.has("sign")) { const jsonm::Value *x = v.get("sign"); if (x && x->kind == jsonm::Value::BOOL) cfg.sign = x->b; else e = "sign: true or false"; }
		if (e.empty() && v.has("trust")) { Trust t; if (parse_trust(v.str("trust"), t)) cfg.trust = t; else e = "trust: stored, tofu or verified"; }
		if (e.empty() && v.has("verify_trust")) { Trust t; if (parse_trust(v.str("verify_trust"), t)) cfg.vtrust = t; else e = "verify_trust: stored, tofu or verified"; }
		if (e.empty() && v.has("compress")) { const jsonm::Value *x = v.get("compress"); if (x && x->kind == jsonm::Value::BOOL) cfg.compress = x->b; else e = "compress: true or false"; }
		if (e.empty() && v.has("wait_s")) { const double s = v.num("wait_s"); if (s >= 10 && s <= 600) cfg.wait_s = s; else e = "wait_s: 10-600"; }
		if (link.changed) link.changed();
		return done(e, "ok");
	}
	if (op == "ack_withdraw") { const bool had = acked(); return done(withdraw(), had ? "acknowledgement withdrawn, encryption off" : "no acknowledgement was stored; encryption off"); }
	if (op == "keygen") {
		const std::string e = keys.generate(flag("force"));
		return done(e, "new key pair", e.empty() ? ",\"fingerprint\":" + q(fingerprint(keys.me().pub)) : "");
	}
	if (op == "export") {
		const std::string what = v.str("what", "public");
		if (what == "public") return keys.has_key() ? done("", "", ",\"block\":" + q(keys.public_block(mycall ? mycall() : ""))) : done("no key pair", "");
		if (what == "private") return keys.has_key() ? done("", "", ",\"block\":" + q(keys.private_block())) : done("no key pair", "");
		if (what == "peer") { const Peer *p = keys.peer(call); return p ? done("", "", ",\"block\":" + q(make_block({"PUBLIC KEY", call, p->pub}))) : done("no known key for " + call, ""); }
		if (what == "psk") { for (auto &p : keys.psks()) if (p.first == name) return done("", "", ",\"block\":" + q(make_block({"PRE-SHARED KEY", name, p.second}))); return done("no pre-shared key named " + name, ""); }
		return done("what: public, private, peer or psk", "");
	}
	if (op == "import") { const std::string r = import_text(v.str("text"), call, flag("force")); return r.compare(0, 5, "ERROR") == 0 ? done(r.substr(7), "") : done("", r); }
	if (op == "peer_add") {
		Key32 k;
		if (!unb64_32(v.str("key"), k)) return done("key: 44 base64 characters", "");
		return done(keys.add_peer(call, k, flag("verified")), call + " added");
	}
	if (op == "peer_verify") return done(verify(call, !v.has("verified") || flag("verified"), v.str("fingerprint")), call + (flag("verified") || !v.has("verified") ? " verified" : " not verified"));
	if (op == "peer_replace") return done(replace(call, v.str("fingerprint"), flag("verified")), call + "'s stored key replaced" + (flag("verified") ? " and verified" : ", NOT verified"));
	if (op == "peer_delete") return done(keys.delete_peer(call), call + " deleted");
	if (op == "received_accept") return done(keys.accept_received(call, flag("verified")), call + " accepted");
	if (op == "received_reject") return done(keys.reject_received(call), call + " dropped");
	if (op == "send_key") {
		const std::string r = command("SENDKEY");
		return done(r.find("ERROR") != std::string::npos ? r.substr(r.find(": ", 12) + 2) : "", "public key queued for the link");
	}
	if (op == "psk_gen" || op == "psk_set" || op == "psk_phrase") {
		Key32 k;
		if (op == "psk_gen" && !random_bytes(k.data(), 32)) return done("the random generator failed", "");
		if (op == "psk_set" && !unb64_32(v.str("key"), k)) return done("key: 44 base64 characters", "");
		if (op == "psk_phrase") { const std::string p = v.str("phrase"); if (p.size() < 20) return done("a passphrase of at least 20 characters", ""); if (!psk_from_phrase(p, k)) return done("the key derivation failed", ""); }
		const std::string e = keys.set_psk(name, k);
		return done(e, "pre-shared key " + name, e.empty() ? ",\"psk_id\":" + q(psk_id(k)) : "");   // (not "id": that is the request's, echoed)
	}
	if (op == "psk_delete") return done(keys.delete_psk(name), name + " deleted");
	return done("unknown op: " + op, "");
}

}  // namespace signum::crypto
