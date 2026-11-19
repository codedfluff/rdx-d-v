/* udpsend - send one UDP packet and print the reply.
   Usage: udpsend <host> <port> <text>...

   UDP has no connections and no retries: the packet either arrives or is
   lost, and so does the answer. Ctrl+C if nothing ever comes back. */
#include "rdx.h"

int main(int argc, char **argv) {
    if (argc < 4) {
        printf("usage: udpsend <host> <port> <text>...\n");
        return 1;
    }
    unsigned char ip[4];
    if (net_resolve(argv[1], ip) != 0) {
        printf("udpsend: cannot find the address of %s\n", argv[1]);
        return 1;
    }
    char message[1024];
    size_t len = 0;
    for (int i = 3; i < argc; i++) {
        if (i > 3 && len < sizeof(message) - 1) message[len++] = ' ';
        for (const char *p = argv[i]; *p && len < sizeof(message) - 1; p++) message[len++] = *p;
    }

    int fd = udp_open(0);
    if (fd < 0) {
        printf("udpsend: could not open a UDP port\n");
        return 1;
    }
    if (udp_sendto(fd, ip, atoi(argv[2]), message, len) < 0) {
        printf("udpsend: could not send (is the network up?)\n");
        return 1;
    }
    char answer[1500];
    unsigned char from[4];
    int from_port;
    i64 n = udp_recvfrom(fd, answer, sizeof(answer) - 1, from, &from_port);
    if (n < 0) {
        printf("udpsend: receive failed\n");
        return 1;
    }
    answer[n] = 0;
    printf("%d.%d.%d.%d:%d replied (%d bytes): %s\n", from[0], from[1], from[2], from[3], from_port, (int)n, answer);
    return 0;
}
