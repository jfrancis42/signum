// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// link_crypto.hpp: the encryption layer on the host byte stream of an ARQ link (docs/md/encryption.md).
// It sits between the host programs (pty, telnet,
// /shell, the GUI) and the modem's text queue, never in src/modem.
//
// With encryption off it passes everything through unchanged (the link is exactly the reference
// unit's). With it on:
//   1. at link up (P2/P3 only) each side sends its capability record (caps.hpp) as an 'X' SI;
//   2. on the peer's record both compute the agreement; everything after a record in its direction
//      is frames (aircode.hpp): the caller starts the Noise handshake (IK when it knows the callee's
//      key, else XX; NNpsk0 with a pre-shared key only; psk2/psk3 added when both have keys and a PSK);
//   3. after it, records: the host's data, compressed (zstd with the dictionary en1) and encrypted
//      (AES-256-GCM, one key per direction from Noise's Split(), nonce = counter).
// THE RULE (owner): with encryption on, no user byte ever goes out in clear. Every failure (no record,
// a garbled record, no common version, wrong key or PSK, a changed key, a bad tag, a decoding error,
// a rekey failure, running out of memory, a P1 link, a timeout) ends the link with the reason in the
// log and the status; data held for the link is discarded, never sent plain. Unproto is refused.
//
// Frames: type 'H' handshake (the first message: a pattern letter, then the Noise message), 'D' a
// record (the Noise transport ciphertext of: one flag byte, 0 raw / 1 compressed, then the data),
// 'E' an error (the reason in ASCII, sent before our QRT so that the other end can show it).
// Each direction rekeys (Noise Rekey()) after every kRekeyEvery records, both ends counting.
//
// IDENTITY VERIFICATION WITHOUT ENCRYPTION ("verify", docs/md/encryption.md "Verify"): steps 1 and 2 are the
// same (the record says 'a' instead of 'e'; the Noise prologue starts "Signum verify v1"; keys, PSK, trust
// and key-change handling as above), then each side, once its handshake is complete, sends one frame 'C'
// (the AES-GCM tag, from its sending key at nonce 0, over the associated data "Signum verify v1 clear"
// and one byte: 1 when every record is signed, else 0: key confirmation and the switch). After its 'C' a
// direction carries the host's data in clear:
//   - signing off (the default): as typed, no overhead. The check is made at link start only: a station
//     that takes the link over after it is NOT detected (the docs say so).
//   - signing on (either side asked for it): records of at most kBatch bytes, each the data in clear
//     followed by an 'X' SI "FT" + base64 of a 16-byte tag: AES-GCM with no plaintext and the record's
//     bytes as associated data, from the sending key, nonce = the counter (the 'C' frame used 0; rekeyed
//     like the encrypted records). The receiver holds a record until its tag checks: a bad or missing tag
//     ends the link, and the record is never passed on.
// An error after a direction went clear travels as an 'X' SI "FR" + the reason (spaces as '_').
// THE RULE for verify: no data goes out before the identity check, and none is passed on unverified.
#pragma once
#include <zstd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <set>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <new>
#include <string>
#include <vector>

#include "aircode.hpp"
#include "caps.hpp"
#include "dict_en1.hpp"
#include "keys.hpp"
#include "legal.hpp"
#include "noise.hpp"
#include "json_mini.hpp"

