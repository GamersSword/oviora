// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <chiaki/e2e.h>

#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>

#include <string.h>

static const char E2E_INFO[] = "Oviora E2E v1";

static EVP_PKEY *x25519_priv(const uint8_t priv[CHIAKI_E2E_KEY_SIZE])
{
	return EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, priv, CHIAKI_E2E_KEY_SIZE);
}

static EVP_PKEY *x25519_pub(const uint8_t pub[CHIAKI_E2E_KEY_SIZE])
{
	return EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, pub, CHIAKI_E2E_KEY_SIZE);
}

/** X25519(priv, pub) into out (32 bytes); rejects an all-zero result (a low-order public key) */
static ChiakiErrorCode dh(const uint8_t priv[CHIAKI_E2E_KEY_SIZE], const uint8_t pub[CHIAKI_E2E_KEY_SIZE], uint8_t out[CHIAKI_E2E_KEY_SIZE])
{
	ChiakiErrorCode err = CHIAKI_ERR_UNKNOWN;
	EVP_PKEY *a = x25519_priv(priv);
	EVP_PKEY *b = x25519_pub(pub);
	EVP_PKEY_CTX *ctx = a ? EVP_PKEY_CTX_new(a, NULL) : NULL;
	size_t len = CHIAKI_E2E_KEY_SIZE;
	if(ctx && b && EVP_PKEY_derive_init(ctx) == 1 && EVP_PKEY_derive_set_peer(ctx, b) == 1
		&& EVP_PKEY_derive(ctx, out, &len) == 1 && len == CHIAKI_E2E_KEY_SIZE)
	{
		uint8_t zero[CHIAKI_E2E_KEY_SIZE] = { 0 };
		err = CRYPTO_memcmp(out, zero, sizeof(zero)) == 0 ? CHIAKI_ERR_INVALID_DATA : CHIAKI_ERR_SUCCESS;
	}
	EVP_PKEY_CTX_free(ctx);
	EVP_PKEY_free(a);
	EVP_PKEY_free(b);
	return err;
}

