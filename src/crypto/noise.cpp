// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jeff Francis, N0GQ
//
// The functions of noise.hpp (out of the header, so that they build in parallel).
#include "noise.hpp"

namespace signum::crypto {

bool x25519(const Key32 &priv, const Key32 &pub, Key32 &out)
{
	EVP_PKEY *a = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, priv.data(), priv.size());
	EVP_PKEY *b = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, pub.data(), pub.size());
	EVP_PKEY_CTX *c = a ? EVP_PKEY_CTX_new(a, nullptr) : nullptr;
	size_t n = out.size();
	const bool ok = a && b && c && EVP_PKEY_derive_init(c) == 1 && EVP_PKEY_derive_set_peer(c, b) == 1 &&
	                EVP_PKEY_derive(c, out.data(), &n) == 1 && n == 32;
	EVP_PKEY_CTX_free(c);
	EVP_PKEY_free(b);
	EVP_PKEY_free(a);
	return ok;
}

bool aesgcm(bool enc, const Key32 &k, uint64_t n, const uint8_t *ad, size_t adn, const uint8_t *in, size_t inn, Bytes &out)
{
	uint8_t iv[12] = {0};
	for (int i = 0; i < 8; i++) iv[4 + i] = uint8_t(n >> (56 - 8 * i));
	if (!enc && inn < 16) return false;
	const size_t body = enc ? inn : inn - 16;
	out.assign(body + (enc ? 16 : 0), 0);
	EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
	if (!c) return false;
	int l = 0;
	bool ok = (enc ? EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) : EVP_DecryptInit_ex(c, EVP_aes_256_gcm(), nullptr, nullptr, nullptr)) == 1 &&
	          EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1 &&
	          (enc ? EVP_EncryptInit_ex(c, nullptr, nullptr, k.data(), iv) : EVP_DecryptInit_ex(c, nullptr, nullptr, k.data(), iv)) == 1;
	if (ok && adn) ok = (enc ? EVP_EncryptUpdate(c, nullptr, &l, ad, int(adn)) : EVP_DecryptUpdate(c, nullptr, &l, ad, int(adn))) == 1;
	if (ok && body) ok = (enc ? EVP_EncryptUpdate(c, out.data(), &l, in, int(body)) : EVP_DecryptUpdate(c, out.data(), &l, in, int(body))) == 1;
	if (ok) {
		if (enc) ok = EVP_EncryptFinal_ex(c, out.data() + body, &l) == 1 && EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, out.data() + body) == 1;
		else ok = EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, 16, const_cast<uint8_t *>(in + body)) == 1 && EVP_DecryptFinal_ex(c, out.data() + body, &l) == 1;
	}
	EVP_CIPHER_CTX_free(c);
	if (!ok) { wipe(out.data(), out.size()); out.clear(); }
	return ok;
}

}  // namespace signum::crypto
