/* browser - a web browser.

   Pages come over HTTP and HTTPS (tls.c; compressed replies are unpacked
   by inflate.c) or from files. They are parsed into a document tree
   (web/dom.c), styled by their CSS (web/css.c, web/cascade.c), laid out
   (web/layout.c: block flow, floats, inline boxes, flexbox, grid, tables,
   positioning) and drawn (web/paint.c), with PNG, JPEG and GIF pictures
   (image.c) and working forms.

   In the window: click the address bar (or Ctrl+L), type, Enter. "<" goes
   back, "R" reloads. Scroll with the arrows, Page Up/Down, Space,
   Home/End or the scroll bar. Click links, buttons, checkboxes; click a
   text field and type; Enter in a field submits its form; Tab moves to the
   next field.

   Usage: browser [address]          open a window (default: home.html)
          browser --dump <address>   print the page as text (for scripts) */
#include "gui.h"
#include "image.h"
#include "inflate.h"
#include "tls.h"

#include "web/dom.c"
#include "web/css.c"
#include "web/cascade.c"
#include "web/layout.c"
#include "web/paint.c"
#include "web/fetch.c"
#include "web/js_core.c"
#include "web/js_parse.c"
#include "web/js_eval.c"
#include "web/js_lib.c"
#include "web/js_dom.c"

#define W 820
#define H 580
#define BAR 34
#define STATUS 20
#define SCROLLBAR 12
#define PAGE_W (W - SCROLLBAR)
#define HOME "/home.html"
#define MAX_IMAGES 60
#define MAX_IMAGE_BYTES (3 * 1024 * 1024)

#define C_TEXT 0x1C2030
#define C_DIM 0x6A7287

static char page_title[128];
static char status_text[200];
static char page_security[256];
static char *css_buffers[64];
static int css_buffer_count;
static int dumping;

/* ================================================================ loading a page */

static void free_page(void) {
    js_page_end(); /* scripts' objects first: they point at nodes */
    if (document) free_tree(document);
    document = html_element = body_element = head_element = NULL;
    focused_node = NULL;
    for (int i = 0; i < css_buffer_count; i++) free(css_buffers[i]);
    css_buffer_count = 0;
    rule_count = rule_order = 0;
    indexed_count = -1;
    page_title[0] = 0;
}

static void keep_css(char *text) {
    if (css_buffer_count < 64) css_buffers[css_buffer_count++] = text;
    else free(text);
}

/* The built-in stylesheet, then <style> blocks and linked stylesheets in
   document order. */
static void load_stylesheets(void) {
    char *defaults = copy_of(DEFAULT_CSS, (int)sizeof(DEFAULT_CSS) - 1);
    if (defaults) {
        parse_css(defaults, (int)sizeof(DEFAULT_CSS) - 1, 0);
        keep_css(defaults);
    }
    if (!js_enabled) { /* without scripts, <noscript> content shows */
        static const char show[] = "noscript{display:block}";
        char *css = copy_of(show, (int)sizeof(show) - 1);
        if (css) {
            parse_css(css, (int)sizeof(show) - 1, 0);
            keep_css(css);
        }
    }
    int fetched = 0;
    char value[1024], media[128];
    for (Node *n = document; n; n = next_in_tree(n, document)) {
        if (n->type != NODE_ELEMENT) continue;
        media[0] = 0;
        attr(n, "media", media, sizeof(media));
        if (is(n->tag, "style") && n->first && n->first->type == NODE_TEXT && media_applies(media, (int)strlen(media))) {
            char *css = copy_of(n->first->text, n->first->text_len);
            if (css) {
                parse_css(css, n->first->text_len, 1);
                keep_css(css);
            }
        } else if (is(n->tag, "link") && attr(n, "rel", value, sizeof(value)) && strstr(value, "stylesheet") &&
                   !strstr(value, "alternate") && media_applies(media, (int)strlen(media)) && fetched < 12 &&
                   attr(n, "href", value, sizeof(value)) && value[0]) {
            char url[1024], error[400];
            resolve_url(base_url, value, url, sizeof(url));
            Fetched f;
            fetched++;
            if (fetch_url(url, NULL, &f, error) == 0) {
                if (f.len > 1024 * 1024) f.len = 1024 * 1024;
                parse_css(f.body, (int)f.len, 1);
                keep_css(f.body);
            }
        } else if (is(n->tag, "base") && attr(n, "href", value, sizeof(value)) && value[0]) {
            char full[1024];
            resolve_url(base_url, value, full, sizeof(full));
            strcpy(base_url, full);
        }
    }
}

/* Form controls start out as their attributes say. only_new: just those
   not set up yet (made by scripts). */
static void init_controls_mode(int only_new) {
    static char value[1024];
    for (Node *n = document; n; n = next_in_tree(n, document)) {
        if (n->type != NODE_ELEMENT) continue;
        if (only_new && (n->value || is(n->tag, "select"))) continue;
        if (is(n->tag, "input")) {
            if (!attr(n, "value", value, sizeof(value))) value[0] = 0;
            free(n->value);
            n->value = copy_of(value, (int)strlen(value));
            n->checked = has_attr(n, "checked");
        } else if (is(n->tag, "textarea")) {
            int len = text_content(n, value, sizeof(value));
            free(n->value);
            n->value = copy_of(value, len);
        } else if (is(n->tag, "select")) {
            int index = 0;
            n->selected_index = 0;
            for (Node *o = n; o; o = next_in_tree(o, n)) {
                if (o->type == NODE_ELEMENT && is(o->tag, "option")) {
                    if (has_attr(o, "selected")) n->selected_index = index;
                    index++;
                }
            }
        }
    }
}