namespace signum::crypto {

constexpr size_t kBatch = 1024;          // plaintext per record at most
constexpr size_t kLowWater = 64;         // a new record when the modem has fewer characters than this unsent
constexpr size_t kMaxFrame = 4096;       // a frame's body at most (a record: 1 + 1024 compressed + 16, with room)
constexpr size_t kMaxHeld = 4u << 20;    // plaintext held for the link at most (4 MB)
constexpr int kZstdLevel = 19;           // the best ratio (20-22 gain nothing here)
constexpr int kZstdWindowLog = 20;       // part of method zen1: a 1 MB window (the receiver takes no more)

// zstd with the dictionary en1, one stream per direction; reset() starts a new context on both ends
// (after a record that did not shrink and went raw).
class Zstd {
public:
	Zstd()
	{
		cd_ = ZSTD_createCDict(kDictEn1, kDictEn1Size, kZstdLevel);
		dd_ = ZSTD_createDDict(kDictEn1, kDictEn1Size);
		c_ = ZSTD_createCCtx(); d_ = ZSTD_createDCtx();
		reset_c(); reset_d();
	}
	~Zstd() { ZSTD_freeCCtx(c_); ZSTD_freeDCtx(d_); ZSTD_freeCDict(cd_); ZSTD_freeDDict(dd_); }
	bool ok() const { return cd_ && dd_ && c_ && d_; }
	void reset_c()
	{
		ZSTD_CCtx_reset(c_, ZSTD_reset_session_and_parameters);
		ZSTD_CCtx_refCDict(c_, cd_);
		ZSTD_CCtx_setParameter(c_, ZSTD_c_windowLog, kZstdWindowLog);
		ZSTD_CCtx_setParameter(c_, ZSTD_c_checksumFlag, 0);
		ZSTD_CCtx_setParameter(c_, ZSTD_c_contentSizeFlag, 0);
		ZSTD_CCtx_setParameter(c_, ZSTD_c_dictIDFlag, 0);
	}
	void reset_d()
	{
		ZSTD_DCtx_reset(d_, ZSTD_reset_session_and_parameters);
		ZSTD_DCtx_refDDict(d_, dd_);
		ZSTD_DCtx_setParameter(d_, ZSTD_d_windowLogMax, kZstdWindowLog);
	}
	bool comp(const std::string &in, Bytes &out)
	{
		out.assign(ZSTD_compressBound(in.size()) + 32, 0);
		ZSTD_inBuffer ib{in.data(), in.size(), 0};
		ZSTD_outBuffer ob{out.data(), out.size(), 0};
		size_t r;
		do { r = ZSTD_compressStream2(c_, &ob, &ib, ZSTD_e_flush); if (ZSTD_isError(r)) return false; } while (r != 0);
		out.resize(ob.pos);
		return true;
	}
	// false: not valid, or more than `max` bytes out
	bool decomp(const uint8_t *p, size_t n, std::string &out, size_t max)
	{
		out.clear();
		ZSTD_inBuffer ib{p, n, 0};
		char buf[4096];
		while (ib.pos < ib.size) {
			ZSTD_outBuffer ob{buf, sizeof buf, 0};
			const size_t r = ZSTD_decompressStream(d_, &ob, &ib);
			if (ZSTD_isError(r)) return false;
			out.append(buf, ob.pos);
			if (out.size() > max) return false;
			if (ob.pos == 0 && ib.pos < ib.size && r == 0) return false;
		}
		for (;;) {                                     // whatever the flush left inside
			ZSTD_outBuffer ob{buf, sizeof buf, 0};
			const size_t r = ZSTD_decompressStream(d_, &ob, &ib);
			if (ZSTD_isError(r)) return false;
			out.append(buf, ob.pos);
			if (out.size() > max) return false;
			if (ob.pos < sizeof buf) break;
		}
		return true;
	}

private:
	ZSTD_CDict *cd_ = nullptr;
	ZSTD_DDict *dd_ = nullptr;
	ZSTD_CCtx *c_ = nullptr;
	ZSTD_DCtx *d_ = nullptr;
};

// First contact with a station's key (docs/md/encryption-keys.md "Trust"): stored: only a key already stored
// here passes (verified or not); tofu: a key not known yet is stored, unverified, and the link goes on;
// verified: only a stored key marked verified.
enum class Trust { STORED, TOFU, VERIFIED };
inline const char *trust_name(Trust t) { return t == Trust::STORED ? "stored" : t == Trust::TOFU ? "tofu" : "verified"; }
inline bool parse_trust(std::string v, Trust &t)
{
	for (auto &c : v) c = char(std::tolower(uint8_t(c)));
	if (v == "stored") t = Trust::STORED; else if (v == "tofu") t = Trust::TOFU; else if (v == "verified") t = Trust::VERIFIED; else return false;
	return true;
}

struct CryptoConfig {
	bool on = false;                     // encryption specified (the acknowledgement given)
	bool verify = false;                 // identity verification without encryption (never with on)
	bool sign = false;                   // verify: ask for a tag on every record (default off)
	double wait_s = 60;                  // the capability record and the handshake, each at most this long
	Trust trust = Trust::TOFU;           // encryption's first-contact policy (--encryption-trust; default tofu, as before)
	Trust vtrust = Trust::STORED;        // verify's (--verify-trust; default stored)
	bool compress = true;                // offer zstd+en1 in the capability record (both must offer it; else records go raw)
	bool active() const { return on || verify; }
	Trust policy() const { return on ? trust : vtrust; }
	const char *mode_name() const { return on ? "on" : verify ? "verify" : "off"; }
};

// base64 of exactly 16 bytes (a tag; false otherwise)
inline bool unb64_16(const std::string &t, uint8_t out[16])
{
	if (t.size() != 24 || t[22] != '=' || t[23] != '=') return false;
	unsigned char o[19];
	if (EVP_DecodeBlock(o, reinterpret_cast<const unsigned char *>(t.data()), 24) != 18) return false;
	std::memcpy(out, o, 16);
	return true;
}

class LinkCrypto {
public:
	enum State { IDLE, PLAIN, WAIT_CAPS, HANDSHAKE, SECURE, FAILED };

