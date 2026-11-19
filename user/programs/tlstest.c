/* tlstest - checks RDX's cryptography against the official test vectors
   (from the standards that define each algorithm), so that https is
   built on parts known to be right. Prints one line per check and
   "CRYPTO OK" if they all pass. */
#include "tls.h"

static int failures;

static int unhex(const char *h, unsigned char *out) {
    int n = 0;
    while (h[0] && h[1]) {
        int a = h[0] <= '9' ? h[0] - '0' : (h[0] | 32) - 'a' + 10;
        int b = h[1] <= '9' ? h[1] - '0' : (h[1] | 32) - 'a' + 10;
        out[n++] = (unsigned char)(a << 4 | b);
        h += 2;
    }
    return n;
}

static void check(const char *what, const unsigned char *got, const char *want_hex) {
    unsigned char want[128];
    int n = unhex(want_hex, want);
    int ok = memcmp(got, want, (size_t)n) == 0;
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        failures++;
        printf("      got  ");
        for (int i = 0; i < n; i++) printf("%02x", got[i]);
        printf("\n      want %s\n", want_hex);
    }
}

int main(void) {
    unsigned char out[128], key[64], msg[256], nonce[12], tag[16];

    /* SHA-256: FIPS 180-4 examples. */
    sha256("abc", 3, out);
    check("SHA-256 of \"abc\"", out, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char *two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256(two_blocks, strlen(two_blocks), out);
    check("SHA-256 of a two-block message", out, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    /* HMAC-SHA-256: RFC 4231 test cases 1 and 2. */
    memset(key, 0x0b, 20);
    hmac_sha256(key, 20, "Hi There", 8, out);
    check("HMAC-SHA-256, RFC 4231 case 1", out, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, out);
    check("HMAC-SHA-256, RFC 4231 case 2", out, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    /* X25519: RFC 7748 section 5.2, and the Diffie-Hellman example in 6.1. */
    unsigned char scalar[32], point[32];
    unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", scalar);
    unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", point);
    x25519(out, scalar, point);
    check("X25519, RFC 7748 vector 1", out, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
    unsigned char alice[32], bob_public[32], base[32] = {9};
    unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", alice);
    x25519(out, alice, base);
    check("X25519, Alice's public key (RFC 7748 6.1)", out,
          "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    unhex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", bob_public);
    x25519(out, alice, bob_public);
    check("X25519, shared secret (RFC 7748 6.1)", out,
          "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");

    /* ChaCha20: RFC 8439 2.4.2. */
    for (int i = 0; i < 32; i++) key[i] = (unsigned char)i;
    unhex("000000000000004a00000000", nonce);
    const char *sunscreen = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the "
                            "future, sunscreen would be it.";
    size_t len = strlen(sunscreen);
    memcpy(msg, sunscreen, len);
    chacha20_xor(key, 1, nonce, msg, len);
    check("ChaCha20, RFC 8439 2.4.2", msg, "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0bf91b65c5524733ab8f593dabcd62b3571639d624e65152ab8f530c359f0861d807ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab77937365af90bbf74a35be6b40b8eedf2785e42874d");

    /* Poly1305: RFC 8439 2.5.2. */
    unhex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b", key);
    poly1305(tag, (const unsigned char *)"Cryptographic Forum Research Group", 34, key);
    check("Poly1305, RFC 8439 2.5.2", tag, "a8061dc1305136c6c22b8baf0c0127a9");

    /* ChaCha20-Poly1305 AEAD: RFC 8439 2.8.2. */
    unhex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f", key);
    unhex("070000004041424344454647", nonce);
    unsigned char aad[12];
    unhex("50515253c0c1c2c3c4c5c6c7", aad);
    memcpy(msg, sunscreen, len);
    aead_seal(0, key, nonce, aad, 12, msg, len, tag);
    check("ChaCha20-Poly1305 encryption, RFC 8439 2.8.2", msg,
          "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc3ff4def08e4b7a9de576d26586cec64b6116");
    check("ChaCha20-Poly1305 tag, RFC 8439 2.8.2", tag, "1ae10b594f09e26a7e902ecbd0600691");
    int opened = aead_open(0, key, nonce, aad, 12, msg, len, tag) == 0 && memcmp(msg, sunscreen, len) == 0;
    printf("%s  ChaCha20-Poly1305 decrypts and checks its tag\n", opened ? "ok  " : "FAIL");
    if (!opened) failures++;
    tag[0] ^= 1;
    int refused = aead_open(0, key, nonce, aad, 12, msg, len, tag) != 0;
    printf("%s  ChaCha20-Poly1305 refuses a tampered message\n", refused ? "ok  " : "FAIL");
    if (!refused) failures++;

    /* AES-128: FIPS 197 appendix C.1. */
    unsigned char block[16];
    unhex("000102030405060708090a0b0c0d0e0f", key);
    unhex("00112233445566778899aabbccddeeff", block);
    aes128_encrypt_block(key, block, out);
    check("AES-128, FIPS 197 C.1", out, "69c4e0d86a7b0430d8cdb78070b4c55a");

    /* AES-128-GCM: the GCM specification's test cases 2 and 4. */
    memset(key, 0, 16);
    memset(nonce, 0, 12);
    memset(msg, 0, 16);
    aead_seal(1, key, nonce, NULL, 0, msg, 16, tag);
    check("AES-128-GCM case 2, ciphertext", msg, "0388dace60b6a392f328c2b971b2fe78");
    check("AES-128-GCM case 2, tag", tag, "ab6e47d42cec13bdf53a67b21257bddf");
    unhex("feffe9928665731c6d6a8f9467308308", key);
    unhex("cafebabefacedbaddecaf888", nonce);
    unsigned char plain4[60], aad4[20];
    unhex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39", plain4);
    unhex("feedfacedeadbeeffeedfacedeadbeefabaddad2", aad4);
    memcpy(msg, plain4, 60);
    aead_seal(1, key, nonce, aad4, 20, msg, 60, tag);
    check("AES-128-GCM case 4, ciphertext", msg,
          "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091");
    check("AES-128-GCM case 4, tag", tag, "5bc94fbc3221a5db94fae95ae7121a47");
    int gcm_open = aead_open(1, key, nonce, aad4, 20, msg, 60, tag) == 0 && memcmp(msg, plain4, 60) == 0;
    printf("%s  AES-128-GCM decrypts and checks its tag\n", gcm_open ? "ok  " : "FAIL");
    if (!gcm_open) failures++;

    /* Random bytes: two draws should not be the same. */
    unsigned char r1[32], r2[32];
    getrandom(r1, 32);
    getrandom(r2, 32);
    int differ = memcmp(r1, r2, 32) != 0;
    printf("%s  the kernel's random numbers vary\n", differ ? "ok  " : "FAIL");
    if (!differ) failures++;

    printf(failures ? "CRYPTO FAILED (%d)\n" : "CRYPTO OK\n", failures);
    return failures != 0;
}
