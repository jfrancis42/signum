// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// keys.hpp: the encryption layer's keys (docs/md/encryption.md "Keys"). One directory, by default
// ~/.config/signum/keys ($XDG_CONFIG_HOME/signum/keys; --key-dir DIR), mode 0700:
//   station.key  this station's X25519 private key (text block, mode 0600)
//   peers        known stations: CALL KEY verified|unverified SINCE (one per line, mode 0600)
//   psk          pre-shared keys: NAME KEY (NAME a call sign, or * for every station; mode 0600)
//   received     public keys received over the air, waiting for the user: CALL KEY WHEN
//   lock         held (flock) by the one Signum that uses this directory, for its whole run; holds its pid
// Keys in the files are base64. The private key has no passphrase (a station runs unattended; the
// protection is the file mode and the computer's own disk encryption; docs/md/encryption.md says why).
//
// Text blocks for export and import (pasted into mail or sent over the air), lines ended CR or LF:
//   -----BEGIN SIGNUM PUBLIC KEY-----        (or PRIVATE KEY, PRE-SHARED KEY)
//   Call: N0AAA                                (Name: for a pre-shared key)
//   Fingerprint: 1A2B 3C4D 5E6F 7081 92A3 B4C5 D6E7 F809
//   Key: <base64 of the 32 bytes>
//   Check: <the first 4 bytes of SHA-256("Signum key check v1" | kind | key), hex>
//   -----END SIGNUM PUBLIC KEY-----
#pragma once
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

#include <openssl/evp.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

#include "noise.hpp"

namespace signum::crypto {

inline std::string hex(const uint8_t *p, size_t n, bool upper = false)
{
	static const char *l = "0123456789abcdef", *u = "0123456789ABCDEF";
	std::string s;
	for (size_t i = 0; i < n; i++) { s += (upper ? u : l)[p[i] >> 4]; s += (upper ? u : l)[p[i] & 15]; }
	return s;
}
inline std::string b64(const uint8_t *p, size_t n)
{
	std::string o(4 * ((n + 2) / 3) + 1, '\0');
	const int k = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(&o[0]), p, int(n));
	o.resize(size_t(k));
	return o;
}
// base64 of exactly 32 bytes (false otherwise)
bool unb64_32(const std::string &s, Key32 &k);
// The fingerprint of a public key: SHA-256 of it, the first 16 bytes as 8 groups of 4 hex digits.
inline std::string fingerprint(const Key32 &pub)
{
	const Key32 h = sha256(pub.data(), 32);
	std::string s;
	for (int i = 0; i < 16; i += 2) { if (i) s += ' '; s += hex(h.data() + i, 2, true); }
	return s;
}
inline std::string tagged_hash_hex(const std::string &tag, const uint8_t *p, size_t n, size_t bytes)
{
	Bytes b(tag.begin(), tag.end());
	b.insert(b.end(), p, p + n);
	const Key32 h = sha256(b);
	return hex(h.data(), bytes);
}
// A pre-shared key's short id (sent in the capability record so that different keys are named, not
// just failed on): 2 bytes of SHA-256("Signum PSK id v1" | key).
inline std::string psk_id(const Key32 &k) { return tagged_hash_hex("Signum PSK id v1", k.data(), 32, 2); }
// The session's short authentication string: both ends show the same 8 digits only if no one sits in
// the middle (SHA-256("Signum SAS v1" | handshake hash), the first 4 bytes mod 10^8).
std::string sas(const Key32 &handshake_hash);
// A PSK from a passphrase: PBKDF2-HMAC-SHA256, 200,000 rounds, salt "Signum PSK v1".
inline bool psk_from_phrase(const std::string &phrase, Key32 &k)
{
	static const char salt[] = "Signum PSK v1";
	return PKCS5_PBKDF2_HMAC(phrase.data(), int(phrase.size()), reinterpret_cast<const unsigned char *>(salt), int(sizeof salt - 1), 200000, EVP_sha256(), 32, k.data()) == 1;
}

inline std::string upper(std::string s) { for (auto &c : s) c = char(std::toupper(uint8_t(c))); return s; }
inline bool valid_call(const std::string &c)
{
	if (c.empty() || c.size() > 12) return false;
	for (char x : c) if (!(std::isalnum(uint8_t(x)) || x == '-' || x == '/')) return false;
	return true;
}
inline bool valid_psk_name(const std::string &c) { return c == "*" || valid_call(c); }