static void init_controls(void) { init_controls_mode(0); }

static u64 cascade_ms, layout_ms;

static void relayout(void) {
    u64 t0 = uptime_ms();
    compute_styles();
    u64 t1 = uptime_ms();
    layout_page(PAGE_W);
    cascade_ms = t1 - t0;
    layout_ms = uptime_ms() - t1;
}

static void error_page(const char *message, const char *url) {
    static char page[4096];
    strcpy(page, "<html><head><title>Can't open this page</title></head><body style='margin:24px'>"
                 "<h1>Can't open this page</h1><p>");
    strncat_safe(page, message, 600);
    strncat_safe(page, "</p><p>Address: <code>", 40);
    strncat_safe(page, url, 900);
    strncat_safe(page, "</code></p>", 30);
    if (strstr(message, "untrusted certificate")) {
        strncat_safe(page,
                     "<p style='background:#fdecea;padding:8px'>This site could not prove it is the site you asked for. "
                     "Someone between you and it could be reading or changing what you send and see. "
                     "Do not type passwords or other private things into it.</p><p><a href='rdx-proceed:",
                     400);
        strncat_safe(page, url, 900);
        strncat_safe(page, "'>Go on anyway (not safe)</a></p>", 40);
    }
    strncat_safe(page, "</body></html>", 20);
    parse_html(page, (int)strlen(page));
}

static void number_text(char *out, unsigned long long v) {
    char digits[24];
    int k = 0, m = 0;
    do digits[k++] = (char)('0' + v % 10);
    while (v /= 10);
    while (k) out[m++] = digits[--k];
    out[m] = 0;
}

static unsigned page_epoch; /* changes with every page loaded */

/* Load `address` (GET, or POST with `post`) and lay it out. */
static void load(const char *address, const char *post) {
    page_epoch++;
    static char url[1024];
    strcpy(url, address);
    static char error[400];
    error[0] = 0;
    free_page();
    Fetched f;
    u64 t0 = uptime_ms();
    int failed = fetch_url(url, post, &f, error) != 0;
    u64 t1 = uptime_ms();
    strcpy(current_url, url);
    strcpy(base_url, url);
    if (failed) {
        error_page(error, url);
        strcpy(status_text, "Failed");
        page_security[0] = 0;
    } else {
        strcpy(page_security, f.security);
        if (!strncasecmp(f.type, "text/html", 9) || !strncasecmp(f.type, "application/xhtml", 17)) {
            parse_html(f.body, (int)f.len);
        } else if (!strncasecmp(f.type, "image/", 6) || image_sniff((unsigned char *)f.body, f.len)) {
            static char page[1400];
            strcpy(page, "<html><body style='margin:0;background:#2b2f3a;text-align:center'><img style='max-width:100%' src='");
            strncat_safe(page, url, 1000);
            strncat_safe(page, "'></body></html>", 20);
            parse_html(page, (int)strlen(page));
        } else if (!strncasecmp(f.type, "text/", 5) || !strncasecmp(f.type, "application/json", 16) ||
                   !strncasecmp(f.type, "application/javascript", 22)) {
            /* Plain text: in a <pre>. */
            static const char shell[] = "<html><body><pre></pre></body></html>";
            parse_html(shell, (int)sizeof(shell) - 1);
            Node *pre = find_element(document, "pre");
            Node *t = make_text(f.body, (int)f.len, 0);
            if (pre && t) append_child(pre, t);
        } else {
            char message[200] = "This is not a page but a file of type ";
            strncat_safe(message, f.type, 80);
            strncat_safe(message, ". Use wget to save it.", 30);
            error_page(message, url);
        }
        free(f.body);
        char kib[24];
        number_text(kib, (f.len + 1023) / 1024);
        strcpy(status_text, "Done: ");
        strncat_safe(status_text, kib, 16);
        strncat_safe(status_text, " KiB, ", 8);
        strncat_safe(status_text, page_security, 180);
    }
    Node *title = find_element(document, "title");
    if (title) {
        char text[256];
        text_content(title, text, sizeof(text));
        int k = 0, space = 0;
        for (const char *p = text; *p && k < (int)sizeof(page_title) - 1; p++) {
            if (is_space(*p)) {
                space = k > 0;
                continue;
            }
            if (space) page_title[k++] = ' ';
            space = 0;
            page_title[k++] = *p;
        }
        page_title[k] = 0;
    }
    load_stylesheets();
    init_controls();
    u64 t2 = uptime_ms();
    if (js_enabled && document) {
        js_page_start();
        dom_dirty = 1; /* the first question about layout lays the page out */
        js_run_page_scripts();
        js_fire(document, "DOMContentLoaded", 1, 0);
        js_fire(document, "readystatechange", 0, 0);
        init_controls_mode(1);
        dom_dirty = 0;
        if (js_errors) {
            char n[16];
            number_text(n, (unsigned long long)js_errors);
            strncat_safe(status_text, " - ", 4);
            strncat_safe(status_text, n, 10);
            strncat_safe(status_text, js_errors == 1 ? " script error" : " script errors", 16);
        }
    }
    collect_links();
    u64 t3 = uptime_ms();
    relayout();
    if (getenv("BROWSER_TIMING")) printf("[timing] scripts %llu ms\n", t3 - t2);
    if (getenv("BROWSER_TIMING"))
        printf("[timing] fetch %llu ms, stylesheets %llu ms, cascade %llu ms, layout %llu ms, %d rules, %d nodes, %d items\n",
               t1 - t0, t2 - t1, cascade_ms, layout_ms, rule_count, node_count, item_count);
    if (getenv("BROWSER_TIMING"))
        printf("[timing] %ld compound tests, %ld keyless tries, %ld rules applied\n", stat_compound, stat_keyless, stat_applied);
}

