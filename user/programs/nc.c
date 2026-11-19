/* nc - connect to a TCP port and talk to it.
   Usage: nc <host> <port>

   What you type (or pipe in) is sent; whatever comes back is printed.
   Ctrl+D, or the end of piped input, tells the other side you have finished
   sending; nc then waits for it to finish too. Ctrl+C quits at once.

   Two threads do the work: one copies input to the connection, the other
   copies the connection to the screen, so neither direction waits on the
   other. */
#include "rdx.h"

static int connection;

static void receive(void *arg) {
    (void)arg;
    char buf[2048];
    i64 n;
    while ((n = read(connection, buf, sizeof(buf))) > 0) fd_write(1, buf, (size_t)n);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        printf("usage: nc <host> <port>\n");
        return 1;
    }
    connection = tcp_connect_name(argv[1], atoi(argv[2]));
    if (connection < 0) {
        printf("nc: could not connect to %s port %s\n", argv[1], argv[2]);
        return 1;
    }
    int receiver = thread_create(receive, NULL);

    char buf[2048];
    i64 n;
    while ((n = read(0, buf, sizeof(buf))) > 0) {
        if (fd_write(connection, buf, (size_t)n) < 0) break;
    }
    tcp_shutdown(connection);
    thread_join(receiver);
    return 0;
}
