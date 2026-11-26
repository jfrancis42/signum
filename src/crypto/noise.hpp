// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// noise.hpp: the Noise Protocol Framework (revision 34, noiseprotocol.org/noise.html), the part
// Signum's encryption uses: DH 25519, cipher AESGCM, hash SHA256, the handshake patterns NN, NNpsk0,
// IK, IKpsk2, XX and XXpsk3, written from the specification on top of OpenSSL's libcrypto
// (X25519, AES-256-GCM, SHA-256, HMAC). Checked against the cacophony and noise-c test vectors.
//
// Host layer only (file and OS access are elsewhere; this file allocates): never in src/modem.
#pragma once
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace signum::crypto {

using Bytes = std::vector<uint8_t>;
using Key32 = std::array<uint8_t, 32>;

inline void wipe(void *p, size_t n) { OPENSSL_cleanse(p, n); }
inline Bytes to_bytes(const std::string &s) { return Bytes(s.begin(), s.end()); }

// ---- primitives --------------------------------------------------------------------------------
inline bool random_bytes(uint8_t *p, size_t n) { return RAND_bytes(p, int(n)) == 1; }

inline Key32 sha256(const uint8_t *p, size_t n)
{
	Key32 h{};
	unsigned int l = 0;
	EVP_Digest(p, n, h.data(), &l, EVP_sha256(), nullptr);
	return h;
}
inline Key32 sha256(const Bytes &b) { return sha256(b.data(), b.size()); }

inline Key32 hmac256(const uint8_t *k, size_t kn, const uint8_t *d, size_t dn)
{
	Key32 o{};
	unsigned int l = 0;
	HMAC(EVP_sha256(), k, int(kn), d, dn, o.data(), &l);
	return o;
}

struct KeyPair {
	Key32 priv{}, pub{};
	bool set = false;
	~KeyPair() { wipe(priv.data(), priv.size()); }
};

// The public key of a 25519 private key (false: OpenSSL refused it).
inline bool x25519_public(const Key32 &priv, Key32 &pub)
{
	EVP_PKEY *k = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, priv.data(), priv.size());
	if (!k) return false;
	size_t n = pub.size();
	const bool ok = EVP_PKEY_get_raw_public_key(k, pub.data(), &n) == 1 && n == 32;
	EVP_PKEY_free(k);
	return ok;
}
inline bool keypair_from_private(const Key32 &priv, KeyPair &kp)
{
	kp.priv = priv;
	kp.set = x25519_public(priv, kp.pub);
	return kp.set;
}
inline bool generate_keypair(KeyPair &kp)
{
	Key32 p{};
	if (!random_bytes(p.data(), p.size())) return false;
	const bool ok = keypair_from_private(p, kp);
	wipe(p.data(), p.size());
	return ok;
}
// DH(priv, pub) (false: OpenSSL refused, e.g. an all-zero result from a low-order point)
bool x25519(const Key32 &priv, const Key32 &pub, Key32 &out);

// AES-256-GCM as Noise's AESGCM: nonce = 4 zero bytes + the counter big-endian; ciphertext || 16-byte tag.
bool aesgcm(bool enc, const Key32 &k, uint64_t n, const uint8_t *ad, size_t adn, const uint8_t *in, size_t inn, Bytes &out);

// ---- CipherState ---------------------------------------------------------------------------------
class CipherState {
public:
	~CipherState() { wipe(k_.data(), k_.size()); }
	void init(const Key32 &k) { k_ = k; has_ = true; n_ = 0; }
	bool has_key() const { return has_; }
	uint64_t nonce() const { return n_; }
	void set_nonce(uint64_t n) { n_ = n; }
	// false: the nonce is used up (2^64-1 is reserved) or the cipher failed
	bool encrypt(const uint8_t *ad, size_t adn, const uint8_t *p, size_t pn, Bytes &out)
	{
		if (!has_) { out.assign(p, p + pn); return true; }
		if (n_ == ~uint64_t(0)) return false;
		if (!aesgcm(true, k_, n_, ad, adn, p, pn, out)) return false;
		n_++;
		return true;
	}
	// false: authentication failed (the nonce is not advanced: the caller ends the session)
	bool decrypt(const uint8_t *ad, size_t adn, const uint8_t *c, size_t cn, Bytes &out)
	{
		if (!has_) { out.assign(c, c + cn); return true; }
		if (n_ == ~uint64_t(0)) return false;
		if (!aesgcm(false, k_, n_, ad, adn, c, cn, out)) return false;
		n_++;
		return true;
	}
	// Noise's Rekey(): k = the first 32 bytes of ENCRYPT(k, 2^64-1, "", 32 zero bytes)
	bool rekey()
	{
		Bytes o;
		const uint8_t z[32] = {0};
		if (!aesgcm(true, k_, ~uint64_t(0), nullptr, 0, z, 32, o)) return false;
		std::memcpy(k_.data(), o.data(), 32);
		wipe(o.data(), o.size());
		return true;
	}

private:
	Key32 k_{};
	bool has_ = false;
	uint64_t n_ = 0;
};

