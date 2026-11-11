#ifndef AUTH_H
#define AUTH_H

#include <stddef.h>
#include "protocol.h"

/*
 * PBKDF2-HMAC-SHA256 password hashing (600 000 iterations, 32-byte output).
 * Salt is 16 random bytes from OpenSSL RAND_bytes, hex-encoded (32 chars).
 * New hashes store their work factor; verification also accepts the old
 * 100 000-iteration bare-hex format for existing accounts.
 * verify_password uses CRYPTO_memcmp for constant-time comparison.
 */

/* Fill `salt` with SALT_HEX_LEN-1 random hex characters + NUL. */
int generate_salt(char *salt, size_t len);

/* Derive a hex-encoded hash of password+salt; write into out_hash (MAX_HASH bytes). */
int hash_password(const char *password, const char *salt,
                  char *out_hash, size_t hash_len);

/* Return 1 if password matches stored_hash/salt, 0 otherwise. */
int verify_password(const char *password, const char *salt,
                    const char *stored_hash);

#endif /* AUTH_H */