// ---- text blocks ---------------------------------------------------------------------------------
struct KeyBlock {
	std::string kind;          // "PUBLIC KEY", "PRIVATE KEY", "PRE-SHARED KEY"
	std::string name;          // Call: / Name:
	Key32 key{};
};
inline std::string block_check(const std::string &kind, const Key32 &k)
{
	Bytes b = to_bytes(kind);
	b.insert(b.end(), k.begin(), k.end());
	return tagged_hash_hex("Signum key check v1", b.data(), b.size(), 4);
}
std::string make_block(const KeyBlock &b, const char *eol = "\n");
// Every complete block in `text` (CR, LF or CRLF lines; other text around them ignored). err: why a
// block that was started was refused (a bad checksum: a typing or transfer error).
std::vector<KeyBlock> parse_blocks(const std::string &text, std::string *err = nullptr);

// ---- the key store -------------------------------------------------------------------------------
struct Peer {
	Key32 pub{};
	bool verified = false;
	long long since = 0;
};

class KeyStore {
public:
	// The directory: dir if given, else $SIGNUM_KEY_DIR (the tests: never the user's), else
	// $XDG_CONFIG_HOME/signum/keys, else ~/.config/signum/keys.
	void set_dir(const std::string &dir);
	const std::string &dir() const { return dir_; }
	KeyStore() = default;
	KeyStore(const KeyStore &) = delete;
	KeyStore &operator=(const KeyStore &) = delete;
	~KeyStore() { if (lock_fd_ >= 0) ::close(lock_fd_); }
	// The directory's lock: one Signum per key directory (two sharing one would share the station key
	// and overwrite each other's files). Taken for the whole run (released when the process ends).
	// "" or why not (another Signum holds it: the message names its pid and --key-dir). create: make
	// the directory if it does not exist (else a missing directory is not locked, and "" is returned).
	std::string lock(bool create = true);
	bool locked() const { return lock_fd_ >= 0; }
	// "" when this process holds the lock (taking it now if it can), else why not. Every change to the
	// files goes through this, so a second Signum on the same directory changes nothing.
	std::string ensure_lock() { return lock(true); }
	const std::string &lock_error() const { return lock_err_; }
	// Reads everything; "" or what was wrong (the files are left alone).
	std::string load();

	bool has_key() const { return me_.set; }
	const KeyPair &me() const { return me_; }
	// A new key pair (replacing the old only with force); "" or why not.
	std::string generate(bool force);
	std::string set_private(const Key32 &priv);
	std::string public_block(const std::string &call, const char *eol = "\n") const { return me_.set ? make_block({"PUBLIC KEY", call, me_.pub}, eol) : std::string(); }
	std::string private_block() const { return me_.set ? make_block({"PRIVATE KEY", "", me_.priv}) : std::string(); }

	// Peers. The lookup takes the call as the link names it, then without an SSID.
	const Peer *peer(const std::string &call) const
	{
		auto it = peers_.find(upper(call));
		if (it == peers_.end()) { const std::string b = base_call(call); if (b != upper(call)) it = peers_.find(b); }
		return it == peers_.end() ? nullptr : &it->second;
	}
	const std::map<std::string, Peer> &peers() const { return peers_; }
	// Every change below: the lock, then the file read again (what is on disk is the truth), the one
	// change, the file written (to a new file renamed over the old).
	std::string add_peer(const std::string &call, const Key32 &pub, bool verified);
	// expect: mark verified only if the stored key is this one (the key the user compared, or the one
	// the other station presented on this link); never a different key stored under the same call.
	std::string verify_peer(const std::string &call, bool v, const Key32 *expect = nullptr);
	std::string delete_peer(const std::string &call)
	{
		const std::string le = ensure_lock();
		if (!le.empty()) return le;
		reread_peers();
		if (!peers_.erase(upper(call))) return "no known key for " + upper(call);
		return save_peers();
	}
	// Keys received over the air (not trusted until accepted).
	const std::map<std::string, Peer> &received() const { return received_; }
	std::string note_received(const std::string &call, const Key32 &pub)
	{
		if (!valid_call(call)) return "not a call sign: " + call;
		const std::string le = ensure_lock();
		if (!le.empty()) return le;
		reread_received();
		received_[upper(call)] = Peer{pub, false, (long long)std::time(nullptr)};
		return save_received();
	}
	std::string accept_received(const std::string &call, bool verified)
	{
		reread_received();
		auto it = received_.find(upper(call));
		if (it == received_.end()) return "no key received from " + upper(call);
		const Key32 k = it->second.pub;
		return add_peer(call, k, verified);
	}
	std::string reject_received(const std::string &call)
	{
		const std::string le = ensure_lock();
		if (!le.empty()) return le;
		reread_received();
		if (!received_.erase(upper(call))) return "no key received from " + upper(call);
		return save_received();
	}

