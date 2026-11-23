/* x509.h - checking certificates: is this server really the site it says?

   A server proves who it is with a chain of certificates: its own (naming
   the site), signed by an authority's, signed by ... a root authority this
   machine trusts. The trusted roots are in /roots.pem (the same list
   Firefox and most systems use); to trust another authority, append its
   certificate (PEM) to that file.

   Checked: every signature in the chain (RSA PKCS#1 v1.5 and PSS with
   SHA-256/384/512, ECDSA on P-256 and P-384), the dates, that the
   authorities are allowed to sign certificates, and that the site's
   certificate names the host that was asked for. Not checked: revocation
   (whether an authority has withdrawn a certificate early). */
#pragma once
#include "rdx.h"

/* ---- SHA-384 and SHA-512 (FIPS 180-4) ---- */
typedef struct {
    u64 state[8];
    u64 length;
    unsigned char block[128];
    unsigned used, out_len;
} sha512_ctx;
void sha512_init(sha512_ctx *c);
void sha384_init(sha512_ctx *c);
void sha512_update(sha512_ctx *c, const void *data, size_t len);
void sha512_final(sha512_ctx *c, unsigned char *out); /* 64 bytes, or 48 for SHA-384 */
void sha512(const void *data, size_t len, unsigned char out[64]);
void sha384(const void *data, size_t len, unsigned char out[48]);

/* Check a server's certificate chain (DER, the site's own first) for
   `host`. Returns 0 if a trusted root vouches for it, with the name of the
   authority that issued it in `who`; otherwise -1 with the reason in
   `error` (at least 160 bytes). */
int x509_check_chain(const unsigned char *const *certs, const size_t *lens, int count, const char *host, char *who,
                     size_t who_max, char *error);

/* Check `sig`, a signature over `data` made with the key in certificate
   `cert`, using TLS signature scheme `scheme` (0x0401 rsa_pkcs1_sha256,
   0x0804 rsa_pss_rsae_sha256, 0x0403 ecdsa_secp256r1_sha256, ...).
   0 if it is right. */
int x509_check_signature(const unsigned char *cert, size_t cert_len, int scheme, const unsigned char *data,
                         size_t data_len, const unsigned char *sig, size_t sig_len);

/* One line about a certificate: who it is for, who issued it, until when. */
void x509_describe(const unsigned char *cert, size_t len, char *out, size_t max);

/* Elliptic-curve Diffie-Hellman on P-256 (for key exchange): a new key
   pair (public key uncompressed, 65 bytes), and the shared secret (the x
   coordinate) with a peer's public key. 0 on success. */
int p256_keypair(unsigned char priv[32], unsigned char pub[65]);
int p256_shared(unsigned char out[32], const unsigned char priv[32], const unsigned char *peer, size_t peer_len);
/* The same for P-256 (bits 256: 32-byte keys, 65-byte public) or P-384
   (bits 384: 48-byte keys, 97-byte public, 48-byte secret). */
int ecdh_keypair(int bits, unsigned char *priv, unsigned char *pub);
int ecdh_shared(int bits, unsigned char *out, const unsigned char *priv, const unsigned char *peer, size_t peer_len);

/* Check a raw signature with a raw key, for tests:
   kind 'r': key = RSA modulus n (with e in key2), 'e': key = EC point
   (65 or 97 bytes). Same schemes as above. 0 if right. */
int x509_check_raw(int kind, const unsigned char *key, size_t key_len, const unsigned char *key2, size_t key2_len,
                   int scheme, const unsigned char *data, size_t data_len, const unsigned char *sig, size_t sig_len);
void x509_prepare(void); /* load the trusted roots (once, before using threads) */