static ChiakiErrorCode derive_key(const uint8_t ikm[2 * CHIAKI_E2E_KEY_SIZE], const uint8_t salt[3 * CHIAKI_E2E_KEY_SIZE],
	uint8_t key[CHIAKI_E2E_KEY_SIZE])
{
	ChiakiErrorCode err = CHIAKI_ERR_UNKNOWN;
	EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
	size_t len = CHIAKI_E2E_KEY_SIZE;
	if(ctx && EVP_PKEY_derive_init(ctx) == 1
		&& EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha256()) == 1
		&& EVP_PKEY_CTX_set1_hkdf_salt(ctx, salt, 3 * CHIAKI_E2E_KEY_SIZE) == 1
		&& EVP_PKEY_CTX_set1_hkdf_key(ctx, ikm, 2 * CHIAKI_E2E_KEY_SIZE) == 1
		&& EVP_PKEY_CTX_add1_hkdf_info(ctx, (const unsigned char *)E2E_INFO, sizeof(E2E_INFO) - 1) == 1
		&& EVP_PKEY_derive(ctx, key, &len) == 1 && len == CHIAKI_E2E_KEY_SIZE)
		err = CHIAKI_ERR_SUCCESS;
	EVP_PKEY_CTX_free(ctx);
	return err;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_e2e_public(const uint8_t priv[CHIAKI_E2E_KEY_SIZE], uint8_t pub[CHIAKI_E2E_KEY_SIZE])
{
	EVP_PKEY *k = x25519_priv(priv);
	size_t len = CHIAKI_E2E_KEY_SIZE;
	ChiakiErrorCode err = (k && EVP_PKEY_get_raw_public_key(k, pub, &len) == 1 && len == CHIAKI_E2E_KEY_SIZE)
		? CHIAKI_ERR_SUCCESS : CHIAKI_ERR_UNKNOWN;
	EVP_PKEY_free(k);
	return err;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_e2e_keygen(uint8_t priv[CHIAKI_E2E_KEY_SIZE], uint8_t pub[CHIAKI_E2E_KEY_SIZE])
{
	// X25519 clamps any 32 random bytes into a valid private key
	if(RAND_bytes(priv, CHIAKI_E2E_KEY_SIZE) != 1)
		return CHIAKI_ERR_UNKNOWN;
	return chiaki_e2e_public(priv, pub);
}

static ChiakiErrorCode aes_gcm(bool enc, const uint8_t key[CHIAKI_E2E_KEY_SIZE], const uint8_t nonce[CHIAKI_E2E_NONCE_SIZE],
	const uint8_t *aad, size_t aad_len, const uint8_t *in, size_t in_len, uint8_t *out, uint8_t tag[CHIAKI_E2E_TAG_SIZE])
{
	ChiakiErrorCode err = enc ? CHIAKI_ERR_UNKNOWN : CHIAKI_ERR_INVALID_MAC;
	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	int len = 0;
	if(!ctx)
		return CHIAKI_ERR_MEMORY;
	if(EVP_CipherInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL, enc ? 1 : 0) != 1
		|| EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, CHIAKI_E2E_NONCE_SIZE, NULL) != 1
		|| EVP_CipherInit_ex(ctx, NULL, NULL, key, nonce, enc ? 1 : 0) != 1)
		goto done;
	if(aad_len && EVP_CipherUpdate(ctx, NULL, &len, aad, (int)aad_len) != 1)
		goto done;
	if(in_len && EVP_CipherUpdate(ctx, out, &len, in, (int)in_len) != 1)
		goto done;
	if(!enc && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, CHIAKI_E2E_TAG_SIZE, tag) != 1)
		goto done;
	if(EVP_CipherFinal_ex(ctx, out + (in_len ? len : 0), &len) != 1)
		goto done;
	if(enc && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, CHIAKI_E2E_TAG_SIZE, tag) != 1)
		goto done;
	err = CHIAKI_ERR_SUCCESS;