// ---- SymmetricState ------------------------------------------------------------------------------
class SymmetricState {
public:
	~SymmetricState() { wipe(ck_.data(), 32); wipe(h_.data(), 32); }
	void init(const std::string &name)
	{
		h_.fill(0);
		if (name.size() <= 32) std::memcpy(h_.data(), name.data(), name.size());
		else h_ = sha256(reinterpret_cast<const uint8_t *>(name.data()), name.size());
		ck_ = h_;
	}
	void mix_hash(const uint8_t *d, size_t n)
	{
		Bytes b(h_.begin(), h_.end());
		b.insert(b.end(), d, d + n);
		h_ = sha256(b);
	}
	void mix_key(const uint8_t *ikm, size_t n)
	{
		Key32 o1, o2, o3;
		hkdf(ikm, n, o1, o2, o3, 2);
		ck_ = o1;
		cs_.init(o2);
		wipe(o2.data(), 32);
	}
	void mix_key_and_hash(const uint8_t *ikm, size_t n)
	{
		Key32 o1, o2, o3;
		hkdf(ikm, n, o1, o2, o3, 3);
		ck_ = o1;
		mix_hash(o2.data(), 32);
		cs_.init(o3);
		wipe(o3.data(), 32);
	}
	bool encrypt_and_hash(const uint8_t *p, size_t n, Bytes &out)
	{
		if (!cs_.encrypt(h_.data(), 32, p, n, out)) return false;
		mix_hash(out.data(), out.size());
		return true;
	}
	bool decrypt_and_hash(const uint8_t *c, size_t n, Bytes &out)
	{
		if (!cs_.decrypt(h_.data(), 32, c, n, out)) return false;
		mix_hash(c, n);
		return true;
	}
	void split(CipherState &c1, CipherState &c2)
	{
		Key32 o1, o2, o3;
		hkdf(nullptr, 0, o1, o2, o3, 2);
		c1.init(o1);
		c2.init(o2);
		wipe(o1.data(), 32); wipe(o2.data(), 32);
	}
	bool has_key() const { return cs_.has_key(); }
	const Key32 &h() const { return h_; }

private:
	void hkdf(const uint8_t *ikm, size_t n, Key32 &o1, Key32 &o2, Key32 &o3, int outs)
	{
		Key32 tk = hmac256(ck_.data(), 32, ikm, n);
		const uint8_t one = 1;
		o1 = hmac256(tk.data(), 32, &one, 1);
		uint8_t b[33];
		std::memcpy(b, o1.data(), 32); b[32] = 2;
		o2 = hmac256(tk.data(), 32, b, 33);
		if (outs == 3) { std::memcpy(b, o2.data(), 32); b[32] = 3; o3 = hmac256(tk.data(), 32, b, 33); }
		wipe(tk.data(), 32); wipe(b, sizeof b);
	}
	Key32 ck_{}, h_{};
	CipherState cs_;
};

// ---- HandshakeState ------------------------------------------------------------------------------
enum class Pattern { NN, NNpsk0, IK, IKpsk2, XX, XXpsk3 };
inline const char *pattern_name(Pattern p)
{
	switch (p) {
	case Pattern::NN: return "NN";
	case Pattern::NNpsk0: return "NNpsk0";
	case Pattern::IK: return "IK";
	case Pattern::IKpsk2: return "IKpsk2";
	case Pattern::XX: return "XX";
	default: return "XXpsk3";
	}
}
inline bool pattern_from_name(const std::string &s, Pattern &p)
{
	for (Pattern q : {Pattern::NN, Pattern::NNpsk0, Pattern::IK, Pattern::IKpsk2, Pattern::XX, Pattern::XXpsk3})
		if (s == pattern_name(q)) { p = q; return true; }
	return false;
}
inline std::string protocol_name(Pattern p) { return std::string("Noise_") + pattern_name(p) + "_25519_AESGCM_SHA256"; }

