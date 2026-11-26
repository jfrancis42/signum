// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// The functions of crypto_cli.hpp (out of the header, so that they build in parallel).
#include "crypto_cli.hpp"

namespace signum::crypto {

void crypto_usage(FILE *f)
{
	std::fprintf(f,
	    "  --encryption off|verify|on  off (default, as the reference unit); verify: check the other Signum's\n"
	    "                         key at link start (the same handshake and keys as encryption), then send the\n"
	    "                         data IN CLEAR; no acknowledgement needed; a link where the check cannot be made\n"
	    "                         (the reference unit, an older Signum, an untrusted key) is ended, never\n"
	    "                         continued unverified. on: encrypt links to other Signums. ILLEGAL ON THE AMATEUR BANDS\n"
	    "                         (USA: 47 CFR 97.113(a)(4)); NOT compatible with SCS modems' AES;\n"
	    "                         never falls back to clear text (docs/md/encryption.md). Needs --encryption-ack\n"
	    "                         the first time; the acknowledgement is then stored in the settings file\n"
	    "  --encryption-ack %s   the acknowledgement that this station is not on the amateur bands; stored in the\n"
	    "                         settings file once given (then --encryption on needs no more); --encryption-ack\n"
	    "                         none withdraws it and turns encryption off in the file\n"
	    "  --encryption-trust stored|tofu|verified  encryption's first-contact policy: tofu (default): a new\n"
	    "                         station's key is stored unverified at first contact; stored: only a key already\n"
	    "                         stored here; verified: only keys you verified\n"
	    "  --verify-trust stored|tofu|verified  the same for verify; default stored (only a key already stored)\n"
	    "  --sign on|off          verify: an authentication tag on every record (data still in clear; a bad tag\n"
	    "                         ends the link); off (default): the key is checked at link start only, and a\n"
	    "                         takeover after the check is NOT detected. No extra effect with encryption on\n"
	    "  --encryption-wait S    the capability record and the handshake, each at most S s (10-600, default 60)\n"
	    "  --encryption-compression on|off  compress before encrypting (zstd, Signum to Signum only; default on)\n"
	    "  --key-dir DIR          the encryption keys (default ~/.config/signum/keys); one Signum per\n"
	    "                         directory (it is locked while Signum runs)\n"
	    "  --keygen               make this station's key pair (if it has none), print the public key, exit\n"
	    "  --show-key             print this station's public key and fingerprint, exit\n", kAck);
}

bool crypto_arg(const std::string &a, const std::function<const char *()> &val, CryptoOptions &o)
{
	auto bad = [&](const char *why) { std::fprintf(stderr, "bad %s (%s)\n", a.c_str(), why); std::exit(2); };
	if (a == "--encryption") { o.encryption = val(); if (o.encryption != "on" && o.encryption != "off" && o.encryption != "verify") bad("off, verify or on"); return true; }
	if (a == "--verify-trust") { o.vtrust = val(); Trust t; if (!parse_trust(o.vtrust, t)) bad("stored, tofu or verified"); return true; }
	if (a == "--sign") { o.sign = val(); if (o.sign != "on" && o.sign != "off") bad("on or off"); return true; }
	if (a == "--encryption-ack") { o.ack = val(); o.ack_none = upper(o.ack) == "NONE"; if (o.ack_none) o.ack.clear(); return true; }
	if (a == "--encryption-trust") { o.trust = val(); Trust t; if (!parse_trust(o.trust, t)) bad("stored, tofu or verified"); return true; }
	if (a == "--encryption-wait") { const char *v = val(); char *e = nullptr; o.wait_s = std::strtod(v, &e); if (e == v || *e || o.wait_s < 10 || o.wait_s > 600) bad("10-600 s"); return true; }
	if (a == "--encryption-compression") { o.compression = val(); if (o.compression != "on" && o.compression != "off") bad("on or off"); return true; }
	if (a == "--key-dir") { o.key_dir = val(); return true; }
	if (a == "--keygen") { o.keygen = true; return true; }
	if (a == "--show-key") { o.show_key = true; return true; }
	return false;
}

int crypto_setup(const CryptoOptions &o, CryptoControl &c, const std::string &mycall)
{
	c.keys.set_dir(o.key_dir);
	const std::string le = c.keys.load();
	if (!le.empty()) std::fprintf(stderr, "[crypto] %s\n", le.c_str());
	// One Signum per key directory (docs/md/encryption.md "Files"): the lock is taken here for the whole run
	// (an existing directory, or a new one when encryption is on; --keygen takes it when it writes).
	if (!o.keygen && !o.show_key) {
		const bool want = o.encryption == "on" || o.encryption == "verify";
		const std::string lk = c.keys.lock(want);
		if (!lk.empty() && want) {
			std::fprintf(stderr, "Refusing to start: --encryption %s, but %s.\n", o.encryption.c_str(), lk.c_str());
			return 2;
		}
		if (!lk.empty()) std::fprintf(stderr, "[crypto] %s. Encryption cannot be turned on in this run, and its keys cannot be changed.\n", lk.c_str());
	}
	if (o.keygen || o.show_key) {
		if (o.keygen && !c.keys.has_key()) {
			const std::string e = c.keys.generate(false);
			if (!e.empty()) { std::fprintf(stderr, "[crypto] %s\n", e.c_str()); return 1; }
			std::printf("new key pair in %s/station.key (mode 0600)\n", c.keys.dir().c_str());
		} else if (o.keygen) std::printf("this station already has a key pair (%s/station.key); kept\n", c.keys.dir().c_str());
		if (!c.keys.has_key()) { std::fprintf(stderr, "no key pair in %s (signum --keygen)\n", c.keys.dir().c_str()); return 1; }
		std::fputs(c.keys.public_block(mycall).c_str(), stdout);
		return 0;
	}
	parse_trust(o.trust, c.cfg.trust);
	parse_trust(o.vtrust, c.cfg.vtrust);
	c.cfg.sign = o.sign == "on";
	c.cfg.wait_s = o.wait_s;
	c.cfg.compress = o.compression == "on";
	if (o.encryption == "on") {
		if (upper(o.ack) != kAck) {
			std::fprintf(stderr, "%s\n%s\nRefusing to start: --encryption on needs --encryption-ack %s the first time (it is then stored as encryption-ack = %s in the settings file).\n",
			             kLegalWarning, kNotCompatible, kAck, kAck);
			return 2;
		}
		c.cfg.on = true;
		std::printf("[crypto] ENCRYPTION ON. %s\n[crypto] %s\n[crypto] %s\n", kLegalWarning, kNotCompatible, kNoPlainFallback);
		if (!c.keys.has_key() && c.keys.psks().empty())
			std::printf("[crypto] note: no key pair and no pre-shared key yet: every link will be refused until you make one (CRYPTO KEYGEN or CRYPTO PSK GEN)\n");
	} else if (o.encryption == "verify") {
		c.cfg.verify = true;
		std::printf("[crypto] IDENTITY CHECK ON (verify): the other Signum's key is checked at link start (trust %s), then the data goes IN CLEAR%s; a link where the check cannot be made is ended\n",
		            o.vtrust.c_str(), c.cfg.sign ? ", every record with an authentication tag" : " (checked at link start only: a takeover after the check is not detected)");
		if (!c.keys.has_key() && c.keys.psks().empty())
			std::printf("[crypto] note: no key pair and no pre-shared key yet: every link will be refused until you make one (CRYPTO KEYGEN or CRYPTO PSK GEN)\n");
	}
	return -1;
}

}  // namespace signum::crypto
