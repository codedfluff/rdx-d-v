/* guess.exe - number guessing game. Keeps your best score in a file. */
#include "rdx.h"

int main(void) {
    int secret = (int)(uptime_ms() * 7919 % 100) + 1;
    char line[32];

    char saved[16] = {0};
    int best = file_read("guess.best", saved, sizeof(saved) - 1) > 0 ? atoi(saved) : 0;

    printf("I'm thinking of a number between 1 and 100.\n");
    if (best) printf("Best so far: %d guesses.\n", best);

    for (int tries = 1;; tries++) {
        printf("Guess %d: ", tries);
        if (readline(line, sizeof(line)) == 0) {
            tries--;
            continue;
        }
        int guess = atoi(line);
        if (guess < secret) {
            printf("Too low.\n");
        } else if (guess > secret) {
            printf("Too high.\n");
        } else {
            printf("Correct! You got it in %d %s.\n", tries, tries == 1 ? "guess" : "guesses");
            if (!best || tries < best) {
                char out[16];
                int n = 0, t = tries;
                char rev[16];
                do {
                    rev[n++] = (char)('0' + t % 10);
                    t /= 10;
                } while (t);
                for (int i = 0; i < n; i++) out[i] = rev[n - 1 - i];
                file_write("guess.best", out, (size_t)n);
                printf("New best score!\n");
            }
            return 0;
        }
    }
}
