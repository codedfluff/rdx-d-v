/* web/realm.c - more than one page at a time: the page in the window, the
   pages in its <iframe>s, and workers. Part of the browser.

   Everything the page code keeps in statics (the document, its styles and
   layout, the whole JavaScript engine) is put by the compiler into two
   linker sections of their own: .rlmb (starting zeroed) and .rlmd
   (starting with values). A realm is a saved copy of those two sections;
   switching realms saves the running one's and puts back another's. So
   each frame and worker is a complete, separate page, with its own
   window, document and objects, and nothing it makes can be reached from
   another: they talk only by postMessage, whose data is copied (see
   clone.c), as between real browsing contexts.

   Realms are switched only between tasks (never while a script runs),
   and the networking (fetch.c, cookies.c) is shared by all of them. */

#pragma clang section bss=".rlmb$a" data=".rlmd$a"
char realm_bss_begin[1];
char realm_data_begin[1] = {1};
#pragma clang section bss=".rlmb$z" data=".rlmd$z"
char realm_bss_end[1];
char realm_data_end[1] = {1};
#pragma clang section bss="" data=""

enum { REALM_PAGE, REALM_FRAME, REALM_WORKER };

typedef struct {
    int used, id, parent, kind;
    char *bss, *data;    /* its saved sections (stale while it runs) */
    Node *owner;         /* frames: the <iframe> in the parent's document */
    int width, height;   /* frames: the size of the box it shows in */
    u64 next_due;        /* its next timer (uptime), 0 for none: noted when it stops running */
    int messages;        /* messages waiting for it */
    int started;         /* workers: running their script */
    int doomed;          /* to be taken down (terminate(), close()) */
    char url[1024];
} Realm;

#define MAX_REALMS 32
static Realm realm_list[MAX_REALMS];
static int realm_now;            /* the running one: an index in realm_list */
static int realm_serial;         /* ids, never reused */
static char *realm_fresh_data;   /* .rlmd as the program started */
static size_t realm_bss_size, realm_data_size;
static int frames_changed;       /* a frame's page changed: draw again */
static char *main_stack_top;

static void realm_boot(char *stack_top) {
    realm_bss_size = (size_t)(realm_bss_end - realm_bss_begin);
    realm_data_size = (size_t)(realm_data_end - realm_data_begin);
    realm_fresh_data = malloc(realm_data_size);
    if (realm_fresh_data) memcpy(realm_fresh_data, realm_data_begin, realm_data_size);
    main_stack_top = stack_top;
    Realm *r = &realm_list[0];
    r->used = 1;
    r->id = ++realm_serial;
    r->parent = -1;
    r->kind = REALM_PAGE;
    realm_now = 0;
    if (getenv("BROWSER_TIMING")) printf("[realm] a realm is %d KiB\n", (int)((realm_bss_size + realm_data_size) / 1024));
}

static int realm_index(int id) {
    for (int i = 0; i < MAX_REALMS; i++)
        if (realm_list[i].used && realm_list[i].id == id) return i;
    return -1;
}

/* Make realm i the running one. */
static void realm_switch(int i) {
    if (i == realm_now || i < 0 || !realm_list[i].used) return;
    Realm *from = &realm_list[realm_now], *to = &realm_list[i];
    if (!to->bss || !to->data || (!from->bss && !(from->bss = malloc(realm_bss_size))) ||
        (!from->data && !(from->data = malloc(realm_data_size)))) {
        printf("browser: not enough memory to switch frames\n");
        return;
    }
    int t = js_next_timer();
    from->next_due = t < 0 ? 0 : uptime_ms() + (u64)t;
    memcpy(from->bss, realm_bss_begin, realm_bss_size);
    memcpy(from->data, realm_data_begin, realm_data_size);
    memcpy(realm_bss_begin, to->bss, realm_bss_size);
    memcpy(realm_data_begin, to->data, realm_data_size);
    realm_now = i;
    fetch_realm_id = to->id;
}

/* the settings every realm starts with (main and dump set them for the first) */
static int realm_dumping;
static void realm_settings(void) {
    js_stack_top = main_stack_top;
    js_enabled = !getenv("BROWSER_NOJS");
    if (getenv("BROWSER_CONSOLE")) console_echo = 1;
    if (getenv("BROWSER_TRACE")) js_trace_throws = 1;
}

/* A new realm, as the program started (not running yet). Returns its index or -1. */
static int realm_new(int kind) {
    int i = 1;
    while (i < MAX_REALMS && realm_list[i].used) i++;
    if (i == MAX_REALMS || !realm_fresh_data) return -1;
    Realm *r = &realm_list[i];
    memset(r, 0, sizeof(*r));
    r->bss = malloc(realm_bss_size);
    r->data = malloc(realm_data_size);
    if (!r->bss || !r->data) {
        free(r->bss);
        free(r->data);
        return -1;
    }
    memset(r->bss, 0, realm_bss_size);
    memcpy(r->data, realm_fresh_data, realm_data_size);
    r->used = 1;
    r->id = ++realm_serial;
    r->kind = kind;
    r->parent = realm_list[realm_now].id;
    return i;
}