class Handshake {
public:
	enum Tok { E, S, EE, ES, SE, SS, PSK };
	// s: our static key pair (IK, XX); rs: the responder's static public key, known to an IK initiator;
	// psk: 32 bytes for the psk patterns.
	void init(Pattern p, bool initiator, const Bytes &prologue, const KeyPair *s, const Key32 *rs, const Key32 *psk)
	{
		pat_ = p; init_ = initiator; msg_ = 0; done_ = false;
		if (s) { s_.priv = s->priv; s_.pub = s->pub; s_.set = s->set; }
		if (rs) { rs_ = *rs; has_rs_ = true; }
		if (psk) { psk_ = *psk; has_psk_ = true; }
		ss_.init(protocol_name(p));
		ss_.mix_hash(prologue.data(), prologue.size());
		if (p == Pattern::IK || p == Pattern::IKpsk2) {     // pre-message <- s
			if (init_) ss_.mix_hash(rs_.data(), 32);
			else ss_.mix_hash(s_.pub.data(), 32);
		}
		switch (p) {
		case Pattern::NN: msgs_ = {{E}, {E, EE}}; break;
		case Pattern::NNpsk0: msgs_ = {{PSK, E}, {E, EE}}; break;
		case Pattern::IK: msgs_ = {{E, ES, S, SS}, {E, EE, SE}}; break;
		case Pattern::IKpsk2: msgs_ = {{E, ES, S, SS}, {E, EE, SE, PSK}}; break;
		case Pattern::XX: msgs_ = {{E}, {E, EE, S, ES}, {S, SE}}; break;
		case Pattern::XXpsk3: msgs_ = {{E}, {E, EE, S, ES}, {S, SE, PSK}}; break;
		}
		psk_mode_ = p == Pattern::NNpsk0 || p == Pattern::IKpsk2 || p == Pattern::XXpsk3;
	}
	// A fixed ephemeral key (the test vectors only).
	void set_ephemeral(const Key32 &priv) { keypair_from_private(priv, e_); fixed_e_ = true; }
	bool my_turn() const { return !done_ && (msg_ % 2 == 0) == init_; }
	bool done() const { return done_; }
	int message_index() const { return msg_; }
	int message_count() const { return int(msgs_.size()); }
	bool write(const Bytes &payload, Bytes &out)
	{
		if (!my_turn()) return false;
		out.clear();
		for (Tok t : msgs_[size_t(msg_)]) {
			switch (t) {
			case E:
				if (!fixed_e_ && !generate_keypair(e_)) return false;
				out.insert(out.end(), e_.pub.begin(), e_.pub.end());
				ss_.mix_hash(e_.pub.data(), 32);
				if (psk_mode_) ss_.mix_key(e_.pub.data(), 32);
				break;
			case S: {
				if (!s_.set) return false;
				Bytes c;
				if (!ss_.encrypt_and_hash(s_.pub.data(), 32, c)) return false;
				out.insert(out.end(), c.begin(), c.end());
				break;
			}
			case PSK: if (!has_psk_) return false; ss_.mix_key_and_hash(psk_.data(), 32); break;
			default: if (!dh(t)) return false; break;
			}
		}
		Bytes c;
		if (!ss_.encrypt_and_hash(payload.data(), payload.size(), c)) return false;
		out.insert(out.end(), c.begin(), c.end());
		advance();
		return true;
	}
	bool read(const Bytes &in, Bytes &payload)
	{
		if (done_ || my_turn()) return false;
		size_t i = 0;
		for (Tok t : msgs_[size_t(msg_)]) {
			switch (t) {
			case E:
				if (in.size() < i + 32) return false;
				std::memcpy(re_.data(), in.data() + i, 32); i += 32;
				ss_.mix_hash(re_.data(), 32);
				if (psk_mode_) ss_.mix_key(re_.data(), 32);
				break;
			case S: {
				const size_t n = ss_.has_key() ? 48 : 32;
				if (in.size() < i + n) return false;
				Bytes p;
				if (!ss_.decrypt_and_hash(in.data() + i, n, p) || p.size() != 32) return false;
				std::memcpy(rs_.data(), p.data(), 32); has_rs_ = true; i += n;
				break;
			}
			case PSK: if (!has_psk_) return false; ss_.mix_key_and_hash(psk_.data(), 32); break;
			default: if (!dh(t)) return false; break;
			}
		}
		if (!ss_.decrypt_and_hash(in.data() + i, in.size() - i, payload)) return false;
		advance();
		return true;
	}
	// After done(): the transport ciphers (send, receive) for this side, and the handshake hash.
	CipherState &tx() { return init_ ? c1_ : c2_; }
	CipherState &rx() { return init_ ? c2_ : c1_; }
	const Key32 &hash() const { return h_; }
	const Key32 &remote_static() const { return rs_; }
	bool has_remote_static() const { return has_rs_; }
	~Handshake() { wipe(psk_.data(), 32); }

private:
	void advance()
	{
		msg_++;
		if (msg_ >= int(msgs_.size())) { done_ = true; h_ = ss_.h(); ss_.split(c1_, c2_); }
	}
	bool dh(Tok t)
	{
		Key32 o;
		bool ok = false;
		switch (t) {
		case EE: ok = x25519(e_.priv, re_, o); break;
		case ES: ok = init_ ? (has_rs_ && x25519(e_.priv, rs_, o)) : (s_.set && x25519(s_.priv, re_, o)); break;
		case SE: ok = init_ ? (s_.set && x25519(s_.priv, re_, o)) : (has_rs_ && x25519(e_.priv, rs_, o)); break;
		case SS: ok = s_.set && has_rs_ && x25519(s_.priv, rs_, o); break;
		default: break;
		}
		if (ok) ss_.mix_key(o.data(), 32);
		wipe(o.data(), 32);
		return ok;
	}
	Pattern pat_ = Pattern::NN;
	bool init_ = false, done_ = false, psk_mode_ = false, fixed_e_ = false, has_rs_ = false, has_psk_ = false;
	int msg_ = 0;
	std::vector<std::vector<Tok>> msgs_;
	SymmetricState ss_;
	KeyPair s_, e_;
	Key32 re_{}, rs_{}, psk_{}, h_{};
	CipherState c1_, c2_;
};

}  // namespace signum::crypto