done:
	EVP_CIPHER_CTX_free(ctx);
	return err;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_e2e_encrypt(
	const uint8_t sender_priv[CHIAKI_E2E_KEY_SIZE], const uint8_t recipient_pub[CHIAKI_E2E_KEY_SIZE],
	const uint8_t *aad, size_t aad_len, const uint8_t *plain, size_t plain_len,
	uint8_t eph_pub[CHIAKI_E2E_KEY_SIZE], uint8_t nonce[CHIAKI_E2E_NONCE_SIZE], uint8_t *out)
{
	uint8_t eph_priv[CHIAKI_E2E_KEY_SIZE], sender_pub[CHIAKI_E2E_KEY_SIZE];
	uint8_t ikm[2 * CHIAKI_E2E_KEY_SIZE], salt[3 * CHIAKI_E2E_KEY_SIZE], key[CHIAKI_E2E_KEY_SIZE];
	ChiakiErrorCode err = chiaki_e2e_keygen(eph_priv, eph_pub);
	if(err == CHIAKI_ERR_SUCCESS)
		err = chiaki_e2e_public(sender_priv, sender_pub);
	if(err == CHIAKI_ERR_SUCCESS)
		err = dh(eph_priv, recipient_pub, ikm);
	if(err == CHIAKI_ERR_SUCCESS)
		err = dh(sender_priv, recipient_pub, ikm + CHIAKI_E2E_KEY_SIZE);
	if(err == CHIAKI_ERR_SUCCESS)
	{
		memcpy(salt, eph_pub, CHIAKI_E2E_KEY_SIZE);
		memcpy(salt + CHIAKI_E2E_KEY_SIZE, sender_pub, CHIAKI_E2E_KEY_SIZE);
		memcpy(salt + 2 * CHIAKI_E2E_KEY_SIZE, recipient_pub, CHIAKI_E2E_KEY_SIZE);
		err = derive_key(ikm, salt, key);
	}
	if(err == CHIAKI_ERR_SUCCESS && RAND_bytes(nonce, CHIAKI_E2E_NONCE_SIZE) != 1)
		err = CHIAKI_ERR_UNKNOWN;
	if(err == CHIAKI_ERR_SUCCESS)
		err = aes_gcm(true, key, nonce, aad, aad_len, plain, plain_len, out, out + plain_len);
	OPENSSL_cleanse(eph_priv, sizeof(eph_priv));
	OPENSSL_cleanse(ikm, sizeof(ikm));
	OPENSSL_cleanse(key, sizeof(key));
	return err;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_e2e_decrypt(
	const uint8_t recipient_priv[CHIAKI_E2E_KEY_SIZE], const uint8_t sender_pub[CHIAKI_E2E_KEY_SIZE],
	const uint8_t eph_pub[CHIAKI_E2E_KEY_SIZE], const uint8_t nonce[CHIAKI_E2E_NONCE_SIZE],
	const uint8_t *aad, size_t aad_len, const uint8_t *ct, size_t ct_len, uint8_t *out)
{
	if(ct_len < CHIAKI_E2E_TAG_SIZE)
		return CHIAKI_ERR_INVALID_DATA;
	uint8_t recipient_pub[CHIAKI_E2E_KEY_SIZE];
	uint8_t ikm[2 * CHIAKI_E2E_KEY_SIZE], salt[3 * CHIAKI_E2E_KEY_SIZE], key[CHIAKI_E2E_KEY_SIZE];
	uint8_t tag[CHIAKI_E2E_TAG_SIZE];
	const size_t plain_len = ct_len - CHIAKI_E2E_TAG_SIZE;
	memcpy(tag, ct + plain_len, CHIAKI_E2E_TAG_SIZE);
	ChiakiErrorCode err = chiaki_e2e_public(recipient_priv, recipient_pub);
	if(err == CHIAKI_ERR_SUCCESS)
		err = dh(recipient_priv, eph_pub, ikm);
	if(err == CHIAKI_ERR_SUCCESS)
		err = dh(recipient_priv, sender_pub, ikm + CHIAKI_E2E_KEY_SIZE);
	if(err == CHIAKI_ERR_SUCCESS)
	{
		memcpy(salt, eph_pub, CHIAKI_E2E_KEY_SIZE);
		memcpy(salt + CHIAKI_E2E_KEY_SIZE, sender_pub, CHIAKI_E2E_KEY_SIZE);
		memcpy(salt + 2 * CHIAKI_E2E_KEY_SIZE, recipient_pub, CHIAKI_E2E_KEY_SIZE);
		err = derive_key(ikm, salt, key);
	}
	if(err == CHIAKI_ERR_SUCCESS)
		err = aes_gcm(false, key, nonce, aad, aad_len, ct, plain_len, out, tag);
	OPENSSL_cleanse(ikm, sizeof(ikm));
	OPENSSL_cleanse(key, sizeof(key));
	return err;
}

CHIAKI_EXPORT void chiaki_e2e_fingerprint(const uint8_t a_pub[CHIAKI_E2E_KEY_SIZE], const uint8_t b_pub[CHIAKI_E2E_KEY_SIZE],
	uint8_t out[CHIAKI_E2E_FINGERPRINT_SIZE])
{
	const bool a_first = memcmp(a_pub, b_pub, CHIAKI_E2E_KEY_SIZE) <= 0;
	uint8_t both[2 * CHIAKI_E2E_KEY_SIZE];
	memcpy(both, a_first ? a_pub : b_pub, CHIAKI_E2E_KEY_SIZE);
	memcpy(both + CHIAKI_E2E_KEY_SIZE, a_first ? b_pub : a_pub, CHIAKI_E2E_KEY_SIZE);
	unsigned int len = CHIAKI_E2E_FINGERPRINT_SIZE;
	EVP_Digest(both, sizeof(both), out, &len, EVP_sha256(), NULL);
}
