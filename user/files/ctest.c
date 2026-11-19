/* ctest.c - checks that c-comp generates correct code.
   Inside RDX:   c-comp ctest.c    then    ctest                      */
#include "rdx.h"

int passed, failed;

void check(const char *what, int ok) {
    if (ok) {
        passed++;
    } else {
        failed++;
        printf("FAILED: %s\n", what);
    }
}

/* ---- globals with initializers ---- */
int counter = 7;
int squares[5] = {0, 1, 4, 9, 16};
int grid[2][3] = {{1, 2, 3}, {4, 5, 6}};
char greeting[] = "hello";
const char *words[] = {"zero", "one", "two"};
long long big = 0x123456789ALL;
unsigned char bytes[4] = {250, 251, 252, 253};
int zeroed[100];

struct point {
    int x, y;
};

struct shape {
    const char *name;
    struct point corner;
    char tag;
    long long area;
};

struct shape shapes[2] = {{"box", {1, 2}, 'b', 50}, {"dot", {3, 4}, 'd', 1}};
int *counter_ptr = &counter;

typedef struct node {
    int value;
    struct node *next;
} Node;

enum color { RED, GREEN = 5, BLUE };

union both {
    unsigned whole;
    unsigned char part[4];
};

#define LIMIT 10
#define DOUBLE_LIMIT (LIMIT * 2)

