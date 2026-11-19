/* tty - say whether this program is connected to a terminal (the console,
   or a terminal window) or to a pipe or file. Exit code 0 if a terminal. */
#include "rdx.h"

int main(void) {
    int terminal = isatty(0);
    printf(terminal ? "a terminal\n" : "not a terminal\n");
    return !terminal;
}
