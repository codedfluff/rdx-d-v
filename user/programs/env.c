/* env - print the environment variables this program was started with.
   Usage: env [NAME]   With a name, prints just that variable (exit code 1
   if it is not set). */
#include "rdx.h"

int main(int argc, char **argv) {
    if (argc > 1) {
        const char *value = getenv(argv[1]);
        if (!value) return 1;
        printf("%s\n", value);
        return 0;
    }
    char entry[1200];
    for (int i = 0; env_entry(i, entry, sizeof(entry)) >= 0; i++) printf("%s\n", entry);
    return 0;
}
