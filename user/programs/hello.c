/* hello.exe - the first program to run on RDX. Try: hello one two three */
#include "rdx.h"

int main(int argc, char **argv) {
    printf("Hello from ring 3! I am process %d, written in C.\n", getpid());
    for (int i = 1; i < argc; i++) {
        printf("  argument %d: %s\n", i, argv[i]);
    }
    printf("The system has been up for %llu ms.\n", uptime_ms());
    return 0;
}
