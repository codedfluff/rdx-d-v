/* sh - the RDX shell: the program that reads what you type and runs it.

   It is an ordinary program. The kernel starts one on the console at boot
   (and again if it exits); you can also run `sh` yourself for a nested
   shell, or `sh file` to run a file of commands.

   What it does itself (the "built-ins"): cd, pwd, set, unset, source, help,
   exit. Everything else is a program it finds through PATH and starts.
   It understands:
       a | b | c           pipes
       < in  > out  >> out redirection
       program &           running in the background
       $NAME ${NAME} $?    variables, and the last exit code
       # ...               comments
   and the line editor has history (Up/Down), Tab completion, and cursor
   movement (Left/Right, Home/End, Delete, Ctrl+U).

   It never calls malloc: all its memory is laid out up front, so a shell
   that has been running for a week is the same size as a new one. */
#include "rdx.h"

#define LINE_MAX 250      /* the kernel passes command lines of up to 255 bytes */
#define HISTORY_MAX 100
#define MAX_STAGES 8
#define MAX_CANDIDATES 200
#define NAME_MAX 64

#define GREEN 0x5AF78E
#define BLUE 0x57C7FF
#define DIM 0x6C7380
#define RED 0xFF5C57
#define YELLOW 0xF3F99D

static const char *BUILTINS[] = {"cd", "pwd", "set", "unset", "source", "help", "exit", NULL};

static char history[HISTORY_MAX][LINE_MAX + 1];
static int history_count;
static int last_status;
static int interactive;

static void execute(const char *line);

/* ---------------------------------------------------------------- small helpers */

/* Write straight to the screen, after anything printf still has buffered. */
static void put(const char *text, size_t n) {
    flush();
    write(text, n);
}


static void error(const char *fmt, const char *a, const char *b) {
    screen_color(RED);
    printf(fmt, a, b);
    printf("\n");
    screen_color(COLOR_DEFAULT);
}

static int starts_with(const char *text, const char *prefix) {
    while (*prefix) {
        if (*text++ != *prefix++) return 0;
    }
    return 1;
}

static int ends_with(const char *text, const char *suffix) {
    size_t n = strlen(text), m = strlen(suffix);
    return n >= m && strcmp(text + n - m, suffix) == 0;
}

/* Copy at most max-1 characters and always terminate. */
static void copy(char *dst, const char *src, size_t max) {
    size_t i = 0;
    for (; src[i] && i + 1 < max; i++) dst[i] = src[i];
    dst[i] = 0;
}

static void append(char *dst, const char *src, size_t max) {
    size_t n = strlen(dst);
    if (n < max) copy(dst + n, src, max - n);
}

/* Remove leading and trailing spaces in place; returns the new start. */
static char *trim(char *text) {
    while (*text == ' ' || *text == '\t') text++;
    size_t n = strlen(text);
    while (n > 0 && (text[n - 1] == ' ' || text[n - 1] == '\t' || text[n - 1] == '\r')) text[--n] = 0;
    return text;
}

/* ---------------------------------------------------------------- the prompt and line editor */

static void prompt(void) {
    char path[256];
    getcwd(path, sizeof(path));
    screen_color(GREEN);
    printf("rdx");
    if (strcmp(path, "/") != 0) {
        screen_color(BLUE);
        printf("%s", path);
    }
    screen_color(DIM);
    printf("> ");
    screen_color(COLOR_DEFAULT);
}

/* The line being typed and where in it the cursor is. */
static char line[LINE_MAX + 1];
static int length, cursor;

/* Bring the screen in line after the text changed: step back to the start
   of the input, print the new text, blank out whatever the old one had
   beyond it, and step back to where the cursor now is. */
static void repaint(int old_length, int old_cursor) {
    screen_left(old_cursor);
    put(line, (size_t)length);
    int extra = old_length > length ? old_length - length : 0;
    for (int i = 0; i < extra; i++) put(" ", 1);
    screen_left(length + extra - cursor);
}

static void replace_line(const char *text) {
    int old_length = length, old_cursor = cursor;
    copy(line, text, sizeof(line));
    length = cursor = (int)strlen(line);
    repaint(old_length, old_cursor);
}

/* --- Tab completion --- */

static char candidates[MAX_CANDIDATES][NAME_MAX + 2];
static int candidate_count;