/* Fetch and decode the page's pictures. `each` is called after every one
   (to redraw); it returns nonzero to stop early. */
static int load_images(int (*each)(void)) {
    int loaded = 0;
    unsigned page = page_epoch, dom = dom_epoch;
    static char src[1024], url[1024], error[400];
    for (Node *n = document; n && loaded < MAX_IMAGES; n = next_in_tree(n, document)) {
        if (n->type != NODE_ELEMENT || n->img_state || !is(n->tag, "img")) continue;
        if (n->style.display == D_NONE) continue;
        if ((!attr(n, "src", src, sizeof(src)) || !src[0] || !strncasecmp(src, "data:", 5)) &&
            (!attr(n, "data-src", src, sizeof(src)) || !src[0])) {
            n->img_state = 2;
            continue;
        }
        resolve_url(base_url, src, url, sizeof(url));
        Fetched f;
        n->img_state = 2;
        if (fetch_url(url, NULL, &f, error) == 0) {
            image *img = malloc(sizeof(image));
            char why[96];
            if (img && f.len <= MAX_IMAGE_BYTES && image_decode((unsigned char *)f.body, f.len, img, why) == 0) {
                n->img = img;
                n->img_w = img->width;
                n->img_h = img->height;
                n->img_state = 1;
            } else {
                if (getenv("BROWSER_TIMING")) printf("[image] %s: %s\n", url, img ? why : "no memory");
                free(img);
            }
            free(f.body);
        } else if (getenv("BROWSER_TIMING")) {
            printf("[image] %s: %s\n", url, error);
        }
        loaded++;
        if (each && each()) return 1;
        /* `each` handles the user's input, which may load another page or
           let a script remove nodes (perhaps n): never walk on from a freed one. */
        if (page != page_epoch) return 1;
        if (dom != dom_epoch) {
            dom = dom_epoch;
            n = document; /* start again: pictures already tried are skipped */
        }
    }
    return 0;
}

/* ================================================================ forms */

static Node *form_of(Node *n) {
    for (Node *a = n; a && a != document; a = a->parent) {
        if (a->type == NODE_ELEMENT && is(a->tag, "form")) return a;
    }
    return NULL;
}

static void url_encode(char *out, size_t max, const char *text) {
    size_t at = strlen(out);
    static const char *hexdigits = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)text; *p && at + 4 < max; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
            *p == '.' || *p == '~') {
            out[at++] = (char)*p;
        } else if (*p == ' ') {
            out[at++] = '+';
        } else {
            out[at++] = '%';
            out[at++] = hexdigits[*p >> 4];
            out[at++] = hexdigits[*p & 15];
        }
    }
    out[at] = 0;
}

static void add_field(char *out, size_t max, const char *name, const char *value) {
    if (out[0]) strncat_safe(out, "&", 2);
    url_encode(out, max, name);
    strncat_safe(out, "=", 2);
    url_encode(out, max, value);
}

static void go(const char *url, int remember, const char *post);

/* Submit the form `form`, pressed by `submitter` (or NULL). */
static void submit(Node *form, Node *submitter) {
    if (js_fire(form, "submit", 1, 1)) return; /* a script handled it */
    static char data[8192];
    data[0] = 0;
    static char name[128], value[1024];
    char type[16];
    for (Node *n = form; n; n = next_in_tree(n, form)) {
        if (n->type != NODE_ELEMENT || !attr(n, "name", name, sizeof(name)) || !name[0] || has_attr(n, "disabled")) continue;
        if (is(n->tag, "input")) {
            strcpy(type, "text");
            attr(n, "type", type, sizeof(type));
            if (!strcasecmp_word(type, "checkbox") || !strcasecmp_word(type, "radio")) {
                if (!n->checked) continue;
                add_field(data, sizeof(data), name, attr(n, "value", value, sizeof(value)) ? value : "on");
            } else if (!strcasecmp_word(type, "submit") || !strcasecmp_word(type, "button") || !strcasecmp_word(type, "image") ||
                       !strcasecmp_word(type, "reset")) {
                if (n == submitter && attr(n, "value", value, sizeof(value))) add_field(data, sizeof(data), name, value);
            } else if (strcasecmp_word(type, "file")) {
                add_field(data, sizeof(data), name, n->value ? n->value : "");
            }
        } else if (is(n->tag, "textarea")) {
            add_field(data, sizeof(data), name, n->value ? n->value : "");
        } else if (is(n->tag, "select")) {
            int index = 0;
            for (Node *o = n; o; o = next_in_tree(o, n)) {
                if (o->type == NODE_ELEMENT && is(o->tag, "option")) {
                    if (index == n->selected_index) {
                        if (!attr(o, "value", value, sizeof(value))) text_content(o, value, sizeof(value));
                        add_field(data, sizeof(data), name, value);
                        break;
                    }
                    index++;
                }
            }
        } else if (is(n->tag, "button") && n == submitter && attr(n, "value", value, sizeof(value))) {
            add_field(data, sizeof(data), name, value);
        }
    }
    static char action[1024];
    char method[16] = "get";
    action[0] = 0;
    attr(form, "action", action, sizeof(action));
    attr(form, "method", method, sizeof(method));
    if (submitter) {
        attr(submitter, "formaction", action, sizeof(action));
        attr(submitter, "formmethod", method, sizeof(method));
    }
    static char target[2048];
    resolve_url(base_url, action[0] ? action : current_url, target, sizeof(target));
    if (!strcasecmp_word(method, "post")) {
        go(target, 1, data);
    } else {
        char *q = strchr(target, '?');
        if (q) *q = 0;
        strncat_safe(target, "?", 2);
        strncat_safe(target, data, sizeof(target) - strlen(target) - 1);
        go(target, 1, NULL);
    }
}

