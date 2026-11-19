/* nslookup - look up the address of a host name.
   Usage: nslookup <name>...          e.g. nslookup example.com */
#include "rdx.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        unsigned char c[16];
        if (net_info(c) != 0) {
            printf("the network is not configured (see: net)\n");
            return 1;
        }
        printf("usage: nslookup <name>...   (asking the DNS server at %d.%d.%d.%d)\n", c[12], c[13], c[14], c[15]);
        return 1;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        unsigned char ip[4];
        if (net_resolve(argv[i], ip) == 0) {
            printf("%s has address %d.%d.%d.%d\n", argv[i], ip[0], ip[1], ip[2], ip[3]);
        } else {
            printf("%s: no address found\n", argv[i]);
            status = 1;
        }
    }
    return status;
}
