/* shmdemo - two processes sharing one block of memory, and a pipe.

   Run it with no arguments. It creates a shared memory object, starts a
   second copy of itself as a child process, and the two exchange data
   through the shared block directly - no copying through the kernel. The
   child also reports back down a pipe. */
#include "rdx.h"

typedef struct {
    int numbers[8];
    int sum;       /* filled in by the child */
    char note[64]; /* written by the child */
} Shared;

int main(int argc, char **argv) {
    Shared *shared = shm_open("shmdemo", sizeof(Shared));
    if (!shared) {
        printf("could not map the shared memory\n");
        return 1;
    }

    if (argc > 1 && strcmp(argv[1], "child") == 0) {
        /* The child sees what the parent wrote, at its own address. */
        int sum = 0;
        for (int i = 0; i < 8; i++) sum += shared->numbers[i];
        shared->sum = sum;
        strcpy(shared->note, "written by the child process");
        printf("child %d added the numbers", getpid()); /* goes down the pipe */
        return 0;
    }

    for (int i = 0; i < 8; i++) shared->numbers[i] = (i + 1) * 11;
    int ends[2];
    if (pipe(ends) != 0) {
        printf("could not create a pipe\n");
        return 1;
    }
    int child = spawn("shmdemo child", -1, ends[1]);
    close(ends[1]); /* only the child writes now */
    if (child < 0) {
        printf("could not start the child\n");
        return 1;
    }
    char message[80];
    int n = 0;
    i64 got;
    while (n < 79 && (got = read(ends[0], message + n, (size_t)(79 - n))) > 0) n += (int)got;
    message[n] = 0;
    int code = wait(child);

    printf("parent %d: child said \"%s\" (exit code %d)\n", getpid(), message, code);
    printf("shared sum = %d (expected 396), note = \"%s\"\n", shared->sum, shared->note);
    shm_unlink("shmdemo");
    return shared->sum == 396 ? 0 : 1;
}