static int navigate_requested;

/* Activate a control (clicked, or Enter / Space while it has focus).
   Returns 1 if anything changed. */
static int activate(Node *n) {
    char type[16] = "text";
    attr(n, "type", type, sizeof(type));
    if (has_attr(n, "disabled")) return 0;
    if (is(n->tag, "input") && !strcasecmp_word(type, "checkbox")) {
        n->checked = !n->checked;
        js_fire(n, "input", 1, 0);
        js_fire(n, "change", 1, 0);
        return 1;
    }
    if (is(n->tag, "input") && !strcasecmp_word(type, "radio")) {
        char name[128], other[128];
        name[0] = 0;
        attr(n, "name", name, sizeof(name));
        Node *scope = form_of(n) ? form_of(n) : document;
        for (Node *o = scope; o; o = next_in_tree(o, scope)) {
            if (o->type == NODE_ELEMENT && is(o->tag, "input") && attr(o, "name", other, sizeof(other)) && !strcmp(name, other))
                o->checked = 0;
        }
        n->checked = 1;
        js_fire(n, "input", 1, 0);
        js_fire(n, "change", 1, 0);
        return 1;
    }
    if (is(n->tag, "select")) {
        int count = 0;
        for (Node *o = n; o; o = next_in_tree(o, n)) count += o->type == NODE_ELEMENT && is(o->tag, "option");
        if (count) n->selected_index = (n->selected_index + 1) % count;
        js_fire(n, "input", 1, 0);
        js_fire(n, "change", 1, 0);
        return 1;
    }
    int submit_button = (is(n->tag, "input") && (!strcasecmp_word(type, "submit") || !strcasecmp_word(type, "image"))) ||
                        (is(n->tag, "button") && (!attr(n, "type", type, sizeof(type)) || !strcasecmp_word(type, "submit")));
    if (submit_button && form_of(n)) {
        submit(form_of(n), n);
        return 1;
    }
    if (is(n->tag, "input") && !strcasecmp_word(type, "reset") && form_of(n)) {
        init_controls();
        return 1;
    }
    return 0;
}

/* ================================================================ --dump: the page as text */

typedef struct {
    int y, x, index;
} Placed;