static void add_candidate(const char *name, int is_dir) {
    if (candidate_count >= MAX_CANDIDATES || strlen(name) > NAME_MAX) return;
    for (int i = 0; i < candidate_count; i++) {
        if (strcmp(candidates[i], name) == 0) return;
    }
    copy(candidates[candidate_count], name, NAME_MAX + 1);
    if (is_dir) append(candidates[candidate_count], "/", NAME_MAX + 2);
    candidate_count++;
}

/* Offer the entries of `dir` whose names start with `partial`. With
   `programs`, only .exe files, offered without the ending. */
static void candidates_from(const char *dir, const char *partial, int programs) {
    char here[256], name[NAME_MAX];
    getcwd(here, sizeof(here));
    if (chdir(dir) != 0) return;
    i64 size;
    for (int i = 0; (size = file_list(i, name, sizeof(name))) != -1; i++) {
        if (programs) {
            if (size == -2 || !ends_with(name, ".exe")) continue;
            name[strlen(name) - 4] = 0;
        }
        if (starts_with(name, partial)) add_candidate(name, size == -2);
    }
    chdir(here);
}

/* Complete the word at the end of the line: a command name in the first
   position, a file or directory name anywhere else. */
static void complete(void) {
    int word_start = length;
    while (word_start > 0 && line[word_start - 1] != ' ') word_start--;
    char *word = line + word_start;
    char *slash = NULL;
    for (char *p = word; *p; p++) {
        if (*p == '/') slash = p;
    }
    const char *partial = slash ? slash + 1 : word;
    candidate_count = 0;

    if (word_start == 0 && !slash) {
        for (int i = 0; BUILTINS[i]; i++) {
            if (starts_with(BUILTINS[i], partial)) add_candidate(BUILTINS[i], 0);
        }
        /* Programs: the .exe files in each folder of PATH. */
        char path[512];
        const char *value = getenv("PATH");
        copy(path, value ? value : "/", sizeof(path));
        for (char *dir = path; dir && *dir;) {
            char *colon = dir;
            while (*colon && *colon != ':') colon++;
            char *next = *colon ? colon + 1 : NULL;
            *colon = 0;
            candidates_from(dir, partial, 1);
            dir = next;
        }
    } else {
        char dir[LINE_MAX + 1] = ".";
        if (slash) {
            size_t n = (size_t)(slash - word);
            if (n == 0) {
                copy(dir, "/", sizeof(dir));
            } else {
                memcpy(dir, word, n);
                dir[n] = 0;
            }
        }
        candidates_from(dir, partial, 0);
    }
    if (candidate_count == 0) return;

    /* Sort them (there are few) and find what they all start with. */
    for (int i = 1; i < candidate_count; i++) {
        for (int j = i; j > 0 && strcmp(candidates[j - 1], candidates[j]) > 0; j--) {
            char swap[NAME_MAX + 2];
            strcpy(swap, candidates[j]);
            strcpy(candidates[j], candidates[j - 1]);
            strcpy(candidates[j - 1], swap);
        }
    }
    size_t common = strlen(candidates[0]);
    for (int i = 1; i < candidate_count; i++) {
        size_t k = 0;
        while (k < common && candidates[i][k] == candidates[0][k]) k++;
        common = k;
    }

    size_t have = strlen(partial);
    if (common > have) {
        char add[NAME_MAX + 4];
        memcpy(add, candidates[0] + have, common - have);
        add[common - have] = 0;
        /* A completed file name gets a space after it; a directory keeps
           its slash so the next Tab looks inside. */
        if (candidate_count == 1 && !ends_with(candidates[0], "/")) append(add, " ", sizeof(add));
        if (length + (int)strlen(add) <= LINE_MAX) {
            append(line, add, sizeof(line));
            put(add, strlen(add));
            length = cursor = (int)strlen(line);
        }
    } else if (candidate_count > 1) {
        /* Nothing more in common: show the choices and redraw the line. */
        printf("\n");
        screen_color(DIM);
        for (int i = 0; i < candidate_count; i++) printf("%s  ", candidates[i]);
        screen_color(COLOR_DEFAULT);
        printf("\n");
        prompt();
        put(line, (size_t)length);
    }
}

