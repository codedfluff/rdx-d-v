/* date - the date and time, from the hardware clock (taken to be UTC).
   Usage: date [-s]   -s prints seconds since 1970 instead. */
#include "rdx.h"

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "-s") == 0) {
        printf("%llu\n", time_now());
        return 0;
    }
    char text[20];
    format_time(time_now(), text);
    printf("%s\n", text);
    return 0;
}
