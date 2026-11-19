/* yes - print a line over and over until stopped (try: yes | head -3). */
#include "rdx.h"

int main(int argc, char **argv) {
    for (;;) puts(argc > 1 ? argv[1] : "y");
}