/* ---- functions ---- */
int fib(int n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

int sum_to(int n) {
    int total = 0;
    for (int i = 1; i <= n; i++) total += i;
    return total;
}

int many(int a, int b, int c, int d, int e, int f, int g, int h) { return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8; }

int sum_all(int count, ...) {
    long long *arg = (long long *)&count + 1;
    int total = 0;
    for (int i = 0; i < count; i++) total += (int)arg[i];
    return total;
}

int classify(int n) {
    switch (n) {
    case 1:
        return 10;
    case 2:
    case 3:
        return 20;
    case -4:
        return 40;
    default:
        return -1;
    }
}

int next_id(void) {
    static int id = 100;
    return id++;
}

void swap(int *a, int *b) {
    int t = *a;
    *a = *b;
    *b = t;
}

int length(Node *list) {
    int n = 0;
    while (list) {
        n++;
        list = list->next;
    }
    return n;
}

int side_effects;
int bump(int result) {
    side_effects++;
    return result;
}

int main(void) {
    /* arithmetic */
    check("add/mul precedence", 2 + 3 * 4 == 14);
    check("parentheses", (2 + 3) * 4 == 20);
    check("division", 17 / 5 == 3 && 17 % 5 == 2);
    check("negative division", -17 / 5 == -3 && -17 % 5 == -2);
    check("unary minus", -(3 - 10) == 7);
    check("bit ops", (0xF0 | 0x0F) == 0xFF && (0xF0 & 0x3C) == 0x30 && (0xFF ^ 0x0F) == 0xF0);
    check("shifts", 1 << 10 == 1024 && 1024 >> 3 == 128 && -16 >> 2 == -4);
    check("bit not", ~0 == -1 && (~5 & 0xFF) == 250);
    check("comparisons", 3 < 5 && 5 > 3 && 3 <= 3 && 3 >= 3 && 3 != 4 && !(3 == 4));
    check("octal and char literals", 010 == 8 && 'A' == 65 && '\n' == 10 && '\x41' == 65);

    /* integer sizes and wrapping */
    int wrap = 2147483647;
    wrap = wrap + 1;
    check("int wraps at 32 bits", wrap < 0);
    unsigned u = 0;
    u = u - 1;
    check("unsigned wraps", u == 4294967295u && u > 0);
    check("unsigned compare", (unsigned)-1 > 1u);
    check("unsigned division", 4000000000u / 2 == 2000000000u);
    long long wide = 1;
    wide = wide << 40;
    check("64-bit math", wide == 1099511627776LL && wide / 1024 == 1073741824LL);
    check("64-bit multiply", 100000LL * 100000LL == 10000000000LL);
    check("64-bit global", big == 78187493530LL);
    char small = (char)200;
    check("char is signed", small < 0 && small == -56);
    unsigned char ubyte = (unsigned char)200;
    check("unsigned char", ubyte == 200);
    short sh = (short)70000;
    check("short truncates", sh == 4464);
    check("sizeof", sizeof(char) == 1 && sizeof(short) == 2 && sizeof(int) == 4 && sizeof(long long) == 8
                    && sizeof(char *) == 8 && sizeof(squares) == 20 && sizeof(struct point) == 8);
    check("sizeof struct padding", sizeof(struct shape) == 32);

    /* assignment forms */
    int a = 10;
    a += 5;
    a -= 3;
    a *= 4;
    a /= 6;
    a %= 5;
    check("compound assignment", a == 3);
    a <<= 4;
    a |= 3;
    a &= 0x3E;
    a ^= 0xFF;
    a >>= 1;
    check("compound bit assignment", a == 102);
    int b = a++ + 1;
    int c = ++a;
    check("increment", b == 103 && c == 104 && a == 104);
    b = a-- - --a;
    check("decrement", a == 102);
    int x, y;
    x = y = 9;
    check("chained assignment", x == 9 && y == 9);
    check("comma operator", (x = 1, y = 2, x + y) == 3);
    check("ternary", (x < y ? 100 : 200) == 100 && (x > y ? 100 : 200) == 200);

    /* short-circuit evaluation */
    side_effects = 0;
    check("&& stops early", !(bump(0) && bump(1)) && side_effects == 1);
    check("|| stops early", (bump(1) || bump(0)) && side_effects == 2);
    check("logical values", (5 && 3) == 1 && (0 || 0) == 0 && !0 == 1 && !7 == 0);

    /* control flow */
    check("recursion", fib(15) == 610);
    check("for loop", sum_to(100) == 5050);
    int evens = 0, loops = 0;
    for (int i = 0; i < 100; i++) {
        if (i % 2) continue;
        if (i >= 20) break;
        evens += i;
        loops++;
    }
    check("break and continue", evens == 90 && loops == 10);
    int n = 0;
    do {
        n += 3;
    } while (n < 10);
    check("do-while", n == 12);
    int w = 0;
    while (1) {
        if (++w == 50) break;
    }
    check("while with break", w == 50);
    check("switch", classify(1) == 10 && classify(2) == 20 && classify(3) == 20 && classify(-4) == 40
                    && classify(99) == -1);
    int fall = 0;
    switch (2) {
    case 1:
        fall += 1;
    case 2:
        fall += 10;
    case 3:
        fall += 100;
        break;
    case 4:
        fall += 1000;
    }
    check("switch fallthrough", fall == 110);
    int nested = 0;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            if (j == 2) break;
            nested += i * j;
        }
    }
    check("nested loops", nested == 6);

    /* functions */
    check("eight arguments", many(1, 2, 3, 4, 5, 6, 7, 8) == 204);
    check("variadic function", sum_all(4, 10, 20, 30, 40) == 100 && sum_all(0) == 0);
    check("static local", next_id() == 100 && next_id() == 101 && next_id() == 102);
    int p = 1, q = 2;
    swap(&p, &q);
    check("pointer parameters", p == 2 && q == 1);

    /* pointers and arrays */
    int arr[5] = {5, 4, 3};
    check("local array init", arr[0] == 5 && arr[2] == 3 && arr[3] == 0 && arr[4] == 0);
    int *ptr = arr;
    ptr++;
    check("pointer arithmetic", *ptr == 4 && *(ptr + 1) == 3 && ptr[-1] == 5 && ptr - arr == 1);
    check("pointer comparison", ptr > arr && &arr[4] - &arr[1] == 3);
    *ptr = 44;
    ptr[1] += 6;
    check("write through pointer", arr[1] == 44 && arr[2] == 9);
    char text[] = "abc";
    char buffer[16];
    strcpy(buffer, text);
    buffer[0] = 'X';
    check("local strings", sizeof(text) == 4 && strcmp(buffer, "Xbc") == 0 && strlen(buffer) == 3);
    check("string indexing", "hello"[1] == 'e' && greeting[4] == 'o' && sizeof(greeting) == 6);
    int matrix[3][4];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) matrix[i][j] = i * 10 + j;
    }
    check("2D arrays", matrix[2][3] == 23 && matrix[1][0] == 10 && sizeof(matrix) == 48);
    char **wp = (char **)words;
    check("pointer to pointer", wp[2][0] == 't' && **wp == 'z');

    /* globals */
    check("global scalars", counter == 7 && *counter_ptr == 7);
    *counter_ptr = 8;
    check("global through pointer", counter == 8);
    check("global arrays", squares[4] == 16 && grid[1][2] == 6 && grid[0][0] == 1);
    check("global strings", strcmp(words[1], "one") == 0 && strcmp(greeting, "hello") == 0);
    check("global bytes", bytes[0] == 250 && bytes[3] == 253);
    int all_zero = 1;
    for (int i = 0; i < 100; i++) {
        if (zeroed[i]) all_zero = 0;
    }
    zeroed[99] = 5;
    check("zeroed globals", all_zero && zeroed[99] == 5);
    check("constants", LIMIT == 10 && DOUBLE_LIMIT == 20 && RED == 0 && GREEN == 5 && BLUE == 6);

    /* structs */
    struct point pt;
    pt.x = 3;
    pt.y = 4;
    struct point copy;
    copy = pt;
    copy.x = 30;
    check("struct members and copy", pt.x == 3 && copy.x == 30 && copy.y == 4);
    struct point *pp = &pt;
    pp->y = 40;
    check("struct pointer", pt.y == 40 && (*pp).x == 3);
    check("global structs", shapes[1].corner.y == 4 && shapes[0].tag == 'b' && shapes[0].area == 50
                            && strcmp(shapes[1].name, "dot") == 0);
    shapes[0].corner = copy;
    check("nested struct assignment", shapes[0].corner.x == 30);
    union both pun;
    pun.whole = 0x11223344;
    check("union", pun.part[0] == 0x44 && pun.part[3] == 0x11);

    /* heap */
    Node *list = NULL;
    for (int i = 0; i < 10; i++) {
        Node *fresh = malloc(sizeof(Node));
        fresh->value = i * i;
        fresh->next = list;
        list = fresh;
    }
    check("linked list on the heap", length(list) == 10 && list->value == 81 && list->next->next->value == 49);
    char *block = malloc(1000);
    memset(block, 7, 1000);
    block = realloc(block, 100000);
    check("malloc and realloc", block[0] == 7 && block[999] == 7);
    free(block);

    printf("printf: [%d] [%5d] [%05d] [%x] [%s] [%c] [%lld] [%u]\n", -42, 42, 42, 255, "str", 'Z', big, 3000000000u);

    if (failed) {
        printf("%d passed, %d FAILED\n", passed, failed);
        return 1;
    }
    printf("ALL %d CHECKS PASSED\n", passed);
    return 0;
}
