#include "auth.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>

#define PBKDF2_ITERS   600000
#define LEGACY_ITERS   100000
#define PBKDF2_KEYLEN  32     /* 32 bytes → 64 hex chars; fits in MAX_HASH=129 */
#define HASH_PREFIX    "pbkdf2_sha256$"
#define HASH_FORMAT_LEN (sizeof(HASH_PREFIX) - 1 + 6 + 1 + PBKDF2_KEYLEN * 2 + 1)

static int derive_hex(const char *password, const char *salt,
                      int iterations, char hex[PBKDF2_KEYLEN * 2 + 1]) {
    uint8_t dk[PBKDF2_KEYLEN];
    if (PKCS5_PBKDF2_HMAC(
        password, (int)strlen(password),
        (const unsigned char *)salt, (int)strlen(salt),
        iterations, EVP_sha256(),
        PBKDF2_KEYLEN, dk) != 1) {
        return -1;
    }
    for (int i = 0; i < PBKDF2_KEYLEN; i++)
        snprintf(hex + i * 2, 3, "%02x", dk[i]);
    OPENSSL_cleanse(dk, sizeof(dk));
    return 0;
}

int hash_password(const char *password, const char *salt,
                  char *out_hash, size_t hash_len) {
    if (!password || !salt || !out_hash || hash_len < HASH_FORMAT_LEN)
        return -1;
    char hex[PBKDF2_KEYLEN * 2 + 1];
    if (derive_hex(password, salt, PBKDF2_ITERS, hex) != 0) {
        out_hash[0] = '\0';
        return -1;
    }
    int n = snprintf(out_hash, hash_len, HASH_PREFIX "%d$%s",
                     PBKDF2_ITERS, hex);
    OPENSSL_cleanse(hex, sizeof(hex));
    return n > 0 && (size_t)n < hash_len ? 0 : -1;
}

int generate_salt(char *salt, size_t len) {
    if (!salt || len < SALT_HEX_LEN) return -1;
    uint8_t raw[SALT_LEN];

    if (RAND_bytes(raw, SALT_LEN) != 1) {
        salt[0] = '\0';
        return -1;
    }

    size_t out = 0;
    for (int i = 0; i < SALT_LEN && out + 2 < len; i++, out += 2)
        snprintf(salt + out, 3, "%02x", raw[i]);
    salt[out] = '\0';
    OPENSSL_cleanse(raw, sizeof(raw));
    return 0;
}

int verify_password(const char *password, const char *salt,
                    const char *stored_hash) {
    if (!password || !salt || !stored_hash ||
        strnlen(stored_hash, MAX_HASH) == MAX_HASH) return 0;
    int iterations = LEGACY_ITERS;
    const char *expected = stored_hash;
    if (strncmp(stored_hash, HASH_PREFIX, sizeof(HASH_PREFIX) - 1) == 0) {
        const char *work_factor = stored_hash + sizeof(HASH_PREFIX) - 1;
        char *end = NULL;
        errno = 0;
        long parsed = strtol(work_factor, &end, 10);
        if (errno || end == work_factor || !end || *end != '$' ||
            parsed < 1 || parsed > 10000000) return 0;
        iterations = (int)parsed;
        expected = end + 1;
    }
    if (strlen(expected) != PBKDF2_KEYLEN * 2) return 0;
    for (int i = 0; i < PBKDF2_KEYLEN * 2; i++)
        if (!((expected[i] >= '0' && expected[i] <= '9') ||
              (expected[i] >= 'a' && expected[i] <= 'f'))) return 0;

    char computed[PBKDF2_KEYLEN * 2 + 1];
    if (derive_hex(password, salt, iterations, computed) != 0) return 0;
    int match = CRYPTO_memcmp(computed, expected, PBKDF2_KEYLEN * 2) == 0;
    OPENSSL_cleanse(computed, sizeof(computed));
    return match;
}
