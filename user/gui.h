/* gui.h - windows on the RDX desktop.

   The desktop (desktop.exe) is the window server: it owns the screen, the
   keyboard and the mouse. A program that wants a window connects to it,
   asks for one, draws into the window's pixels (shared memory, so no
   copying through the kernel) and tells the desktop which part changed.
   The desktop sends back what happens to the window: keys while it has the
   focus, mouse clicks and drags inside it, and a request to close when its
   close button is clicked.

       if (gui_connect() < 0) { printf("start the desktop first\n"); return 1; }
       gui_window *w = gui_window_new(300, 200, "Hello");
       fill_rect(&w->pixels, 0, 0, 300, 200, 0xFFFFFF);
       draw_text(&w->pixels, 10, 10, "Hello, desktop", 0x000000);
       gui_update(w, 0, 0, 300, 200);
       gui_message m;
       while (gui_next(&m, -1) > 0 && m.type != MSG_CLOSE) { ... }

   Everything here is built on calls any program can make: local_connect,
   shared memory, poll. */
#pragma once
#include "rdx.h"

/* The name the desktop listens on. */
#define GUI_SERVICE "desktop"

/* Every message, each way, is exactly this 64-byte record. */
typedef struct {
    unsigned type;   /* MSG_... */
    unsigned window; /* which window it is about */
    int a, b, c, d;  /* numbers; what they mean depends on the type */
    char text[40];   /* a title or a name, NUL-terminated */
} gui_message;

/* Program -> desktop */
#define MSG_CREATE 1  /* a = width, b = height, text = title */
#define MSG_UPDATE 2  /* a, b, c, d = x, y, width, height that changed */
#define MSG_TITLE 3   /* text = new title */
#define MSG_DESTROY 4 /* the window is no longer wanted */
/* Desktop -> program */
#define MSG_CREATED 16 /* a = width, b = height, text = shared memory name */
#define MSG_KEY 17     /* a = key, exactly as getchar() would give it */
#define MSG_MOUSE 18   /* a, b = x, y inside the window; c = buttons held (MOUSE_...) */
#define MSG_CLOSE 19   /* the close button was clicked */
#define MSG_FOCUS 20   /* a = 1 the window now has the keyboard, 0 it lost it */

typedef struct {
    unsigned id;
    canvas pixels;       /* draw here, then gui_update */
    int width, height;
    char shm_name[40];
} gui_window;

/* Connect to the desktop. Returns the connection's descriptor (for poll),
   or -1 if the desktop is not running. Done once per program. */
int gui_connect(void);
/* The connection's descriptor, or -1. */
int gui_fd(void);

/* Programs that make their own windows say so, first thing in main:
       int main(void) { GUI_PROGRAM(); ... }
   This leaves a marker in the program file. gui_launch looks for it to
   decide how to start a program: one with the marker is started as it is;
   any other is a text program, and gets a terminal window of its own,
   titled "Terminal - name.exe". */
#define GUI_PROGRAM() gui_mark("RDX-GUI-PROGRAM")
void gui_mark(const char *marker);
/* Start a program (command line: name and arguments) the right way for
   it, as above. Returns its process ID (the terminal's, for a text
   program), or -1 if there is no such program. */
int gui_launch(const char *cmdline);
/* 1 if the named program is a graphical one, 0 if it is a text program,
   -1 if there is no such program. Found like the shell finds programs. */
int gui_is_graphical(const char *name);
/* A new window with a content area of this size. NULL if it failed. */
gui_window *gui_window_new(int width, int height, const char *title);
/* Show what has been drawn in this part of the window. */
void gui_update(gui_window *w, int x, int y, int width, int height);
void gui_set_title(gui_window *w, const char *title);
void gui_window_close(gui_window *w);
/* The next message from the desktop. Waits up to timeout_ms (-1: as long
   as it takes). Returns 1 with a message, 0 if the time ran out, -1 if the
   desktop has gone away. */
int gui_next(gui_message *m, int timeout_ms);
/* Like gui_next with timeout 0, but only from what has already arrived:
   use after poll() said the connection is readable. */
int gui_poll(gui_message *m);