/* Read one command line into `line`. Returns 0 at end of input. */
static int read_line(void) {
    prompt();
    line[0] = 0;
    length = cursor = 0;
    int browse = history_count; /* history_count means "the line being typed" */
    char stash[LINE_MAX + 1] = "";
    for (;;) {
        int c = getchar();
        int old_length = length, old_cursor = cursor;
        if (c == EINTR) continue;
        if (c < 0 || (c == 4 && length == 0)) return 0; /* end of input, or Ctrl+D */
        if (c == '\n') {
            put(line + cursor, (size_t)(length - cursor)); /* leave the cursor after the text */
            put("\n", 1);
            return 1;
        } else if (c == 8) {
            if (cursor > 0) {
                memmove(line + cursor - 1, line + cursor, (size_t)(length - cursor + 1));
                cursor--;
                length--;
                repaint(old_length, old_cursor);
            }
        } else if (c == KEY_DELETE) {
            if (cursor < length) {
                memmove(line + cursor, line + cursor + 1, (size_t)(length - cursor));
                length--;
                repaint(old_length, old_cursor);
            }
        } else if (c == KEY_LEFT) {
            if (cursor > 0) {
                cursor--;
                screen_left(1);
            }
        } else if (c == KEY_RIGHT) {
            if (cursor < length) put(line + cursor++, 1);
        } else if (c == KEY_HOME || c == CTRL('A')) {
            cursor = 0;
            repaint(old_length, old_cursor);
        } else if (c == KEY_END || c == CTRL('E')) {
            cursor = length;
            repaint(old_length, old_cursor);
        } else if (c == CTRL('U')) {
            replace_line("");
        } else if (c == '\t') {
            if (cursor == length) { /* completion works on the last word */
                browse = history_count;
                complete();
            }
        } else if (c == CTRL('C')) {
            put(line + cursor, (size_t)(length - cursor));
            put("^C\n", 3);
            line[0] = 0;
            return 1;
        } else if (c == KEY_UP) {
            if (browse > 0) {
                if (browse == history_count) strcpy(stash, line);
                replace_line(history[--browse]);
            }
        } else if (c == KEY_DOWN) {
            if (browse < history_count) {
                browse++;
                replace_line(browse == history_count ? stash : history[browse]);
            }
        } else if (c >= 32 && c < 127 && length < LINE_MAX) {
            memmove(line + cursor + 1, line + cursor, (size_t)(length - cursor + 1));
            line[cursor++] = (char)c;
            length++;
            if (cursor == length) put(line + cursor - 1, 1); /* typing at the end: nothing to repaint */
            else repaint(old_length, old_cursor);
        }
    }
}

static void remember(const char *text) {
    if (!*text || (history_count > 0 && strcmp(history[history_count - 1], text) == 0)) return;
    if (history_count == HISTORY_MAX) {
        memmove(history[0], history[1], sizeof(history[0]) * (HISTORY_MAX - 1));
        history_count--;
    }
    copy(history[history_count++], text, LINE_MAX + 1);
}

/* ---------------------------------------------------------------- variables */

/* Replace $NAME, ${NAME} and $? with their values. An unset variable
   becomes nothing; a $ followed by anything else stays as it is. */
static void expand(const char *in, char *out, size_t max) {
    size_t n = 0;
    out[0] = 0;
    while (*in && n + 1 < max) {
        if (*in != '$') {
            out[n++] = *in++;
            out[n] = 0;
            continue;
        }
        char name[NAME_MAX] = "";
        const char *value = NULL;
        char number[24];
        if (in[1] == '?') {
            /* Build the digits by hand: this must not disturb printf's buffer. */
            int v = last_status, digits = 0;
            char reversed[16];
            unsigned magnitude = v < 0 ? 0u - (unsigned)v : (unsigned)v;
            do reversed[digits++] = (char)('0' + magnitude % 10);
            while (magnitude /= 10);
            int k = 0;
            if (v < 0) number[k++] = '-';
            while (digits) number[k++] = reversed[--digits];
            number[k] = 0;
            value = number;
            in += 2;
        } else if (in[1] == '{') {
            const char *close = in + 2;
            while (*close && *close != '}') close++;
            if (!*close || close - (in + 2) >= NAME_MAX) {
                out[n++] = *in++;
                out[n] = 0;
                continue;
            }
            memcpy(name, in + 2, (size_t)(close - (in + 2)));
            name[close - (in + 2)] = 0;
            in = close + 1;
            value = getenv(name);
        } else {
            int len = 0;
            const char *p = in + 1;
            while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_') {
                if (len < NAME_MAX - 1) name[len++] = *p;
                p++;
            }
            name[len] = 0;
            if (len == 0) {
                out[n++] = *in++;
                out[n] = 0;
                continue;
            }
            in = p;
            value = getenv(name);
        }
        if (value) {
            append(out, value, max);
            n = strlen(out);
        }
    }
}