	// ---- what the core provides ----
	std::function<void(const std::string &)> modem_send;   // text for the link (the modem escapes it)
	std::function<void(const std::string &)> modem_si;     // an SI body, sent unescaped
	std::function<void()> modem_qrt, modem_abort;
	std::function<size_t()> modem_unsent;
	// The change of direction, as the reference unit (text waits for the turn): the layer asks for it only
	// while it sets the session up (its own records and handshake must cross), and once after it to give
	// the turn back to the caller, who had it at link up. modem_sending: we are the sender now.
	std::function<bool()> modem_sending, modem_break_in, modem_changeover;
	std::function<void(const std::string &)> log;          // one line, without "[crypto] " and newline
	std::function<void()> changed;                         // the status changed
	std::function<void(const std::string &call, const Key32 &pub)> key_received;   // a public key block arrived
	std::string version = "0";

	~LinkCrypto() { worker_stop(); }
	LinkCrypto(KeyStore &ks, CryptoConfig &cfg) : ks_(ks), cfg_(cfg)
	{
		if (const char *e = std::getenv("SIGNUM_CRYPTO_REKEY_EVERY")) rekey_every_ = std::max<uint64_t>(1, std::strtoull(e, nullptr, 10));
		if (const char *e = std::getenv("SIGNUM_CRYPTO_TEST_FAIL")) test_fail_ = e;   // tests: "rekey", "nomem"
	}

	State state() const { return st_; }
	bool on() const { return cfg_.on; }
	bool verifying() const { return prot_ == Agreement::VERIFY; }   // this link runs (or set up) the identity check
	size_t held() const { return pend_.size(); }
	bool link_active() const { return st_ != IDLE; }
	const std::string &reason() const { return reason_; }
	const std::string &sas() const { return sas_; }
	// A station that presented a key other than the one stored for it (the link failed): kept until the
	// stored key is replaced or deleted, or a later link with it succeeds, so the user can replace the
	// stored key with the presented one (CRYPTO PEER REPLACE; the GUI's Replace stored key...).
	struct ChangedKey { std::string call, stored_as; Key32 old_pub{}, new_pub{}; bool set = false; };
	const ChangedKey &changed_key() const { return changed_; }
	void clear_changed_key() { changed_ = ChangedKey(); notify(); }
	// The other station's static key on this link (the key it presented, or for the IK caller the key it
	// proved it holds); set: there is one.
	bool peer_key(Key32 &k) const { if (!have_peer_pub_) return false; k = peer_pub_; return true; }

	// ---- events from the core ----
	void link_up(const std::string &call, bool caller, int plevel, double t);
	void link_down(double);
	// Received text and the 'X' SIs in it (positions in text): what the host programs get.
	std::string rx(const std::string &text, const std::vector<std::pair<size_t, std::string>> &si, double t);
	// Data from a host program for the link.
	void host_send(const std::string &d);
	// A host asked for QRT. true: this layer QRTs itself once everything held has gone out encrypted
	// (the caller must not QRT now); false: the caller QRTs as usual (nothing held, or encryption off).
	bool take_qrt();
	void tick(double t);
	// Turning it on or off is refused while a link is up (no change of rules mid-link).
	std::string set_on(bool on) { return set_mode(on ? "on" : "off"); }
	// mode: "off", "verify" or "on" (encryption).
	std::string set_mode(const std::string &mode);
	// After a change to the known stations (verify, unverify, delete, add, accept): this link's "verified"
	// follows the stored key, so "mark verified" during an encrypted link shows at once.
	void refresh_peer();