static int dump(const char *address) {
    dumping = 1;
    if (getenv("BROWSER_CONSOLE")) console_echo = 1;
    if (getenv("BROWSER_TRACE")) js_trace_throws = 1;
    load(address, NULL);
    /* Let the page's timers run for a moment, and follow script redirects. */
    for (int hops = 0; hops < 4; hops++) {
        u64 start = uptime_ms();
        for (int i = 0; i < 400 && !js_navigate; i++) {
            int t = js_next_timer();
            if (t < 0 || uptime_ms() - start + (u64)t > 4000) break;
            if (t > 0) sleep_ms((u64)t);
            js_run_timers();
            js_maybe_gc();
        }
        if (!js_navigate) break;
        static char next[2048];
        strcpy(next, strncmp(js_navigate, "rdx-back:", 9) ? js_navigate : current_url);
        free(js_navigate);
        js_navigate = NULL;
        load(next, NULL);
    }
    if (getenv("BROWSER_IMAGES")) load_images(NULL);
    page_loaded = 1;
    js_fire(NULL, "load", 0, 0);
    if (dom_dirty || getenv("BROWSER_IMAGES")) {
        init_controls_mode(1);
        collect_links();
        relayout();
        dom_dirty = 0;
    }
    if (page_title[0]) printf("Title: %s\n", page_title);
    printf("Connection: %s\n\n", page_security[0] ? page_security : "none");
    /* Text items, in reading order: by line (rows of 12 pixels), then left
       to right. */
    Placed *order = malloc(sizeof(Placed) * (size_t)(item_count + 1));
    if (!order) return 1;
    int n = 0;
    for (int i = 0; i < item_count; i++) {
        Item *it = &items[i];
        if (it->clipped && (it->clip[2] - it->clip[0] < 4 || it->clip[3] - it->clip[1] < 4 || it->x >= it->clip[2] ||
                            it->x + it->w <= it->clip[0] || it->y >= it->clip[3] || it->y + it->h <= it->clip[1]))
            continue;
        if (it->kind == IT_TEXT || it->kind == IT_MARKER || (it->kind == IT_IMAGE && it->w > 0) || it->kind == IT_CONTROL)
            order[n++] = (Placed){it->y + it->h / 2, it->x, i};
    }
    for (int i = 1; i < n; i++) { /* insertion sort by height: nearly sorted already */
        Placed p = order[i];
        int j = i;
        while (j > 0 && order[j - 1].y > p.y) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = p;
    }
    /* Rows: items whose middles are within 8 pixels of the row's first
       (text of different sizes on one line). Then left to right in each. */
    int row_id = 0, first_y = n ? order[0].y : 0;
    for (int i = 0; i < n; i++) {
        if (order[i].y - first_y > 8) {
            first_y = order[i].y;
            row_id++;
        }
        order[i].y = row_id * 12;
    }
    for (int i = 1; i < n; i++) {
        Placed p = order[i];
        int j = i;
        while (j > 0 && order[j - 1].y == p.y && order[j - 1].x > p.x) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = p;
    }
    static char line[600];
    int used = 0, row = -1000, prev_end = -1, prev_bottom = 0;
    for (int k = 0; k <= n; k++) {
        if (k == n || order[k].y / 12 != row) {
            if (used) {
                while (used > 0 && line[used - 1] == ' ') used--;
                line[used] = 0;
                printf("%s\n", line);
            }
            if (k == n) break;
            if (row != -1000 && items[order[k].index].y - prev_bottom > 12) printf("\n");
            row = order[k].y / 12;
            used = 0;
            prev_end = -1;
        }
        Item *it = &items[order[k].index];
        char label[160];
        const char *text;
        int len;
        if (it->kind == IT_IMAGE) {
            char alt[120] = "";
            attr(it->node, "alt", alt, sizeof(alt));
            strcpy(label, "[image");
            if (alt[0]) {
                strncat_safe(label, ": ", 3);
                strncat_safe(label, alt, 100);
            }
            strncat_safe(label, "]", 2);
            text = label;
            len = (int)strlen(label);
        } else if (it->kind == IT_CONTROL && is(it->node->tag, "svg")) {
            continue; /* icons */
        } else if (it->kind == IT_CONTROL) {
            char inner[160];
            control_label(it->node, inner, sizeof(inner));
            if (!inner[0] && is_text_control(it->node)) {
                strcpy(label, "[");
                strncat_safe(label, it->node->value && it->node->value[0] ? it->node->value : "________", 60);
                strncat_safe(label, "]", 2);
            } else if (inner[0]) {
                strcpy(label, "[");
                strncat_safe(label, inner, 150);
                strncat_safe(label, "]", 2);
            } else {
                char type[16] = "";
                attr(it->node, "type", type, sizeof(type));
                strcpy(label, !strcasecmp_word(type, "checkbox") ? (it->node->checked ? "[x]" : "[ ]")
                              : !strcasecmp_word(type, "radio") ? (it->node->checked ? "(*)" : "( )")
                                                                : "[ ]");
            }
            text = label;
            len = (int)strlen(label);
        } else {
            text = pool + it->text;
            len = it->len;
        }
        int col = it->x / 8;
        if (prev_end >= 0) {
            /* Words a space apart (in any font size) are one space apart in
               the text; only wide gaps (table columns) keep their position. */
            int gap = it->x - prev_end, cell = it->cell ? it->cell : 8;
            int space = gap > 0 ? 1 : 0;
            if (gap <= 2 * cell || col < used + space) col = used + space;
        }
        if (col > 500) col = 500;
        while (used < col && used < 590) line[used++] = ' ';
        for (int q = 0; q < len && used < 590; q++) line[used++] = text[q];
        prev_end = it->x + (it->kind == IT_TEXT ? it->len * it->cell : it->w);
        if (it->y + it->h > prev_bottom || k == 0) prev_bottom = it->y + it->h;
        /* A link's last word gets [number] after it. */
        int link = it->kind == IT_TEXT || it->kind == IT_IMAGE ? it->link : -1;
        int next_link = -1;
        if (k + 1 < n && order[k + 1].y / 12 == row) {
            Item *next = &items[order[k + 1].index];
            next_link = next->kind == IT_TEXT || next->kind == IT_IMAGE ? next->link : -1;
        }
        if (link >= 0 && next_link != link) {
            char mark[16] = "[";
            number_text(mark + 1, (unsigned long long)link + 1);
            strncat_safe(mark, "]", 2);
            for (int q = 0; mark[q] && used < 590; q++) line[used++] = mark[q];
        }
    }
    free(order);
    if (link_count) {
        printf("\nLinks:\n");
        for (int i = 0; i < link_count; i++) {
            static char full[1024];
            resolve_url(base_url, links[i], full, sizeof(full));
            printf("%4d. %s\n", i + 1, full);
        }
    }
    return strcmp(status_text, "Failed") == 0;
}

/* ================================================================ the window */

static gui_window *win;
static int scroll;
static char address[1024];
static int editing, selected_all;
static char *history[32];
static int history_count;

static int view_height(void) { return H - BAR - STATUS; }

static int max_scroll(void) {
    int m = page_height - view_height();
    return m > 0 ? m : 0;
}

static void start_editing(void) {
    editing = 1;
    selected_all = 1;
    strcpy(address, current_url);
}