/* ---------------------------------------------------------------- running programs */

typedef struct {
    char cmdline[LINE_MAX + 1]; /* the program and its arguments */
    char label[NAME_MAX + 8];   /* its file name, for messages */
    char input[LINE_MAX + 1];   /* "< file", or empty */
    char output[LINE_MAX + 1];  /* "> file", or empty */
    int append;                 /* >> rather than > */
} Stage;

static Stage stages[MAX_STAGES];

/* Split one pipeline stage into its command and its redirections.
   Accepts both `> file` and `>file`. Returns 0 (after complaining) if it
   makes no sense. */
static int parse_stage(char *text, Stage *stage) {
    stage->cmdline[0] = stage->input[0] = stage->output[0] = stage->label[0] = 0;
    stage->append = 0;
    char *p = text;
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char *token = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = 0;

        char *target = NULL;
        const char *kind = NULL;
        if (starts_with(token, ">>")) {
            kind = ">>";
            target = stage->output;
            stage->append = 1;
        } else if (token[0] == '>') {
            kind = ">";
            target = stage->output;
            stage->append = 0;
        } else if (token[0] == '<') {
            kind = "<";
            target = stage->input;
        }
        if (!target) {
            if (!stage->cmdline[0]) {
                copy(stage->label, token, NAME_MAX);
                if (!ends_with(stage->label, ".exe")) append(stage->label, ".exe", sizeof(stage->label));
            } else {
                append(stage->cmdline, " ", sizeof(stage->cmdline));
            }
            append(stage->cmdline, token, sizeof(stage->cmdline));
            continue;
        }
        const char *name = token + strlen(kind);
        if (!*name) { /* the file name is the next word */
            while (*p == ' ' || *p == '\t') p++;
            name = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = 0;
        }
        if (!*name) {
            error("expected a file name after %s", kind, "");
            return 0;
        }
        copy(target, name, LINE_MAX + 1);
    }
    if (!stage->cmdline[0]) {
        error("there is an empty command in the pipeline", "", "");
        return 0;
    }
    return 1;
}

static void close_if_open(int fd) {
    if (fd >= 0) close(fd);
}

/* Run `a | b | c`, each with optional redirection. In the foreground the
   shell waits for them all and Ctrl+C goes to them; in the background it
   reports their process IDs and carries on. */
static void run_pipeline(char *text, int background) {
    int count = 0;
    for (char *part = text; part;) {
        char *bar = part;
        while (*bar && *bar != '|') bar++;
        char *next = *bar ? bar + 1 : NULL;
        *bar = 0;
        if (count == MAX_STAGES) {
            error("too many programs in one pipeline", "", "");
            return;
        }
        if (!parse_stage(part, &stages[count++])) return;
        part = next;
    }

    /* Give each program its standard input and output: a file if it asked
       for one, else the pipe to its neighbour, else the shell's own. */
    int pids[MAX_STAGES];
    int started = 0;
    int from_previous = -1; /* read end of the pipe from the stage before */
    for (int i = 0; i < count; i++) {
        Stage *stage = &stages[i];
        int in = from_previous, out = -1, next_read = -1, failed = 0;
        from_previous = -1;
        if (stage->input[0]) {
            close_if_open(in);
            in = open(stage->input, OPEN_READ);
            if (in < 0) {
                error("%s: no such file", stage->input, "");
                failed = 1;
            }
        }
        if (!failed && stage->output[0]) {
            out = open(stage->output, stage->append ? OPEN_APPEND : OPEN_WRITE);
            if (out < 0) {
                error("%s: %s", stage->output, last_error());
                failed = 1;
            }
        } else if (!failed && i < count - 1) {
            int ends[2];
            if (pipe(ends) != 0) {
                error("cannot make a pipe", "", "");
                failed = 1;
            } else {
                next_read = ends[0];
                out = ends[1];
            }
        }
        if (!failed) {
            int pid = spawn(stage->cmdline, in, out);
            if (pid == SPAWN_NO_PROGRAM) {
                /* Just the name, as typed. */
                char name[NAME_MAX];
                copy(name, stage->cmdline, sizeof(name));
                for (char *p = name; *p; p++) {
                    if (*p == ' ') *p = 0;
                }
                error("unknown command or program: %s", name, "");
                failed = 1;
            } else if (pid < 0) {
                error("cannot run %s: %s", stage->label, last_error());
                failed = 1;
            } else {
                pids[started++] = pid;
            }
        }
        /* The shell keeps no copies of the files and pipe ends it handed
           over, so each pipe closes as soon as its programs finish. */
        close_if_open(in);
        close_if_open(out);
        if (failed) {
            /* Don't leave half a pipeline running. */
            close_if_open(next_read);
            for (int k = 0; k < started; k++) kill(pids[k], SIGKILL);
            for (int k = 0; k < started; k++) wait(pids[k]);
            last_status = 127;
            return;
        }
        from_previous = next_read;
    }

    if (background) {
        screen_color(DIM);
        printf("[running in the background as process ");
        for (int k = 0; k < started; k++) printf(k ? ", %d" : "%d", pids[k]);
        printf("]\n");
        screen_color(COLOR_DEFAULT);
        return;
    }

    /* In a terminal window, show what is running in its title bar. */
    int titled = getenv("TERM_WINDOW") != NULL;
    if (titled) {
        printf("\x1b]0;%s\x07", stages[0].label);
        flush();
    }
    set_foreground(pids, started);
    int status = 0;
    for (int k = 0; k < started; k++) status = wait(pids[k]);
    set_foreground(NULL, 0);
    if (titled) {
        printf("\x1b]0;\x07");
        flush();
    }
    /* Undo anything a full-screen program left behind. */
    screen_mirror(1);
    screen_color(COLOR_DEFAULT);

    last_status = status;
    if (status != 0) {
        screen_color(DIM);
        if (status == -1) printf("[%s was terminated]\n", stages[count - 1].label);
        else printf("[%s exited with code %d]\n", stages[count - 1].label, status);
        screen_color(COLOR_DEFAULT);
    }
}