	// ---- status (the WebSocket API's "crypto" message; src/web/README.md) ----
	std::string status_json() const;
	// One line for the console (CRYPTO STATUS) and the log.
	std::string status_line() const;

private:
	// Every step runs under this: an exception (out of memory, anything) ends encryption, never plain.
	template <class F> void guard(F f)
	{
		try { f(); }
		catch (const std::bad_alloc &) { fail_nothrow("out of memory in the encryption layer"); }
		catch (const std::exception &e) { fail_nothrow(std::string("internal error in the encryption layer: ") + e.what()); }
	}
	void fail_nothrow(const std::string &r) { try { fail(r, st_ == HANDSHAKE || st_ == SECURE); } catch (...) { st_ = FAILED; if (modem_abort) modem_abort(); } }
	void say(const std::string &s) { if (log) log(s); }
	std::string own_key_text() const;
	void notify() { if (changed) changed(); }
	void reset_session();
	std::string peer_kind_text() const
	{
		if (peer_caps_.valid) return "signum";
		if (st_ != IDLE && st_ != PLAIN && pre_chars_ >= kNoRecordAfter) return "other";
		return "";
	}
	Caps make_caps() const;
	std::string features_json() const;

	// ---- receive ----
	void segment(const std::string &s, std::string &out);
	void record_in(const std::string &body, double t);
	std::string feature_line() const;
	Bytes prologue() const
	{
		const std::string p = std::string(prot_ == Agreement::VERIFY ? "Signum verify v1" : "Signum encryption v1") + '\0' + (caller_ ? my_body_ : peer_body_) + '\0' + (caller_ ? peer_body_ : my_body_);
		return to_bytes(p);
	}
	static char pattern_letter(Pattern p)
	{
		switch (p) { case Pattern::NNpsk0: return 'N'; case Pattern::IK: return 'I'; case Pattern::IKpsk2: return 'i'; case Pattern::XX: return 'X'; case Pattern::XXpsk3: return 'x'; default: return '?'; }
	}
	void start_handshake(double t);
	void frame_in(uint8_t type, const Bytes &body, std::string &out);
	void handshake_in(const Bytes &body);
	// The other station's static key against the stored one. false: failed (the link ends).
	bool trust(const Key32 &rs);
	void secure();
	void record_data(const Bytes &body, std::string &out);
	bool rekey_point(CipherState &c, uint64_t &n)
	{
		if (++n % rekey_every_) return true;
		if (test_fail_ == "rekey" || !c.rekey()) { fail("rekeying failed", true); return false; }
		return true;
	}

	// ---- send ----
	void send_frame(uint8_t type, const Bytes &body)   // the main thread's frames (handshake, error)
	{
		const std::string ch = frame_chars(code_, type, body);
		tx_chars_ += ch.size();
		modem_send(ch);
	}
	// Who sends: during the set-up the records and handshake messages must cross, so the layer breaks in
	// when it has set-up bytes waiting as the receiver, and hands over when it is the sender with nothing
	// left and waits for the other station. Once secure, the callee hands the turn back once (the caller
	// had it at link up); after that the user's own keys (Ctrl-Y, %O/%I, PDuplex) decide, as on a plain link.
	void turns(double t);
	// The send pipeline. Compressing (zstd at level 19) and encrypting run on a worker thread, never on
	// the audio loop: the main thread cuts the held data into records (a full record at once; a partial
	// one when the modem is about to run dry and nothing is in the works), the worker makes the frames in
	// order, and the main thread hands finished frames to the modem. The worker alone uses the send
	// cipher and the compressor while a session runs; the main thread waits for it before any reset.
	struct Job { std::string plain; };
	struct Done { std::string chars, err; size_t plain = 0, comp = 0; };
	bool worker_busy()
	{
		std::lock_guard<std::mutex> l(wm_);
		return !jobs_.empty() || busy_ || !done_.empty();
	}
	void pump();
	void worker_start()
	{
		if (worker_.joinable()) return;
		wstop_ = false;
		worker_ = std::thread([this] { worker_loop(); });
	}
	void worker_stop()
	{
		if (!worker_.joinable()) return;
		{ std::lock_guard<std::mutex> l(wm_); wstop_ = true; jobs_.clear(); }
		wcv_.notify_one();
		worker_.join();
		std::lock_guard<std::mutex> l(wm_);
		done_.clear(); busy_ = false;
	}
	void worker_loop();
	// On the worker: one record's frame (or why not).
	Done make_record(const std::string &in);
	void fail(const std::string &r, bool tell_peer, const std::string &peer_text = "");
	// Public key blocks in received text (also on plain links): kept as "received", never trusted
	// until the user accepts them.
	void scan_keys(const std::string &text);