static void draw(void) {
    canvas *c = &win->pixels;
    fill_rect(c, 0, 0, W, BAR, 0x2B3245);
    fill_rect(c, 6, 6, 26, BAR - 12, history_count ? 0x4A5878 : 0x353C52);
    draw_char(c, 15, 9, '<', history_count ? 0xFFFFFF : 0x7A8299);
    fill_rect(c, 36, 6, 26, BAR - 12, 0x4A5878);
    draw_char(c, 45, 9, 'R', 0xFFFFFF);
    fill_rect(c, 70, 6, W - 80, BAR - 12, editing ? 0xFFFFFF : 0xE8EBF2);
    draw_rect(c, 70, 6, W - 80, BAR - 12, editing ? 0x3D7DFF : 0xB8BFCF);
    const char *shown = editing ? address : current_url;
    int fit = (W - 96) / CHAR_WIDTH;
    int n = (int)strlen(shown);
    if (editing && selected_all && n) {
        fill_rect(c, 77, 8, (n > fit ? fit : n) * CHAR_WIDTH + 2, BAR - 16, 0x3D7DFF);
        draw_glyphs(c, 78, 9, shown + (n > fit ? n - fit : 0), n > fit ? fit : n, 8, 0xFFFFFF, 0);
    } else {
        draw_glyphs(c, 78, 9, shown + (n > fit ? n - fit : 0), n > fit ? fit : n, 8, C_TEXT, 0);
    }
    if (editing && !selected_all) fill_rect(c, 78 + (n > fit ? fit : n) * CHAR_WIDTH, 9, 2, CHAR_HEIGHT, 0x3D7DFF);

    canvas view = {c->pixels + (size_t)BAR * (size_t)c->stride, PAGE_W, view_height(), c->stride};
    paint_page(&view, scroll, 0);
    canvas bar = {c->pixels + (size_t)BAR * (size_t)c->stride + PAGE_W, SCROLLBAR, view_height(), c->stride};
    int track = view_height();
    fill_rect(&bar, 0, 0, SCROLLBAR, track, 0xEEF0F4);
    if (page_height > track) {
        int thumb = (int)((long long)track * track / page_height);
        if (thumb < 20) thumb = 20;
        int top = (int)((long long)(track - thumb) * scroll / (max_scroll() ? max_scroll() : 1));
        fill_rect(&bar, 2, top, SCROLLBAR - 4, thumb, 0xA9B1C4);
    }
    fill_rect(c, 0, H - STATUS, W, STATUS, 0xE3E6EE);
    int sl = (int)strlen(status_text);
    draw_glyphs(c, 8, H - STATUS + 2, status_text, sl > (W - 16) / 8 ? (W - 16) / 8 : sl, 8, C_DIM, 0);
    gui_update(win, 0, 0, W, H);
}

static void set_window_title(void) {
    char title[40] = "Browser";
    if (page_title[0]) {
        strcpy(title, "Browser - ");
        size_t n = strlen(title);
        for (int i = 0; page_title[i] && n < sizeof(title) - 1; i++) title[n++] = page_title[i];
        title[n] = 0;
    }
    gui_set_title(win, title);
}

static void handle(gui_message *m);
static void after_js(void);
static void back(void);

/* Between pictures: redraw, and deal with anything the user did. */
static int image_progress(void) {
    relayout();
    if (scroll > max_scroll()) scroll = max_scroll();
    draw();
    gui_message m;
    unsigned page = page_epoch;
    while (gui_poll(&m) > 0) {
        handle(&m);
        if (navigate_requested || page != page_epoch) return 1; /* another page is showing now */
    }
    return 0;
}

static void go(const char *url, int remember, const char *post) {
    navigate_requested = 1;
    if (remember && current_url[0] && history_count < 32) {
        history[history_count] = copy_of(current_url, (int)strlen(current_url));
        if (history[history_count]) history_count++;
    }
    strcpy(status_text, "Loading ");
    strncat_safe(status_text, url, sizeof(status_text) - 10);
    editing = 0;
    if (!dumping) draw();
    static char target[2048];
    strcpy(target, url);
    static char body[8192];
    if (post) strcpy(body, post);
    load(target, post ? body : NULL);
    scroll = 0;
    navigate_requested = 0;
    if (dumping) return;
    set_window_title();
    draw();
    char saved_status[200];
    strcpy(saved_status, status_text);
    if (!load_images(image_progress)) {
        strcpy(status_text, saved_status);
        page_loaded = 1;
        js_fire(NULL, "load", 0, 0);
        relayout();
        draw();
        after_js();
    }
}

/* After scripts ran (an event, a timer): do what they asked for. */
static void after_js(void) {
    if (js_navigate) {
        char *url = js_navigate;
        js_navigate = NULL;
        if (!strncmp(url, "rdx-back:", 9)) back();
        else go(url, 1, NULL);
        free(url);
        return;
    }
    int redraw = 0;
    if (js_alert[0]) {
        strcpy(status_text, js_alert);
        js_alert[0] = 0;
        redraw = 1;
    }
    if (dom_dirty) {
        init_controls_mode(1);
        collect_links();
        relayout();
        dom_dirty = 0;
        load_images(NULL);
        relayout();
        redraw = 1;
    }
    if (js_scroll_to >= 0) {
        scroll = js_scroll_to;
        if (scroll > max_scroll()) scroll = max_scroll();
        js_scroll_to = -1;
        redraw = 1;
    }
    page_scroll_y = scroll;
    if (redraw) draw();
    js_maybe_gc();
}

static void back(void) {
    if (!history_count) return;
    char *url = history[--history_count];
    go(url, 0, NULL);
    free(url);
}

static void scroll_by(int dy) {
    scroll += dy;
    if (scroll > max_scroll()) scroll = max_scroll();
    if (scroll < 0) scroll = 0;
    draw();
}

/* Move the keyboard to the next (or previous) form field. */
static void focus_next(int backwards) {
    static Node *list[512];
    int count = 0, at = -1;
    for (Node *n = document; n && count < 512; n = next_in_tree(n, document)) {
        if (n->type == NODE_ELEMENT && (is(n->tag, "input") || is(n->tag, "textarea") || is(n->tag, "select") || is(n->tag, "button")) &&
            n->style.display != D_NONE && n->w > 0) {
            char type[16] = "";
            attr(n, "type", type, sizeof(type));
            if (!strcasecmp_word(type, "hidden")) continue;
            if (n == focused_node) at = count;
            list[count++] = n;
        }
    }
    if (!count) return;
    at = backwards ? (at <= 0 ? count - 1 : at - 1) : (at + 1) % count;
    focused_node = list[at];
    /* Bring it into view. */
    if (focused_node->y < scroll || focused_node->y + focused_node->h > scroll + view_height())
        scroll = focused_node->y - view_height() / 3;
    if (scroll > max_scroll()) scroll = max_scroll();
    if (scroll < 0) scroll = 0;
    draw();
}

