// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_E2E_H
#define CHIAKI_E2E_H

#include "common.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Oviora: end-to-end encrypted messages between friends. Every device has an X25519 identity key pair;
 * only the public key leaves the device. A message is encrypted separately for each device that should
 * read it:
 *
 *   e = fresh X25519 key pair (per message and device)
 *   ikm = X25519(e, recipient) || X25519(sender, recipient)
 *   key = HKDF-SHA256(ikm, salt = e.pub || sender.pub || recipient.pub, info = "Oviora E2E v1", 32 bytes)
 *   ct = AES-256-GCM(key, nonce = 12 random bytes, aad, plaintext) with the 16 byte tag appended
 *
 * The static-static part ties the message to the sender's key (only the holder of that private key can
 * make a message the recipient opens with that sender key); the ephemeral part means a later leak of the
 * sender's private key alone does not open old messages. aad binds the message to its metadata (the
 * apps put sender user, recipient user and message id there) so the server cannot move it elsewhere.
 */

#define CHIAKI_E2E_KEY_SIZE 32
#define CHIAKI_E2E_NONCE_SIZE 12
#define CHIAKI_E2E_TAG_SIZE 16
/** Bytes of the "safety code" fingerprint (shown as digit groups by the apps) */
#define CHIAKI_E2E_FINGERPRINT_SIZE 32

/** A new identity key pair */
CHIAKI_EXPORT ChiakiErrorCode chiaki_e2e_keygen(uint8_t priv[CHIAKI_E2E_KEY_SIZE], uint8_t pub[CHIAKI_E2E_KEY_SIZE]);

/** The public key of a private key */
CHIAKI_EXPORT ChiakiErrorCode chiaki_e2e_public(const uint8_t priv[CHIAKI_E2E_KEY_SIZE], uint8_t pub[CHIAKI_E2E_KEY_SIZE]);

/**
 * Encrypts for one recipient device. out must hold plain_len + CHIAKI_E2E_TAG_SIZE bytes.
 * eph_pub and nonce are written and travel with the message.
 */
CHIAKI_EXPORT ChiakiErrorCode chiaki_e2e_encrypt(
	const uint8_t sender_priv[CHIAKI_E2E_KEY_SIZE], const uint8_t recipient_pub[CHIAKI_E2E_KEY_SIZE],
	const uint8_t *aad, size_t aad_len, const uint8_t *plain, size_t plain_len,
	uint8_t eph_pub[CHIAKI_E2E_KEY_SIZE], uint8_t nonce[CHIAKI_E2E_NONCE_SIZE], uint8_t *out);

/**
 * Opens a message for this device. ct_len includes the tag; out must hold ct_len - CHIAKI_E2E_TAG_SIZE bytes.
 * CHIAKI_ERR_INVALID_MAC when it was not made by sender_pub for us, or was changed.
 */
CHIAKI_EXPORT ChiakiErrorCode chiaki_e2e_decrypt(
	const uint8_t recipient_priv[CHIAKI_E2E_KEY_SIZE], const uint8_t sender_pub[CHIAKI_E2E_KEY_SIZE],
	const uint8_t eph_pub[CHIAKI_E2E_KEY_SIZE], const uint8_t nonce[CHIAKI_E2E_NONCE_SIZE],
	const uint8_t *aad, size_t aad_len, const uint8_t *ct, size_t ct_len, uint8_t *out);

/**
 * The safety code of two keys, the same on both sides (order-independent): SHA-256 over the two public
 * keys sorted. Friends compare it to know nobody (not even the server) swapped a key.
 */
CHIAKI_EXPORT void chiaki_e2e_fingerprint(const uint8_t a_pub[CHIAKI_E2E_KEY_SIZE], const uint8_t b_pub[CHIAKI_E2E_KEY_SIZE],
	uint8_t out[CHIAKI_E2E_FINGERPRINT_SIZE]);

#ifdef __cplusplus
}
#endif

#endif // CHIAKI_E2E_H
