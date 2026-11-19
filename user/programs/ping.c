/* ping - check that another machine answers.
   Usage: ping <address or name>     e.g. ping 10.0.2.2
   Sends four echo requests a second apart and reports on the answers. */
#include "rdx.h"

int main(int argc, char **argv) {
    unsigned char ip[4];
    if (argc < 2 || net_resolve(argv[1], ip) != 0) {
        printf("usage: ping <address>   (e.g. ping 10.0.2.2)\n");
        return 1;
    }
    char who[20];
    {
        /* Dotted form of the address, for the messages. */
        int n = 0;
        for (int i = 0; i < 4; i++) {
            if (ip[i] >= 100) who[n++] = (char)('0' + ip[i] / 100);
            if (ip[i] >= 10) who[n++] = (char)('0' + ip[i] / 10 % 10);
            who[n++] = (char)('0' + ip[i] % 10);
            who[n++] = i < 3 ? '.' : 0;
        }
    }
    /* Sequence numbers nobody else is likely to be using right now. */
    int base = (int)((uptime_ms() / 7 + (u64)getpid() * 64) & 0x7FFF);
    int received = 0;
    for (int i = 0; i < 4; i++) {
        int sequence = base + i;
        u64 started = uptime_ms();
        int sent;
        /* The first packet to a new neighbour has to wait for its hardware
           address to be looked up: allow that a second. */
        while ((sent = ping_send(ip, sequence)) == -3 && uptime_ms() < started + 1000) sleep_ms(10);
        if (sent == -1) {
            printf("there is no network adapter\n");
            return 1;
        }
        if (sent == -2) {
            printf("no address yet (is a DHCP server reachable? see: net)\n");
            return 1;
        }
        if (sent != 0) {
            printf("no route to %s: nothing answered on the local network\n", who);
            continue;
        }
        /* Wait up to a second for the reply, then pause out the rest of it. */
        i64 answered = -1;
        while (uptime_ms() < started + 1000 && (answered = ping_reply(sequence)) < 0) sleep_ms(10);
        if (answered >= 0) {
            received++;
            printf("reply from %s: time %llu ms\n", who, (u64)answered > started ? (u64)answered - started : 0);
        } else {
            printf("no reply from %s within 1 second\n", who);
        }
        while (uptime_ms() < started + 1000) sleep_ms(10);
    }
    printf("4 sent, %d received\n", received);
    return received ? 0 : 1;
}