/* ---------------------------------------------------------------- built-in commands */

static void help(void) {
    static const char *lines[][2] = {
        {"cd [dir]", "change directory (cd on its own goes to /)"},
        {"pwd", "print the working directory"},
        {"set NAME=value", "set a variable; use it as $NAME ($? = last exit code)"},
        {"unset NAME", "remove a variable"},
        {"source <file>", "run a file of commands (startup.rc runs at boot)"},
        {"exit", "leave this shell"},
        {"<program> [args]", "run a program (Ctrl+C stops it)"},
        {"<program> &", "run it in the background (see ps; stop it with kill)"},
        {"a | b, < in, > out", "connect programs with pipes; read or write files"},
        {"", ""},
        {"desktop", "the graphical desktop: windows, terminal, files, monitor"},
        {"paint, gfxtest", "full-screen graphics (outside the desktop)"},
        {"ls, tree, find, du", "look at files and folders"},
        {"cat, more, head, tail", "read files"},
        {"write <file> <text>", "create a file; edit <file> opens the editor"},
        {"cp, mv, rm, mkdir, rmdir", "copy, rename, delete, make and remove folders"},
        {"grep, sort, uniq, wc, rev", "work on text, usually in a pipeline"},
        {"ps, top, kill", "see and stop what is running"},
        {"mem, free, df, disk", "memory and disk space"},
        {"cpu, usb, net, about", "what the machine has (all of /proc: ls /proc)"},
        {"date, uptime, uname", "time, and which system this is"},
        {"ping, wget, nc, httpd", "the network"},
        {"rx [script]", "run an RX script, or start the RX prompt"},
        {"c-comp <file.c>", "compile a C program into a .exe"},
        {"crash, panic, spawn", "tests of the kernel itself"},
        {"reboot, shutdown", "restart or power off"},
    };
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        screen_color(YELLOW);
        printf("  %s", lines[i][0]);
        for (size_t pad = strlen(lines[i][0]); pad < 26; pad++) putchar(' ');
        screen_color(COLOR_DEFAULT);
        printf("%s\n", lines[i][1]);
    }
}

/* Run each line of a file as a command. Returns 0 if there is no such file. */
static int run_script(const char *path) {
    static char text[16384];
    static int depth;
    i64 size = file_read(path, text, sizeof(text) - 1);
    if (size < 0) return 0;
    if (size > (i64)sizeof(text) - 1) size = sizeof(text) - 1;
    text[size] = 0;
    if (depth > 0) {
        /* The buffer is shared, and the script that is running is in it. */
        error("%s: a script cannot run another script with source", path, "");
        return 1;
    }
    depth++;
    for (char *p = text; *p;) {
        char *end = p;
        while (*end && *end != '\n') end++;
        char *next = *end ? end + 1 : end;
        *end = 0;
        execute(p);
        p = next;
    }
    depth--;
    return 1;
}

