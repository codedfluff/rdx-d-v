/* tls.h - encrypted connections (TLS 1.3), for https.

   Written from scratch for RDX, with its own cryptography: X25519 to agree
   a key, SHA-256 / HMAC / HKDF to derive keys, and ChaCha20-Poly1305 or
   AES-128-GCM (whichever the server picks) to encrypt.

   The server's certificate chain is checked (x509.c) against the trusted
   authorities in /roots.pem, and so is its signature over the handshake,
   which proves it holds the certificate's key. Not checked: revocation
   (an authority withdrawing a certificate before it expires).
   tls_describe() says whether the site's identity was verified.

   Usage:
       int fd = tcp_connect(ip, 443);
       char why[160];
       tls_session *t = tls_connect(fd, "example.com", why);
       if (!t) { printf("failed: %s\n", why); ... }
       tls_write(t, request, len);
       while ((n = tls_read(t, buf, sizeof(buf), 15000)) > 0) ...
       tls_close(t); close(fd); */
#pragma once
#include "rdx.h"

typedef struct tls_session tls_session;

/* Do the handshake over an open TCP connection. NULL on failure, with the
   reason in `error` (at least 160 bytes). `host` is sent so that a server
   hosting several sites knows which one is wanted, and its certificate
   must name it and be vouched for by a trusted authority (see x509.h);
   if not, the error starts "untrusted certificate:".
   Setting the environment variable TLS_INSECURE=1 connects anyway. */
tls_session *tls_connect(int fd, const char *host, char *error);
/* The same, with flags: TLS_NO_VERIFY connects even if the site's
   identity cannot be verified (tls_describe then says so, and why). */
#define TLS_NO_VERIFY 1
tls_session *tls_connect_options(int fd, const char *host, int flags, char *error);
/* Send data. 0 on success, -1 if the connection has failed. */
int tls_write(tls_session *t, const void *data, size_t len);
/* Receive up to `max` bytes, waiting up to timeout_ms for them. Returns
   the number of bytes, 0 when the server has finished, -1 on an error or
   if the time runs out. */
i64 tls_read(tls_session *t, void *buf, size_t max, int timeout_ms);
/* Say goodbye (close_notify) and free the session. Does not close fd. */
void tls_close(tls_session *t);
/* "encrypted (TLS 1.3, AES-128-GCM), identity verified by Let's Encrypt",
   or "..., site identity NOT verified: <why>" */
void tls_describe(tls_session *t, char *out, size_t max);

/* ---- the cryptography, for tests and for anyone who needs it ---- */
typedef struct {
    unsigned state[8];
    u64 length;
    unsigned char block[64];
    unsigned used;
} sha256_ctx;
void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);
void sha256_final(sha256_ctx *c, unsigned char out[32]);
void sha256(const void *data, size_t len, unsigned char out[32]);
void hmac_sha256(const void *key, size_t key_len, const void *data, size_t len, unsigned char out[32]);
void x25519(unsigned char out[32], const unsigned char scalar[32], const unsigned char point[32]);
void chacha20_xor(const unsigned char key[32], unsigned counter, const unsigned char nonce[12], unsigned char *data,
                  size_t len);
void poly1305(unsigned char tag[16], const unsigned char *msg, size_t len, const unsigned char key[32]);
void aes128_encrypt_block(const unsigned char key[16], const unsigned char in[16], unsigned char out[16]);
/* AEAD: encrypt in place and write the 16-byte tag; decrypt in place and
   return 0 only if the tag is right. cipher 0: ChaCha20-Poly1305 (32-byte
   key), 1: AES-128-GCM (16-byte key). */
void aead_seal(int cipher, const unsigned char *key, const unsigned char nonce[12], const unsigned char *aad,
               size_t aad_len, unsigned char *data, size_t len, unsigned char tag[16]);
int aead_open(int cipher, const unsigned char *key, const unsigned char nonce[12], const unsigned char *aad,
              size_t aad_len, unsigned char *data, size_t len, const unsigned char tag[16]);
void tls_prepare(void); /* one-time set-up; call before making connections from several threads */