static void type_into(Node *n, int k) {
    size_t len = n->value ? strlen(n->value) : 0;
    if (k == 8) {
        if (len) n->value[len - 1] = 0;
    } else if ((k >= 32 && k < 127) || (k == '\n' && is(n->tag, "textarea"))) {
        char *bigger = realloc(n->value, len + 2);
        if (!bigger) return;
        bigger[len] = (char)k;
        bigger[len + 1] = 0;
        n->value = bigger;
    } else if (k == CTRL('U')) {
        if (n->value) n->value[0] = 0;
    }
    js_fire(n, "input", 1, 0);
    draw();
}

/* A key for scripts: keydown at the focused element (or the page).
   Returns 1 if a script prevented the browser's own handling. */
static int key_event(int k) {
    if (!js_enabled || !global_obj) return 0;
    const char *name = NULL;
    int code = k;
    switch (k) {
    case '\n': name = "Enter"; code = 13; break;
    case 8: name = "Backspace"; break;
    case '\t': name = "Tab"; code = 9; break;
    case KEY_ESCAPE: name = "Escape"; code = 27; break;
    case KEY_UP: name = "ArrowUp"; code = 38; break;
    case KEY_DOWN: name = "ArrowDown"; code = 40; break;
    case KEY_LEFT: name = "ArrowLeft"; code = 37; break;
    case KEY_RIGHT: name = "ArrowRight"; code = 39; break;
    case KEY_PAGE_UP: name = "PageUp"; code = 33; break;
    case KEY_PAGE_DOWN: name = "PageDown"; code = 34; break;
    case KEY_HOME: name = "Home"; code = 36; break;
    case KEY_END: name = "End"; code = 35; break;
    case KEY_DELETE: name = "Delete"; code = 46; break;
    }
    char ch[2] = {(char)k, 0};
    if (!name) {
        if (k < 32 || k >= 127) return 0;
        name = ch;
        code = k >= 'a' && k <= 'z' ? k - 32 : k;
    }
    JsObj *e = new_event("keydown", 1, 1);
    prop_set_raw(e, intern_c("key"), v_str(str_c(name)), 0);
    prop_set_raw(e, intern_c("code"), v_str(str_c(name)), 0);
    prop_set_raw(e, intern_c("keyCode"), v_num(code), 0);
    prop_set_raw(e, intern_c("which"), v_num(code), 0);
    Node *target = focused_node ? focused_node : body_element;
    int prevented = js_dispatch(target, e);
    JsObj *up = new_event("keyup", 1, 1);
    prop_set_raw(up, intern_c("key"), v_str(str_c(name)), 0);
    prop_set_raw(up, intern_c("keyCode"), v_num(code), 0);
    js_dispatch(target, up);
    return prevented;
}

static void key(int k) {
    if (editing) {
        if (selected_all && ((k >= 32 && k < 127) || k == 8 || k == KEY_DELETE)) address[0] = 0;
        if (k == KEY_LEFT || k == KEY_RIGHT || k == KEY_END || k == KEY_HOME) {
            selected_all = 0;
            draw();
            return;
        }
        if (k != '\n') selected_all = 0;
        size_t n = strlen(address);
        if (k == '\n') {
            static char url[1100];
            if (!strstr(address, "://") && address[0] != '/' && address[0] != '.' && strchr(address, '.') && !strstr(address, ".htm") &&
                !strchr(address, ' ')) {
                strcpy(url, "https://");
                strncat_safe(url, address, 1000);
            } else if (strchr(address, ' ') || (!strchr(address, '.') && address[0] != '/' && !strstr(address, "://"))) {
                /* Words, not an address: search for them. */
                strcpy(url, "https://html.duckduckgo.com/html/?q=");
                url_encode(url, sizeof(url), address);
            } else {
                strcpy(url, address);
            }
            go(url, 1, NULL);
        } else if (k == KEY_ESCAPE) {
            editing = 0;
            draw();
        } else if (k == 8 || k == KEY_DELETE) {
            if (n) address[n - 1] = 0;
            draw();
        } else if (k == CTRL('U')) {
            address[0] = 0;
            draw();
        } else if (k >= 32 && k < 127 && n < sizeof(address) - 1) {
            address[n] = (char)k;
            address[n + 1] = 0;
            draw();
        }
        return;
    }
    if (key_event(k) || js_navigate) {
        after_js();
        return;
    }
    if (k == '\t') {
        focus_next(0);
        return;
    }
    Node *f = focused_node;
    if (f && is_text_control(f)) {
        if (k == '\n' && !is(f->tag, "textarea")) {
            if (form_of(f)) submit(form_of(f), NULL);
            return;
        }
        if (k == KEY_ESCAPE) {
            focused_node = NULL;
            draw();
            return;
        }
        if (k != KEY_UP && k != KEY_DOWN && k != KEY_PAGE_UP && k != KEY_PAGE_DOWN) {
            type_into(f, k);
            return;
        }
    } else if (f && (k == '\n' || k == ' ')) {
        if (activate(f) && !navigate_requested) {
            relayout();
            draw();
        }
        return;
    }
    int page = view_height() - 40;
    switch (k) {
    case KEY_DOWN: scroll_by(54); break;
    case KEY_UP: scroll_by(-54); break;
    case KEY_PAGE_DOWN: case ' ': scroll_by(page); break;
    case KEY_PAGE_UP: scroll_by(-page); break;
    case KEY_HOME: scroll_by(-scroll); break;
    case KEY_END: scroll_by(max_scroll()); break;
    case 8: back(); break;
    case CTRL('L'):
        start_editing();
        draw();
        break;
    case CTRL('R'): go(current_url, 0, NULL); break;
    }
}