	// ---- verify: the other station's 'C', its data in clear, its tags and errors ----
	Bytes clear_ad() const
	{
		const std::string a = std::string("Signum verify v1 clear") + char(agreement_.sign ? 1 : 0);
		return to_bytes(a);
	}
	void clear_frame_in(const Bytes &body);
	void clear_in(const std::string &s, std::string &out)
	{
		if (!agreement_.sign) { rx_plain_ += s.size(); out += s; return; }
		rxbuf_ += s;
		if (rxbuf_.size() > kBatch) fail("the other station sent more than " + std::to_string(kBatch) + " bytes without an authentication tag: the link is ended (the data is not passed on)", true);
	}
	void clear_si(const std::string &body);
	// verify with signing: cut records (a full one at once; a partial one when the modem is about to run
	// dry and the text ends a line or has waited 1 s, or at the QRT), each the data then its tag SI.
	void pump_signed();

	static constexpr size_t kNoRecordAfter = 64;     // as PeerId::kOtherAfter: characters with no record mean another modem
	KeyStore &ks_;
	CryptoConfig &cfg_;
	AirCode code_;
	State st_ = IDLE;
	std::string call_, reason_, my_body_, peer_body_, pattern_, mode_text_, sas_, peer_fp_, peer_key_note_, pend_, scan_buf_, test_fail_;
	std::string psk_name_;                               // the pre-shared key used: a call's own, or "*"
	std::string last_fail_call_, last_fail_reason_;      // the last link's failure, kept until the next link
	bool caller_ = false, peer_verified_ = false, discard_noted_ = false, have_peer_pub_ = false;
	Key32 peer_pub_{}, ik_pub_{};
	ChangedKey changed_;
	std::set<std::string> xx_next_;                      // stations whose last IK handshake they refused: XX next time
	int before_noted_ = 0;
	double t0_ = 0, last_t_ = 0, fail_t_ = -1;
	Pattern pat_ = Pattern::NN;
	Caps my_caps_, peer_caps_;
	Agreement agreement_;
	std::unique_ptr<Handshake> hs_;
	std::unique_ptr<Zstd> zs_;
	std::unique_ptr<FrameReader> reader_;
	std::thread worker_;
	std::mutex wm_;
	std::condition_variable wcv_;
	std::deque<Job> jobs_;
	std::deque<Done> done_;
	bool busy_ = false, wstop_ = false, qrt_wanted_ = false, hand_back_ = false;
	Agreement::Prot prot_ = Agreement::NONE;             // this link's protection (from link up)
	bool tx_clear_ = false, rx_clear_ = false;           // verify: our 'C' sent / theirs received
	std::string rxbuf_, pending_out_, peer_trust_;       // verify+sign: a record waiting for its tag; records checked; the peer's trust level
	size_t pre_chars_ = 0;                               // characters before the other station's record
	double pend_t_ = 0;
	double turn_t_ = -10;
	uint64_t tx_rec_ = 0, rx_rec_ = 0, tx_plain_ = 0, tx_comp_ = 0, tx_chars_ = 0, rx_plain_ = 0, tx_n_ = 0, rx_n_ = 0, rekey_every_ = 1u << 16;
};

}  // namespace signum::crypto
