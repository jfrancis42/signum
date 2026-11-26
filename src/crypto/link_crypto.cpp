// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// LinkCrypto member functions (out of link_crypto.hpp, so that they build in parallel).
#include "link_crypto.hpp"

namespace signum::crypto {

void LinkCrypto::link_up(const std::string &call, bool caller, int plevel, double t)
{
	guard([&] {
		reset_session();
		last_fail_call_.clear(); last_fail_reason_.clear();   // a new link: the last one's failure is no longer shown
		call_ = call; caller_ = caller; t0_ = t;
		if (!cfg_.active()) { st_ = PLAIN; notify(); return; }
		prot_ = cfg_.on ? Agreement::ENCRYPT : Agreement::VERIFY;
		if (plevel < 2) { fail(std::string(cfg_.on ? "encryption" : "the modem-layer identity check") + " needs a P2 or P3 link (this link is P1, which cannot carry the capability record)", false); return; }
		my_caps_ = make_caps();
		my_body_ = caps_body(my_caps_);
		st_ = WAIT_CAPS;
		modem_si(my_body_);
		say("link up with " + call + ": capability record sent (" + my_body_ + "); waiting for the other station's");
		notify();
	});
}

void LinkCrypto::link_down(double)
{
	if (st_ == IDLE) return;
	if (!pend_.empty()) say(std::to_string(pend_.size()) + (prot_ == Agreement::VERIFY ? " bytes held for the link were discarded (never sent unverified)" : " bytes held for the link were discarded (never sent in clear)"));
	if (!rxbuf_.empty()) say(std::to_string(rxbuf_.size()) + " bytes received without their tag were discarded (never passed on)");
	if (st_ == SECURE) say("link ended; the session keys are gone (a new link makes new ones)");
	if (st_ == FAILED) { last_fail_call_ = call_; last_fail_reason_ = reason_; }   // shown (the panel's Key lamp red) until the next link
	reset_session();
	pend_.clear();
	st_ = IDLE;
	notify();
}

std::string LinkCrypto::rx(const std::string &text, const std::vector<std::pair<size_t, std::string>> &si, double t)
{
	std::string out;
	guard([&] {
		size_t a = 0, k = 0;
		while (a < text.size() || k < si.size()) {
			const size_t b = k < si.size() ? std::min(si[k].first, text.size()) : text.size();
			if (b > a) segment(text.substr(a, b - a), out);
			a = std::max(a, b);
			if (k < si.size()) record_in(si[k++].second, t);
			else break;
		}
		// verify with signing: every record passed on is one whose tag checked (in order: all clear data comes this way)
		if (!pending_out_.empty()) { out += pending_out_; pending_out_.clear(); }
	});
	if (!out.empty()) scan_keys(out);
	return out;
}

void LinkCrypto::host_send(const std::string &d)
{
	if (!cfg_.active() && (st_ == PLAIN || st_ == IDLE)) { modem_send(d); return; }
	if (st_ == FAILED) { if (!discard_noted_) { say(prot_ == Agreement::VERIFY ? "data from the host discarded: the identity check failed on this link (never sent unverified)" : "data from the host discarded: encryption failed on this link (never sent in clear)"); discard_noted_ = true; } return; }
	if (st_ == SECURE && prot_ == Agreement::VERIFY && tx_clear_ && !agreement_.sign) { tx_plain_ += d.size(); tx_chars_ += d.size(); modem_send(d); return; }   // verified, signing off: as typed
	guard([&] {
		if (pend_.size() + d.size() > kMaxHeld || test_fail_ == "nomem") throw std::bad_alloc();
		if (pend_.empty()) pend_t_ = last_t_;
		pend_ += d;
		if (st_ == SECURE) pump();
	});
}

bool LinkCrypto::take_qrt()
{
	if (st_ == SECURE && (!pend_.empty() || worker_busy())) { qrt_wanted_ = true; guard([&] { pump(); }); return true; }
	if (cfg_.active() && !pend_.empty()) { say(std::to_string(pend_.size()) + " bytes held for the link discarded at the QRT: the session was not up (never sent " + (prot_ == Agreement::VERIFY || !cfg_.on ? "unverified)" : "in clear)")); pend_.clear(); }
	return false;
}

void LinkCrypto::tick(double t)
{
	guard([&] {
		if ((st_ == WAIT_CAPS || st_ == HANDSHAKE) && t - t0_ > cfg_.wait_s)
			fail(st_ == WAIT_CAPS ? "no capability record from the other station within " + std::to_string(int(cfg_.wait_s)) +
			                            (prot_ == Agreement::VERIFY ? " s: it is not a Signum with the identity check on (a reference unit, another modem, or the record was lost): modem-layer identity check unavailable"
			                                                        : " s: it is not a Signum with encryption on (or the record was lost)")
			                      : "the handshake did not complete within " + std::to_string(int(cfg_.wait_s)) + " s", st_ == HANDSHAKE);
		turns(t);
		if (st_ == SECURE) pump();
		if (st_ == SECURE && qrt_wanted_ && pend_.empty() && !worker_busy()) { qrt_wanted_ = false; say(prot_ == Agreement::VERIFY ? "everything held has gone out (signed): QRT" : "everything held has gone out encrypted: QRT"); if (modem_qrt) modem_qrt(); }
		if (st_ == FAILED && fail_t_ >= 0 && t - fail_t_ > 30 && modem_abort) { say("the link did not end within 30 s of the failure: aborted"); fail_t_ = -1; modem_abort(); }
		last_t_ = t;
	});
}

std::string LinkCrypto::set_mode(const std::string &mode)
{
	const bool on = mode == "on", ver = mode == "verify";
	if (on == cfg_.on && ver == cfg_.verify) return "";
	if (st_ != IDLE) return "not while a link is up or being set up: end the link first";
	if ((on || ver) && modem_unsent && modem_unsent() > 0) return "the modem still holds data queued in clear for the next link: send it or restart first";
	cfg_.on = on; cfg_.verify = ver;
	last_fail_call_.clear(); last_fail_reason_.clear();
	notify();
	return "";
}

void LinkCrypto::refresh_peer()
{
	if (changed_.set) {                           // the stored key replaced or deleted: nothing left to offer
		const auto it = ks_.peers().find(changed_.stored_as);
		if (it == ks_.peers().end() || it->second.pub != changed_.old_pub) { changed_ = ChangedKey(); notify(); }
	}
	if (st_ != SECURE || peer_fp_.empty()) return;
	const Peer *known = ks_.peer(call_);
	const bool v = known && fingerprint(known->pub) == peer_fp_ && known->verified;
	if (v == peer_verified_) return;
	peer_verified_ = v;
	peer_key_note_ = !known || fingerprint(known->pub) != peer_fp_ ? "no longer stored here, NOT verified" : v ? "known, verified" : "known, NOT verified";
	notify();
}

std::string LinkCrypto::status_json() const
{
	static const char *names[] = {"idle", "plain", "waiting", "handshake", "secure", "failed"};
	auto q = [](const std::string &s) { return jsonm::quote(s); };
	// "secure": encrypted; "verified": the identity check passed, the data in clear (never "secure")
	const std::string sn = st_ == SECURE && prot_ == Agreement::VERIFY ? "verified" : names[st_];
	std::string j = "{\"state\":" + q(sn) + ",\"reason\":" + q(reason_) + ",\"call\":" + q(call_) +
	                ",\"role\":" + q(st_ == IDLE ? "" : caller_ ? "caller" : "callee") + ",\"held\":" + std::to_string(pend_.size());
	j += ",\"protection\":" + q(st_ == IDLE || st_ == PLAIN ? "" : prot_ == Agreement::VERIFY ? "verify" : "encrypt") + ",\"signed\":" + std::string(st_ == SECURE && prot_ == Agreement::VERIFY && agreement_.sign ? "true" : "false") +
	     ",\"trust_level\":" + q(st_ == SECURE ? peer_trust_ : "") + ",\"peer_kind\":" + q(peer_kind_text());
	j += ",\"record_ours\":" + q(my_body_) + ",\"record_peer\":" + q(peer_body_);
	if (peer_caps_.valid) j += ",\"peer\":{\"product\":" + q(peer_caps_.product) + ",\"version\":" + q(peer_caps_.version) + "}";
	else j += ",\"peer\":null";
	j += ",\"features\":[" + features_json() + "]";
	j += ",\"pattern\":" + q(pattern_) + ",\"mode\":" + q(mode_text_) + ",\"sas\":" + q(sas_) + ",\"peer_fingerprint\":" + q(peer_fp_) +
	     ",\"peer_verified\":" + std::string(peer_verified_ ? "true" : "false") + ",\"peer_key\":" + q(peer_key_note_) + ",\"psk\":" + q(psk_name_);
	if (!last_fail_reason_.empty()) j += ",\"last_failed\":{\"call\":" + q(last_fail_call_) + ",\"reason\":" + q(last_fail_reason_) + "}";
	else j += ",\"last_failed\":null";
	if (changed_.set) j += ",\"changed_key\":{\"call\":" + q(changed_.stored_as) + ",\"old\":" + q(fingerprint(changed_.old_pub)) + ",\"new\":" + q(fingerprint(changed_.new_pub)) + "}";
	else j += ",\"changed_key\":null";
	j += ",\"tx_records\":" + std::to_string(tx_rec_) + ",\"rx_records\":" + std::to_string(rx_rec_) + ",\"tx_bytes\":" + std::to_string(tx_plain_) +
	     ",\"tx_compressed\":" + std::to_string(tx_comp_) + ",\"tx_chars\":" + std::to_string(tx_chars_) + ",\"rx_bytes\":" + std::to_string(rx_plain_) + "}";
	return j;
}

std::string LinkCrypto::status_line() const
{
	switch (st_) {
	case IDLE: return cfg_.on ? "on, no link" : cfg_.verify ? std::string("verify (identity check, data in clear") + (cfg_.sign ? ", every record signed" : "") + "), no link" : "off";
	case PLAIN: return "off: this link is not encrypted";
	case WAIT_CAPS: return "link with " + call_ + ": waiting for its capability record";
	case HANDSHAKE: return "link with " + call_ + ": handshake (" + pattern_ + ")";
	case SECURE:
		if (prot_ == Agreement::VERIFY)
			return "link with " + call_ + ": IDENTITY VERIFIED, DATA IN CLEAR (" + mode_text_ + ", " + pattern_ + (agreement_.sign ? ", every record signed" : ", checked at link start only") + "), SAS " + sas_ + ", peer key " + (peer_fp_.empty() ? "-" : peer_fp_) +
			       (peer_fp_.empty() ? " (" + peer_trust_ + ")" : peer_verified_ ? " (verified)" : " (NOT VERIFIED)");
		return "link with " + call_ + ": ENCRYPTED (" + mode_text_ + ", " + pattern_ + "), SAS " + sas_ + ", peer key " + (peer_fp_.empty() ? "-" : peer_fp_) + (peer_fp_.empty() ? "" : peer_verified_ ? " (verified)" : " (NOT VERIFIED)");
	default: return "link with " + call_ + ": FAILED: " + reason_;
	}
}

std::string LinkCrypto::own_key_text() const
{
	return "the other station presents this station's own key (" + fingerprint(ks_.me().pub) + "): the two stations share one key pair, probably because they use the same key"
	       " directory. Give each Signum its own --key-dir and its own key pair (CRYPTO KEYGEN FORCE in the copy), then exchange the public keys again";
}

void LinkCrypto::reset_session()
{
	worker_stop();
	hs_.reset(); zs_.reset(); reader_.reset();
	peer_caps_ = Caps(); my_caps_ = Caps(); my_body_.clear(); peer_body_.clear(); agreement_ = Agreement();
	pattern_.clear(); mode_text_.clear(); sas_.clear(); peer_fp_.clear(); peer_key_note_.clear(); psk_name_.clear(); peer_verified_ = false; reason_.clear(); have_peer_pub_ = false; peer_pub_ = Key32{};
	tx_rec_ = rx_rec_ = tx_plain_ = tx_comp_ = tx_chars_ = rx_plain_ = 0; tx_n_ = rx_n_ = 0;
	fail_t_ = -1; discard_noted_ = false; before_noted_ = 0; qrt_wanted_ = false; hand_back_ = false; turn_t_ = -10;
	prot_ = Agreement::NONE; tx_clear_ = rx_clear_ = false; rxbuf_.clear(); pending_out_.clear(); peer_trust_.clear(); pre_chars_ = 0; pend_t_ = 0;
}

Caps LinkCrypto::make_caps() const
{
	Caps c;
	c.product = "Signum"; c.version = version; c.has_static = ks_.has_key();
	if (cfg_.on) c.enc = {1};
	else { c.ver = {1}; c.sign = cfg_.sign; }
	if (const Key32 *p = ks_.psk(call_)) c.psk_id = psk_id(*p);
	if (cfg_.on && cfg_.compress && !std::getenv("SIGNUM_CRYPTO_NO_COMPRESS")) c.comp = {"zen1"};   // tests: the records uncompressed (the measurement without it)
	c.valid = true;
	return c;
}

std::string LinkCrypto::features_json() const
{
	if (!peer_caps_.valid && my_body_.empty()) return "";
	auto list = [](const std::vector<int> &v) { std::string s; for (int x : v) s += (s.empty() ? "" : ",") + std::to_string(x); return s; };
	auto slist = [](const std::vector<std::string> &v) { std::string s; for (auto &x : v) s += (s.empty() ? "" : ",") + x; return s; };
	if (prot_ == Agreement::VERIFY)
		return "{\"name\":\"verify\",\"ours\":" + jsonm::quote(list(my_caps_.ver)) + ",\"theirs\":" + jsonm::quote(peer_caps_.valid ? list(peer_caps_.ver) : "?") +
		       ",\"agreed\":" + jsonm::quote(agreement_.enc ? std::to_string(agreement_.enc) : "") + ",\"ok\":" + (agreement_.enc && agreement_.reason.empty() ? "true" : "false") +
		       ",\"why\":" + jsonm::quote(peer_caps_.valid ? agreement_.reason : "") + "},{\"name\":\"sign\",\"ours\":" + jsonm::quote(my_caps_.sign ? "1" : "0") +
		       ",\"theirs\":" + jsonm::quote(peer_caps_.valid ? (peer_caps_.sign ? "1" : "0") : "?") + ",\"agreed\":" + jsonm::quote(peer_caps_.valid && agreement_.reason.empty() ? (agreement_.sign ? "1" : "0") : "") +
		       ",\"ok\":" + (peer_caps_.valid && agreement_.sign ? "true" : "false") + ",\"why\":" +
		       jsonm::quote(!peer_caps_.valid || agreement_.sign ? "" : "neither station asks for it: the identity is checked at link start only (a takeover after the check is not detected)") + "}";
	std::string j = "{\"name\":\"encryption\",\"ours\":" + jsonm::quote(list(my_caps_.enc)) + ",\"theirs\":" + jsonm::quote(peer_caps_.valid ? list(peer_caps_.enc) : "?") +
	                ",\"agreed\":" + jsonm::quote(agreement_.enc ? std::to_string(agreement_.enc) : "") + ",\"ok\":" + (agreement_.enc && agreement_.reason.empty() ? "true" : "false") +
	                ",\"why\":" + jsonm::quote(peer_caps_.valid ? agreement_.reason : "") + "}";
	j += ",{\"name\":\"compression\",\"ours\":" + jsonm::quote(slist(my_caps_.comp)) + ",\"theirs\":" + jsonm::quote(peer_caps_.valid ? slist(peer_caps_.comp) : "?") +
	     ",\"agreed\":" + jsonm::quote(peer_caps_.valid ? agreement_.comp : "") + ",\"ok\":" + (peer_caps_.valid && agreement_.comp != "none" ? "true" : "false") +
	     ",\"why\":" + jsonm::quote(!peer_caps_.valid || agreement_.comp != "none" ? "" : my_caps_.comp.empty() ? "off at this station: records go uncompressed" :
	                              peer_caps_.comp.empty() ? "off at the other station: records go uncompressed" : "no common method: records go uncompressed") + "}";
	return j;
}

void LinkCrypto::segment(const std::string &s, std::string &out)
{
	switch (st_) {
	case PLAIN: case IDLE: out += s; break;      // encryption off: as received
	case WAIT_CAPS:                                // before the peer's record: never passed on (the host must not start in clear)
		pre_chars_ += s.size();
		if (pre_chars_ >= kNoRecordAfter && !peer_caps_.valid) {
			// A Signum with a protection on sends its record before any text of its own (only its short P3
			// connect text may come first): this many characters and no record mean another modem.
			fail(std::string("the other station sent ") + std::to_string(pre_chars_) + " characters and no capability record: it is not a Signum with " +
			     (prot_ == Agreement::VERIFY ? "the identity check on (a reference unit, another modem, or an older Signum): modem-layer identity check unavailable" : "encryption on (a reference unit, another modem, or a Signum with encryption off)"), false);
			break;
		}
		if (before_noted_ < 3) {
			std::string v;
			for (char c : s.substr(0, 60)) v += (uint8_t(c) >= 0x20 && uint8_t(c) < 0x7F) ? c : '.';
			say("received before encryption was set up (not passed to the host programs): \"" + v + "\"");
			before_noted_++;
		}
		break;
	case HANDSHAKE: case SECURE:
		for (size_t i = 0; i < s.size(); i++) {
			if (st_ != HANDSHAKE && st_ != SECURE) break;
			if (rx_clear_) { clear_in(s.substr(i), out); break; }   // verify: after the other station's 'C', its data in clear
			const auto r = reader_->feed(s[i], kMaxFrame);
			if (r == FrameReader::BAD) { fail("a damaged frame from the other station (not in the air code, or too long)", true); break; }
			if (r == FrameReader::FRAME) frame_in(reader_->type(), reader_->body(), out);
		}
		break;
	default: break;                                // FAILED: nothing more is read
	}
}

void LinkCrypto::record_in(const std::string &body, double t)
{
	if (st_ == PLAIN || st_ == IDLE) {
		if (body.compare(0, 2, "FE") == 0) {
			const Caps c = parse_caps(body);
			if (c.valid && !c.enc.empty()) say("the other station asks for encryption, which is off here: the link stays plain at this end");
			else if (c.valid && !c.ver.empty()) say("the other station asks for the modem-layer identity check (verify), which is off here: the link stays plain at this end (the other station ends it)");
		}
		return;
	}
	if (st_ == SECURE && prot_ == Agreement::VERIFY && (body.compare(0, 2, "FT") == 0 || body.compare(0, 2, "FR") == 0)) { clear_si(body); return; }
	if (st_ != WAIT_CAPS) {
		if (st_ != FAILED) fail("a second capability record in the middle of the link", true);
		return;
	}
	if (body.compare(0, 2, "FE") != 0) { say("an 'X' system sequence that is not Signum's, ignored: " + body.substr(0, 40)); return; }
	peer_caps_ = parse_caps(body);
	if (!peer_caps_.valid) { fail("the other station's capability record is garbled: " + body.substr(0, 60), false); return; }
	peer_body_ = body;
	agreement_ = caller_ ? agree(my_caps_, peer_caps_, true) : agree(peer_caps_, my_caps_, false);
	say("peer: " + peer_caps_.product + " " + peer_caps_.version + " (record " + body + "): " + feature_line());
	if (!agreement_.reason.empty()) { fail(agreement_.reason, false); return; }
	reader_.reset(new FrameReader(code_));
	if (agreement_.prot == Agreement::ENCRYPT) {
		zs_.reset(new Zstd());
		if (!zs_->ok()) { fail("the compressor could not start (out of memory?)", true); return; }
	}
	start_handshake(t);
}

std::string LinkCrypto::feature_line() const
{
	if (prot_ == Agreement::VERIFY) {
		std::string s = "identity check ";
		if (agreement_.enc && agreement_.reason.empty()) s += "v" + std::to_string(agreement_.enc) + " OK, data in clear, " + (agreement_.sign ? "every record signed" : "checked at link start only");
		else s += "NO (" + agreement_.reason + ")";
		return s;
	}
	std::string s = "encryption ";
	if (agreement_.enc && agreement_.reason.empty()) s += "v" + std::to_string(agreement_.enc) + " OK";
	else s += "NO (" + agreement_.reason + ")";
	s += ", compression " + agreement_.comp;
	return s;
}

void LinkCrypto::start_handshake(double t)
{
	st_ = HANDSHAKE; t0_ = t;
	const Key32 *psk = ks_.psk(call_);
	const bool use_psk = agreement_.mode != Agreement::STATIC;
	mode_text_ = agreement_.mode == Agreement::PSK_ONLY ? "pre-shared key" : agreement_.mode == Agreement::STATIC ? "station keys" : "station keys + pre-shared key";
	if (use_psk && !psk) { fail("no pre-shared key here for " + call_, true); return; }
	if (use_psk) for (const auto &kv : ks_.psks()) if (&kv.second == psk) psk_name_ = kv.first;   // the station's own, or "*" (any station)
	if (prot_ == Agreement::VERIFY && agreement_.mode == Agreement::PSK_ONLY && psk_name_ == "*" && cfg_.policy() != Trust::TOFU) {
		fail(std::string("this link would rest on the \"*\" pre-shared key alone, which every station holding it shares: it does not show which station this is, and the trust policy is \"") +
		     trust_name(cfg_.policy()) + "\" (a key pair at both ends, or a pre-shared key for " + call_ + " alone, would)", true);
		return;
	}
	if (agreement_.mode == Agreement::PSK_ONLY) peer_trust_ = psk_name_ == "*" ? "psk-any" : "psk";
	if (!caller_) { say("handshake: waiting for the caller's first message"); notify(); return; }
	Pattern p = Pattern::NNpsk0;
	const Peer *known = ks_.peer(call_);
	if (agreement_.mode != Agreement::PSK_ONLY) {
		if (known && cfg_.policy() == Trust::VERIFIED && !known->verified) { fail("the key stored for " + call_ + " is not verified, and trust is \"verified\"", true); return; }
		if (!known && cfg_.policy() == Trust::VERIFIED) { fail("no verified key for " + call_ + " here, and trust is \"verified\" (import it and verify it first)", true); return; }
		if (!known && cfg_.policy() == Trust::STORED) { fail("no key stored for " + call_ + " here, and trust is \"stored\" (import its public key first, or allow first contact: trust tofu)", true); return; }
		if (known && ks_.has_key() && known->pub == ks_.me().pub) { fail(own_key_text(), true); return; }
		p = known ? (use_psk ? Pattern::IKpsk2 : Pattern::IK) : (use_psk ? Pattern::XXpsk3 : Pattern::XX);
		// The last IK call to this station was refused by it (its key may have changed): this time ask
		// for its key (XX); trust() still holds it to the stored one, and offers the replacement.
		if (known && xx_next_.erase(upper(call_))) { p = use_psk ? Pattern::XXpsk3 : Pattern::XX; say("the last IK handshake with " + call_ + " was refused by it: asking for its key this time (" + pattern_name(p) + ")"); }
	}
	hs_.reset(new Handshake());
	const bool ik = p == Pattern::IK || p == Pattern::IKpsk2;
	hs_->init(p, true, prologue(), agreement_.mode == Agreement::PSK_ONLY ? nullptr : &ks_.me(), known && ik ? &known->pub : nullptr, use_psk ? psk : nullptr);
	if (known && ik) { ik_pub_ = known->pub; }
	pattern_ = pattern_name(p); pat_ = p;
	Bytes m;
	if (!hs_->write({}, m)) { fail("the handshake could not start (the random generator or the key failed)", true); return; }
	Bytes f(1, uint8_t(pattern_letter(p)));
	f.insert(f.end(), m.begin(), m.end());
	send_frame('H', f);
	say("handshake " + protocol_name(p) + ": message 1 sent" + (known ? " (the callee's key is known: " + std::string(known->verified ? "verified" : "unverified") + ")" : " (first contact: keys exchanged in the handshake)"));
	notify();
}

void LinkCrypto::frame_in(uint8_t type, const Bytes &body, std::string &out)
{
	if (type == 'E') {
		std::string r;
		for (uint8_t c : body) r += (c >= 0x20 && c < 0x7F) ? char(c) : '?';
		if (st_ == HANDSHAKE && caller_ && (pat_ == Pattern::IK || pat_ == Pattern::IKpsk2) && !hs_->done()) {
			xx_next_.insert(upper(call_));
			fail("the other station ended encryption: " + r + ". If its key changed, call it again: Signum then asks for its key (XX) and, if it differs from the stored one, offers to replace it", false);
			return;
		}
		fail("the other station ended encryption: " + r, false);
		return;
	}
	if (type == 'H') { handshake_in(body); return; }
	if (type == 'C' && prot_ == Agreement::VERIFY) { clear_frame_in(body); return; }
	if (type == 'D' && prot_ == Agreement::ENCRYPT) { record_data(body, out); return; }
	fail("an unknown frame type from the other station", true);
}

void LinkCrypto::handshake_in(const Bytes &body)
{
	if (st_ != HANDSHAKE) { fail("a handshake message after the handshake", true); return; }
	Bytes payload;
	if (!hs_) {                                      // the responder: the caller's first message
		if (caller_ || body.empty()) { fail("an unexpected handshake message", true); return; }
		Pattern p;
		switch (char(body[0])) { case 'N': p = Pattern::NNpsk0; break; case 'I': p = Pattern::IK; break; case 'i': p = Pattern::IKpsk2; break; case 'X': p = Pattern::XX; break; case 'x': p = Pattern::XXpsk3; break; default: fail("an unknown handshake pattern from the caller", true); return; }
		const bool want_psk = agreement_.mode != Agreement::STATIC;
		const bool psk_p = p == Pattern::NNpsk0 || p == Pattern::IKpsk2 || p == Pattern::XXpsk3;
		if (psk_p != want_psk || (agreement_.mode == Agreement::PSK_ONLY) != (p == Pattern::NNpsk0)) { fail(std::string("the caller chose a pattern (") + pattern_name(p) + ") the records do not agree on", true); return; }
		hs_.reset(new Handshake());
		hs_->init(p, false, prologue(), p == Pattern::NNpsk0 ? nullptr : &ks_.me(), nullptr, want_psk ? ks_.psk(call_) : nullptr);
		pattern_ = pattern_name(p); pat_ = p;
		if (!hs_->read(Bytes(body.begin() + 1, body.end()), payload)) {
			fail(p == Pattern::NNpsk0 ? "the handshake failed: the pre-shared keys differ"
			     : p == Pattern::IK || p == Pattern::IKpsk2 ? "the handshake failed: the caller's copy of this station's public key is wrong (or the pre-shared keys differ, or the record was altered)"
			                                               : "the handshake failed (the message was damaged or altered)", true);
			return;
		}
		if (hs_->has_remote_static() && !trust(hs_->remote_static())) return;
	} else if (!hs_->read(body, payload)) {
		fail(pattern_ == "NNpsk0" || pattern_ == "IKpsk2" || pattern_ == "XXpsk3" ? "the handshake failed: wrong key or the pre-shared keys differ"
		                                                                          : "the handshake failed: the other station's key is not the one stored for it, or the message was altered", true);
		return;
	} else if (caller_ && hs_->message_index() == 2 && hs_->has_remote_static() && (pattern_ == "XX" || pattern_ == "XXpsk3") && !trust(hs_->remote_static()))
		return;
	else if (!caller_ && hs_->done() && hs_->has_remote_static() && (pattern_ == "XX" || pattern_ == "XXpsk3") && !trust(hs_->remote_static()))
		return;
	if (!hs_->done() && hs_->my_turn()) {
		Bytes m;
		if (!hs_->write({}, m)) { fail("the handshake could not continue", true); return; }
		send_frame('H', m);
	}
	if (hs_->done()) secure();
	notify();
}

bool LinkCrypto::trust(const Key32 &rs)
{
	const Peer *known = ks_.peer(call_);
	peer_fp_ = fingerprint(rs);
	peer_pub_ = rs; have_peer_pub_ = true;
	if (ks_.has_key() && rs == ks_.me().pub) { fail(own_key_text(), true); return false; }
	if (known && known->pub != rs) {
		const std::string name = ks_.peers().count(upper(call_)) ? upper(call_) : KeyStore::base_call(call_);
		changed_.call = call_; changed_.stored_as = name; changed_.old_pub = known->pub; changed_.new_pub = rs; changed_.set = true;
		fail("the other station's key is NOT the one stored for " + name + " (stored " + fingerprint(known->pub) + ", presented " + peer_fp_ +
		     "): someone may be posing as it. If its key really changed, confirm the new fingerprint with its operator by another channel, then replace the stored key: CRYPTO PEER REPLACE " +
		     name + " " + peer_fp_.substr(0, 4) + " (GUI: Replace stored key... on the Status tab or under Known stations)", true,
		     "your station's key (" + peer_fp_ + ") is not the one stored for you at this station: its operator must confirm your new key and replace the stored one, or someone is posing as you");
		return false;
	}
	if (changed_.set && changed_.stored_as == (ks_.peers().count(upper(call_)) ? upper(call_) : KeyStore::base_call(call_))) changed_ = ChangedKey();
	if (known) { peer_verified_ = known->verified; peer_key_note_ = known->verified ? "known, verified" : "known, NOT verified"; peer_trust_ = known->verified ? "verified" : "stored"; }
	else {
		if (cfg_.policy() != Trust::TOFU) { fail("the other station's key (" + peer_fp_ + ") is not known here, and trust is \"" + trust_name(cfg_.policy()) + "\"", true); return false; }
		const std::string e = ks_.add_peer(call_, rs, false);
		peer_key_note_ = "new: stored, NOT verified (trust on first use)";
		peer_trust_ = "new";
		say("first contact with " + call_ + ": its key " + peer_fp_ + " is stored, NOT verified. Compare the SAS or the fingerprint by another channel, then CRYPTO PEER VERIFY " + call_ + (e.empty() ? "" : " (not saved: " + e + ")"));
	}
	if (known && cfg_.policy() == Trust::VERIFIED && !known->verified) { fail("the key stored for " + call_ + " is not verified, and trust is \"verified\"", true); return false; }
	return true;
}

void LinkCrypto::secure()
{
	st_ = SECURE;
	hand_back_ = !caller_;
	if (caller_ && (pat_ == Pattern::IK || pat_ == Pattern::IKpsk2) && !have_peer_pub_) {
		// IK: the caller knew the callee's key before the handshake, which proved the callee holds it
		peer_pub_ = ik_pub_; have_peer_pub_ = true; peer_fp_ = fingerprint(ik_pub_);
		const Peer *known = ks_.peer(call_);
		peer_verified_ = known && known->pub == ik_pub_ && known->verified;
		peer_key_note_ = !known || known->pub != ik_pub_ ? "no longer stored here, NOT verified" : peer_verified_ ? "known, verified" : "known, NOT verified";
		peer_trust_ = peer_verified_ ? "verified" : "stored";
	}
	sas_ = crypto::sas(hs_->hash());
	if (prot_ == Agreement::VERIFY) {
		// The 'C' frame: key confirmation and the switch; from here our direction carries the data in clear.
		Bytes tag;
		const Bytes ad = clear_ad();
		if (!hs_->tx().encrypt(ad.data(), ad.size(), nullptr, 0, tag)) { fail("the identity check could not finish (the cipher failed)", true); return; }
		send_frame('C', tag);
		tx_clear_ = true;
		say("IDENTITY VERIFIED: " + call_ + ", " + protocol_name(pat_) + ", " + mode_text_ + ". DATA IN CLEAR (not encrypted), " +
		    (agreement_.sign ? "every record carries an authentication tag (a bad or missing tag ends the link)" : "checked at link start only: a station taking the link over after the check would NOT be detected") +
		    ". SAS " + sas_ + (peer_fp_.empty() ? ", " + std::string(peer_trust_ == "psk" ? "it holds the pre-shared key set for it here" : "pre-shared key \"*\" (shared by every station holding it)") : ", peer key " + peer_fp_ + " (" + peer_key_note_ + ")") + ".");
		if (!pend_.empty()) {
			if (agreement_.sign) pump();
			else { tx_plain_ += pend_.size(); tx_chars_ += pend_.size(); modem_send(pend_); pend_.clear(); }
		}
		notify();
		return;
	}
	say("ENCRYPTED: " + call_ + ", " + protocol_name(pat_) + ", " + mode_text_ + ", compression " + agreement_.comp + ". SAS " + sas_ +
	    (peer_fp_.empty() ? std::string() : ", peer key " + peer_fp_ + " (" + peer_key_note_ + ")") + ". Compare the SAS with the other operator to rule out a station in the middle.");
	if (!pend_.empty()) pump();
}

void LinkCrypto::record_data(const Bytes &body, std::string &out)
{
	if (st_ != SECURE) { fail("data before the handshake completed", true); return; }
	Bytes pt;
	if (!hs_->rx().decrypt(nullptr, 0, body.data(), body.size(), pt)) { fail("a record failed authentication (altered, replayed, reordered, or wrong keys): the link is ended", true); return; }
	if (!rekey_point(hs_->rx(), rx_n_)) return;
	if (pt.empty() || pt[0] > 1) { fail("a record with an unknown flag", true); return; }
	std::string d;
	if (pt[0] == 0) { d.assign(pt.begin() + 1, pt.end()); if (agreement_.comp == "zen1") zs_->reset_d(); }
	else if (agreement_.comp != "zen1" || !zs_->decomp(pt.data() + 1, pt.size() - 1, d, kBatch * 4)) { fail("a record could not be decompressed", true); return; }
	wipe(pt.data(), pt.size());
	rx_rec_++; rx_plain_ += d.size();
	out += d;
}

void LinkCrypto::turns(double t)
{
	if (!modem_sending || !modem_break_in || !modem_changeover || t - turn_t_ < 1.0) return;
	const bool sending = modem_sending();
	const size_t unsent = modem_unsent ? modem_unsent() : 0;
	bool asked = false;
	if (st_ == WAIT_CAPS || st_ == HANDSHAKE || (st_ == FAILED && fail_t_ >= 0)) {
		if (!sending && unsent > 0) asked = modem_break_in();
		else if (sending && unsent == 0 && st_ != FAILED && !worker_busy()) asked = modem_changeover();
	} else if (st_ == SECURE && hand_back_ && sending && unsent == 0 && pend_.empty() && !worker_busy()) {
		hand_back_ = false;
		asked = modem_changeover();
	}
	if (asked) turn_t_ = t;
}

void LinkCrypto::pump()
{
	if (prot_ == Agreement::VERIFY) { pump_signed(); return; }
	{   // finished frames to the modem, in order
		std::deque<Done> d;
		{ std::lock_guard<std::mutex> l(wm_); d.swap(done_); }
		for (auto &x : d) {
			if (!x.err.empty()) { fail(x.err, true); return; }
			tx_rec_++; tx_plain_ += x.plain; tx_comp_ += x.comp; tx_chars_ += x.chars.size();
			modem_send(x.chars);
		}
	}
	if (st_ != SECURE) return;
	worker_start();
	std::lock_guard<std::mutex> l(wm_);
	const bool idle = jobs_.empty() && !busy_;
	while (pend_.size() >= kBatch || (!pend_.empty() && (qrt_wanted_ || (idle && modem_unsent() < kLowWater)))) {
		const size_t n = std::min(pend_.size(), kBatch);
		jobs_.push_back({pend_.substr(0, n)});
		pend_.erase(0, n);
		if (n < kBatch && !qrt_wanted_) break;
	}
	wcv_.notify_one();
}

void LinkCrypto::worker_loop()
{
	for (;;) {
		Job j;
		{
			std::unique_lock<std::mutex> l(wm_);
			wcv_.wait(l, [&] { return wstop_ || !jobs_.empty(); });
			if (wstop_) return;
			j = std::move(jobs_.front());
			jobs_.pop_front();
			busy_ = true;
		}
		Done d = make_record(j.plain);
		wipe(&j.plain[0], j.plain.size());
		std::lock_guard<std::mutex> l(wm_);
		busy_ = false;
		const bool err = !d.err.empty();
		done_.push_back(std::move(d));
		if (err) { jobs_.clear(); return; }
	}
}

auto LinkCrypto::make_record(const std::string &in) -> Done
{
	Done d;
	try {
		Bytes pt(1, 0), c;
		if (agreement_.comp == "zen1") {
			if (!zs_->comp(in, c)) { d.err = "the compressor failed"; return d; }
			if (c.size() < in.size()) { pt[0] = 1; pt.insert(pt.end(), c.begin(), c.end()); }
			else zs_->reset_c();                       // did not shrink: raw, and both ends start a new context
		}
		if (pt[0] == 0) pt.insert(pt.end(), in.begin(), in.end());
		Bytes ct;
		if (!hs_->tx().encrypt(nullptr, 0, pt.data(), pt.size(), ct)) { d.err = "encryption failed"; return d; }
		d.comp = pt.size() - 1; d.plain = in.size();
		wipe(pt.data(), pt.size());
		if (++tx_n_ % rekey_every_ == 0 && (test_fail_ == "rekey" || !hs_->tx().rekey())) { d.err = "rekeying failed"; return d; }
		d.chars = frame_chars(code_, 'D', ct);
	} catch (const std::exception &) { d.err = "out of memory in the encryption layer"; }
	return d;
}

void LinkCrypto::fail(const std::string &r, bool tell_peer, const std::string &peer_text)
{
	if (st_ == FAILED) return;
	const bool framed = st_ == HANDSHAKE || st_ == SECURE;
	st_ = FAILED;
	worker_stop();                                   // nothing more of ours goes out but the error frame
	reason_ = r;
	if (prot_ == Agreement::VERIFY)
		say("IDENTITY CHECK FAILED: " + r + ". The link is being ended; no unverified data was sent or passed on" + (pend_.empty() ? "." : " (" + std::to_string(pend_.size()) + " bytes held were discarded)."));
	else say("ENCRYPTION FAILED: " + r + ". The link is being ended; no data was sent in clear" + (pend_.empty() ? "." : " (" + std::to_string(pend_.size()) + " bytes held were discarded)."));
	pend_.clear();
	rxbuf_.clear();
	if (framed && tell_peer) {
		const std::string m = (peer_text.empty() ? r : peer_text).substr(0, 200);
		if (tx_clear_) {                             // our direction is clear text: the reason as an SI
			std::string b = "FR";
			for (char c : m.substr(0, 100)) b += (uint8_t(c) > 0x20 && uint8_t(c) < 0x7F && c != ';') ? c : '_';
			if (modem_si) modem_si(b);
		} else send_frame('E', Bytes(m.begin(), m.end()));
	}
	fail_t_ = last_t_;                               // after 30 s without the link ending: abort (tick)
	if (modem_qrt) modem_qrt();
	notify();
}

void LinkCrypto::scan_keys(const std::string &text)
{
	scan_buf_ += text;
	if (scan_buf_.size() > 8192) scan_buf_.erase(0, scan_buf_.size() - 8192);
	if (scan_buf_.find("-----END SIGNUM PUBLIC KEY-----") == std::string::npos) return;
	for (const auto &b : parse_blocks(scan_buf_))
		if (b.kind == "PUBLIC KEY" && !b.name.empty() && key_received) key_received(b.name, b.key);
	scan_buf_.clear();
}

void LinkCrypto::clear_frame_in(const Bytes &body)
{
	if (st_ != SECURE || rx_clear_) { fail(st_ != SECURE ? "the other station switched to clear text before the handshake completed" : "a second switch to clear text", true); return; }
	Bytes pt;
	const Bytes ad = clear_ad();
	if (!hs_->rx().decrypt(ad.data(), ad.size(), body.data(), body.size(), pt)) { fail("the other station's key confirmation failed (altered, or wrong keys): the link is ended", true); return; }
	rx_clear_ = true;
	notify();
}

void LinkCrypto::clear_si(const std::string &body)
{
	if (body.compare(0, 2, "FR") == 0) {
		std::string r = body.substr(2);
		for (auto &c : r) if (c == '_') c = ' ';
		fail("the other station ended the link: " + r, false);
		return;
	}
	if (!rx_clear_ || !agreement_.sign) { fail(!rx_clear_ ? "an authentication tag before the other station's switch to clear text" : "an authentication tag, but signing was not agreed", true); return; }
	uint8_t tag[16];
	if (!unb64_16(body.substr(2), tag)) { fail("a garbled authentication tag from the other station", true); return; }
	Bytes pt;
	if (!hs_->rx().decrypt(reinterpret_cast<const uint8_t *>(rxbuf_.data()), rxbuf_.size(), tag, 16, pt)) {
		fail("a record failed its authentication tag (altered, replayed, reordered, or not from the verified station): the link is ended; the record was not passed on", true);
		return;
	}
	if (!rekey_point(hs_->rx(), rx_n_)) return;
	rx_rec_++; rx_plain_ += rxbuf_.size();
	pending_out_ += rxbuf_;
	rxbuf_.clear();
}

void LinkCrypto::pump_signed()
{
	if (st_ != SECURE || !tx_clear_) return;
	if (!agreement_.sign) { if (!pend_.empty()) { tx_plain_ += pend_.size(); tx_chars_ += pend_.size(); modem_send(pend_); pend_.clear(); } return; }
	while (!pend_.empty()) {
		const bool eol = pend_.back() == '\r' || pend_.back() == '\n';
		if (pend_.size() < kBatch && !qrt_wanted_ && !((modem_unsent ? modem_unsent() : 0) < kLowWater && (eol || last_t_ - pend_t_ >= 1.0))) break;
		const size_t n = std::min(pend_.size(), kBatch);
		const std::string rec = pend_.substr(0, n);
		Bytes tag;
		if (!hs_->tx().encrypt(reinterpret_cast<const uint8_t *>(rec.data()), rec.size(), nullptr, 0, tag) || tag.size() != 16) { fail("signing failed", true); return; }
		if (++tx_n_ % rekey_every_ == 0 && (test_fail_ == "rekey" || !hs_->tx().rekey())) { fail("rekeying failed", true); return; }
		const std::string si = "FT" + b64(tag.data(), tag.size());
		modem_send(rec);
		modem_si(si);
		pend_.erase(0, n);
		pend_t_ = last_t_;
		tx_rec_++; tx_plain_ += n; tx_chars_ += n + si.size() + 3;
	}
}

}  // namespace signum::crypto
