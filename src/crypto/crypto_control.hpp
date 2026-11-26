// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// crypto_control.hpp: the encryption feature's controls, one place for every interface: the command
// line (crypto_cli.hpp), the console command CRYPTO (docs/md/commands.md), the WebSocket request
// "crypto" (src/web/README.md) and the status the GUI shows. Built only without SIGNUM_NO_CRYPTO.
#pragma once
#include <cstdio>
#include <functional>
#include <string>

#include "json_mini.hpp"
#include "keys.hpp"
#include "legal.hpp"
#include "link_crypto.hpp"

namespace signum::crypto {

class CryptoControl {
public:
	KeyStore keys;
	CryptoConfig cfg;
	LinkCrypto link{keys, cfg};
	std::function<std::string()> mycall;                 // for the public key block's Call line
	std::function<bool()> link_is_up;                    // a link up (SENDKEY)
	// The acknowledgement is stored in the settings file (encryption-ack), once given, so that encryption can be
	// turned on and off without typing it again. ack_stored: is it there now. store_ack(true): write it;
	// store_ack(false): remove it and encryption = on (the withdrawal). Both "" or why not. Unset: nothing is stored.
	std::function<bool()> ack_stored;
	std::function<std::string(bool)> store_ack;
	bool acked() const { return ack_stored && ack_stored(); }

	CryptoControl();

	// ---- turning it on ----
	// "" or why not. ack must be kAck, unless it is stored in the settings file; a valid ack not yet stored is stored.
	std::string enable(bool on, const std::string &ack);
	// Identity verification without encryption (verify): no acknowledgement (nothing is hidden). "" or why not.
	std::string enable_verify();
	std::string verify_text() const;
	static std::string sign_text(bool on);
	// Withdraw the stored acknowledgement: encryption off now (refused while a link is up) and no longer
	// turned on at start. "" or why not.
	std::string withdraw();

	// ---- known stations: verifying a given key, replacing a changed one ----
	// Mark verified only the key with this fingerprint (fp empty: the stored one, as CRYPTO PEER VERIFY).
	std::string verify(const std::string &call, bool v, const std::string &fp);
	// Replace the stored key of `call` with the key it presented on the failed link. confirm: the new
	// fingerprint, or at least its first 4 digits (the user read it). "" or why not.
	std::string replace(const std::string &call, const std::string &confirm, bool verified);
	static std::string norm_fp(const std::string &f) { std::string o; for (char c : f) if (!std::isspace(uint8_t(c))) o += char(std::toupper(uint8_t(c))); return o; }

	// ---- the console command: CRYPTO [sub ...] ----
	// Every console command and API request: afterwards this link's "verified" follows the known stations.
	std::string command(const std::string &arg_in) { std::string r = command_(arg_in); link.refresh_peer(); return r; }
	std::string request(const jsonm::Value &v) { std::string r = request_(v); link.refresh_peer(); return r; }
	std::string command_(const std::string &arg_in);

	// Key blocks from text: public keys become known (unverified) stations, a private key this
	// station's key (only with force when it has one), pre-shared keys are stored. call: the call for a
	// public block without a Call line.
	std::string import_text(const std::string &text, const std::string &call, bool force);

	// ---- the WebSocket API ----
	std::string status_json() const;
	// One request {"type":"crypto","op":...}: the reply's fields after "type"/"id" (ok, text, and per op).
	std::string request_(const jsonm::Value &v);
};

}  // namespace signum::crypto