	// Pre-shared keys: for a call, else the one named "*".
	const Key32 *psk(const std::string &call) const
	{
		auto it = psks_.find(upper(call));
		if (it == psks_.end()) it = psks_.find(base_call(call));
		if (it == psks_.end()) it = psks_.find("*");
		return it == psks_.end() ? nullptr : &it->second;
	}
	const std::map<std::string, Key32> &psks() const { return psks_; }
	std::string set_psk(const std::string &name, const Key32 &k)
	{
		if (!valid_psk_name(name)) return "a pre-shared key's name is a call sign or *";
		const std::string le = ensure_lock();
		if (!le.empty()) return le;
		reread_psks();
		psks_[upper(name)] = k;
		return save_psks();
	}
	std::string delete_psk(const std::string &name)
	{
		const std::string le = ensure_lock();
		if (!le.empty()) return le;
		reread_psks();
		if (!psks_.erase(upper(name))) return "no pre-shared key named " + upper(name);
		return save_psks();
	}
	static std::string base_call(const std::string &c) { const std::string u = upper(c); return u.substr(0, u.find('-')); }

private:
	std::string path(const char *f) const { return dir_ + "/" + f; }
	// The files read again into memory (a missing file: none). Called before every change.
	void reread_station()
	{
		std::string t;
		if (!read_file("station.key", t)) return;
		auto bl = parse_blocks(t);
		KeyPair kp;
		if (bl.size() == 1 && bl[0].kind == "PRIVATE KEY" && keypair_from_private(bl[0].key, kp)) me_ = kp;
		wipe(&t[0], t.size());
	}
	void reread_peers();
	void reread_psks()
	{
		std::string t;
		psks_.clear();
		if (read_file("psk", t)) { for (auto &f : lines_fields(t)) { Key32 k; if (f.size() >= 2 && valid_psk_name(f[0]) && unb64_32(f[1], k)) psks_[upper(f[0])] = k; } wipe(&t[0], t.size()); }
	}
	void reread_received()
	{
		std::string t;
		received_.clear();
		if (read_file("received", t)) for (auto &f : lines_fields(t)) {
			Peer p;
			if (f.size() >= 2 && valid_call(f[0]) && unb64_32(f[1], p.pub)) { p.since = f.size() > 2 ? std::atoll(f[2].c_str()) : 0; received_[upper(f[0])] = p; }
		}
	}
	bool read_file(const char *f, std::string &t) const
	{
		t.clear();
		FILE *fp = std::fopen(path(f).c_str(), "r");
		if (!fp) return false;
		char b[4096];
		size_t n;
		while ((n = std::fread(b, 1, sizeof b, fp)) > 0 && t.size() < (1u << 20)) t.append(b, n);
		std::fclose(fp);
		wipe(b, sizeof b);
		return true;
	}
	static std::vector<std::vector<std::string>> lines_fields(const std::string &t);
	std::string mkdirs() const;
	// Written to a new file (0600) and renamed over the old, so a crash never leaves half a file.
	std::string write_file(const char *f, const std::string &t, bool secret) const;
	std::string save_peers() const;
	std::string save_received() const
	{
		std::string t = "# Signum: public keys received over the air, not yet accepted (CALL KEY WHEN)\n";
		for (auto &p : received_) t += p.first + " " + b64(p.second.pub.data(), 32) + " " + std::to_string(p.second.since) + "\n";
		return write_file("received", t, false);
	}
	std::string save_psks() const
	{
		std::string t = "# Signum: pre-shared keys (NAME KEY; NAME a call sign or * for every station). SECRET.\n";
		for (auto &p : psks_) t += p.first + " " + b64(p.second.data(), 32) + "\n";
		const std::string e = write_file("psk", t, true);
		wipe(&t[0], t.size());
		return e;
	}
	std::string dir_, lock_err_;
	int lock_fd_ = -1;
	KeyPair me_;
	std::map<std::string, Peer> peers_, received_;
	std::map<std::string, Key32> psks_;
};

}  // namespace signum::crypto