static Node *control_for_label(Node *label) {
    char id[64], other[64];
    if (attr(label, "for", id, sizeof(id))) {
        for (Node *o = document; o; o = next_in_tree(o, document)) {
            if (o->type == NODE_ELEMENT && attr(o, "id", other, sizeof(other)) && !strcmp(id, other)) return o;
        }
        return NULL;
    }
    for (Node *o = label; o; o = next_in_tree(o, label)) {
        if (o->type == NODE_ELEMENT && (is(o->tag, "input") || is(o->tag, "select") || is(o->tag, "textarea"))) return o;
    }
    return NULL;
}

static void click(int x, int y) {
    if (y < BAR) {
        if (x >= 6 && x < 32) back();
        else if (x >= 36 && x < 62) go(current_url, 0, NULL);
        else if (x >= 70) {
            start_editing();
            draw();
        }
        return;
    }
    if (editing) {
        editing = 0;
        draw();
    }
    if (y >= H - STATUS) return;
    int vy = y - BAR;
    if (x >= PAGE_W) {
        int track = view_height();
        int thumb = page_height > track ? (int)((long long)track * track / page_height) : track;
        int thumb_top = max_scroll() ? (int)((long long)(track - thumb) * scroll / max_scroll()) : 0;
        scroll_by(vy < thumb_top ? -(track - 40) : track - 40);
        return;
    }
    int i = item_at(x, vy + scroll);
    Node *target = i >= 0 ? items[i].node : NULL;
    if (i < 0 || items[i].kind == IT_RECT) { /* not on text: the innermost box there */
        Node *box = node_at(x, vy + scroll);
        if (box) target = box;
    }
    static char href[2048];
    href[0] = 0;
    if (i >= 0 && items[i].link >= 0) resolve_url(base_url, links[items[i].link], href, sizeof(href));
    /* Scripts first: they may handle the click themselves. */
    if (js_enabled && global_obj) {
        Node *t = target ? target : body_element;
        js_fire(t, "mousedown", 1, 1);
        js_fire(t, "mouseup", 1, 1);
        JsObj *e = new_event("click", 1, 1);
        prop_set_raw(e, intern_c("clientX"), v_num(x), 0);
        prop_set_raw(e, intern_c("clientY"), v_num(vy), 0);
        prop_set_raw(e, intern_c("pageX"), v_num(x), 0);
        prop_set_raw(e, intern_c("pageY"), v_num(vy + scroll), 0);
        prop_set_raw(e, intern_c("button"), v_num(0), 0);
        prop_set_raw(e, intern_c("which"), v_num(1), 0);
        int prevented = js_dispatch(t, e);
        if (prevented || js_navigate) {
            after_js();
            return;
        }
        if (dom_dirty) after_js(); /* (the clicked node is still there: scripts hold it) */
    }
    /* Form controls and labels. */
    for (Node *n = target; n && n != document; n = n->parent) {
        if (n->type != NODE_ELEMENT) continue;
        if (is(n->tag, "input") || is(n->tag, "select") || is(n->tag, "textarea") || is(n->tag, "button")) {
            focused_node = n;
            if (!is_text_control(n) && activate(n)) {
                if (navigate_requested) return;
                relayout();
            }
            draw();
            return;
        }
        if (is(n->tag, "label")) {
            Node *control = control_for_label(n);
            if (control) {
                focused_node = control;
                if (!is_text_control(control)) activate(control);
                if (navigate_requested) return;
                relayout();
                draw();
                return;
            }
        }
    }
    if (href[0]) {
        char *url = href;
        if (!strncasecmp(url, "javascript:", 11)) { /* run it */
            if (js_enabled && global_obj) {
                js_run_script(url + 11, (int)strlen(url + 11), "link");
                run_microtasks();
                after_js();
            }
            return;
        }
        if (!strncasecmp(url, "mailto:", 7)) return;
        if (!strncasecmp(url, "rdx-proceed:", 12)) { /* the user chose to open an unverified site */
            allow_insecure(url + 12);
            go(url + 12, 0, NULL);
            return;
        }
        go(url, 1, NULL);
        return;
    }
    if (focused_node) {
        focused_node = NULL;
        draw();
    }
}

static int last_buttons;

static void handle(gui_message *m) {
    if (m->type == MSG_KEY) key(m->a);
    if (m->type == MSG_MOUSE) {
        int pressed = m->c & ~last_buttons;
        last_buttons = m->c;
        if (pressed & MOUSE_LEFT) click(m->a, m->b);
    }
}

int main(int argc, char **argv) {
    GUI_PROGRAM();
    if (getenv("BROWSER_NOJS")) js_enabled = 0; /* scripts off */
    if (argc > 2 && strcmp(argv[1], "--dump") == 0) return dump(argv[2]);
    if (gui_connect() < 0) {
        printf("browser: the desktop is not running (start it with: desktop)\n");
        printf("         or print a page as text with: browser --dump <address>\n");
        return 1;
    }
    win = gui_window_new(W, H, "Browser");
    if (!win) return 1;
    go(argc > 1 ? argv[1] : HOME, 0, NULL);
    gui_message m;
    for (;;) {
        int got = gui_next(&m, js_next_timer());
        if (got < 0) return 0;
        if (got > 0) {
            if (m.type == MSG_CLOSE) break;
            handle(&m);
        }
        js_run_timers();
        after_js();
    }
    gui_window_close(win);
    return 0;
}
