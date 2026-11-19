/* certcheck - look at certificates, and check them like the browser does.

   certcheck <file> [host]
       Describe the certificates in <file> (PEM). With a host name, check
       the chain they form (the site's own first) against the trusted
       authorities in /roots.pem, as a browser would for that host.
   certcheck --signature <cert file> <scheme> <data file> <signature file>
       Check a signature made with the certificate's key. <scheme> is a TLS
       signature scheme: rsa-sha256, rsa-pss-sha256 (or -sha384, -sha512),
       ecdsa-sha256, ecdsa-sha384.
   certcheck --hash <file>
       Print its SHA-384 and SHA-512. */
#include "x509.h"

static int b64(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* The certificates in a PEM file (or the file itself, if it is DER). */
static int read_certs(const char *path, unsigned char **certs, size_t *lens, int max) {
    size_t len;
    char *text = read_all(path, &len);
    if (!text) return -1;
    int count = 0;
    if (len && (unsigned char)text[0] == 0x30) {
        certs[0] = (unsigned char *)text;
        lens[0] = len;
        return 1;
    }
    for (char *p = text; count < max && (p = strstr(p, "-----BEGIN CERTIFICATE-----")) != NULL;) {
        p += 27;
        char *end = strstr(p, "-----END CERTIFICATE-----");
        if (!end) break;
        unsigned char *der = malloc((size_t)(end - p));
        size_t n = 0;
        unsigned acc = 0;
        int got = 0;
        for (char *q = p; q < end; q++) {
            int v = b64((unsigned char)*q);
            if (v < 0) continue;
            acc = acc << 6 | (unsigned)v;
            if (++got == 4) {
                der[n++] = (unsigned char)(acc >> 16);
                der[n++] = (unsigned char)(acc >> 8);
                der[n++] = (unsigned char)acc;
                got = 0;
                acc = 0;
            }
        }
        if (got == 3) {
            der[n++] = (unsigned char)(acc >> 10);
            der[n++] = (unsigned char)(acc >> 2);
        } else if (got == 2) {
            der[n++] = (unsigned char)(acc >> 4);
        }
        certs[count] = der;
        lens[count++] = n;
        p = end;
    }
    return count;
}

static void print_hex(const unsigned char *p, int n) {
    static const char digits[] = "0123456789abcdef";
    char out[130];
    for (int i = 0; i < n; i++) {
        out[2 * i] = digits[p[i] >> 4];
        out[2 * i + 1] = digits[p[i] & 15];
    }
    out[2 * n] = 0;
    printf("%s\n", out);
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "--hash") == 0) {
        size_t len;
        char *data = read_all(argv[2], &len);
        if (!data) {
            printf("certcheck: no file %s\n", argv[2]);
            return 1;
        }
        unsigned char out[64];
        sha384(data, len, out);
        printf("SHA-384 ");
        print_hex(out, 48);
        sha512(data, len, out);
        printf("SHA-512 ");
        print_hex(out, 64);
        return 0;
    }
    if (argc >= 6 && strcmp(argv[1], "--signature") == 0) {
        static const struct {
            const char *name;
            int scheme;
        } schemes[] = {{"rsa-sha256", 0x0401},     {"rsa-sha384", 0x0501},     {"rsa-sha512", 0x0601},
                       {"rsa-pss-sha256", 0x0804}, {"rsa-pss-sha384", 0x0805}, {"rsa-pss-sha512", 0x0806},
                       {"ecdsa-sha256", 0x0403},   {"ecdsa-sha384", 0x0503},   {"ecdsa-sha512", 0x0603}};
        int scheme = 0;
        for (size_t i = 0; i < sizeof(schemes) / sizeof(schemes[0]); i++) {
            if (strcmp(argv[3], schemes[i].name) == 0) scheme = schemes[i].scheme;
        }
        unsigned char *certs[1];
        size_t lens[1], data_len, sig_len;
        char *data = read_all(argv[4], &data_len), *sig = read_all(argv[5], &sig_len);
        if (!scheme || read_certs(argv[2], certs, lens, 1) < 1 || !data || !sig) {
            printf("certcheck: bad arguments (see the top of certcheck.c)\n");
            return 1;
        }
        u64 t0 = uptime_ms();
        int bad = x509_check_signature(certs[0], lens[0], scheme, (unsigned char *)data, data_len, (unsigned char *)sig, sig_len);
        printf("%s (%s, %llu ms)\n", bad ? "SIGNATURE BAD" : "SIGNATURE OK", argv[3], uptime_ms() - t0);
        return bad ? 1 : 0;
    }
    if (argc < 2) {
        printf("usage: certcheck <file.pem> [host]\n       certcheck --signature <cert> <scheme> <data> <sig>\n");
        return 1;
    }
    static unsigned char *certs[8];
    static size_t lens[8];
    int count = read_certs(argv[1], certs, lens, 8);
    if (count <= 0) {
        printf("certcheck: no certificates in %s\n", argv[1]);
        return 1;
    }
    for (int i = 0; i < count; i++) {
        char text[300];
        x509_describe(certs[i], lens[i], text, sizeof(text));
        printf("%d. %s\n", i + 1, text);
    }
    if (argc < 3) return 0;
    char who[96], error[160];
    u64 t0 = uptime_ms();
    if (x509_check_chain((const unsigned char *const *)certs, lens, count, argv[2], who, sizeof(who), error) == 0) {
        printf("TRUSTED for %s: issued by %s (%llu ms)\n", argv[2], who, uptime_ms() - t0);
        return 0;
    }
    printf("NOT TRUSTED for %s: %s\n", argv[2], error);
    return 1;
}