static void free_page(void);
static void realm_free_engine(void);
static void messages_drop(int id);

/* Take down realm i and everything in it (and its frames and workers). */
static void realm_destroy(int i) {
    if (i <= 0 || i >= MAX_REALMS || !realm_list[i].used) return;
    for (int k = 1; k < MAX_REALMS; k++) /* its children first */
        if (realm_list[k].used && realm_list[k].parent == realm_list[i].id) realm_destroy(k);
    int back = realm_now;
    realm_switch(i);
    free_page();
    realm_free_engine();
    realm_switch(back);
    messages_drop(realm_list[i].id);
    free(realm_list[i].bss);
    free(realm_list[i].data);
    memset(&realm_list[i], 0, sizeof(Realm));
}

/* The frame showing in <iframe> n of the running realm: its index, or -1. */
static int frame_of(Node *n) {
    int me = realm_list[realm_now].id;
    for (int i = 1; i < MAX_REALMS; i++)
        if (realm_list[i].used && realm_list[i].kind == REALM_FRAME && realm_list[i].parent == me && realm_list[i].owner == n) return i;
    return -1;
}

/* ---------------------------------------------------------------- frames */

static void load(const char *address, const char *post);
static void relayout(void);
static int load_images(int (*each)(void));


/* the content box of an <iframe> */
static void frame_box(Node *n, int *x, int *y, int *w, int *h) {
    *x = n->x + n->style.border[3] + pad(n, 3, 0);
    *y = n->y + n->style.border[0] + pad(n, 0, 0);
    *w = n->w - frame_h(n, 0);
    *h = n->h - frame_v(n, 0);
    if (*w < 0) *w = 0;
    if (*h < 0) *h = 0;
}

/* What an <iframe> shows: its src, resolved; "" for none or about:blank
   (shown empty, and in the same realm: scripts get this window). */
static void frame_address(Node *n, char *out, size_t max) {
    char src[1024];
    out[0] = 0;
    if (!attr(n, "src", src, sizeof(src)) || !src[0] || !strncasecmp(src, "about:", 6) || !strncasecmp(src, "javascript:", 11)) return;
    resolve_url(base_url, src, out, max);
}

static void frame_load(int i, const char *url) {
    Realm *r = &realm_list[i];
    int back = realm_now;
    realm_switch(i);
    realm_settings();
    layout_viewport_w = r->width;
    layout_viewport_h = r->height;
    load(url, NULL);
    load_images(NULL);
    page_loaded = 1;
    js_fire(NULL, "load", 0, 0);
    if (dom_dirty) {
        relayout();
        dom_dirty = 0;
    }
    realm_switch(back);
    /* the <iframe>'s own load event, in the parent */
    if (r->owner) js_fire(r->owner, "load", 0, 0);
    frames_changed = 1;
}

/* After the running page was laid out: start frames for new <iframe>s,
   resize those whose box changed, end those whose <iframe> went. */
static void frames_update(void) {
    if (!document) return;
    int me = realm_list[realm_now].id;
    /* gone: no longer in the document (or not shown) */
    for (int i = 1; i < MAX_REALMS; i++) {
        Realm *r = &realm_list[i];
        if (!r->used || r->kind != REALM_FRAME || r->parent != me) continue;
        int found = 0;
        for (Node *n = document; n && !found; n = next_in_tree(n, document)) found = n == r->owner;
        char url[1024];
        if (found) frame_address(r->owner, url, sizeof(url));
        if (!found || strcmp(url, r->url)) realm_destroy(i); /* (hidden frames load and run too, as in browsers) */
    }
    static char url[1024];
    for (Node *n = document; n; n = next_in_tree(n, document)) {
        if (n->type != NODE_ELEMENT || !is(n->tag, "iframe")) continue;
        int x, y, w, h;
        frame_box(n, &x, &y, &w, &h);
        int i = frame_of(n);
        if (i >= 0) {
            if (realm_list[i].width != w || realm_list[i].height != h) { /* resized */
                realm_list[i].width = w;
                realm_list[i].height = h;
                int back = realm_now;
                realm_switch(i);
                layout_viewport_w = w;
                layout_viewport_h = h;
                relayout();
                realm_switch(back);
                frames_changed = 1;
            }
            continue;
        }
        frame_address(n, url, sizeof(url));
        if (!url[0]) continue;
        i = realm_new(REALM_FRAME);
        if (i < 0) continue;
        realm_list[i].owner = n;
        realm_list[i].width = w;
        realm_list[i].height = h;
        strcpy(realm_list[i].url, url);
        if (getenv("BROWSER_TIMING")) printf("[frame] loading %s (%dx%d)\n", url, w, h);
        frame_load(i, url);
    }
}

static void paint_page(canvas *v, int scroll, int left);

