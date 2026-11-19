/* pwd - print the working directory. (The shell has this built in; the
   program exists so that it also works in a pipeline.) */
#include "rdx.h"

int main(void) {
    char path[256];
    getcwd(path, sizeof(path));
    printf("%s\n", path);
    return 0;
}
