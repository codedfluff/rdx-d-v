/* colors - show the console palette. */
#include "rdx.h"

int main(void) {
    static const struct {
        const char *name;
        unsigned rgb;
    } palette[] = {{"red", 0xFF5C57},     {"green", 0x5AF78E}, {"yellow", 0xF3F99D}, {"blue", 0x57C7FF},
                   {"magenta", 0xFF6AC1}, {"cyan", 0x9AEDFE},  {"dim", 0x6C7380}};
    for (size_t i = 0; i < sizeof(palette) / sizeof(palette[0]); i++) {
        screen_color(palette[i].rgb);
        printf("%s ", palette[i].name);
    }
    screen_color(COLOR_DEFAULT);
    printf("\n");
    return 0;
}