/* Draw the frame in <iframe> n (whose content box is at x, y in canvas v). */
static int frame_paint(Node *n, canvas *v, int x, int y) {
    int i = frame_of(n);
    if (i < 0) return 0;
    int fx, fy, w, h;
    frame_box(n, &fx, &fy, &w, &h);
    /* the part of the frame inside v */
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > v->width ? v->width : x + w, y1 = y + h > v->height ? v->height : y + h;
    if (x1 <= x0 || y1 <= y0) return 1;
    canvas part = {v->pixels + (size_t)y0 * (size_t)v->stride + (size_t)x0, x1 - x0, y1 - y0, v->stride};
    int back = realm_now;
    realm_switch(i);
    paint_page(&part, y0 - y, x - x0);
    realm_switch(back);
    return 1;
}

static int click_page(int x, int y);
static void frame_after_js(void);

/* A click at page point (px, py) on <iframe> n: into its frame. */
static int frame_click(Node *n, int px, int py) {
    int i = frame_of(n);
    if (i < 0) return 0;
    int fx, fy, w, h;
    frame_box(n, &fx, &fy, &w, &h);
    if (px < fx || py < fy || px >= fx + w || py >= fy + h) return 1;
    int back = realm_now;
    realm_switch(i);
    click_page(px - fx, py - fy);
    frame_after_js();
    realm_switch(back);
    frames_changed = 1;
    return 1;
}

/* ---------------------------------------------------------------- running them all */

static void deliver_messages(void);
static void worker_start(void);

/* Milliseconds until something in any realm is due (-1: nothing). */
static int realms_next_timer(void) {
    int best = js_next_timer();
    u64 now = uptime_ms();
    for (int i = 0; i < MAX_REALMS; i++) {
        Realm *r = &realm_list[i];
        if (!r->used || i == realm_now) continue;
        int t = r->messages || r->doomed || (r->kind == REALM_WORKER && !r->started) ? 0
              : r->next_due ? (r->next_due <= now ? 0 : (int)(r->next_due - now)) : -1;
        if (t >= 0 && (best < 0 || t < best)) best = t;
    }
    if (realm_list[realm_now].messages) best = 0;
    return best;
}

/* Run what is due in the realms other than the running one (the running
   one's own timers are the caller's). */
static void realms_run_others(void) {
    u64 now = uptime_ms();
    int back = realm_now;
    for (int i = 0; i < MAX_REALMS; i++) {
        Realm *r = &realm_list[i];
        if (!r->used || i == back) continue;
        if (r->doomed && i) {
            realm_destroy(i);
            continue;
        }
        int start = r->kind == REALM_WORKER && !r->started;
        if (!start && !r->messages && !(r->next_due && r->next_due <= now)) continue;
        realm_switch(i);
        if (start) {
            r->started = 1;
            worker_start();
        }
        deliver_messages();
        js_run_timers();
        frame_after_js();
        realm_switch(back);
    }
    if (realm_list[back].messages) deliver_messages();
}

/* Free what a realm's engine keeps between pages (it is going away). */
static void realm_free_engine(void) {
    for (int i = 0; i < intern_cap; i++) free(intern_table ? intern_table[i] : NULL);
    free(intern_table);
    intern_table = NULL;
    intern_cap = intern_count = 0;
    free(items);
    free(pool);
    free(links);
    items = NULL;
    pool = NULL;
    links = NULL;
    free(gc_stack);
    free(gc_index);
    free(console_text.p);
    free(orphans);
}

/* ---------------------------------------------------------------- messages */

/* postMessage between realms: the data already copied into bytes
   (js_frames.c), waiting in order for the realm it is for. */
typedef struct Message {
    struct Message *next;
    int to, kind, from;
    char origin[256];
    char *data;
    int len;
} Message;
static Message *message_head, *message_tail;

static void message_post(int to, int kind, int from, const char *origin, char *data, int len) {
    int r = realm_index(to);
    Message *m = malloc(sizeof(Message));
    if (r < 0 || !m) {
        free(data);
        free(m);
        return;
    }
    memset(m, 0, sizeof(*m));
    m->to = to;
    m->kind = kind;
    m->from = from;
    strncpy(m->origin, origin, sizeof(m->origin) - 1);
    m->data = data;
    m->len = len;
    if (message_tail) message_tail->next = m;
    else message_head = m;
    message_tail = m;
    realm_list[r].messages++;
}

static void message_receive(int kind, int from, const char *origin, const char *data, int len);

/* Hand the running realm its messages (in the order they were sent). */
static void deliver_messages(void) {
    Realm *me = &realm_list[realm_now];
    for (int rounds = 0; me->messages && rounds < 64; rounds++) {
        Message *prev = NULL, *m = message_head;
        while (m && m->to != me->id) {
            prev = m;
            m = m->next;
        }
        if (!m) {
            me->messages = 0;
            break;
        }
        if (prev) prev->next = m->next;
        else message_head = m->next;
        if (message_tail == m) message_tail = prev;
        me->messages--;
        message_receive(m->kind, m->from, m->origin, m->data, m->len);
        free(m->data);
        free(m);
    }
}

/* Messages for a realm that is gone. */
static void messages_drop(int id) {
    Message *prev = NULL, *m = message_head;
    while (m) {
        Message *next = m->next;
        if (m->to == id) {
            if (prev) prev->next = next;
            else message_head = next;
            if (message_tail == m) message_tail = prev;
            free(m->data);
            free(m);
        } else {
            prev = m;
        }
        m = next;
    }
}