/* Try `line` as a built-in. Returns 1 if it was one. */
static int builtin(const char *command, char *rest) {
    if (strcmp(command, "cd") == 0) {
        if (chdir(*rest ? rest : "/") != 0) error("no such directory", "", "");
    } else if (strcmp(command, "pwd") == 0) {
        char path[256];
        getcwd(path, sizeof(path));
        printf("%s\n", path);
    } else if (strcmp(command, "set") == 0 || strcmp(command, "export") == 0) {
        char *equals = rest;
        while (*equals && *equals != '=') equals++;
        if (!*rest) {
            char entry[1200];
            for (int i = 0; env_entry(i, entry, sizeof(entry)) >= 0; i++) printf("%s\n", entry);
        } else if (*equals && equals != rest) {
            *equals = 0;
            char *name = trim(rest), *value = trim(equals + 1);
            int spaced = 0;
            for (char *p = name; *p; p++) spaced |= *p == ' ';
            if (!*name || spaced) error("usage: set NAME=value   (set on its own lists the variables)", "", "");
            else setenv(name, value);
        } else {
            error("usage: set NAME=value   (set on its own lists the variables)", "", "");
        }
    } else if (strcmp(command, "unset") == 0) {
        if (*rest) unsetenv(rest);
        else error("usage: unset NAME", "", "");
    } else if (strcmp(command, "source") == 0 || strcmp(command, ".") == 0) {
        if (!*rest) error("usage: source <file of commands>", "", "");
        else if (!run_script(rest)) error("no such file", "", "");
    } else if (strcmp(command, "help") == 0) {
        help();
    } else if (strcmp(command, "exit") == 0) {
        exit(0);
    } else {
        return 0;
    }
    return 1;
}

static void execute(const char *typed) {
    char expanded[LINE_MAX + 1];
    while (*typed == ' ' || *typed == '\t') typed++;
    if (!*typed || *typed == '#') return; /* nothing, or a comment */
    expand(typed, expanded, sizeof(expanded));
    char *text = trim(expanded);
    if (!*text) return;

    /* `program &` starts it in the background: the shell does not wait. */
    size_t n = strlen(text);
    int background = text[n - 1] == '&';
    if (background) {
        text[n - 1] = 0;
        text = trim(text);
    }

    /* Built-ins are recognised only on their own: with a pipe or a
       redirection the line is for programs. `run x` is just `x`. */
    int plumbing = 0;
    for (char *p = text; *p; p++) plumbing |= *p == '|' || *p == '<' || *p == '>';
    if (starts_with(text, "run ")) text = trim(text + 4);
    if (!plumbing && !background) {
        char command[NAME_MAX];
        size_t len = 0;
        while (text[len] && text[len] != ' ' && len < sizeof(command) - 1) {
            command[len] = text[len];
            len++;
        }
        command[len] = 0;
        char rest[LINE_MAX + 1];
        copy(rest, text + len, sizeof(rest));
        if (builtin(command, trim(rest))) return;
    }
    run_pipeline(text, background);
}

int main(int argc, char **argv) {
    /* `sh file`: run that file of commands and stop. */
    if (argc > 1) {
        if (!run_script(argv[1])) {
            error("sh: %s: no such file", argv[1], "");
            return 1;
        }
        return last_status;
    }

    interactive = isatty(0);
    if (!interactive) {
        /* Commands arriving through a pipe or from a file: no prompt, no
           editing, just run each line. */
        int n = 0, c;
        while ((c = getchar()) >= 0) {
            if (c != '\n' && n < LINE_MAX) {
                line[n++] = (char)c;
                continue;
            }
            line[n] = 0;
            n = 0;
            execute(line);
        }
        line[n] = 0;
        execute(line);
        return last_status;
    }

    run_script("/startup.rc");
    printf("Type 'help' for a list of commands, 'ls' to see programs.\n\n");
    while (read_line()) {
        char *typed = trim(line);
        remember(typed);
        /* Work on a copy: running a command reuses the line buffer. */
        char command[LINE_MAX + 1];
        copy(command, typed, sizeof(command));
        execute(command);
    }
    return 0;
}
